/**
 * @file MqttModule.cpp
 * @brief Implementation of the MqttModule class for OpenKNX MQTT integration.
 *
 * This module provides MQTT client and broker functionality for OpenKNX devices,
 * supporting both WiFi and LAN configurations on ESP32 platforms. It handles
 * MQTT setup, publishing, subscribing, and message processing, including device
 * status reporting and integration with KNX TP raw data (when enabled).
 *
 * Main Features:
 * - Dynamic device topic prefix generation based on configuration or serial number.
 * - MQTT broker or client mode selection.
 * - Secure connection with username and password.
 * - Last Will and Testament (LWT) configuration for device status.
 * - Subscription management with callback support.
 * - Periodic publishing of device status and uptime.
 * - Optional publishing of KNX TP raw data frames.
 *
 * @note Requires PicoMQTT library and OpenKNX framework.
 */
#if defined(KNX_IP_WIFI) || defined(KNX_IP_LAN)
#ifdef OPENKNX_MQTT
#ifdef ARDUINO_ARCH_ESP32
#include "NetworkModule.h"

/**
 * @brief Constructs and returns the MQTT device prefix string.
 *
 * This function initializes the internal device prefix string if it has not been set.
 * The prefix is constructed as follows:
 * - Starts with "openknx/".
 * - Appends the result of getPrefix() if it is not empty.
 * - Otherwise, appends 8 characters from the human serial number (starting at offset 5).
 * - Appends a trailing '/' character.
 *
 * @return The constructed device prefix as a std::string.
 */
std::string MqttModule::devicePrefix()
{
    if (_devicePrefix[0] == '\0') // if not initialized
    {
        memcpy(_devicePrefix, "openknx/", 8);
        if (knx.configured() && getPrefix()[0] == '\0')
            memcpy(_devicePrefix + 8, getPrefix(), strlen(getPrefix()));
        else
            memcpy(_devicePrefix + 8, openknx.info.humanSerialNumber().c_str() + 5, 8);

        memcpy(_devicePrefix + strlen(_devicePrefix), "/", 1);
    }

    return _devicePrefix;
}

/**
 * @brief Initializes and sets up the MQTT module based on configuration.
 *
 * This function configures the MQTT module as either a broker or client depending on the value of ParamNET_MQTTMode.
 * It logs relevant information about the MQTT setup, including mode, username, and topic prefix.
 * If configured as a broker, it creates and starts an MQTT server. If configured as a client, it checks for server
 * configuration, sets up Last Will and Testament (LWT), and starts the MQTT client.
 * The function also subscribes to all MQTT topics and binds incoming messages to the processIncomingMessage handler.
 * For specific MASK_VERSION and configuration, it registers a handler to publish raw KNX TP-UART frames to MQTT.
 *
 * @param configured Indicates whether the MQTT module is configured and should be set up.
 */
void MqttModule::setup(bool configured)
{
    if (!configured) return; // not configured?
    if (!active()) return;   // MQTT disabled?

    logInfoP("Setting up MQTT");
    logIndentUp();
    if (ParamNET_MQTTMode == 1)
        logInfoP("Mode: Broker (%i)", getPort());
    else
        logInfoP("Mode: Client (%s:%i)", getServer(), getPort());

    logInfoP("Username: %s", getUsername());
    logInfoP("Prefix: %s", devicePrefix().c_str());
    logIndentDown();

    if (ParamNET_MQTTMode == 1)
    {
        _server = new PicoMQTT::OpenKNXServer(getPort(), getUsername(), getPassword());
        _server->begin();
        _subscriber = _server;
        _publisher = _server;
        _initialized = true;
    }
    else
    {
        if (strlen(getServer()) == 0)
        {
            logErrorP("MQTT server is not configured.");
            return;
        }

        _client = new PicoMQTT::OpenKNXClient(getServer(), getPort(), openknxNetwork.getHostname(), getUsername(), getPassword());

        // LWT
        _client->will.topic = String((devicePrefix() + "status").c_str());
        _client->will.retain = true;
        _client->will.payload = "0";
        _client->will.qos = 2;

        _client->begin();
        _subscriber = _client;
        _publisher = _client;
        _initialized = true;
    }

    _subscriber->subscribe("#", std::bind(&MqttModule::processIncomingMessage, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));

#if MASK_VERSION == 0x07B0
    if (ParamNET_MQTTTPRawData)
    {
        knx.bau().getDataLinkLayer()->getTPUart().registerReceivedFrame([](TPUart::Frame &frame) -> void {
            openknxNetwork.mqtt.publishP("knxtp/raw", frame.data(), frame.size());
        });
    }
#endif
}

/**
 * @brief Main loop function for the MqttModule.
 *
 * This function should be called regularly to handle MQTT communication and device status updates.
 * It performs the following actions:
 * - Checks if the module is initialized; returns immediately if not.
 * - If connected to the MQTT broker and the status update interval has elapsed,
 *   publishes the device status and uptime.
 * - Calls the loop function of the MQTT client and server, if they are initialized.
 *
 * @param configured Indicates whether the module is configured (currently unused).
 */
void MqttModule::loop(bool configured)
{
    if (!_initialized) return; // not configured?
    // if (!openknxNetwork.established()) return; // connected to network?

    // Device status & uptime
    if (connected() && delayCheck(_lastStatus, OPENKNX_MQTT_STATUS_TIME))
    {
        _lastStatus = millis();
        publishP("status", "1");
        publishP("uptime", std::to_string(uptime()));
    }

    if (_client != nullptr) _client->loop();
    if (_server != nullptr) _server->loop();
}

