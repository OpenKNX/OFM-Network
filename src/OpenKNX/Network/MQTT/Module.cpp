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

                if (!mqttTxReserve(OPENKNX_MQTT_TXBUF_MIN))
                {
                    logErrorP("MQTT: out of memory for TX buffer");
                    return; // transient -- heap may free up, worth retrying
                }

                logInfoP("Setting up MQTT");
                logIndentUp();
                logInfoP("Mode: %s", cfgBroker() ? "Broker" : "Client");
                logInfoP("Port: %u", cfgPort());
                if (!cfgBroker()) logInfoP("Server: %s", cfgServer());
                logInfoP("Prefix: %s", devicePrefix().c_str());
                logIndentDown();

                if (cfgBroker())
                {
                    if (!_broker.begin(cfgPort(), cfgUsername(), cfgPassword()))
                        logErrorP("broker not started, MQTT stays inactive until restart");
                }
                else
                {
                    _client.setWill((devicePrefix() + "status").c_str(), "0", true, 1);

                    _client.begin(cfgServer(), cfgPort(), openknxNetwork.hostName(),
                                  cfgUsername(), cfgPassword());
                }

                // Setup is done either way: an inactive broker no-ops in loop(), and a
                // client that could not connect retries from Client::loop(). Leaving this
                // false would re-run setup() -- and its log block -- on every tick.
                _initialized = true;

#ifdef ARDUINO_ARCH_ESP32
                // Nothing drives broker/client without this task, so a failure here is
                // not silent -- connected() would just stay false forever.
                _mutex = xSemaphoreCreateRecursiveMutex();
                if (!_mutex ||
                    xTaskCreatePinnedToCore(taskRun, "mqtt", OPENKNX_MQTT_TASK_STACK,
                                            this, 5, &_task, 0) != pdPASS)
                    logErrorP("MQTT task not started, MQTT stays inactive until restart");
#endif
            }

            void Module::loop(bool configured)
            {
                if (!configured || !_initialized) return;

#ifndef ARDUINO_ARCH_ESP32
                // ESP32 runs broker/client and drainQueue() in taskRun (Core 0) instead.
                if (cfgBroker())
                    _broker.loop();
                else
                    _client.loop();

                refreshConnected();
                drainQueue();
#endif

                if (connected() && delayCheck(_lastStatus, OPENKNX_MQTT_STATUS_TIME))
                {
                    _lastStatus = millis();
                    publishP("status", "1");
                    publishP("uptime", std::to_string(uptime()));

                    // Gebündelt statt pro Vorfall -- sonst wäre das Log die eigentliche Last.
                    if (_queueDropped)
                    {
                        logErrorP("%u messages dropped (queue full or out of memory)", _queueDropped);
                        _queueDropped = 0;
                    }
                }
            }

#ifdef ARDUINO_ARCH_ESP32
            void Module::taskRun(void *arg)
            {
                Module *self = static_cast<Module *>(arg);
                for (;;)
                {
                    if (xSemaphoreTakeRecursive(self->_mutex, portMAX_DELAY) == pdTRUE)
                    {
                        if (self->cfgBroker())
                            self->_broker.loop();
                        else
                            self->_client.loop();
                        // Inside the lock -- that is what guards the broker/client access.
                        self->refreshConnected();
                        self->drainQueue();
                        xSemaphoreGiveRecursive(self->_mutex);
                    }
                    vTaskDelay(pdMS_TO_TICKS(5));
                }
            }

            bool Module::takeMutex(uint32_t timeoutMs)
            {
                return _mutex && xSemaphoreTakeRecursive(_mutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
            }

            void Module::giveMutex()
            {
                if (_mutex) xSemaphoreGiveRecursive(_mutex);
            }
#endif

            bool Module::active() const
            {
                if (!ParamNET_MQTT) return false;

                // A broker needs no server; a client without one is simply not set up, not broken.
                return cfgBroker() || cfgServer()[0] != '\0';
            }

            bool Module::connected() const
            {
                return _connected;
            }

            void Module::refreshConnected()
            {
                _connected = _initialized && (cfgBroker() ? _broker.active() : _client.connected());
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
                return enqueue(topic, payload, len, qos, retain);
            }

            bool Module::enqueue(const char *topic, const void *payload, size_t len,
                                 uint8_t qos, bool retain)
            {
                if (_queueCount >= OPENKNX_MQTT_MAX_QUEUE) // fast path, re-checked under the lock
                {
                    _queueDropped++;
                    return false;
                }

                // Beyond the ceiling the buffer can never grow far enough, so buildPublish()
                // would fail forever in drainQueue() and block the head.
                const size_t topicLen = strlen(topic);
                if (publishSize(topicLen, len) > OPENKNX_MQTT_TXBUF_MAX)
                {
                    logErrorP("message too large for OPENKNX_MQTT_TXBUF_MAX (%u + %u B)",
                              (unsigned)topicLen, (unsigned)len);
                    return false;
                }

                QueueEntry *e = (QueueEntry *)PSRAM_MALLOC(sizeof(QueueEntry) + topicLen + 1 + len);
                if (!e)
                {
                    _queueDropped++;
                    return false;
                }
                e->next = nullptr;
                e->topicLen = (uint16_t)topicLen;
                e->payloadLen = (uint16_t)len;
                e->qos = qos;
                e->retain = retain;

                char *dst = (char *)(e + 1);
                memcpy(dst, topic, topicLen + 1);
                if (len) memcpy(dst + topicLen + 1, payload, len);

                queueLock();
                if (_queueCount >= OPENKNX_MQTT_MAX_QUEUE) // another producer got there first
                {
                    queueUnlock();
                    free(e);
                    _queueDropped++;
                    return false;
                }
                if (_queueTail)
                    _queueTail->next = e;
                else
                    _queueHead = e;
                _queueTail = e;
                _queueCount++;
                queueUnlock();

                return true;
            }

            void Module::clearQueue()
            {
                // Unlink under the lock, free outside it.
                queueLock();
                QueueEntry *e = _queueHead;
                _queueHead = _queueTail = nullptr;
                _queueCount = 0;
                queueUnlock();

                while (e)
                {
                    QueueEntry *next = e->next;
                    free(e);
                    e = next;
                }
            }

            void Module::drainQueue()
            {
                // No timestamp in the payload, so a post-reconnect flush would look fresh.
                if (!connected())
                {
                    clearQueue();
                    return;
                }

                for (;;)
                {
                    queueLock();
                    QueueEntry *e = _queueHead;
                    queueUnlock();
                    if (!e) break;

                    // Grow the shared TX buffer here and nowhere else: enqueue() runs in
                    // other tasks and would pull it out from under the MQTT send path.
                    if (!mqttTxReserve(publishSize(e->topicLen, e->payloadLen)))
                    {
                        _queueDropped++; // dropping beats stalling the head forever
                        unlinkHead(e);
                        continue;
                    }

                    // Kept on failure: a full send buffer frees up again, retry beats losing it.
                    const bool sent = cfgBroker()
                                          ? _broker.publish(e->topic(), e->payload(), e->payloadLen, e->qos, e->retain)
                                          : _client.publish(e->topic(), e->payload(), e->payloadLen, e->qos, e->retain);
                    if (!sent) break;

                    unlinkHead(e);
                }
            }

            void Module::unlinkHead(QueueEntry *e)
            {
                queueLock();
                _queueHead = e->next;
                if (!_queueHead) _queueTail = nullptr;
                _queueCount--;
                queueUnlock();

                free(e);
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
