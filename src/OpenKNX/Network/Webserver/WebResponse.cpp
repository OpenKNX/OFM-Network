#include "OpenKNX/Network/Webserver/WebResponse.h"
#include <cstring>

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
            if (_bodyOwned) delete[] _body;
            _bodyOwned = true;
            if (text == nullptr)
            {
                _body = nullptr;
                _bodyLength = 0;
                return;
            }
            _bodyLength = strlen(text);
            _body = new uint8_t[_bodyLength + 1];
            memcpy(_body, text, _bodyLength + 1);
        }

        void WebResponse::send(const uint8_t* data, int length)
        {
            if (_bodyOwned) delete[] _body;
            _bodyOwned = true;
            if (data == nullptr || length == 0)
            {
                _body = nullptr;
                _bodyLength = 0;
                return;
            }
            _bodyLength = length;
            _body = new uint8_t[length];
            memcpy(_body, data, length);
        }

        void WebResponse::sendStatic(const char* text)
        {
            if (_bodyOwned) delete[] _body;
            _bodyOwned = false;
            _body = reinterpret_cast<uint8_t*>(const_cast<char*>(text));
            _bodyLength = text ? (int)strlen(text) : 0;
        }

        void WebResponse::sendStatic(const uint8_t* data, int length)
        {
            if (_bodyOwned) delete[] _body;
            _bodyOwned = false;
            _body = const_cast<uint8_t*>(data);
            _bodyLength = length;
        }

        WebResponse::~WebResponse()
        {
            if (_bodyOwned) delete[] _body;
        }

    } // namespace Network
} // namespace OpenKNX
