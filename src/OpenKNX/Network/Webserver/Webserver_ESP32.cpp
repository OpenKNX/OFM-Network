#ifdef ARDUINO_ARCH_ESP32
#ifdef OPENKNX_WEBSERVER

#include "OpenKNX.h"
#include "OpenKNX/Network/Module.h"
#include "OpenKNX/Network/Webserver/Webserver.h"
#include <arpa/inet.h>
#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <map>
#include <mbedtls/base64.h>
#include <mbedtls/sha1.h>
#include <sys/socket.h>

#ifndef ESP32_WEB_CTRL_PORT
#define ESP32_WEB_CTRL_PORT 9080
#endif

namespace OpenKNX
{
    namespace Network
    {

        // ── WS handshake helpers ────────────────────────────────────────────

        static const char WS_MAGIC[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

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

        // Sends a WS text frame directly over the raw socket (callable from any task).
        // Uses MSG_DONTWAIT so calls from loopTask (logger callbacks) never block —
        // EAGAIN means the TCP send buffer is full (slow/stalled client); drop the frame
        // rather than stalling the watchdog-monitored loop.
        static bool wsSendText(int fd, const char* data, size_t len)
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

            int r = send(fd, frame.data(), (int)frame.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
            if (r < 0)
                return (errno == EAGAIN || errno == EWOULDBLOCK); // slow client → drop, stay connected
            return r == (int)frame.size();
        }

        // ── Per-session WS state ────────────────────────────────────────────

        struct WsSession
        {
            Webserver* ws;
            httpd_handle_t server;
            int fd;
            std::string uri;
        };

        static std::map<int, WsSession*> g_wsSessions; // fd → session, httpd-task only
        static SemaphoreHandle_t g_wsStateMutex = nullptr;

        static SemaphoreHandle_t wsStateMutex()
        {
            if (g_wsStateMutex == nullptr)
            {
                g_wsStateMutex = xSemaphoreCreateMutex();
            }
            return g_wsStateMutex;
        }

        class WsStateLock
        {
          public:
            WsStateLock()
            {
                SemaphoreHandle_t m = wsStateMutex();
                _locked = (m != nullptr) && (xSemaphoreTake(m, portMAX_DELAY) == pdTRUE);
            }

            ~WsStateLock()
            {
                if (_locked)
                {
                    xSemaphoreGive(wsStateMutex());
                }
            }

          private:
            bool _locked = false;
        };

        // Installed as recv override; owns the socket for the WS session lifetime.
        // Processes at most one WS frame per invocation so the HTTPD task is never
        // monopolized by a single idle/alive websocket session.
        static int wsFrameRecv(httpd_handle_t hd, int fd, char* buf, size_t bufLen, int flags)
        {
            WsSession* sess = nullptr;
            {
                WsStateLock lock;
                auto it = g_wsSessions.find(fd);
                if (it != g_wsSessions.end()) sess = it->second;
            }

            uint8_t hdr[2];
            int r = recv(fd, hdr, 2, MSG_WAITALL);
            if (r <= 0)
            {
                // No data right now: hand control back to HTTPD so other sockets/routes keep flowing.
                if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                    return HTTPD_SOCK_ERR_TIMEOUT;
                if (sess) sess->ws->notifySocketConnect(sess->uri, fd, false);
                return HTTPD_SOCK_ERR_FAIL;
            }

            bool fin = (hdr[0] & 0x80) != 0;
            (void)fin;
            uint8_t op = hdr[0] & 0x0F;
            bool masked = (hdr[1] & 0x80) != 0;
            uint64_t payLen = hdr[1] & 0x7F;

            if (payLen == 126)
            {
                uint8_t ext[2];
                if (recv(fd, ext, 2, MSG_WAITALL) != 2)
                {
                    if (sess) sess->ws->notifySocketConnect(sess->uri, fd, false);
                    return HTTPD_SOCK_ERR_FAIL;
                }
                payLen = ((uint16_t)ext[0] << 8) | ext[1];
            }
            else if (payLen == 127)
            {
                uint8_t ext[8];
                if (recv(fd, ext, 8, MSG_WAITALL) != 8)
                {
                    if (sess) sess->ws->notifySocketConnect(sess->uri, fd, false);
                    return HTTPD_SOCK_ERR_FAIL;
                }
                payLen = 0;
                for (int i = 0; i < 8; ++i)
                    payLen = (payLen << 8) | ext[i];
            }

            uint8_t mask[4] = {};
            if (masked)
            {
                if (recv(fd, mask, 4, MSG_WAITALL) != 4)
                {
                    if (sess) sess->ws->notifySocketConnect(sess->uri, fd, false);
                    return HTTPD_SOCK_ERR_FAIL;
                }
            }

            if (op == 0x08) // Close frame — reply and terminate immediately
            {
                uint8_t closeReply[2] = {0x88, 0x00};
                send(fd, closeReply, 2, MSG_NOSIGNAL);
                if (sess) sess->ws->notifySocketConnect(sess->uri, fd, false);
                return HTTPD_SOCK_ERR_FAIL;
            }

            // Read payload for all remaining frame types (ping may carry data)
            std::vector<uint8_t> payload((size_t)payLen);
            if (payLen > 0)
            {
                size_t received = 0;
                while (received < (size_t)payLen)
                {
                    int got = recv(fd, payload.data() + received, (size_t)payLen - received, 0);
                    if (got <= 0)
                    {
                        if (sess) sess->ws->notifySocketConnect(sess->uri, fd, false);
                        return HTTPD_SOCK_ERR_FAIL;
                    }
                    received += got;
                }
                if (masked)
                    for (size_t i = 0; i < (size_t)payLen; ++i)
                        payload[i] ^= mask[i % 4];
            }

            if (op == 0x09) // Ping → pong (RFC 6455: works with any payload length)
            {
                uint8_t pong[2] = {0x8A, 0x00};
                send(fd, pong, 2, MSG_NOSIGNAL);
                return HTTPD_SOCK_ERR_TIMEOUT;
            }

            if (op == 0x0A) // Pong → silently discard
                return HTTPD_SOCK_ERR_TIMEOUT;

            if (sess && (op == 0x01 || op == 0x02))
            {
                bool isBinary = (op == 0x02);
                sess->ws->notifySocketMessage(sess->uri, fd, payload.data(), (int)payLen, isBinary);

                // Session may have been cleaned up while handling the message.
                {
                    WsStateLock lock;
                    auto it = g_wsSessions.find(fd);
                    if (it == g_wsSessions.end())
                        return HTTPD_SOCK_ERR_FAIL;
                }
                return HTTPD_SOCK_ERR_TIMEOUT;
            }

            return HTTPD_SOCK_ERR_TIMEOUT;
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

            char resp[256];
            int rLen = snprintf(resp, sizeof(resp),
                                "HTTP/1.1 101 Switching Protocols\r\n"
                                "Upgrade: websocket\r\n"
                                "Connection: Upgrade\r\n"
                                "Sec-WebSocket-Accept: %s\r\n\r\n",
                                accept);
            send(fd, resp, rLen, MSG_NOSIGNAL);

            // TCP keepalive — detect dead connections quickly instead of waiting for SO_RCVTIMEO
            int ka = 1, kaIdle = 3, kaIntvl = 2, kaCnt = 3;
            setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &ka, sizeof(ka));
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &kaIdle, sizeof(kaIdle));
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &kaIntvl, sizeof(kaIntvl));
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &kaCnt, sizeof(kaCnt));

            // Reduce recv timeout to 1 s so the keepalive check loop is responsive
            struct timeval rcvTv = {.tv_sec = 1, .tv_usec = 0};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvTv, sizeof(rcvTv));

            WsSession* sess = new WsSession{wsObj, (httpd_handle_t)wsObj->serverHandle(), fd, uri};
            {
                WsStateLock lock;
                g_wsSessions[fd] = sess;
            }

            httpd_sess_set_recv_override((httpd_handle_t)wsObj->serverHandle(), fd, wsFrameRecv);

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

                    // Check if it's IPv4-mapped IPv6 (::ffff:x.x.x.x)
                    struct in6_addr* addr6 = &peer6.sin6_addr;
                    if (IN6_IS_ADDR_V4MAPPED(addr6))
                    {
                        // Extract IPv4 from IPv4-mapped IPv6
                        uint32_t ipv4 = *(uint32_t*)&addr6->s6_addr[12];
                        struct in_addr ipv4_addr;
                        ipv4_addr.s_addr = ipv4;
                        inet_ntop(AF_INET, &ipv4_addr, clientIp, sizeof(clientIp));
                        remoteIpAddr = ipv4;
                    }
                    else
                    {
                        // Pure IPv6
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
            std::vector<uint8_t> bodyData;
            if (req->content_len > 0 && req->content_len <= OPENKNX_WEBSERVER_MAX_BODY)
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
                request.setBody(bodyData.data(), (size_t)received);
            }

            WebResponse response;
            ws->handleRequest(request, response);

            ws->logRequest(request, response);

            // Build HTTP status string (e.g., "200 OK", "404 Not Found")
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
            else if (statusCode == 413)
                statusText = "Content Too Large";
            else if (statusCode == 500)
                statusText = "Internal Server Error";

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
            else if (response.useLayout())
            {
                std::string header = ws->buildHeader(
                    response.activeMenuUri().empty() ? uri : response.activeMenuUri());
                std::string footer = ws->buildFooter();

                int totalLen = (int)header.length() + response.bodyLength() + (int)footer.length();
                uint8_t* buffer = new uint8_t[totalLen];

                int pos = 0;
                memcpy(buffer + pos, header.c_str(), header.length());
                pos += header.length();
                memcpy(buffer + pos, response.body(), response.bodyLength());
                pos += response.bodyLength();
                memcpy(buffer + pos, footer.c_str(), footer.length());

                httpd_resp_send(req, (char*)buffer, totalLen);
                delete[] buffer;
            }
            else
            {
                httpd_resp_send(req, response.body(), response.bodyLength());
            }
            return ESP_OK;
        }

        // ── setupEsp32 — httpd start ────────────────────────────────────────

        void Webserver::setupEsp32()
        {
            // httpd runs in its own FreeRTOS task; wsFrameRecv loops per WS session
            httpd_config_t config = HTTPD_DEFAULT_CONFIG();
            config.uri_match_fn = httpd_uri_match_wildcard;
            config.ctrl_port = ESP32_WEB_CTRL_PORT;
            config.max_uri_handlers = 8; // 4 Methoden × Wildcard + Headroom
            config.max_open_sockets = 7; // page + assets (css/js/svg) + WS + headroom
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
        }

        bool Webserver::isRunning()
        {
            return _server != nullptr;
        }

        bool Webserver::sendToClient(const std::string& uri, int fd, const char* data, size_t len)
        {
            if (!_server) return false;
            bool ok = wsSendText(fd, data, len);
            if (!ok) notifySocketConnect(uri, fd, false);
            return ok;
        }

        // ── Socket dispatch ─────────────────────────────────────────────────

        void Webserver::notifySocketConnect(const std::string& uri, int fd, bool connected)
        {
            WebSocketConnectHandler onConnect = nullptr;

            if (connected)
            {
                WsStateLock lock;
                auto& clients = _socketClients[uri];
                if (std::find(clients.begin(), clients.end(), fd) == clients.end())
                    clients.push_back(fd);
            }
            else
            {
                // Remove from client list FIRST — uri may be a reference into the session
                // object that gets deleted below (use-after-free otherwise)
                {
                    WsStateLock lock;
                    auto it = _socketClients.find(uri);
                    if (it != _socketClients.end())
                    {
                        auto& v = it->second;
                        v.erase(std::remove(v.begin(), v.end(), fd), v.end());
                    }

                    auto sit = g_wsSessions.find(fd);
                    if (sit != g_wsSessions.end())
                    {
                        delete sit->second;
                        g_wsSessions.erase(sit);
                    }
                }
            }

            for (auto& sock : _sockets)
            {
                if (sock.uri == uri && sock.onConnect)
                {
                    onConnect = sock.onConnect;
                    break;
                }
            }

            if (onConnect)
                onConnect(fd, connected);
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
                if (!wsSendText(fd, message, strlen(message)))
                    notifySocketConnect(uri, fd, false);
                return;
            }
            std::vector<int> clients;
            {
                WsStateLock lock;
                auto it = _socketClients.find(uri);
                if (it == _socketClients.end() || it->second.empty()) return;
                clients = it->second;
            }

            size_t len = strlen(message);
            for (int cfd : clients)
            {
                if (!wsSendText(cfd, message, len))
                    notifySocketConnect(uri, cfd, false);
            }
        }

    } // namespace Network
} // namespace OpenKNX

#endif // OPENKNX_WEBSERVER
#endif // ARDUINO_ARCH_ESP32
