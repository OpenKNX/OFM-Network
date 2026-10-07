#if defined(KNX_IP_WIFI) || defined(KNX_IP_LAN)
#ifdef OPENKNX_MQTT

#include "Broker.h"
#include "OpenKNX.h"
#include <algorithm>

#if defined(ARDUINO_ARCH_ESP32)
#include <WiFiClient.h>
#include <WiFiServer.h>
#if defined(KNX_IP_LAN)
#include <ETH.h>
#else
#include <WiFi.h>
#endif
#elif defined(ARDUINO_ARCH_RP2040)
#include <lwip/tcp.h>
#include <lwip_wrap.h> // LWIPMutex: makes the ethernet IRQ defer
#endif

namespace OpenKNX
{
    namespace Network
    {
        namespace MQTT
        {
            // Helper: build a 4-byte two-field packet (type + 2-byte msgId)
            static size_t buildTwoBytePacket(uint8_t *buf, size_t bufLen,
                                             uint8_t type, uint16_t msgId)
            {
                if (bufLen < 4) return 0;
                buf[0] = type;
                buf[1] = 0x02;
                writeU16(buf + 2, msgId);
                return 4;
            }

            // ----------------------------------------------------------------
            // Public API
            // ----------------------------------------------------------------

            bool Broker::begin(uint16_t port, const char *username, const char *password)
            {
                _port = port;
                _authRequired = username && username[0];
                strncpy(_username, username ? username : "", sizeof(_username) - 1);
                strncpy(_password, password ? password : "", sizeof(_password) - 1);

#if defined(ARDUINO_ARCH_ESP32)
                WiFiServer *srv = new WiFiServer(port);
                srv->begin();
                _server = srv;
#elif defined(ARDUINO_ARCH_RP2040)
                struct tcp_pcb *pcb = tcp_new();
                if (!pcb)
                {
                    logError("MQTT", "broker: tcp_new failed");
                    return false;
                }
                if (tcp_bind(pcb, IP_ADDR_ANY, port) != ERR_OK)
                {
                    logError("MQTT", "broker: port %u already in use", port);
                    tcp_close(pcb);
                    return false;
                }
                // tcp_listen() frees pcb on success only -- on failure it is still ours
                _listenPcb = tcp_listen(pcb);
                if (!_listenPcb)
                {
                    logError("MQTT", "broker: tcp_listen failed");
                    tcp_close(pcb);
                    return false;
                }
                tcp_arg((struct tcp_pcb *)_listenPcb, this);
                tcp_accept((struct tcp_pcb *)_listenPcb, lwipAccept);
#endif
                _active = true;
                logInfo("MQTT", "broker started on port %u", port);
                return true;
            }

            bool Broker::publish(const char *topic, const void *payload, size_t len,
                                 uint8_t qos, bool retain)
            {
                if (!_active) return false;
                routePublish(topic, payload, len, qos, retain);
                return true;
            }

            bool Broker::publish(const std::string &topic, const std::string &payload,
                                 uint8_t qos, bool retain)
            {
                return publish(topic.c_str(), payload.c_str(), payload.size(), qos, retain);
            }

            void Broker::subscribe(const std::string &topic, MessageCallback callback)
            {
                _observers.push_back({topic, callback});
            }

            // ----------------------------------------------------------------
            // Loop
            // ----------------------------------------------------------------

            void Broker::loop()
            {
                if (!_active) return;
                acceptNew();
                for (auto &c : _conns)
                {
#if defined(ARDUINO_ARCH_ESP32)
                    if (!c.active) continue;
                    readConn(c);
                    if (c.active) serviceConn(c);
#elif defined(ARDUINO_ARCH_RP2040)
                    if (!c.active && c.pcb) openConn(c);
                    if (!c.active) continue;
                    const bool gone = readConn(c);
                    serviceConn(c); // data received before FIN still counts
                    if (gone && c.active) closeConn(c);
#endif
                }
            }

            // ----------------------------------------------------------------
            // Internal — connection management
            // ----------------------------------------------------------------

