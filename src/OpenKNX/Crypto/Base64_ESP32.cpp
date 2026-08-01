#include "Base64.h"

#ifdef ARDUINO_ARCH_ESP32

#include <mbedtls/base64.h>

namespace OpenKNX
{
    namespace Crypto
    {

        size_t Base64::encode(const uint8_t* in, size_t inLen, char* out, size_t outSize)
        {
            size_t olen = 0;
            if (mbedtls_base64_encode((uint8_t*)out, outSize, &olen, in, inLen) != 0)
                return 0;
            return olen;
        }

    } // namespace Crypto
} // namespace OpenKNX

#endif // ARDUINO_ARCH_ESP32
