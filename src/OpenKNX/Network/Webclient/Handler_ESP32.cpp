#ifdef OPENKNX_WEBCLIENT
#ifdef ARDUINO_ARCH_ESP32

#include "OpenKNX/Network/Webclient/Handler.h"
#include "OpenKNX.h"

#include <WiFiClient.h>
#include <WiFiClientSecure.h>

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
                        auto *sc = new WiFiClientSecure();
                        if (req->insecure)
                            sc->setInsecure();
                        else if (req->caCert)
                            sc->setCACert(req->caCert);
                        client = sc;
                    }
                    else
                    {
                        client = new WiFiClient();
                    }

                    client->setTimeout(req->timeoutMs / 1000);

                    auto sendDone = [&](bool ok, uint16_t status) {
                        auto *entry = new ResultEntry();
                        entry->type = ResultEntry::Type::DONE;
                        entry->requestId = req->requestId;
                        entry->dataLen = 0;
                        entry->success = ok;
                        entry->httpStatus = status;
                        xQueueSend(self->_resultQueue, &entry, portMAX_DELAY);
                    };

                    // connect() by hostname — sets SNI for TLS
                    if (!client->connect(req->host, req->port))
                    {
                        delete client;
                        delete req;
                        sendDone(false, 0);
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
                            delete client;
                            delete req;
                            sendDone(false, 0);
                            continue;
                        }
                    }

                    // HEAD / 204 / 304: no body
                    if (strcmp(req->method, "HEAD") == 0 || httpStatus == 204 || httpStatus == 304)
                    {
                        client->stop();
                        delete client;
                        delete req;
                        sendDone(true, httpStatus);
                        continue;
                    }

                    // ── Stream body ───────────────────────────────────────────────────
                    uint8_t bodyBuf[512];
                    uint32_t bodyReceived = 0;
                    bool bodyOk = true;
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
                            auto *entry = new ResultEntry();
                            entry->type = ResultEntry::Type::DATA;
                            entry->requestId = req->requestId;
                            entry->dataLen = (size_t)n;
                            memcpy(entry->data, bodyBuf, n);
                            entry->success = false;
                            entry->httpStatus = httpStatus;
                            xQueueSend(self->_resultQueue, &entry, portMAX_DELAY);

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

                                        auto *entry = new ResultEntry();
                                        entry->type = ResultEntry::Type::DATA;
                                        entry->requestId = req->requestId;
                                        entry->dataLen = deliver;
                                        memcpy(entry->data, bodyBuf + i, deliver);
                                        entry->success = false;
                                        entry->httpStatus = httpStatus;
                                        xQueueSend(self->_resultQueue, &entry, portMAX_DELAY);

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
                        // plain: done when connection closed or content-length reached
                        bodyOk = (contentLength < 0) ||
                                 (contentLength >= 0 && (int32_t)bodyReceived >= contentLength);
                    }
                    else if (isChunked && !chunkDone)
                    {
                        bodyOk = false;
                    }

                    if (millis() >= deadline) bodyOk = false;

                    client->stop();
                    delete client;
                    delete req;
                    sendDone(bodyOk, httpStatus);
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
                                it->second.onDone(entry->success, entry->httpStatus);
                            _activeCallbacks.erase(it);
                        }
                        break;
                    }

                    delete entry;
                }
            }

            // ─── enqueueResult (called from task) ────────────────────────────────────

            void Handler::enqueueResult(ResultEntry &&entryVal)
            {
                auto *entry = new ResultEntry(std::move(entryVal));
                xQueueSend(_resultQueue, &entry, portMAX_DELAY);
            }

        } // namespace Webclient
    }     // namespace Network
} // namespace OpenKNX

#endif // ARDUINO_ARCH_ESP32
#endif // OPENKNX_WEBCLIENT