            void Broker::acceptNew()
            {
#if defined(ARDUINO_ARCH_ESP32)
                WiFiServer *srv = static_cast<WiFiServer *>(_server);
                if (!srv->hasClient()) return;
                WiFiClient incoming = srv->accept();
                if (!incoming) return;

                for (auto &c : _conns)
                {
                    if (!c.active)
                    {
                        c.active = true;
                        c.connected = false;
                        c.rxLen = 0;
                        c.lastActivity = millis();
                        c.subscriptions.clear();
                        WiFiClient *stored = new WiFiClient(incoming);
                        c.wifiClient = stored;
                        logInfo("MQTT", "broker: TCP connection from %s", incoming.remoteIP().toString().c_str());
                        return;
                    }
                }
                // No free slot — reject
                incoming.stop();
                logInfo("MQTT", "broker: max clients reached");
#endif
            }

#if defined(ARDUINO_ARCH_ESP32)
            void Broker::readConn(Conn &c)
            {
                auto *wc = static_cast<WiFiClient *>(c.wifiClient);
                if (!wc || !wc->connected())
                {
                    closeConn(c);
                    return;
                }
                int avail = wc->available();
                while (avail > 0 && c.rxLen < sizeof(c.rxBuf))
                {
                    size_t space = sizeof(c.rxBuf) - c.rxLen;
                    int n = wc->read(c.rxBuf + c.rxLen, space < (size_t)avail ? space : (size_t)avail);
                    if (n <= 0) break;
                    c.rxLen += n;
                    avail -= n;
                }
            }
#endif

            void Broker::serviceConn(Conn &c)
            {
                // Keep-alive timeout (only after CONNECT received)
                if (c.connected && c.keepAlive > 0 &&
                    delayCheck(c.lastActivity, (uint32_t)c.keepAlive * 1500))
                {
                    logInfo("MQTT", "broker: client '%s' keepalive timeout", c.clientId);
                    closeConn(c);
                    return;
                }

                // Parse buffered data
                size_t offset = 0;
                while (offset < c.rxLen)
                {
                    Packet pkt;
                    size_t consumed;
                    if (!parsePacket(c.rxBuf + offset, c.rxLen - offset, pkt, consumed)) break;
                    handlePacket(c, pkt);
                    // closeConn() resets rxLen — bail out before arithmetic underflows
                    if (!c.active) return;
                    offset += consumed;
                }
                c.rxLen -= offset;
                if (offset && c.rxLen) memmove(c.rxBuf, c.rxBuf + offset, c.rxLen);

                // Puffer voll, aber kein vollständiges Paket darin → passt nie hinein
                if (c.rxLen >= sizeof(c.rxBuf))
                {
                    logError("MQTT", "broker: packet exceeds RX buffer, dropping client '%s'", c.clientId);
                    closeConn(c);
                }
            }

            void Broker::closeConn(Conn &c)
            {
#if defined(ARDUINO_ARCH_ESP32)
                if (c.wifiClient)
                {
                    static_cast<WiFiClient *>(c.wifiClient)->stop();
                    delete static_cast<WiFiClient *>(c.wifiClient);
                    c.wifiClient = nullptr;
                }
#elif defined(ARDUINO_ARCH_RP2040)
                LWIPMutex lock;
                if (c.rxPbuf)
                {
                    // Unread data left in the window makes tcp_close() send RST and drop
                    // what we still had queued (e.g. a refusing CONNACK).
                    if (c.pcb) tcp_recved(c.pcb, c.rxPbuf->tot_len);
                    pbuf_free(c.rxPbuf);
                    c.rxPbuf = nullptr;
                }
                c.rxClosed = false;
                struct tcp_pcb *pcb = c.pcb;
                c.pcb = nullptr;
                if (pcb)
                {
                    tcp_arg(pcb, nullptr);
                    tcp_recv(pcb, nullptr);
                    tcp_err(pcb, nullptr);
                    if (tcp_close(pcb) != ERR_OK) tcp_abort(pcb);
                }
#endif
                c.connected = false;
                c.subscriptions.clear();
                c.rxLen = 0;
                c.active = false; // last: from here on the accept IRQ may reuse the slot
            }

