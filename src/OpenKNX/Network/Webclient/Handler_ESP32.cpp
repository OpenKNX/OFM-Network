#ifdef OPENKNX_WEBCLIENT
#ifdef ARDUINO_ARCH_ESP32

#include "OpenKNX/Network/Webclient/Handler.h"
#include "OpenKNX.h"
#include <algorithm>

#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#ifdef CONFIG_SPIRAM
#include <esp_heap_caps.h>
template<typename T> static T* wcNew()      { void *m = heap_caps_malloc(sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); return m ? new(m) T() : new T(); }
template<typename T> static void wcDelete(T *p) { if (p) { p->~T(); free(p); } }
#else
template<typename T> static T* wcNew()          { return new T(); }
template<typename T> static void wcDelete(T *p) { delete p; }
#endif

namespace OpenKNX
{
    namespace Network
    {
        namespace Webclient
        {
            // ─── No-ops: platform API not used on ESP32 (task handles everything) ─────

            bool Handler::platformConnect(Slot & /*s*/, IPAddress /*ip*/, uint16_t /*port*/) { return false; }
            bool Handler::platformConnectComplete(Slot & /*s*/) { return false; }
            bool Handler::platformConnected(Slot & /*s*/) { return false; }
            int  Handler::platformSend(Slot & /*s*/, const uint8_t * /*d*/, size_t /*l*/) { return -1; }
            int  Handler::platformRead(Slot & /*s*/, uint8_t * /*b*/, size_t /*l*/) { return -1; }
            void Handler::platformClose(Slot & /*s*/) {}

            // ─── Task init ────────────────────────────────────────────────────────────

            void Handler::initTask()
            {
                _requestQueue = xQueueCreate(OPENKNX_WEBCLIENT_SLOTS * 4, sizeof(TaskRequest *));
                _resultQueue  = xQueueCreate(OPENKNX_WEBCLIENT_SLOTS * 16, sizeof(ResultEntry *));

                xTaskCreatePinnedToCore(
                    Handler::taskRun,
                    "wclient",
                    OPENKNX_WEBCLIENT_TASK_STACK,
                    this,
                    1,      // priority 1 — EMAC tasks (prio 12-15) preempt us freely
                    &_task,
                    0       // Core 0: Arduino loop is on Core 1, no CPU competition
                );
            }

            // ─── Worker task ──────────────────────────────────────────────────────────

