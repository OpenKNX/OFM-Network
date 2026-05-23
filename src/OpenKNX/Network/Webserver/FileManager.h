#pragma once

#ifdef OPENKNX_WEBFS

#include "OpenKNX/Network/Webserver/WebRequest.h"
#include "OpenKNX/Network/Webserver/WebResponse.h"
#include <string>

namespace OpenKNX
{
    namespace Network
    {

        class FileManager
        {
          public:
            void setup();
            void loop() {}

          private:
            void handleList(WebRequest& req, WebResponse& res);
            void handleDownload(WebRequest& req, WebResponse& res);
            void handleUpload(WebRequest& req, WebResponse& res);
            void handleDelete(WebRequest& req, WebResponse& res);
            void handleMkdir(WebRequest& req, WebResponse& res);

            bool ensureFs();
            static std::string sanitizePath(const std::string& raw);
            static std::string urlEncode(const std::string& s);
        };

    } // namespace Network
} // namespace OpenKNX

#endif // OPENKNX_WEBFS
