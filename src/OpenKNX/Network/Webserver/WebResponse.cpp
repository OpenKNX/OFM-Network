#include "OpenKNX/Network/Webserver/WebResponse.h"
#include "OpenKNX/Helper.h" // PSRAM_MALLOC
#include <cstring>
#include <cstdlib>
#include <algorithm>

namespace OpenKNX
{
    namespace Network
    {

        namespace
        {
            struct HtmlChunk
            {
                HtmlChunk* next;
                size_t used;
                uint8_t data[512];
            };

            struct HtmlBody
            {
                HtmlChunk* first;
                HtmlChunk* last;
                HtmlChunk* reading;
                size_t offset;
            };

            void freeHtml(void* context)
            {
                auto* body = static_cast<HtmlBody*>(context);
                while (body->first)
                {
                    auto* next = body->first->next;
                    free(body->first);
                    body->first = next;
                }
                free(body);
            }

            size_t readHtml(void* context, uint8_t* buffer, size_t length)
            {
                auto* body = static_cast<HtmlBody*>(context);
                size_t copied = 0;
                while (body->reading && copied < length)
                {
                    auto* chunk = body->reading;
                    const size_t count = std::min(length - copied, chunk->used - body->offset);
                    memcpy(buffer + copied, chunk->data + body->offset, count);
                    copied += count;
                    body->offset += count;
                    if (body->offset == chunk->used)
                    {
                        body->reading = chunk->next;
                        body->first = chunk->next;
                        if (body->last == chunk) body->last = nullptr;
                        free(chunk);
                        body->offset = 0;
                    }
                }
                return copied;
            }
        }

        void WebResponse::clearBody()
        {
            if (_bodyOwned) free(_body);
            _body = nullptr;
            _bodyLength = 0;
            cleanupStream();
            _segCount = 0;
        }

        void WebResponse::allocationFailed()
        {
            clearBody();
            _statusCode = 503;
            setContentType("text/plain");
            _allocationFailed = true; // after setContentType(), which it locks
            _useLayout = false;
            _layoutHeader = std::string();
            _layoutFooter = std::string();
            _bodyOwned = false;
            static const char message[] = "Temporarily unavailable: insufficient memory";
            _body = reinterpret_cast<uint8_t*>(const_cast<char*>(message));
            _bodyLength = sizeof(message) - 1;
        }

        void WebResponse::beginHtml()
        {
            clearBody();
            _allocationFailed = false;
            auto* body = static_cast<HtmlBody*>(PSRAM_MALLOC(sizeof(HtmlBody)));
            if (!body)
            {
                allocationFailed();
                return;
            }
            *body = {};
            sendStream(0, readHtml, freeHtml, body);
        }

        bool WebResponse::appendHtml(const char* text, size_t length)
        {
            if (_allocationFailed || !_streaming || _streamReadFn != readHtml) return false;
            auto* body = static_cast<HtmlBody*>(_streamCtx);
            while (length)
            {
                if (!body->last || body->last->used == sizeof(HtmlChunk::data))
                {
                    auto* chunk = static_cast<HtmlChunk*>(PSRAM_MALLOC(sizeof(HtmlChunk)));
                    if (!chunk)
                    {
                        allocationFailed();
                        return false;
                    }
                    chunk->next = nullptr;
                    chunk->used = 0;
                    if (body->last) body->last->next = chunk;
                    else body->first = body->reading = chunk;
                    body->last = chunk;
                }
                const size_t count = std::min(length, sizeof(HtmlChunk::data) - body->last->used);
                memcpy(body->last->data + body->last->used, text, count);
                body->last->used += count;
                _streamTotal += count;
                text += count;
                length -= count;
            }
            return true;
        }

        HtmlWriter& HtmlWriter::operator+=(const char* text)
        {
            if (text) _response.appendHtml(text, strlen(text));
            return *this;
        }

        bool WebResponse::isHtmlStream() const
        {
            return _streaming && _streamReadFn == readHtml;
        }

        WebResponse::WebResponse()
        {
            _headers.push_back({"Cache-Control", "no-store"});
        }

        void WebResponse::setStatus(uint16_t code)
        {
            if (!_allocationFailed) _statusCode = code;
        }

        void WebResponse::setContentType(const char* mimeType)
        {
            if (!mimeType || _allocationFailed) return;
            strncpy(_contentType, mimeType, sizeof(_contentType) - 1);
            _contentType[sizeof(_contentType) - 1] = '\0';
            if (strncmp(_contentType, "text/", 5) == 0 && strstr(_contentType, "charset") == nullptr)
            {
                const char* suffix = "; charset=utf-8";
                if (strlen(_contentType) + strlen(suffix) < sizeof(_contentType))
                    strcat(_contentType, suffix);
            }
        }

        void WebResponse::setHeader(const char* name, const char* value)
        {
            for (auto& h : _headers)
            {
                if (h.first == name)
                {
                    h.second = value;
                    return;
                }
            }
            _headers.push_back({name, value});
        }

        void WebResponse::send(const char* text)
        {
            clearBody();
            _bodyOwned = true;
            if (text == nullptr)
            {
                _body = nullptr;
                _bodyLength = 0;
                return;
            }
            _bodyLength = strlen(text);
            _body = (uint8_t*)PSRAM_MALLOC(_bodyLength + 1);
            if (_body == nullptr)
            {
                allocationFailed();
                return;
            }
            memcpy(_body, text, _bodyLength + 1);
        }