            bool Broker::sendToConn(Conn &c, const uint8_t *data, size_t len)
            {
#if defined(ARDUINO_ARCH_ESP32)
                if (!c.wifiClient) return false;
                return static_cast<WiFiClient *>(c.wifiClient)->write(data, len) == len;
#elif defined(ARDUINO_ARCH_RP2040)
                LWIPMutex lock;
                struct tcp_pcb *pcb = c.pcb;
                if (!pcb) return false;
                if (tcp_write(pcb, data, len, TCP_WRITE_FLAG_COPY) != ERR_OK) return false;
                tcp_output(pcb);
                return true;
#else
                return false;
#endif
            }

            bool Broker::checkAuth(const char *user, const char *pass) const
            {
                if (!_authRequired) return true;
                if (!user || !pass) return false;
                return strcmp(user, _username) == 0 && strcmp(pass, _password) == 0;
            }

            // ----------------------------------------------------------------
            // Packet handlers
            // ----------------------------------------------------------------

            void Broker::handlePacket(Conn &c, const Packet &pkt)
            {
                c.lastActivity = millis();
                switch (pkt.type)
                {
                    case PT_CONNECT: handleConnect(c, pkt); break;
                    case PT_SUBSCRIBE: handleSubscribe(c, pkt); break;
                    case PT_UNSUBSCRIBE: handleUnsubscribe(c, pkt); break;
                    case PT_PUBLISH: handlePublishPkt(c, pkt); break;
                    case PT_PINGREQ: handlePingReq(c); break;
                    case PT_DISCONNECT: handleDisconnect(c); break;
                    case PT_PUBREL:
                    {
                        if (pkt.payloadLen >= 2)
                        {
                            uint16_t mid = readU16(pkt.payload);
                            uint8_t buf[4];
                            size_t n = buildPubComp(buf, sizeof(buf), mid);
                            if (n) sendToConn(c, buf, n);
                        }
                        break;
                    }
                    default:
                        logInfo("MQTT", "broker: unknown packet type 0x%02X from '%s'", pkt.type, c.clientId);
                        break;
                }
            }

            void Broker::handleConnect(Conn &c, const Packet &pkt)
            {
                if (pkt.payloadLen < 10)
                {
                    logInfo("MQTT", "broker: CONNECT packet too short (%u)", (unsigned)pkt.payloadLen);
                    closeConn(c);
                    return;
                }
                const uint8_t *p = pkt.payload;
                size_t rem = pkt.payloadLen;

                // Protocol name (4 bytes length + "MQTT") + protocol level
                if (rem < 7)
                {
                    logInfo("MQTT", "broker: CONNECT missing protocol header");
                    closeConn(c);
                    return;
                }
                p += 7;
                rem -= 7; // skip: 0x00 0x04 "MQTT" + protocol level byte

                uint8_t flags = *p++;
                rem--;
                uint16_t keepAlive = readU16(p);
                p += 2;
                rem -= 2;

                // Extract strings helper
                auto readStr = [&](char *dst, size_t dstLen) -> bool {
                    if (rem < 2) return false;
                    uint16_t slen = readU16(p);
                    p += 2;
                    rem -= 2;
                    if (rem < slen) return false;
                    size_t copy = slen < dstLen - 1 ? slen : dstLen - 1;
                    memcpy(dst, p, copy);
                    dst[copy] = '\0';
                    p += slen;
                    rem -= slen;
                    return true;
                };

                static char clientId[24];
                static char willTopic[64];
                static char willPayload[64];
                static char user[24];
                static char pass[24];
                clientId[0] = willTopic[0] = willPayload[0] = user[0] = pass[0] = '\0';

                if (!readStr(clientId, sizeof(clientId)))
                {
                    logInfo("MQTT", "broker: CONNECT failed reading clientId");
                    closeConn(c);
                    return;
                }

                // Will
                if (flags & 0x04)
                {
                    if (!readStr(willTopic, sizeof(willTopic)))
                    {
                        closeConn(c);
                        return;
                    }
                    if (!readStr(willPayload, sizeof(willPayload)))
                    {
                        closeConn(c);
                        return;
                    }
                }

                if (flags & 0x80) readStr(user, sizeof(user));
                if (flags & 0x40) readStr(pass, sizeof(pass));

                uint8_t connAck[4] = {PT_CONNACK, 0x02, 0x00, CRC_ACCEPTED};
                if (!checkAuth(user[0] ? user : nullptr, pass[0] ? pass : nullptr))
                {
                    logInfo("MQTT", "broker: CONNECT clientId='%s' user='%s' — auth REFUSED", clientId, user);
                    connAck[3] = CRC_REFUSED_CREDENTIALS;
                    sendToConn(c, connAck, sizeof(connAck));
                    closeConn(c);
                    return;
                }

                logInfo("MQTT", "broker: CONNECT clientId='%s' user='%s' keepAlive=%us — accepted", clientId, user, keepAlive);

                strncpy(c.clientId, clientId, sizeof(c.clientId) - 1);
                c.keepAlive = keepAlive;
                c.connected = true;
                sendToConn(c, connAck, sizeof(connAck));

                // Send retained messages
                for (auto &[topic, msg] : _retained)
                {
                    size_t n = buildPublish(mqttTxBuf, mqttTxBufSize, topic.c_str(),
                                            msg.payload.c_str(), msg.payload.size(),
                                            0, true, 0); // QoS 0, siehe routePublish()
                    if (n) sendToConn(c, mqttTxBuf, n);
                }
                logInfo("MQTT", "broker: client '%s' connected", c.clientId);
            }

