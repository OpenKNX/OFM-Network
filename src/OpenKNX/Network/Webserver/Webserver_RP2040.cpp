#ifdef ARDUINO_ARCH_RP2040
#ifdef OPENKNX_WEBSERVER

#include "OpenKNX.h"
#include "OpenKNX/Crypto/Base64.h"
#include "OpenKNX/Crypto/SHA1.h"
#include "OpenKNX/Network/Module.h"
#include "OpenKNX/Network/Webserver/Webserver.h"
#include <algorithm>
#include <lwip/tcp.h>
#include <lwip_wrap.h> // LWIPMutex: makes the ethernet IRQ defer
#include <string>
#include <vector>

// Der Slot-Default steht in Webserver.h, weil die Sendequeue ihre Cursortabelle
// danach dimensioniert. HTTP und WS teilen sich den Pool.
//
// MEMP_NUM_TCP_PCB ist die Zahl gleichzeitig aktiver TCP-PCBs im *ganzen* Gerät --
// inklusive MQTT, Webclient, OTA und den TIME_WAIT-Zuständen frisch geschlossener
// Verbindungen. arduino-pico setzt sie auf __LWIP_MEMMULT * 5, der Default ist also
// 5. Reicht sie nicht, scheitert der letzte Accept still in lwIP.
#if OPENKNX_WEBSERVER_MAX_CONN >= MEMP_NUM_TCP_PCB
#warning "OPENKNX_WEBSERVER_MAX_CONN >= MEMP_NUM_TCP_PCB: set -D__LWIP_MEMMULT=2 in the parent project, otherwise the extra connection slots have no effect"
#endif
// One RX buffer per slot, for HTTP headers and WS payloads alike. Oversized headers
// get a 431, an oversized WS payload drops the connection -- same as ESP32, and the
// client reconnects. Max 65535 (rxLen is uint16_t).
#ifndef OPENKNX_WEBSERVER_RX_CAP
#define OPENKNX_WEBSERVER_RX_CAP 1024
#endif

namespace OpenKNX
{
    namespace Network
    {

        // ── WS handshake helper ───────────────────────────────────────────────

