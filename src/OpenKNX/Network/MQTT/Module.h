#pragma once
#if defined(KNX_IP_WIFI) || defined(KNX_IP_LAN)
#ifdef OPENKNX_MQTT

#include "Broker.h"
#include "Client.h"
#include "OpenKNX.h"
#include <functional>
#include <string>

#ifndef OPENKNX_MQTT_STATUS_TIME
#define OPENKNX_MQTT_STATUS_TIME 10000
#endif

#ifndef OPENKNX_MQTT_TASK_STACK
#define OPENKNX_MQTT_TASK_STACK 4096
#endif

// Max messages waiting to go out. Entries are allocated on demand and sized to the
// actual topic + payload (PSRAM where available), so this caps the worst case only --
// an idle queue costs nothing and the common status/uptime message is a few dozen bytes.
#ifndef OPENKNX_MQTT_MAX_QUEUE
#define OPENKNX_MQTT_MAX_QUEUE 32
#endif

#ifdef ARDUINO_ARCH_ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#endif

namespace OpenKNX
{
    namespace Network
    {
        namespace MQTT
        {
            class Module
            {
              public:
                void setup(bool configured);
                void loop(bool configured);

                bool active() const;
                bool connected() const;
                std::string devicePrefix();

                // Publish to absolute topic. Queues the message and returns immediately --
                // it never waits on the socket, so it is safe to call from timing-critical
                // paths. true = accepted for sending, not delivered.
                bool publish(const char *topic, const void *payload, size_t len,
                             uint8_t qos = 0, bool retain = false);
                bool publish(const std::string &topic, const std::string &payload,
                             uint8_t qos = 0, bool retain = false);

                // Publish with device prefix prepended
                bool publishP(const std::string &topic, const std::string &payload,
                              uint8_t qos = 0, bool retain = false);
                bool publishP(const std::string &topic, const void *payload, size_t len,
                              uint8_t qos = 0, bool retain = false);

                void subscribe(const std::string &topic,
                               std::function<void(const char *, const void *, size_t)> callback,
                               uint8_t qos = 0);

              private:
                std::string logPrefix() const { return "Network<MQTT>"; }

                const char *cfgServer() const { return (const char *)ParamNET_MQTTServer; }
                const char *cfgUsername() const { return (const char *)ParamNET_MQTTUsername; }
                const char *cfgPassword() const { return (const char *)ParamNET_MQTTPassword; }
                const char *cfgPrefix() const { return (const char *)ParamNET_MQTTPrefix; }
                uint16_t cfgPort() const { return ParamNET_MQTTPort; }
                bool cfgBroker() const { return ParamNET_MQTTMode == 1; }

                char _devicePrefix[30] = {};
                uint32_t _lastStatus = 0;
                bool _initialized = false;

                // Mirror, refreshed once per MQTT round -- reading broker/client directly
                // would mean taking _mutex from the KNX loop. See AGENTS.md.
                volatile bool _connected = false;
                void refreshConnected();

                Client _client;
                Broker _broker;

                // Send queue: publishing inline would block (ESP32) or drop under load
                // (RP2040) -- see AGENTS.md. One block per entry, header + topic + payload.
                struct QueueEntry
                {
                    QueueEntry *next;
                    uint16_t topicLen;
                    uint16_t payloadLen;
                    uint8_t qos;
                    bool retain;
                    // topic (NUL-terminated) then payload follow in the same block
                    const char *topic() const { return (const char *)(this + 1); }
                    const void *payload() const { return topic() + topicLen + 1; }
                };
                QueueEntry *_queueHead = nullptr; // dequeued here
                QueueEntry *_queueTail = nullptr; // enqueued here
                uint16_t _queueCount = 0;
                uint32_t _queueDropped = 0; // cap reached or out of memory

                bool enqueue(const char *topic, const void *payload, size_t len,
                             uint8_t qos, bool retain);
                void drainQueue();                 // MQTT send path only, _mutex already held on ESP32
                void clearQueue();                 // same context as drainQueue()
                void unlinkHead(QueueEntry *e);    // dito -- frees the entry too

#ifdef ARDUINO_ARCH_ESP32
                // Guards link/unlink only, never I/O -- so a spinlock, not _mutex.
                portMUX_TYPE _queueLock = portMUX_INITIALIZER_UNLOCKED;
                void queueLock() { portENTER_CRITICAL(&_queueLock); }
                void queueUnlock() { portEXIT_CRITICAL(&_queueLock); }
#else
                void queueLock() {} // RP2040: single-threaded, nothing to guard
                void queueUnlock() {}
#endif

#ifdef ARDUINO_ARCH_ESP32
                TaskHandle_t _task = nullptr;
                // Recursive: a subscription callback may call subscribe() while taskRun()
                // already holds it.
                SemaphoreHandle_t _mutex = nullptr;
                static void taskRun(void *arg);
                bool takeMutex(uint32_t timeoutMs = 100);
                void giveMutex();
#endif
            };

        } // namespace MQTT
    } // namespace Network
} // namespace OpenKNX

#endif // OPENKNX_MQTT
#endif // KNX_IP_WIFI || KNX_IP_LAN
