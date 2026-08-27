#include "OpenKNX/Crypto/TrustAnchor.h"

#if defined(ARDUINO_ARCH_RP2040) && defined(OPENKNX_WEBCLIENT)

    #include "OpenKNX.h"
    #include <cstring>
    #include <new>

namespace OpenKNX
{
    namespace Crypto
    {
        // Unix epoch (1970-01-01) expressed in BearSSL's "days since 0 AD" scale.
        static const uint32_t BEARSSL_UNIX_EPOCH_DAYS = 719528;

        /** @brief base64 character to 6-bit value; 0xFF = not a base64 character. */
        static uint8_t b64Value(char c)
        {
            if (c >= 'A' && c <= 'Z') return (uint8_t)(c - 'A');
            if (c >= 'a' && c <= 'z') return (uint8_t)(c - 'a' + 26);
            if (c >= '0' && c <= '9') return (uint8_t)(c - '0' + 52);
            if (c == '+') return 62;
            if (c == '/') return 63;
            return 0xFF;
        }

        void TrustAnchor::appendDn(void* ctx, const void* buf, size_t len)
        {
            TrustAnchor* ta = (TrustAnchor*)ctx;
            if (ta->_dnLen + len > sizeof(ta->_dn))
            {
                ta->_dnOverflow = true;
                return;
            }
            memcpy(ta->_dn + ta->_dnLen, buf, len);
            ta->_dnLen += len;
        }

        bool TrustAnchor::fromPem(const char* pem)
        {
            // br_x509_decoder_context is ~1.2 KB (pkey_data 520 + pad 256 + two 128 B stacks);
            // the loop() stack has far less headroom than that, so it goes on the heap.
            br_x509_decoder_context* dec = new (std::nothrow) br_x509_decoder_context;
            if (dec == nullptr)
            {
                _ok = false;
                _err = ERR_NO_MEMORY;
                return false;
            }
            bool result = decode(*dec, pem);
            delete dec;
            return result;
        }

        bool TrustAnchor::decode(br_x509_decoder_context& dec, const char* pem)
        {
            _ok = false;
            _err = 0;
            _dnLen = 0;
            _dnOverflow = false;
            if (pem == nullptr) return (_err = ERR_NO_PEM), false;

            const char* begin = strstr(pem, "-----BEGIN CERTIFICATE-----");
            if (begin == nullptr) return (_err = ERR_NO_PEM), false;
            begin = strchr(begin, '\n');
            if (begin == nullptr) return (_err = ERR_NO_PEM), false;
            begin++;
            const char* end = strstr(begin, "-----END CERTIFICATE-----");
            if (end == nullptr) return (_err = ERR_NO_PEM), false;

            br_x509_decoder_init(&dec, appendDn, this, nullptr, nullptr);

            uint8_t out[48];
            size_t outLen = 0;
            uint32_t acc = 0;
            uint8_t accBits = 0;
            for (const char* p = begin; p < end; p++)
            {
                if (*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t') continue;
                if (*p == '=') break; // padding: no further payload bits
                uint8_t v = b64Value(*p);
                if (v == 0xFF) return (_err = ERR_BASE64), false;
                acc = (acc << 6) | v;
                accBits += 6;
                if (accBits >= 8)
                {
                    accBits -= 8;
                    out[outLen++] = (uint8_t)(acc >> accBits);
                    if (outLen == sizeof(out))
                    {
                        br_x509_decoder_push(&dec, out, outLen);
                        outLen = 0;
                    }
                }
            }
            if (outLen) br_x509_decoder_push(&dec, out, outLen);

            int err = br_x509_decoder_last_error(&dec);
            if (err != 0) return (_err = err), false;
            if (_dnOverflow) return (_err = ERR_DN_SIZE), false;
            if (!br_x509_decoder_isCA(&dec)) return (_err = ERR_NOT_CA), false;

            br_x509_pkey* pk = br_x509_decoder_get_pkey(&dec);
            if (pk == nullptr) return (_err = ERR_NO_KEY), false;

            // The decoder's key points into its own context, which dies with this call --
            // copy the key material into our buffer and re-point the anchor at the copy.
            _ta.pkey = *pk;
            if (pk->key_type == BR_KEYTYPE_RSA)
            {
                size_t n = pk->key.rsa.nlen + pk->key.rsa.elen;
                if (n > sizeof(_key)) return (_err = ERR_KEY_SIZE), false;
                memcpy(_key, pk->key.rsa.n, pk->key.rsa.nlen);
                memcpy(_key + pk->key.rsa.nlen, pk->key.rsa.e, pk->key.rsa.elen);
                _ta.pkey.key.rsa.n = _key;
                _ta.pkey.key.rsa.e = _key + pk->key.rsa.nlen;
            }
            else if (pk->key_type == BR_KEYTYPE_EC)
            {
                if (pk->key.ec.qlen > sizeof(_key)) return (_err = ERR_KEY_SIZE), false;
                memcpy(_key, pk->key.ec.q, pk->key.ec.qlen);
                _ta.pkey.key.ec.q = _key;
            }
            else
            {
                return (_err = ERR_NO_KEY), false;
            }

            _ta.dn.data = _dn;
            _ta.dn.len = _dnLen;
            _ta.flags = BR_X509_TA_CA;
            _ok = true;
            return true;
        }

        bool TrustAnchor::currentTime(uint32_t& days, uint32_t& seconds)
        {
            if (!openknx.time.isValid()) return false;
            time_t utc = openknx.time.getUtcTime().toTime_t();
            if (utc <= 0) return false;
            days = BEARSSL_UNIX_EPOCH_DAYS + (uint32_t)(utc / 86400);
            seconds = (uint32_t)(utc % 86400);
            return true;
        }
    } // namespace Crypto
} // namespace OpenKNX

#endif // ARDUINO_ARCH_RP2040 && OPENKNX_WEBCLIENT
