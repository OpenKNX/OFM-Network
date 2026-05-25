#pragma once

#ifdef OPENKNX_WEBCLIENT

#if defined(ARDUINO_ARCH_RP2040) || defined(ARDUINO_ARCH_ESP32)

#include <IPAddress.h>
#include <Client.h>
#ifdef ARDUINO_ARCH_RP2040
#include <Arduino.h>  // ensures 'using namespace arduino' so Client resolves correctly
#include <lwip/tcp.h>
#endif
#include <functional>
#include <string>
#include <vector>
#include <deque>

#ifndef OPENKNX_WEBCLIENT_SLOTS
#define OPENKNX_WEBCLIENT_SLOTS 2
#endif
#ifndef OPENKNX_WEBCLIENT_TIMEOUT
#define OPENKNX_WEBCLIENT_TIMEOUT 10000
#endif
#ifndef OPENKNX_WEBCLIENT_MAX_BODY
#define OPENKNX_WEBCLIENT_MAX_BODY 4096
#endif
#ifndef OPENKNX_WEBCLIENT_TASK_STACK
#define OPENKNX_WEBCLIENT_TASK_STACK 6144
#endif

#ifdef ARDUINO_ARCH_ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#endif

namespace OpenKNX
{
    namespace Network
    {
        namespace Webclient
        {
            struct Response
            {
                bool _success = false;
                uint16_t _status = 0;
                uint32_t _bodySize = 0;
                bool _bodyIncomplete = false;
                std::string _body;
                std::vector<std::pair<std::string, std::string>> _headers;

                bool success() const { return _success; }
                uint16_t status() const { return _status; }
                uint32_t bodySize() const { return _bodySize; }
                bool bodyIncomplete() const { return _bodyIncomplete; }
                const std::string &body() const { return _body; }

                // case-insensitive lookup; returns "" if not present
                std::string header(const char *name) const;
            };

            using DataCallback = std::function<void(const uint8_t *data, size_t len)>;
            using DoneCallback = std::function<void(const Response &response)>;

            enum class VerifyMode : uint8_t { NONE, CERTIFICATE };

            class Handler;

            class Request
            {
              public:
                Request &header(const char *name, const char *value);
                Request &body(const char *data, size_t len);
                Request &body(const std::string &text);
                Request &contentType(const char *ct);
                Request &verifyCertificate(const char *pemCert);
                Request &onData(DataCallback cb);
                Request &onDone(DoneCallback cb);
                Request &ignoreHeaders();
                Request &maxBodySize(size_t maxSize = OPENKNX_WEBCLIENT_MAX_BODY);
                Request &ignoreBody();

                bool send();

              private:
                friend class Handler;
                Request(Handler *handler, const char *method, std::string url);

                Handler *_handler;
                std::string _url;
                char _method[8];
                std::string _body;
                VerifyMode _verifyMode = VerifyMode::NONE;
                const char *_verify = nullptr;
                std::vector<std::pair<std::string, std::string>> _headers;
                DataCallback _onData;
                DoneCallback _onDone;
                bool _ignoreHeaders = false;
                size_t _bodyMaxSize = OPENKNX_WEBCLIENT_MAX_BODY;
            };

            struct Slot
            {
                enum class State : uint8_t
                {
                    IDLE,
                    RESOLVING,
                    CONNECTING,
                    SSL_HANDSHAKE,
                    SENDING_HDR,
                    SENDING_BODY,
                    HEADERS,
                    BODY
                } state = State::IDLE;

                uint32_t requestId = 0;
                uint32_t startMs = 0;

                char host[64] = {};
                uint16_t port = 80;
                char path[256] = {};
                bool ssl = false;
                VerifyMode verifyMode = VerifyMode::NONE;
                const char *verify = nullptr;
                char method[8] = {};

                DataCallback onData;
                DoneCallback onDone;

                bool ignoreHeaders = false;
                uint32_t bodyMaxSize = 0;
                std::vector<std::pair<std::string, std::string>> responseHeaders;

                uint8_t txBuf[512] = {};
                uint16_t txLen = 0;
                uint16_t txSent = 0;
                std::string requestBody;
                uint32_t bodySent = 0;
                std::vector<std::pair<std::string, std::string>> extraHeaders;

                uint8_t rxBuf[512] = {};

                bool headersComplete = false;
                bool hdrExpectLF = false;
                uint16_t httpStatus = 0;
                int32_t contentLength = -1;
                bool isChunked = false;
                char hdrLine[128] = {};
                uint8_t hdrLineLen = 0;

                uint32_t bodyReceived = 0;
                bool bodyIncomplete = false;
                std::string responseBody;

