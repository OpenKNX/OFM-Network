#ifdef OPENKNX_WEBCLIENT
#ifdef ARDUINO_ARCH_RP2040

#include <lwip/tcp.h>
#include <lwip_wrap.h> // LWIPMutex: makes the ethernet IRQ defer
#include <bearssl/bearssl.h>
#include "OpenKNX/Network/Webclient/Handler.h"
#include "OpenKNX.h"

// ─── Custom no-verify X.509 validator (uses only standard BearSSL types) ─────
// br_x509_insecure_context is an incomplete type in Arduino headers; we implement
// our own variant using br_x509_decoder_context (fully defined in bearssl_x509.h).

struct NoVerifyX509
{
    const br_x509_class *vtable;
    br_x509_decoder_context dec;
    bool                    gotKey;
    unsigned char           keyBuf[512];
    br_x509_pkey            pk;
};

static void nv_start_chain(const br_x509_class **ctx, const char * /*server_name*/)
{
    NoVerifyX509 *nv = (NoVerifyX509 *)(void *)ctx;
    nv->gotKey = false;
}

static void nv_start_cert(const br_x509_class **ctx, uint32_t /*length*/)
{
    NoVerifyX509 *nv = (NoVerifyX509 *)(void *)ctx;
    // Only decode the first certificate in the chain (the leaf)
    if (!nv->gotKey)
        br_x509_decoder_init(&nv->dec, nullptr, nullptr, nullptr, nullptr);
}

static void nv_append(const br_x509_class **ctx, const unsigned char *buf, size_t len)
{
    NoVerifyX509 *nv = (NoVerifyX509 *)(void *)ctx;
    if (!nv->gotKey)
        br_x509_decoder_push(&nv->dec, buf, len);
}

static void nv_end_cert(const br_x509_class **ctx)
{
    NoVerifyX509 *nv = (NoVerifyX509 *)(void *)ctx;
    if (!nv->gotKey)
    {
        br_x509_pkey *pk = br_x509_decoder_get_pkey(&nv->dec);
        if (pk)
        {
            nv->pk     = *pk;
            nv->gotKey = true;
            // Deep-copy key material into our own buffer so it survives past the decoder
            if (pk->key_type == BR_KEYTYPE_RSA)
            {
                size_t n = pk->key.rsa.nlen + pk->key.rsa.elen;
                if (n <= sizeof(nv->keyBuf))
                {
                    memcpy(nv->keyBuf, pk->key.rsa.n, pk->key.rsa.nlen);
                    memcpy(nv->keyBuf + pk->key.rsa.nlen, pk->key.rsa.e, pk->key.rsa.elen);
                    nv->pk.key.rsa.n = nv->keyBuf;
                    nv->pk.key.rsa.e = nv->keyBuf + pk->key.rsa.nlen;
                }
            }
            else if (pk->key_type == BR_KEYTYPE_EC)
            {
                size_t n = pk->key.ec.qlen;
                if (n <= sizeof(nv->keyBuf))
                {
                    memcpy(nv->keyBuf, pk->key.ec.q, n);
                    nv->pk.key.ec.q = nv->keyBuf;
                }
            }
        }
    }
}

static unsigned nv_end_chain(const br_x509_class ** /*ctx*/)
{
    return 0; // always accept
}

static const br_x509_pkey *nv_get_pkey(const br_x509_class * const *ctx, unsigned *usages)
{
    NoVerifyX509 *nv = (NoVerifyX509 *)(void *)ctx;
    if (usages) *usages = BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN;
    return nv->gotKey ? &nv->pk : nullptr;
}

static const br_x509_class nv_vtable = {
    sizeof(NoVerifyX509),
    nv_start_chain,
    nv_start_cert,
    nv_append,
    nv_end_cert,
    nv_end_chain,
    nv_get_pkey,
};

static void noVerifyX509Init(NoVerifyX509 *nv)
{
    memset(nv, 0, sizeof(*nv));
    nv->vtable = &nv_vtable;
}