/**
 * @brief Publishes a message to a specified MQTT topic.
 *
 * This function sends a payload to the given MQTT topic with the specified Quality of Service (QoS),
 * retain flag, and message ID. It first checks if the module is initialized and connected to the MQTT broker.
 *
 * @param topic        The MQTT topic to publish to.
 * @param payload      The message payload to send.
 * @param size         The size of the payload in bytes.
 * @param qos          The Quality of Service level (0, 1, or 2).
 * @param retain       If true, the message will be retained by the broker.
 * @param message_id   The identifier for the MQTT message.
 * @return true if the message was published successfully; false otherwise.
 */
bool MqttModule::publish(const char *topic, const char *payload, const size_t size, uint8_t qos, bool retain, uint16_t message_id)
{
    if (!_initialized) return false; // initialized?
    if (!connected()) return false;  // connected to MQTT broker?

    logTraceP("Publishing to topic %s", topic);
    return _publisher->publish(topic, payload, size, qos, retain, message_id);
}

/**
 * @brief Publishes a message to the specified MQTT topic.
 *
 * This overload accepts topic and payload as std::string objects and forwards the call
 * to the underlying publish method that takes C-style strings.
 *
 * @param topic The MQTT topic to publish to.
 * @param payload The message payload to send.
 * @param qos The Quality of Service level for message delivery (0, 1, or 2).
 * @param retain If true, the message will be retained by the broker.
 * @param message_id The identifier for the MQTT message.
 * @return true if the message was published successfully, false otherwise.
 */
bool MqttModule::publish(std::string topic, std::string payload, uint8_t qos, bool retain, uint16_t message_id)
{
    return publish(topic.c_str(), payload.c_str(), payload.size(), qos, retain, message_id);
}

/**
 * @brief Publishes a message to an MQTT topic with a device-specific prefix.
 *
 * This function constructs the full topic by prepending the device prefix to the provided topic,
 * then publishes the given payload to that topic with the specified QoS, retain flag, and message ID.
 *
 * @param topic The MQTT topic (without device prefix) to publish to.
 * @param payload The message payload to send.
 * @param qos The Quality of Service level for the message (0, 1, or 2).
 * @param retain Whether the message should be retained by the broker.
 * @param message_id The identifier for the MQTT message.
 * @return true if the message was published successfully, false otherwise.
 */
bool MqttModule::publishP(std::string topic, std::string payload, uint8_t qos, bool retain, uint16_t message_id)
{
    return publish((devicePrefix() + topic).c_str(), payload.c_str(), payload.size(), qos, retain, message_id);
}

const char *MqttModule::getServer()
{
    return (char *)ParamNET_MQTTServer;
}

const char *MqttModule::getUsername()
{
    return (char *)ParamNET_MQTTUsername;
}

const char *MqttModule::getPassword()
{
    return (char *)ParamNET_MQTTPassword;
}

const char *MqttModule::getPrefix()
{
    return (char *)ParamNET_MQTTPrefix;
}

const uint16_t MqttModule::getPort()
{
    return ParamNET_MQTTPort; // Default MQTT port
}

bool MqttModule::active()
{
    return ParamNET_MQTT;
}

/**
 * @brief Returns the current MQTT mode.
 *
 * This function checks the value of ParamNET_MQTTMode to determine the MQTT mode.
 * @return true if the module is operating as a Broker, false if operating as a Client.
 */
const bool MqttModule::getMode()
{
    return ParamNET_MQTTMode == 1; // true for Broker, false for Client
}

/**
 * @brief Checks the connection status of the MQTT module.
 *
 * This method determines whether the MQTT module is currently connected,
 * based on its initialization state, operating mode, and the status of
 * its server or client objects.
 *
 * @return true if the module is initialized and connected (either as a server or client),
 *         false otherwise.
 */
bool MqttModule::connected()
{
    if (!_initialized) return false; // not initialized?

    if (getMode() && _server != nullptr) return true;
    if (!getMode()) return _client != nullptr && _client->connected();

    return false;
}

/**
 * @brief Subscribes to a specified MQTT topic with a callback function.
 *
 * Registers a callback to be invoked when a message is received on the given topic.
 *
 * Sample topics for subscription:
 *
 * Topics without wildcards:
 *   - openknx/device/status
 *   - openknx/device/uptime
 *
 * Topics with wildcards:
 *   - openknx/+/status
 *   - openknx/#
 *
 * @param topic The MQTT topic to subscribe to (supports wildcards '#' and '+').
 * @param callback The function to call when a message is received. The callback receives:
 *        - const char *: The topic name.
 *        - const void *: The message payload.
 *        - size_t: The size of the payload.
 */
void MqttModule::subscribe(const std::string topic, std::function<void(const char *, const void *, size_t)> callback)
{
    _subscriptions.push_back({topic, callback});
}

void MqttModule::processIncomingMessage(const char *topic, const void *payload, size_t payload_size)
{
    logTraceP("Processing message in topic %s (%u)", topic, payload_size);
    // logIndentUp();
    // logHexTraceP((const uint8_t *)payload, payload_size);
    // logIndentDown();

    for (auto &subscription : _subscriptions)
    {
        if (PicoMQTT::Subscriber::topic_matches(subscription.topic.c_str(), topic))
        {
            subscription.callback(topic, payload, payload_size);
        }
    }
}

#endif
#endif
#endif