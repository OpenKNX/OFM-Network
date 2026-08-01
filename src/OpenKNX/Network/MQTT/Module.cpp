#if defined(KNX_IP_WIFI) || defined(KNX_IP_LAN)
#ifdef OPENKNX_MQTT

#include "Module.h"
#include "../Module.h" // for openknxNetwork

namespace OpenKNX
{
    namespace Network
    {
        namespace MQTT
        {
            void Module::setup(bool configured)
            {
                if (_initialized) return;
                if (!configured || !active()) return;

                logInfoP("Setting up MQTT");
                logIndentUp();
                logInfoP("Mode: %s", cfgBroker() ? "Broker" : "Client");
                logInfoP("Port: %u", cfgPort());
                if (!cfgBroker()) logInfoP("Server: %s", cfgServer());
                logInfoP("Prefix: %s", devicePrefix().c_str());
                logIndentDown();

                if (cfgBroker())
                {
                    _initialized = _broker.begin(cfgPort(), cfgUsername(), cfgPassword());
                }
                else
                {
                    if (!cfgServer()[0])
                    {
                        logErrorP("MQTT server not configured");
                        return;
                    }

                    _client.setWill((devicePrefix() + "status").c_str(), "0", true, 1);

                    _initialized = _client.begin(cfgServer(), cfgPort(),
                                                 openknxNetwork.hostName(),
                                                 cfgUsername(), cfgPassword());
                }

#if MASK_VERSION == 0x07B0
                if (ParamNET_MQTTTPRawData && !cfgBroker())
                {
                    knx.bau().getDataLinkLayer()->getTPUart().registerReceivedFrame(
                        [](TPUart::Frame &frame) {
                            openknxNetwork.mqtt.publishP("knxtp/raw", frame.data(), frame.size());
                        });
                }
#endif

#ifdef ARDUINO_ARCH_ESP32
                if (_initialized)
                {
                    _mutex = xSemaphoreCreateMutex();
                    if (_mutex)
                        xTaskCreatePinnedToCore(taskRun, "mqtt", OPENKNX_MQTT_TASK_STACK,
                                                this, 5, &_task, 0);
                }
#endif
            }

            void Module::loop(bool configured)
            {
#ifdef ARDUINO_ARCH_ESP32
                // Broker/client loop runs in its own FreeRTOS task (taskRun, Core 0).
                // Only publish periodic status from here since publishP() is mutex-guarded.
                if (!configured || !_initialized) return;
                if (connected() && delayCheck(_lastStatus, OPENKNX_MQTT_STATUS_TIME))
                {
                    _lastStatus = millis();
                    publishP("status", "1");
                    publishP("uptime", std::to_string(uptime()));
                }
#else
                if (!configured || !_initialized) return;

                if (cfgBroker())
                    _broker.loop();
                else
                    _client.loop();

                if (connected() && delayCheck(_lastStatus, OPENKNX_MQTT_STATUS_TIME))
                {
                    _lastStatus = millis();
                    publishP("status", "1");
                    publishP("uptime", std::to_string(uptime()));
                }
#endif
            }

#ifdef ARDUINO_ARCH_ESP32
            void Module::taskRun(void *arg)
            {
                Module *self = static_cast<Module *>(arg);
                for (;;)
                {
                    if (xSemaphoreTake(self->_mutex, portMAX_DELAY) == pdTRUE)
                    {
                        if (self->cfgBroker())
                            self->_broker.loop();
                        else
                            self->_client.loop();
                        xSemaphoreGive(self->_mutex);
                    }
                    vTaskDelay(pdMS_TO_TICKS(5));
                }
            }

            bool Module::takeMutex(uint32_t timeoutMs)
            {
                return _mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
            }

            void Module::giveMutex()
            {
                if (_mutex) xSemaphoreGive(_mutex);
            }
#endif

            bool Module::active() const
            {
                return ParamNET_MQTT;
            }

            bool Module::connected() const
            {
                if (!_initialized) return false;
                return cfgBroker() ? _broker.active() : _client.connected();
            }

            std::string Module::devicePrefix()
            {
                if (_devicePrefix[0] != '\0') return _devicePrefix;

                memcpy(_devicePrefix, "openknx/", 8);
                const char *prefix = cfgPrefix();
                if (knx.configured() && prefix[0] != '\0')
                    memcpy(_devicePrefix + 8, prefix, strlen(prefix));
                else
                    memcpy(_devicePrefix + 8, openknx.info.humanSerialNumber().c_str() + 5, 8);

                size_t len = strlen(_devicePrefix);
                if (len < sizeof(_devicePrefix) - 1)
                    _devicePrefix[len] = '/';

                return _devicePrefix;
            }

            bool Module::publish(const char *topic, const void *payload, size_t len,
                                 uint8_t qos, bool retain)
            {
                if (!connected()) return false;
#ifdef ARDUINO_ARCH_ESP32
                if (!takeMutex()) return false;
#endif
                bool result = cfgBroker() ? _broker.publish(topic, payload, len, qos, retain)
                                          : _client.publish(topic, payload, len, qos, retain);
#ifdef ARDUINO_ARCH_ESP32
                giveMutex();
#endif
                return result;
            }

            bool Module::publish(const std::string &topic, const std::string &payload,
                                 uint8_t qos, bool retain)
            {
                return publish(topic.c_str(), payload.c_str(), payload.size(), qos, retain);
            }

            bool Module::publishP(const std::string &topic, const std::string &payload,
                                  uint8_t qos, bool retain)
            {
                return publish(devicePrefix() + topic, payload, qos, retain);
            }

            bool Module::publishP(const std::string &topic, const void *payload, size_t len,
                                  uint8_t qos, bool retain)
            {
                return publish((devicePrefix() + topic).c_str(), payload, len, qos, retain);
            }

            void Module::subscribe(const std::string &topic,
                                   std::function<void(const char *, const void *, size_t)> callback,
                                   uint8_t qos)
            {
#ifdef ARDUINO_ARCH_ESP32
                takeMutex(portMAX_DELAY);
#endif
                if (cfgBroker())
                    _broker.subscribe(topic, callback);
                else
                    _client.subscribe(topic, qos, callback);
#ifdef ARDUINO_ARCH_ESP32
                giveMutex();
#endif
            }

        } // namespace MQTT
    } // namespace Network
} // namespace OpenKNX

#endif // OPENKNX_MQTT
#endif // KNX_IP_WIFI || KNX_IP_LAN
