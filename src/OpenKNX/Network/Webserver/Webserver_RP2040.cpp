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

            char rxBuf[OPENKNX_WEBSERVER_RX_CAP];
            uint16_t rxLen;

            // Parsed request
            uint8_t method;
            char uri[128];
            bool isWs;
            bool uriTooLong;   // uri[] didn't fit the actual request line — reject with 414
            uint16_t wsKeyOff; // Sec-WebSocket-Key offset into rxBuf, 0 = absent

            // HTTP TX — nur die Position; die zu sendenden Blöcke liegen in
            // g_response[idx].segments(), gehören also der WebResponse.
            uint8_t txSegIdx; // aktuell sendendes Segment
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
            uint8_t* bodyBuf = nullptr; // heap-alloc für Overflow
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

        // Muss den asynchronen Versand über mehrere onSent()-Ticks überleben. Nicht in
        // ConnSlot: onAccept nullt den per memset und zerstörte so die std::string-Member.
        static WebResponse g_response[MAX_CONN];

        static constexpr size_t WS_FRAME_BUF = 1500;
        static constexpr size_t WS_MAX_PAYLOAD = WS_FRAME_BUF - 4; // 4 B header, 126..65535 form

        // ── Slot management ───────────────────────────────────────────────────

        static ConnSlot* findSlot(tcp_pcb* pcb)
        {
            for (auto& s : g_slots)
                if (s.pcb == pcb) return &s;
            return nullptr;
        }

        static ConnSlot* freeSlot()
        {
            for (auto& s : g_slots)
                if (s.pcb == nullptr) return &s;
            return nullptr;
        }

        static void releaseSlot(ConnSlot* s)
        {
            if (!s) return;
            s->txSegIdx = 0;
            s->txSent = 0;
            // Gibt Body und Layout-Hüllen der Antwort frei
            if (s->idx >= 0 && s->idx < MAX_CONN) g_response[s->idx].reset();
            if (s->bodyBuf)
            {
                free(s->bodyBuf); // PSRAM_MALLOC in onRecv
                s->bodyBuf = nullptr;
            }
            if (s->streaming && s->streamCleanup)
            {
                s->streamCleanup(s->streamCtx);
                s->streaming = false;
            }
            s->streamReader = nullptr;
            s->streamCleanup = nullptr;
            s->streamCtx = nullptr;
            s->pcb = nullptr;
        }

        // ── TCP send helpers ──────────────────────────────────────────────────

        static bool txDone(const ConnSlot* s)
        {
            return s->txSegIdx >= g_response[s->idx].segmentCount();
        }

        static void tcpSendChunk(ConnSlot* s)
        {
            const WebResponse& res = g_response[s->idx];
            while (s->txSegIdx < res.segmentCount())
            {
                const ResponseSegment& seg = res.segments()[s->txSegIdx];
                if (s->txSent >= seg.len)
                {
                    // Segment fertig → nahtlos am nächsten weiter, solange Platz ist
                    s->txSegIdx++;
                    s->txSent = 0;
                    continue;
                }

                int canWrite = (int)tcp_sndbuf(s->pcb);
                if (canWrite <= 0) break;
                int remaining = seg.len - s->txSent;
                int toWrite = remaining < canWrite ? remaining : canWrite;

                if (tcp_write(s->pcb, seg.data + s->txSent, (u16_t)toWrite, TCP_WRITE_FLAG_COPY) != ERR_OK)
                    break; // ERR_MEM — nicht weiterzählen, Retry kommt aus onSent
                s->txSent += toWrite;
            }
            // Immer flushen: auch eine Antwort ganz ohne Segmente (z.B. 303-Redirect) muss
            // ihren bereits geschriebenen HTTP-Header sofort loswerden.
            tcp_output(s->pcb);
        }

        // Streaming-Download: liest einen Chunk aus LittleFS und schreibt ihn via TCP.
        // 512-Byte Stack-Buffer — kein Heap, kein Fragmentierungsrisiko.
        //
        // Nur EIN Chunk pro Aufruf: der ganze tcp_sndbuf() (bis zu TCP_SND_BUF, hier
        // 8*TCP_MSS ~ 11.6 KB) leerzulesen hätte diesen einen loop()-Tick um die Zeit
        // für bis zu ~23 Flash-/SPI-Reads verlängert -- sichtbar als "loop took longer
        // than usual", proportional zur Dateigröße. onSent() ruft für den Rest erneut
        // auf, sobald der Client die nächste Portion bestätigt (mehrfach pro Sekunde,
        // nicht am ~2 s Poll-Timer hängend), Durchsatz bleibt dadurch unverändert.
        static void tcpSendStreamChunk(ConnSlot* s)
        {
            // Flush on the early exits too: doHttpDispatch() only wrote the header. For
            // an empty file nothing would leave, and onSent() -- the only path that closes
            // the connection and runs streamCleanup -- depends on it.
            if (s->streamSent >= s->streamTotal)
            {
                tcp_output(s->pcb);
                return;
            }

            int avail = (int)tcp_sndbuf(s->pcb);
            if (avail <= 0)
            {
                tcp_output(s->pcb); // onSent fires once there is room
                return;
            }

            uint8_t buf[512];
            size_t toRead = sizeof(buf);
            if (toRead > (size_t)avail) toRead = (size_t)avail;
            size_t rem = s->streamTotal - s->streamSent;
            if (toRead > rem) toRead = rem;

            size_t got = s->streamReader(s->streamCtx, buf, toRead);
            if (got == 0)
            {
                s->streamSent = s->streamTotal; // EOF
                tcp_output(s->pcb);
                return;
            }
            if (tcp_write(s->pcb, buf, (u16_t)got, TCP_WRITE_FLAG_COPY) != ERR_OK) return;
            s->streamSent += got;
            tcp_output(s->pcb);
        }

        // Sendet ein WS-Textframe über lwIP. Header + Payload gehen in einem einzigen
        // tcp_write raus: zwei getrennte Writes riskieren, dass der zweite mit ERR_MEM
        // scheitert während der Header schon in der Queue liegt — das zerlegt den Stream.
        // Zu grosse Payloads sind Failed (nie sendbar), voller Sendepuffer WouldBlock.
        static WsSendResult wsTcpSend(tcp_pcb* pcb, const char* data, size_t len)
        {
            // Stack buffer avoids heap fragmentation
            uint8_t frame[WS_FRAME_BUF];

            int hLen = 0;
            frame[hLen++] = 0x81; // FIN + text opcode
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

                tcp_close(s->pcb);
                releaseSlot(s);
                return;
            }

            if (s->isWs && s->ws->hasSocket(s->uri))
            {
                // WebSocket upgrade
                char accept[64] = {};
                if (!wsComputeAccept(s->rxBuf + s->wsKeyOff, accept, sizeof(accept)))
                {
                    tcp_close(s->pcb);
                    releaseSlot(s);
                    return;
                }

                char resp[256];
                int rLen = snprintf(resp, sizeof(resp),
                                    "HTTP/1.1 101 Switching Protocols\r\n"
                                    "Upgrade: websocket\r\n"
                                    "Connection: Upgrade\r\n"
                                    "Sec-WebSocket-Accept: %s\r\n\r\n",
                                    accept);

                tcp_write(s->pcb, resp, (u16_t)rLen, TCP_WRITE_FLAG_COPY);
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

            // POST-Body übergeben (aus rxBuf oder heap-alloc bodyBuf)
            if (s->contentLength > 0)
            {
                if (s->bodyBuf)
                    request.setBody(s->bodyBuf, s->bodyReceived);
                else
                    request.setBody((const uint8_t*)s->rxBuf + s->headerEnd, s->contentLength);
            }

            // Antwort im Slot-Speicher aufbauen: der Versand läuft asynchron über
            // onSent() weiter, eine lokale Instanz wäre dann längst zerstört.
            WebResponse& response = g_response[s->idx];
            response.reset();
            s->ws->handleRequest(request, response);

            s->ws->logRequest(request, response);

            int statusCode = response.statusCode();
            const char* statusText = "OK";
            if (statusCode == 404) statusText = "Not Found";
            else if (statusCode == 301)
                statusText = "Moved Permanently";
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
                // Streaming-Download: HTTP-Header mit Content-Length senden, dann chunks via onSent
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

                tcp_write(s->pcb, hdr, (u16_t)hLen, TCP_WRITE_FLAG_COPY);

                s->streamReader = response.streamReadFn();
                s->streamCleanup = response.streamCleanupFn();
                s->streamCtx = response.streamCtx();
                s->streamTotal = response.streamTotal();
                s->streamSent = 0;
                s->streaming = true;
                s->txSegIdx = 0;
                s->txSent = 0;
                s->state = CS_HTTP_SEND;

                tcpSendStreamChunk(s);
                return;
            }

            // Die Segmente stehen bereits in der Response (handleRequest hat sie gebaut) —
            // hier wird nur noch die Sendeposition zurückgesetzt und rausgeschrieben.
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

            tcp_write(s->pcb, hdr, (u16_t)hLen, TCP_WRITE_FLAG_COPY);

            // Send as much body as fits now; remainder via sent_cb
            tcpSendChunk(s);
        }

        // ── WebSocket frame processing ────────────────────────────────────────

        static void wsProcessData(ConnSlot* s, const uint8_t* data, u16_t len)
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
                        // Leere Frames hier abschliessen: WR_PAYLOAD liest ein Byte pro
                        // Durchlauf und liefe bei wsPayLen==0 in den nächsten Frame.
                        if (s->wsPayLen == 0)
                        {
                            if (s->wsOpcode == 0x08) // Close (no payload)
                            {
                                uint8_t closeReply[2] = {0x88, 0x00};
                                tcp_write(s->pcb, closeReply, 2, TCP_WRITE_FLAG_COPY);
                                tcp_output(s->pcb);
                                tcp_pcb* pcb = s->pcb;
                                s->ws->notifySocketConnect(s->uri, s->idx, false);
                                tcp_arg(pcb, nullptr);
                                tcp_recv(pcb, nullptr);
                                tcp_sent(pcb, nullptr);
                                tcp_poll(pcb, nullptr, 0);
                                releaseSlot(s);
                                tcp_close(pcb);
                                return;
                            }
                            if (s->wsOpcode == 0x09) // Ping (no payload) → send empty pong
                            {
                                uint8_t pong[2] = {0x8A, 0x00};
                                tcp_write(s->pcb, pong, 2, TCP_WRITE_FLAG_COPY);
                                tcp_output(s->pcb);
                            }
                            else if (s->wsOpcode == 0x01 || s->wsOpcode == 0x02) // empty text/binary
                            {
                                // Leeres Frame zustellen (z.B. blankes Enter in der Webconsole),
                                // damit der Empfänger eine Leerzeile verarbeiten kann.
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
                            s->ws->dropClient(s->uri, s->idx);
                            return; // slot is released, do not touch s again
                        }

                        if (s->wsPayRcvd < sizeof(s->rxBuf))
                            s->rxBuf[s->wsPayRcvd] = s->wsMasked
                                                          ? (b ^ s->wsMask[s->wsPayRcvd % 4])
                                                          : b;
                        s->wsPayRcvd++;

                        if (s->wsPayRcvd < (uint32_t)s->wsPayLen) break;

                        // Frame complete
                        if (s->wsOpcode == 0x08) // Close
                        {
                            uint8_t closeReply[2] = {0x88, 0x00};
                            tcp_write(s->pcb, closeReply, 2, TCP_WRITE_FLAG_COPY);
                            tcp_output(s->pcb);
                            tcp_pcb* pcb = s->pcb;
                            s->ws->notifySocketConnect(s->uri, s->idx, false);
                            // tcp_arg vor releaseSlot löschen, sonst findet lwIPs FIN-ACK
                            // einen bereits wiederverwendeten Slot.
                            tcp_arg(pcb, nullptr);
                            tcp_recv(pcb, nullptr);
                            tcp_sent(pcb, nullptr);
                            tcp_poll(pcb, nullptr, 0);
                            releaseSlot(s);
                            tcp_close(pcb);
                            return;
                        }
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
        }

        // ── lwIP callbacks ────────────────────────────────────────────────────

        static void onClose(ConnSlot* s)
        {
            if (!s) return;
            if (s->pcb)
            {
                // Callbacks abmelden, sonst greifen spätere lwIP-Events auf einen
                // wiederverwendeten Slot zu.
                tcp_arg(s->pcb, nullptr);
                tcp_recv(s->pcb, nullptr);
                tcp_sent(s->pcb, nullptr);
                tcp_poll(s->pcb, nullptr, 0);
                // tcp_err is left — lwIP fires it without a pcb pointer so it can't be cleared here.
            }
            if (s->state == CS_WS)
            {
                s->ws->notifySocketConnect(s->uri, s->idx, false);
            }
            releaseSlot(s);
        }

        static void onErr(void* arg, err_t /*err*/)
        {
            ConnSlot* s = (ConnSlot*)arg;
            if (!s) return;
            // pcb is already freed by lwIP on error — don't touch it
            s->pcb = nullptr; // mark as gone before onClose
            onClose(s);
        }

        static err_t onSent(void* arg, tcp_pcb* tpcb, u16_t /*len*/)
        {
            ConnSlot* s = (ConnSlot*)arg;
            if (!s || s->pcb != tpcb) return ERR_OK; // stale callback from a reused slot

            // Sendefortschritt = Lebenszeichen, sonst killt der Stall-Detector in
            // onPoll() jeden Transfer über ~22 s (grosse Downloads).
            s->pollCount = 0;

            if (s->state == CS_HTTP_SEND)
            {
                if (s->streaming)
                {
                    if (s->streamSent < s->streamTotal)
                    {
                        tcpSendStreamChunk(s);
                    }
                    else
                    {
                        // Streaming fertig — Cleanup und Verbindung schließen
                        if (s->streamCleanup) s->streamCleanup(s->streamCtx);
                        s->streaming = false;
                        s->streamReader = nullptr;
                        s->streamCleanup = nullptr;
                        s->streamCtx = nullptr;
                        tcp_pcb* pcb = s->pcb;
                        tcp_arg(pcb, nullptr);
                        tcp_recv(pcb, nullptr);
                        tcp_sent(pcb, nullptr);
                        tcp_poll(pcb, nullptr, 0);
                        releaseSlot(s);
                        tcp_close(pcb);
                    }
                }
                else if (!txDone(s))
                {
                    tcpSendChunk(s);
                }
                else
                {
                    // Callbacks vor releaseSlot löschen: nach tcp_close feuert lwIP im
                    // TIME_WAIT weiter onSent/onPoll für den alten pcb.
                    tcp_pcb* pcb = s->pcb;
                    tcp_arg(pcb, nullptr);
                    tcp_recv(pcb, nullptr);
                    tcp_sent(pcb, nullptr);
                    tcp_poll(pcb, nullptr, 0);
                    releaseSlot(s);
                    tcp_close(pcb);
                }
            }
            return ERR_OK;
        }

        static err_t onRecv(void* arg, tcp_pcb* tpcb, pbuf* p, err_t err)
        {
            ConnSlot* s = (ConnSlot*)arg;
            if (!s || s->pcb != tpcb) return ERR_OK; // stale callback from a reused slot

            if (err != ERR_OK || p == nullptr)
            {
                onClose(s);
                tcp_close(tpcb);
                return ERR_OK;
            }

            tcp_recved(tpcb, p->tot_len);
            s->pollCount = 0;

            if (s->state == CS_HTTP_RECV)
            {
                if (s->waitingForBody)
                {
                    // Header bereits geparst – restliche Body-Bytes sammeln
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
                    // Bytes in rxBuf akkumulieren (Header noch nicht komplett)
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
                            // sonst würde die abgeschnittene URI lautlos geroutet
                            const char resp414[] =
                                "HTTP/1.1 414 URI Too Long\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                            tcp_pcb* pcb = s->pcb;
                            tcp_write(pcb, resp414, sizeof(resp414) - 1, TCP_WRITE_FLAG_COPY);
                            tcp_output(pcb);
                            tcp_arg(pcb, nullptr);
                            tcp_recv(pcb, nullptr);
                            tcp_sent(pcb, nullptr);
                            tcp_poll(pcb, nullptr, 0);
                            releaseSlot(s);
                            tcp_close(pcb);
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
                                // Body vollständig im rxBuf
                                s->hasPendingRequest = true;
                            }
                            else if (s->contentLength > OPENKNX_WEBSERVER_MAX_BODY)
                            {
                                // Zu groß – 413 senden und Verbindung schließen
                                const char resp413[] =
                                    "HTTP/1.1 413 Content Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                                tcp_pcb* pcb = s->pcb;
                                tcp_write(pcb, resp413, sizeof(resp413) - 1, TCP_WRITE_FLAG_COPY);
                                tcp_output(pcb);
                                tcp_arg(pcb, nullptr);
                                tcp_recv(pcb, nullptr);
                                tcp_sent(pcb, nullptr);
                                tcp_poll(pcb, nullptr, 0);
                                releaseSlot(s);
                                tcp_close(pcb);
                            }
                            else
                            {
                                // Body-Buffer allozieren und bereits empfangene Bytes kopieren
                                s->bodyBuf = (uint8_t*)PSRAM_MALLOC(s->contentLength);
                                if (s->bodyBuf == nullptr)
                                {
                                    // Allocation fehlgeschlagen – Verbindung schließen
                                    tcp_pcb* pcb = s->pcb;
                                    tcp_arg(pcb, nullptr);
                                    tcp_recv(pcb, nullptr);
                                    tcp_sent(pcb, nullptr);
                                    tcp_poll(pcb, nullptr, 0);
                                    releaseSlot(s);
                                    tcp_close(pcb);
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
                        // rxBuf voll ohne komplette Header → wird nie vollständig
                        const char resp431[] =
                            "HTTP/1.1 431 Request Header Fields Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                        tcp_pcb* pcb = s->pcb;
                        tcp_write(pcb, resp431, sizeof(resp431) - 1, TCP_WRITE_FLAG_COPY);
                        tcp_output(pcb);
                        tcp_arg(pcb, nullptr);
                        tcp_recv(pcb, nullptr);
                        tcp_sent(pcb, nullptr);
                        tcp_poll(pcb, nullptr, 0);
                        releaseSlot(s);
                        tcp_close(pcb);
                    }
                }
            }
            else if (s->state == CS_WS)
            {
                for (pbuf* q = p; q != nullptr; q = q->next)
                {
                    if (!s->pcb) break; // slot released inside wsProcessData (close frame)
                    wsProcessData(s, (const uint8_t*)q->payload, q->len);
                }
            }

            pbuf_free(p);
            return ERR_OK;
        }

        static err_t onPoll(void* arg, tcp_pcb* tpcb)
        {
            ConnSlot* s = (ConnSlot*)arg;
            if (!s || s->pcb != tpcb) return ERR_OK; // stale callback from a reused slot

            s->pollCount++;

            if (s->state == CS_WS)
            {
                // WS-Ping alle ~60 s; bleibt der Pong aus, räumt lwIPs Retransmit-Timeout
                // die Verbindung über onErr ab.
                if (s->pollCount >= 30)
                {
                    s->pollCount = 0;
                    uint8_t ping[2] = {0x89, 0x00};
                    if (tcp_write(tpcb, ping, 2, TCP_WRITE_FLAG_COPY) == ERR_OK)
                        tcp_output(tpcb);
                }
                return ERR_OK;
            }

            // Abort stalled HTTP connections that never deliver a complete request.
            if (s->pollCount > 10) // ~20 s (poll interval = 4 × 500ms)
            {
                onClose(s);
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

            tcp_bind(pcb, IP_ADDR_ANY, 80);
            _listenPcb = tcp_listen(pcb);
            if (!_listenPcb)
            {
                logError("Webserver", "tcp_listen failed");
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
            // Ausserhalb des lwIP-Callback-Stacks: doHttpDispatch() baut die komplette
            // Antwort und ist für onRecv() zu schwer.
            for (int ci = 0; ci < MAX_CONN; ci++)
            {
                ConnSlot& s = g_slots[ci];
                if (!s.pcb || !s.hasPendingRequest) continue;
                s.hasPendingRequest = false;
                doHttpDispatch(&s);
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

        // Sendet sofort, an der Queue vorbei -- nur der Drain ruft das auf.
        // Failed braucht hier keine Sonderbehandlung: tote Verbindungen räumt lwIP
        // über onErr/onPoll ab, und der Drain versucht es bis dahin einmal pro Tick.
        WsSendResult Webserver::wsSendNow(const char*, int fd, const char* data, size_t len)
        {
            if (!_listenPcb) return WsSendResult::Failed;
            if (fd < 0 || fd >= MAX_CONN || !g_slots[fd].pcb || g_slots[fd].state != CS_WS)
                return WsSendResult::Failed;
            return wsTcpSend(g_slots[fd].pcb, data, len);
        }

        void Webserver::dropClient(const char* uri, int fd)
        {
            if (fd < 0 || fd >= MAX_CONN) return;
            ConnSlot* s = &g_slots[fd];

            // Deregister first, even for an already free slot: otherwise a cursor stays
            // in the queue that nothing removes and that warns on every tick.
            notifySocketConnect(uri, fd, false);

            // The teardown must run as a whole: each tcp_* wrapper takes the lock on its
            // own and releases the deferred ethernet IRQ on the way out, which would then
            // free the pcb the remaining lines still work on.
            LWIPMutex lock;

            // Only tear down if the slot still holds the same WS connection -- onAccept
            // may have reused it for a new HTTP request.
            if (!s->pcb || s->state != CS_WS) return;
            tcp_pcb* pcb = s->pcb;
            // tcp_arg vor releaseSlot löschen, sonst findet lwIPs FIN-ACK einen
            // bereits wiederverwendeten Slot.
            tcp_arg(pcb, nullptr);
            tcp_recv(pcb, nullptr);
            tcp_sent(pcb, nullptr);
            tcp_poll(pcb, nullptr, 0);
            releaseSlot(s);
            tcp_close(pcb);
        }

    } // namespace Network
} // namespace OpenKNX

#endif // OPENKNX_WEBSERVER
#endif // ARDUINO_ARCH_RP2040