// ─── Full BrState definition (forward-declared in Handler.h as Webclient::BrState) ──────

namespace OpenKNX { namespace Network { namespace Webclient {

struct BrState
{
    br_ssl_client_context   client;
    br_x509_minimal_context x509min; // required by br_ssl_client_init_full
    NoVerifyX509            x509ins; // our always-accept validator
    uint8_t iobuf_in[4096 + 325];   // TLS record input buffer
    uint8_t iobuf_out[512 + 85];    // TLS record output buffer
};

} } } // namespace OpenKNX::Network::Webclient

namespace OpenKNX
{
    namespace Network
    {
        namespace Webclient
        {
            // ─── lwIP callbacks ───────────────────────────────────────────────────────
            // They run in the ethernet IRQ and only hand over: pbufs into rxPbuf, the
            // outcome into tcpConnEst/tcpError/rxClosed/tcpPcb. The loop touches pcb and
            // pbuf chain only under LWIPMutex (rawPbufRead, rawTcpWrite, platformClose).

            err_t Handler::lwipConnected(void *arg, struct tcp_pcb * /*pcb*/, err_t err)
            {
                Slot *s = static_cast<Slot *>(arg);
                if (!s) return ERR_OK;
                if (err != ERR_OK)
                    s->tcpError = true;
                else
                    s->tcpConnEst = true;
                return ERR_OK;
            }

            err_t Handler::lwipRecv(void *arg, struct tcp_pcb * /*pcb*/, struct pbuf *p, err_t err)
            {
                Slot *s = static_cast<Slot *>(arg);
                if (!p)
                {
                    // FIN: the pcb stays ours until platformClose(); data before it is still read
                    if (s) s->rxClosed = true;
                    return ERR_OK;
                }
                if (!s || err != ERR_OK)
                {
                    pbuf_free(p);
                    return ERR_OK;
                }
                // Zero-copy; tcp_recved follows in rawPbufRead as bytes are consumed.
                if (s->rxPbuf)
                    pbuf_cat(s->rxPbuf, p);
                else
                    s->rxPbuf = p;
                return ERR_OK;
            }

            void Handler::lwipErr(void *arg, err_t /*err*/)
            {
                Slot *s = static_cast<Slot *>(arg);
                if (!s) return;
                // PCB has already been freed by lwIP — do NOT call tcp_close()
                s->tcpPcb   = nullptr;
                s->tcpError = true;
            }

            // ─── Raw TCP access: the only places that touch pcb and pbuf chain ───────

            static int rawPbufRead(Slot &s, unsigned char *buf, size_t len)
            {
                LWIPMutex lock;
                if (!s.rxPbuf || len == 0) return 0;
                if (len > 0xFFFF) len = 0xFFFF;
                const u16_t n = pbuf_copy_partial(s.rxPbuf, buf, (u16_t)len, 0);
                s.rxPbuf = pbuf_free_header(s.rxPbuf, n);
                if (s.tcpPcb) tcp_recved(s.tcpPcb, n);
                return (int)n;
            }

            // Bytes queued; 0 while the send buffer is full, -1 without a connection.
            static int rawTcpWrite(Slot &s, const void *data, size_t len)
            {
                LWIPMutex lock;
                struct tcp_pcb *pcb = s.tcpPcb;
                if (!pcb) return -1;
                const size_t room = tcp_sndbuf(pcb);
                const size_t n = (len < room) ? len : room;
                if (n == 0) return 0;
                const err_t err = tcp_write(pcb, data, (u16_t)n, TCP_WRITE_FLAG_COPY);
                if (err == ERR_MEM) return 0;
                if (err != ERR_OK) return -1;
                tcp_output(pcb);
                return (int)n;
            }

            // BearSSL itself runs outside the lock: record processing can take a while.
            static void flushTlsRecords(Slot &s, br_ssl_engine_context &eng)
            {
                size_t len;
                unsigned char *buf = br_ssl_engine_sendrec_buf(&eng, &len);
                if (!buf || len == 0) return;
                const int n = rawTcpWrite(s, buf, len);
                if (n > 0) br_ssl_engine_sendrec_ack(&eng, (size_t)n);
            }

