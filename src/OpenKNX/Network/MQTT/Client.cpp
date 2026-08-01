#if defined(KNX_IP_WIFI) || defined(KNX_IP_LAN)
#ifdef OPENKNX_MQTT

#include "Client.h"
#include "OpenKNX.h"

#if defined(ARDUINO_ARCH_ESP32)
#include <WiFiClient.h>
#if defined(KNX_IP_LAN)
#include <ETH.h>
#else
#include <WiFi.h>
#endif
#elif defined(ARDUINO_ARCH_RP2040)
#include <lwip/dns.h>
#include <lwip/tcp.h>
#endif

namespace OpenKNX
{
    namespace Network
    {
        namespace MQTT
        {
            // ----------------------------------------------------------------
            // Public API
            // ----------------------------------------------------------------

            void Client::setWill(const char *topic, const char *payload,
                                 bool retain, uint8_t qos)
            {
                strncpy(_willTopic, topic ? topic : "", sizeof(_willTopic) - 1);
                strncpy(_willPayload, payload ? payload : "", sizeof(_willPayload) - 1);
                _willRetain = retain;
                _willQos = qos;
            }

            bool Client::begin(const char *host, uint16_t port,
                               const char *clientId,
                               const char *username, const char *password,
                               uint16_t keepAliveSeconds)
            {
                strncpy(_host, host ? host : "", sizeof(_host) - 1);
                strncpy(_clientId, clientId ? clientId : "", sizeof(_clientId) - 1);
                strncpy(_username, username ? username : "", sizeof(_username) - 1);
                strncpy(_password, password ? password : "", sizeof(_password) - 1);
                _port = port;
                _keepAlive = keepAliveSeconds;
                _state = STATE_DISCONNECTED;
                if (tcpConnect(_host, _port)) return true;
                armReconnectTimer();
                return false;
            }

            void Client::armReconnectTimer()
            {
                _reconnectTimer = millis();
                if (_reconnectTimer == 0) _reconnectTimer = 1; // 0 = kein Timer aktiv
            }

            void Client::disconnect()
            {
                if (_state == STATE_READY || _state == STATE_CONNECTED)
                {
                    uint8_t buf[2];
                    buildDisconnect(buf, sizeof(buf));
                    sendRaw(buf, 2);
                }
                tcpDisconnect();
                _state = STATE_DISCONNECTED;
            }

            void Client::subscribe(const std::string &topic, uint8_t qos,
                                   MessageCallback callback)
            {
                _subscriptions.push_back({topic, qos, callback});
                // Will be sent once READY, or immediately if already READY
                if (_state == STATE_READY)
                    sendPendingSubscriptions();
            }

            bool Client::publish(const char *topic, const void *payload, size_t len,
                                 uint8_t qos, bool retain)
            {
                if (_state != STATE_READY) return false;
                uint16_t msgId = (qos > 0) ? _msgId++ : 0;
                size_t n = buildPublish(_txBuf, sizeof(_txBuf), topic, payload, len, qos, retain, msgId);
                if (!n) return false;
                bool result = sendRaw(_txBuf, n);

                // Echo to local subscribers. Callback vorher kopieren und per Index
                // iterieren — ein subscribe() aus dem Callback realloziert den Vector.
                if (result)
                {
                    for (size_t i = 0; i < _subscriptions.size(); i++)
                    {
                        if (!topicMatches(_subscriptions[i].topic.c_str(), topic)) continue;
                        MessageCallback cb = _subscriptions[i].callback;
                        if (cb) cb(topic, payload, len);
                    }
                }

                return result;
            }

            bool Client::publish(const std::string &topic, const std::string &payload,
                                 uint8_t qos, bool retain)
            {
                return publish(topic.c_str(), payload.c_str(), payload.size(), qos, retain);
            }

            // ----------------------------------------------------------------
            // Loop
            // ----------------------------------------------------------------

