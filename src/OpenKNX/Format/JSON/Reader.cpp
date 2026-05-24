#include "Reader.h"

#include <cstdlib>
#include <cstring>

namespace OpenKNX
{
    namespace Format
    {
        namespace JSON
        {

            // ----------------------------------------------------------------
            // Static parser helpers — file scope, not exposed in header
            // ----------------------------------------------------------------

            static const char* skipWhitespace(const char* p, const char* end)
            {
                while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
                    ++p;
                return p;
            }

            // p must point to the opening '"'. Returns pointer one past the closing '"'.
            // Invariant: always advances past the closing '"' when the string is well-formed.
            static const char* skipString(const char* p, const char* end)
            {
                ++p; // skip opening "
                while (p < end && *p != '"')
                {
                    if (*p == '\\')
                        ++p; // skip escape char; next iteration skips the escaped char
                    if (p < end)
                        ++p;
                }
                if (p < end)
                    ++p; // skip closing "
                return p;
            }

            // p must point to '-' or a digit.
            static const char* skipNumber(const char* p, const char* end)
            {
                if (p < end && *p == '-')
                    ++p;
                while (p < end && *p >= '0' && *p <= '9')
                    ++p;
                if (p < end && *p == '.')
                {
                    ++p;
                    while (p < end && *p >= '0' && *p <= '9')
                        ++p;
                }
                if (p < end && (*p == 'e' || *p == 'E'))
                {
                    ++p;
                    if (p < end && (*p == '+' || *p == '-'))
                        ++p;
                    while (p < end && *p >= '0' && *p <= '9')
                        ++p;
                }
                return p;
            }

            // Skip any JSON value. Uses a depth counter for objects/arrays (not recursive)
            // so nesting depth does not consume stack. Strings are always skipped atomically
            // first so brackets inside string literals are never counted as nesting.
            static const char* skipValue(const char* p, const char* end)
            {
                p = skipWhitespace(p, end);
                if (p >= end)
                    return p;

                if (*p == '"')
                    return skipString(p, end);
                if (*p == 't' && p + 4 <= end)
                    return p + 4; // true
                if (*p == 'f' && p + 5 <= end)
                    return p + 5; // false
                if (*p == 'n' && p + 4 <= end)
                    return p + 4; // null
                if (*p != '{' && *p != '[')
                    return skipNumber(p, end);

                int depth = 0;
                while (p < end)
                {
                    if (*p == '"')
                    {
                        p = skipString(p, end);
                        continue;
                    }
                    if (*p == '{' || *p == '[')
                    {
                        ++depth;
                        ++p;
                        continue;
                    }
                    if (*p == '}' || *p == ']')
                    {
                        --depth;
                        ++p;
                        if (depth == 0)
                            return p;
                        continue;
                    }
                    ++p;
                }
                return p;
            }

            // Compare the raw content of a JSON string (between the quotes, without the
            // quotes) against a plain C string, applying escape decoding.
            // 'content' points to the first byte after opening '"'; 'contentEnd' points
            // to the closing '"' (exclusive).
            // \uXXXX: the 'u' char is compared literally (ASCII keys only in OpenKNX).
            static bool jsonStringEquals(const char* p, const char* pEnd, const char* target)
            {
                while (p < pEnd && *target)
                {
                    if (*p == '\\' && p + 1 < pEnd)
                    {
                        ++p;
                        char c;
                        switch (*p)
                        {
                            case '"':  c = '"';  break;
                            case '\\': c = '\\'; break;
                            case '/':  c = '/';  break;
                            case 'n':  c = '\n'; break;
                            case 'r':  c = '\r'; break;
                            case 't':  c = '\t'; break;
                            case 'b':  c = '\b'; break;
                            case 'f':  c = '\f'; break;
                            default:   c = *p;   break;
                        }
                        if (c != *target)
                            return false;
                        ++p;
                        ++target;
                    }
                    else
                    {
                        if (*p != *target)
                            return false;
                        ++p;
                        ++target;
                    }
                }
                return (p == pEnd && *target == '\0');
            }

            // Scan object body (p just after '{') for 'key'.
            // Returns pointer to the start of the value, or nullptr if not found.
            static const char* findKey(const char* p, const char* end, const char* key)
            {
                while (p < end)
                {
                    p = skipWhitespace(p, end);
                    if (p >= end || *p == '}')
                        return nullptr;
                    if (*p != '"')
                        return nullptr; // malformed JSON

                    const char* keyContent = p + 1;
                    const char* afterKey   = skipString(p, end);
                    // afterKey - 1 points to the closing '"' of the key string
                    bool match = jsonStringEquals(keyContent, afterKey - 1, key);

                    p = skipWhitespace(afterKey, end);
                    if (p >= end || *p != ':')
                        return nullptr;
                    ++p; // skip ':'
                    p = skipWhitespace(p, end);

                    if (match)
                        return p;

                    p = skipValue(p, end);
                    p = skipWhitespace(p, end);
                    if (p < end && *p == ',')
                        ++p;
                }
                return nullptr;
            }

