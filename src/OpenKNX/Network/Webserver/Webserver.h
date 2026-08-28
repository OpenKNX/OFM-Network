#pragma once

#ifdef OPENKNX_WEBSERVER

#include "OpenKNX/Format/JSON/Writer.h"
#include "OpenKNX/Network/Webserver/WebRequest.h"
#include "OpenKNX/Network/Webserver/WebResponse.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#ifndef OPENKNX_WEBSERVER_MAX_BODY
#ifdef ARDUINO_ARCH_RP2040
#define OPENKNX_WEBSERVER_MAX_BODY (4 * 1024) // 4 KB – RAM-begrenzt auf RP2040
#else
#define OPENKNX_WEBSERVER_MAX_BODY (32 * 1024) // 32 KB für ESP32
#endif
#endif

// Webserver connection slots, shared by HTTP and WebSocket in any mix. Only the sum
// is capped, so the device's other services (NTP, MQTT, OTA, KNX-IP) keep sockets.
// A visitor takes two: the always-open WebSocket plus an HTTP slot while loading.
//
// RP2040 follows the lwIP pool it actually has: arduino-pico sets
// MEMP_NUM_TCP_PCB = __LWIP_MEMMULT * 5 for the *whole* device, so without that flag
// six slots would be a promise the stack cannot keep. 3 is the value that works
// there; with -D__LWIP_MEMMULT=2 (10 PCBs) the default rises to 6, i.e. three
// visitors. MEMP_NUM_TCP_PCB itself is not visible here -- lwipopts.h is not included
// by this header -- so the flag stands in for it.
#ifndef OPENKNX_WEBSERVER_MAX_CONN
#ifdef ARDUINO_ARCH_ESP32
#define OPENKNX_WEBSERVER_MAX_CONN 7
#elif defined(__LWIP_MEMMULT) && __LWIP_MEMMULT >= 2
#define OPENKNX_WEBSERVER_MAX_CONN 6
#else
#define OPENKNX_WEBSERVER_MAX_CONN 3
#endif
#endif

// Every slot can be a WebSocket, so the queue's cursor table must cover the whole
// pool -- a client without a cursor would loop reconnecting in silence.
#define WS_MAX_CLIENTS OPENKNX_WEBSERVER_MAX_CONN

// Sendequeue: ein Ring für alle Clients (Senderichtung). Nicht zu verwechseln mit
// OPENKNX_WEBSOCKET_RX_CAP, das den Empfangs-Sammelpuffer je Verbindung begrenzt.
// Dimensionierend ist der Schwall pro Tick, nicht die Dauerlast.
#ifndef OPENKNX_WEBSOCKET_TX_BUFFER
#define OPENKNX_WEBSOCKET_TX_BUFFER 4096
#endif