            void Client::loop()
            {
                switch (_state)
                {
                    case STATE_DISCONNECTED:
                        // Reconnect timer elapsed → attempt a fresh connection.
                        // On failure re-arm the timer so we retry in another 5 s.
                        if (_reconnectTimer && delayCheck(_reconnectTimer, 5000))
                        {
                            if (tcpConnect(_host, _port))
                                _reconnectTimer = 0;
                            else
                                armReconnectTimer();
                        }
                        return;

                    case STATE_CONNECTING:
#if defined(ARDUINO_ARCH_RP2040)
                        // lwIP: connection result arrives via callback — nothing to poll here
                        return;
#elif defined(ARDUINO_ARCH_ESP32)
                        if (tcpConnected())
                        {
                            _state = STATE_CONNECTED;
                            sendConnect();
                        }
                        else if (delayCheck(_lastActivity, 5000))
                        {
                            logError("MQTT", "connect timeout");
                            reconnect();
                        }
                        return;
#endif

                    case STATE_CONNECTED:
                        // Waiting for CONNACK — enforce timeout
                        if (delayCheck(_lastActivity, 5000))
                        {
                            logError("MQTT", "CONNACK timeout");
                            reconnect();
                            return;
                        }
                        break;

                    case STATE_READY:
                        // Keep-alive ping
                        if (_keepAlive > 0 && delayCheck(_lastPing, (uint32_t)_keepAlive * 900))
                            sendPingReq();

                        // Server silence timeout
                        if (_keepAlive > 0 && delayCheck(_lastActivity, (uint32_t)_keepAlive * 1500 + 2000))
                        {
                            logError("MQTT", "keepalive timeout - reconnecting");
                            reconnect();
                            return;
                        }

                        sendPendingSubscriptions();
                        break;

                    case STATE_RECONNECTING:
                        return;
                }

                // Read incoming data
                uint8_t tmp[256];
                int n = tcpRead(tmp, sizeof(tmp));
                if (n < 0)
                {
                    logError("MQTT", "socket error - reconnecting");
                    reconnect();
                    return;
                }
                if (n == 0) return;

                _lastActivity = millis();

                // Append to reassembly buffer
                if (_rxLen + (size_t)n > RX_BUF_SIZE)
                {
                    logError("MQTT", "RX overflow - reconnecting");
                    reconnect();
                    return;
                }
                memcpy(_rxBuf + _rxLen, tmp, n);
                _rxLen += n;

                // Parse complete packets
                size_t offset = 0;
                while (offset < _rxLen)
                {
                    Packet pkt;
                    size_t consumed;
                    if (!parsePacket(_rxBuf + offset, _rxLen - offset, pkt, consumed)) break;
                    processPacket(pkt);
                    // reconnect() resets _rxLen — bail out before arithmetic underflows
                    if (_state == STATE_DISCONNECTED || _state == STATE_RECONNECTING) return;
                    offset += consumed;
                }
                _rxLen -= offset;
                if (offset && _rxLen) memmove(_rxBuf, _rxBuf + offset, _rxLen);
            }

            // ----------------------------------------------------------------
            // Internal helpers
            // ----------------------------------------------------------------

            void Client::sendConnect()
            {
                ConnectOptions opts;
                opts.clientId = _clientId;
                opts.username = _username[0] ? _username : nullptr;
                opts.password = _password[0] ? _password : nullptr;
                opts.keepAlive = _keepAlive;
                if (_willTopic[0])
                {
                    opts.will.topic = _willTopic;
                    opts.will.payload = _willPayload;
                    opts.will.retain = _willRetain;
                    opts.will.qos = _willQos;
                }
                size_t n = buildConnect(_txBuf, sizeof(_txBuf), opts);
                if (n) sendRaw(_txBuf, n);
                _lastActivity = millis();
            }

            void Client::sendPendingSubscriptions()
            {
                while (_subscribedUpTo < _subscriptions.size())
                {
                    auto &sub = _subscriptions[_subscribedUpTo];
                    size_t n = buildSubscribe(_txBuf, sizeof(_txBuf),
                                              sub.topic.c_str(), sub.qos, _msgId++);
                    if (!n || !sendRaw(_txBuf, n)) break;
                    ++_subscribedUpTo;
                }
            }

            void Client::sendPingReq()
            {
                uint8_t buf[2];
                size_t n = buildPingReq(buf, sizeof(buf));
                if (n) sendRaw(buf, n);
                _lastPing = millis();
            }

            void Client::reconnect()
            {
                tcpDisconnect();
                _state = STATE_DISCONNECTED;
                _rxLen = 0;
                _subscribedUpTo = 0;
                armReconnectTimer();
                ++_reconnectCount;
                logInfo("MQTT", "reconnect #%u in 5 s", _reconnectCount);
            }