            static void feedTlsRecords(Slot &s, br_ssl_engine_context &eng)
            {
                size_t len;
                unsigned char *buf = br_ssl_engine_recvrec_buf(&eng, &len);
                if (!buf || len == 0) return;
                const int n = rawPbufRead(s, buf, len);
                if (n > 0) br_ssl_engine_recvrec_ack(&eng, (size_t)n);
            }

            // ─── Platform methods ─────────────────────────────────────────────────────

            bool Handler::platformConnect(Slot &s, IPAddress ip, uint16_t port)
            {
                // Both plain HTTP and SSL use our non-blocking lwIP raw tcp_ API.
                // WiFiClientSecure is NOT used: its ClientContext::connect() spin loop
                // blocks for 5 s on W5500 because its default TCP connect timeout is 5000 ms
                // and the TCP SYN-ACK is not delivered during the spin loop on W5500.
                ip_addr_t addr;
                IP_ADDR4(&addr, ip[0], ip[1], ip[2], ip[3]);

                LWIPMutex lock;
                struct tcp_pcb *pcb = tcp_new();
                if (!pcb) return false;

                if (s.rxPbuf) pbuf_free(s.rxPbuf);
                s.rxPbuf     = nullptr;
                s.rxClosed   = false;
                s.tcpConnEst = false;
                s.tcpError   = false;
                s.tcpPcb     = pcb;

                tcp_arg(pcb, &s);
                tcp_recv(pcb, lwipRecv);
                tcp_err(pcb, lwipErr);

                if (tcp_connect(pcb, &addr, port, lwipConnected) != ERR_OK)
                {
                    s.tcpPcb = nullptr;
                    tcp_arg(pcb, nullptr);
                    tcp_err(pcb, nullptr);
                    tcp_abort(pcb);
                    return false;
                }
                return true;
            }

            bool Handler::platformConnectComplete(Slot &s)
            {
                return s.tcpConnEst;
            }

            bool Handler::platformConnected(Slot &s)
            {
                if (s.tcpError) return false;
                if (s.ssl && s.brState)
                {
                    unsigned st = br_ssl_engine_current_state(&s.brState->client.eng);
                    if (st == BR_SSL_CLOSED) return false;
                    if (st & BR_SSL_RECVAPP) return true; // decrypted data still to read after FIN
                }
                if (s.tcpPcb && !s.rxClosed) return true;
                return s.rxPbuf != nullptr;
            }

            // ─── BearSSL handshake over lwIP (incremental, non-blocking) ─────────────

            bool Handler::platformSslInit(Slot &s)
            {
                s.brState = psram_new(BrState)();
                if (!s.brState) return false;

                br_ssl_client_context &cl = s.brState->client;
                br_ssl_client_init_full(&cl, &s.brState->x509min, nullptr, 0);

                noVerifyX509Init(&s.brState->x509ins);
                br_ssl_engine_set_x509(&cl.eng, &s.brState->x509ins.vtable);

                br_ssl_engine_set_buffers_bidi(&cl.eng,
                    s.brState->iobuf_in,  sizeof(s.brState->iobuf_in),
                    s.brState->iobuf_out, sizeof(s.brState->iobuf_out));

                if (!br_ssl_client_reset(&cl, s.host, 0))
                {
                    psram_delete(s.brState);
                    s.brState = nullptr;
                    return false;
                }
                return true;
            }

            int Handler::platformSslStep(Slot &s)
            {
                if (!s.brState || s.tcpError) return -1;
                br_ssl_engine_context &eng = s.brState->client.eng;
                unsigned st = br_ssl_engine_current_state(&eng);

                if (st == BR_SSL_CLOSED)
                {
                    logError("Webclient", "SSL handshake failed: BearSSL error %d",
                             br_ssl_engine_last_error(&eng));
                    return -1;
                }

                if (st & BR_SSL_SENDAPP) return 1; // Handshake complete

                if (st & BR_SSL_SENDREC) flushTlsRecords(s, eng);

                st = br_ssl_engine_current_state(&eng);
                if (st & BR_SSL_RECVREC) feedTlsRecords(s, eng);

                return 0; // In progress — call again next loop()
            }

