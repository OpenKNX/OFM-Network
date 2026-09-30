#if defined(ARDUINO_ARCH_RP2040) || defined(ARDUINO_ARCH_ESP32)

#include "OpenKNX/Network/DNS.h"

#include <lwip/dns.h>
#include <lwip/ip_addr.h>
#include <utility>
#include <vector>

#ifdef ARDUINO_ARCH_ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lwip/tcpip.h>
#else
#include <lwip_wrap.h> // LWIPMutex: makes the ethernet IRQ defer
#endif

namespace OpenKNX
{
    namespace Network
    {
        namespace DNS
        {
            using Callback = std::function<void(IPAddress)>;

            // Results are only ever delivered from loop(): the lwIP callback runs in the
            // TCP/IP task (ESP32) or the ethernet IRQ (RP2040).
            static std::vector<std::pair<IPAddress, Callback>> s_pending;

#ifdef ARDUINO_ARCH_ESP32
            // A mutex, not a critical section: push_back and std::function allocate.
            static SemaphoreHandle_t s_lock = nullptr;

            static void ensureLock()
            {
                if (s_lock == nullptr) s_lock = xSemaphoreCreateMutex();
            }

            struct PendingLock
            {
                PendingLock()
                {
                    ensureLock();
                    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
                }
                ~PendingLock()
                {
                    if (s_lock) xSemaphoreGive(s_lock);
                }
            };
#else
            using PendingLock = LWIPMutex;
#endif

            static void enqueue(IPAddress ip, Callback&& cb)
            {
                PendingLock lock;
                s_pending.push_back({ip, std::move(cb)});
            }

            static void dnsFoundCallback(const char*, const ip_addr_t* ipaddr, void* arg)
            {
                auto* cb = static_cast<Callback*>(arg);
                IPAddress ip;
                if (ipaddr)
                {
                    const ip4_addr_t* ip4 = ip_2_ip4(ipaddr);
                    ip = IPAddress(ip4_addr1(ip4), ip4_addr2(ip4), ip4_addr3(ip4), ip4_addr4(ip4));
                }
                enqueue(ip, std::move(*cb));
                delete cb;
            }

            void query(const std::string& name, Callback callback)
            {
                auto* cb = new Callback(std::move(callback));
                ip_addr_t resolved;
#ifdef ARDUINO_ARCH_ESP32
                // Created here while only the loop task runs; dnsFoundCallback comes
                // later from the TCP/IP task.
                ensureLock();
                LOCK_TCPIP_CORE();
#endif
                err_t err = dns_gethostbyname(name.c_str(), &resolved, dnsFoundCallback, cb);
#ifdef ARDUINO_ARCH_ESP32
                UNLOCK_TCPIP_CORE();
#endif
                if (err == ERR_INPROGRESS) return;

                IPAddress ip;
                if (err == ERR_OK)
                {
                    const ip4_addr_t* ip4 = ip_2_ip4(&resolved);
                    ip = IPAddress(ip4_addr1(ip4), ip4_addr2(ip4), ip4_addr3(ip4), ip4_addr4(ip4));
                }
                enqueue(ip, std::move(*cb));
                delete cb;
            }

            void loop()
            {
                // Unlocked peek: a torn read only delays a result by one tick, while taking
                // LWIPMutex every tick would keep rescheduling the ethernet poll on RP2040.
                if (s_pending.empty()) return;
                std::vector<std::pair<IPAddress, Callback>> pending;
                {
                    PendingLock lock;
                    if (s_pending.empty()) return;
                    pending.swap(s_pending);
                }
                for (auto& [ip, cb] : pending)
                    cb(ip);
            }

        } // namespace DNS
    } // namespace Network
} // namespace OpenKNX

#endif // defined(ARDUINO_ARCH_RP2040) || defined(ARDUINO_ARCH_ESP32)
