#ifdef ARDUINO_ARCH_RP2040

#include "OpenKNX.h"
#include "OpenKNX/Network/Ping/Handler.h"

#ifdef OPENKNX_PING

#include <Arduino.h>
#include <lwip/icmp.h>
#include <lwip/inet_chksum.h>
#include <lwip/ip.h>
#include <lwip/ip4_addr.h>
#include <lwip/ip_addr.h>
#include <lwip/netif.h>
#include <lwip/pbuf.h>
#include <lwip/raw.h>

static struct raw_pcb* _icmpPcb = nullptr;
static volatile bool _replyReceived[OPENKNX_PING_PARALLEL];
static volatile uint16_t _expectedSeq[OPENKNX_PING_PARALLEL];
static volatile uint32_t _replyTimeMs[OPENKNX_PING_PARALLEL];

static uint8_t icmpRecvCallback(void* arg, struct raw_pcb* pcb, struct pbuf* p, const ip_addr_t* addr)
{
    // raw_recv on arduino-pico lwip passes the full packet including IP header
    if (!p || p->tot_len < sizeof(struct ip_hdr) + sizeof(struct icmp_echo_hdr))
        return 0;

    struct ip_hdr* iphdr = (struct ip_hdr*)p->payload;
    uint16_t iphdr_len = IPH_HL(iphdr) * 4;

    if (p->tot_len < iphdr_len + sizeof(struct icmp_echo_hdr))
        return 0;

    struct icmp_echo_hdr* icmphdr = (struct icmp_echo_hdr*)((uint8_t*)p->payload + iphdr_len);

    if (icmphdr->type == ICMP_ER)
    {
        uint16_t id = ntohs(icmphdr->id);
        uint16_t seq = ntohs(icmphdr->seqno);
        if (id < OPENKNX_PING_PARALLEL && seq == _expectedSeq[id])
        {
            _replyTimeMs[id] = millis();
            _replyReceived[id] = true;
        }
        pbuf_free(p);
        return 1;
    }

    return 0;
}

namespace OpenKNX
{
    namespace Network
    {
        namespace Ping
        {

            bool Handler::platformSendPing(int slotIndex, IPAddress target)
            {
                logTraceP("Ping to %d.%d.%d.%d (slot %d)", target[0], target[1], target[2], target[3], slotIndex);
                if (netif_default == nullptr || !netif_is_up(netif_default) || !netif_is_link_up(netif_default))
                    return false;

                if (!_icmpPcb)
                {
                    _icmpPcb = raw_new(IP_PROTO_ICMP);
                    if (!_icmpPcb)
                    {
                        logErrorP("raw_new failed");
                        return false;
                    }
                    raw_recv(_icmpPcb, icmpRecvCallback, nullptr);
                    raw_bind(_icmpPcb, IP_ADDR_ANY);
                    logTraceP("raw PCB ok, local ip=%s", ip4addr_ntoa(netif_ip4_addr(netif_default)));
                }

                const size_t pingSize = sizeof(struct icmp_echo_hdr) + 32;
                struct pbuf* p = pbuf_alloc(PBUF_IP, pingSize, PBUF_RAM);
                if (!p)
                {
                    logErrorP("pbuf_alloc failed");
                    return false;
                }

                uint16_t seq = _activeSlots[slotIndex].echoSeq;
                _expectedSeq[slotIndex] = seq;
                _replyReceived[slotIndex] = false;

                struct icmp_echo_hdr* iecho = (struct icmp_echo_hdr*)p->payload;
                iecho->type = ICMP_ECHO;
                iecho->code = 0;
                iecho->chksum = 0;
                iecho->id = htons((uint16_t)slotIndex);
                iecho->seqno = htons(seq);

                uint8_t* data = (uint8_t*)p->payload + sizeof(struct icmp_echo_hdr);
                for (int i = 0; i < 32; i++)
                    data[i] = (uint8_t)('a' + i);

                iecho->chksum = inet_chksum(iecho, pingSize);

                ip_addr_t dst;
                IP_ADDR4(&dst, target[0], target[1], target[2], target[3]);

                // Use explicit interface + source to bypass routing table lookup
                ip_addr_t src;
                ip_addr_copy_from_ip4(src, *netif_ip4_addr(netif_default));

                err_t ret = raw_sendto_if_src(_icmpPcb, p, &dst, netif_default, &src);
                pbuf_free(p);

                if (ret != ERR_OK)
                    logErrorP("raw_sendto_if_src err=%d target=%d.%d.%d.%d",
                              (int)ret, target[0], target[1], target[2], target[3]);

                return ret == ERR_OK;
            }

            bool Handler::platformCheckReply(int slotIndex, uint32_t& replyTimeMs)
            {
                if (slotIndex < 0 || slotIndex >= OPENKNX_PING_PARALLEL)
                    return false;

                if (_replyReceived[slotIndex])
                {
                    replyTimeMs = _replyTimeMs[slotIndex];
                    _replyReceived[slotIndex] = false;
                    return true;
                }
                return false;
            }

            void Handler::platformClosePing(int slotIndex)
            {
                if (slotIndex >= 0 && slotIndex < OPENKNX_PING_PARALLEL)
                    _replyReceived[slotIndex] = false;
            }

            void Handler::platformInitialize()
            {
                memset((void*)_replyReceived, 0, sizeof(_replyReceived));
                memset((void*)_expectedSeq, 0, sizeof(_expectedSeq));
                memset((void*)_replyTimeMs, 0, sizeof(_replyTimeMs));
            }

            void Handler::platformCleanup()
            {
                if (_icmpPcb)
                {
                    raw_remove(_icmpPcb);
                    _icmpPcb = nullptr;
                }
            }

        } // namespace Ping
    } // namespace Network
} // namespace OpenKNX

#endif // OPENKNX_PING
#endif // ARDUINO_ARCH_RP2040
