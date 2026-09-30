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
#include "OpenKNX/Network/DNS.h"
#include <lwip/tcp.h>
#include <lwip_wrap.h> // LWIPMutex: makes the ethernet IRQ defer
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
                // Setup succeeds even if the first connect does not -- that is runtime
                // state, retried from loop(). See README.MQTT.md.
                if (!tcpConnect(_host, _port)) armReconnectTimer();
                return true;
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
                size_t n = buildPublish(mqttTxBuf, mqttTxBufSize, topic, payload, len, qos, retain, msgId);
                if (!n) return false;
                bool result = sendRaw(mqttTxBuf, n);

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
                        // Cleared first: a failing tcpConnect() re-arms it.
                        if (_reconnectTimer && delayCheck(_reconnectTimer, 5000))
                        {
                            _reconnectTimer = 0;
                            if (!tcpConnect(_host, _port)) armReconnectTimer();
                        }
                        return;

                    case STATE_RESOLVING:
                        // Answered by callback; lwIP times out itself, this is the backstop
                        if (delayCheck(_lastActivity, 15000))
                        {
                            logError("MQTT", "DNS timeout");
                            _state = STATE_DISCONNECTED;
                            armReconnectTimer();
                        }
                        return;

                    case STATE_CONNECTING:
                        if (tcpConnected())
                        {
                            _state = STATE_CONNECTED;
                            sendConnect();
                        }
#if defined(ARDUINO_ARCH_RP2040)
                        else if (!_pcb) // lwipErr: refused or unreachable
                        {
                            logError("MQTT", "connect to %s:%u failed", _host, _port);
                            reconnect();
                        }
