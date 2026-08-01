#ifdef ARDUINO_ARCH_ESP32

#include "OpenKNX/Network/Ping/Handler.h"

#ifdef OPENKNX_PING

#include <Arduino.h>
#include <lwip/icmp.h>
#include <lwip/inet.h>
#include <lwip/inet_chksum.h>
#include <lwip/ip.h>
#include <lwip/mem.h>
#include <lwip/sockets.h>
#include <string.h>

static int _sharedSock = -1;

// Antworten vom gemeinsamen Socket, nach Slot abgelegt (siehe platformCheckReply)
static bool _replyPending[OPENKNX_PING_PARALLEL] = {};
static uint32_t _replyTime[OPENKNX_PING_PARALLEL] = {};

namespace OpenKNX
{
    namespace Network
    {
        namespace Ping
        {

            bool Handler::platformSendPing(int slotIndex, IPAddress target)
            {
                if (_sharedSock < 0)
                {
                    _sharedSock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
                    if (_sharedSock < 0)
                        return false;

                    int flags = fcntl(_sharedSock, F_GETFL, 0);
                    fcntl(_sharedSock, F_SETFL, flags | O_NONBLOCK);
                }

                _replyPending[slotIndex] = false;

                struct icmp_echo_hdr iecho;
                iecho.type = ICMP_ECHO;
                iecho.code = 0;
                iecho.chksum = 0;
                iecho.id = htons(slotIndex);
                iecho.seqno = htons(_activeSlots[slotIndex].echoSeq);

                uint8_t payload[sizeof(iecho) + 32];
                memcpy(payload, &iecho, sizeof(iecho));
                for (int i = 0; i < 32; i++)
                    payload[sizeof(iecho) + i] = (uint8_t)('a' + i);

                struct icmp_echo_hdr* phdr = (struct icmp_echo_hdr*)payload;
                phdr->chksum = inet_chksum(payload, sizeof(payload));

                struct sockaddr_in toaddr;
                memset(&toaddr, 0, sizeof(toaddr));
                toaddr.sin_family = AF_INET;
                toaddr.sin_addr.s_addr = static_cast<uint32_t>(target);

                int ret = sendto(_sharedSock, payload, sizeof(payload), 0,
                                 (struct sockaddr*)&toaddr, sizeof(toaddr));
                return ret >= 0;
            }

            bool Handler::platformCheckReply(int slotIndex, uint32_t& replyTimeMs)
            {
                if (_sharedSock < 0)
                    return false;

                // Socket komplett leeren und jede Antwort ihrem Slot zuordnen —
                // sonst verwirft die Abfrage von Slot i die Antwort für Slot j
                for (;;)
                {
                    char buffer[64];
                    struct sockaddr_in fromaddr;
                    socklen_t fromlen = sizeof(fromaddr);

                    int recvlen = recvfrom(_sharedSock, buffer, sizeof(buffer), MSG_DONTWAIT,
                                           (struct sockaddr*)&fromaddr, &fromlen);
                    if (recvlen <= 0)
                        break;

                    struct ip_hdr* iphdr = (struct ip_hdr*)buffer;
                    int iphdr_len = IPH_HL(iphdr) * 4;
                    if (iphdr_len < (int)sizeof(struct ip_hdr) ||
                        recvlen < iphdr_len + (int)sizeof(struct icmp_echo_hdr))
                        continue;

                    struct icmp_echo_hdr* reply = (struct icmp_echo_hdr*)(buffer + iphdr_len);
                    if (reply->type != ICMP_ER)
                        continue;

                    uint16_t id = ntohs(reply->id); // = Slot-Index

                    if (id >= OPENKNX_PING_PARALLEL)
                        continue;
                    if (_activeSlots[id].state != PingRequest::PINGING)
                        continue;
                    if (reply->seqno != htons(_activeSlots[id].echoSeq))
                        continue;

                    _replyPending[id] = true;
                    _replyTime[id] = millis();
                }

                if (slotIndex < 0 || slotIndex >= OPENKNX_PING_PARALLEL || !_replyPending[slotIndex])
                    return false;

                _replyPending[slotIndex] = false;
                replyTimeMs = _replyTime[slotIndex];
                return true;
            }

            void Handler::platformClosePing(int slotIndex)
            {
                if (slotIndex >= 0 && slotIndex < OPENKNX_PING_PARALLEL)
                    _replyPending[slotIndex] = false;
            }

            void Handler::platformInitialize() {}

            void Handler::platformCleanup()
            {
                if (_sharedSock >= 0)
                {
                    close(_sharedSock);
                    _sharedSock = -1;
                }
            }

        } // namespace Ping
    } // namespace Network
} // namespace OpenKNX

#endif // OPENKNX_PING
#endif // ARDUINO_ARCH_ESP32
