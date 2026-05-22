#include "Base64.h"

#ifdef ARDUINO_ARCH_RP2040

namespace OpenKNX
{
    namespace Crypto
    {

        static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

        size_t Base64::encode(const uint8_t* in, size_t inLen, char* out, size_t outSize)
        {
            size_t o = 0;
            for (size_t i = 0; i < inLen && o + 4 < outSize; i += 3)
            {
                uint32_t v = (uint32_t)in[i] << 16;
                if (i + 1 < inLen) v |= (uint32_t)in[i+1] << 8;
                if (i + 2 < inLen) v |= in[i+2];

                out[o++] = B64[(v >> 18) & 63];
                out[o++] = B64[(v >> 12) & 63];
                out[o++] = (i + 1 < inLen) ? B64[(v >> 6) & 63] : '=';
                out[o++] = (i + 2 < inLen) ? B64[v & 63] : '=';
            }
            out[o] = '\0';
            return o;
        }

    } // namespace Crypto
} // namespace OpenKNX

#endif // ARDUINO_ARCH_RP2040
