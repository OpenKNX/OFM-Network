#include "OpenKNX/Network/Webserver/WebRequest.h"

namespace OpenKNX
{
    namespace Network
    {

        std::string WebRequest::header(const std::string& name) const
        {
            auto it = headers.find(name);
            if (it != headers.end()) return it->second;
            return "";
        }

        void WebRequest::setHeader(const std::string& name, const std::string& value)
        {
            headers[name] = value;
        }

    } // namespace Network
} // namespace OpenKNX