            void Handler::taskRun(void *arg)
            {
                Handler *self = static_cast<Handler *>(arg);

                for (;;)
                {
                    TaskRequest *req = nullptr;
                    xQueueReceive(self->_requestQueue, &req, portMAX_DELAY);
                    if (!req) continue;

                    Client *client = nullptr;
                    if (req->ssl)
                    {
                        auto *sc = wcNew<WiFiClientSecure>();
                        if (req->verifyMode == VerifyMode::CERTIFICATE && req->verify)
                            sc->setCACert(req->verify);
                        else
                            sc->setInsecure();
                        client = sc;
                    }
                    else
                    {
                        client = wcNew<WiFiClient>();
                    }

                    client->setTimeout(req->timeoutMs / 1000);

                    auto sendDone = [&](Response res) {
                        auto *entry = wcNew<ResultEntry>();
                        entry->type = ResultEntry::Type::DONE;
                        entry->requestId = req->requestId;
                        entry->dataLen = 0;
                        entry->response = std::move(res);
                        xQueueSend(self->_resultQueue, &entry, portMAX_DELAY);
                    };

                    // connect() by hostname — sets SNI for TLS
                    if (!client->connect(req->host, req->port))
                    {
                        wcDelete(client);
                        delete req;
                        sendDone(Response{});
                        continue;
                    }

                    // ── Send request headers ──────────────────────────────────────────
                    {
                        char hdrBuf[768];
                        int hdrLen = snprintf(hdrBuf, sizeof(hdrBuf),
                            "%s %s HTTP/1.1\r\n"
                            "Host: %s\r\n"
                            "Connection: close\r\n",
                            req->method, req->path, req->host);

                        for (auto &h : req->headers)
                        {
                            int n = snprintf(hdrBuf + hdrLen, sizeof(hdrBuf) - hdrLen,
                                "%s: %s\r\n", h.first.c_str(), h.second.c_str());
                            if (n > 0 && hdrLen + n < (int)sizeof(hdrBuf)) hdrLen += n;
                        }

                        if (!req->body.empty())
                        {
                            int n = snprintf(hdrBuf + hdrLen, sizeof(hdrBuf) - hdrLen,
                                "Content-Length: %zu\r\n", req->body.size());
                            if (n > 0 && hdrLen + n < (int)sizeof(hdrBuf)) hdrLen += n;
                        }

                        if (hdrLen + 2 < (int)sizeof(hdrBuf))
                        {
                            hdrBuf[hdrLen++] = '\r';
                            hdrBuf[hdrLen++] = '\n';
                        }

                        client->write((const uint8_t *)hdrBuf, hdrLen);
                    }

                    // ── Send body ─────────────────────────────────────────────────────
                    if (!req->body.empty())
                        client->write((const uint8_t *)req->body.data(), req->body.size());

                    // ── Read response headers ─────────────────────────────────────────
                    uint16_t httpStatus = 0;
                    int32_t contentLength = -1;
                    bool isChunked = false;
                    std::vector<std::pair<std::string, std::string>> responseHeaders;
                    {
                        char lineBuf[128];
                        int lineLen = 0;
                        bool expectLF = false;
                        bool headersDone = false;
                        uint32_t deadline = millis() + req->timeoutMs;

                        while (!headersDone && millis() < deadline)
                        {
                            if (!client->available())
                            {
                                if (!client->connected()) break;
                                vTaskDelay(1);
                                continue;
                            }

                            uint8_t b = (uint8_t)client->read();

                            if (expectLF)
                            {
                                expectLF = false;
                                if (b != '\n') break;

                                if (lineLen == 0)
                                {
                                    headersDone = true;
                                    break;
                                }

                                lineBuf[lineLen] = '\0';

                                if (strncmp(lineBuf, "HTTP/", 5) == 0)
                                {
                                    const char *sp = strchr(lineBuf, ' ');
                                    if (sp) httpStatus = (uint16_t)atoi(sp + 1);
                                }
                                else
                                {
                                    const char *colon = strchr(lineBuf, ':');
                                    if (colon)
                                    {
                                        size_t nameLen = colon - lineBuf;
                                        const char *val = colon + 1;
                                        while (*val == ' ') val++;
                                        if (nameLen == 14 && strncasecmp(lineBuf, "content-length", 14) == 0)
                                            contentLength = atol(val);
                                        else if (nameLen == 17 && strncasecmp(lineBuf, "transfer-encoding", 17) == 0)
                                            if (strcasestr(val, "chunked")) isChunked = true;

                                        if (!req->ignoreHeaders)
                                            responseHeaders.emplace_back(std::string(lineBuf, nameLen), val);
                                    }
                                }
                                lineLen = 0;
                            }
                            else if (b == '\r')
                            {
                                expectLF = true;
                            }
                            else if (lineLen < (int)sizeof(lineBuf) - 1)
                            {
                                lineBuf[lineLen++] = (char)b;
                            }
                        }

                        if (!headersDone)
                        {
                            client->stop();
                            wcDelete(client);
                            delete req;
                            sendDone(Response{});
                            continue;
                        }
                    }

                    // HEAD / 204 / 304: done after headers, no body
                    if (strcmp(req->method, "HEAD") == 0 || httpStatus == 204 || httpStatus == 304)
                    {
                        client->stop();
                        wcDelete(client);
                        Response res;
                        res._success = httpStatus >= 200 && httpStatus < 300;
                        res._status = httpStatus;
                        res._bodySize = contentLength >= 0 ? (uint32_t)contentLength : 0;
                        if (!req->ignoreHeaders) res._headers = std::move(responseHeaders);
                        delete req;
                        sendDone(std::move(res));
                        continue;
                    }

                    // ── Stream body ───────────────────────────────────────────────────
                    uint8_t bodyBuf[512];
                    uint32_t bodyReceived = 0;
                    bool bodyOk = true;
                    std::string bodyAccum;
                    if (req->bodyMaxSize > 0 && contentLength > 0)
                        bodyAccum.reserve((size_t)std::min((uint32_t)contentLength, req->bodyMaxSize));
                    uint32_t deadline = millis() + req->timeoutMs;

                    bool chunkDone = false;
                    uint32_t chunkRemaining = 0;
                    char chunkSizeBuf[9] = {};
                    uint8_t chunkSizeLen = 0;
                    enum ChunkPhase : uint8_t { CS_SIZE, CS_SIZE_LF, CS_DATA, CS_DATA_CR, CS_DATA_LF, CS_TRAILER } chunkPhase = CS_SIZE;

                    while (!chunkDone && millis() < deadline)
                    {
                        if (!client->available())
                        {
                            if (!client->connected()) break;
                            vTaskDelay(1);
                            continue;
                        }

                        int n = client->read(bodyBuf, sizeof(bodyBuf));
                        if (n <= 0) continue;

                        if (!isChunked)
                        {
                            bodyReceived += n;

                            if (req->bodyMaxSize > 0)
                            {
                                if (bodyAccum.size() + (size_t)n > req->bodyMaxSize)
                                {
                                    bodyOk = false;
                                    break;
                                }
                                bodyAccum.append(reinterpret_cast<const char *>(bodyBuf), n);
                            }

                            if (req->hasDataCallback)
                            {
                                auto *entry = wcNew<ResultEntry>();
                                entry->type = ResultEntry::Type::DATA;
                                entry->requestId = req->requestId;
                                entry->dataLen = (size_t)n;
                                memcpy(entry->data, bodyBuf, n);
                                xQueueSend(self->_resultQueue, &entry, portMAX_DELAY);
                            }

                            if (contentLength >= 0 && (int32_t)bodyReceived >= contentLength)
                                break;
                        }
                        else
                        {
                            for (int i = 0; i < n && !chunkDone; i++)
                            {
                                uint8_t b = bodyBuf[i];
                                switch (chunkPhase)
                                {
                                    case CS_SIZE:
                                        if (b == '\r') chunkPhase = CS_SIZE_LF;
                                        else if (chunkSizeLen < sizeof(chunkSizeBuf) - 1)
                                            chunkSizeBuf[chunkSizeLen++] = (char)b;
                                        break;

                                    case CS_SIZE_LF:
                                        if (b == '\n')
                                        {
                                            chunkSizeBuf[chunkSizeLen] = '\0';
                                            chunkRemaining = strtoul(chunkSizeBuf, nullptr, 16);
                                            chunkSizeLen = 0;
                                            memset(chunkSizeBuf, 0, sizeof(chunkSizeBuf));
                                            chunkPhase = (chunkRemaining == 0) ? CS_TRAILER : CS_DATA;
                                        }
                                        break;

                                    case CS_DATA:
                                    {
                                        size_t avail = (size_t)(n - i);
                                        size_t deliver = (avail <= chunkRemaining) ? avail : chunkRemaining;
                                        bodyReceived += deliver;
                                        chunkRemaining -= deliver;

                                        if (req->bodyMaxSize > 0)
                                        {
                                            if (bodyAccum.size() + deliver > req->bodyMaxSize)
                                            {
                                                bodyOk = false;
                                                chunkDone = true;
                                                break;
                                            }
                                            bodyAccum.append(reinterpret_cast<const char *>(bodyBuf + i), deliver);
                                        }

                                        if (req->hasDataCallback)
                                        {
                                            auto *entry = wcNew<ResultEntry>();
                                            entry->type = ResultEntry::Type::DATA;
                                            entry->requestId = req->requestId;
                                            entry->dataLen = deliver;
                                            memcpy(entry->data, bodyBuf + i, deliver);
                                            xQueueSend(self->_resultQueue, &entry, portMAX_DELAY);
                                        }

                                        i += (int)(deliver - 1);
                                        if (chunkRemaining == 0) chunkPhase = CS_DATA_CR;
                                        break;
                                    }

                                    case CS_DATA_CR:
                                        if (b == '\r') chunkPhase = CS_DATA_LF;
                                        break;

                                    case CS_DATA_LF:
                                        if (b == '\n') chunkPhase = CS_SIZE;
                                        break;

                                    case CS_TRAILER:
                                        if (b == '\n') chunkDone = true;
                                        break;
                                }
                            }
                        }
                    }

                    if (!isChunked && !chunkDone)
                    {
                        bodyOk = bodyOk && ((contentLength < 0) ||
                                            (contentLength >= 0 && (int32_t)bodyReceived >= contentLength));
                    }
                    else if (isChunked && !chunkDone)
                    {
                        bodyOk = false;
                    }

                    if (millis() >= deadline) bodyOk = false;

                    client->stop();
                    wcDelete(client);

                    Response res;
                    res._bodyIncomplete = req->bodyMaxSize > 0 && !bodyOk;
                    res._success = httpStatus >= 200 && httpStatus < 300;
                    res._status = httpStatus;
                    res._bodySize = contentLength >= 0 ? (uint32_t)contentLength : bodyReceived;
                    if (req->bodyMaxSize > 0) res._body = std::move(bodyAccum);
                    if (!req->ignoreHeaders) res._headers = std::move(responseHeaders);
                    delete req;
                    sendDone(std::move(res));
                }
            }

