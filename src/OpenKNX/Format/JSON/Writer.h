#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <utility>

namespace OpenKNX
{
    namespace Format
    {
        namespace JSON
        {

            class Writer
            {
                std::string _ownedBuf;
                std::string* _buf = &_ownedBuf;
                bool _needsComma = false;

                std::string& buffer() { return *_buf; }
                const std::string& buffer() const { return *_buf; }

                void appendCommaIfNeeded();
                void appendEscaped(const char* s, size_t len);

              public:
                                Writer() = default;
                                explicit Writer(std::string& buffer) : _buf(&buffer) { buffer.clear(); }
                                Writer(const Writer& other) : _ownedBuf(other.buffer()), _buf(&_ownedBuf), _needsComma(other._needsComma) {}
                                Writer(Writer&& other) noexcept : _ownedBuf(std::move(other.buffer())), _buf(&_ownedBuf), _needsComma(other._needsComma) {}
                                Writer& operator=(const Writer& other);
                                Writer& operator=(Writer&& other) noexcept;

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
