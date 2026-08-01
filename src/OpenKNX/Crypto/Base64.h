#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace OpenKNX
{
    namespace Crypto
    {

        class Base64
        {
          public:
            static size_t encode(const uint8_t* in, size_t inLen, char* out, size_t outSize);
            static size_t encode(const char* in, char* out, size_t outSize)
            {
                return encode((const uint8_t*)in, in ? strlen(in) : 0, out, outSize);
            }
        };

    } // namespace Crypto
} // namespace OpenKNX
