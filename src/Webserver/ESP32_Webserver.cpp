#include "ESP32_Webserver.h"
#include "OpenKNX.h"


void ESP32_Webserver::begin()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&_server, &config) == ESP_OK) {
        // Registering the ws handler
        logDebug("Webserver", "Registering URI handlers");
        logIndentUp();

        #ifndef WEBUI_BASE_URI_USED
        logDebug("Webserver", "URI handler for /");
        httpd_uri_t baseUri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = [](httpd_req_t *req) { 
                ESP32_Webserver *ui = (ESP32_Webserver *)req->user_ctx;
                return ui->handleRequest(req);
            },
            .user_ctx = this
        };
        httpd_register_uri_handler(_server, &baseUri);
        #endif

        logDebug("Webserver", "URI handler for %s", WEBSERVER_BASE_URI);
        httpd_uri_t baseUri = {
            .uri = (std::string(webserver_base_uri) + "*").c_str(),
            .method = HTTP_GET,
            .handler = [](httpd_req_t *req) { 
                ESP32_Webserver *ui = (ESP32_Webserver *)req->user_ctx;
                return ui->handleRequest(req);
            },
            .user_ctx = this
        };
        httpd_register_uri_handler(_server, &baseUri);

        for(auto &handle : _handler)
        {
            logDebug("Webserver", "URI handler for %s", handle.uri);
            httpd_register_uri_handler(_server, &handle);
        }

        for(auto &page : _pages)
        {
            logDebug("Webserver", "URI page for %s at %s", page.name, page.uri);
        }
        logIndentDown();
    } else {
        logError("Webserver", "Failed to start the server");
    }
}

void ESP32_Webserver::addHandler(httpd_uri_t h)
{
    _handler.push_back(h);
}

esp_err_t ESP32_Webserver::handleRequest(httpd_req_t *req)
{
    WebRequest request;
    request.uri = req->uri;
    request.method = (uint8_t)req->method;
    
    int response = handleBase(&request);

        // httpd_resp_set_type(req, "text/html");
        // httpd_resp_set_status(req, "301 Moved Permanently");
        // httpd_resp_set_hdr(req, "Location", WEBSERVER_BASE_URI);
        // httpd_resp_send(req, NULL, 0);

    if(request.getStatusCode() == 404)
    {
        httpd_resp_send_404(req);
        return ESP_OK;
    }

    for(auto &header : request.headers)
    {
        httpd_resp_set_hdr(req, header.name, header.value);
    }

    int statusCode = request.getStatusCode();
    std::string statusMessage = std::to_string(statusCode) + " " +
                                 (httpStatusMessages.count(statusCode) ? httpStatusMessages.at(statusCode) : "Unknown Status");
    httpd_resp_set_status(req, statusMessage.c_str());
    httpd_resp_set_type(req, request.getResponseType());
    httpd_resp_send(req, request.getResponseBody(), request.getResponseBodyLength());

    free(request.getResponseBody());
    return response;
}