            void Client::processPacket(const Packet &pkt)
            {
                switch (pkt.type)
                {
                    case PT_CONNACK: handleConnAck(pkt); break;
                    case PT_PUBLISH: handlePublish(pkt); break;
                    case PT_PUBACK: break; // QoS 1 ack — not tracking inflight here
                    case PT_PINGRESP: _lastActivity = millis(); break;
                    default: break;
                }
            }

            void Client::handleConnAck(const Packet &pkt)
            {
                if (pkt.payloadLen < 2) return;
                uint8_t rc = pkt.payload[1];
                if (rc != CRC_ACCEPTED)
                {
                    logError("MQTT", "CONNACK refused: %u", rc);
                    reconnect();
                    return;
                }
                logInfo("MQTT", "connected");
                _state = STATE_READY;
                _lastActivity = millis();
                _lastPing = millis();
                _reconnectCount = 0;
                sendPendingSubscriptions();
            }

            void Client::handlePublish(const Packet &pkt)
            {
                if (pkt.payloadLen < 2) return;
                const uint8_t *p = pkt.payload;
                uint16_t topicLen = readU16(p);
                p += 2;
                if (pkt.payloadLen < (size_t)(2 + topicLen)) return;

                static char topic[128];
                size_t tl = (topicLen < sizeof(topic) - 1) ? topicLen : sizeof(topic) - 1;
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
                        if (n) sendRaw(ack, n);
                    }
                }

                size_t payloadOff = (size_t)(p - pkt.payload);
                size_t payloadLen = (pkt.payloadLen > payloadOff) ? pkt.payloadLen - payloadOff : 0;

                for (size_t i = 0; i < _subscriptions.size(); i++)
                {
                    if (!topicMatches(_subscriptions[i].topic.c_str(), topic)) continue;
                    MessageCallback cb = _subscriptions[i].callback;
                    if (cb) cb(topic, p, payloadLen);
                }
            }

            // ----------------------------------------------------------------
            // Platform TCP — ESP32
            // ----------------------------------------------------------------
#if defined(ARDUINO_ARCH_ESP32)

            bool Client::tcpConnect(const char *host, uint16_t port)
            {
                tcpDisconnect(); // free any previous client (e.g. repeated begin())
                WiFiClient *c = new WiFiClient();
                _wifiClient = c;
                _lastActivity = millis();
                _state = STATE_CONNECTING;
                // WiFiClient::connect() blockiert bis zu seinem eigenen Timeout;
                // bei Fehlschlag übernimmt der CONNACK-Timeout in loop()
                if (!c->connect(host, port))
                    return true;
                _state = STATE_CONNECTED;
                sendConnect();
                return true;
            }

            void Client::tcpDisconnect()
            {
                if (_wifiClient)
                {
                    static_cast<WiFiClient *>(_wifiClient)->stop();
                    delete static_cast<WiFiClient *>(_wifiClient);
                    _wifiClient = nullptr;
                }
            }

            bool Client::tcpConnected()
            {
                return _wifiClient && static_cast<WiFiClient *>(_wifiClient)->connected();
            }

            int Client::tcpRead(uint8_t *buf, size_t maxLen)
            {
                if (!_wifiClient) return -1;
                auto *c = static_cast<WiFiClient *>(_wifiClient);
                if (!c->connected()) return -1;
                int avail = c->available();
                if (avail <= 0) return 0;
                int n = c->read(buf, (size_t)avail < maxLen ? (size_t)avail : maxLen);
                return n < 0 ? 0 : n;
            }

            bool Client::tcpWrite(const uint8_t *data, size_t len)
            {
                if (!_wifiClient) return false;
                return static_cast<WiFiClient *>(_wifiClient)->write(data, len) == len;
            }

            bool Client::sendRaw(const uint8_t *data, size_t len)
            {
                return tcpWrite(data, len);
            }

            // ----------------------------------------------------------------
            // Platform TCP — RP2040 (lwIP NO_SYS=1)
            // ----------------------------------------------------------------
#elif defined(ARDUINO_ARCH_RP2040)

            Client *Client::_instance = nullptr;

