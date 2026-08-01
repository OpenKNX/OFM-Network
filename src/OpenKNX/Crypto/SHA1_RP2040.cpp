#include "SHA1.h"

#ifdef ARDUINO_ARCH_RP2040

namespace OpenKNX
{
    namespace Crypto
    {

        static inline uint32_t rol32(uint32_t v, int n)
        {
            return (v << n) | (v >> (32 - n));
        }

        SHA1::SHA1()
        {
            struct Context { uint32_t h[5]; uint8_t buf[64]; uint32_t lo, hi; };
            Context* ctx = (Context*)_ctx;
            ctx->h[0] = 0x67452301;
            ctx->h[1] = 0xEFCDAB89;
            ctx->h[2] = 0x98BADCFE;
            ctx->h[3] = 0x10325476;
            ctx->h[4] = 0xC3D2E1F0;
            ctx->lo = ctx->hi = 0;
        }

        void SHA1::processBlock()
        {
            struct Context { uint32_t h[5]; uint8_t buf[64]; uint32_t lo, hi; };
            Context* ctx = (Context*)_ctx;

            uint32_t w[80];
            for (int i = 0; i < 16; ++i)
                w[i] = ((uint32_t)ctx->buf[i*4] << 24) | ((uint32_t)ctx->buf[i*4+1] << 16) |
                       ((uint32_t)ctx->buf[i*4+2] << 8) | (uint32_t)ctx->buf[i*4+3];

            for (int i = 16; i < 80; ++i)
                w[i] = rol32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

            uint32_t a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3], e = ctx->h[4], f, k, t;
            for (int i = 0; i < 80; ++i)
            {
                if      (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
                else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
                else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
                else             { f = b ^ c ^ d; k = 0xCA62C1D6; }

                t = rol32(a, 5) + f + e + k + w[i];
                e = d;
                d = c;
                c = rol32(b, 30);
                b = a;
                a = t;
            }

            ctx->h[0] += a;
            ctx->h[1] += b;
            ctx->h[2] += c;
            ctx->h[3] += d;
            ctx->h[4] += e;
        }

        void SHA1::update(const uint8_t* data, size_t len)
        {
            struct Context { uint32_t h[5]; uint8_t buf[64]; uint32_t lo, hi; };
            Context* ctx = (Context*)_ctx;

            uint32_t pos = ctx->lo & 63;
            ctx->lo += (uint32_t)len;
            if (ctx->lo < (uint32_t)len) ctx->hi++;
            ctx->hi += (uint32_t)(len >> 29);

            for (size_t i = 0; i < len; ++i)
            {
                ctx->buf[pos++] = data[i];
                if (pos == 64)
                {
                    processBlock();
                    pos = 0;
                }
            }
        }

        void SHA1::final(uint8_t digest[20])
        {
            struct Context { uint32_t h[5]; uint8_t buf[64]; uint32_t lo, hi; };
            Context* ctx = (Context*)_ctx;

            uint32_t pos = ctx->lo & 63;
            ctx->buf[pos++] = 0x80;

            if (pos > 56)
            {
                while (pos < 64) ctx->buf[pos++] = 0;
                processBlock();
                pos = 0;
            }

            while (pos < 56) ctx->buf[pos++] = 0;

            uint64_t bits = ((uint64_t)ctx->hi << 32 | ctx->lo) << 3;
            for (int i = 7; i >= 0; --i)
                ctx->buf[56 + 7 - i] = (uint8_t)(bits >> (i * 8));

            processBlock();

            for (int i = 0; i < 5; ++i)
            {
                digest[i*4]   = (uint8_t)(ctx->h[i] >> 24);
                digest[i*4+1] = (uint8_t)(ctx->h[i] >> 16);
                digest[i*4+2] = (uint8_t)(ctx->h[i] >> 8);
                digest[i*4+3] = (uint8_t)ctx->h[i];
            }
        }

    } // namespace Crypto
} // namespace OpenKNX

#endif // ARDUINO_ARCH_RP2040
