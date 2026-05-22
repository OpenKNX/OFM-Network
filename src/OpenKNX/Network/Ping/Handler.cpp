#include "OpenKNX/Network/Ping/Handler.h"

#ifdef OPENKNX_PING

#include <algorithm>

#if defined(ARDUINO_ARCH_RP2040) || defined(ARDUINO_ARCH_ESP32)
#include "OpenKNX.h"
#include <lwip/dns.h>
#include <lwip/ip_addr.h>
#endif
#ifdef ARDUINO_ARCH_ESP32
#include <lwip/tcpip.h>
#endif

namespace OpenKNX
{
    namespace Network
    {
        namespace Ping
        {

#if defined(ARDUINO_ARCH_RP2040) || defined(ARDUINO_ARCH_ESP32)
            struct DnsCallbackArg
            {
                Handler* queue;
                uint32_t timeoutMs;
                std::function<void(IPAddress, bool, uint32_t)> callback;
            };

            struct DnsCallbackArgRetry
            {
                Handler* queue;
                uint32_t timeoutMs;
                uint8_t retries;
                std::function<void(IPAddress, bool)> callback;
            };

            static void dnsFoundCallback(const char* name, const ip_addr_t* ipaddr, void* arg)
            {
                DnsCallbackArg* darg = static_cast<DnsCallbackArg*>(arg);
                if (ipaddr != nullptr)
                {
                    const ip4_addr_t* ip4 = ip_2_ip4(ipaddr);
                    IPAddress ip(ip4_addr1(ip4), ip4_addr2(ip4), ip4_addr3(ip4), ip4_addr4(ip4));
#ifdef ARDUINO_ARCH_ESP32
                    // Callback runs inside TCPIP task which holds the core lock.
                    // Calling sendto() here would deadlock — enqueue for loop() instead.
                    darg->queue->enqueueDnsResult(ip, darg->timeoutMs, darg->callback);
#else
                    // RP2040: NO_SYS=1, single-threaded — direct call is safe.
                    darg->queue->ping(ip, darg->callback, darg->timeoutMs);
#endif
                }
                else if (darg->callback)
                {
                    darg->callback(IPAddress(0, 0, 0, 0), false, 0);
                }
                delete darg;
            }

            static void dnsFoundCallbackRetry(const char* name, const ip_addr_t* ipaddr, void* arg)
            {
                DnsCallbackArgRetry* darg = static_cast<DnsCallbackArgRetry*>(arg);
                if (ipaddr != nullptr)
                {
                    const ip4_addr_t* ip4 = ip_2_ip4(ipaddr);
                    IPAddress ip(ip4_addr1(ip4), ip4_addr2(ip4), ip4_addr3(ip4), ip4_addr4(ip4));
#ifdef ARDUINO_ARCH_ESP32
                    // Callback runs inside TCPIP task which holds the core lock.
                    // Calling sendto() here would deadlock — enqueue for loop() instead.
                    darg->queue->enqueueDnsResultRetry(ip, darg->retries, darg->timeoutMs, darg->callback);
#else
                    // RP2040: NO_SYS=1, single-threaded — direct call is safe.
                    darg->queue->ping(ip, darg->callback, darg->retries, darg->timeoutMs);
#endif
                }
                else if (darg->callback)
                {
                    darg->callback(IPAddress(0, 0, 0, 0), false);
                }
                delete darg;
            }
#endif

