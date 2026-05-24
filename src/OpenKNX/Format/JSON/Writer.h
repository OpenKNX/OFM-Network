#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace OpenKNX
{
    namespace Format
    {
        namespace JSON
        {

            class Writer
            {
                std::string _buf;
                bool _needsComma = false;

                void appendCommaIfNeeded();
                void appendEscaped(const char* s, size_t len);

              public:
                Writer& beginObject();
                Writer& endObject();
                Writer& beginArray();
                Writer& endArray();

                Writer& key(const char* k);

                Writer& value(const char* v);              // nullptr → JSON null
                Writer& value(const std::string& v);
                Writer& value(int32_t v);
                Writer& value(uint32_t v);
                Writer& value(float v, uint8_t decimals = 4);
                Writer& value(bool v);
                Writer& null();

                // field() = key() + value() in one call
                Writer& field(const char* k, const char* v);
                Writer& field(const char* k, const std::string& v);
                Writer& field(const char* k, int32_t v);
                Writer& field(const char* k, uint32_t v);
                Writer& field(const char* k, float v, uint8_t decimals = 4);
                Writer& field(const char* k, bool v);
                Writer& fieldNull(const char* k);

                const std::string& str() const;
                const char* c_str() const;
                void reset();
            };

        } // namespace JSON
    } // namespace Format
} // namespace OpenKNX
