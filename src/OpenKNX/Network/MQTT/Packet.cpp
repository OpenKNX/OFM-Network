#if defined(KNX_IP_WIFI) || defined(KNX_IP_LAN)
#ifdef OPENKNX_MQTT

#include "Packet.h"

namespace OpenKNX
{
    namespace Network
    {
        namespace MQTT
        {
            uint8_t *mqttTxBuf = nullptr;

            // Variable-length integer (MQTT 2.2.3)

            size_t writeVarInt(uint8_t *buf, size_t bufLen, uint32_t value)
            {
                size_t i = 0;
                do
                {
                    if (i >= bufLen) return 0;
                    uint8_t byte = value & 0x7F;
                    value >>= 7;
                    buf[i++] = (value > 0) ? (byte | 0x80) : byte;
                }
                while (value > 0);
                return i;
            }

            bool readVarInt(const uint8_t *data, size_t len, uint32_t &value, size_t &consumed)
            {
                value = 0;
                consumed = 0;
                uint32_t multiplier = 1;
                uint8_t byte;
                do
                {
                    if (consumed >= len) return false;
                    byte = data[consumed++];
                    value += (byte & 0x7F) * multiplier;
                    multiplier <<= 7;
                    if (multiplier > 0x200000) return false; // malformed
                }
                while (byte & 0x80);
                return true;
            }

            // String: 2-byte big-endian length + bytes (no NUL terminator)

            size_t writeString(uint8_t *buf, size_t bufLen, const char *str)
            {
                if (!str) str = "";
                size_t slen = strlen(str);
                if (bufLen < 2 + slen) return 0;
                writeU16(buf, (uint16_t)slen);
                memcpy(buf + 2, str, slen);
                return 2 + slen;
            }

            size_t buildConnect(uint8_t *buf, size_t bufLen, const ConnectOptions &opts)
            {
                // Variable header: protocol name, level, flags, keepAlive
                uint8_t vh[10];
                vh[0] = 0x00;
                vh[1] = 0x04;
                vh[2] = 'M';
                vh[3] = 'Q';
                vh[4] = 'T';
                vh[5] = 'T';
                vh[6] = 0x04; // protocol level 3.1.1

                uint8_t flags = 0x02; // clean session
                if (opts.username && opts.username[0]) flags |= 0x80;
                if (opts.password && opts.password[0]) flags |= 0x40;
                if (opts.will.topic && opts.will.topic[0])
                {
                    flags |= 0x04;
                    flags |= (opts.will.qos & 0x03) << 3;
                    if (opts.will.retain) flags |= 0x20;
                }
                vh[7] = flags;
                writeU16(vh + 8, opts.keepAlive);

                // Payload
                uint8_t payload[512];
                size_t plen = 0;
                auto addStr = [&](const char *s) -> bool {
                    size_t n = writeString(payload + plen, sizeof(payload) - plen, s);
                    if (!n) return false;
                    plen += n;
                    return true;
                };

                if (!addStr(opts.clientId)) return 0;
                if (opts.will.topic && opts.will.topic[0])
                {
                    if (!addStr(opts.will.topic)) return 0;
                    if (!addStr(opts.will.payload ? opts.will.payload : "")) return 0;
                }
                if (flags & 0x80)
                {
                    if (!addStr(opts.username)) return 0;
                }
                if (flags & 0x40)
                {
                    if (!addStr(opts.password)) return 0;
                }

                uint32_t remaining = sizeof(vh) + plen;
                uint8_t varint[4];
                size_t varintLen = writeVarInt(varint, sizeof(varint), remaining);
                if (!varintLen) return 0;

                size_t total = 1 + varintLen + sizeof(vh) + plen;
                if (bufLen < total) return 0;

                size_t pos = 0;
                buf[pos++] = PT_CONNECT;
                memcpy(buf + pos, varint, varintLen);
                pos += varintLen;
                memcpy(buf + pos, vh, sizeof(vh));
                pos += sizeof(vh);
                memcpy(buf + pos, payload, plen);
                return total;
            }

            size_t buildPublish(uint8_t *buf, size_t bufLen,
                                const char *topic, const void *payload, size_t payloadLen,
                                uint8_t qos, bool retain, uint16_t msgId)
            {
                size_t topicLen = topic ? strlen(topic) : 0;
                uint32_t remaining = 2 + topicLen + (qos > 0 ? 2 : 0) + payloadLen;

                uint8_t varint[4];
                size_t varintLen = writeVarInt(varint, sizeof(varint), remaining);
                if (!varintLen) return 0;

                size_t total = 1 + varintLen + 2 + topicLen + (qos > 0 ? 2 : 0) + payloadLen;
                if (bufLen < total) return 0;

                uint8_t firstByte = PT_PUBLISH | ((qos & 0x03) << 1) | (retain ? 0x01 : 0x00);
                size_t pos = 0;
                buf[pos++] = firstByte;
                memcpy(buf + pos, varint, varintLen);
                pos += varintLen;
                writeU16(buf + pos, (uint16_t)topicLen);
                pos += 2;
                if (topicLen)
                {
                    memcpy(buf + pos, topic, topicLen);
                    pos += topicLen;
                }
                if (qos > 0)
                {
                    writeU16(buf + pos, msgId);
                    pos += 2;
                }
                if (payloadLen) memcpy(buf + pos, payload, payloadLen);
                return total;
            }