                enum class ChunkState : uint8_t
                {
                    SIZE,
                    SIZE_LF,
                    DATA,
                    DATA_CR,
                    DATA_LF,
                    TRAILER
                } chunkState = ChunkState::SIZE;
                uint32_t chunkRemaining = 0;
                char chunkSizeBuf[9] = {};
                uint8_t chunkSizeLen = 0;
                bool chunkDone = false;

#ifdef ARDUINO_ARCH_RP2040
                // lwIP raw API for non-blocking TCP (plain HTTP and SSL alike)
                struct tcp_pcb *tcpPcb    = nullptr;
                volatile bool   tcpConnEst = false;
                volatile bool   tcpError   = false;

                // Zero-copy receive: pbuf chain from lwipRecv, consumed in platformRead
                struct pbuf *rxHead   = nullptr; // first pbuf with unread data
                struct pbuf *rxTail   = nullptr; // last pbuf for O(1) append
                uint16_t     rxOffset = 0;       // byte offset into rxHead->payload

                // BearSSL state — allocated on demand when ssl=true; defined in Handler_RP2040.cpp
                struct BrState *brState = nullptr;
#endif
            };

            struct PendingRequest
            {
                std::string url;
                char method[8];
                std::string body;
                VerifyMode verifyMode = VerifyMode::NONE;
                const char *verify = nullptr;
                std::vector<std::pair<std::string, std::string>> headers;
                DataCallback onData;
                DoneCallback onDone;
                bool ignoreHeaders = false;
                uint32_t bodyMaxSize = 0;
            };

            class Handler
            {
              public:
                Request get(const std::string &url);
                Request post(const std::string &url);
                Request put(const std::string &url);
                Request del(const std::string &url);
                Request patch(const std::string &url);
                Request head(const std::string &url);

                void loop();
                bool enqueue(PendingRequest &&req);

              private:
                Slot _slots[OPENKNX_WEBCLIENT_SLOTS];
                std::deque<PendingRequest> _pending;
                uint32_t _nextRequestId = 1;

                bool parseUrl(const std::string &url, char *host, uint16_t &port, char *path, bool &ssl);
                void startSlot(int idx, PendingRequest &&req);
                void processSlot(int idx);
                void finishSlot(int idx, bool success);
                void startNextPending();

                void formatRequestHeaders(Slot &s);
                bool processHeaderByte(Slot &s, uint8_t b);
                void parseHeaderLine(Slot &s);
                bool processBodyBytes(Slot &s, const uint8_t *data, size_t len);

                // Platform methods (implemented in Handler_ESP32.cpp / Handler_RP2040.cpp)
                bool platformConnect(Slot &s, IPAddress ip, uint16_t port);
                bool platformConnectComplete(Slot &s);
                bool platformConnected(Slot &s);
                int platformSend(Slot &s, const uint8_t *data, size_t len);
                int platformRead(Slot &s, uint8_t *buf, size_t len);
                void platformClose(Slot &s);

#ifdef ARDUINO_ARCH_RP2040
                bool platformSslInit(Slot &s);
                int  platformSslStep(Slot &s); // -1=error, 0=in progress, 1=done
                static err_t lwipConnected(void *arg, struct tcp_pcb *pcb, err_t err);
                static err_t lwipRecv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err);
                static void  lwipErr(void *arg, err_t err);
#endif

#ifdef ARDUINO_ARCH_ESP32
                struct TaskRequest
                {
                    uint32_t requestId;
                    uint32_t timeoutMs;
                    char host[64];
                    uint16_t port;
                    char path[256];
                    bool ssl;
                    VerifyMode verifyMode;
                    const char *verify;
                    char method[8];
                    std::string body;
                    std::vector<std::pair<std::string, std::string>> headers;
                    bool hasDataCallback;
                    bool ignoreHeaders;
                    uint32_t bodyMaxSize;
                };

                struct ResultEntry
                {
                    enum class Type : uint8_t { DATA, DONE } type;
                    uint32_t requestId;
                    uint8_t data[512];
                    size_t dataLen;
                    Response response;
                };

                struct ActiveCallback
                {
                    DataCallback onData;
                    DoneCallback onDone;
                };

                QueueHandle_t _requestQueue = nullptr;
                QueueHandle_t _resultQueue  = nullptr;
                TaskHandle_t  _task         = nullptr;
                std::vector<std::pair<uint32_t, ActiveCallback>> _activeCallbacks;

                void initTask();
                void enqueueResult(ResultEntry &&entry);
                static void taskRun(void *arg);
#endif
            };

        } // namespace Webclient
    }     // namespace Network
} // namespace OpenKNX

#endif // defined(ARDUINO_ARCH_RP2040) || defined(ARDUINO_ARCH_ESP32)
#endif // OPENKNX_WEBCLIENT
