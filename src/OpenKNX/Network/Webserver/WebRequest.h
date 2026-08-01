#pragma once

#include <IPAddress.h>
#include <cstdint>
#include <map>
#include <string>

namespace OpenKNX
{
    namespace Network
    {

        class WebRequest
        {
          public:
            std::string uri;
            uint8_t method = 0;

            std::string header(const std::string& name) const;
            void setHeader(const std::string& name, const std::string& value);

            IPAddress getRemoteAddr() const { return IPAddress(_remoteIpAddr); }
            void setRemoteAddr(uint32_t addr) { _remoteIpAddr = addr; }

            uint16_t getRemotePort() const { return _remotePort; }
            void setRemotePort(uint16_t port) { _remotePort = port; }

            std::string getUri() const
            {
                size_t qpos = uri.find('?');
                return (qpos != std::string::npos) ? uri.substr(0, qpos) : uri;
            }

            std::map<std::string, std::string> headers;

            // Non-owning view auf den POST-Body (Platform setzt den Pointer vor handleRequest)
            void setBody(const uint8_t* data, size_t length)
            {
                _body = data;
                _bodyLength = length;
            }
            const uint8_t* body() const { return _body; }
            size_t bodyLength() const { return _bodyLength; }

            // Query-Parameter aus URI auslesen (URL-dekodiert)
            std::string getQueryParam(const std::string& name) const;

          private:
            uint32_t _remoteIpAddr = 0;
            uint16_t _remotePort = 0;
            const uint8_t* _body = nullptr;
            size_t _bodyLength = 0;
        };

    } // namespace Network
} // namespace OpenKNX
