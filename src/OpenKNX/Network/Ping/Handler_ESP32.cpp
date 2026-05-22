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

                _activeSlots[slotIndex].sock = _sharedSock;
                return ret >= 0;
            }

            bool Handler::platformCheckReply(int slotIndex, uint32_t& replyTimeMs)
            {
                if (_sharedSock < 0)
                    return false;

                char buffer[64];
                struct sockaddr_in fromaddr;
                socklen_t fromlen = sizeof(fromaddr);

                int recvlen = recvfrom(_sharedSock, buffer, sizeof(buffer), MSG_DONTWAIT,
                                       (struct sockaddr*)&fromaddr, &fromlen);

                if (recvlen <= 0)
                    return false;

                struct ip_hdr* iphdr = (struct ip_hdr*)buffer;
                int iphdr_len = IPH_HL(iphdr) * 4;

                if (recvlen < iphdr_len + (int)sizeof(struct icmp_echo_hdr))
                    return false;

                struct icmp_echo_hdr* reply = (struct icmp_echo_hdr*)(buffer + iphdr_len);

                if (reply->type == ICMP_ER &&
                    reply->id == htons(slotIndex) &&
                    reply->seqno == htons(_activeSlots[slotIndex].echoSeq))
                {
                    replyTimeMs = millis();
                    return true;
                }

                return false;
            }

            void Handler::platformClosePing(int slotIndex)
            {
                _activeSlots[slotIndex].sock = -1;
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
