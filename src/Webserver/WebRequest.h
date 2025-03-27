#pragma once

#include "Arduino.h"
#include "WebserverHeader.h"

class WebRequest
{
    private:
        const uint8_t* response;
        char* response_type = "text/html";
        int response_length = 0;
        uint16_t status_code = 200;

    public:
        std::vector<WebserverHeader> headers;
        const char *uri;
        uint8_t method;
        void setResponse(char* type, const char* text);
        void setResponse(char* type, const uint8_t *data, int length);
        void addResponseHeader(char *name, char *value);
        void setStatusCode(uint16_t code);
        uint16_t getStatusCode();

        char* getResponseBody() { return (char*)response; }
        int getResponseBodyLength() { return response_length; }
        char* getResponseType() { return response_type; }


};