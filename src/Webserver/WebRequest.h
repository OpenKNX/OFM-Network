#pragma once

#include "Arduino.h"
#include "WebserverHeader.h"

class WebRequest
{
    private:
        uint8_t* response = nullptr;
        const char* response_type;
        int response_length = 0;
        uint16_t status_code = 200;

    public:
        ~WebRequest();
        std::vector<WebserverHeader> headers;
        const char *uri;
        uint8_t method;
        void setResponse(const char* type, const char* text);
        void setResponse(const char* type, const uint8_t *data, int length);
        void addResponseHeader(char *name, char *value);
        void setStatusCode(uint16_t code);
        uint16_t getStatusCode();

        char* getResponseBody() { return (char*)response; }
        int getResponseBodyLength() { return response_length; }
        const char* getResponseType() { return response_type; }


};