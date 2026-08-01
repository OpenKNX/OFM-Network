#include "SHA1.h"

#ifdef ARDUINO_ARCH_ESP32

#include <mbedtls/sha1.h>

namespace OpenKNX
{
    namespace Crypto
    {

        SHA1::SHA1()
        {
            mbedtls_sha1_context* ctx = (mbedtls_sha1_context*)_ctx;
            mbedtls_sha1_init(ctx);
            mbedtls_sha1_starts(ctx);
        }

        void SHA1::processBlock()
        {
            // Not used on ESP32 - mbedTLS handles internally
        }

        void SHA1::update(const uint8_t* data, size_t len)
        {
            mbedtls_sha1_context* ctx = (mbedtls_sha1_context*)_ctx;
            mbedtls_sha1_update(ctx, data, len);
        }

        void SHA1::final(uint8_t digest[20])
        {
            mbedtls_sha1_context* ctx = (mbedtls_sha1_context*)_ctx;
            mbedtls_sha1_finish(ctx, digest);
            mbedtls_sha1_free(ctx);
        }

    } // namespace Crypto
} // namespace OpenKNX

#endif // ARDUINO_ARCH_ESP32