        static const char WS_MAGIC[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

        static bool wsComputeAccept(const char* key, char* out, size_t outSize)
        {
            char combined[128];
            int n = snprintf(combined, sizeof(combined), "%s%s", key, WS_MAGIC);
            if (n <= 0 || n >= (int)sizeof(combined)) return false;

            uint8_t sha[20];
            OpenKNX::Crypto::SHA1::compute(combined, sha);
            return OpenKNX::Crypto::Base64::encode(sha, sizeof(sha), out, outSize) > 0;
        }

        // ── Connection state ──────────────────────────────────────────────────

        enum ConnState
        {
            CS_HTTP_RECV,
            CS_HTTP_SEND,
            CS_WS
        };
        enum WsRxState
        {
            WR_HDR,
            WR_EXTLEN2,
            WR_EXTLEN8,
            WR_MASK,
            WR_PAYLOAD
        };

        struct ConnSlot
        {
            tcp_pcb* pcb;
            Webserver* ws;
            int idx;
            ConnState state;
            uint8_t pollCount;
            uint32_t lastPeerMs; // last ACK or received byte
            uint16_t pendOff;    // bytes of g_pending[idx] already written

            char rxBuf[OPENKNX_WEBSERVER_RX_CAP];
            uint16_t rxLen;

            // Parsed request
            uint8_t method;
            char uri[128];
            bool isWs;
            bool uriTooLong;   // uri[] didn't fit the actual request line — reject with 414
            uint16_t wsKeyOff; // Sec-WebSocket-Key offset into rxBuf, 0 = absent

            // HTTP TX position only; the blocks belong to g_response[idx]
            uint8_t txSegIdx; // segment being sent
            int txSent;       // bereits gesendete Bytes innerhalb dieses Segments

            // WS RX state machine
            WsRxState wsRx;
            uint8_t wsOpcode;
            uint64_t wsPayLen;
            uint32_t wsPayRcvd;
            uint8_t wsMask[4];
            bool wsMasked;
            uint8_t wsHdrBuf[10]; // accumulate header bytes
            uint8_t wsHdrRcvd;

            bool hasPendingRequest = false;

            // POST body buffering
            uint32_t contentLength = 0; // aus Content-Length Header
            uint16_t headerEnd = 0;     // Offset nach \r\n\r\n in rxBuf
            uint8_t* bodyBuf = nullptr; // heap, for bodies beyond rxBuf
            uint32_t bodyReceived = 0;
            bool waitingForBody = false;

            // Streaming Download (LittleFS → TCP)
            WebStreamReadFn streamReader = nullptr;
            WebStreamCleanupFn streamCleanup = nullptr;
            void* streamCtx = nullptr;
            size_t streamTotal = 0;
            size_t streamSent = 0;
            bool streaming = false;
        };

        static constexpr int MAX_CONN = OPENKNX_WEBSERVER_MAX_CONN; // shared HTTP + WS slot pool
        static ConnSlot g_slots[MAX_CONN];

        // Must outlive the asynchronous send. Not in ConnSlot: onAccept() clears that with
        // memset, which would wreck the std::string members.
        static WebResponse g_response[MAX_CONN];

        // Sent before anything else: the HTTP header, or a stream chunk tcp_write() refused.
        static std::string g_pending[MAX_CONN];

        // Stream cleanups close files, so they run in loopPlatform() -- never in a
        // callback, which is the ethernet IRQ. A slot with one pending stays unused.
        struct StreamGc
        {
            WebStreamCleanupFn fn;
            void* ctx;
        };
        static StreamGc g_streamGc[MAX_CONN];

        // Per HTTP connection; uncapped, one page takes most of the 16 KB lwIP heap.
        static constexpr int HTTP_INFLIGHT = 2 * TCP_MSS;

        static constexpr size_t WS_FRAME_BUF = 1500;
        static constexpr size_t WS_MAX_PAYLOAD = WS_FRAME_BUF - 4; // 4 B header, 126..65535 form

        // ── Slot management ───────────────────────────────────────────────────

        static ConnSlot* freeSlot()
        {
            for (int i = 0; i < MAX_CONN; i++)
                if (g_slots[i].pcb == nullptr && !g_streamGc[i].fn) return &g_slots[i];
            return nullptr;
        }

        static void releaseSlot(ConnSlot* s)
        {
            if (!s) return;
            s->txSegIdx = 0;
            s->txSent = 0;
            s->pendOff = 0;
            if (s->idx >= 0 && s->idx < MAX_CONN)
            {
                g_response[s->idx].reset(); // body and layout shells
                g_pending[s->idx] = std::string();
                if (s->streaming && s->streamCleanup) g_streamGc[s->idx] = {s->streamCleanup, s->streamCtx};
            }
            if (s->bodyBuf)
            {
                free(s->bodyBuf); // PSRAM_MALLOC in onRecv
                s->bodyBuf = nullptr;
            }
            s->streaming = false;
            s->streamReader = nullptr;
            s->streamCleanup = nullptr;
            s->streamCtx = nullptr;
            s->pcb = nullptr;
        }

        // Before every close, tcp_err included: a lingering pcb must not reach the slot's next user.
        static void detach(tcp_pcb* pcb)
        {
            tcp_arg(pcb, nullptr);
            tcp_recv(pcb, nullptr);
            tcp_sent(pcb, nullptr);
            tcp_poll(pcb, nullptr, 0);
            tcp_err(pcb, nullptr);
        }

        // Frees the slot, telling the WS layer first. Leaves the pcb alone.
        static void endSlot(ConnSlot* s)
        {
            if (s->state == CS_WS) s->ws->notifySocketConnect(s->uri, s->idx, false);
            releaseSlot(s);
        }

        // ERR_ABRT = the pcb had to be aborted; a callback must return exactly that.
        static err_t closeSlot(ConnSlot* s, bool notify = true)
        {
            tcp_pcb* pcb = s->pcb;
            if (notify)
                endSlot(s);
            else
                releaseSlot(s);
            if (!pcb) return ERR_OK;
            detach(pcb);
            if (tcp_close(pcb) == ERR_OK) return ERR_OK;
            tcp_abort(pcb);
            return ERR_ABRT;
        }

        // ── TCP send helpers ──────────────────────────────────────────────────

        static bool txDone(const ConnSlot* s)
        {
            return s->pendOff >= g_pending[s->idx].size() &&
                   s->txSegIdx >= g_response[s->idx].segmentCount();
        }

        static int txRoom(tcp_pcb* pcb)
        {
            return HTTP_INFLIGHT - (int)(TCP_SND_BUF - tcp_sndbuf(pcb));
        }

        // false = bytes still waiting (window full or ERR_MEM), retried later.
        static bool flushPending(ConnSlot* s)
        {
            const std::string& p = g_pending[s->idx];
            while (s->pendOff < p.size())
            {
                int n = (int)(p.size() - s->pendOff);
                int room = txRoom(s->pcb);
                if (room <= 0) return false;
                if (n > room) n = room;
                if (tcp_write(s->pcb, p.data() + s->pendOff, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK) return false;
                s->pendOff += (uint16_t)n;
            }
            return true;
        }

        static void tcpSendChunk(ConnSlot* s)
        {
            if (flushPending(s))
            {
                const WebResponse& res = g_response[s->idx];
                while (s->txSegIdx < res.segmentCount())
                {
                    const ResponseSegment& seg = res.segments()[s->txSegIdx];
                    if (s->txSent >= seg.len)
                    {
                        s->txSegIdx++;
                        s->txSent = 0;
                        continue;
                    }

                    int toWrite = seg.len - s->txSent;
                    int room = txRoom(s->pcb);
                    if (room <= 0) break;
                    if (toWrite > room) toWrite = room;

                    // ERR_MEM: nothing counted, loopPlatform() retries -- after an ERR_MEM
                    // with nothing in flight, no ACK would ever call onSent().
                    if (tcp_write(s->pcb, seg.data + s->txSent, (u16_t)toWrite, TCP_WRITE_FLAG_COPY) != ERR_OK)
                        break;
                    s->txSent += toWrite;
                }
            }
            // Always flush: a response without segments (a 303) still has its header queued.
            tcp_output(s->pcb);
        }

        // Loop context only: the reader touches the filesystem. A few chunks per tick,
        // so a large download does not stretch the loop.
        static void tcpSendStream(ConnSlot* s)
        {
            uint8_t buf[512];
            for (int i = 0; i < 4 && flushPending(s) && s->streamSent < s->streamTotal; i++)
            {
                int room = txRoom(s->pcb);
                if (room <= 0) break;
                size_t toRead = sizeof(buf);
                if (toRead > (size_t)room) toRead = (size_t)room;
                size_t rem = s->streamTotal - s->streamSent;
                if (toRead > rem) toRead = rem;

                size_t got = s->streamReader(s->streamCtx, buf, toRead);
                if (got == 0)
                {
                    s->streamSent = s->streamTotal; // early EOF, the close ends it
                    break;
                }
                s->streamSent += got;
                if (tcp_write(s->pcb, buf, (u16_t)got, TCP_WRITE_FLAG_COPY) != ERR_OK)
                {
                    // Already consumed from the file -- dropping it would corrupt the download.
                    g_pending[s->idx].assign((const char*)buf, got);
                    s->pendOff = 0;
                    break;
                }
            }
            tcp_output(s->pcb);
        }

        // Refill and finish from the loop: streams only get refilled here, and a plain
        // response needs it after an ERR_MEM that left nothing in flight.
        static void pumpHttp(ConnSlot* s)
        {
            if (s->streaming)
                tcpSendStream(s);
            else
                tcpSendChunk(s);
            // tcp_close() keeps queued data and sends the FIN after it.
            if (txDone(s) && (!s->streaming || s->streamSent >= s->streamTotal))
                closeSlot(s);
        }

        // Header and payload in one tcp_write: with two, an ERR_MEM on the second would
        // leave a header without payload in the stream. Too large = Failed, full = WouldBlock.
        static WsSendResult wsTcpSend(tcp_pcb* pcb, const char* data, size_t len, bool binary)
        {
            // Stack buffer avoids heap fragmentation
            uint8_t frame[WS_FRAME_BUF];

            int hLen = 0;
            frame[hLen++] = binary ? 0x82 : 0x81; // FIN + binary/text opcode
            if (len < 126)
            {
                frame[hLen++] = (uint8_t)len;
            }
            else if (len < 65536)
            {
                frame[hLen++] = 126;
                frame[hLen++] = (uint8_t)(len >> 8);
                frame[hLen++] = (uint8_t)(len & 0xFF);
            }
            else
            {
                return WsSendResult::Failed; // Frame too large
            }

            size_t frameLen = (size_t)hLen + len;
            if (frameLen > sizeof(frame)) return WsSendResult::Failed;

            int avail = (int)tcp_sndbuf(pcb);
            if (avail < (int)frameLen) return WsSendResult::WouldBlock;

            if (len > 0) memcpy(frame + hLen, data, len);

            err_t err = tcp_write(pcb, frame, (u16_t)frameLen, TCP_WRITE_FLAG_COPY);
            if (err != ERR_OK) return WsSendResult::WouldBlock; // ERR_MEM — nichts geschrieben

            tcp_output(pcb);
            return WsSendResult::Ok;
        }

        // ── HTTP parsing ──────────────────────────────────────────────────────

        // false = URI passte nicht in uriSize; uri ist dann abgeschnitten und der
        // Aufrufer muss den Request ablehnen.
        static bool parseFirstLine(const char* line, uint8_t& method, char* uri, size_t uriSize)
        {
            method = WEB_GET;
            if (strncmp(line, "POST ", 5) == 0)
            {
                method = WEB_POST;
                line += 5;
            }
            else if (strncmp(line, "PUT ", 4) == 0)
            {
                method = WEB_PUT;
                line += 4;
            }
            else if (strncmp(line, "DELETE ", 7) == 0)
            {
                method = WEB_DELETE;
                line += 7;
            }
            else if (strncmp(line, "GET ", 4) == 0)
            {
                line += 4;
            }

            const char* sp = strchr(line, ' ');
            size_t uLen = sp ? (size_t)(sp - line) : strlen(line);
            bool fits = uLen < uriSize;
            if (!fits) uLen = uriSize - 1;
            memcpy(uri, line, uLen);
            uri[uLen] = '\0';
            return fits;
        }

        // Returns true if headers are complete; sets slot fields from parsed headers.
        static bool parseHeaders(ConnSlot* s)
        {
            char* end = strstr(s->rxBuf, "\r\n\r\n");
            if (!end) return false;

            char* p = s->rxBuf;
            char* lineEnd = strstr(p, "\r\n");
            if (!lineEnd) return false;
            *lineEnd = '\0';
            s->uriTooLong = !parseFirstLine(p, s->method, s->uri, sizeof(s->uri));
            p = lineEnd + 2;

            s->isWs = false;
            s->wsKeyOff = 0;
            s->contentLength = 0;
            bool hasUpgrade = false;

            while (p < end)
            {
                char* nl = strstr(p, "\r\n");
                if (!nl) break;
                *nl = '\0';

                char* colon = strchr(p, ':');
                if (colon)
                {
                    *colon = '\0';
                    const char* name = p;
                    const char* value = colon + 1;
                    while (*value == ' ')
                        value++;

                    if (strcasecmp(name, "Upgrade") == 0 && strcasecmp(value, "websocket") == 0)
                        hasUpgrade = true;
                    else if (strcasecmp(name, "Sec-WebSocket-Key") == 0 && *value)
                        s->wsKeyOff = (uint16_t)(value - s->rxBuf); // already NUL-terminated in rxBuf
                    else if (strcasecmp(name, "Content-Length") == 0)
                        s->contentLength = (uint32_t)atoi(value);
                }
                p = nl + 2;
            }

            s->headerEnd = (uint16_t)((end + 4) - s->rxBuf);
            s->isWs = hasUpgrade && s->wsKeyOff != 0 && s->method == WEB_GET;
            return true;
        }

        // ── HTTP dispatch ─────────────────────────────────────────────────────

        // Response-Header plus abschliessende Leerzeile. snprintf liefert die
        // ungekürzte Länge zurück, hLen muss deshalb explizit geklemmt werden.
        static void appendResponseHeaders(char* hdr, size_t hdrSize, int& hLen, const WebResponse& res)
        {
            for (auto& h : res.responseHeaders())
            {
                if (hLen < 0 || (size_t)hLen + 3 >= hdrSize) break;
                int n = snprintf(hdr + hLen, hdrSize - hLen, "%s: %s\r\n", h.first.c_str(), h.second.c_str());
                if (n < 0 || (size_t)hLen + n + 3 >= hdrSize)
                {
                    hdr[hLen] = '\0'; // abgeschnittenen Header verwerfen
                    break;
                }
                hLen += n;
            }
            if (hLen < 0) hLen = 0;
            if ((size_t)hLen + 3 > hdrSize) hLen = (int)hdrSize - 3;
            hdr[hLen++] = '\r';
            hdr[hLen++] = '\n';
            hdr[hLen] = '\0';
        }

        static void doHttpDispatch(ConnSlot* s)
        {
            if (s->isWs && !s->ws->hasSocket(s->uri))
            {
                const char resp[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                tcp_write(s->pcb, resp, sizeof(resp) - 1, TCP_WRITE_FLAG_COPY);
                tcp_output(s->pcb);

                WebRequest wsReq;
                wsReq.uri = s->uri;
                wsReq.method = WEB_GET;
                if (s->pcb)
                {
                    const ip4_addr_t* ip4 = ip_2_ip4(&s->pcb->remote_ip);
                    uint32_t remoteIpAddr = ((uint32_t)ip4_addr1(ip4) << 0) | ((uint32_t)ip4_addr2(ip4) << 8) |
                                            ((uint32_t)ip4_addr3(ip4) << 16) | ((uint32_t)ip4_addr4(ip4) << 24);
                    wsReq.setRemoteAddr(remoteIpAddr);
                    wsReq.setRemotePort(s->pcb->remote_port);
                }
                s->ws->logWebsocket(wsReq, 404);

                closeSlot(s);
                return;
            }

#ifdef OPENKNX_WIFI_AP // open setup AP: no websocket (a webserver build may have no network module)
            if (s->isWs && s->ws->hasSocket(s->uri) && openknxNetwork.wifiSetupPathAllowed(s->uri))
#else
            if (s->isWs && s->ws->hasSocket(s->uri))
#endif
            {
                // WebSocket upgrade
                char accept[64] = {};
                if (!wsComputeAccept(s->rxBuf + s->wsKeyOff, accept, sizeof(accept)))
                {
                    closeSlot(s);
                    return;
                }

                char resp[256];
                int rLen = snprintf(resp, sizeof(resp),
                                    "HTTP/1.1 101 Switching Protocols\r\n"
                                    "Upgrade: websocket\r\n"
                                    "Connection: Upgrade\r\n"
                                    "Sec-WebSocket-Accept: %s\r\n\r\n",
                                    accept);

                // Without the 101 the browser would wait for its timeout; closing lets it retry now.
                if (tcp_write(s->pcb, resp, (u16_t)rLen, TCP_WRITE_FLAG_COPY) != ERR_OK)
                {
                    closeSlot(s);
                    return;
                }
                tcp_output(s->pcb);

                s->state = CS_WS;
                s->wsRx = WR_HDR;
                s->wsHdrRcvd = 0;
                s->wsPayRcvd = 0;

                s->ws->notifySocketConnect(s->uri, s->idx, true);

                uint32_t remoteIpAddr = 0;
                uint16_t clientPort = 0;
                if (s->pcb)
                {
                    const ip4_addr_t* ip4 = ip_2_ip4(&s->pcb->remote_ip);
                    uint8_t a = ip4_addr1(ip4);
                    uint8_t b = ip4_addr2(ip4);
                    uint8_t c = ip4_addr3(ip4);
                    uint8_t d = ip4_addr4(ip4);
                    remoteIpAddr = ((uint32_t)a << 0) | ((uint32_t)b << 8) | ((uint32_t)c << 16) | ((uint32_t)d << 24);
                    clientPort = s->pcb->remote_port;
                }

                WebRequest wsReq;
                wsReq.uri = s->uri;
                wsReq.setRemoteAddr(remoteIpAddr);
                wsReq.setRemotePort(clientPort);
                s->ws->logWebsocket(wsReq);
                return;
            }

            // Plain HTTP
            WebRequest request;
            request.uri = s->uri;
            request.method = s->method;

            if (s->pcb)
            {
                const ip4_addr_t* ip4 = ip_2_ip4(&s->pcb->remote_ip);
                uint8_t a = ip4_addr1(ip4);
                uint8_t b = ip4_addr2(ip4);
                uint8_t c = ip4_addr3(ip4);
                uint8_t d = ip4_addr4(ip4);
                uint32_t remoteIpAddr = ((uint32_t)a << 0) | ((uint32_t)b << 8) | ((uint32_t)c << 16) | ((uint32_t)d << 24);
                uint16_t remotePort = s->pcb->remote_port;
                request.setRemoteAddr(remoteIpAddr);
                request.setRemotePort(remotePort);
            }

            // POST body, from rxBuf or the heap bodyBuf
            if (s->contentLength > 0)
            {
                if (s->bodyBuf)
                    request.setBody(s->bodyBuf, s->bodyReceived);
                else
                    request.setBody((const uint8_t*)s->rxBuf + s->headerEnd, s->contentLength);
            }

            // Built in the slot's storage: sending continues asynchronously, a local
            // instance would be long gone by then.
            WebResponse& response = g_response[s->idx];
            response.reset();
            s->ws->handleRequest(request, response);

            s->ws->logRequest(request, response);

            int statusCode = response.statusCode();
            const char* statusText = "OK";
            if (statusCode == 404) statusText = "Not Found";
            else if (statusCode == 301)
                statusText = "Moved Permanently";
            else if (statusCode == 302)
                statusText = "Found";
            else if (statusCode == 303)
                statusText = "See Other";
            else if (statusCode == 400)
                statusText = "Bad Request";
            else if (statusCode == 401)
                statusText = "Unauthorized";
            else if (statusCode == 403)
                statusText = "Forbidden";
            else if (statusCode == 409)
                statusText = "Conflict";
            else if (statusCode == 413)
                statusText = "Content Too Large";
            else if (statusCode == 500)
                statusText = "Internal Server Error";
            else if (statusCode == 503)
                statusText = "Service Unavailable";

            if (response.isStreaming())
            {
                // Header with Content-Length first, then the chunks from loopPlatform()
                char hdr[512];
                char statusStr[32];
                snprintf(statusStr, sizeof(statusStr), "%d %s", statusCode, statusText);
                int hLen = snprintf(hdr, sizeof(hdr),
                                    "HTTP/1.1 %s\r\n"
                                    "Content-Type: %s\r\n"
                                    "Content-Length: %zu\r\n"
                                    "Connection: close\r\n",
                                    statusStr,
                                    response.contentType(),
                                    response.streamTotal());
                appendResponseHeaders(hdr, sizeof(hdr), hLen, response);

                g_pending[s->idx].assign(hdr, (size_t)hLen);
                s->pendOff = 0;
                s->streamReader = response.streamReadFn();
                s->streamCleanup = response.streamCleanupFn();
                s->streamCtx = response.streamCtx();
                s->streamTotal = response.streamTotal();
                s->streamSent = 0;
                s->streaming = true;
                s->txSegIdx = 0;
                s->txSent = 0;
                s->state = CS_HTTP_SEND;

                tcpSendStream(s);
                return;
            }

            // handleRequest() has built the segments already; only the send position is reset here.
            s->state = CS_HTTP_SEND;
            s->txSegIdx = 0;
            s->txSent = 0;

            const int contentLen = response.totalLength();

            char hdr[512];
            char statusStr[32];
            snprintf(statusStr, sizeof(statusStr), "%d %s", statusCode, statusText);
            int hLen = snprintf(hdr, sizeof(hdr),
                                "HTTP/1.1 %s\r\n"
                                "Content-Type: %s\r\n"
                                "Content-Length: %d\r\n"
                                "Connection: close\r\n",
                                statusStr,
                                response.contentType(),
                                contentLen);

            appendResponseHeaders(hdr, sizeof(hdr), hLen, response);

            // Through g_pending like the body: an ERR_MEM here used to lose the header
            // and send the body bare.
            g_pending[s->idx].assign(hdr, (size_t)hLen);
            s->pendOff = 0;

            // As much as fits now, the rest from onSent() and loopPlatform()
            tcpSendChunk(s);
        }

        // ── WebSocket frame processing ────────────────────────────────────────

        // Close frame from the browser: answer it, then end the connection.
        static err_t wsClose(ConnSlot* s)
        {
            uint8_t closeReply[2] = {0x88, 0x00};
            tcp_write(s->pcb, closeReply, 2, TCP_WRITE_FLAG_COPY);
            tcp_output(s->pcb);
            return closeSlot(s);
        }

        // Returns what the recv callback has to return; the slot is gone once s->pcb is null.
        static err_t wsProcessData(ConnSlot* s, const uint8_t* data, u16_t len)
        {
            u16_t pos = 0;

            while (pos < len)
            {
                uint8_t b = data[pos++];

                switch (s->wsRx)
                {
                    case WR_HDR:
                        s->wsHdrBuf[s->wsHdrRcvd++] = b;
                        if (s->wsHdrRcvd < 2) break;

                        s->wsOpcode = s->wsHdrBuf[0] & 0x0F;
                        s->wsMasked = (s->wsHdrBuf[1] & 0x80) != 0;
                        s->wsPayLen = s->wsHdrBuf[1] & 0x7F;
                        s->wsHdrRcvd = 0;

                        if (s->wsPayLen == 126)
                        {
                            s->wsRx = WR_EXTLEN2;
                            s->wsPayLen = 0;
                        }
                        else if (s->wsPayLen == 127)
                        {
                            s->wsRx = WR_EXTLEN8;
                            s->wsPayLen = 0;
                        }
                        else
                        {
                            s->wsRx = s->wsMasked ? WR_MASK : WR_PAYLOAD;
                        }
                        break;

                    case WR_EXTLEN2:
                        s->wsHdrBuf[s->wsHdrRcvd++] = b;
                        if (s->wsHdrRcvd < 2) break;
                        s->wsPayLen = ((uint16_t)s->wsHdrBuf[0] << 8) | s->wsHdrBuf[1];
                        s->wsHdrRcvd = 0;
                        s->wsRx = s->wsMasked ? WR_MASK : WR_PAYLOAD;
                        break;

                    case WR_EXTLEN8:
                        s->wsHdrBuf[s->wsHdrRcvd++] = b;
                        if (s->wsHdrRcvd < 8) break;
                        s->wsPayLen = 0;
                        for (int i = 0; i < 8; ++i)
                            s->wsPayLen = (s->wsPayLen << 8) | s->wsHdrBuf[i];
                        s->wsHdrRcvd = 0;
                        s->wsRx = s->wsMasked ? WR_MASK : WR_PAYLOAD;
                        break;

                    case WR_MASK:
                        s->wsMask[s->wsHdrRcvd++] = b;
                        if (s->wsHdrRcvd < 4) break;
                        s->wsHdrRcvd = 0;
                        s->wsPayRcvd = 0;
                        // Empty frames end here: WR_PAYLOAD consumes a byte and would run into the next frame.
                        if (s->wsPayLen == 0)
                        {
                            if (s->wsOpcode == 0x08) // Close (no payload)
                                return wsClose(s);
                            if (s->wsOpcode == 0x09) // Ping (no payload) → send empty pong
                            {
                                uint8_t pong[2] = {0x8A, 0x00};
                                tcp_write(s->pcb, pong, 2, TCP_WRITE_FLAG_COPY);
                                tcp_output(s->pcb);
                            }
                            else if (s->wsOpcode == 0x01 || s->wsOpcode == 0x02) // empty text/binary
                            {
                                // Delivered anyway: a bare Enter in the console is an empty line.
                                bool isBinary = (s->wsOpcode == 0x02);
                                s->ws->notifySocketMessage(s->uri, s->idx, (uint8_t*)s->rxBuf, 0, isBinary);
                            }
                            // Pong (0x0A): nothing to deliver.
                            s->wsRx = WR_HDR;
                            s->wsHdrRcvd = 0;
                            break;
                        }
                        s->wsRx = WR_PAYLOAD;
                        break;

                    case WR_PAYLOAD:
                    {
                        // Oversized frame: drop the connection like ESP32 does, never
                        // truncate. A truncated frame used to be delivered, turning a long
                        // "cmd:" into half a console command that then ran. wsPayLen is
                        // 64-bit (RFC 6455's 127 form) so this comparison sees the real
                        // wire value; a narrower field would let 0x1_0000_0200 pass as 512
                        // and desynchronise the parser.
                        if (s->wsPayRcvd == 0 && s->wsPayLen > sizeof(s->rxBuf))
                        {
                            logWarning("Webserver", "WS frame %u B > %u B buffer, disconnected",
                                       (unsigned)s->wsPayLen, (unsigned)sizeof(s->rxBuf));
                            return closeSlot(s); // slot is released, do not touch s again
                        }

                        if (s->wsPayRcvd < sizeof(s->rxBuf))
                            s->rxBuf[s->wsPayRcvd] = s->wsMasked
                                                          ? (b ^ s->wsMask[s->wsPayRcvd % 4])
                                                          : b;
                        s->wsPayRcvd++;

                        if (s->wsPayRcvd < (uint32_t)s->wsPayLen) break;

                        // Frame complete
                        if (s->wsOpcode == 0x08) // Close
                            return wsClose(s);
                        else if (s->wsOpcode == 0x09) // Ping → Pong
                        {
                            uint8_t pong[2] = {0x8A, 0x00};
                            tcp_write(s->pcb, pong, 2, TCP_WRITE_FLAG_COPY);
                            tcp_output(s->pcb);
                        }
                        else if (s->wsOpcode == 0x01 || s->wsOpcode == 0x02) // text/binary
                        {
                            bool isBinary = (s->wsOpcode == 0x02);
                            int payLen = (int)(s->wsPayRcvd < sizeof(s->rxBuf)
                                                   ? s->wsPayRcvd
                                                   : sizeof(s->rxBuf));
                            s->ws->notifySocketMessage(s->uri, s->idx, (uint8_t*)s->rxBuf, payLen, isBinary);
                        }

                        s->wsRx = WR_HDR;
                        s->wsHdrRcvd = 0;
                        s->wsPayRcvd = 0;
                        break;
                    }
                }
            }
            return ERR_OK;
        }

        // ── lwIP callbacks ────────────────────────────────────────────────────
        // With PIN_ETH_INT they run in the W5500 interrupt. They keep to bookkeeping,
        // parsing and RAM-only sending; anything touching files runs in loopPlatform().

        // A pcb whose slot has moved on. Normally unreachable -- every close detaches --
        // but should one arrive, cut it loose instead of acting on someone else's slot.
        static void dropStale(tcp_pcb* tpcb)
        {
            if (tpcb) detach(tpcb);
        }

        static void onErr(void* arg, err_t /*err*/)
        {
            ConnSlot* s = (ConnSlot*)arg;
            if (!s) return;
            s->pcb = nullptr; // already freed by lwIP -- must not be touched
            endSlot(s);
        }

        static err_t onSent(void* arg, tcp_pcb* tpcb, u16_t /*len*/)
        {
            ConnSlot* s = (ConnSlot*)arg;
            if (!s || s->pcb != tpcb)
            {
                dropStale(tpcb);
                return ERR_OK;
            }

            // Progress is a sign of life, or onPoll() would kill any transfer over ~22 s.
            s->pollCount = 0;
            s->lastPeerMs = millis();

            // Streams are refilled from loopPlatform() only: their reader touches files.
            if (s->state != CS_HTTP_SEND || s->streaming) return ERR_OK;

            tcpSendChunk(s);
            return txDone(s) ? closeSlot(s) : ERR_OK;
        }

        // Error responses for requests that never reach the router.
        static err_t rejectRequest(ConnSlot* s, const char* resp, size_t len)
        {
            tcp_write(s->pcb, resp, (u16_t)len, TCP_WRITE_FLAG_COPY);
            tcp_output(s->pcb);
            return closeSlot(s);
        }

        static err_t onRecv(void* arg, tcp_pcb* tpcb, pbuf* p, err_t err)
        {
            ConnSlot* s = (ConnSlot*)arg;
            if (!s || s->pcb != tpcb)
            {
                // Returning ERR_OK means the pbuf is ours -- free it, or it leaks.
                dropStale(tpcb);
                if (p) pbuf_free(p);
                return ERR_OK;
            }

            if (err != ERR_OK || p == nullptr)
            {
                if (p) pbuf_free(p);
                return closeSlot(s);
            }

            tcp_recved(tpcb, p->tot_len);
            s->pollCount = 0;
            s->lastPeerMs = millis();

            err_t ret = ERR_OK;
            // One request per connection (Connection: close). Anything after it is
            // ignored -- parsing it again would allocate a second bodyBuf over the first.
            if (s->state == CS_HTTP_RECV && !s->hasPendingRequest)
            {
                if (s->waitingForBody)
                {
                    // Header already parsed -- collect the remaining body bytes
                    for (pbuf* q = p; q != nullptr; q = q->next)
                    {
                        uint32_t space = s->contentLength - s->bodyReceived;
                        uint32_t toCopy = (q->len < space) ? (uint32_t)q->len : space;
                        memcpy(s->bodyBuf + s->bodyReceived, q->payload, toCopy);
                        s->bodyReceived += toCopy;
                    }
                    if (s->bodyReceived >= s->contentLength)
                    {
                        s->waitingForBody = false;
                        s->hasPendingRequest = true;
                    }
                }
                else
                {
                    // Accumulate into rxBuf until the header is complete
                    for (pbuf* q = p; q != nullptr; q = q->next)
                    {
                        size_t space = sizeof(s->rxBuf) - s->rxLen - 1;
                        size_t copy = q->len < space ? q->len : space;
                        memcpy(s->rxBuf + s->rxLen, q->payload, copy);
                        s->rxLen += (uint16_t)copy;
                        s->rxBuf[s->rxLen] = '\0';
                    }

                    if (parseHeaders(s))
                    {
                        if (s->uriTooLong)
                        {
                            // Otherwise the truncated URI would be routed silently. Body kept so a
                            // human sees the reason; 20 ASCII bytes, keep it in step with Content-Length.
                            static const char resp414[] =
                                "HTTP/1.1 414 URI Too Long\r\nContent-Type: text/plain\r\n"
                                "Content-Length: 20\r\nConnection: close\r\n\r\n"
                                "Anfragezeile zu lang";
                            ret = rejectRequest(s, resp414, sizeof(resp414) - 1);
                        }
                        else if (s->contentLength == 0)
                        {
                            s->hasPendingRequest = true;
                        }
                        else
                        {
                            uint32_t bodyInBuf = (uint32_t)(s->rxLen - s->headerEnd);
                            if (bodyInBuf >= s->contentLength)
                            {
                                s->hasPendingRequest = true; // body complete in rxBuf
                            }
                            else if (s->contentLength > OPENKNX_WEBSERVER_MAX_BODY)
                            {
                                static const char resp413[] =
                                    "HTTP/1.1 413 Content Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                                ret = rejectRequest(s, resp413, sizeof(resp413) - 1);
                            }
                            else
                            {
                                // Body buffer, seeded with what already arrived
                                s->bodyBuf = (uint8_t*)PSRAM_MALLOC(s->contentLength);
                                if (s->bodyBuf == nullptr)
                                {
                                    ret = closeSlot(s);
                                }
                                else
                                {
                                    memcpy(s->bodyBuf, s->rxBuf + s->headerEnd, bodyInBuf);
                                    s->bodyReceived = bodyInBuf;
                                    s->waitingForBody = true;
                                }
                            }
                        }
                    }
                    else if (s->rxLen >= sizeof(s->rxBuf) - 1)
                    {
                        // rxBuf full without a complete header -- it never will be
                        static const char resp431[] =
                            "HTTP/1.1 431 Request Header Fields Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                        ret = rejectRequest(s, resp431, sizeof(resp431) - 1);
                    }
                }
            }
            else if (s->state == CS_WS)
            {
                for (pbuf* q = p; q != nullptr && s->pcb; q = q->next)
                    ret = wsProcessData(s, (const uint8_t*)q->payload, q->len);
            }

            pbuf_free(p);
            return ret;
        }

        static err_t onPoll(void* arg, tcp_pcb* tpcb)
        {
            ConnSlot* s = (ConnSlot*)arg;
            if (!s || s->pcb != tpcb)
            {
                dropStale(tpcb);
                return ERR_OK;
            }

            s->pollCount++;

            if (s->state == CS_WS)
            {
                // WS ping every ~60 s; without an answer lwIP's retransmit timeout ends
                // the connection through onErr.
                if (s->pollCount >= 30)
                {
                    s->pollCount = 0;
                    uint8_t ping[2] = {0x89, 0x00};
                    if (tcp_write(tpcb, ping, 2, TCP_WRITE_FLAG_COPY) == ERR_OK)
                        tcp_output(tpcb);
                }
                return ERR_OK;
            }

            // Abort HTTP connections without progress (no request, no ACK).
            if (s->pollCount > 10) // ~20 s (poll interval = 4 × 500 ms)
            {
                endSlot(s);
                detach(tpcb);
                tcp_abort(tpcb);
                return ERR_ABRT;
            }
            return ERR_OK;
        }

        static err_t onAccept(void* arg, tcp_pcb* newpcb, err_t err)
        {
            if (err != ERR_OK || newpcb == nullptr) return ERR_VAL;

            ConnSlot* s = freeSlot();
            if (!s)
            {
                tcp_abort(newpcb);
                return ERR_ABRT;
            }

            int slotIdx = (int)(s - g_slots); // save before memset zeros it
            memset(s, 0, sizeof(*s));
            s->idx = slotIdx;
            s->pcb = newpcb;
            s->ws = (Webserver*)arg;
            s->state = CS_HTTP_RECV;
            s->pollCount = 0;
            s->lastPeerMs = millis();

            tcp_arg(newpcb, s);
            tcp_recv(newpcb, onRecv);
            tcp_sent(newpcb, onSent);
            tcp_err(newpcb, onErr);
            tcp_poll(newpcb, onPoll, 4); // ~2 s interval

            return ERR_OK;
        }

        // ── setupRp2040 — lwIP listen socket ─────────────────────────────────

        void Webserver::setupRp2040()
        {
            tcp_pcb* pcb = tcp_new();
            if (!pcb)
            {
                logError("Webserver", "tcp_new failed");
                return;
            }

            // start() retries every few seconds -- a pcb left behind per attempt would
            // drain the pool of 5 that MQTT, OTA and the webclient share.
            if (tcp_bind(pcb, IP_ADDR_ANY, 80) != ERR_OK)
            {
                logError("Webserver", "port 80 already in use");
                tcp_close(pcb);
                return;
            }
            _listenPcb = tcp_listen(pcb); // frees pcb on success only
            if (!_listenPcb)
            {
                logError("Webserver", "tcp_listen failed");
                tcp_close(pcb);
                return;
            }

            for (int i = 0; i < MAX_CONN; ++i)
            {
                g_slots[i] = {};
                g_slots[i].idx = i;
            }

            tcp_arg((tcp_pcb*)_listenPcb, this);
            tcp_accept((tcp_pcb*)_listenPcb, onAccept);

            logInfoP("Started on port 80");
        }

        void Webserver::loopPlatform()
        {
            for (int ci = 0; ci < MAX_CONN; ci++)
            {
                // Unlocked peek: every lock release runs the async context's workers.
                const ConnSlot& peek = g_slots[ci];
                if (!g_streamGc[ci].fn && !(peek.pcb && (peek.hasPendingRequest || peek.state == CS_HTTP_SEND)))
                    continue;

                StreamGc gc = {};
                {
                    // Keeps the IRQ from releasing the slot mid-dispatch; see WebserverPlatform.md.
                    LWIPMutex lock;
                    ConnSlot& s = g_slots[ci];
                    if (s.pcb && s.hasPendingRequest)
                    {
                        s.hasPendingRequest = false;
                        doHttpDispatch(&s);
                    }
                    else if (s.pcb && s.state == CS_HTTP_SEND)
                    {
                        pumpHttp(&s);
                    }
                    gc = g_streamGc[ci];
                    g_streamGc[ci] = {};
                }
                if (gc.fn) gc.fn(gc.ctx); // closes files -- no need for the lwIP lock
            }
        }

        bool Webserver::isRunning()
        {
            return _listenPcb != nullptr;
        }

        size_t Webserver::maxWebsocketPayload() const
        {
            return WS_MAX_PAYLOAD;
        }

        // Bypasses the queue -- only the drain calls this, and it handles Failed.
        WsSendResult Webserver::wsSendNow(const char*, int fd, const char* data, size_t len, bool binary)
        {
            if (!_listenPcb) return WsSendResult::Failed;
            if (fd < 0 || fd >= MAX_CONN || !g_slots[fd].pcb || g_slots[fd].state != CS_WS)
                return WsSendResult::Failed;
            return wsTcpSend(g_slots[fd].pcb, data, len, binary);
        }

        uint32_t Webserver::wsPeerIdleMs(int fd) const
        {
            if (fd < 0 || fd >= MAX_CONN || !g_slots[fd].pcb) return UINT32_MAX;
            return millis() - g_slots[fd].lastPeerMs;
        }

        // wnd 0 / persist > 0: the browser closed its window, i.e. stopped reading.
        // nrtx > 0 with unacked data: we retransmit and nothing comes back.
        void Webserver::wsLinkInfo(int fd, char* buf, size_t len) const
        {
            buf[0] = 0;
            if (fd < 0 || fd >= MAX_CONN || !g_slots[fd].pcb) return;
            const tcp_pcb* p = g_slots[fd].pcb;
            snprintf(buf, len, "; wnd %u, sndbuf %u, qlen %u, unacked %d, unsent %d, nrtx %u, persist %u",
                     (unsigned)p->snd_wnd, (unsigned)p->snd_buf, (unsigned)p->snd_queuelen,
                     p->unacked != nullptr, p->unsent != nullptr, (unsigned)p->nrtx, (unsigned)p->persist_backoff);
        }

        void Webserver::dropClient(const char* uri, int fd)
        {
            if (fd < 0 || fd >= MAX_CONN) return;
            ConnSlot* s = &g_slots[fd];

            // Deregister first and outside the lock, even for an already free slot: the
            // module's onConnect(false) must not run with the ethernet IRQ deferred, and a
            // cursor left in the queue would warn on every tick.
            notifySocketConnect(uri, fd, false);

            // The teardown must run as a whole: each tcp_* wrapper releases the deferred
            // IRQ on the way out, which could free the pcb the next line works on.
            LWIPMutex lock;

            // Only if the slot still holds a WS connection -- onAccept may have reused it.
            if (s->pcb && s->state == CS_WS) closeSlot(s, false);
        }

    } // namespace Network
} // namespace OpenKNX

#endif // OPENKNX_WEBSERVER
#endif // ARDUINO_ARCH_RP2040
