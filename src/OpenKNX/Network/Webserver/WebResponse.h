#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace OpenKNX
{
    namespace Network
    {

        class WebResponse
        {
          public:
            void setStatus(uint16_t code);
            void setContentType(const char* mimeType);
            void addHeader(const char* name, const char* value);
            void setLayout(bool useLayout) { _useLayout = useLayout; }
            void setActiveMenu(const std::string& uri) { _activeMenuUri = uri; }

            void send(const char* text);
            void send(const uint8_t* data, int length);
            void sendStatic(const char* text);
            void sendStatic(const uint8_t* data, int length);

            uint16_t statusCode() const { return _statusCode; }
            const char* contentType() const { return _contentType; }
            const char* body() const { return (const char*)_body; }
            int bodyLength() const { return _bodyLength; }
            bool isStatic() const { return !_bodyOwned; }
            bool useLayout() const { return _useLayout; }
            const std::string& activeMenuUri() const { return _activeMenuUri; }

            const std::vector<std::pair<std::string, std::string>>& responseHeaders() const { return _headers; }

            ~WebResponse();

          private:
            uint16_t _statusCode = 200;
            const char* _contentType = "text/html";
            uint8_t* _body = nullptr;
            int _bodyLength = 0;
            bool _bodyOwned = true;
            bool _useLayout = false;
            std::string _activeMenuUri;
            std::vector<std::pair<std::string, std::string>> _headers;
        };

    } // namespace Network
} // namespace OpenKNX
