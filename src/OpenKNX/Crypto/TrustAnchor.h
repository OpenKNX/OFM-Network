#pragma once

// Only the RP2040 HTTPS client decodes a trust anchor; without the web client nothing here has a
// caller, and the unit is 1254 B of flash on every RP2040 build. Same gate as its only user
// (Webclient/Handler_RP2040.cpp) so the two cannot drift apart.
#if defined(ARDUINO_ARCH_RP2040) && defined(OPENKNX_WEBCLIENT)

#include <bearssl/bearssl.h>
#include <cstddef>
#include <cstdint>

// Buffer sizes are overridable so a flash/RAM-tight target can shrink them.
#ifndef OPENKNX_TRUSTANCHOR_DN_SIZE
    #define OPENKNX_TRUSTANCHOR_DN_SIZE 256
#endif
#ifndef OPENKNX_TRUSTANCHOR_KEY_SIZE
    #define OPENKNX_TRUSTANCHOR_KEY_SIZE 528
#endif

namespace OpenKNX
{
    namespace Crypto
    {
        /**
         * @brief One PEM root certificate decoded into a BearSSL trust anchor.
         *
         * The PEM is base64-decoded in 48-character steps straight into the X.509
         * decoder, so no DER copy is held; only the subject DN and the public key
         * are retained, because BearSSL keeps pointing at them during the handshake.
         */
        class TrustAnchor
        {
          public:
            /** @brief Decode the first CERTIFICATE block of @p pem. Returns false on a parse error or a non-CA certificate. */
            bool fromPem(const char* pem);

            /** @brief The decoded anchor, or nullptr when fromPem() did not succeed. */
            const br_x509_trust_anchor* anchor() const { return _ok ? &_ta : nullptr; }

            /** @brief Reason of the last fromPem() failure (BearSSL BR_ERR_X509_* or one of the codes below). */
            int lastError() const { return _err; }

            static const int ERR_NO_PEM = -1;    // no BEGIN/END CERTIFICATE block
            static const int ERR_BASE64 = -2;    // illegal base64 character
            static const int ERR_DN_SIZE = -3;   // subject DN longer than OPENKNX_TRUSTANCHOR_DN_SIZE
            static const int ERR_KEY_SIZE = -4;  // public key longer than OPENKNX_TRUSTANCHOR_KEY_SIZE
            static const int ERR_NOT_CA = -5;    // certificate carries no CA flag
            static const int ERR_NO_KEY = -6;    // decoder returned no public key
            static const int ERR_NO_MEMORY = -7; // decoder context could not be allocated

            /**
             * @brief Current UTC as BearSSL expects it (days since 0 AD, seconds since midnight).
             * @return false while the device has no valid time — the caller must then not validate.
             */
            static bool currentTime(uint32_t& days, uint32_t& seconds);

          private:
            static void appendDn(void* ctx, const void* buf, size_t len);
            bool decode(br_x509_decoder_context& dec, const char* pem);

            br_x509_trust_anchor _ta = {};
            uint8_t _dn[OPENKNX_TRUSTANCHOR_DN_SIZE] = {};
            uint8_t _key[OPENKNX_TRUSTANCHOR_KEY_SIZE] = {};
            size_t _dnLen = 0;
            bool _dnOverflow = false;
            bool _ok = false;
            int _err = 0;
        };
    } // namespace Crypto
} // namespace OpenKNX

#endif // ARDUINO_ARCH_RP2040 && OPENKNX_WEBCLIENT
