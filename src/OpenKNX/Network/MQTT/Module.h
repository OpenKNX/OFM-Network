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

                // Publish to absolute topic
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

                Client _client;
                Broker _broker;

#ifdef ARDUINO_ARCH_ESP32
                TaskHandle_t _task = nullptr;
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
