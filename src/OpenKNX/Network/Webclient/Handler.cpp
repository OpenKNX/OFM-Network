#ifdef OPENKNX_WEBCLIENT
#if defined(ARDUINO_ARCH_RP2040) || defined(ARDUINO_ARCH_ESP32)

#include "OpenKNX/Network/Webclient/Handler.h"
#include "OpenKNX/Network/DNS.h"
#include "OpenKNX.h"
#include <cctype>
#include <cstdio>
#include <cstring>
#include <algorithm>

#ifdef ARDUINO_ARCH_ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#endif

namespace OpenKNX
{
    namespace Network
    {
        namespace Webclient
        {
            // ─── Response ─────────────────────────────────────────────────────────────

            std::string Response::header(const char *name) const
            {
                for (auto &h : _headers)
                {
                    if (strcasecmp(h.first.c_str(), name) == 0)
                        return h.second;
                }
                return "";
            }

            // ─── Request builder ──────────────────────────────────────────────────────

            Request::Request(Handler *handler, const char *method, std::string url)
                : _handler(handler), _url(std::move(url))
            {
                strncpy(_method, method, sizeof(_method) - 1);
                _method[sizeof(_method) - 1] = '\0';
            }

            Request &Request::header(const char *name, const char *value)
            {
                _headers.emplace_back(name, value);
                return *this;
            }

            Request &Request::body(const char *data, size_t len)
            {
                _body.assign(data, len);
                return *this;
            }

            Request &Request::body(const std::string &text)
            {
                _body = text;
                return *this;
            }

            Request &Request::contentType(const char *ct)
            {
                return header("Content-Type", ct);
            }

            Request &Request::verifyCertificate(const char *pemCert)
            {
                _verifyMode = VerifyMode::CERTIFICATE;
                _verify = pemCert;
                return *this;
            }

            Request &Request::onData(DataCallback cb)
            {
                _onData = std::move(cb);
                return *this;
            }

            Request &Request::onDone(DoneCallback cb)
            {
                _onDone = std::move(cb);
                return *this;
            }

            Request &Request::ignoreHeaders()
            {
                _ignoreHeaders = true;
                return *this;
            }

            Request &Request::maxBodySize(size_t maxSize)
            {
                _bodyMaxSize = maxSize;
                return *this;
            }

            Request &Request::ignoreBody()
            {
                _bodyMaxSize = 0;
                return *this;
            }

            bool Request::send()
            {
                PendingRequest req;
                req.url = std::move(_url);
                strncpy(req.method, _method, sizeof(req.method));
                req.body = std::move(_body);
                req.verifyMode = _verifyMode;
                req.verify = _verify;
                req.headers = std::move(_headers);
                req.onData = std::move(_onData);
                req.onDone = std::move(_onDone);
                req.ignoreHeaders = _ignoreHeaders;
                req.bodyMaxSize = _bodyMaxSize;
                return _handler->enqueue(std::move(req));
            }

            // ─── Handler factory methods ───────────────────────────────────────────────

            Request Handler::get(const std::string &url)   { return Request(this, "GET",    url); }
            Request Handler::post(const std::string &url)  { return Request(this, "POST",   url); }
            Request Handler::put(const std::string &url)   { return Request(this, "PUT",    url); }
            Request Handler::del(const std::string &url)   { return Request(this, "DELETE", url); }
            Request Handler::patch(const std::string &url) { return Request(this, "PATCH",  url); }
            Request Handler::head(const std::string &url)  { return Request(this, "HEAD",   url); }

            // ─── URL parser ────────────────────────────────────────────────────────────

