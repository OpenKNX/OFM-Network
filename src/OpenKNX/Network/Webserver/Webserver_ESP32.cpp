#ifdef ARDUINO_ARCH_ESP32
#ifdef OPENKNX_WEBSERVER

#include "OpenKNX.h"
#include "OpenKNX/Network/Module.h"
#include "OpenKNX/Network/Webserver/Webserver.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cstdint>
#include <esp_http_server.h>
#include <fcntl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha1.h>
#include <sys/socket.h>
#include <vector>

#ifndef ESP32_WEB_CTRL_PORT
#define ESP32_WEB_CTRL_PORT 9080
#endif

// Gleichzeitige WS-Verbindungen; darüber hinaus HTTP 503. Jede belegt einen
// httpd-Socketslot (max_open_sockets).
#ifndef OPENKNX_WEBSOCKET_MAX
#define OPENKNX_WEBSOCKET_MAX 3
#endif

// RX-Obergrenze pro Verbindung; grössere Frames trennen die Verbindung.
#ifndef OPENKNX_WEBSOCKET_RX_CAP
#define OPENKNX_WEBSOCKET_RX_CAP 2048
#endif

namespace OpenKNX
{
    namespace Network
    {

        // ── WS handshake helpers ────────────────────────────────────────────

        static const char WS_MAGIC[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

        // Serialisiert die send()-Syscalls — zwei Sender würden sonst ihre Frames
        // auf demselben Socket verschränken.
        static SemaphoreHandle_t g_wsTxMutex = nullptr;

        // RAII-Guard für _socketClients + g_wsSessions. Der rekursive Mutex liegt auf
        // der Webserver-Instanz, damit auch Webserver.cpp ihn erreicht.
        class WsStateGuard
        {
            const Webserver* _w;

          public:
            explicit WsStateGuard(const Webserver* w) : _w(w) { _w->wsStateLock(); }
            ~WsStateGuard() { _w->wsStateUnlock(); }
        };

        // Non-blocking raw socket write, serialized against other senders.
        static int wsRawSend(int fd, const void* data, size_t len)
        {
            if (g_wsTxMutex) xSemaphoreTake(g_wsTxMutex, portMAX_DELAY);
            int r = send(fd, data, len, MSG_NOSIGNAL | MSG_DONTWAIT);
            if (g_wsTxMutex) xSemaphoreGive(g_wsTxMutex);
            return r;
        }

        static bool wsComputeAccept(const char* key, char* out, size_t outSize)
        {
            char combined[128];
            int n = snprintf(combined, sizeof(combined), "%s%s", key, WS_MAGIC);
            if (n <= 0 || n >= (int)sizeof(combined)) return false;

            uint8_t sha[20];
#if defined(MBEDTLS_VERSION_NUMBER) && MBEDTLS_VERSION_NUMBER >= 0x03000000
            mbedtls_sha1((const uint8_t*)combined, (size_t)n, sha);
#else
            mbedtls_sha1_ret((const uint8_t*)combined, (size_t)n, sha);
#endif

            size_t written = 0;
            int rc = mbedtls_base64_encode((uint8_t*)out, outSize, &written, sha, sizeof(sha));
            return rc == 0;
        }

        // WS-Textframe über den rohen Socket, aus jedem Task aufrufbar. MSG_DONTWAIT,
        // damit nichts blockiert.
        static WsSendResult wsSendText(int fd, const char* data, size_t len)
        {
            uint8_t hdr[10];
            int hLen = 0;
            hdr[hLen++] = 0x81; // FIN + text opcode
            if (len < 126)
            {
                hdr[hLen++] = (uint8_t)len;
            }
            else if (len < 65536)
            {
                hdr[hLen++] = 126;
                hdr[hLen++] = (uint8_t)(len >> 8);
                hdr[hLen++] = (uint8_t)(len & 0xFF);
            }
            else
            {
                hdr[hLen++] = 127;
                for (int i = 7; i >= 0; --i)
                    hdr[hLen++] = (uint8_t)((len >> (8 * i)) & 0xFF);
            }

            // One combined buffer → single send() call → atomically complete or not sent.
            std::vector<uint8_t> frame(hLen + len);
            memcpy(frame.data(), hdr, hLen);
            if (len > 0) memcpy(frame.data() + hLen, data, len);

            int r = wsRawSend(fd, frame.data(), frame.size());
            if (r < 0)
                return (errno == EAGAIN || errno == EWOULDBLOCK) ? WsSendResult::WouldBlock
                                                                 : WsSendResult::Failed;
            if (r != (int)frame.size()) return WsSendResult::Failed; // Teil-Write
            return WsSendResult::Ok;
        }

        // ── Per-session WS state ────────────────────────────────────────────

        struct WsSession
        {
            Webserver* ws;
            int fd;
            std::string uri;
            httpd_req_t* async;       // async handle — keeps the socket out of httpd's poll set
            std::vector<uint8_t> rx;  // RX accumulation buffer (partial frames span loop ticks)
            bool markedDead = false; // siehe markSessionDead()
        };

        // Bedient von Webserver::loop(). Unter wsStateLock(): httpd-Task hängt beim
        // Upgrade an, der Loop-Task iteriert und entfernt.
        static std::vector<WsSession*> g_wsSessions;

        // Ein Teil-Write hat den Frame-Stream desynchronisiert, aber recv() läuft
        // weiter — die normale Dead-Erkennung in loop() greift also nicht. Markieren,
        // loop() räumt beim nächsten Tick ab.
        static void markSessionDead(Webserver* ws, int fd)
        {
            WsStateGuard guard(ws);
            for (WsSession* sess : g_wsSessions)
            {
                if (sess->fd == fd)
                {
                    sess->markedDead = true;
                    break;
                }
            }
        }

        // Verarbeitet alle vollständigen Frames in sess->rx; ein angefangenes bleibt für
        // den nächsten Tick liegen. true = Verbindung schliessen. Ruft nie recv().
        static bool wsParseFrames(WsSession* sess)
        {
            std::vector<uint8_t>& rx = sess->rx;
            size_t off = 0;
            bool close = false;

            while (rx.size() - off >= 2)
            {
                uint8_t b0 = rx[off];
                uint8_t b1 = rx[off + 1];
                uint8_t op = b0 & 0x0F;
                bool masked = (b1 & 0x80) != 0;
                uint64_t payLen = b1 & 0x7F;
                size_t hdrLen = 2;

                if (payLen == 126)
                {
                    if (rx.size() - off < 4) break;
                    payLen = ((uint16_t)rx[off + 2] << 8) | rx[off + 3];
                    hdrLen = 4;
                }
                else if (payLen == 127)
                {
                    if (rx.size() - off < 10) break;
                    payLen = 0;
                    for (int i = 0; i < 8; ++i)
                        payLen = (payLen << 8) | rx[off + 2 + i];
                    hdrLen = 10;
                }

                // A frame larger than the bounded RX buffer can never complete → drop client.
                if (payLen > OPENKNX_WEBSOCKET_RX_CAP)
                {
                    close = true;
                    break;
                }

                size_t maskLen = masked ? 4 : 0;
                uint64_t need = (uint64_t)hdrLen + maskLen + payLen;
                if ((uint64_t)(rx.size() - off) < need) break; // frame incomplete → wait for more

                uint8_t mask[4] = {};
                if (masked)
                    for (int i = 0; i < 4; ++i) mask[i] = rx[off + hdrLen + i];

                size_t dataOff = off + hdrLen + maskLen;
                std::vector<uint8_t> payload((size_t)payLen);
                for (size_t i = 0; i < (size_t)payLen; ++i)
                    payload[i] = masked ? (rx[dataOff + i] ^ mask[i % 4]) : rx[dataOff + i];

                off = dataOff + (size_t)payLen; // frame consumed

                if (op == 0x08) // Close → reply and stop
                {
                    uint8_t closeReply[2] = {0x88, 0x00};
                    wsRawSend(sess->fd, closeReply, 2);
                    close = true;
                    break;
                }
                if (op == 0x09) // Ping → pong
                {
                    uint8_t pong[2] = {0x8A, 0x00};
                    wsRawSend(sess->fd, pong, 2);
                    continue;
                }
                if (op == 0x0A) // Pong → silently discard
                    continue;
                if (op == 0x01 || op == 0x02) // text / binary
                {
                    bool isBinary = (op == 0x02);
                    sess->ws->notifySocketMessage(sess->uri, sess->fd, payload.data(), (int)payLen, isBinary);
                }
            }

            if (off > 0) rx.erase(rx.begin(), rx.begin() + off);
            return close;
        }

        // ── WebSocket upgrade ───────────────────────────────────────────────

        static bool doWsUpgrade(httpd_req_t* req, Webserver* wsObj)
        {
            char key[128] = {};
            if (httpd_req_get_hdr_value_str(req, "Sec-WebSocket-Key", key, sizeof(key)) != ESP_OK)
                return false;

            char accept[64] = {};
            if (!wsComputeAccept(key, accept, sizeof(accept)))
                return false;

            int fd = httpd_req_to_sockfd(req);
            std::string uri = req->uri;

            // Zuerst aus httpds Poll-Set lösen, sonst hält httpd die WS-Bytes für einen
            // neuen HTTP-Request. Vor dem 101, damit ein Fehler hier noch eine normale
            // HTTP-Fehlerantwort erlaubt.
            httpd_req_t* async = nullptr;
            if (httpd_req_async_handler_begin(req, &async) != ESP_OK)
                return false;

            char resp[256];
            int rLen = snprintf(resp, sizeof(resp),
                                "HTTP/1.1 101 Switching Protocols\r\n"
                                "Upgrade: websocket\r\n"
                                "Connection: Upgrade\r\n"
                                "Sec-WebSocket-Accept: %s\r\n\r\n",
                                accept);
            send(fd, resp, rLen, MSG_NOSIGNAL);

            // TCP keepalive — detect silently dead peers within ~9 s
            int ka = 1, kaIdle = 3, kaIntvl = 2, kaCnt = 3;
            setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &ka, sizeof(ka));
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &kaIdle, sizeof(kaIdle));
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &kaIntvl, sizeof(kaIntvl));
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &kaCnt, sizeof(kaCnt));

            // Non-blocking — the loop-task recv() in Webserver::loop() must never stall.
            int fl = fcntl(fd, F_GETFL, 0);
            if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);

            WsSession* sess = new WsSession{wsObj, fd, uri, async, {}};
            {
                WsStateGuard guard(wsObj);
                g_wsSessions.push_back(sess);
            }

            wsObj->notifySocketConnect(uri, fd, true);
            return true;
        }

        // ── HTTP handler (handles both plain HTTP and WS upgrade) ───────────

        static uint8_t fromHttpdMethod(int method)
        {
            if (method == HTTP_POST) return WEB_POST;
            if (method == HTTP_PUT) return WEB_PUT;
            if (method == HTTP_DELETE) return WEB_DELETE;
            return WEB_GET;
        }

        static esp_err_t platformHttpHandler(httpd_req_t* req)
        {
            Webserver* ws = (Webserver*)req->user_ctx;
            std::string uri = req->uri;

            char clientIp[INET6_ADDRSTRLEN] = "?";
            uint32_t remoteIpAddr = 0;
            uint16_t remotePort = 0;

            int sockfd = httpd_req_to_sockfd(req);

            if (sockfd >= 0)
            {
                struct sockaddr_in6 peer6;
                socklen_t peerLen = sizeof(peer6);
                memset(&peer6, 0, sizeof(peer6));

                int gp_result = getpeername(sockfd, (struct sockaddr*)&peer6, &peerLen);

                if (gp_result == 0 && peer6.sin6_family == AF_INET6)
                {
                    remotePort = ntohs(peer6.sin6_port);

                    struct in6_addr* addr6 = &peer6.sin6_addr;
                    if (IN6_IS_ADDR_V4MAPPED(addr6))
                    {
                        uint32_t ipv4 = *(uint32_t*)&addr6->s6_addr[12];
                        struct in_addr ipv4_addr;
                        ipv4_addr.s_addr = ipv4;
                        inet_ntop(AF_INET, &ipv4_addr, clientIp, sizeof(clientIp));
                        remoteIpAddr = ipv4;
                    }
                    else
                    {
                        inet_ntop(AF_INET6, addr6, clientIp, sizeof(clientIp));
                    }
                }
            }

            if (req->method == HTTP_GET)
            {
                char upgrade[32] = {};
                httpd_req_get_hdr_value_str(req, "Upgrade", upgrade, sizeof(upgrade));
                if (strcasecmp(upgrade, "websocket") == 0)
                {
                    if (!ws->hasSocket(uri))
                    {
                        WebRequest wsReq;
                        wsReq.uri = uri;
                        wsReq.method = WEB_GET;
                        wsReq.setRemoteAddr(remoteIpAddr);
                        wsReq.setRemotePort(remotePort);
                        ws->logWebsocket(wsReq, 404);
                        httpd_resp_set_status(req, "404 Not Found");
                        httpd_resp_set_type(req, "text/plain");
                        httpd_resp_sendstr(req, "No WebSocket handler registered for this URI");
                        return ESP_OK;
                    }
                    // Reject when all WS slots are in use rather than holding sockets open.
                    size_t active;
                    {
                        WsStateGuard guard(ws);
                        active = g_wsSessions.size();
                    }
                    if (active >= OPENKNX_WEBSOCKET_MAX)
                    {
                        WebRequest wsReq;
                        wsReq.uri = uri;
                        wsReq.method = WEB_GET;
                        wsReq.setRemoteAddr(remoteIpAddr);
                        wsReq.setRemotePort(remotePort);
                        ws->logWebsocket(wsReq, 503);
                        httpd_resp_set_status(req, "503 Service Unavailable");
                        httpd_resp_set_type(req, "text/plain");
                        httpd_resp_sendstr(req, "WebSocket connection limit reached");
                        return ESP_OK;
                    }

                    WebRequest wsReq;
                    wsReq.uri = uri;
                    wsReq.setRemoteAddr(remoteIpAddr);
                    wsReq.setRemotePort(remotePort);
                    ws->logWebsocket(wsReq);
                    if (doWsUpgrade(req, ws))
                        return ESP_OK;
                    logError("Webserver", "WS upgrade failed: %s", uri.c_str());
                }
            }

            WebRequest request;
            request.uri = uri;
            request.method = fromHttpdMethod((int)req->method);
            request.setRemoteAddr(remoteIpAddr);
            request.setRemotePort(remotePort);

            // Body lesen (POST/PUT) – bis OPENKNX_WEBSERVER_MAX_BODY
            if (req->content_len > OPENKNX_WEBSERVER_MAX_BODY)
            {
                // sonst käme der Request mit leerem Body bei handleRequest() an
                WebResponse response413;
                response413.setStatus(413);
                ws->logRequest(request, response413);
                httpd_resp_set_status(req, "413 Content Too Large");
                httpd_resp_set_type(req, "text/plain");
                httpd_resp_sendstr(req, "Content Too Large");
                return ESP_OK;
            }

            std::vector<uint8_t, PsramAllocator<uint8_t>> bodyData;
            if (req->content_len > 0)
            {
                bodyData.resize(req->content_len);
                int remaining = (int)req->content_len, received = 0;
                while (remaining > 0)
                {
                    int r = httpd_req_recv(req, (char*)bodyData.data() + received, remaining);
                    if (r <= 0) break;
                    received += r;
                    remaining -= r;
                }
                if (received != (int)req->content_len)
                {
                    // Abbruch während des Empfangs — sonst gilt der verkürzte Body als vollständig
                    httpd_resp_set_status(req, "400 Bad Request");
                    httpd_resp_set_type(req, "text/plain");
                    httpd_resp_sendstr(req, "Incomplete body");
                    return ESP_OK;
                }
                request.setBody(bodyData.data(), (size_t)received);
            }

            WebResponse response;
            ws->handleRequest(request, response);

            ws->logRequest(request, response);

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

            char statusStr[32];
            snprintf(statusStr, sizeof(statusStr), "%d %s", statusCode, statusText);
            httpd_resp_set_status(req, statusStr);
            httpd_resp_set_type(req, response.contentType());
            for (auto& h : response.responseHeaders())
                httpd_resp_set_hdr(req, h.first.c_str(), h.second.c_str());

            if (response.isStreaming())
            {
                // Streaming-Download: Transfer-Encoding: chunked (kein Content-Length nötig)
                uint8_t buf[512];
                size_t got;
                while ((got = response.readStreamChunk(buf, sizeof(buf))) > 0)
                    httpd_resp_send_chunk(req, (char*)buf, (ssize_t)got);
                httpd_resp_send_chunk(req, nullptr, 0);
                response.cleanupStream();
            }
            else if (response.segmentCount() <= 1)
            {
                // Ein Block (oder leer) → in einem Rutsch, httpd setzt Content-Length
                const bool has = response.segmentCount() > 0;
                httpd_resp_send(req,
                                has ? (const char*)response.segments()[0].data : "",
                                has ? response.segments()[0].len : 0);
            }
            else
            {
                // Mehrere Blöcke als Chunks: keine Kopie, dafür chunked statt Content-Length
                for (int i = 0; i < response.segmentCount(); i++)
                    httpd_resp_send_chunk(req,
                                          (const char*)response.segments()[i].data,
                                          response.segments()[i].len);
                httpd_resp_send_chunk(req, nullptr, 0);
            }
            return ESP_OK;
        }

        // ── setupEsp32 — httpd start ────────────────────────────────────────

        void Webserver::setupEsp32()
        {
            // Shared-state mutexes (created once, before any WS can be upgraded)
            if (_wsLock == nullptr) _wsLock = xSemaphoreCreateRecursiveMutex();
            if (g_wsTxMutex == nullptr) g_wsTxMutex = xSemaphoreCreateMutex();

            // WS-Sockets werden per Async-Handler aus httpd gelöst und aus
            // Webserver::loop() bedient — kein Task pro Verbindung
            httpd_config_t config = HTTPD_DEFAULT_CONFIG();
            config.uri_match_fn = httpd_uri_match_wildcard;
            config.ctrl_port = ESP32_WEB_CTRL_PORT;
            config.max_uri_handlers = 8; // 4 Methoden × Wildcard + Headroom
            config.max_open_sockets = 7; // HTTP keep-alive + detached WS sockets (3 reserved internally)
            config.lru_purge_enable = true; // free idle HTTP keep-alives so new WS upgrades find a slot
            config.recv_wait_timeout = 5;
            config.send_wait_timeout = 5;
            config.stack_size = 8192; // Logger-Mutex (xSemaphoreTakeRecursive) braucht ~400 B extra gegenüber Default 4096

            httpd_handle_t server;
            if (httpd_start(&server, &config) != ESP_OK)
            {
                logError("Webserver", "Failed to start");
                return;
            }
            _server = server;

            static const char wildcardUri[] = "/*";
            for (int m : {HTTP_GET, HTTP_POST, HTTP_PUT, HTTP_DELETE})
            {
                httpd_uri_t httpUri = {
                    .uri = wildcardUri,
                    .method = (httpd_method_t)m,
                    .handler = platformHttpHandler,
                    .user_ctx = this,
                };
                httpd_register_uri_handler(server, &httpUri);
            }

            openknx.logger.logWithValues("Started on port 80");
        }

        // ── Webserver lifecycle ─────────────────────────────────────────────

        void Webserver::loop()
        {
            if (!_server) return;

            // Nur diese Schleife löscht Sessions, der Snapshot bleibt also gültig
            std::vector<WsSession*> sessions;
            {
                WsStateGuard guard(this);
                sessions = g_wsSessions;
            }
            if (sessions.empty()) return;

            uint8_t tmp[512];
            for (WsSession* sess : sessions)
            {
                bool dead = false;

                // Drain whatever is available right now — fully non-blocking.
                for (;;)
                {
                    int n = recv(sess->fd, tmp, sizeof(tmp), MSG_DONTWAIT);
                    if (n > 0)
                    {
                        sess->rx.insert(sess->rx.end(), tmp, tmp + n);
                        if (sess->rx.size() > OPENKNX_WEBSOCKET_RX_CAP)
                        {
                            dead = true;
                            break;
                        }
                        continue;
                    }
                    if (n == 0)
                    {
                        dead = true; // peer closed
                        break;
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        break; // nothing more pending
                    dead = true; // real socket error
                    break;
                }

                if (!dead && wsParseFrames(sess)) dead = true;
                if (sess->markedDead) dead = true;

                if (dead)
                {
                    // Socket zurück an httpd (schliesst ihn), dann Registry räumen
                    notifySocketConnect(sess->uri, sess->fd, false);
                    httpd_req_async_handler_complete(sess->async);
                    {
                        WsStateGuard guard(this);
                        auto it = std::find(g_wsSessions.begin(), g_wsSessions.end(), sess);
                        if (it != g_wsSessions.end()) g_wsSessions.erase(it);
                    }
                    delete sess;
                }
            }
        }

        bool Webserver::isRunning()
        {
            return _server != nullptr;
        }

        size_t Webserver::maxWebsocketPayload() const
        {
            return SIZE_MAX; // wsSendText() allokiert pro Frame auf dem Heap
        }

        bool Webserver::sendToClient(const std::string& uri, int fd, const char* data, size_t len)
        {
            if (!_server) return false;
            WsSendResult r = wsSendText(fd, data, len);
            if (r == WsSendResult::Failed)
            {
                notifySocketConnect(uri, fd, false);
                markSessionDead(this, fd);
            }
            return r == WsSendResult::Ok;
        }

        // ── Shared WS state lock (recursive; also used from Webserver.cpp) ───

        void Webserver::wsStateLock() const
        {
            if (_wsLock) xSemaphoreTakeRecursive((SemaphoreHandle_t)_wsLock, portMAX_DELAY);
        }

        void Webserver::wsStateUnlock() const
        {
            if (_wsLock) xSemaphoreGiveRecursive((SemaphoreHandle_t)_wsLock);
        }

        // ── Socket dispatch ─────────────────────────────────────────────────

        // Pflegt nur _socketClients; die WsSession gehört Webserver::loop(). Damit aus
        // jedem Task aufrufbar und beim Entfernen idempotent.
        void Webserver::notifySocketConnect(const std::string& uri, int fd, bool connected)
        {
            {
                WsStateGuard guard(this);
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
            }

            // Fire the user callback outside the lock (it may call back into the webserver)
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

        // ── WS send — direct socket calls, safe from any task ───────────────

        void Webserver::sendWebsocketMessage(const std::string& uri, const char* message, int fd)
        {
            if (!_server) return;
            if (fd >= 0)
            {
                if (wsSendText(fd, message, strlen(message)) == WsSendResult::Failed)
                {
                    notifySocketConnect(uri, fd, false);
                    markSessionDead(this, fd);
                }
                return;
            }
            std::vector<int> clients;
            {
                WsStateGuard guard(this);
                auto it = _socketClients.find(uri);
                if (it == _socketClients.end() || it->second.empty()) return;
                clients = it->second; // snapshot under lock
            }

            size_t len = strlen(message);
            for (int cfd : clients)
            {
                if (wsSendText(cfd, message, len) == WsSendResult::Failed)
                {
                    notifySocketConnect(uri, cfd, false);
                    markSessionDead(this, cfd);
                }
            }
        }

    } // namespace Network
} // namespace OpenKNX

#endif // OPENKNX_WEBSERVER
#endif // ARDUINO_ARCH_ESP32
