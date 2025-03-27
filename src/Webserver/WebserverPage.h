#pragma once

#include "WebRequest.h"
#include <string>

struct WebserverPage {
    const char *uri;
    const char *name;
    bool isVisible = true;
    std::function<int(const char *uri, WebRequest *req, void *arg)> handler;
    void *arg;
};