            bool Handler::parseUrl(const std::string &url, char *host, uint16_t &port, char *path, bool &ssl)
            {
                const char *p = url.c_str();

                if (strncasecmp(p, "https://", 8) == 0)
                {
                    ssl = true;
                    port = 443;
                    p += 8;
                }
                else if (strncasecmp(p, "http://", 7) == 0)
                {
                    ssl = false;
                    port = 80;
                    p += 7;
                }
                else
                {
                    return false;
                }

                const char *hostStart = p;
                const char *colon = nullptr;

                while (*p && *p != '/' && *p != ':') p++;
                if (*p == ':') { colon = p; while (*p && *p != '/') p++; }
                const char *slash = (*p == '/') ? p : nullptr;

                size_t hostLen = (colon ? colon : (slash ? slash : p)) - hostStart;
                if (hostLen == 0 || hostLen >= 64) return false;
                memcpy(host, hostStart, hostLen);
                host[hostLen] = '\0';

                if (colon)
                {
                    port = (uint16_t)atoi(colon + 1);
                    if (port == 0) return false;
                }

                strncpy(path, slash ? slash : "/", 255);
                path[255] = '\0';

                return true;
            }

            // ─── Enqueue ───────────────────────────────────────────────────────────────

            bool Handler::enqueue(PendingRequest &&req)
            {
#ifdef ARDUINO_ARCH_ESP32
                if (_requestQueue == nullptr) initTask();

                TaskRequest *tr = new TaskRequest();
                tr->requestId = _nextRequestId++;
                tr->timeoutMs = OPENKNX_WEBCLIENT_TIMEOUT;
                strncpy(tr->method, req.method, sizeof(tr->method) - 1);
                tr->method[sizeof(tr->method) - 1] = '\0';
                tr->body = std::move(req.body);
                tr->verifyMode = req.verifyMode;
                tr->verify = req.verify;
                tr->headers = std::move(req.headers);
                tr->ignoreHeaders = req.ignoreHeaders;
                tr->hasDataCallback = (bool)req.onData;
                tr->bodyMaxSize = req.bodyMaxSize;

                if (!parseUrl(req.url, tr->host, tr->port, tr->path, tr->ssl))
                {
                    logError("Webclient", "Invalid URL: %s", req.url.c_str());
                    delete tr;
                    if (req.onDone)
                    {
                        Response res;
                        req.onDone(res);
                    }
                    return false;
                }

                ActiveCallback ac;
                ac.onData = std::move(req.onData);
                ac.onDone = std::move(req.onDone);
                _activeCallbacks.push_back({tr->requestId, std::move(ac)});

                if (xQueueSend(_requestQueue, &tr, 0) != pdTRUE)
                {
                    logError("Webclient", "Request queue full");
                    ActiveCallback cb = std::move(_activeCallbacks.back().second);
                    _activeCallbacks.pop_back();
                    if (cb.onDone)
                    {
                        Response res;
                        cb.onDone(res);
                    }
                    delete tr;
                    return false;
                }
                return true;
#else
                for (int i = 0; i < OPENKNX_WEBCLIENT_SLOTS; i++)
                {
                    if (_slots[i].state == Slot::State::IDLE)
                    {
                        startSlot(i, std::move(req));
                        return true;
                    }
                }
                _pending.push_back(std::move(req));
                return true;
#endif
            }

#ifdef ARDUINO_ARCH_RP2040

            void Handler::loop()
            {
                for (int i = 0; i < OPENKNX_WEBCLIENT_SLOTS; i++)
                    processSlot(i);
            }