            void Broker::handleSubscribe(Conn &c, const Packet &pkt)
            {
                if (!c.connected || pkt.payloadLen < 4) return;
                const uint8_t *p = pkt.payload;
                uint16_t msgId = readU16(p);
                p += 2;
                size_t rem = pkt.payloadLen - 2;

                static uint8_t subAck[64];
                subAck[0] = PT_SUBACK;
                size_t ackLen = 2;
                writeU16(subAck + 2, msgId);

                while (rem >= 3)
                {
                    uint16_t tlen = readU16(p);
                    p += 2;
                    rem -= 2;
                    if (rem < tlen + 1) break;
                    static char topic[128];
                    size_t copy = tlen < sizeof(topic) - 1 ? tlen : sizeof(topic) - 1;
                    memcpy(topic, p, copy);
                    topic[copy] = '\0';
                    p += tlen;
                    rem -= tlen;
                    uint8_t qos = *p++ & 0x03;
                    rem--;

                    // Update an existing subscription instead of accumulating
                    // duplicates; cap the total to bound heap usage.
                    bool found = false;
                    for (auto &sub : c.subscriptions)
                        if (sub.topic == topic) { sub.qos = qos; found = true; break; }
                    if (!found)
                    {
                        if (c.subscriptions.size() < OPENKNX_MQTT_BROKER_MAX_SUBS)
                            c.subscriptions.push_back({topic, qos});
                        else
                            logError("MQTT", "broker: subscription limit (%u) reached for '%s'",
                                     (unsigned)OPENKNX_MQTT_BROKER_MAX_SUBS, c.clientId);
                    }

                    if (2 + ackLen < sizeof(subAck))
                        subAck[2 + ackLen++] = 0; // granted QoS — wir liefern nur QoS 0 aus

                    // Send retained messages matching this subscription
                    for (auto &[rtopic, msg] : _retained)
                    {
                        if (topicMatches(topic, rtopic.c_str()))
                        {
                            size_t n = buildPublish(mqttTxBuf, mqttTxBufSize, rtopic.c_str(),
                                                    msg.payload.c_str(), msg.payload.size(),
                                                    0, true, 0); // QoS 0, siehe routePublish()
                            if (n) sendToConn(c, mqttTxBuf, n);
                        }
                    }
                }

                subAck[1] = (uint8_t)(ackLen); // remaining length
                sendToConn(c, subAck, 2 + ackLen);
            }

