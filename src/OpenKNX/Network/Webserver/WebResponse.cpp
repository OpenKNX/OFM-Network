#include "OpenKNX/Network/Webserver/WebResponse.h"
#include "OpenKNX/Helper.h" // PSRAM_MALLOC
#include <cstring>
#include <cstdlib>

namespace OpenKNX
{
    namespace Network
    {

        WebResponse::WebResponse()
        {
            _headers.push_back({"Cache-Control", "no-store"});
        }

        void WebResponse::setStatus(uint16_t code)
        {
            _statusCode = code;
        }

        void WebResponse::setContentType(const char* mimeType)
        {
            if (!mimeType) return;
            strncpy(_contentType, mimeType, sizeof(_contentType) - 1);
            _contentType[sizeof(_contentType) - 1] = '\0';
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
            if (_bodyOwned) free(_body); // PSRAM_MALLOC
            _bodyOwned = true;
            if (text == nullptr)
            {
                _body = nullptr;
                _bodyLength = 0;
                return;
            }
            _bodyLength = strlen(text);
            _body = (uint8_t*)PSRAM_MALLOC(_bodyLength + 1);
            if (_body == nullptr) { _bodyLength = 0; return; }
            memcpy(_body, text, _bodyLength + 1);
        }

        void WebResponse::send(const uint8_t* data, int length)
        {
            if (_bodyOwned) free(_body); // PSRAM_MALLOC
            _bodyOwned = true;
            if (data == nullptr || length == 0)
            {
                _body = nullptr;
                _bodyLength = 0;
                return;
            }
            _bodyLength = length;
            _body = (uint8_t*)PSRAM_MALLOC(length);
            if (_body == nullptr) { _bodyLength = 0; return; }
            memcpy(_body, data, length);
        }

        void WebResponse::sendStatic(const char* text)
        {
            if (_bodyOwned) free(_body); // PSRAM_MALLOC
            _bodyOwned = false;
            _body = reinterpret_cast<uint8_t*>(const_cast<char*>(text));
            _bodyLength = text ? (int)strlen(text) : 0;
        }

        void WebResponse::sendStatic(const uint8_t* data, int length)
        {
            if (_bodyOwned) free(_body); // PSRAM_MALLOC
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
            if (_bodyOwned) free(_body); // PSRAM_MALLOC
            // _streamCtx wird NICHT hier freigegeben – Ownership liegt bei der Plattform
        }

        void WebResponse::sendStream(size_t totalLength,
                                     WebStreamReadFn readFn, WebStreamCleanupFn cleanupFn, void* ctx)
        {
            if (_bodyOwned) free(_body); // PSRAM_MALLOC
            _body = nullptr;
            _bodyLength = 0;
            _bodyOwned = false;
            _streaming = true;
            _streamTotal = totalLength;
            _streamReadFn = readFn;
            _streamCleanupFn = cleanupFn;
            _streamCtx = ctx;
        }

        size_t WebResponse::readStreamChunk(uint8_t* buf, size_t maxLen)
        {
            if (!_streamReadFn || !_streamCtx) return 0;
            return _streamReadFn(_streamCtx, buf, maxLen);
        }

        void WebResponse::cleanupStream()
        {
            if (_streamCleanupFn && _streamCtx)
                _streamCleanupFn(_streamCtx);
            _streaming = false;
            _streamCtx = nullptr;
            _streamReadFn = nullptr;
            _streamCleanupFn = nullptr;
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
            if (_bodyOwned) free(_body); // PSRAM_MALLOC
            _body = nullptr;
            _bodyLength = 0;
            _bodyOwned = true;

            _statusCode = 200;
            strncpy(_contentType, "text/html", sizeof(_contentType) - 1);
            _contentType[sizeof(_contentType) - 1] = '\0';
            _useLayout = false;
            _activeMenuUri.clear();

            _headers.clear();
            _headers.push_back({"Cache-Control", "no-store"});

            _streaming = false;
            _streamTotal = 0;
            _streamReadFn = nullptr;
            _streamCleanupFn = nullptr;
            _streamCtx = nullptr;

            // Zuweisung statt clear(): gibt den Heap-Puffer wirklich frei
            _layoutHeader = std::string();
            _layoutFooter = std::string();
            _segCount = 0;
        }

    } // namespace Network
} // namespace OpenKNX
