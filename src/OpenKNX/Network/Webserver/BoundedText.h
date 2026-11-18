/**
 * @file        BoundedText.h
 * @brief       Response text in one malloc block of fixed capacity: never grows, never calls operator new
 * @copyright   Copyright (c) 2026, Erkan Çolak (erkan@colak.de)
 *              Licensed under GNU GPL v3.0
 *
 * A failed malloc returns NULL (RP2040, -fno-exceptions); a failed operator new aborts. failed() = answer 503.
 */
#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <string>

namespace OpenKNX
{
    namespace Network
    {
        /** @brief Appends into one block taken up front; a write that does not fit keeps what fits and sets cut(). */
        class BoundedText
        {
          public:
            /** @brief Takes cap bytes, one of them for the NUL; failed() when the heap has no such block. */
            explicit BoundedText(size_t cap);
            /** @brief Takes the largest of cap, cap/2, ... not below minCap that the heap has; for text that may end early. */
            BoundedText(size_t cap, size_t minCap);
            size_t capacity() const { return _cap; }
            ~BoundedText();
            BoundedText(const BoundedText&) = delete;
            BoundedText& operator=(const BoundedText&) = delete;

            bool failed() const { return _p == nullptr; }
            bool cut() const { return _cut; }
            size_t size() const { return _n; }
            /** @brief Bytes still free, the NUL not counted. */
            size_t room() const { return _p ? _cap - 1 - _n : 0; }
            const char* c_str() const { return _p ? _p : ""; }

            void add(const char* s, size_t n);
            BoundedText& operator+=(const char* s);
            BoundedText& operator+=(const std::string& s) { add(s.data(), s.size()); return *this; }
            BoundedText& operator+=(char c) { add(&c, 1); return *this; }
            __attribute__((format(printf, 2, 3))) void addf(const char* fmt, ...);

            /** @brief Shortens to n bytes and clears cut(): drops a part that did not fit completely. */
            void rollback(size_t n);

            /** @brief Hands the block (malloc) to the caller, e.g. WebResponse::sendOwned(); nullptr when failed. */
            uint8_t* take(int& len);

          private:
            char* _p = nullptr;
            size_t _cap = 0; // NUL included
            size_t _n = 0;
            bool _cut = false;
        };
    } // namespace Network
} // namespace OpenKNX
