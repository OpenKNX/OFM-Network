#pragma once

#ifdef OPENKNX_PING

#include <IPAddress.h>
#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#ifndef OPENKNX_PING_TIMEOUT
#define OPENKNX_PING_TIMEOUT 1000
#endif

#ifndef OPENKNX_PING_PARALLEL
#define OPENKNX_PING_PARALLEL 5
#endif

namespace OpenKNX
{
    namespace Network
    {
        namespace Ping
        {

            struct PingRequest
            {
                enum State
                {
                    IDLE = 0,
                    PINGING = 1,
                    TIMEOUT = 2,
                    SUCCESS = 3
                };

                IPAddress target;
                uint32_t timeoutMs = OPENKNX_PING_TIMEOUT;
                uint32_t startTimeMs = 0;
                State state = IDLE;
                std::function<void(IPAddress, bool, uint32_t)> callback;
                uint8_t echoSeq = 0;
            };

            class Handler
            {
              public:
                Handler();
                ~Handler();

                std::string logPrefix() { return "Ping"; }

                void ping(IPAddress target, std::function<void(IPAddress, bool, uint32_t)> callback = nullptr,
                          uint32_t timeoutMs = OPENKNX_PING_TIMEOUT);
                void ping(IPAddress target, std::function<void(IPAddress, bool)> callback,
                          uint8_t retries = 2, uint32_t timeoutMs = OPENKNX_PING_TIMEOUT);
                void ping(const std::string& host, std::function<void(IPAddress, bool, uint32_t)> callback = nullptr,
                          uint32_t timeoutMs = OPENKNX_PING_TIMEOUT);
                void ping(const std::string& host, std::function<void(IPAddress, bool)> callback,
                          uint8_t retries = 2, uint32_t timeoutMs = OPENKNX_PING_TIMEOUT);
                void loop();

                uint32_t getPendingCount() const { return _pendingQueue.size(); }
                uint32_t getActivePingCount() const { return _activePingCount; }

              private:
                struct RetryContext
                {
                    Handler* handler;
                    IPAddress target;
                    std::function<void(IPAddress, bool)> userCallback;
                    uint8_t retriesLeft;
                    uint32_t timeoutMs;
                    std::function<void(IPAddress, bool, uint32_t)> innerCallback;
                };

                std::deque<PingRequest> _pendingQueue;
                std::array<PingRequest, OPENKNX_PING_PARALLEL> _activeSlots;
                std::vector<std::shared_ptr<RetryContext>> _pendingRetries;
                uint32_t _activePingCount;
                uint32_t _lastLoopTime;
                uint8_t _nextEchoSeq;

                void startNextPending();
                void checkReplies();
                void processTimeouts();
                void processPendingSlots();
                int findFreeSlot();
                void dispatchCallback(int slotIndex, bool reachable, uint32_t rttMs);

                bool platformSendPing(int slotIndex, IPAddress target);
                bool platformCheckReply(int slotIndex, uint32_t& replyTimeMs);
                void platformClosePing(int slotIndex);
                void platformInitialize();
                void platformCleanup();
            };

        } // namespace Ping
    } // namespace Network
} // namespace OpenKNX

#endif // OPENKNX_PING
