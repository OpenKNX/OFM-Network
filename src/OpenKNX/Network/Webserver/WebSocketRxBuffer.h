#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <utility>

namespace OpenKNX
{
    namespace Network
    {
        template <std::size_t MaxPayload>
        class WebSocketRxBuffer
        {
            static constexpr std::size_t headerBytes = 14;
            static constexpr std::size_t inlineBytes = (MaxPayload < 256 ? MaxPayload : 256) + headerBytes;
            std::array<uint8_t, inlineBytes> _inline;
            std::unique_ptr<uint8_t[]> _overflow;
            std::size_t _capacity = inlineBytes;
            std::size_t _size = 0;

          public:
            uint8_t* data() { return _overflow ? _overflow.get() : _inline.data(); }
            uint8_t* tail() { return data() + _size; }
            std::size_t size() const { return _size; }
            std::size_t room() const { return _capacity - _size; }
            void commit(std::size_t count) { _size += count; }

            bool growForFrame(std::size_t bytes)
            {
                if (bytes <= _capacity || bytes > MaxPayload + headerBytes) return false;
                auto larger = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[bytes]);
                if (!larger) return false;
                std::memcpy(larger.get(), data(), _size);
                _overflow = std::move(larger);
                _capacity = bytes;
                return true;
            }

            void consume(std::size_t count)
            {
                if (count == 0) return;
                _size -= count;
                if (_overflow && _size <= inlineBytes)
                {
                    std::memcpy(_inline.data(), _overflow.get() + count, _size);
                    _overflow.reset();
                    _capacity = inlineBytes;
                }
                else
                    std::memmove(data(), data() + count, _size);
            }
        };
    } // namespace Network
} // namespace OpenKNX