            // ─── platformSend / platformRead / platformClose ──────────────────────────

            int Handler::platformSend(Slot &s, const uint8_t *data, size_t len)
            {
                if (s.ssl)
                {
                    if (!s.brState || !s.tcpPcb) return -1;
                    br_ssl_engine_context &eng = s.brState->client.eng;
                    unsigned st = br_ssl_engine_current_state(&eng);
                    if (st == BR_SSL_CLOSED) return -1;

                    // Flush pending outgoing TLS record before accepting new app data
                    if (st & BR_SSL_SENDREC)
                    {
                        flushTlsRecords(s, eng);
                        return 0; // yield — come back next loop iteration
                    }

                    if (!(st & BR_SSL_SENDAPP)) return 0;

                    size_t alen;
                    unsigned char *abuf = br_ssl_engine_sendapp_buf(&eng, &alen);
                    if (!abuf || alen == 0) return 0;

                    size_t toWrite = (len < alen) ? len : alen;
                    memcpy(abuf, data, toWrite);
                    br_ssl_engine_sendapp_ack(&eng, toWrite);
                    br_ssl_engine_flush(&eng, 0);
                    return (int)toWrite;
                }

                return rawTcpWrite(s, data, len);
            }

            int Handler::platformRead(Slot &s, uint8_t *buf, size_t len)
            {
                if (s.ssl)
                {
                    if (!s.brState) return -1;
                    br_ssl_engine_context &eng = s.brState->client.eng;
                    unsigned st = br_ssl_engine_current_state(&eng);

                    // Forward any pending outgoing TLS records (e.g. TLS alerts, ACKs)
                    if (st & BR_SSL_SENDREC) flushTlsRecords(s, eng);

                    // Feed incoming raw TLS record data from pbuf chain into BearSSL
                    st = br_ssl_engine_current_state(&eng);
                    if (st & BR_SSL_RECVREC) feedTlsRecords(s, eng);

                    // Return decrypted application data if available
                    st = br_ssl_engine_current_state(&eng);
                    if (st & BR_SSL_RECVAPP)
                    {
                        size_t alen;
                        const unsigned char *abuf = br_ssl_engine_recvapp_buf(&eng, &alen);
                        size_t copy = (len < alen) ? len : alen;
                        memcpy(buf, abuf, copy);
                        br_ssl_engine_recvapp_ack(&eng, copy);
                        return (int)copy;
                    }

                    if (st == BR_SSL_CLOSED) return -1; // TLS session terminated
                    return 0;
                }

                // Plain HTTP: consume from pbuf chain
                return rawPbufRead(s, buf, len);
            }

            void Handler::platformClose(Slot &s)
            {
                if (s.ssl && s.brState)
                {
                    br_ssl_engine_close(&s.brState->client.eng);
                    psram_delete(s.brState);
                    s.brState = nullptr;
                }

                LWIPMutex lock;
                if (s.rxPbuf)
                {
                    // Otherwise tcp_close() answers with RST instead of FIN
                    if (s.tcpPcb) tcp_recved(s.tcpPcb, s.rxPbuf->tot_len);
                    pbuf_free(s.rxPbuf); // frees the entire remaining chain
                    s.rxPbuf = nullptr;
                }
                s.rxClosed = false;
                struct tcp_pcb *pcb = s.tcpPcb;
                s.tcpPcb = nullptr;
                if (pcb)
                {
                    tcp_arg(pcb, nullptr);
                    tcp_recv(pcb, nullptr);
                    tcp_err(pcb, nullptr);
                    if (tcp_close(pcb) != ERR_OK) tcp_abort(pcb);
                }
            }

        } // namespace Webclient
    }     // namespace Network
} // namespace OpenKNX

#endif // ARDUINO_ARCH_RP2040
#endif // OPENKNX_WEBCLIENT