            // ─── Main-thread loop: drain result queue → dispatch callbacks ────────────

            void Handler::loop()
            {
                if (_requestQueue == nullptr) return;

                ResultEntry *entry = nullptr;
                while (xQueueReceive(_resultQueue, &entry, 0) == pdTRUE)
                {
                    if (!entry) continue;

                    for (auto it = _activeCallbacks.begin(); it != _activeCallbacks.end(); ++it)
                    {
                        if (it->first != entry->requestId) continue;

                        if (entry->type == ResultEntry::Type::DATA)
                        {
                            if (it->second.onData)
                                it->second.onData(entry->data, entry->dataLen);
                        }
                        else
                        {
                            if (it->second.onDone)
                                it->second.onDone(entry->response);
                            _activeCallbacks.erase(it);
                        }
                        break;
                    }

                    wcDelete(entry);
                }
            }

            // ─── enqueueResult (called from task) ────────────────────────────────────

            void Handler::enqueueResult(ResultEntry &&entryVal)
            {
                auto *entry = wcNew<ResultEntry>();
                *entry = std::move(entryVal);
                xQueueSend(_resultQueue, &entry, portMAX_DELAY);
            }

        } // namespace Webclient
    }     // namespace Network
} // namespace OpenKNX

#endif // ARDUINO_ARCH_ESP32
#endif // OPENKNX_WEBCLIENT
