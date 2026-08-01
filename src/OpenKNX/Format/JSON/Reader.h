#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace OpenKNX
{
    namespace Format
    {
        namespace JSON
        {

            class Reader
            {
                const char* _start; // points into caller's JSON buffer, not owned
                const char* _end;
                bool _valid;

                Reader(const char* start, const char* end, bool valid);

              public:
                Reader(const char* json);
                Reader(const char* json, size_t len);

                bool valid() const { return _valid; }

                bool isObject() const;
                bool isArray()  const;
                bool isString() const;
                bool isNumber() const;
                bool isBool()   const;
                bool isNull()   const;

                Reader get(const char* key) const;
                Reader operator[](const char* key) const { return get(key); }
                Reader get(size_t index) const;
                Reader operator[](size_t index) const { return get(index); }

                // JSON Pointer (RFC 6901): "" = root, "/config/ip", "/sensors/0/val"
                // Escaping: ~1 = '/', ~0 = '~' in key names
                Reader select(const char* path) const;

                // Compare value directly without intermediate buffer (no heap)
                bool equals(const char* s) const;
                bool equals(int32_t v) const;
                bool equals(bool v) const;

                // Primary: fixed buffer, no heap allocation
                bool        toString(char* buf, size_t bufSize) const;
                // Convenience: allocates via std::string
                std::string toString() const;

                int32_t  toInt(int32_t def = 0) const;
                uint32_t toUInt(uint32_t def = 0) const;
                float    toFloat(float def = 0.0f) const;
                bool     toBool(bool def = false) const;
            };

        } // namespace JSON
    } // namespace Format
} // namespace OpenKNX
