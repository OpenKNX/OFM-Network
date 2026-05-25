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
            using DataCallback = std::function<bool(const uint8_t *data, size_t len)>;
            using DoneCallback = std::function<void(bool success, uint16_t httpStatus)>;

            class Handler;

            class Request
            {
              public:
                Request &header(const char *name, const char *value);
                Request &body(const char *data, size_t len);
                Request &body(const std::string &text);
                Request &contentType(const char *ct);
                Request &insecure();
                Request &ca(const char *pemCert);

                void send(DataCallback onData, DoneCallback onDone = nullptr);
                void send(DoneCallback onDone);

              private:
                friend class Handler;
                Request(Handler *handler, const char *method, std::string url);

                Handler *_handler;
                std::string _url;
                char _method[8];
                std::string _body;
                const char *_caCert = nullptr;
                bool _insecure = false;
                std::vector<std::pair<std::string, std::string>> _headers;
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
                bool insecure = false;
                const char *caCert = nullptr;
                char method[8] = {};

                DataCallback onData;
                DoneCallback onDone;

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
                const char *caCert;
                bool insecure;
                std::vector<std::pair<std::string, std::string>> headers;
                DataCallback onData;
                DoneCallback onDone;
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
                void enqueue(PendingRequest &&req);

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
                    bool insecure;
                    const char *caCert;
                    char method[8];
                    std::string body;
                    std::vector<std::pair<std::string, std::string>> headers;
                };

                struct ResultEntry
                {
                    enum class Type : uint8_t { DATA, DONE } type;
                    uint32_t requestId;
                    uint8_t data[512];
                    size_t dataLen;
                    bool success;
                    uint16_t httpStatus;
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
