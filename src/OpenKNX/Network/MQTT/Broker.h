#pragma once
#if defined(KNX_IP_WIFI) || defined(KNX_IP_LAN)
#ifdef OPENKNX_MQTT

#include "Packet.h"
#include "OpenKNX/Helper.h" // PsramAllocator
#include <functional>
#include <map>
#include <string>
#include <vector>

#ifdef ARDUINO_ARCH_RP2040
#include <lwip/tcp.h>
#endif

#ifndef OPENKNX_MQTT_BROKER_MAX_CLIENTS
#if defined(ARDUINO_ARCH_ESP32)
#define OPENKNX_MQTT_BROKER_MAX_CLIENTS 5
#else
#define OPENKNX_MQTT_BROKER_MAX_CLIENTS 3
#endif
#endif

// Upper bounds to prevent unbounded heap growth from remote clients
#ifndef OPENKNX_MQTT_BROKER_MAX_RETAINED
#define OPENKNX_MQTT_BROKER_MAX_RETAINED 32
#endif
#ifndef OPENKNX_MQTT_BROKER_MAX_SUBS
#define OPENKNX_MQTT_BROKER_MAX_SUBS 16
#endif

namespace OpenKNX
{
    namespace Network
    {
        namespace MQTT
        {
            using MessageCallback = std::function<void(const char *topic, const void *payload, size_t len)>;

            struct RetainedMessage
            {
                std::string payload;
                uint8_t qos = 0;
            };

            struct BrokerSubscription
            {
                std::string topic;
                uint8_t qos;
            };

            class Broker
            {
              public:
                // Start listening on given port.
                // username/password empty = no authentication required.
                bool begin(uint16_t port,
                           const char *username = nullptr,
                           const char *password = nullptr);

                void loop();
                bool active() const { return _active; }

                // Publish from the broker itself to all matching subscribers
                bool publish(const char *topic, const void *payload, size_t len,
                             uint8_t qos = 0, bool retain = false);
                bool publish(const std::string &topic, const std::string &payload,
                             uint8_t qos = 0, bool retain = false);

                // Subscribe for messages received by the broker (local observer)
                void subscribe(const std::string &topic, MessageCallback callback);

              private:
                // Per-connection state
                struct Conn
                {
                    bool active = false;
                    bool connected = false; // CONNECT received and accepted
                    char clientId[24] = {};
                    uint32_t lastActivity = 0;
                    uint16_t keepAlive = 0;
                    std::vector<BrokerSubscription> subscriptions;

                    uint8_t rxBuf[512];
                    size_t rxLen = 0;

                    // Platform handle
#if defined(ARDUINO_ARCH_ESP32)
                    void *wifiClient = nullptr; // WiFiClient*
#elif defined(ARDUINO_ARCH_RP2040)
                    void *pcb = nullptr; // tcp_pcb*
#endif
                };

                void acceptNew();
                void serviceConn(Conn &c);
                void closeConn(Conn &c);
                void handlePacket(Conn &c, const Packet &pkt);
                void handleConnect(Conn &c, const Packet &pkt);
                void handleSubscribe(Conn &c, const Packet &pkt);
                void handleUnsubscribe(Conn &c, const Packet &pkt);
                void handlePublishPkt(Conn &c, const Packet &pkt);
                void handlePingReq(Conn &c);
                void handleDisconnect(Conn &c);
                void routePublish(const char *topic, const void *payload, size_t len,
                                  uint8_t qos, bool retain);
                bool sendToConn(Conn &c, const uint8_t *data, size_t len);
                bool checkAuth(const char *user, const char *pass) const;

                bool _active = false;
                uint16_t _port = 1883;
                char _username[24] = {};
                char _password[24] = {};
                bool _authRequired = false;

                Conn _conns[OPENKNX_MQTT_BROKER_MAX_CLIENTS];
                // Retained-Map-Knoten und Observer-Liste in PSRAM (loop-/task-Kontext,
                // kein lwIP-Callback-Zugriff). Keys/Payloads (std::string) bleiben im internen RAM.
                std::map<std::string, RetainedMessage, std::less<std::string>,
                         PsramAllocator<std::pair<const std::string, RetainedMessage>>> _retained;
                std::vector<std::pair<std::string, MessageCallback>,
                            PsramAllocator<std::pair<std::string, MessageCallback>>> _observers;

                // Shared transmit scratch buffer — avoids 512-byte stack allocations
                uint8_t _txBuf[512];

#if defined(ARDUINO_ARCH_ESP32)
                void *_server = nullptr; // WiFiServer*
                void readConn(Conn &c);
#elif defined(ARDUINO_ARCH_RP2040)
                void *_listenPcb = nullptr; // tcp_pcb*
                static Broker *_instance;
                static err_t lwipAccept(void *arg, struct tcp_pcb *newPcb, err_t err);
                static err_t lwipRecv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err);
                static void lwipErr(void *arg, err_t err);
                static err_t lwipPoll(void *arg, struct tcp_pcb *pcb);
                Conn *findConnByPcb(void *pcb);
                void readConn(Conn &c, struct pbuf *p);
#endif
            };

        } // namespace MQTT
    } // namespace Network
} // namespace OpenKNX

#endif // OPENKNX_MQTT
#endif // KNX_IP_WIFI || KNX_IP_LAN