            void Handler::startSlot(int idx, PendingRequest &&req)
            {
                Slot &s = _slots[idx];
                s = Slot{};

                s.txBuf = (uint8_t*)PSRAM_CALLOC(1, OPENKNX_WEBCLIENT_BUF_SIZE);
                s.rxBuf = (uint8_t*)PSRAM_CALLOC(1, OPENKNX_WEBCLIENT_BUF_SIZE);
                if (!s.txBuf || !s.rxBuf)
                {
                    logError("Webclient", "Out of memory for request buffers");
                    free(s.txBuf);
                    free(s.rxBuf);
                    s.txBuf = nullptr;
                    s.rxBuf = nullptr;
                    if (req.onDone)
                    {
                        Response res;
                        req.onDone(res);
                    }
                    startNextPending();
                    return;
                }

                if (!parseUrl(req.url, s.host, s.port, s.path, s.ssl))
                {
                    logError("Webclient", "Invalid URL: %s", req.url.c_str());
                    free(s.txBuf);
                    free(s.rxBuf);
                    s.txBuf = nullptr;
                    s.rxBuf = nullptr;
                    if (req.onDone)
                    {
                        Response res;
                        req.onDone(res);
                    }
                    startNextPending();
                    return;
                }

                s.requestId = _nextRequestId++;
                s.startMs = millis();
                s.verifyMode = req.verifyMode;
                s.verify = req.verify;
                strncpy(s.method, req.method, sizeof(s.method) - 1);
                s.requestBody = std::move(req.body);
                s.extraHeaders = std::move(req.headers);
                s.onData = std::move(req.onData);
                s.onDone = std::move(req.onDone);
                s.ignoreHeaders = req.ignoreHeaders;
                s.bodyMaxSize = req.bodyMaxSize;
                s.responseBody.clear();
                s.state = Slot::State::RESOLVING;

                uint32_t capturedId = s.requestId;
                int capturedIdx = idx;

                // Direct IP — skip DNS
                IPAddress directIP;
                if (directIP.fromString(s.host))
                {
                    s.state = Slot::State::CONNECTING;
                    if (!platformConnect(s, directIP, s.port))
                    {
                        finishSlot(idx, false);
                        return;
                    }
                    return;
                }

                DNS::query(s.host, [this, capturedId, capturedIdx](IPAddress ip) {
                    Slot &slot = _slots[capturedIdx];
                    if (slot.requestId != capturedId || slot.state != Slot::State::RESOLVING)
                        return;

                    if (ip == IPAddress(0, 0, 0, 0))
                    {
                        finishSlot(capturedIdx, false);
                        return;
                    }

                    slot.state = Slot::State::CONNECTING;
                    if (!platformConnect(slot, ip, slot.port))
                    {
                        finishSlot(capturedIdx, false);
                        return;
                    }
                });
            }