            // Scan array body (p just after '[') for element at 'idx'.
            // Returns pointer to the element, or nullptr if out of range.
            static const char* findIndex(const char* p, const char* end, size_t idx)
            {
                size_t current = 0;
                while (p < end)
                {
                    p = skipWhitespace(p, end);
                    if (p >= end || *p == ']')
                        return nullptr;

                    if (current == idx)
                        return p;

                    p = skipValue(p, end);
                    ++current;
                    p = skipWhitespace(p, end);
                    if (p < end && *p == ',')
                        ++p;
                }
                return nullptr;
            }

            // ----------------------------------------------------------------
            // Reader implementation
            // ----------------------------------------------------------------

            Reader::Reader(const char* start, const char* end, bool valid)
                : _start(start), _end(end), _valid(valid)
            {
            }

            Reader::Reader(const char* json)
                : _start(nullptr), _end(nullptr), _valid(false)
            {
                if (!json)
                    return;
                _end   = json + strlen(json);
                _start = skipWhitespace(json, _end);
                _valid = (_start < _end);
            }

            Reader::Reader(const char* json, size_t len)
                : _start(nullptr), _end(nullptr), _valid(false)
            {
                if (!json)
                    return;
                _end   = json + len;
                _start = skipWhitespace(json, _end);
                _valid = (_start < _end);
            }

            bool Reader::isObject() const { return _valid && _start && _start < _end && *_start == '{'; }
            bool Reader::isArray()  const { return _valid && _start && _start < _end && *_start == '['; }
            bool Reader::isString() const { return _valid && _start && _start < _end && *_start == '"'; }
            bool Reader::isNumber() const
            {
                return _valid && _start && _start < _end &&
                       (*_start == '-' || (*_start >= '0' && *_start <= '9'));
            }
            bool Reader::isBool()   const { return _valid && _start && _start < _end && (*_start == 't' || *_start == 'f'); }
            bool Reader::isNull()   const { return _valid && _start && _start < _end && *_start == 'n'; }

            Reader Reader::get(const char* key) const
            {
                if (!_valid || !key || !isObject())
                    return Reader(nullptr, nullptr, false);
                const char* val = findKey(_start + 1, _end, key);
                if (!val)
                    return Reader(nullptr, nullptr, false);
                return Reader(val, _end, true);
            }

            Reader Reader::get(size_t index) const
            {
                if (!_valid || !isArray())
                    return Reader(nullptr, nullptr, false);
                const char* elem = findIndex(_start + 1, _end, index);
                if (!elem)
                    return Reader(nullptr, nullptr, false);
                return Reader(elem, _end, true);
            }

            bool Reader::toString(char* buf, size_t bufSize) const
            {
                if (!buf || bufSize == 0)
                    return false;
                buf[0] = '\0';
                if (!_valid || !_start || _start >= _end)
                    return false;

                if (!isString())
                {
                    // For non-strings: copy raw value bytes
                    const char* valueEnd = skipValue(_start, _end);
                    size_t len = (size_t)(valueEnd - _start);
                    if (len >= bufSize)
                    {
                        memcpy(buf, _start, bufSize - 1);
                        buf[bufSize - 1] = '\0';
                        return false;
                    }
                    memcpy(buf, _start, len);
                    buf[len] = '\0';
                    return true;
                }

                // String: unescape into buf
                const char* p    = _start + 1; // past opening "
                size_t       out = 0;
                while (p < _end && *p != '"')
                {
                    if (out + 1 >= bufSize)
                    {
                        buf[out] = '\0';
                        return false;
                    }
                    if (*p == '\\' && p + 1 < _end)
                    {
                        ++p;
                        switch (*p)
                        {
                            case '"':  buf[out++] = '"';  break;
                            case '\\': buf[out++] = '\\'; break;
                            case '/':  buf[out++] = '/';  break;
                            case 'n':  buf[out++] = '\n'; break;
                            case 'r':  buf[out++] = '\r'; break;
                            case 't':  buf[out++] = '\t'; break;
                            case 'b':  buf[out++] = '\b'; break;
                            case 'f':  buf[out++] = '\f'; break;
                            default:   buf[out++] = *p;   break;
                        }
                        ++p;
                    }
                    else
                    {
                        buf[out++] = *p++;
                    }
                }
                buf[out] = '\0';
                return true;
            }

            std::string Reader::toString() const
            {
                if (!_valid || !_start || _start >= _end)
                    return std::string();

                if (!isString())
                {
                    const char* valueEnd = skipValue(_start, _end);
                    return std::string(_start, valueEnd);
                }

                std::string result;
                const char* p = _start + 1; // past opening "
                while (p < _end && *p != '"')
                {
                    if (*p == '\\' && p + 1 < _end)
                    {
                        ++p;
                        switch (*p)
                        {
                            case '"':  result += '"';  break;
                            case '\\': result += '\\'; break;
                            case '/':  result += '/';  break;
                            case 'n':  result += '\n'; break;
                            case 'r':  result += '\r'; break;
                            case 't':  result += '\t'; break;
                            case 'b':  result += '\b'; break;
                            case 'f':  result += '\f'; break;
                            default:   result += *p;   break;
                        }
                        ++p;
                    }
                    else
                    {
                        result += *p++;
                    }
                }
                return result;
            }

