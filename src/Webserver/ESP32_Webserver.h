#pragma once

#include "Base_Webserver.h"
#include <esp_http_server.h>

static const std::unordered_map<int, std::string> httpStatusMessages = {
    {200, "OK"},
    {301, "Moved Permanently"},
    {302, "Found"},
    {400, "Bad Request"},
    {401, "Unauthorized"},
    {403, "Forbidden"},
    // {404, "Not Found"}, we already easy send this one
    {500, "Internal Server Error"},
    {502, "Bad Gateway"},
    {503, "Service Unavailable"}
};

class ESP32_Webserver : public Base_Webserver
{
    public:
        void begin();
        void addHandler(httpd_uri_t h);

        esp_err_t handleRequest(httpd_req_t *req);
        httpd_handle_t getServerHandle() { return _server; }

    private:
        std::vector<httpd_uri_t> _handler;
        httpd_handle_t _server;
};