#pragma once
#if defined(KNX_IP_WIFI) || defined(KNX_IP_LAN)
#ifdef OPENKNX_MQTT

#include "Packet.h"
#include <functional>
#include <string>
#include <vector>

#ifdef ARDUINO_ARCH_RP2040
#include <lwip/tcp.h>
#endif

namespace OpenKNX
{
    namespace Network
    {
        namespace MQTT
        {
            using MessageCallback = std::function<void(const char *topic, const void *payload, size_t len)>;

            struct Subscription
            {
                std::string topic;
                uint8_t qos;
                MessageCallback callback;
            };

            class Client
            {
              public:
                // Call before setup(). Must be called while not connected.
                void setWill(const char *topic, const char *payload,
                             bool retain = true, uint8_t qos = 1);

                // Begin connecting. Non-blocking — call loop() repeatedly.
                bool begin(const char *host, uint16_t port,
                           const char *clientId,
                           const char *username, const char *password,
                           uint16_t keepAliveSeconds = 60);

                void loop();
                bool connected() const { return _state == STATE_READY; }

                bool publish(const char *topic, const void *payload, size_t len,
                             uint8_t qos = 0, bool retain = false);
                bool publish(const std::string &topic, const std::string &payload,
                             uint8_t qos = 0, bool retain = false);

                void subscribe(const std::string &topic, uint8_t qos,
                               MessageCallback callback);

                void disconnect();

              private:
                enum State : uint8_t
                {
                    STATE_DISCONNECTED,
                    STATE_CONNECTING,
                    STATE_CONNECTED, // TCP up, CONNECT sent, waiting CONNACK
                    STATE_READY,     // CONNACK received, subscriptions pending/done
                    STATE_RECONNECTING,
                };

                void sendConnect();
                void sendPendingSubscriptions();
                void sendPingReq();
                bool sendRaw(const uint8_t *data, size_t len);
                void processPacket(const Packet &pkt);
                void handlePublish(const Packet &pkt);
                void handleConnAck(const Packet &pkt);
                void reconnect();

                // Platform TCP interface (implemented in .cpp per platform)
                bool tcpConnect(const char *host, uint16_t port);
                void tcpDisconnect();
                bool tcpConnected();
                int tcpRead(uint8_t *buf, size_t maxLen); // returns bytes read, -1 on error
                bool tcpWrite(const uint8_t *data, size_t len);

                State _state = STATE_DISCONNECTED;
                char _host[64] = {};
                uint16_t _port = 1883;
                char _clientId[24] = {};
                char _username[24] = {};
                char _password[24] = {};
                uint16_t _keepAlive = 60;
                uint16_t _msgId = 1;

                char _willTopic[64] = {};
                char _willPayload[64] = {};
                bool _willRetain = false;
                uint8_t _willQos = 0;

                uint32_t _lastPing = 0;
                uint32_t _lastActivity = 0;
                uint32_t _reconnectTimer = 0;
                uint8_t _reconnectCount = 0;

                // Receive ring buffer (handles packet fragmentation)
                static const size_t RX_BUF_SIZE = 1024;
                uint8_t _rxBuf[RX_BUF_SIZE];
                size_t _rxLen = 0;

                // Shared transmit scratch buffer — avoids 512-byte stack allocations
                uint8_t _txBuf[512];

                std::vector<Subscription> _subscriptions;
                size_t _subscribedUpTo = 0; // index into _subscriptions for pending sends

#if defined(ARDUINO_ARCH_ESP32)
                // ESP32: Arduino WiFiClient
                void *_wifiClient = nullptr; // WiFiClient* — void* to avoid including WiFi.h here
#elif defined(ARDUINO_ARCH_RP2040)
                // RP2040: lwIP tcp_pcb
                void *_pcb = nullptr;
                // Reassembly buffer managed via lwIP callbacks (static trampoline)
                static Client *_instance; // single-instance for callback routing
                static err_t lwipRecv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err);
                static void lwipErr(void *arg, err_t err);
                static err_t lwipConnected(void *arg, struct tcp_pcb *pcb, err_t err);
                static err_t lwipPoll(void *arg, struct tcp_pcb *pcb);
                bool _tcpConnectPending = false;
#endif
            };

        } // namespace MQTT
    } // namespace Network
} // namespace OpenKNX

#endif // OPENKNX_MQTT
#endif // KNX_IP_WIFI || KNX_IP_LAN
