#ifdef OPENKNX_WEBCLIENT
#ifdef ARDUINO_ARCH_RP2040

#include <lwip/tcp.h>
#include <bearssl/bearssl.h>
#include "OpenKNX/Crypto/TrustAnchor.h"
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
    NoVerifyX509            x509ins; // always-accept validator, used only for VerifyMode::NONE
    OpenKNX::Crypto::TrustAnchor ta;  // decoded root, used only for VerifyMode::CERTIFICATE
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

            err_t Handler::lwipConnected(void *arg, struct tcp_pcb * /*pcb*/, err_t err)
            {
                Slot *s = static_cast<Slot *>(arg);
                if (!s) return ERR_OK;
                if (err != ERR_OK)
                {
                    s->tcpError = true;
                    return ERR_VAL;
                }
                s->tcpConnEst = true;
                return ERR_OK;
            }

            err_t Handler::lwipRecv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t /*err*/)
            {
                Slot *s = static_cast<Slot *>(arg);
                if (!s) return ERR_OK;
                if (!p)
                {
                    // Server closed connection — complete the TCP teardown and forget the PCB
                    tcp_arg(pcb, nullptr);
                    tcp_recv(pcb, nullptr);
                    tcp_err(pcb, nullptr);
                    tcp_close(pcb);
                    s->tcpPcb = nullptr;
                    return ERR_OK;
                }
                // Append pbuf to our receive chain without copying.
                // tcp_recved is called from platformRead as bytes are actually consumed.
                if (!s->rxHead)
                {
                    s->rxHead = p;
                    s->rxTail = p;
                }
                else
                {
                    s->rxTail->next = p;
                }
                while (s->rxTail->next)
                    s->rxTail = s->rxTail->next;
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

            // ─── Raw pbuf read (used by BearSSL handshake and read loop) ─────────────

            static int rawPbufRead(Slot &s, unsigned char *buf, size_t len)
            {
                size_t n = 0;
                while (n < len && s.rxHead)
                {
                    size_t avail = s.rxHead->len - s.rxOffset;
                    size_t take  = (avail < len - n) ? avail : (len - n);
                    memcpy(buf + n, (const uint8_t *)s.rxHead->payload + s.rxOffset, take);
                    n          += take;
                    s.rxOffset += (uint16_t)take;

                    if (s.rxOffset >= s.rxHead->len)
                    {
                        struct pbuf *done = s.rxHead;
                        s.rxHead = done->next;
                        if (!s.rxHead) s.rxTail = nullptr;
                        done->next = nullptr;
                        pbuf_free(done);
                        s.rxOffset = 0;
                    }
                }
                if (n > 0 && s.tcpPcb)
                    tcp_recved(s.tcpPcb, (u16_t)n);
                return (int)n;
            }

            // ─── Platform methods ─────────────────────────────────────────────────────

            bool Handler::platformConnect(Slot &s, IPAddress ip, uint16_t port)
            {
                // Both plain HTTP and SSL use our non-blocking lwIP raw tcp_ API.
                // WiFiClientSecure is NOT used: its ClientContext::connect() spin loop
                // blocks for 5 s on W5500 because its default TCP connect timeout is 5000 ms
                // and the TCP SYN-ACK is not delivered during the spin loop on W5500.
                struct tcp_pcb *pcb = tcp_new();
                if (!pcb) return false;

                s.tcpConnEst = false;
                s.tcpError   = false;
                s.rxHead     = nullptr;
                s.rxTail     = nullptr;
                s.rxOffset   = 0;
                s.tcpPcb     = pcb;

                tcp_arg(pcb, &s);
                tcp_recv(pcb, lwipRecv);
                tcp_err(pcb, lwipErr);

                ip_addr_t addr;
                IP_ADDR4(&addr, ip[0], ip[1], ip[2], ip[3]);

                err_t err = tcp_connect(pcb, &addr, port, lwipConnected);
                if (err != ERR_OK)
                {
                    tcp_abort(pcb);
                    s.tcpPcb = nullptr;
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
                }
                if (s.tcpPcb != nullptr) return true;
                return s.rxHead != nullptr;
            }

            // ─── BearSSL handshake over lwIP (incremental, non-blocking) ─────────────

            bool Handler::platformSslInit(Slot &s)
            {
                s.brState = psram_new(BrState)();
                if (!s.brState) return false;

                br_ssl_client_context &cl = s.brState->client;
                br_ssl_client_init_full(&cl, &s.brState->x509min, nullptr, 0);

                if (s.verifyMode == VerifyMode::CERTIFICATE && s.verify != nullptr)
                {
                    uint32_t days = 0, seconds = 0;
                    if (!s.brState->ta.fromPem(s.verify))
                    {
                        logError("Webclient", "certificate check: root certificate rejected (%d)",
                                 s.brState->ta.lastError());
                        psram_delete(s.brState);
                        s.brState = nullptr;
                        return false;
                    }
                    // BearSSL treats "no validation time" as a validation failure, so a device
                    // without a valid clock must not pretend to have checked the chain.
                    if (!OpenKNX::Crypto::TrustAnchor::currentTime(days, seconds))
                    {
                        logError("Webclient", "certificate check: no valid device time");
                        psram_delete(s.brState);
                        s.brState = nullptr;
                        return false;
                    }
                    br_x509_minimal_init_full(&s.brState->x509min, s.brState->ta.anchor(), 1);
                    br_x509_minimal_set_time(&s.brState->x509min, days, seconds);
                    br_ssl_engine_set_x509(&cl.eng, &s.brState->x509min.vtable);
                }
                else
                {
                    noVerifyX509Init(&s.brState->x509ins);
                    br_ssl_engine_set_x509(&cl.eng, &s.brState->x509ins.vtable);
                }

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


#ifdef OPENKNX_SSL_STEP_PROFILE
            /**
             * @brief Stall profile of a single platformSslStep() call.
             * Records how long one loop() pass is held up by the TLS engine; the summary is
             * logged once the handshake completes. Compiled out unless the flag is set.
             */
            struct SslStepProfile
            {
                static uint32_t maxUs, sumUs, count, over2ms, stateAtMax;
                uint32_t t0;
                unsigned state = 0;
                bool done = false;

                SslStepProfile() : t0(micros()) {}
                ~SslStepProfile()
                {
                    uint32_t dt = micros() - t0;
                    if (dt > maxUs)
                    {
                        maxUs = dt;
                        stateAtMax = state;
                    }
                    sumUs += dt;
                    count++;
                    if (dt > 2000) over2ms++;
                    if (done)
                    {
                        logInfo("SslProfile", "handshake done: %u steps, max %u us (state 0x%02X), avg %u us, %u steps > 2 ms",
                                count, maxUs, stateAtMax, count ? sumUs / count : 0, over2ms);
                        maxUs = sumUs = count = over2ms = stateAtMax = 0;
                    }
                }
            };
            uint32_t SslStepProfile::maxUs = 0;
            uint32_t SslStepProfile::sumUs = 0;
            uint32_t SslStepProfile::count = 0;
            uint32_t SslStepProfile::over2ms = 0;
            uint32_t SslStepProfile::stateAtMax = 0;
#endif

            int Handler::platformSslStep(Slot &s)
            {
#ifdef OPENKNX_SSL_STEP_PROFILE
                SslStepProfile _prof;
#endif
                if (!s.brState || s.tcpError) return -1;
                br_ssl_engine_context &eng = s.brState->client.eng;
                unsigned st = br_ssl_engine_current_state(&eng);
#ifdef OPENKNX_SSL_STEP_PROFILE
                _prof.state = st;
#endif

                if (st == BR_SSL_CLOSED)
                {
                    logError("Webclient", "SSL handshake failed: BearSSL error %d",
                             br_ssl_engine_last_error(&eng));
                    return -1;
                }

                if (st & BR_SSL_SENDAPP)
                {
#ifdef OPENKNX_SSL_STEP_PROFILE
                    _prof.done = true;
#endif
                    return 1; // Handshake complete
                }

                if ((st & BR_SSL_SENDREC) && s.tcpPcb)
                {
                    size_t len;
                    unsigned char *buf = br_ssl_engine_sendrec_buf(&eng, &len);
                    uint16_t sndbuf = tcp_sndbuf(s.tcpPcb);
                    if (sndbuf > 0 && len > 0)
                    {
                        size_t toSend = (len < (size_t)sndbuf) ? len : (size_t)sndbuf;
                        if (tcp_write(s.tcpPcb, buf, (u16_t)toSend, TCP_WRITE_FLAG_COPY) == ERR_OK)
                        {
                            tcp_output(s.tcpPcb);
                            br_ssl_engine_sendrec_ack(&eng, toSend);
                        }
                    }
                }

                st = br_ssl_engine_current_state(&eng);
                if (st & BR_SSL_RECVREC)
                {
                    size_t len;
                    unsigned char *buf = br_ssl_engine_recvrec_buf(&eng, &len);
                    int n = rawPbufRead(s, buf, len);
                    if (n > 0)
                        br_ssl_engine_recvrec_ack(&eng, (size_t)n);
                }

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
                        size_t rlen;
                        unsigned char *rbuf = br_ssl_engine_sendrec_buf(&eng, &rlen);
                        uint16_t sndbuf = tcp_sndbuf(s.tcpPcb);
                        if (sndbuf > 0 && rlen > 0)
                        {
                            size_t toSend = (rlen < (size_t)sndbuf) ? rlen : (size_t)sndbuf;
                            if (tcp_write(s.tcpPcb, rbuf, (u16_t)toSend, TCP_WRITE_FLAG_COPY) == ERR_OK)
                            {
                                tcp_output(s.tcpPcb);
                                br_ssl_engine_sendrec_ack(&eng, toSend);
                            }
                        }
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

                if (!s.tcpPcb) return -1;
                uint16_t sndbuf = tcp_sndbuf(s.tcpPcb);
                if (sndbuf == 0) return 0;
                size_t toSend = (len < (size_t)sndbuf) ? len : (size_t)sndbuf;
                err_t err = tcp_write(s.tcpPcb, data, (u16_t)toSend, TCP_WRITE_FLAG_COPY);
                if (err != ERR_OK) return -1;
                tcp_output(s.tcpPcb);
                return (int)toSend;
            }

            int Handler::platformRead(Slot &s, uint8_t *buf, size_t len)
            {
                if (s.ssl)
                {
                    if (!s.brState) return -1;
                    br_ssl_engine_context &eng = s.brState->client.eng;
                    unsigned st = br_ssl_engine_current_state(&eng);

                    // Forward any pending outgoing TLS records (e.g. TLS alerts, ACKs)
                    if ((st & BR_SSL_SENDREC) && s.tcpPcb)
                    {
                        size_t rlen;
                        unsigned char *rbuf = br_ssl_engine_sendrec_buf(&eng, &rlen);
                        uint16_t sndbuf = tcp_sndbuf(s.tcpPcb);
                        if (sndbuf > 0 && rlen > 0)
                        {
                            size_t toSend = (rlen < (size_t)sndbuf) ? rlen : (size_t)sndbuf;
                            if (tcp_write(s.tcpPcb, rbuf, (u16_t)toSend, TCP_WRITE_FLAG_COPY) == ERR_OK)
                            {
                                tcp_output(s.tcpPcb);
                                br_ssl_engine_sendrec_ack(&eng, toSend);
                            }
                        }
                    }

                    // Feed incoming raw TLS record data from pbuf chain into BearSSL
                    st = br_ssl_engine_current_state(&eng);
                    if (st & BR_SSL_RECVREC)
                    {
                        size_t rlen;
                        unsigned char *rbuf = br_ssl_engine_recvrec_buf(&eng, &rlen);
                        int n = rawPbufRead(s, rbuf, rlen);
                        if (n > 0)
                            br_ssl_engine_recvrec_ack(&eng, (size_t)n);
                    }

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
                size_t n = 0;
                while (n < len && s.rxHead)
                {
                    size_t avail = s.rxHead->len - s.rxOffset;
                    size_t take  = (avail < len - n) ? avail : (len - n);
                    memcpy(buf + n, (const uint8_t *)s.rxHead->payload + s.rxOffset, take);
                    n          += take;
                    s.rxOffset += (uint16_t)take;

                    if (s.rxOffset >= s.rxHead->len)
                    {
                        struct pbuf *done = s.rxHead;
                        s.rxHead = done->next;
                        if (!s.rxHead) s.rxTail = nullptr;
                        done->next = nullptr;
                        pbuf_free(done);
                        s.rxOffset = 0;
                    }
                }
                if (n > 0 && s.tcpPcb)
                    tcp_recved(s.tcpPcb, (u16_t)n);
                return (int)n;
            }

            void Handler::platformClose(Slot &s)
            {
                if (s.ssl && s.brState)
                {
                    br_ssl_engine_close(&s.brState->client.eng);
                    psram_delete(s.brState);
                    s.brState = nullptr;
                }

                if (s.tcpPcb)
                {
                    tcp_arg(s.tcpPcb, nullptr);
                    tcp_recv(s.tcpPcb, nullptr);
                    tcp_err(s.tcpPcb, nullptr);
                    tcp_close(s.tcpPcb);
                    s.tcpPcb = nullptr;
                }
                if (s.rxHead)
                {
                    pbuf_free(s.rxHead); // frees the entire remaining chain
                    s.rxHead = nullptr;
                    s.rxTail = nullptr;
                }
            }

        } // namespace Webclient
    }     // namespace Network
} // namespace OpenKNX

#endif // ARDUINO_ARCH_RP2040
#endif // OPENKNX_WEBCLIENT