            void Handler::processSlot(int idx)
            {
                Slot &s = _slots[idx];

                if (s.state == Slot::State::IDLE || s.state == Slot::State::RESOLVING)
                    return;

                if ((uint32_t)(millis() - s.startMs) > OPENKNX_WEBCLIENT_TIMEOUT)
                {
                    logError("Webclient", "Timeout slot %d", idx);
                    if (s.state == Slot::State::BODY && s.bodyMaxSize > 0) s.bodyIncomplete = true;
                    finishSlot(idx, s.state == Slot::State::BODY);
                    return;
                }

                switch (s.state)
                {
                    case Slot::State::CONNECTING:
                        if (s.tcpError) { finishSlot(idx, false); return; }
                        if (!platformConnectComplete(s)) return;
                        if (!platformConnected(s)) { finishSlot(idx, false); return; }
#ifdef ARDUINO_ARCH_RP2040
                        if (s.ssl)
                        {
                            if (!platformSslInit(s)) { finishSlot(idx, false); return; }
                            s.state = Slot::State::SSL_HANDSHAKE;
                            return;
                        }
#endif
                        formatRequestHeaders(s);
                        s.state = Slot::State::SENDING_HDR;
                        break;

#ifdef ARDUINO_ARCH_RP2040
                    case Slot::State::SSL_HANDSHAKE:
                    {
                        if (millis() - s.startMs > OPENKNX_WEBCLIENT_TIMEOUT)
                        {
                            logError("Webclient", "SSL handshake timeout");
                            finishSlot(idx, false);
                            return;
                        }
                        int r = platformSslStep(s);
                        if (r < 0) { finishSlot(idx, false); return; }
                        if (r == 0) return;
                        formatRequestHeaders(s);
                        s.state = Slot::State::SENDING_HDR;
                        break;
                    }
#endif

                    case Slot::State::SENDING_HDR:
                    {
                        while (s.txSent < s.txLen)
                        {
                            int sent = platformSend(s, s.txBuf + s.txSent, s.txLen - s.txSent);
                            if (sent < 0) { finishSlot(idx, false); return; }
                            if (sent == 0) return;
                            s.txSent += (uint16_t)sent;
                        }
                        s.state = s.requestBody.empty() ? Slot::State::HEADERS : Slot::State::SENDING_BODY;
                        s.bodySent = 0;
                        break;
                    }

                    case Slot::State::SENDING_BODY:
                    {
                        while (s.bodySent < s.requestBody.size())
                        {
                            const uint8_t *ptr = (const uint8_t *)s.requestBody.data() + s.bodySent;
                            size_t remaining = s.requestBody.size() - s.bodySent;
                            int sent = platformSend(s, ptr, remaining);
                            if (sent < 0) { finishSlot(idx, false); return; }
                            if (sent == 0) return;
                            s.bodySent += (uint32_t)sent;
                        }
                        s.state = Slot::State::HEADERS;
                        break;
                    }

                    case Slot::State::HEADERS:
                    {
                        int n = platformRead(s, s.rxBuf, OPENKNX_WEBCLIENT_BUF_SIZE);
                        if (n < 0) { finishSlot(idx, false); return; }
                        int i = 0;
                        for (; i < n && !s.headersComplete; i++)
                        {
                            if (!processHeaderByte(s, s.rxBuf[i]))
                            {
                                finishSlot(idx, false);
                                return;
                            }
                        }
                        if (s.headersComplete)
                        {
                            if (strcmp(s.method, "HEAD") == 0 || s.httpStatus == 204 || s.httpStatus == 304)
                            {
                                finishSlot(idx, true);
                                return;
                            }
                            if (s.bodyMaxSize > 0 && s.contentLength > 0)
                                s.responseBody.reserve((size_t)std::min((uint32_t)s.contentLength, (uint32_t)s.bodyMaxSize));
                            s.state = Slot::State::BODY;
                            if (i < n && !processBodyBytes(s, s.rxBuf + i, n - i))
                                return;
                            if (s.contentLength >= 0 && (int32_t)s.bodyReceived >= s.contentLength)
                                { finishSlot(idx, true); return; }
                            if (s.isChunked && s.chunkDone)
                                { finishSlot(idx, true); return; }
                        }
                        else if (n == 0 && !platformConnected(s))
                        {
                            finishSlot(idx, false);
                        }
                        break;
                    }

                    case Slot::State::BODY:
                    {
                        int n = platformRead(s, s.rxBuf, OPENKNX_WEBCLIENT_BUF_SIZE);
                        if (n < 0) { finishSlot(idx, false); return; }
                        if (n > 0)
                        {
                            if (!processBodyBytes(s, s.rxBuf, (size_t)n))
                                return;
                        }

                        if (s.contentLength >= 0 && (int32_t)s.bodyReceived >= s.contentLength)
                        {
                            finishSlot(idx, true);
                        }
                        else if (s.isChunked && s.chunkDone)
                        {
                            finishSlot(idx, true);
                        }
                        else if (!platformConnected(s))
                        {
                            bool ok = (s.contentLength < 0 && !s.isChunked) ||
                                      (s.contentLength >= 0 && (int32_t)s.bodyReceived >= s.contentLength) ||
                                      (s.isChunked && s.chunkDone);
                            if (!ok && s.bodyMaxSize > 0) s.bodyIncomplete = true;
                            finishSlot(idx, true);
                        }
                        break;
                    }

                    default:
                        break;
                }
            }

