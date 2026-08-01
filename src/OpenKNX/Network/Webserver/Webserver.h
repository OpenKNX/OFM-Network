#pragma once

#ifdef OPENKNX_WEBSERVER

#include "OpenKNX/Network/Webserver/WebRequest.h"
#include "OpenKNX/Network/Webserver/WebResponse.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#ifndef OPENKNX_WEBSERVER_MAX_BODY
#ifdef ARDUINO_ARCH_RP2040
#define OPENKNX_WEBSERVER_MAX_BODY (4 * 1024) // 4 KB – RAM-begrenzt auf RP2040
#else
#define OPENKNX_WEBSERVER_MAX_BODY (32 * 1024) // 32 KB für ESP32
#endif
#endif

namespace OpenKNX
{
    namespace Network
    {

        // Ergebnis eines WS-Sendeversuchs. WouldBlock = Sendepuffer voll (langsamer
        // Client), das Frame ist unversehrt nicht rausgegangen und kann wiederholt
        // werden. Failed = Stream kaputt oder Verbindung weg → Client abräumen.
        enum class WsSendResult : uint8_t
        {
            Ok,
            WouldBlock,
            Failed
        };

        // WEB_* Prefix vermeidet Kollision mit ESP-IDF's http_method Enum (HTTP_GET etc.)
        constexpr uint8_t WEB_GET = 0;
        constexpr uint8_t WEB_POST = 1;
        constexpr uint8_t WEB_PUT = 2;
        constexpr uint8_t WEB_DELETE = 3;

        struct WebSocketFrame
        {
            uint8_t* data;
            int length;
            bool isBinary;
        };

        using WebRouteHandler = std::function<void(WebRequest&, WebResponse&)>;
        using WebSocketHandler = std::function<void(int clientId, WebSocketFrame*)>;
        using WebSocketConnectHandler = std::function<void(int clientId, bool connected)>;

        struct WebRoute
        {
            uint8_t method;
            std::string uri;
            WebRouteHandler handler;
        };

        struct WebSocketRoute
        {
            std::string uri;
            WebSocketHandler onMessage;
            WebSocketConnectHandler onConnect;
        };

        struct WebMenuItem
        {
            std::string label;
            std::string uri;
            int8_t priority = 0;
        };

        class Webserver
        {
          public:
            std::string logPrefix() { return "Webserver"; }

            void addRoute(uint8_t method, const std::string& uri, WebRouteHandler handler);
            void addSocket(const std::string& uri, WebSocketHandler onMessage,
                           WebSocketConnectHandler onConnect = nullptr);
            void addMenuItem(const std::string& label, const std::string& uri, int8_t priority = 0);
            void addStylesheet(const char* uri);
            void addJavaScript(const char* uri);

            void setup();
            void loop();
            bool isRunning();
            void sendWebsocketMessage(const std::string& uri, const char* message, int fd = -1);
            bool sendToClient(const std::string& uri, int fd, const char* data, size_t len);
            // Grösste Payload pro WS-Frame; Retries müssen sich daran halten
            size_t maxWebsocketPayload() const;
            std::vector<int> connectedClientFds(const std::string& uri) const;
            bool hasClients(const std::string& uri) const; // ohne Vector-Kopie

            std::string buildHeader(const std::string& activeUri = "");
            std::string buildFooter();

            const std::vector<WebMenuItem>& menuItems() const { return _menu; }

            static WebRouteHandler Redirect(const std::string& target);
            static WebRouteHandler Static(const char* mimeType, const char* text);
            static WebRouteHandler Static(const char* mimeType, const uint8_t* data, int length);
            static WebRouteHandler Asset(const char* mimeType, const uint8_t* data, size_t length); // aus webassets.h

            void logRequest(const WebRequest& req, const WebResponse& res);
            void logWebsocket(const WebRequest& req, int statusCode = 101);

            bool handleRequest(WebRequest& req, WebResponse& res);
            bool hasSocket(const std::string& uri) const;
            void notifySocketConnect(const std::string& uri, int fd, bool connected);
            void notifySocketMessage(const std::string& uri, int fd,
                                     uint8_t* data, int length, bool isBinary);

          protected:
            std::vector<WebRoute> _routes;
            std::vector<WebSocketRoute> _sockets;
            std::vector<WebMenuItem> _menu;
            std::vector<std::string> _stylesheets;
            std::vector<std::string> _scripts;

          private:
            void buildOverviewPage(WebResponse& res);
#ifdef ARDUINO_ARCH_ESP32
            void setupEsp32();
#elif defined(ARDUINO_ARCH_RP2040)
            void setupRp2040();
#endif

#ifdef ARDUINO_ARCH_ESP32
            void* _server = nullptr;          // httpd_handle_t — opaque, kein esp_http_server.h im Header
            mutable void* _wsLock = nullptr;  // recursive mutex guarding _socketClients + the WS session registry
            std::map<std::string, std::vector<int>> _socketClients;

          public:
            void* serverHandle() const { return _server; }
            void wsStateLock() const; // rekursiv, No-op bis setupEsp32() den Mutex anlegt
            void wsStateUnlock() const;
#elif defined(ARDUINO_ARCH_RP2040)
            void* _listenPcb = nullptr;
            std::map<std::string, std::vector<int>> _socketClients;
#endif
        };

    } // namespace Network
} // namespace OpenKNX

#endif // OPENKNX_WEBSERVER
