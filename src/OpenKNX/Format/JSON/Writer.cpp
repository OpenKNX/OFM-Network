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
                    _buf += ',';
            }

            void Writer::appendEscaped(const char* s, size_t len)
            {
                _buf += '"';
                for (size_t i = 0; i < len; ++i)
                {
                    unsigned char c = (unsigned char)s[i];
                    switch (c)
                    {
                        case '"':  _buf += "\\\""; break;
                        case '\\': _buf += "\\\\"; break;
                        case '\n': _buf += "\\n";  break;
                        case '\r': _buf += "\\r";  break;
                        case '\t': _buf += "\\t";  break;
                        case '\b': _buf += "\\b";  break;
                        case '\f': _buf += "\\f";  break;
                        default:
                            if (c < 0x20)
                            {
                                char esc[7];
                                snprintf(esc, sizeof(esc), "\\u%04X", c);
                                _buf += esc;
                            }
                            else
                            {
                                _buf += (char)c;
                            }
                            break;
                    }
                }
                _buf += '"';
            }

            Writer& Writer::beginObject()
            {
                appendCommaIfNeeded();
                _buf += '{';
                _needsComma = false;
                return *this;
            }

            Writer& Writer::endObject()
            {
                _buf += '}';
                _needsComma = true;
                return *this;
            }

            Writer& Writer::beginArray()
            {
                appendCommaIfNeeded();
                _buf += '[';
                _needsComma = false;
                return *this;
            }

            Writer& Writer::endArray()
            {
                _buf += ']';
                _needsComma = true;
                return *this;
            }

            Writer& Writer::key(const char* k)
            {
                appendCommaIfNeeded();
                appendEscaped(k, strlen(k));
                _buf += ':';
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
                _buf += buf;
                _needsComma = true;
                return *this;
            }

            Writer& Writer::value(uint32_t v)
            {
                appendCommaIfNeeded();
                char buf[12];
                snprintf(buf, sizeof(buf), "%" PRIu32, v);
                _buf += buf;
                _needsComma = true;
                return *this;
            }

            Writer& Writer::value(float v, uint8_t decimals)
            {
                appendCommaIfNeeded();
                if (std::isnan(v) || std::isinf(v))
                {
                    _buf += "null";
                }
                else
                {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "%.*f", (int)decimals, v);
                    _buf += buf;
                }
                _needsComma = true;
                return *this;
            }

            Writer& Writer::value(bool v)
            {
                appendCommaIfNeeded();
                _buf += v ? "true" : "false";
                _needsComma = true;
                return *this;
            }

            Writer& Writer::null()
            {
                appendCommaIfNeeded();
                _buf += "null";
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

            const std::string& Writer::str() const  { return _buf; }
            const char*        Writer::c_str() const { return _buf.c_str(); }
            void               Writer::reset()       { _buf.clear(); _needsComma = false; }

        } // namespace JSON
    } // namespace Format
} // namespace OpenKNX