#include "OpenKNX/Network/Webserver/WsTxQueue.h"

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
        // UI-Socket: der Registrant bekommt jede eingehende Nachricht und prüft den Key
        // selbst -- die Firmware hält deshalb keine Keytabelle vor.
        using WebUiHandler = std::function<void(const char* key, const char* data, size_t len)>;

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
            std::vector<int> clients; // connected fds/slot indices for this uri
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

            // WebSocket-Endpoint der Oberfläche. Klassenkonstante statt Makro, und
            // bewusst nicht "/ws": addSocket() matcht exakt und kennt kein Dedup, ein
            // OAM mit eigenem "/ws" bekäme sonst still den ersten Treffer.
            static constexpr const char* UI_WS_URI = "/openknx/ws";

            // Oberflächen-Socket: immer Broadcast, Key-basiert. Wer nur sendet,
            // registriert nichts. Der reservierte Key "connect" meldet den Handlern
            // einen neu verbundenen Client -- ein Reconnect ist davon nicht zu
            // unterscheiden, der Client resynct also vollständig.
            struct Ui
            {
                void sendMessage(const char* key, const char* data, size_t len = 0);
                void receiveMessage(WebUiHandler cb);
                // Like hasClients(UI_WS_URI) without the std::string -- asked once per
                // tick by the console and once per telegram by the group monitor.
                bool hasClients() const;
                // Largest message the queue takes in one piece, in bytes. Covers
                // "key:data" *together*, so subtract strlen(key) + 1 from your own budget
                // or a message at the limit is rejected silently.
                size_t maxMessage() const;
            };
            Ui ui;

            void addRoute(uint8_t method, const std::string& uri, WebRouteHandler handler);
            void addSocket(const std::string& uri, WebSocketHandler onMessage,
                           WebSocketConnectHandler onConnect = nullptr);
            void addMenuItem(const std::string& label, const std::string& uri, int8_t priority = 0);
            void addStylesheet(const char* uri);
            void addJavaScript(const char* uri);

            void setup();
            // Opens the port. Separate from setup() so the feature modules can register
            // their routes first -- on ESP32 httpd serves from its own task.
            void start();
            void loop();
            void loopPlatform(); // Empfang und Transport der jeweiligen Plattform
            bool isRunning();
            // Reiht ein statt sofort zu senden -- der Drain in loop() gibt die Records an
            // die Sockets. fd < 0 = Broadcast an alle Clients dieses Endpoints.
            void sendWebsocketMessage(const std::string& uri, const char* message, int fd = -1);
            // Grösste Payload pro WS-Frame
            size_t maxWebsocketPayload() const;
            bool hasClients(const std::string& uri) const; // ohne Vector-Kopie
            // Size and free space of the send queue -- one for all endpoints, hence here
            // and not on ui. Producers throttle on it.
            size_t txCapacity() const;
            size_t txFree() const;

            std::string buildHeader(const std::string& activeUri = "");
            std::string buildFooter();

            const std::vector<WebMenuItem>& menuItems() const { return _menu; }

            static WebRouteHandler Redirect(const std::string& target);
            static WebRouteHandler Static(const char* mimeType, const char* text);
            static WebRouteHandler Static(const char* mimeType, const uint8_t* data, int length);
            static WebRouteHandler Asset(const char* mimeType, const uint8_t* data, size_t length); // aus webassets.h

            void logRequest(const WebRequest& req, const WebResponse& res);
            void logWebsocket(const WebRequest& req, int statusCode = 101);

            // Access-Log: 4xx/5xx immer, alles darunter nur mit aktiviertem Toggle
            bool accessLog() const { return _accessLog; }
            void accessLog(bool enabled) { _accessLog = enabled; }

            // Platform transport glue only (Webserver_RP2040.cpp / Webserver_ESP32.cpp) --
            // no feature module calls these. const char* on purpose: the RP2040 side
            // calls notifySocketMessage() from the per-WS-frame lwIP callback with a
            // char[] already in hand, and const std::string& forced a std::string alloc
            // on every frame just to satisfy the signature.
            bool handleRequest(WebRequest& req, WebResponse& res);
            bool hasSocket(const char* uri) const;
            void notifySocketConnect(const char* uri, int fd, bool connected);
            void notifySocketMessage(const char* uri, int fd,
                                     uint8_t* data, int length, bool isBinary);
            // Nicht mehr Teil der dokumentierten API: der bool war nur für eine eigene
            // Retry-Logik da, und die übernimmt jetzt die Sendequeue. Reiht ebenfalls
            // ein -- sonst verlöre ein OAM, das es benutzt, weiterhin Frames.
            bool sendToClient(const std::string& uri, int fd, const char* data, size_t len);
            // Sendet sofort, an der Queue vorbei. Nur für den Drain -- ginge er über
            // sendToClient(), reihte er endlos wieder ein statt zu senden.
            WsSendResult wsSendNow(const char* uri, int fd, const char* data, size_t len);
            void dropClient(const char* uri, int fd);

          protected:
            std::vector<WebRoute> _routes;
            std::vector<WebSocketRoute> _sockets;
            std::vector<WebMenuItem> _menu;
            std::vector<std::string> _stylesheets;
            std::vector<std::string> _scripts;

          private:
            bool _accessLog = false;
            bool _initialized = false;
            uint32_t _startRetry = 0;    // last start attempt
            uint32_t _startDelay = 1000; // backoff, doubles per failure up to 10 s

            WsTxQueue _txQueue;
            std::vector<WebUiHandler> _uiHandlers;
            // Index statt Zeiger: _sockets kann bei einem weiteren addSocket() umziehen.
            // Erspart pro Nachricht die std::string-Konstruktion von hasClients().
            int _uiRouteIdx = -1;

            // Zuletzt gesendeter Status. Die Änderungserkennung gehört dem Sender, nicht
            // der Transportschicht -- verglichen werden die Skalare selbst, kein Payload.
            struct StatusSnapshot
            {
                bool valid = false;
                bool prog = false;
                bool configured = false;
                bool netUp = false;
                bool busmon = false;
                uint32_t ip = 0, mask = 0, gw = 0, dns = 0;
                uint16_t pa = 0;
                uint32_t memKiB = 0;
                int16_t cpuC = 0;
            } _lastStatus;
            uint32_t _lastSample = 0; // sample gate for the expensive diagnostics
            // The prog handler follows the rule every registrant must: record in the
            // callback, work in loop(). publishStatus() allocates.
            volatile int8_t _pendingProg = -1; // 0/1/2(=toggle)
            OpenKNX::Format::JSON::Writer _statusJson;
            // Literal pointer, not a content compare -- only to avoid repeating the
            // same message every tick.
            const char* _uiOversizeKey = nullptr;

            void buildOverviewPage(WebResponse& res);
            void setupUiSocket();
            void notifyUiHandlers(const char* key, const char* data, size_t len);
            void publishStatus(bool force);
            void deliverPendingConnects();
            void drainTxQueue();
            int routeIndex(const char* uri) const;
            const char* routeUri(uint8_t index) const;
#ifdef ARDUINO_ARCH_ESP32
            void setupEsp32();
#elif defined(ARDUINO_ARCH_RP2040)
            void setupRp2040();
#endif

            // Linear scan over _sockets -- a handful of registered WS routes at most,
            // cheaper and smaller than a std::map keyed by uri. Client fds live inline
            // on the matched WebSocketRoute (WebSocketRoute::clients).
            WebSocketRoute* findSocketRoute(const char* uri);
            const WebSocketRoute* findSocketRoute(const char* uri) const;

#ifdef ARDUINO_ARCH_ESP32
            void* _server = nullptr;          // httpd_handle_t — opaque, kein esp_http_server.h im Header
            mutable void* _wsLock = nullptr;  // recursive mutex guarding WebSocketRoute::clients + the WS session registry

          public:
            void* serverHandle() const { return _server; }
            void wsStateLock() const; // rekursiv, No-op bis setupEsp32() den Mutex anlegt
            void wsStateUnlock() const;
#elif defined(ARDUINO_ARCH_RP2040)
            void* _listenPcb = nullptr;
#endif
        };

    } // namespace Network
} // namespace OpenKNX

#endif // OPENKNX_WEBSERVER
