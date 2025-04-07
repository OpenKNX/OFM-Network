#pragma once

#include <string>

struct WebserverFile {
    std::string uri;
    const char *type;
    const uint8_t *data;
    int size;
};