            int32_t Reader::toInt(int32_t def) const
            {
                if (!_valid || !_start || _start >= _end)
                    return def;
                if (isBool())
                    return toBool() ? 1 : 0;
                if (isNull())
                    return def;
                if (!isNumber())
                    return def;
                char* endp = nullptr;
                long val   = strtol(_start, &endp, 10);
                return (endp && endp > _start) ? (int32_t)val : def;
            }

            uint32_t Reader::toUInt(uint32_t def) const
            {
                if (!_valid || !_start || _start >= _end)
                    return def;
                if (isBool())
                    return toBool() ? 1u : 0u;
                if (isNull())
                    return def;
                if (!isNumber())
                    return def;
                char* endp       = nullptr;
                unsigned long val = strtoul(_start, &endp, 10);
                return (endp && endp > _start) ? (uint32_t)val : def;
            }

            float Reader::toFloat(float def) const
            {
                if (!_valid || !_start || _start >= _end)
                    return def;
                if (!isNumber())
                    return def;
                char* endp = nullptr;
                float val  = strtof(_start, &endp);
                return (endp && endp > _start) ? val : def;
            }

            bool Reader::toBool(bool def) const
            {
                if (!_valid || !_start || _start >= _end)
                    return def;
                if (*_start == 't')
                    return true;
                if (*_start == 'f' || *_start == 'n')
                    return false;
                if (isNumber())
                    return toInt(0) != 0;
                return def;
            }

            Reader Reader::select(const char* path) const
            {
                if (!_valid)
                    return Reader(nullptr, nullptr, false);
                if (!path || *path == '\0')
                    return *this; // empty string = root document per RFC 6901

                Reader current = *this;
                const char* p  = path;

                if (*p == '/')
                    ++p; // skip leading slash

                while (*p && current.valid())
                {
                    // extract segment until next '/', applying RFC 6901 unescaping
                    // ~1 → '/'  |  ~0 → '~'  (order matters: ~1 before ~0)
                    char seg[64];
                    size_t i = 0;
                    while (*p && *p != '/')
                    {
                        if (*p == '~' && *(p + 1) == '1')
                        {
                            if (i < sizeof(seg) - 1) seg[i++] = '/';
                            p += 2;
                        }
                        else if (*p == '~' && *(p + 1) == '0')
                        {
                            if (i < sizeof(seg) - 1) seg[i++] = '~';
                            p += 2;
                        }
                        else
                        {
                            if (i < sizeof(seg) - 1) seg[i++] = *p;
                            ++p;
                        }
                    }
                    seg[i] = '\0';

                    if (*p == '/')
                        ++p; // skip separator

                    if (i == 0)
                        break; // empty segment

                    // numeric segment → array index
                    bool isNum = (i > 0);
                    for (size_t j = 0; j < i; ++j)
                        if (seg[j] < '0' || seg[j] > '9') { isNum = false; break; }

                    if (isNum)
                        current = current.get((size_t)strtoul(seg, nullptr, 10));
                    else
                        current = current.get(seg);
                }
                return current;
            }

            bool Reader::equals(const char* s) const
            {
                if (!_valid || !_start || _start >= _end || !s)
                    return false;

                if (isString())
                {
                    const char* p = _start + 1; // past opening "
                    while (p < _end && *p != '"' && *s)
                    {
                        char c;
                        if (*p == '\\' && p + 1 < _end)
                        {
                            ++p;
                            switch (*p)
                            {
                                case '"':  c = '"';  break;
                                case '\\': c = '\\'; break;
                                case '/':  c = '/';  break;
                                case 'n':  c = '\n'; break;
                                case 'r':  c = '\r'; break;
                                case 't':  c = '\t'; break;
                                case 'b':  c = '\b'; break;
                                case 'f':  c = '\f'; break;
                                default:   c = *p;   break;
                            }
                            ++p;
                        }
                        else
                        {
                            c = *p++;
                        }
                        if (c != *s++)
                            return false;
                    }
                    return (p < _end && *p == '"' && *s == '\0');
                }

                // numbers, bools, null: compare raw JSON bytes against s
                // for numbers: try numeric parse first so equals("1") works on int 1
                if (isNumber())
                {
                    char* endp = nullptr;
                    long sv    = strtol(s, &endp, 10);
                    if (endp && endp > s && *endp == '\0')
                        return toInt() == (int32_t)sv;
                }

                const char* p = _start;
                while (p < _end && *s)
                {
                    if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
                        break;
                    if (*p != *s)
                        return false;
                    ++p;
                    ++s;
                }
                return (*s == '\0');
            }

            bool Reader::equals(int32_t v) const
            {
                return isNumber() && toInt() == v;
            }

            bool Reader::equals(bool v) const
            {
                return isBool() && toBool() == v;
            }

        } // namespace JSON
    } // namespace Format
} // namespace OpenKNX
