#pragma once
#if defined(KNX_IP_WIFI) || defined(KNX_IP_LAN)
#ifdef OPENKNX_MQTT

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef OPENKNX_MQTT_TXBUF_SIZE
#define OPENKNX_MQTT_TXBUF_SIZE 512
#endif

namespace OpenKNX
{
    namespace Network
    {
        namespace MQTT
        {
            // MQTT 3.1.1 packet type nibbles (upper 4 bits of first byte)
            enum PacketType : uint8_t
            {
                PT_CONNECT = 0x10,
                PT_CONNACK = 0x20,
                PT_PUBLISH = 0x30,
                PT_PUBACK = 0x40,
                PT_PUBREC = 0x50,
                PT_PUBREL = 0x60,
                PT_PUBCOMP = 0x70,
                PT_SUBSCRIBE = 0x80,
                PT_SUBACK = 0x90,
                PT_UNSUBSCRIBE = 0xA0,
                PT_UNSUBACK = 0xB0,
                PT_PINGREQ = 0xC0,
                PT_PINGRESP = 0xD0,
                PT_DISCONNECT = 0xE0,
            };

            // CONNACK return codes
            enum ConnectReturnCode : uint8_t
            {
                CRC_ACCEPTED = 0x00,
                CRC_REFUSED_PROTOCOL = 0x01,
                CRC_REFUSED_ID_REJECTED = 0x02,
                CRC_REFUSED_SERVER = 0x03,
                CRC_REFUSED_CREDENTIALS = 0x04,
                CRC_REFUSED_UNAUTHORIZED = 0x05,
            };

            struct WillOptions
            {
                const char *topic = nullptr;
                const char *payload = nullptr;
                uint8_t qos = 0;
                bool retain = false;
            };

            struct ConnectOptions
            {
                const char *clientId = nullptr;
                const char *username = nullptr;
                const char *password = nullptr;
                uint16_t keepAlive = 60;
                WillOptions will;
            };

            // Parsed incoming packet (zero-copy — pointers into caller's buffer)
            struct Packet
            {
                uint8_t type;           // PacketType value
                uint8_t flags;          // lower 4 bits of first byte
                const uint8_t *payload; // points into source buffer
                size_t payloadLen;
            };

            // ----------------------------------------------------------------
            // Serialization helpers — write into caller-supplied buffer.
            // Return number of bytes written, or 0 on buffer overflow.
            // ----------------------------------------------------------------

            // Write MQTT variable-length integer encoding
            size_t writeVarInt(uint8_t *buf, size_t bufLen, uint32_t value);

            // Write 2-byte big-endian length-prefixed string
            size_t writeString(uint8_t *buf, size_t bufLen, const char *str);

            size_t buildConnect(uint8_t *buf, size_t bufLen, const ConnectOptions &opts);
            size_t buildPublish(uint8_t *buf, size_t bufLen,
                                const char *topic, const void *payload, size_t payloadLen,
                                uint8_t qos, bool retain, uint16_t msgId);
            size_t buildSubscribe(uint8_t *buf, size_t bufLen,
                                  const char *topic, uint8_t qos, uint16_t msgId);
            size_t buildUnsubscribe(uint8_t *buf, size_t bufLen,
                                    const char *topic, uint16_t msgId);
            size_t buildPubAck(uint8_t *buf, size_t bufLen, uint16_t msgId);
            size_t buildPubRel(uint8_t *buf, size_t bufLen, uint16_t msgId);
            size_t buildPubComp(uint8_t *buf, size_t bufLen, uint16_t msgId);
            size_t buildPingReq(uint8_t *buf, size_t bufLen);
            size_t buildDisconnect(uint8_t *buf, size_t bufLen);

            // ----------------------------------------------------------------
            // Parsing — zero-copy, works on any receive buffer.
            // Returns true and fills `out` when a complete packet is available.
            // `consumed` is set to the number of bytes that belong to this packet.
            // ----------------------------------------------------------------
            bool parsePacket(const uint8_t *data, size_t len,
                             Packet &out, size_t &consumed);

            // Decode a variable-length integer; returns false if data is incomplete.
            bool readVarInt(const uint8_t *data, size_t len,
                            uint32_t &value, size_t &consumed);

            // MQTT topic wildcard matching ('+' and '#')
            bool topicMatches(const char *pattern, const char *topic);

            // Shared TX scratch buffer for Broker and Client -- Module runs only one
            // of the two at a time (see Module::cfgBroker()), so one buffer covers
            // both instead of each holding its own. Allocated once in Module::setup(),
            // only if MQTT is actually enabled at runtime -- stays nullptr otherwise.
            extern uint8_t *mqttTxBuf;

            // Read big-endian uint16 from buffer
            inline uint16_t readU16(const uint8_t *p) { return (uint16_t)(p[0] << 8) | p[1]; }

            // Write big-endian uint16 into buffer
            inline void writeU16(uint8_t *p, uint16_t v)
            {
                p[0] = v >> 8;
                p[1] = v & 0xFF;
            }

        } // namespace MQTT
    } // namespace Network
} // namespace OpenKNX

#endif // OPENKNX_MQTT
#endif // KNX_IP_WIFI || KNX_IP_LAN