            size_t buildSubscribe(uint8_t *buf, size_t bufLen,
                                  const char *topic, uint8_t qos, uint16_t msgId)
            {
                size_t topicLen = topic ? strlen(topic) : 0;
                uint32_t remaining = 2 + 2 + topicLen + 1; // msgId + topic string + qos byte

                uint8_t varint[4];
                size_t varintLen = writeVarInt(varint, sizeof(varint), remaining);
                if (!varintLen) return 0;

                size_t total = 1 + varintLen + remaining;
                if (bufLen < total) return 0;

                size_t pos = 0;
                buf[pos++] = PT_SUBSCRIBE | 0x02; // SUBSCRIBE always has flags=0x02
                memcpy(buf + pos, varint, varintLen);
                pos += varintLen;
                writeU16(buf + pos, msgId);
                pos += 2;
                writeU16(buf + pos, (uint16_t)topicLen);
                pos += 2;
                memcpy(buf + pos, topic, topicLen);
                pos += topicLen;
                buf[pos] = qos & 0x03;
                return total;
            }

            size_t buildUnsubscribe(uint8_t *buf, size_t bufLen,
                                    const char *topic, uint16_t msgId)
            {
                size_t topicLen = topic ? strlen(topic) : 0;
                uint32_t remaining = 2 + 2 + topicLen;

                uint8_t varint[4];
                size_t varintLen = writeVarInt(varint, sizeof(varint), remaining);
                if (!varintLen) return 0;

                size_t total = 1 + varintLen + remaining;
                if (bufLen < total) return 0;

                size_t pos = 0;
                buf[pos++] = PT_UNSUBSCRIBE | 0x02;
                memcpy(buf + pos, varint, varintLen);
                pos += varintLen;
                writeU16(buf + pos, msgId);
                pos += 2;
                writeU16(buf + pos, (uint16_t)topicLen);
                pos += 2;
                memcpy(buf + pos, topic, topicLen);
                return total;
            }

            static size_t buildTwoBytePacket(uint8_t *buf, size_t bufLen,
                                             uint8_t type, uint16_t msgId)
            {
                if (bufLen < 4) return 0;
                buf[0] = type;
                buf[1] = 0x02;
                writeU16(buf + 2, msgId);
                return 4;
            }

            size_t buildPubAck(uint8_t *buf, size_t bufLen, uint16_t msgId)
            {
                return buildTwoBytePacket(buf, bufLen, PT_PUBACK, msgId);
            }

            size_t buildPubRel(uint8_t *buf, size_t bufLen, uint16_t msgId)
            {
                return buildTwoBytePacket(buf, bufLen, PT_PUBREL | 0x02, msgId);
            }

            size_t buildPubComp(uint8_t *buf, size_t bufLen, uint16_t msgId)
            {
                return buildTwoBytePacket(buf, bufLen, PT_PUBCOMP, msgId);
            }

            size_t buildPingReq(uint8_t *buf, size_t bufLen)
            {
                if (bufLen < 2) return 0;
                buf[0] = PT_PINGREQ;
                buf[1] = 0x00;
                return 2;
            }

            size_t buildDisconnect(uint8_t *buf, size_t bufLen)
            {
                if (bufLen < 2) return 0;
                buf[0] = PT_DISCONNECT;
                buf[1] = 0x00;
                return 2;
            }

            bool parsePacket(const uint8_t *data, size_t len,
                             Packet &out, size_t &consumed)
            {
                if (len < 2) return false;

                uint32_t remaining;
                size_t varintConsumed;
                if (!readVarInt(data + 1, len - 1, remaining, varintConsumed)) return false;

                size_t headerLen = 1 + varintConsumed;
                if (len < headerLen + remaining) return false;

                out.type = data[0] & 0xF0;
                out.flags = data[0] & 0x0F;
                out.payload = data + headerLen;
                out.payloadLen = remaining;
                consumed = headerLen + remaining;
                return true;
            }

            // MQTT 3.1.1 §4.7: '+' ersetzt genau eine (auch leere) Ebene, '#' muss
            // letztes Zeichen sein und deckt die Elternebene mit ab ("a/#" matcht "a").
            bool topicMatches(const char *pattern, const char *topic)
            {
                if (!pattern || !topic) return false;

                for (;;)
                {
                    if (pattern[0] == '#' && pattern[1] == '\0') return true;

                    const char *pEnd = strchr(pattern, '/');
                    const char *tEnd = strchr(topic, '/');
                    size_t pLen = pEnd ? (size_t)(pEnd - pattern) : strlen(pattern);
                    size_t tLen = tEnd ? (size_t)(tEnd - topic) : strlen(topic);

                    bool plus = (pLen == 1 && pattern[0] == '+');
                    if (!plus && (pLen != tLen || strncmp(pattern, topic, pLen) != 0))
                        return false;

                    if (!pEnd && !tEnd) return true;
                    if (!pEnd || !tEnd)
                    {
                        // Topic zu Ende, Pattern nicht: nur ein abschliessendes '#' matcht
                        return !tEnd && strcmp(pEnd + 1, "#") == 0;
                    }

                    pattern = pEnd + 1;
                    topic = tEnd + 1;
                }
            }

        } // namespace MQTT
    } // namespace Network
} // namespace OpenKNX

#endif // OPENKNX_MQTT
#endif // KNX_IP_WIFI || KNX_IP_LAN
