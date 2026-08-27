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

            // Drive routing, shared with anything that writes files the way the web upload does:
            // "" or "/" = LittleFS, "sd/" = SD card, "efc/" = external flash. Kept here because this
            // is the one place that already knows all three stores.
            static int driveFromPath(const std::string& path, std::string& rel);
            static bool writeChunk(int drive, const char* path, uint32_t offset,
                                   const uint8_t* data, size_t len);


          private:
            void handleList(WebRequest& req, WebResponse& res);
            void handleDownload(WebRequest& req, WebResponse& res);
            void handleUpload(WebRequest& req, WebResponse& res);
            void handleDelete(WebRequest& req, WebResponse& res);
            void handleMkdir(WebRequest& req, WebResponse& res);
#ifdef OPENKNX_WEBCLIENT_TEST
            static constexpr size_t FETCH_URL_MAX = 512;                // cap on the URL taken from the body
            void handleFetch(WebRequest& req, WebResponse& res);        // arm a URL download
            void handleFetchStatus(WebRequest& req, WebResponse& res);  // poll it
#endif

            bool ensureFs();
            static std::string sanitizePath(const std::string& raw);
            static std::string urlEncode(const std::string& s);
        };

    } // namespace Network
} // namespace OpenKNX

#endif // OPENKNX_WEBFS
