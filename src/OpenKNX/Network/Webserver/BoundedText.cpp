/**
 * @file        BoundedText.cpp
 * @brief       Response text in one malloc block of fixed capacity
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 */
#include "OpenKNX/Network/Webserver/BoundedText.h"
#include "OpenKNX/Helper.h" // PSRAM_MALLOC
#ifdef ARDUINO_ARCH_RP2040
    #include <Arduino.h> // rp2040.getFreeHeap()
    #include "OpenKNX/Network/Webserver/Webserver.h" // OPENKNX_WEBSERVER_MIN_HEAP
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace OpenKNX
{
    namespace Network
    {
        BoundedText::BoundedText(size_t cap)
        {
            if (cap == 0) return;
            _p = (char*)PSRAM_MALLOC(cap); // freed with free(), as WebResponse does
            if (!_p) return;
            _cap = cap;
            _p[0] = '\0';
        }

        BoundedText::BoundedText(size_t cap, size_t minCap)
        {
            if (minCap > cap) minCap = cap; // a build override below the floor still gets one try
#ifdef ARDUINO_ARCH_RP2040
            // The block lives until the last ACK: start smaller rather than take the floor the web gate keeps free
            const size_t freeHeap = rp2040.getFreeHeap();
            while (cap / 2 >= minCap && freeHeap < cap + OPENKNX_WEBSERVER_MIN_HEAP) cap /= 2;
#endif
            for (; cap >= minCap && cap > 0; cap /= 2)
            {
                _p = (char*)PSRAM_MALLOC(cap);
                if (!_p) continue;
                _cap = cap;
                _p[0] = '\0';
                return;
            }
        }

        BoundedText::~BoundedText()
        {
            free(_p);
        }

        void BoundedText::add(const char* s, size_t n)
        {
            if (!_p)
            {
                _cut = true;
                return;
            }
            const size_t r = room();
            if (n > r)
            {
                n = r;
                _cut = true;
            }
            memcpy(_p + _n, s, n);
            _n += n;
            _p[_n] = '\0';
        }

        BoundedText& BoundedText::operator+=(const char* s)
        {
            if (s) add(s, strlen(s));
            return *this;
        }

        void BoundedText::addf(const char* fmt, ...)
        {
            if (!_p)
            {
                _cut = true;
                return;
            }
            va_list ap;
            va_start(ap, fmt);
            const int w = vsnprintf(_p + _n, _cap - _n, fmt, ap);
            va_end(ap);
            if (w < 0)
            {
                _p[_n] = '\0';
                _cut = true;
                return;
            }
            if ((size_t)w > room())
            {
                _n = _cap - 1; // vsnprintf kept what fits, NUL at the end
                _cut = true;
                return;
            }
            _n += (size_t)w;
        }

        void BoundedText::rollback(size_t n)
        {
            if (!_p) return;
            if (n < _n)
            {
                _n = n;
                _p[_n] = '\0';
            }
            _cut = false;
        }

        uint8_t* BoundedText::take(int& len)
        {
            uint8_t* p = (uint8_t*)_p;
            if (p && _n + 1 < _cap)
            {
                void* q = PSRAM_REALLOC(p, _n + 1); // only the text stays allocated while it is sent
                if (q) p = (uint8_t*)q;
            }
            len = p ? (int)_n : 0;
            _p = nullptr;
            _cap = _n = 0;
            return p;
        }
    } // namespace Network
} // namespace OpenKNX