            bool Client::tcpConnect(const char *host, uint16_t port)
            {
                tcpDisconnect(); // free any previous pcb (e.g. repeated begin())
                _instance = this;
                _state = STATE_CONNECTING;

                struct tcp_pcb *pcb = tcp_new();
                if (!pcb) return false;
                _pcb = pcb;

                tcp_arg(pcb, this);
                tcp_recv(pcb, lwipRecv);
                tcp_err(pcb, lwipErr);
                tcp_poll(pcb, lwipPoll, 4); // ~2 s

                ip_addr_t addr;
                err_t err = dns_gethostbyname(host, &addr, nullptr, nullptr);
                if (err == ERR_OK)
                {
                    tcp_connect(pcb, &addr, port, lwipConnected);
                }
                else
                {
                    // DNS pending — not fully supported in this minimal impl.
                    // Caller must pass an IP string when using RP2040.
                    tcp_abort(pcb);
                    _pcb = nullptr;
                    _state = STATE_DISCONNECTED;
                    return false;
                }
                _lastActivity = millis();
                return true;
            }

            void Client::tcpDisconnect()
            {
                if (_pcb)
                {
                    tcp_arg((struct tcp_pcb *)_pcb, nullptr);
                    tcp_recv((struct tcp_pcb *)_pcb, nullptr);
                    tcp_err((struct tcp_pcb *)_pcb, nullptr);
                    tcp_close((struct tcp_pcb *)_pcb);
                    _pcb = nullptr;
                }
            }

            bool Client::tcpConnected()
            {
                return _pcb != nullptr;
            }

            int Client::tcpRead(uint8_t *, size_t)
            {
                // Data arrives via lwipRecv callback and is fed directly into _rxBuf
                return 0;
            }

            bool Client::tcpWrite(const uint8_t *data, size_t len)
            {
                if (!_pcb) return false;
                err_t err = tcp_write((struct tcp_pcb *)_pcb, data, len, TCP_WRITE_FLAG_COPY);
                if (err != ERR_OK) return false;
                tcp_output((struct tcp_pcb *)_pcb);
                return true;
            }

            bool Client::sendRaw(const uint8_t *data, size_t len)
            {
                return tcpWrite(data, len);
            }

            err_t Client::lwipConnected(void *arg, struct tcp_pcb *, err_t err)
            {
                Client *self = static_cast<Client *>(arg);
                if (!self) return ERR_OK;
                if (err != ERR_OK)
                {
                    self->reconnect();
                    return ERR_OK;
                }
                self->_state = STATE_CONNECTED;
                self->sendConnect();
                self->_lastActivity = millis();
                return ERR_OK;
            }

            err_t Client::lwipRecv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
            {
                Client *self = static_cast<Client *>(arg);
                if (!self || !p)
                {
                    if (self) self->reconnect();
                    return ERR_OK;
                }
                // Copy pbuf chain into _rxBuf
                struct pbuf *q = p;
                while (q)
                {
                    if (self->_rxLen + q->len <= RX_BUF_SIZE)
                    {
                        memcpy(self->_rxBuf + self->_rxLen, q->payload, q->len);
                        self->_rxLen += q->len;
                    }
                    q = q->next;
                }
                tcp_recved(pcb, p->tot_len);
                pbuf_free(p);
                self->_lastActivity = millis();

                // Parse packets inline (we're already in loop context via lwIP sys_check_timeouts)
                size_t offset = 0;
                while (offset < self->_rxLen)
                {
                    Packet pkt;
                    size_t consumed;
                    if (!parsePacket(self->_rxBuf + offset, self->_rxLen - offset, pkt, consumed)) break;
                    self->processPacket(pkt);
                    if (self->_state == STATE_DISCONNECTED || self->_state == STATE_RECONNECTING) break;
                    offset += consumed;
                }
                if (self->_rxLen >= offset)
                {
                    self->_rxLen -= offset;
                    if (offset && self->_rxLen) memmove(self->_rxBuf, self->_rxBuf + offset, self->_rxLen);
                }
                else
                {
                    self->_rxLen = 0;
                }

                return ERR_OK;
            }

            void Client::lwipErr(void *arg, err_t)
            {
                Client *self = static_cast<Client *>(arg);
                if (self)
                {
                    self->_pcb = nullptr;
                    self->reconnect();
                }
            }

            err_t Client::lwipPoll(void *arg, struct tcp_pcb *)
            {
                Client *self = static_cast<Client *>(arg);
                if (self) self->loop();
                return ERR_OK;
            }

#endif // platform

        } // namespace MQTT
    } // namespace Network
} // namespace OpenKNX

#endif // OPENKNX_MQTT
#endif // KNX_IP_WIFI || KNX_IP_LAN
