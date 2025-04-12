#include "Base_Webserver.h"
#include "OpenKNX.h"

void Base_Webserver::addLink(WebserverLink l)
{
    _links.push_back(l);
}

void Base_Webserver::addLink(std::string name, std::string url)
{
    WebserverLink l = {
        .uri = url,
        .name = name
    };
    _links.push_back(l);
}

void Base_Webserver::addPage(WebserverPage p)
{
    _pages.push_back(p);
}

const char* Base_Webserver::getBaseUri()
{
    return (char*)webserver_base_uri;
}

int Base_Webserver::handleBase(WebRequest *req)
{
    if(strcmp(req->uri, WEBSERVER_BASE_URI) == 0)
    {
        std::string response = index_html;
        
        response += "<h3>Web-Services:</h3><ul>";
        for(auto &page : _pages)
        {
            if(page.isVisible == false)
                continue;
            response += "<a href=\"";
            response += webserver_base_uri;
            response += page.uri;
            response += "\">";
            response += page.name;
            response += "</a><br>";
        }
        
        if(_links.size() > 0)
        {
            response += "</ul><h3>Web-Links:</h3><ul>";
            for(auto &link : _links)
            {
                response += "<a href=\"";
                response += link.uri;
                response += "\">";
                response += link.name;
                response += "</a><br>";
            }
        }
        
        response += "</ul><h3>Verwendete Module:</h3><ul>";

        #ifdef KNX_Version
        response += "<li>KNX - ";
        response += KNX_Version;
        response += "</li>";
        #endif
        #ifdef MODULE_Common_Version
        response += "<li>Common - ";
        response += MODULE_Common_Version;
        response += "</li>";
        #endif

        for(auto &module : openknx.modules.list)
        {
            if(module == nullptr)
                continue;
            response += "<li>";
            response += module->name();
            response += " - ";
            response += module->version();
            response += "</li>";
        }
        response += "</ul></body></html>";

        req->setResponse("text/html", response.c_str());
        return 0;
    }
    else if(strcmp(req->uri, "/") == 0)
    {
        req->setStatusCode(301);
        req->addResponseHeader("Location", WEBSERVER_BASE_URI);
        req->setResponse("text/html", NULL, 0);
        return 0;
    }
    else
    {
        for(auto &file : _files)
        {
            if(strcmp(req->uri + webserver_base_uri_len, file.uri.c_str()) == 0)
            {
                if(file.isGzipped)
                {
                    req->addResponseHeader("Content-Encoding", "gzip");
                }
                req->setResponse(file.type, file.data, file.size);
                return 0;
            }
        }

        for(auto &page : _pages)
        {
            if(strncmp(req->uri + webserver_base_uri_len, page.uri, strlen(page.uri)) == 0)
            {
                return page.handler(req->uri + webserver_base_uri_len, req, page.arg);
            }
        }
    }

    return -1;
}

void Base_Webserver::addStaticFile(std::string url, const char *type, const uint8_t *data, int size, bool isGzipped)
{
    WebserverFile f = {
        .uri = std::string(url),
        .type = type,
        .data = data,
        .size = size,
        .isGzipped = isGzipped
    };
    _files.push_back(f);
}