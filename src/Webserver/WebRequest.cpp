#include "WebRequest.h"

void WebRequest::setResponse(const char* type, const char* text)
{
    response_type = type;
    response_length = strlen(text);

    response = (uint8_t*)malloc(response_length + 1);
    if (response == NULL) {
        // Handle memory allocation failure
        printf("Memory allocation failed\n");
        return;
    }
    memcpy(response, text, response_length);
    response[response_length] = '\0'; // Null-terminate the string
}

void WebRequest::setResponse(const char* type, const uint8_t *data, int length)
{
    response_type = type;
    response_length = length;

    response = (uint8_t*)malloc(response_length);
    if (response == nullptr) {
        // Handle memory allocation failure
        printf("Memory allocation failed\n");
        return;
    }
    memcpy(response, data, response_length);
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

WebRequest::~WebRequest()
{
    if (response != nullptr) {
        free(response);
    }
}