            void Broker::handleUnsubscribe(Conn &c, const Packet &pkt)
            {
                if (!c.connected || pkt.payloadLen < 4) return;
                const uint8_t *p = pkt.payload;
                uint16_t msgId = readU16(p);
                p += 2;
                size_t rem = pkt.payloadLen - 2;

                while (rem >= 2)
                {
                    uint16_t tlen = readU16(p);
                    p += 2;
                    rem -= 2;
                    if (rem < tlen) break;
                    static char topic[128];
                    size_t copy = tlen < sizeof(topic) - 1 ? tlen : sizeof(topic) - 1;
                    memcpy(topic, p, copy);
                    topic[copy] = '\0';
                    p += tlen;
                    rem -= tlen;
                    c.subscriptions.erase(
                        std::remove_if(c.subscriptions.begin(), c.subscriptions.end(),
                                       [&](const BrokerSubscription &s) { return s.topic == topic; }),
                        c.subscriptions.end());
                }

                uint8_t unsuback[4] = {PT_UNSUBACK, 0x02, 0x00, 0x00};
                writeU16(unsuback + 2, msgId);
                sendToConn(c, unsuback, sizeof(unsuback));
            }

            void Broker::handlePublishPkt(Conn &c, const Packet &pkt)
            {
                if (!c.connected || pkt.payloadLen < 2) return;
                const uint8_t *p = pkt.payload;
                uint16_t topicLen = readU16(p);
                p += 2;
                if (pkt.payloadLen < (size_t)(2 + topicLen)) return;

                static char topic[128];
                size_t tl = topicLen < sizeof(topic) - 1 ? topicLen : sizeof(topic) - 1;
                memcpy(topic, p, tl);
                topic[tl] = '\0';
                p += topicLen;

                uint8_t qos = (pkt.flags >> 1) & 0x03;
                uint16_t msgId = 0;
                if (qos > 0)
                {
                    if (pkt.payloadLen < (size_t)(2 + topicLen + 2)) return;
                    msgId = readU16(p);
                    p += 2;
                    if (qos == 1)
                    {
                        uint8_t ack[4];
                        size_t n = buildPubAck(ack, sizeof(ack), msgId);
                        if (n) sendToConn(c, ack, n);
                    }
                    else if (qos == 2)
                    {
                        // Handshake wird vollständig geführt, die Nachricht aber sofort
                        // geroutet statt bis PUBREL gehalten: at-least-once statt
                        // exactly-once. Siehe README.MQTT.md.
                        uint8_t rec[4];
                        size_t n = buildTwoBytePacket(rec, sizeof(rec), PT_PUBREC, msgId);
                        if (n) sendToConn(c, rec, n);
                    }
                }

                size_t payloadOff = (size_t)(p - pkt.payload);
                size_t payloadLen = pkt.payloadLen > payloadOff ? pkt.payloadLen - payloadOff : 0;
                bool retain = pkt.flags & 0x01;

                routePublish(topic, p, payloadLen, qos, retain);
            }

            void Broker::handlePingReq(Conn &c)
            {
                uint8_t resp[2] = {PT_PINGRESP, 0x00};
                sendToConn(c, resp, sizeof(resp));
            }

            void Broker::handleDisconnect(Conn &c)
            {
                logInfo("MQTT", "broker: client '%s' disconnected cleanly", c.clientId);
                closeConn(c);
            }

            // ----------------------------------------------------------------
            // Message routing
            // ----------------------------------------------------------------

            void Broker::routePublish(const char *topic, const void *payload, size_t len,
                                      uint8_t qos, bool retain)
            {
                if (retain)
                {
                    if (len == 0)
                        _retained.erase(topic);
                    else if (_retained.count(topic) || _retained.size() < OPENKNX_MQTT_BROKER_MAX_RETAINED)
                        _retained.insert_or_assign(topic, RetainedMessage{std::string((const char *)payload, len), qos});
                    else
                        logError("MQTT", "broker: retained limit (%u) reached, dropping '%s'",
                                 (unsigned)OPENKNX_MQTT_BROKER_MAX_RETAINED, topic);
                }

                // Ausgehend immer QoS 0: der Broker vergibt keine msgIds und verfolgt
                // keine ausgehenden Acks. Ein PUBLISH mit QoS>0 und msgId 0 wäre
                // protokollwidrig. Passend dazu grantet handleSubscribe() QoS 0.
                // Client messages bypass Module's queue, so the buffer has to grow here.
                // n = 0 only skips the wire, local observers below still get it.
                size_t n = mqttTxReserve(publishSize(strlen(topic), len))
                               ? buildPublish(mqttTxBuf, mqttTxBufSize, topic, payload, len, 0, false, 0)
                               : 0;

                // Route to connected broker clients
                for (auto &c : _conns)
                {
                    if (!c.connected) continue;
                    for (auto &sub : c.subscriptions)
                    {
                        if (topicMatches(sub.topic.c_str(), topic))
                        {
                            if (n) sendToConn(c, mqttTxBuf, n);
                            break;
                        }
                    }
                }

                // Route to local observers. Callback kopieren und per Index iterieren —
                // ein subscribe() aus dem Callback realloziert den Vector.
                for (size_t i = 0; i < _observers.size(); i++)
                {
                    if (!topicMatches(_observers[i].first.c_str(), topic)) continue;
                    MessageCallback cb = _observers[i].second;
                    if (cb) cb(topic, payload, len);
                }
            }

