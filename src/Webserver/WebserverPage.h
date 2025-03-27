#pragma once

#include "WebRequest.h"
#include <string>

struct WebserverPage {
    std::string uri;
    std::string name;
    bool isVisible = true;
    std::function<int(const char *uri, WebRequest *req, void *arg)> handler;
    void *arg;
};