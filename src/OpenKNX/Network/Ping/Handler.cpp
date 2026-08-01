#include "OpenKNX/Network/Ping/Handler.h"

#ifdef OPENKNX_PING

#include <algorithm>

#if defined(ARDUINO_ARCH_RP2040) || defined(ARDUINO_ARCH_ESP32)
#include "OpenKNX.h"
#include "OpenKNX/Network/DNS.h"
#endif

namespace OpenKNX
{
    namespace Network
    {
        namespace Ping
        {

            Handler::Handler()
                : _activePingCount(0), _lastLoopTime(0), _nextEchoSeq(0)
            {
                platformInitialize();
            }

            Handler::~Handler()
            {
                platformCleanup();
            }

            void Handler::ping(IPAddress target, std::function<void(IPAddress, bool, uint32_t)> callback,
                               uint32_t timeoutMs)
            {
                if (target == IPAddress(0, 0, 0, 0))
                {
                    if (callback)
                        callback(target, false, 0);
                    return;
                }

                PingRequest request;
                request.target = target;
                request.timeoutMs = timeoutMs;
                request.callback = callback;
                request.echoSeq = _nextEchoSeq++;

                int freeSlot = findFreeSlot();
                if (freeSlot >= 0)
                {
                    _activeSlots[freeSlot] = request;
                    _activeSlots[freeSlot].state = PingRequest::PINGING;
                    _activeSlots[freeSlot].startTimeMs = millis();
                    _activePingCount++;

                    if (!platformSendPing(freeSlot, target))
                        dispatchCallback(freeSlot, false, 0);
                }
                else
                {
                    _pendingQueue.push_back(request);
                }
            }

            void Handler::ping(IPAddress target, std::function<void(IPAddress, bool)> callback,
                               uint8_t retries, uint32_t timeoutMs)
            {
                if (target == IPAddress(0, 0, 0, 0))
                {
                    if (callback) callback(target, false);
                    return;
                }

                auto ctx = std::make_shared<RetryContext>();
                ctx->handler = this;
                ctx->target = target;
                ctx->userCallback = std::move(callback);
                ctx->retriesLeft = retries;
                ctx->timeoutMs = timeoutMs;

                std::weak_ptr<RetryContext> weak = ctx;
                ctx->innerCallback = [this, weak](IPAddress ip, bool reachable, uint32_t) {
                    auto ctx = weak.lock();
                    if (!ctx) return;

                    if (reachable)
                    {
                        auto cb = std::move(ctx->userCallback);
                        auto* raw = ctx.get();
                        _pendingRetries.erase(
                            std::remove_if(_pendingRetries.begin(), _pendingRetries.end(),
                                           [raw](const std::shared_ptr<RetryContext>& p) { return p.get() == raw; }),
                            _pendingRetries.end());
                        if (cb) cb(ip, true);
                    }
                    else if (ctx->retriesLeft > 0)
                    {
                        ctx->retriesLeft--;
                        ping(ctx->target, ctx->innerCallback, ctx->timeoutMs);
                    }
                    else
                    {
                        auto cb = std::move(ctx->userCallback);
                        auto* raw = ctx.get();
                        _pendingRetries.erase(
                            std::remove_if(_pendingRetries.begin(), _pendingRetries.end(),
                                           [raw](const std::shared_ptr<RetryContext>& p) { return p.get() == raw; }),
                            _pendingRetries.end());
                        if (cb) cb(ip, false);
                    }
                };

                _pendingRetries.push_back(ctx);
                ping(target, ctx->innerCallback, timeoutMs);
            }


            void Handler::loop()
            {
                if (_activePingCount == 0 && _pendingQueue.empty())
                    return;

                checkReplies();

                if (!delayCheckMillis(_lastLoopTime, 50))
                    return;
                _lastLoopTime = millis();

                processTimeouts();
                startNextPending();
            }

            void Handler::checkReplies()
            {
                uint32_t replyTimeMs = 0;
                for (int i = 0; i < OPENKNX_PING_PARALLEL; i++)
                {
                    if (_activeSlots[i].state == PingRequest::PINGING)
                    {
                        if (platformCheckReply(i, replyTimeMs))
                        {
                            uint32_t rttMs = replyTimeMs - _activeSlots[i].startTimeMs;
                            dispatchCallback(i, true, rttMs);
                        }
                    }
                }
            }

