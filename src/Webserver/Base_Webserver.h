#pragma once

#include "WebserverPage.h"
#include "WebserverLink.h"
#include "WebRequest.h"
#include "WebserverFile.h"

#ifndef WEBSERVER_BASE_URI
#define WEBSERVER_BASE_URI "/openknx"
#endif

static const char index_html[] = "<html><head><title>OpenKNX Webserver</title><link rel='icon' href='data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAADIAAAAyAQMAAAAk8RryAAAABlBMVEUAAAD///+l2Z/dAAAARUlEQVQY02P4DwYHGJDofwyMmPQPhnrs9B8SaRzmINmH7p7/f/DQ6OD/eag+XDSa+Q8Y5OF0Azt2uv84aTQuc9Dtw+IeAJiM6TuHdsbFAAAAAElFTkSuQmCC'/></head><body style='font-family:Arial, sans-serif;margin:20px;background-color:#f4f4f4;'><header style='position:relative;padding:10px 0;'><img src='https://raw.githubusercontent.com/OpenKNX/.github/main/profile/images/weiss.svg' alt='Logo' style='width:150px;height:auto;top:20px;right:20px;position:absolute;'><h1>Startseite</h1></header>";
static const char webserver_base_uri[] = WEBSERVER_BASE_URI;
static const uint8_t webserver_base_uri_len = strlen(webserver_base_uri);

class Base_Webserver
{
    protected:
        std::vector<WebserverPage> _pages;
        std::vector<WebserverLink> _links;
        std::vector<WebserverFile> _files;
        int handleBase(WebRequest *request);

    public:
        void addPage(WebserverPage p);
        void addLink(WebserverLink l);
        void addLink(std::string name, std::string url);
        void addStaticFile(std::string url, const char *type, const uint8_t *data, int size, bool isGzipped = true);
        const char* getBaseUri();
};