        void WebResponse::send(const uint8_t* data, int length)
        {
            clearBody();
            _bodyOwned = true;
            if (data == nullptr || length == 0)
            {
                _body = nullptr;
                _bodyLength = 0;
                return;
            }
            _bodyLength = length;
            _body = (uint8_t*)PSRAM_MALLOC(length);
            if (_body == nullptr)
            {
                allocationFailed();
                return;
            }
            memcpy(_body, data, length);
        }

        void WebResponse::sendStatic(const char* text)
        {
            clearBody();
            _bodyOwned = false;
            _body = reinterpret_cast<uint8_t*>(const_cast<char*>(text));
            _bodyLength = text ? (int)strlen(text) : 0;
        }

        void WebResponse::sendStatic(const uint8_t* data, int length)
        {
            clearBody();
            _bodyOwned = false;
            _body = const_cast<uint8_t*>(data);
            _bodyLength = length;
        }

        void WebResponse::sendAsset(const char* mimeType, const uint8_t* data, size_t length)
        {
            setContentType(mimeType);
            setHeader("Cache-Control", "public, max-age=86400");
            setHeader("Content-Encoding", "gzip");
            sendStatic(data, (int)length);
        }

        void WebResponse::sendRedirect(const std::string& target)
        {
            setStatus(301);
            setHeader("Location", target.c_str());
            send("");
        }

        WebResponse::~WebResponse()
        {
            clearBody();
        }

        void WebResponse::sendStream(size_t totalLength,
                                     WebStreamReadFn readFn, WebStreamCleanupFn cleanupFn, void* ctx)
        {
            clearBody();
            _body = nullptr;
            _bodyLength = 0;
            _bodyOwned = false;
            _streaming = true;
            _streamTotal = totalLength;
            _streamReadFn = readFn;
            _streamCleanupFn = cleanupFn;
            _streamCtx = ctx;
            _streamHeaderSent = _streamFooterSent = 0;
            _streamBodyDone = false;
        }

        size_t WebResponse::readStreamChunk(uint8_t* buf, size_t maxLen)
        {
            if (!_streaming || !buf || !maxLen) return 0;
            if (_streamHeaderSent < _layoutHeader.size())
            {
                const size_t count = std::min(maxLen, _layoutHeader.size() - _streamHeaderSent);
                memcpy(buf, _layoutHeader.data() + _streamHeaderSent, count);
                _streamHeaderSent += count;
                return count;
            }
            if (!_streamBodyDone)
            {
                const size_t count = _streamReadFn && _streamCtx ? _streamReadFn(_streamCtx, buf, maxLen) : 0;
                if (count) return count;
                _streamBodyDone = true;
            }
            const size_t count = std::min(maxLen, _layoutFooter.size() - _streamFooterSent);
            memcpy(buf, _layoutFooter.data() + _streamFooterSent, count);
            _streamFooterSent += count;
            return count;
        }

        WebStreamCleanupFn WebResponse::releaseStream(void*& ctx)
        {
            auto cleanup = _streamCleanupFn;
            ctx = _streamCtx;
            _streaming = false;
            _streamTotal = 0;
            _streamCtx = nullptr;
            _streamReadFn = nullptr;
            _streamCleanupFn = nullptr;
            _streamHeaderSent = _streamFooterSent = 0;
            _streamBodyDone = false;
            return cleanup;
        }

        void WebResponse::cleanupStream()
        {
            void* ctx = nullptr;
            auto cleanup = releaseStream(ctx);
            if (cleanup && ctx) cleanup(ctx);
        }

        // ── Segmente ─────────────────────────────────────────────────────────

        void WebResponse::setLayoutChrome(std::string header, std::string footer)
        {
            _layoutHeader = std::move(header);
            _layoutFooter = std::move(footer);
        }

        void WebResponse::finalizeSegments()
        {
            _segCount = 0;
            auto add = [this](const void* data, int len) {
                if (!data || len <= 0 || _segCount >= MAX_SEGMENTS) return;
                _segments[_segCount++] = {(const uint8_t*)data, len};
            };

            // Ab hier darf der Body nicht mehr geändert werden — die Segmente zeigen
            // direkt hinein, ein weiteres send() würde sie ins Leere laufen lassen.
            add(_layoutHeader.data(), (int)_layoutHeader.length());
            add(_body, _bodyLength);
            add(_layoutFooter.data(), (int)_layoutFooter.length());
        }

        int WebResponse::totalLength() const
        {
            int total = 0;
            for (int i = 0; i < _segCount; i++)
                total += _segments[i].len;
            return total;
        }

        void WebResponse::reset()
        {
            clearBody();
            _body = nullptr;
            _bodyLength = 0;
            _bodyOwned = true;

            _statusCode = 200;
            strncpy(_contentType, "text/html; charset=utf-8", sizeof(_contentType) - 1);
            _contentType[sizeof(_contentType) - 1] = '\0';
            _useLayout = false;
            _allocationFailed = false;
            _activeMenuUri.clear();

            _headers.clear();
            _headers.push_back({"Cache-Control", "no-store"});

            _streaming = false;
            _streamTotal = 0;
            _streamReadFn = nullptr;
            _streamCleanupFn = nullptr;
            _streamCtx = nullptr;

            // Zuweisung statt clear(): gibt den Heap-Puffer wirklich frei
            _streamHeaderSent = _streamFooterSent = 0;
            _streamBodyDone = false;
            _layoutHeader = std::string();
            _layoutFooter = std::string();
            _segCount = 0;
        }

    } // namespace Network
} // namespace OpenKNX