            // ----------------------------------------------------------------
            // RP2040 lwIP callbacks
            // ----------------------------------------------------------------
#if defined(ARDUINO_ARCH_RP2040)

            // The lwIP callbacks run in the ethernet IRQ and only hand over: a new pcb
            // into a free slot, pbufs into rxPbuf, FIN/error as flags. Parsing, routing and
            // slot bookkeeping run in loop(), every pcb access under LWIPMutex.

            void Broker::openConn(Conn &c)
            {
                c.connected = false;
                c.clientId[0] = '\0';
                c.keepAlive = 0;
                c.rxLen = 0;
                c.subscriptions.clear();
                c.lastActivity = millis();
                c.active = true;
            }

            bool Broker::readConn(Conn &c)
            {
                LWIPMutex lock;
                if (c.rxPbuf && c.rxLen < sizeof(c.rxBuf))
                {
                    const u16_t n = pbuf_copy_partial(c.rxPbuf, c.rxBuf + c.rxLen, (u16_t)(sizeof(c.rxBuf) - c.rxLen), 0);
                    c.rxPbuf = pbuf_free_header(c.rxPbuf, n);
                    c.rxLen += n;
                    // Acknowledged only once taken: a busy loop closes the window, nothing is lost.
                    if (c.pcb) tcp_recved(c.pcb, n);
                }
                return !c.rxPbuf && (!c.pcb || c.rxClosed);
            }

            err_t Broker::lwipAccept(void *arg, struct tcp_pcb *newPcb, err_t err)
            {
                Broker *self = static_cast<Broker *>(arg);
                if (!self || err != ERR_OK || !newPcb) return ERR_VAL;

                for (auto &c : self->_conns)
                {
                    if (c.active || c.pcb) continue;
                    // Left over when the previous peer failed before loop() opened the slot
                    if (c.rxPbuf) pbuf_free(c.rxPbuf);
                    c.rxPbuf = nullptr;
                    c.rxClosed = false;
                    tcp_arg(newPcb, &c);
                    tcp_recv(newPcb, lwipRecv);
                    tcp_err(newPcb, lwipErr);
                    c.pcb = newPcb; // loop() sees pcb without active and opens the slot
                    return ERR_OK;
                }
                tcp_abort(newPcb);
                return ERR_ABRT;
            }

            err_t Broker::lwipRecv(void *arg, struct tcp_pcb *, struct pbuf *p, err_t err)
            {
                Conn *c = static_cast<Conn *>(arg);
                if (!p)
                {
                    if (c) c->rxClosed = true;
                    return ERR_OK;
                }
                if (!c || err != ERR_OK)
                {
                    pbuf_free(p);
                    return ERR_OK;
                }
                if (c->rxPbuf)
                    pbuf_cat(c->rxPbuf, p);
                else
                    c->rxPbuf = p;
                return ERR_OK;
            }

            void Broker::lwipErr(void *arg, err_t)
            {
                // The pcb is already freed; loop() closes the slot.
                Conn *c = static_cast<Conn *>(arg);
                if (c) c->pcb = nullptr;
            }
#endif // RP2040

        } // namespace MQTT
    } // namespace Network
} // namespace OpenKNX

#endif // OPENKNX_MQTT
#endif // KNX_IP_WIFI || KNX_IP_LAN
