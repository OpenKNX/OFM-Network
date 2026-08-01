#include "OpenKNX/Network/Webserver/WebRequest.h"
#include <cstdlib>

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

        std::string WebRequest::getQueryParam(const std::string& name) const
        {
            size_t qpos = uri.find('?');
            if (qpos == std::string::npos) return "";
            std::string query = uri.substr(qpos + 1);
            std::string prefix = name + "=";

            // find(prefix) alone would also match "mode=" inside "xmode=1" — the hit must
            // start the query or follow a '&'.
            size_t pos = std::string::npos;
            for (size_t search = 0; search < query.size();)
            {
                size_t hit = query.find(prefix, search);
                if (hit == std::string::npos) break;
                if (hit == 0 || query[hit - 1] == '&')
                {
                    pos = hit;
                    break;
                }
                search = hit + 1;
            }
            if (pos == std::string::npos) return "";
            pos += prefix.size();
            size_t end = query.find('&', pos);
            std::string encoded = (end == std::string::npos) ? query.substr(pos) : query.substr(pos, end - pos);

            // URL-decode
            std::string result;
            result.reserve(encoded.size());
            for (size_t i = 0; i < encoded.size(); ++i)
            {
                if (encoded[i] == '+')
                {
                    result += ' ';
                }
                else if (encoded[i] == '%' && i + 2 < encoded.size())
                {
                    char hex[3] = {encoded[i + 1], encoded[i + 2], '\0'};
                    result += (char)strtol(hex, nullptr, 16);
                    i += 2;
                }
                else
                {
                    result += encoded[i];
                }
            }
            return result;
        }

    } // namespace Network
} // namespace OpenKNX