            void Handler::finishSlot(int idx, bool success)
            {
                Slot &s = _slots[idx];
                platformClose(s);

                free(s.txBuf);
                free(s.rxBuf);
                s.txBuf = nullptr;
                s.rxBuf = nullptr;

                DoneCallback doneCb = std::move(s.onDone);

                Response res;
                res._success = success && s.httpStatus >= 200 && s.httpStatus < 300;
                res._status  = s.httpStatus;
                res._bodySize = s.contentLength >= 0 ? (uint32_t)s.contentLength : s.bodyReceived;
                res._bodyIncomplete = s.bodyIncomplete;
                if (s.bodyMaxSize > 0) res._body = std::move(s.responseBody);
                if (!s.ignoreHeaders) res._headers = std::move(s.responseHeaders);

                s.onData = nullptr;
                s.onDone = nullptr;
                s.state = Slot::State::IDLE;
                s.bodyIncomplete = false;
                s.requestBody.clear();
                s.extraHeaders.clear();
                s.responseHeaders.clear();
                s.responseBody.clear();

                if (doneCb) doneCb(res);

                startNextPending();
            }

            void Handler::startNextPending()
            {
                if (_pending.empty()) return;
                for (int i = 0; i < OPENKNX_WEBCLIENT_SLOTS; i++)
                {
                    if (_slots[i].state == Slot::State::IDLE)
                    {
                        startSlot(i, std::move(_pending.front()));
                        _pending.pop_front();
                        return;
                    }
                }
            }

            void Handler::formatRequestHeaders(Slot &s)
            {
                char buf[512];
                int len = snprintf(buf, sizeof(buf),
                    "%s %s HTTP/1.1\r\n"
                    "Host: %s\r\n"
                    "Connection: close\r\n",
                    s.method, s.path, s.host);

                for (auto &hdr : s.extraHeaders)
                {
                    int n = snprintf(buf + len, sizeof(buf) - len, "%s: %s\r\n",
                        hdr.first.c_str(), hdr.second.c_str());
                    if (n > 0 && len + n < (int)sizeof(buf)) len += n;
                }

                if (!s.requestBody.empty())
                {
                    int n = snprintf(buf + len, sizeof(buf) - len, "Content-Length: %zu\r\n",
                        s.requestBody.size());
                    if (n > 0 && len + n < (int)sizeof(buf)) len += n;
                }

                if (len + 2 < (int)sizeof(buf))
                {
                    buf[len++] = '\r';
                    buf[len++] = '\n';
                }

                if (len > OPENKNX_WEBCLIENT_BUF_SIZE) len = OPENKNX_WEBCLIENT_BUF_SIZE;
                memcpy(s.txBuf, buf, len);
                s.txLen = (uint16_t)len;
                s.txSent = 0;
            }

            bool Handler::processHeaderByte(Slot &s, uint8_t b)
            {
                if (s.hdrExpectLF)
                {
                    s.hdrExpectLF = false;
                    if (b != '\n') return false;

                    if (s.hdrLineLen == 0)
                    {
                        s.headersComplete = true;
                        return true;
                    }
                    s.hdrLine[s.hdrLineLen] = '\0';
                    parseHeaderLine(s);
                    s.hdrLineLen = 0;
                    return true;
                }

                if (b == '\r')
                {
                    s.hdrExpectLF = true;
                    return true;
                }

                if (s.hdrLineLen < sizeof(s.hdrLine) - 1)
                    s.hdrLine[s.hdrLineLen++] = (char)b;

                return true;
            }