#endif
                        else if (delayCheck(_lastActivity, 5000))
                        {
                            logError("MQTT", "connect timeout");
                            reconnect();
                        }
                        return;

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

                // Never read more than fits: the rest stays in the socket, unacknowledged.
                int n = tcpRead(_rxBuf + _rxLen, RX_BUF_SIZE - _rxLen);
                if (n < 0)
                {
                    logError("MQTT", "socket error - reconnecting");
                    reconnect();
                    return;
                }
                if (n == 0) return;

                _lastActivity = millis();

                if (_rxSkip)
                {
                    const size_t drop = ((size_t)n < _rxSkip) ? (size_t)n : _rxSkip;
                    _rxSkip -= drop;
                    n -= (int)drop;
                    if (n) memmove(_rxBuf, _rxBuf + drop, n);
                }
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

                // Full without a complete packet: skip that one packet -- a reconnect
                // would just receive the same retained message again.
                if (_rxLen == RX_BUF_SIZE)
                {
                    uint32_t remaining;
                    size_t hdr;
                    if (!readVarInt(_rxBuf + 1, _rxLen - 1, remaining, hdr))
                    {
                        logError("MQTT", "malformed packet - reconnecting");
                        reconnect();
                        return;
                    }
                    const size_t total = 1 + hdr + remaining;
                    logWarning("MQTT", "incoming packet of %u B exceeds the %u B buffer, dropped",
                               (unsigned)total, (unsigned)RX_BUF_SIZE);
                    _rxSkip = total - _rxLen;
                    _rxLen = 0;
                }
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
                size_t n = buildConnect(mqttTxBuf, mqttTxBufSize, opts);
                if (n) sendRaw(mqttTxBuf, n);
                _lastActivity = millis();
            }

            void Client::sendPendingSubscriptions()
            {
                while (_subscribedUpTo < _subscriptions.size())
                {
                    auto &sub = _subscriptions[_subscribedUpTo];
                    size_t n = buildSubscribe(mqttTxBuf, mqttTxBufSize,
                                              sub.topic.c_str(), sub.qos, _msgId++);
                    if (!n || !sendRaw(mqttTxBuf, n)) break;
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
                _rxSkip = 0;
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

            // The lwIP callbacks run in the ethernet IRQ and only hand over: data into
            // _rxPbuf, the outcome into _tcpUp/_rxClosed/_pcb. The state machine, packet
            // parsing and subscription callbacks run in loop(), every pcb access under
            // LWIPMutex so the IRQ cannot free the pcb in between.

            bool Client::tcpConnect(const char *host, uint16_t /*port*/)
            {
                tcpDisconnect(); // free any previous pcb (e.g. repeated begin())
                _state = STATE_RESOLVING;
                _lastActivity = millis();

                const uint16_t id = ++_resolveId;
                OpenKNX::Network::DNS::query(host, [this, id](IPAddress ip) {
                    if (id != _resolveId || _state != STATE_RESOLVING) return;
                    onResolved(ip);
                });
                return true;
            }

            void Client::onResolved(IPAddress ip)
            {
                if (ip == IPAddress(0, 0, 0, 0))
                {
                    logError("MQTT", "cannot resolve '%s'", _host);
                    _state = STATE_DISCONNECTED;
                    armReconnectTimer();
                    return;
                }

                ip_addr_t addr;
                IP_ADDR4(&addr, ip[0], ip[1], ip[2], ip[3]);

                _state = STATE_CONNECTING;
                _lastActivity = millis();

                LWIPMutex lock;
                struct tcp_pcb *pcb = tcp_new();
                if (!pcb)
                {
                    _state = STATE_DISCONNECTED;
                    armReconnectTimer();
                    return;
                }
                tcp_arg(pcb, this);
                tcp_recv(pcb, lwipRecv);
                tcp_err(pcb, lwipErr);
                _pcb = pcb;
                if (tcp_connect(pcb, &addr, _port, lwipConnected) != ERR_OK)
                {
                    _pcb = nullptr;
                    tcp_arg(pcb, nullptr);
                    tcp_err(pcb, nullptr);
                    tcp_abort(pcb);
                    _state = STATE_DISCONNECTED;
                    armReconnectTimer();
                }
            }

            void Client::tcpDisconnect()
            {
                LWIPMutex lock;
                if (_rxPbuf)
                {
                    // Otherwise tcp_close() sends RST and drops the queued DISCONNECT
                    if (_pcb) tcp_recved((struct tcp_pcb *)_pcb, _rxPbuf->tot_len);
                    pbuf_free(_rxPbuf);
                    _rxPbuf = nullptr;
                }
                _tcpUp = false;
                _rxClosed = false;

                struct tcp_pcb *pcb = (struct tcp_pcb *)_pcb;
                _pcb = nullptr;
                if (!pcb) return;
                tcp_arg(pcb, nullptr);
                tcp_recv(pcb, nullptr);
                tcp_err(pcb, nullptr);
                if (tcp_close(pcb) != ERR_OK) tcp_abort(pcb);
            }

            bool Client::tcpConnected()
            {
                return _pcb && _tcpUp;
            }

            int Client::tcpRead(uint8_t *buf, size_t maxLen)
            {
                LWIPMutex lock;
                // Buffered data first, even after FIN or an error.
                if (!_rxPbuf) return (_pcb && !_rxClosed) ? 0 : -1;
                if (maxLen > 0xFFFF) maxLen = 0xFFFF;
                const u16_t n = pbuf_copy_partial(_rxPbuf, buf, (u16_t)maxLen, 0);
                _rxPbuf = pbuf_free_header(_rxPbuf, n);
                // Acknowledged only once consumed: a slow loop closes the window instead of losing data.
                if (_pcb) tcp_recved((struct tcp_pcb *)_pcb, n);
                return n;
            }

            bool Client::tcpWrite(const uint8_t *data, size_t len)
            {
                LWIPMutex lock;
                if (!_pcb) return false;
                struct tcp_pcb *pcb = (struct tcp_pcb *)_pcb;
                if (tcp_write(pcb, data, len, TCP_WRITE_FLAG_COPY) != ERR_OK) return false;
                tcp_output(pcb);
                return true;
            }

            bool Client::sendRaw(const uint8_t *data, size_t len)
            {
                return tcpWrite(data, len);
            }

            err_t Client::lwipConnected(void *arg, struct tcp_pcb *, err_t err)
            {
                Client *self = static_cast<Client *>(arg);
                if (self && err == ERR_OK) self->_tcpUp = true;
                return ERR_OK;
            }

            err_t Client::lwipRecv(void *arg, struct tcp_pcb *, struct pbuf *p, err_t err)
            {
                Client *self = static_cast<Client *>(arg);
                if (!p)
                {
                    if (self) self->_rxClosed = true;
                    return ERR_OK;
                }
                if (!self || err != ERR_OK)
                {
                    pbuf_free(p);
                    return ERR_OK;
                }
                if (self->_rxPbuf)
                    pbuf_cat(self->_rxPbuf, p);
                else
                    self->_rxPbuf = p;
                return ERR_OK;
            }

            void Client::lwipErr(void *arg, err_t)
            {
                // The pcb is already freed; loop() sees the missing pcb and reconnects.
                Client *self = static_cast<Client *>(arg);
                if (self) self->_pcb = nullptr;
            }

#endif // platform

        } // namespace MQTT
    } // namespace Network
} // namespace OpenKNX

#endif // OPENKNX_MQTT
#endif // KNX_IP_WIFI || KNX_IP_LAN