            void Handler::processTimeouts()
            {
                for (int i = 0; i < OPENKNX_PING_PARALLEL; i++)
                {
                    if (_activeSlots[i].state == PingRequest::PINGING)
                    {
                        uint32_t elapsed = millis() - _activeSlots[i].startTimeMs;
                        if (elapsed >= _activeSlots[i].timeoutMs)
                        {
                            platformClosePing(i);
                            dispatchCallback(i, false, 0);
                        }
                    }
                    else if (_activeSlots[i].state == PingRequest::SUCCESS ||
                             _activeSlots[i].state == PingRequest::TIMEOUT)
                    {
                        _activeSlots[i].state = PingRequest::IDLE;
                        _activePingCount--;
                    }
                }
            }

            // kept for backward compatibility — delegates to processTimeouts
            void Handler::processPendingSlots()
            {
                processTimeouts();
            }

            int Handler::findFreeSlot()
            {
                for (int i = 0; i < OPENKNX_PING_PARALLEL; i++)
                {
                    if (_activeSlots[i].state == PingRequest::IDLE)
                        return i;
                }
                return -1;
            }

            void Handler::startNextPending()
            {
                while (!_pendingQueue.empty() && findFreeSlot() >= 0)
                {
                    PingRequest request = _pendingQueue.front();
                    _pendingQueue.pop_front();

                    int freeSlot = findFreeSlot();
                    if (freeSlot >= 0)
                    {
                        _activeSlots[freeSlot] = request;
                        _activeSlots[freeSlot].state = PingRequest::PINGING;
                        _activeSlots[freeSlot].startTimeMs = millis();
                        _activePingCount++;

                        if (!platformSendPing(freeSlot, request.target))
                            dispatchCallback(freeSlot, false, 0);
                    }
                }
            }

            void Handler::dispatchCallback(int slotIndex, bool reachable, uint32_t rttMs)
            {
                if (slotIndex < 0 || slotIndex >= OPENKNX_PING_PARALLEL)
                    return;

                PingRequest& req = _activeSlots[slotIndex];
                if (req.callback)
                    req.callback(req.target, reachable, rttMs);

                req.state = reachable ? PingRequest::SUCCESS : PingRequest::TIMEOUT;
                req.callback = nullptr;
            }

            void Handler::ping(const std::string& host, std::function<void(IPAddress, bool, uint32_t)> callback,
                               uint32_t timeoutMs)
            {
                IPAddress ip;
                if (ip.fromString(host.c_str()))
                {
                    ping(ip, callback, timeoutMs);
                    return;
                }

#if defined(ARDUINO_ARCH_RP2040) || defined(ARDUINO_ARCH_ESP32)
                OpenKNX::Network::DNS::query(host, [this, timeoutMs, callback](IPAddress ip) {
                    ping(ip, callback, timeoutMs);
                });
#else
                if (callback)
                    callback(IPAddress(0, 0, 0, 0), false, 0);
#endif
            }

            void Handler::ping(const std::string& host, std::function<void(IPAddress, bool)> callback,
                               uint8_t retries, uint32_t timeoutMs)
            {
                IPAddress ip;
                if (ip.fromString(host.c_str()))
                {
                    ping(ip, callback, retries, timeoutMs);
                    return;
                }

#if defined(ARDUINO_ARCH_RP2040) || defined(ARDUINO_ARCH_ESP32)
                OpenKNX::Network::DNS::query(host, [this, timeoutMs, retries, callback](IPAddress ip) {
                    ping(ip, callback, retries, timeoutMs);
                });
#else
                if (callback)
                    callback(IPAddress(0, 0, 0, 0), false);
#endif
            }

#if !defined(ARDUINO_ARCH_ESP32) && !defined(ARDUINO_ARCH_RP2040)

            bool Handler::platformSendPing(int slotIndex, IPAddress target)
            {
                return false;
            }

            bool Handler::platformCheckReply(int slotIndex, uint32_t& replyTimeMs)
            {
                return false;
            }

            void Handler::platformClosePing(int slotIndex) {}

            void Handler::platformInitialize() {}

            void Handler::platformCleanup() {}

#endif

        } // namespace Ping
    } // namespace Network
} // namespace OpenKNX

#endif // OPENKNX_PING