            void Handler::parseHeaderLine(Slot &s)
            {
                const char *line = s.hdrLine;

                if (strncmp(line, "HTTP/", 5) == 0)
                {
                    const char *sp = strchr(line, ' ');
                    if (sp) s.httpStatus = (uint16_t)atoi(sp + 1);
                    return;
                }

                const char *colon = strchr(line, ':');
                if (!colon) return;

                size_t nameLen = colon - line;
                const char *val = colon + 1;
                while (*val == ' ') val++;

                if (nameLen == 14 && strncasecmp(line, "content-length", 14) == 0)
                    s.contentLength = atol(val);
                else if (nameLen == 17 && strncasecmp(line, "transfer-encoding", 17) == 0)
                {
                    for (const char *p = val; *p; p++)
                        if (strncasecmp(p, "chunked", 7) == 0) { s.isChunked = true; break; }
                }

                if (!s.ignoreHeaders)
                {
                    std::string name(line, nameLen);
                    s.responseHeaders.emplace_back(std::move(name), val);
                }
            }

            bool Handler::processBodyBytes(Slot &s, const uint8_t *data, size_t len)
            {
                if (!s.isChunked)
                {
                    s.bodyReceived += len;
                    if (s.bodyMaxSize > 0)
                    {
                        if (s.responseBody.size() + len > s.bodyMaxSize)
                        {
                            s.bodyIncomplete = true;
                            finishSlot((int)(&s - _slots), true);
                            return false;
                        }
                        s.responseBody.append(reinterpret_cast<const char *>(data), len);
                    }
                    if (s.onData) s.onData(data, len);
                    return true;
                }

                size_t i = 0;
                while (i < len && !s.chunkDone)
                {
                    uint8_t b = data[i++];
                    switch (s.chunkState)
                    {
                        case Slot::ChunkState::SIZE:
                            if (b == '\r') s.chunkState = Slot::ChunkState::SIZE_LF;
                            else if (s.chunkSizeLen < sizeof(s.chunkSizeBuf) - 1)
                                s.chunkSizeBuf[s.chunkSizeLen++] = (char)b;
                            break;

                        case Slot::ChunkState::SIZE_LF:
                            if (b == '\n')
                            {
                                s.chunkSizeBuf[s.chunkSizeLen] = '\0';
                                s.chunkRemaining = strtoul(s.chunkSizeBuf, nullptr, 16);
                                s.chunkSizeLen = 0;
                                memset(s.chunkSizeBuf, 0, sizeof(s.chunkSizeBuf));
                                s.chunkState = (s.chunkRemaining == 0) ? Slot::ChunkState::TRAILER : Slot::ChunkState::DATA;
                            }
                            break;

                        case Slot::ChunkState::DATA:
                        {
                            size_t avail = len - i + 1;
                            size_t deliver = (avail <= s.chunkRemaining) ? avail : s.chunkRemaining;
                            const uint8_t *startPtr = data + i - 1;

                            s.bodyReceived += deliver;
                            s.chunkRemaining -= deliver;

                            if (s.bodyMaxSize > 0)
                            {
                                if (s.responseBody.size() + deliver > s.bodyMaxSize)
                                {
                                    s.bodyIncomplete = true;
                                    finishSlot((int)(&s - _slots), true);
                                    return false;
                                }
                                s.responseBody.append(reinterpret_cast<const char *>(startPtr), deliver);
                            }

                            if (s.onData) s.onData(startPtr, deliver);

                            i += (deliver - 1);
                            if (s.chunkRemaining == 0) s.chunkState = Slot::ChunkState::DATA_CR;
                            break;
                        }

                        case Slot::ChunkState::DATA_CR:
                            if (b == '\r') s.chunkState = Slot::ChunkState::DATA_LF;
                            break;

                        case Slot::ChunkState::DATA_LF:
                            if (b == '\n') s.chunkState = Slot::ChunkState::SIZE;
                            break;

                        case Slot::ChunkState::TRAILER:
                            if (b == '\n') s.chunkDone = true;
                            break;
                    }
                }

                return true;
            }

#endif // ARDUINO_ARCH_RP2040

        } // namespace Webclient
    }     // namespace Network
} // namespace OpenKNX

#endif // ARDUINO_ARCH_RP2040 || ARDUINO_ARCH_ESP32
#endif // OPENKNX_WEBCLIENT