            Handler::Handler()
                : _activePingCount(0), _lastLoopTime(0), _nextEchoSeq(0)
            {
                for (int i = 0; i < OPENKNX_PING_PARALLEL; i++)
                {
                    _activeSlots[i].state = PingRequest::IDLE;
                    _activeSlots[i].echoId = i;
                    _activeSlots[i].echoSeq = 0;
                    _activeSlots[i].sock = -1;
                }

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
                request.startTimeMs = 0;
                request.rttMs = 0;
                request.state = PingRequest::IDLE;
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

            void Handler::enqueueDnsResult(IPAddress target, uint32_t timeoutMs,
                                           std::function<void(IPAddress, bool, uint32_t)> callback)
            {
#ifdef ARDUINO_ARCH_ESP32
                taskENTER_CRITICAL(&_dnsMux);
#endif
                _resolvedDns.push_back({target, timeoutMs, callback});
#ifdef ARDUINO_ARCH_ESP32
                taskEXIT_CRITICAL(&_dnsMux);
#endif
            }

            void Handler::enqueueDnsResultRetry(IPAddress target, uint8_t retries, uint32_t timeoutMs,
                                                std::function<void(IPAddress, bool)> callback)
            {
#ifdef ARDUINO_ARCH_ESP32
                taskENTER_CRITICAL(&_dnsMux);
#endif
                _resolvedDnsRetry.push_back({target, retries, timeoutMs, callback});
#ifdef ARDUINO_ARCH_ESP32
                taskEXIT_CRITICAL(&_dnsMux);
#endif
            }

            void Handler::loop()
            {
                if (_activePingCount == 0 && _pendingQueue.empty() && _resolvedDns.empty() && _resolvedDnsRetry.empty())
                    return;

                checkReplies();

                if (!delayCheckMillis(_lastLoopTime, 50))
                    return;
                _lastLoopTime = millis();

                processDnsResults();
                processDnsResultsRetry();
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

            void Handler::processDnsResults()
            {
                while (!_resolvedDns.empty())
                {
#ifdef ARDUINO_ARCH_ESP32
                    taskENTER_CRITICAL(&_dnsMux);
#endif
                    DnsResult r = _resolvedDns.front();
                    _resolvedDns.pop_front();
#ifdef ARDUINO_ARCH_ESP32
                    taskEXIT_CRITICAL(&_dnsMux);
#endif
                    ping(r.target, r.callback, r.timeoutMs);
                }
            }

            void Handler::processDnsResultsRetry()
            {
                while (!_resolvedDnsRetry.empty())
                {
#ifdef ARDUINO_ARCH_ESP32
                    taskENTER_CRITICAL(&_dnsMux);
#endif
                    DnsResultRetry r = _resolvedDnsRetry.front();
                    _resolvedDnsRetry.pop_front();
#ifdef ARDUINO_ARCH_ESP32
                    taskEXIT_CRITICAL(&_dnsMux);
#endif
                    ping(r.target, r.callback, r.retries, r.timeoutMs);
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
                DnsCallbackArg* arg = new DnsCallbackArg{this, timeoutMs, callback};
                ip_addr_t resolved;
#ifdef ARDUINO_ARCH_ESP32
                LOCK_TCPIP_CORE();
#endif
                err_t err = dns_gethostbyname(host.c_str(), &resolved, dnsFoundCallback, arg);
#ifdef ARDUINO_ARCH_ESP32
                UNLOCK_TCPIP_CORE();
#endif
                if (err == ERR_OK)
                {
                    // Already cached — enqueue so ping() runs from loop(), not from here
                    dnsFoundCallback(host.c_str(), &resolved, arg);
                }
                else if (err != ERR_INPROGRESS)
                {
                    if (callback)
                        callback(IPAddress(0, 0, 0, 0), false, 0);
                    delete arg;
                }
                // ERR_INPROGRESS: dnsFoundCallback will fire when resolved
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
                DnsCallbackArgRetry* arg = new DnsCallbackArgRetry{this, timeoutMs, retries, callback};
                ip_addr_t resolved;
#ifdef ARDUINO_ARCH_ESP32
                LOCK_TCPIP_CORE();
#endif
                err_t err = dns_gethostbyname(host.c_str(), &resolved, dnsFoundCallbackRetry, arg);
#ifdef ARDUINO_ARCH_ESP32
                UNLOCK_TCPIP_CORE();
#endif
                if (err == ERR_OK)
                {
                    // Already cached — call callback directly
                    dnsFoundCallbackRetry(host.c_str(), &resolved, arg);
                }
                else if (err != ERR_INPROGRESS)
                {
                    if (callback)
                        callback(IPAddress(0, 0, 0, 0), false);
                    delete arg;
                }
                // ERR_INPROGRESS: dnsFoundCallbackRetry will fire when resolved
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
