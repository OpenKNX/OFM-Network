#include "Writer.h"

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace OpenKNX
{
    namespace Format
    {
        namespace JSON
        {

            void Writer::appendCommaIfNeeded()
            {
                if (_needsComma)
                    buffer() += ',';
            }

            void Writer::appendEscaped(const char* s, size_t len)
            {
                buffer() += '"';
                for (size_t i = 0; i < len; ++i)
                {
                    unsigned char c = (unsigned char)s[i];
                    switch (c)
                    {
                        case '"':  buffer() += "\\\""; break;
                        case '\\': buffer() += "\\\\"; break;
                        case '\n': buffer() += "\\n";  break;
                        case '\r': buffer() += "\\r";  break;
                        case '\t': buffer() += "\\t";  break;
                        case '\b': buffer() += "\\b";  break;
                        case '\f': buffer() += "\\f";  break;
                        default:
                            if (c < 0x20)
                            {
                                char esc[7];
                                snprintf(esc, sizeof(esc), "\\u%04X", c);
                                buffer() += esc;
                            }
                            else
                            {
                                buffer() += (char)c;
                            }
                            break;
                    }
                }
                buffer() += '"';
            }

            Writer& Writer::beginObject()
            {
                appendCommaIfNeeded();
                buffer() += '{';
                _needsComma = false;
                return *this;
            }

            Writer& Writer::endObject()
            {
                buffer() += '}';
                _needsComma = true;
                return *this;
            }

            Writer& Writer::beginArray()
            {
                appendCommaIfNeeded();
                buffer() += '[';
                _needsComma = false;
                return *this;
            }

            Writer& Writer::endArray()
            {
                buffer() += ']';
                _needsComma = true;
                return *this;
            }

            Writer& Writer::key(const char* k)
            {
                appendCommaIfNeeded();
                appendEscaped(k, strlen(k));
                buffer() += ':';
                _needsComma = false;
                return *this;
            }

            Writer& Writer::value(const char* v)
            {
                if (!v)
                    return null();
                appendCommaIfNeeded();
                appendEscaped(v, strlen(v));
                _needsComma = true;
                return *this;
            }

            Writer& Writer::value(const std::string& v)
            {
                appendCommaIfNeeded();
                appendEscaped(v.c_str(), v.size());
                _needsComma = true;
                return *this;
            }

            Writer& Writer::value(int32_t v)
            {
                appendCommaIfNeeded();
                char buf[12];
                snprintf(buf, sizeof(buf), "%" PRId32, v);
                buffer() += buf;
                _needsComma = true;
                return *this;
            }

            Writer& Writer::value(uint32_t v)
            {
                appendCommaIfNeeded();
                char buf[12];
                snprintf(buf, sizeof(buf), "%" PRIu32, v);
                buffer() += buf;
                _needsComma = true;
                return *this;
            }

            Writer& Writer::value(float v, uint8_t decimals)
            {
                appendCommaIfNeeded();
                if (std::isnan(v) || std::isinf(v))
                {
                    buffer() += "null";
                }
                else
                {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "%.*f", (int)decimals, v);
                    buffer() += buf;
                }
                _needsComma = true;
                return *this;
            }

            Writer& Writer::value(bool v)
            {
                appendCommaIfNeeded();
                buffer() += v ? "true" : "false";
                _needsComma = true;
                return *this;
            }

            Writer& Writer::null()
            {
                appendCommaIfNeeded();
                buffer() += "null";
                _needsComma = true;
                return *this;
            }

            Writer& Writer::field(const char* k, const char* v)     { return key(k).value(v); }
            Writer& Writer::field(const char* k, const std::string& v) { return key(k).value(v); }
            Writer& Writer::field(const char* k, int32_t v)         { return key(k).value(v); }
            Writer& Writer::field(const char* k, uint32_t v)        { return key(k).value(v); }
            Writer& Writer::field(const char* k, float v, uint8_t d) { return key(k).value(v, d); }
            Writer& Writer::field(const char* k, bool v)            { return key(k).value(v); }
            Writer& Writer::fieldNull(const char* k)                 { return key(k).null(); }

            Writer& Writer::operator=(const Writer& other)
            {
                if (this != &other)
                {
                    buffer() = other.buffer();
                    _needsComma = other._needsComma;
                }
                return *this;
            }

            Writer& Writer::operator=(Writer&& other) noexcept
            {
                if (this != &other)
                {
                    buffer() = std::move(other.buffer());
                    _needsComma = other._needsComma;
                }
                return *this;
            }

            const std::string& Writer::str() const  { return buffer(); }
            const char*        Writer::c_str() const { return buffer().c_str(); }
            void               Writer::reset()       { buffer().clear(); _needsComma = false; }

        } // namespace JSON
    } // namespace Format
} // namespace OpenKNX
