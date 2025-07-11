#pragma once
#if defined(KNX_IP_WIFI) || defined(KNX_IP_LAN)
#ifdef OPENKNX_MQTT
#ifdef ARDUINO_ARCH_ESP32
#include "OpenKNX.h"
#include <PicoMQTT.h>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#ifndef OPENKNX_MQTT_STATUS_TIME
#define OPENKNX_MQTT_STATUS_TIME 5000
#endif

namespace PicoMQTT
{
    class OpenKNXClient : public Client
    {
        using Client::Client;
    };

    class OpenKNXServer : public Server
    {
      public:
        OpenKNXServer(uint16_t port, const char *username, const char *password)
            : Server(port), _username(username), _password(password)
        {
        }

      protected:
        const char *_username;
        const char *_password;

        PicoMQTT::ConnectReturnCode auth(const char *client_id, const char *username, const char *password) override
        {
            logInfo("MQTTServer", "Authenticating client %s with username %s", client_id, username ? username : "NULL");

            // only accept connections if username and password are provided
            if (!username || !password)
            {
                return PicoMQTT::CRC_NOT_AUTHORIZED;
            }

            if (strcmp(username, _username) == 0 && strcmp(password, _password) == 0)
            {
                return PicoMQTT::CRC_ACCEPTED;
            }

            // // reject all other credentials
            return PicoMQTT::CRC_BAD_USERNAME_OR_PASSWORD;
        }
    };
}; // namespace PicoMQTT

class MqttModule : public OpenKNX::Module
{
    struct Subscription
    {
        std::string topic;
        std::function<void(const char *, const void *, size_t)> callback;
    };

  public:
    void setup(bool configured) override;
    void loop(bool configured) override;
    const std::string name() override { return "Network<MQTT>"; }
    const std::string version() override { return "0.0.0"; } // Dummy because Submodule
    std::string devicePrefix();
    bool active();
    bool connected();
    const char *getServer();
    const char *getUsername();
    const char *getPassword();
    const char *getPrefix();
    const bool getMode();
    const uint16_t getPort();

    bool publish(const char *topic, const char *payload, const size_t size, uint8_t qos = 0, bool retain = false, uint16_t message_id = 0);
    bool publish(std::string topic, std::string payload, uint8_t qos = 0, bool retain = false, uint16_t message_id = 0);
    bool publishP(std::string topic, std::string payload, uint8_t qos = 0, bool retain = false, uint16_t message_id = 0);

    void subscribe(const std::string topic, std::function<void(const char *, const void *, size_t)> callback);

  private:
    void processIncomingMessage(const char *topic, const void *payload, size_t payload_size);

    char _devicePrefix[29] = ""; // 8 (openknx/) + 20 ETS Prefix + 1 (\0)
    uint32_t _lastStatus = 0;
    bool _initialized = false;
    PicoMQTT::Client *_client = nullptr;
    PicoMQTT::Server *_server = nullptr;
    PicoMQTT::SubscribedMessageListener *_subscriber = nullptr;
    PicoMQTT::Publisher *_publisher = nullptr;
    std::vector<Subscription> _subscriptions;
};

#else
#pragma warn "MQTT is only supported on ESP32 architecture. Please remove OPENKNX_MQTT from your configuration."
#endif
#endif
#endif