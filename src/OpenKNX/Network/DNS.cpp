#if defined(ARDUINO_ARCH_RP2040) || defined(ARDUINO_ARCH_ESP32)

#include "OpenKNX/Network/DNS.h"

#include <lwip/dns.h>
#include <lwip/ip_addr.h>

#ifdef ARDUINO_ARCH_ESP32
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lwip/tcpip.h>
#include <utility>
#endif

namespace OpenKNX
{
    namespace Network
    {
        namespace DNS
        {
            using Callback = std::function<void(IPAddress)>;

#ifdef ARDUINO_ARCH_ESP32
            // Mutex statt Critical Section: push_back/std::function allokieren,
            // und Heap-Zugriffe sind in einer Critical Section unzulässig.
            static SemaphoreHandle_t s_lock = nullptr;
            static std::vector<std::pair<IPAddress, Callback>> s_pending;

            static void ensureLock()
            {
                if (s_lock == nullptr) s_lock = xSemaphoreCreateMutex();
            }

            static void lock()
            {
                ensureLock();
                if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
            }

            static void unlock()
            {
                if (s_lock) xSemaphoreGive(s_lock);
            }

            static void enqueue(IPAddress ip, Callback&& cb)
            {
                lock();
                s_pending.push_back({ip, std::move(cb)});
                unlock();
            }
#endif

            static void dnsFoundCallback(const char*, const ip_addr_t* ipaddr, void* arg)
            {
                auto* cb = static_cast<Callback*>(arg);
                IPAddress ip;
                if (ipaddr)
                {
                    const ip4_addr_t* ip4 = ip_2_ip4(ipaddr);
                    ip = IPAddress(ip4_addr1(ip4), ip4_addr2(ip4), ip4_addr3(ip4), ip4_addr4(ip4));
                }
#ifdef ARDUINO_ARCH_ESP32
                enqueue(ip, std::move(*cb));
#else
                (*cb)(ip);
#endif
                delete cb;
            }

            void query(const std::string& name, Callback callback)
            {
                auto* cb = new Callback(std::move(callback));
                ip_addr_t resolved;
#ifdef ARDUINO_ARCH_ESP32
                // hier anlegen, solange nur der Loop-Task läuft — dnsFoundCallback
                // kommt später aus dem TCP/IP-Task
                ensureLock();
                LOCK_TCPIP_CORE();
#endif
                err_t err = dns_gethostbyname(name.c_str(), &resolved, dnsFoundCallback, cb);
#ifdef ARDUINO_ARCH_ESP32
                UNLOCK_TCPIP_CORE();
#endif
                if (err != ERR_INPROGRESS)
                {
                    Callback owned = std::move(*cb);
                    delete cb;
                    if (err == ERR_OK)
                    {
                        const ip4_addr_t* ip4 = ip_2_ip4(&resolved);
                        IPAddress ip(ip4_addr1(ip4), ip4_addr2(ip4), ip4_addr3(ip4), ip4_addr4(ip4));
#ifdef ARDUINO_ARCH_ESP32
                        enqueue(ip, std::move(owned));
#else
                        owned(ip);
#endif
                    }
                    else
                    {
                        owned(IPAddress());
                    }
                }
            }

            void loop()
            {
#ifdef ARDUINO_ARCH_ESP32
                lock();
                auto pending = std::move(s_pending);
                s_pending.clear();
                unlock();
                for (auto& [ip, cb] : pending)
                    cb(ip);
#endif
            }

        } // namespace DNS
    } // namespace Network
} // namespace OpenKNX

#endif // defined(ARDUINO_ARCH_RP2040) || defined(ARDUINO_ARCH_ESP32)
