#include "WebRequest.h"

void WebRequest::setResponse(const char* type, const char* text)
{
    response_type = type;
    response = (uint8_t*)text;
    response_length = strlen(text);
}

void WebRequest::setResponse(const char* type, const uint8_t *data, int length)
{
    response_type = type;
    response = data;
    response_length = length;
}

void WebRequest::addResponseHeader(char *name, char *value)
{
    WebserverHeader h;
    h.name = name;
    h.value = value;
    headers.push_back(h);
}

void WebRequest::setStatusCode(uint16_t code)
{
    status_code = code;
}

uint16_t WebRequest::getStatusCode()
{
    return status_code;
}