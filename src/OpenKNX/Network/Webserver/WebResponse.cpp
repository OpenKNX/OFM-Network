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
            _contentType = mimeType;
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

    } // namespace Network
} // namespace OpenKNX
