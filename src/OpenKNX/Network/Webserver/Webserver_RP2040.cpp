#ifdef ARDUINO_ARCH_RP2040
#ifdef OPENKNX_WEBSERVER

#include "OpenKNX.h"
#include "OpenKNX/Crypto/Base64.h"
#include "OpenKNX/Crypto/SHA1.h"
#include "OpenKNX/Network/Module.h"
#include "OpenKNX/Network/Webserver/Webserver.h"
#include <algorithm>
#include <lwip/tcp.h>
#include <map>
#include <string>
#include <vector>

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

            // HTTP RX
            char rxBuf[1536];
            uint16_t rxLen;

            // Parsed request
            uint8_t method;
            char uri[128];
            bool isWs;
            char wsKey[64];

            // HTTP TX (chunked streaming)
            uint8_t* txData;
            int txLen;
            int txSent;
            bool txFree; // delete[] txData when done

            // WS RX state machine
            WsRxState wsRx;
            uint8_t wsOpcode;
            uint64_t wsPayLen;
            uint32_t wsPayRcvd;
            uint8_t wsMask[4];
            bool wsMasked;
            uint8_t wsPayBuf[512];
            uint8_t wsHdrBuf[10]; // accumulate header bytes
            uint8_t wsHdrRcvd;

            bool hasPendingRequest = false;
        };

        static constexpr int MAX_CONN = 3;
        static ConnSlot g_slots[MAX_CONN];

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
            if (s->txFree)
            {
                delete[] s->txData;
                s->txFree = false;
            }
            s->txData = nullptr;
            s->pcb = nullptr;
        }

        // ── TCP send helpers ──────────────────────────────────────────────────

        static void tcpSendChunk(ConnSlot* s)
        {
            if (!s->txData || s->txSent >= s->txLen) return;
            int remaining = s->txLen - s->txSent;
            int canWrite = (int)tcp_sndbuf(s->pcb);
            int toWrite = remaining < canWrite ? remaining : canWrite;
            if (toWrite <= 0) return;

            tcp_write(s->pcb, s->txData + s->txSent, (u16_t)toWrite, TCP_WRITE_FLAG_COPY);
            tcp_output(s->pcb);
            s->txSent += toWrite;
        }

        // Sends a WS text frame over the lwIP TCP connection.
        // Checks tcp_sndbuf() first — if the send buffer is full (slow/stalled client),
        // drop the frame rather than stalling the watchdog-monitored loop.
        // Header + payload are combined into a single tcp_write to guarantee atomicity:
        // two separate writes risk the second failing (ERR_MEM from pbuf pool exhaustion)
        // while the header is already queued, permanently corrupting the WebSocket stream.
        static bool wsTcpSend(tcp_pcb* pcb, const char* data, size_t len)
        {
            // Stack buffer avoids heap fragmentation; 1500 covers max HTML-expanded log line
            uint8_t frame[1500];

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
                return false; // Frame too large
            }

            size_t frameLen = (size_t)hLen + len;
            if (frameLen > sizeof(frame)) return false;

            int avail = (int)tcp_sndbuf(pcb);
            if (avail < (int)frameLen) return false;

            if (len > 0) memcpy(frame + hLen, data, len);

            err_t err = tcp_write(pcb, frame, (u16_t)frameLen, TCP_WRITE_FLAG_COPY);
            if (err != ERR_OK) return false;

            tcp_output(pcb);
            return true;
        }

        // ── HTTP parsing ──────────────────────────────────────────────────────

        static void parseFirstLine(const char* line, uint8_t& method, char* uri, size_t uriSize)
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
            if (uLen >= uriSize) uLen = uriSize - 1;
            memcpy(uri, line, uLen);
            uri[uLen] = '\0';
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
            parseFirstLine(p, s->method, s->uri, sizeof(s->uri));
            p = lineEnd + 2;

            s->isWs = false;
            s->wsKey[0] = '\0';
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
                    else if (strcasecmp(name, "Sec-WebSocket-Key") == 0)
                        strncpy(s->wsKey, value, sizeof(s->wsKey) - 1);
                }
                p = nl + 2;
            }

            s->isWs = hasUpgrade && s->wsKey[0] != '\0' && s->method == WEB_GET;
            return true;
        }

        // ── HTTP dispatch ─────────────────────────────────────────────────────

        static void doHttpDispatch(ConnSlot* s)
        {
            if (s->isWs && s->ws->hasSocket(s->uri))
            {
                // WebSocket upgrade
                char accept[64] = {};
                if (!wsComputeAccept(s->wsKey, accept, sizeof(accept)))
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

                std::string uri(s->uri);
                s->ws->notifySocketConnect(uri, s->idx, true);

                // Get remote IP and port for logging
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

            // Extract remote IP and port from tcp_pcb
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

            WebResponse response;
            s->ws->handleRequest(request, response);

            s->ws->logRequest(request, response);

            // Build HTTP status string (e.g., "200 OK", "404 Not Found")
            int statusCode = response.statusCode();
            const char* statusText = "OK";
            if (statusCode == 404) statusText = "Not Found";
            else if (statusCode == 301) statusText = "Moved Permanently";
            else if (statusCode == 400) statusText = "Bad Request";
            else if (statusCode == 401) statusText = "Unauthorized";
            else if (statusCode == 403) statusText = "Forbidden";
            else if (statusCode == 500) statusText = "Internal Server Error";

            // Calculate content length, including layout if needed
            std::string layoutHeader;
            std::string layoutFooter;
            int contentLen = response.bodyLength();
            if (response.useLayout())
            {
                layoutHeader = s->ws->buildHeader(
                    response.activeMenuUri().empty() ? s->uri : response.activeMenuUri());
                layoutFooter = s->ws->buildFooter();
                contentLen = (int)layoutHeader.length() + response.bodyLength() + (int)layoutFooter.length();
            }

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

            for (auto& h : response.responseHeaders())
            {
                int n = snprintf(hdr + hLen, sizeof(hdr) - hLen,
                                 "%s: %s\r\n", h.first.c_str(), h.second.c_str());
                hLen += n;
                if (hLen >= (int)sizeof(hdr) - 4) break;
            }
            hdr[hLen++] = '\r';
            hdr[hLen++] = '\n';
            hdr[hLen] = '\0';

            int bodyLen = contentLen;
            uint8_t* body = nullptr;
            bool bodyFree = false;
            if (bodyLen > 0)
            {
                if (response.useLayout())
                {
                    body = new uint8_t[bodyLen];
                    int pos = 0;
                    memcpy(body + pos, layoutHeader.c_str(), layoutHeader.length());
                    pos += layoutHeader.length();
                    memcpy(body + pos, response.body(), response.bodyLength());
                    pos += response.bodyLength();
                    memcpy(body + pos, layoutFooter.c_str(), layoutFooter.length());
                    bodyFree = true;
                }
                else if (response.isStatic())
                {
                    body = const_cast<uint8_t*>(
                        reinterpret_cast<const uint8_t*>(response.body()));
                }
                else
                {
                    body = new uint8_t[bodyLen];
                    memcpy(body, response.body(), bodyLen);
                    bodyFree = true;
                }
            }

            s->state = CS_HTTP_SEND;
            s->txData = body;
            s->txLen = bodyLen;
            s->txSent = 0;
            s->txFree = bodyFree;

            // Send header immediately (always fits — max ~256 bytes)
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
                        // Zero-length frames must be completed here: WR_PAYLOAD reads one byte
                        // per iteration, so entering it with wsPayLen==0 would consume the
                        // first byte of the next frame as phantom payload, desynchronising the
                        // stream. This is the common case for browser pong replies to our ping.
                        if (s->wsPayLen == 0)
                        {
                            if (s->wsOpcode == 0x08) // Close (no payload)
                            {
                                uint8_t closeReply[2] = {0x88, 0x00};
                                tcp_write(s->pcb, closeReply, 2, TCP_WRITE_FLAG_COPY);
                                tcp_output(s->pcb);
                                tcp_pcb* pcb = s->pcb;
                                std::string uri(s->uri);
                                s->ws->notifySocketConnect(uri, s->idx, false);
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
                            // Pong (0x0A) and empty text/binary frames: nothing to deliver.
                            s->wsRx = WR_HDR;
                            s->wsHdrRcvd = 0;
                            break;
                        }
                        s->wsRx = WR_PAYLOAD;
                        break;

                    case WR_PAYLOAD:
                    {
                        if (s->wsPayRcvd < sizeof(s->wsPayBuf))
                            s->wsPayBuf[s->wsPayRcvd] = s->wsMasked
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
                            std::string uri(s->uri);
                            s->ws->notifySocketConnect(uri, s->idx, false);
                            // Clear tcp_arg BEFORE releasing slot — prevents lwIP's FIN-ACK callback
                            // from finding a potentially reused slot via the stale tcp_arg pointer.
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
                            int payLen = (int)(s->wsPayRcvd < sizeof(s->wsPayBuf)
                                                   ? s->wsPayRcvd
                                                   : sizeof(s->wsPayBuf));
                            std::string uri(s->uri);
                            s->ws->notifySocketMessage(uri, s->idx, s->wsPayBuf, payLen, isBinary);
                        }

                        // Reset for next frame
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
                // Deregister callbacks so any future lwIP events for this pcb are no-ops.
                // This prevents stale tcp_arg pointers from corrupting a reused slot.
                tcp_arg(s->pcb, nullptr);
                tcp_recv(s->pcb, nullptr);
                tcp_sent(s->pcb, nullptr);
                tcp_poll(s->pcb, nullptr, 0);
                // tcp_err is left — lwIP fires it without a pcb pointer so it can't be cleared here.
            }
            if (s->state == CS_WS)
            {
                std::string uri(s->uri);
                s->ws->notifySocketConnect(uri, s->idx, false);
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

            if (s->state == CS_HTTP_SEND)
            {
                if (s->txSent < s->txLen)
                {
                    tcpSendChunk(s);
                }
                else
                {
                    // Response fully sent — clear all callbacks BEFORE releasing the slot.
                    // tcp_close(pcb) enters TIME_WAIT and lwIP will still fire onSent/onPoll
                    // for the old pcb. Without this, a reused slot gets corrupted by stale callbacks.
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
                // Accumulate into rxBuf
                for (pbuf* q = p; q != nullptr; q = q->next)
                {
                    size_t space = sizeof(s->rxBuf) - s->rxLen - 1;
                    size_t copy = q->len < space ? q->len : space;
                    memcpy(s->rxBuf + s->rxLen, q->payload, copy);
                    s->rxLen += (uint16_t)copy;
                    s->rxBuf[s->rxLen] = '\0';
                }

                if (parseHeaders(s))
                    s->hasPendingRequest = true;
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
                // Send a WS ping every ~60 s to detect dead connections.
                // The browser must reply with a pong; if it doesn't, lwIP's retransmit
                // timeout will eventually fire onErr and clean up the slot.
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

        void Webserver::loop()
        {
            // Dispatch pending HTTP requests outside the lwIP callback stack.
            // doHttpDispatch() builds the full response (incl. buildOverviewPage)
            // which is too heavy to run inside onRecv().
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

        bool Webserver::sendToClient(const std::string&, int fd, const char* data, size_t len)
        {
            if (!_listenPcb) return false;
            if (fd < 0 || fd >= MAX_CONN || !g_slots[fd].pcb || g_slots[fd].state != CS_WS)
                return false;
            return wsTcpSend(g_slots[fd].pcb, data, len);
        }

        void Webserver::sendWebsocketMessage(const std::string& uri, const char* message, int fd)
        {
            if (!_listenPcb) return;

            if (fd >= 0)
            {
                if (fd < MAX_CONN && g_slots[fd].pcb && g_slots[fd].state == CS_WS)
                    wsTcpSend(g_slots[fd].pcb, message, strlen(message));
                return;
            }

            auto it = _socketClients.find(uri);
            if (it == _socketClients.end() || it->second.empty()) return;

            size_t len = strlen(message);
            std::vector<int> clients = it->second;
            for (int idx : clients)
            {
                if (idx < 0 || idx >= MAX_CONN) continue;
                ConnSlot& s = g_slots[idx];
                if (s.pcb && s.state == CS_WS)
                    wsTcpSend(s.pcb, message, len);
            }
        }

        void Webserver::notifySocketConnect(const std::string& uri, int fd, bool connected)
        {
            if (connected)
            {
                _socketClients[uri].push_back(fd);
            }
            else
            {
                auto it = _socketClients.find(uri);
                if (it != _socketClients.end())
                {
                    auto& v = it->second;
                    v.erase(std::remove(v.begin(), v.end(), fd), v.end());
                }
            }

            for (auto& sock : _sockets)
            {
                if (sock.uri == uri && sock.onConnect)
                {
                    sock.onConnect(fd, connected);
                    break;
                }
            }
        }

        void Webserver::notifySocketMessage(const std::string& uri, int fd,
                                            uint8_t* data, int length, bool isBinary)
        {
            for (auto& sock : _sockets)
            {
                if (sock.uri == uri && sock.onMessage)
                {
                    WebSocketFrame frame{data, length, isBinary};
                    sock.onMessage(fd, &frame);
                    break;
                }
            }
        }

    } // namespace Network
} // namespace OpenKNX

#endif // OPENKNX_WEBSERVER
#endif // ARDUINO_ARCH_RP2040
