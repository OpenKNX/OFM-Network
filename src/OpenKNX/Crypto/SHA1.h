#pragma once

#include <cstdint>
#include <cstring>

namespace OpenKNX
{
    namespace Crypto
    {

        class SHA1
        {
          public:
            SHA1();

            void update(const uint8_t* data, size_t len);
            void update(const char* data) { update((const uint8_t*)data, strlen(data)); }

            void final(uint8_t digest[20]);

            static void compute(const uint8_t* data, size_t len, uint8_t digest[20])
            {
                SHA1 sha;
                sha.update(data, len);
                sha.final(digest);
            }

            static void compute(const char* data, uint8_t digest[20])
            {
                compute((const uint8_t*)data, strlen(data), digest);
            }

          private:
            // Opaque buffer for platform-specific context
            // RP2040: SHA1 state (5×uint32 + 64-byte buffer + 2×uint32 = ~100 bytes)
            // ESP32: mbedtls_sha1_context (~100+ bytes)
            // alignas: wird auf Structs mit uint32-Membern gecastet
            alignas(4) uint8_t _ctx[256];

            void processBlock();
        };

    } // namespace Crypto
} // namespace OpenKNX
