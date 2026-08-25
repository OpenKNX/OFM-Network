#include "OpenKNX/Network/Webserver/Webserver.h"

#if defined(OPENKNX_WEBSERVER) && (defined(KNX_IP_WIFI) || defined(KNX_IP_LAN))

#include "OpenKNX.h"
#include "OpenKNX/Network/Module.h"
#include "OpenKNX/Format/JSON/Writer.h"
#include "OpenKNX/Helper.h"
#ifdef ARDUINO_ARCH_RP2040
#include <lwip_wrap.h> // LWIPMutex: makes the ethernet IRQ defer
#endif
#include "buildtime.h"
#include "webassets.h" // generiert von OGM-Common/scripts/pio/prepare.py aus web/assets/
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

namespace OpenKNX
{
    namespace Network
    {

        // Guards ring pointers, cursor table and WebSocketRoute::clients together.
        // RP2040 is not plain single-thread: with PIN_ETH_INT the lwIP callbacks run in
        // the W5500 interrupt and preempt loop(); LWIPMutex makes the IRQ defer instead.
        // Rationale in AGENTS.md, section "WS Send Queue".
        namespace
        {
            struct WsQueueGuard
            {
#ifdef ARDUINO_ARCH_ESP32
                const Webserver* ws;
                explicit WsQueueGuard(const Webserver* w) : ws(w) { ws->wsStateLock(); }
                ~WsQueueGuard() { ws->wsStateUnlock(); }
#else
                LWIPMutex _lock;
                explicit WsQueueGuard(const Webserver*) {}
#endif
            };
        } // namespace

        // ── Layout ─────────────────────────────────────────────────────────
        // base.css/base.js/favicon.svg/logo.svg liegen in web/assets/ und werden
        // dort minifiziert + gzip-komprimiert in webassets.h eingebettet.

        std::string Webserver::buildHeader(const std::string& activeUri)
        {
            std::string nav = "<div class='menu-container'>";
            for (auto& item : _menu)
            {
                bool active = (item.uri == activeUri);
                nav += "<a class='menu";
                if (active) nav += " active";
                nav += "' href='";
                nav += item.uri;
                nav += "'>";
                nav += item.label;
                nav += "</a>";
            }
            nav += "</div>";

            std::string html =
                "<!DOCTYPE html><html><head>"
                "<meta charset='utf-8'>"
                "<title>OpenKNX</title>";

            std::string buster = "?v=" + std::to_string(BUILD_TIMESTAMP);
            html += "<link rel='icon' type='image/svg+xml' href='/assets/favicon.svg" + buster + "'>";

            // Add registered stylesheets
            for (const auto& stylesheet : _stylesheets)
            {
                html += "<link rel='stylesheet' href='" + stylesheet + buster + "'>";
            }

            html += "</head><body>"
                    "<nav>"
                    "<div class='logo'>"
                    "<a href='https://www.openknx.de' target='_blank' rel='noopener noreferrer'>"
                    "<img src='/assets/logo.svg" + buster + "' alt='OpenKNX'>"
                    "</a>"
                    "</div>";

            html += nav;
            html += "<div class='nav-wiki'>"
                    "<a href='https://wiki.openknx.de' target='_blank' rel='noopener noreferrer'>"
                    "wiki</a> | "
                    "<a href='https://forum.openknx.de' target='_blank' rel='noopener noreferrer'>"
                    "forum</a>"
                    "</div>"
                    "<div class='nav-footer'>"
                    "<div class='nav-firm'>";
            html += openknx.info.firmwareName();
            html += "</div>"
                    "<div class='nav-info'>"
                    "<span id='knx-dot' class='prog-dot ";
            html += knx.configured() ? "dot-green" : "dot-off";
            html += "'></span>Adresse: <span id='knx-pa'>";
            html += openknx.info.humanIndividualAddress();
            // Formular statt Link — /prog ist bewusst POST-only. base.js fängt den
            // Submit ab und schickt stattdessen "prog:t"; ohne JS greift der POST.
            // Socket state, always "connecting" server-side: the page arrives over HTTP
            // before the socket exists. Without JavaScript it stays that way, correctly.
            html += "</span></div>"
                    "<div class='nav-info'>"
                    "<span id='ws-dot' class='prog-dot dot-off'></span>"
                    "<span id='ws-text'>Verbinde&hellip;</span>"
                    "</div>"
                    "<form id='prog-form' class='prog-status' method='post' action='/prog?mode=";
            html += knx.progMode() ? "0" : "1";
            html += "'>"
                    "<button type='submit' class='prog-status-btn'>"
                    "<span id='prog-dot' class='prog-dot ";
            html += knx.progMode() ? "dot-red" : "dot-off";
            html += "'></span><span id='prog-text'>";
            html += knx.progMode() ? "Prog-Modus aktiv" : "Prog-Modus inaktiv";
            html += "</span></button>"
                    "</form>"
                    "</div>";
            html += "</nav><main>";
            return html;
        }

        std::string Webserver::buildFooter()
        {
            std::string html = "</main>";

            // Add registered scripts before closing body
            std::string buster = "?v=" + std::to_string(BUILD_TIMESTAMP);
            for (const auto& script : _scripts)
            {
                html += "<script src='" + script + buster + "' defer></script>";
            }

            html += "</body></html>";
            return html;
        }

        void Webserver::addStylesheet(const char* uri)
        {
            _stylesheets.push_back(uri);
        }

        void Webserver::addJavaScript(const char* uri)
        {
            _scripts.push_back(uri);
        }

#ifdef OPENKNX_WEBSERVER
        void Webserver::buildOverviewPage(WebResponse& res)
        {
            char buf[64];
            std::string page = "<div class='container'><h1>Übersicht</h1>";

            // Appends straight into page instead of building+copying a separate string per row.
            // id nur für die Felder, die der Status live nachzieht.
            auto row = [&page](const char* label, const std::string& val, const char* id = nullptr) {
                page += "<tr><td>";
                page += label;
                page += "</td><td";
                if (id)
                {
                    page += " id='";
                    page += id;
                    page += "'";
                }
                page += ">";
                page += val;
                page += "</td></tr>";
            };

            // ── Gerät ──────────────────────────────────────────────────────────
            page += "<h2>Gerät</h2><table class='attribute-table'><tbody>";
#ifdef DEVICE_ID
            row("ID", DEVICE_ID);
#endif
#ifdef DEVICE_NAME
            row("Name", DEVICE_NAME);
#elif defined(HARDWARE_NAME)
            row("Name", HARDWARE_NAME);
#endif
            row("Seriennummer", openknx.info.humanSerialNumber());
            page += "</tbody></table>";

            // ── Firmware ───────────────────────────────────────────────────────
            page += "<h2>Firmware</h2><table class='attribute-table'><tbody>";
            row("Name", openknx.info.firmwareName());
            row("Version", openknx.info.humanFirmwareVersion(true));
            row("Nummer", openknx.info.humanFirmwareNumber());
#if MASK_VERSION == 0x07B0
            row("KNX-Typ", "TP (07B0)");
#elif MASK_VERSION == 0x57B0
            row("KNX-Typ", "IP (57B0)");
#elif MASK_VERSION == 0x091A
            row("KNX-Typ", "Router (091A)");
#else
            snprintf(buf, sizeof(buf), "%04X", MASK_VERSION);
            row("KNX-Typ", buf);
#endif
            {
#ifdef OPENKNX_DUALCORE
                const char* cpuMode = openknx.usesDualCore() ? "Dual-Core" : "Single-Core";
#else
                const char* cpuMode = "Single-Core";
#endif
                float temp = openknx.hardware.cpuTemperature();
                if (temp > 0.0f)
                {
                    // Nur die Zahl bekommt eine id -- der Kernmodus davor ändert sich nie.
                    // Whole degrees like the status JSON, else the first update jumps.
                    snprintf(buf, sizeof(buf), "%s (<span id='ov-cpu'>%d</span> °C)", cpuMode, (int)temp);
                    row("CPU-Modus", buf);
                }
                else
                    row("CPU-Modus", cpuMode);
            }
            page += "</tbody></table>";

            // ── Applikation ────────────────────────────────────────────────────
            page += "<h2>Applikation</h2><table class='attribute-table'><tbody>";
            {
                std::string addr = "<span id='ov-pa'>";
                addr += openknx.info.humanIndividualAddress();
                addr += knx.configured()
                            ? "</span> <span id='ov-cfg' class='green'>(Konfiguriert)</span>"
                            : "</span> <span id='ov-cfg' class='gray'>(Nicht konfiguriert)</span>";
                row("KNX-Adresse", addr);
            }
            if (openknx.info.applicationNumber() > 0)
            {
                row("Version", openknx.info.humanApplicationVersion());
                row("Nummer", openknx.info.humanApplicationNumber());
            }
            page += "</tbody></table>";

            // ── Laufzeit ───────────────────────────────────────────────────────
            page += "<h2>Laufzeit</h2><table class='attribute-table'><tbody>";
            {
                uint32_t s = uptime();
                uint32_t d = s / 86400;
                s %= 86400;
                uint32_t h = s / 3600;
                s %= 3600;
                uint32_t m = s / 60;
                s %= 60;
                snprintf(buf, sizeof(buf), "%ud %02u:%02u:%02u", d, h, m, s);
                // Danach zählt der Browser selbst weiter -- dafür muss nichts gesendet werden.
                row("Uptime", buf, "ov-up");
            }
            {
                uint32_t fm = (uint32_t)(freeMemory() / 1024);
                float fmMin = openknx.common.freeMemoryMin() / 1024.0f;
                snprintf(buf, sizeof(buf), "<span id='ov-mem'>%u</span> KiB (min. %.3f KiB)", fm, fmMin);
                row("Freier Speicher", buf);
            }
#ifdef OPENKNX_WATCHDOG
            if (openknx.watchdog.active())
            {
                snprintf(buf, sizeof(buf), "Running (%is)", (int)openknx.watchdog.maxPeriod());
                row("Watchdog", buf);
            }
            else
                row("Watchdog", "Disabled");
#else
            row("Watchdog", "Unsupported");
#endif
            page += "</tbody></table>";

            // ── Netzwerk ───────────────────────────────────────────────────────
            {
                uint8_t mac[6] = {};
                openknxNetwork.macAddress(mac);
                char macStr[18];
                snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
                bool net = openknxNetwork.established();

                page += "<h2>Netzwerk</h2><table class='attribute-table'><tbody>";
                row("Hostname", openknxNetwork.hostName());
                row("MAC", macStr);
                row("IP-Adresse", net ? openknxNetwork.localIP().toString().c_str()
                                              : "–", "ov-ip");
                row("Subnetz", net ? openknxNetwork.subnetMask().toString().c_str()
                                           : "–", "ov-mask");
                row("Gateway", net ? openknxNetwork.gatewayIP().toString().c_str()
                                           : "–", "ov-gw");
                row("DNS", net ? openknxNetwork.nameServerIP().toString().c_str()
                                       : "–", "ov-dns");
                page += "</tbody></table>";
            }

            // ── Versionen ──────────────────────────────────────────────────────
            page += "<h2>Versionen</h2><table class='attribute-table'><tbody>";
            row("This Firmware", openknx.info.humanFirmwareVersion(true));
#ifdef KNX_Version
            row("KNX", KNX_Version);
#endif
#ifdef MODULE_Common_Version
            row(openknx.common.logPrefix().c_str(), MODULE_Common_Version);
#endif
            for (uint8_t i = 0; i < openknx.modules.count; i++)
            {
                if (openknx.modules.list[i]->version().empty()) continue;
                row(openknx.modules.list[i]->name().c_str(),
                            openknx.modules.list[i]->version());
            }
            page += "<tr><td colspan='2' style='padding-top:1em'></td></tr>";
            row("Buildtime", BUILD_DATETIME);
            page += "</tbody></table>";

            page += "</div>";
            res.setContentType("text/html");
            res.setLayout(true);
            res.send(page.c_str());
        }
#endif // OPENKNX_WEBSERVER

        // ── Routing ────────────────────────────────────────────────────────

        WebSocketRoute* Webserver::findSocketRoute(const char* uri)
        {
            for (auto& s : _sockets)
                if (s.uri == uri) return &s;
            return nullptr;
        }

        const WebSocketRoute* Webserver::findSocketRoute(const char* uri) const
        {
            return const_cast<Webserver*>(this)->findSocketRoute(uri);
        }

        bool Webserver::hasSocket(const char* uri) const
        {
            return findSocketRoute(uri) != nullptr;
        }

        // Records tragen den Endpoint als Index statt als String. Der Index ist stabil,
        // weil addSocket() nur anhängt und es kein removeSocket() gibt.
        int Webserver::routeIndex(const char* uri) const
        {
            for (size_t i = 0; i < _sockets.size(); i++)
                if (_sockets[i].uri == uri) return (int)i;
            return -1;
        }

        const char* Webserver::routeUri(uint8_t index) const
        {
            return index < _sockets.size() ? _sockets[index].uri.c_str() : "";
        }

        // ── Common setup ───────────────────────────────────────────────────

#ifdef OPENKNX_WEBSERVER
        void Webserver::setup()
        {
            // Module::loop() calls setup() again every tick while isRunning() is false.
            // Without the guard routes, scripts and UI handlers would grow without bound,
            // and a duplicate prog handler would turn one click into two toggles.
            //
            // Registration only -- the server start lives in start() and must stay
            // repeatable, or one failed tcp_listen() means no web UI until reboot.
            if (_initialized) return;
            _initialized = true;

            addMenuItem("Übersicht", "/", -127);

            addRoute(WEB_GET, "/assets/base.css", Asset(WebAssets::base_css_mime, WebAssets::base_css_gz, sizeof(WebAssets::base_css_gz)));
            addRoute(WEB_GET, "/assets/base.js", Asset(WebAssets::base_js_mime, WebAssets::base_js_gz, sizeof(WebAssets::base_js_gz)));
            addRoute(WEB_GET, "/assets/logo.svg", Asset(WebAssets::logo_svg_mime, WebAssets::logo_svg_gz, sizeof(WebAssets::logo_svg_gz)));
            addRoute(WEB_GET, "/assets/favicon.svg", Asset(WebAssets::favicon_svg_mime, WebAssets::favicon_svg_gz, sizeof(WebAssets::favicon_svg_gz)));

            addStylesheet("/assets/base.css");
            addJavaScript("/assets/base.js");

            addRoute(WEB_GET, "/", [this](WebRequest&, WebResponse& res) {
                buildOverviewPage(res);
            });

            setupUiSocket();

            addRoute(WEB_POST, "/prog", [](WebRequest& req, WebResponse& res) {
                // kein std::stoi — wirft, und unter -fno-exceptions ist das ein abort()
                std::string mode = req.getQueryParam("mode");
                if (!mode.empty())
                {
                    char* end = nullptr;
                    long value = strtol(mode.c_str(), &end, 10);
                    if (end != mode.c_str() && *end == '\0')
                        knx.progMode(value != 0);
                }
                // Redirect to home
                res.setStatus(303);
                res.setHeader("Location", "/");
                res.send("");
            });
        }

        // Separate from setup() because the feature modules register their routes in
        // between: httpd serves from its own task, so opening the port earlier would let
        // handleRequest() iterate _routes while a push_back reallocates it.
        void Webserver::start()
        {
            if (isRunning()) return;
            // Module::loop() retries every tick; a scarce PCB or heap is no faster for it.
            if (!delayCheck(_startRetry, 1000)) return;
            _startRetry = millis();

            // No ring, no WS send path -- fail before the start so the retry covers both.
            if (!_txQueue.setup(OPENKNX_WEBSOCKET_TX_BUFFER, (uint32_t)maxWebsocketPayload()))
                return;

#ifdef ARDUINO_ARCH_ESP32
            setupEsp32();
#elif defined(ARDUINO_ARCH_RP2040)
            setupRp2040();
#endif
        }
#endif // OPENKNX_WEBSERVER (setup)

        void Webserver::addRoute(uint8_t method, const std::string& uri, WebRouteHandler handler)
        {
            _routes.push_back({method, uri, handler});
        }

        void Webserver::addSocket(const std::string& uri, WebSocketHandler onMessage,
                                  WebSocketConnectHandler onConnect)
        {
            _sockets.push_back({uri, onMessage, onConnect});
        }

        void Webserver::addMenuItem(const std::string& label, const std::string& uri, int8_t priority)
        {
            _menu.push_back({label, uri, priority});
            std::stable_sort(_menu.begin(), _menu.end(),
                             [](const WebMenuItem& a, const WebMenuItem& b) { return a.priority < b.priority; });
        }

        // Same lock as the writers: clients is mutated from the httpd task (ESP32) and
        // from the ethernet IRQ (RP2040) while these read it per telegram / per tick.
        bool Webserver::hasClients(const std::string& uri) const
        {
            WsQueueGuard guard(this);
            const WebSocketRoute* r = findSocketRoute(uri.c_str());
            return r && !r->clients.empty();
        }

        size_t Webserver::txCapacity() const
        {
            return _txQueue.capacity();
        }

        size_t Webserver::txFree() const
        {
            return _txQueue.freeBytes();
        }

        bool Webserver::handleRequest(WebRequest& req, WebResponse& res)
        {
            // Extract path without query string for routing
            std::string path = req.uri;
            size_t qpos = path.find('?');
            if (qpos != std::string::npos)
                path = path.substr(0, qpos);

            bool handled = false;
            for (auto& route : _routes)
            {
                if (route.method != req.method) continue;

                const std::string& pattern = route.uri;
                bool matched = false;

                if (pattern.size() >= 2 && pattern.back() == '*' && pattern[pattern.size() - 2] == '/')
                {
                    std::string prefix = pattern.substr(0, pattern.size() - 1);
                    matched = (path.compare(0, prefix.size(), prefix) == 0) ||
                              (path + "/" == prefix);
                }
                else
                {
                    matched = (path == pattern);
                }

                if (matched)
                {
                    route.handler(req, res);
                    handled = true;
                    break;
                }
            }

            if (!handled)
            {
                res.setStatus(404);
                res.setContentType("text/html");
                res.setLayout(true);
                res.send("<h2>404 &ndash; Seite nicht gefunden</h2>"
                         "<p class='meta'>Die angeforderte Seite existiert nicht.</p>");
            }

            // Layout anwenden und die Antwort in ihre Sendeblöcke zerlegen. Bewusst hier
            // und nicht im Plattformcode: der kennt danach nur noch res.segments() und
            // muss weder useLayout() auswerten noch buildHeader()/buildFooter() kennen.
            if (!res.isStreaming())
            {
                if (res.useLayout())
                    res.setLayoutChrome(
                        buildHeader(res.activeMenuUri().empty() ? path : res.activeMenuUri()),
                        buildFooter());
                res.finalizeSegments();
            }

            return handled;
        }

        // ── Route-Helpers ──────────────────────────────────────────────────

        WebRouteHandler Webserver::Redirect(const std::string& target)
        {
            return [target](WebRequest& req, WebResponse& res) {
                res.sendRedirect(target);
            };
        }

        WebRouteHandler Webserver::Static(const char* mimeType, const char* text)
        {
            std::string mime(mimeType);
            return [mime, text](WebRequest& req, WebResponse& res) {
                res.setContentType(mime.c_str());
                res.setHeader("Cache-Control", "public, max-age=86400");
                res.sendStatic(text);
            };
        }

        WebRouteHandler Webserver::Static(const char* mimeType, const uint8_t* data, int length)
        {
            std::string mime(mimeType);
            return [mime, data, length](WebRequest& req, WebResponse& res) {
                res.setContentType(mime.c_str());
                res.setHeader("Cache-Control", "public, max-age=86400");
                res.sendStatic(data, length);
            };
        }

        // Ein generiertes Web-Asset (webassets.h): mimeType/data kommen aus den
        // generierten WebAssets::x_mime/x_gz-Symbolen, length via sizeof(x_gz) am
        // Aufrufer — siehe sendAsset().
        WebRouteHandler Webserver::Asset(const char* mimeType, const uint8_t* data, size_t length)
        {
            return [mimeType, data, length](WebRequest& req, WebResponse& res) {
                res.sendAsset(mimeType, data, length);
            };
        }


        // ── Sendequeue: Ein- und Ausgang ──────────────────────────────────────
        // Beide Sendewege reihen nur ein. Sofort gesendet wird ausschliesslich im
        // Drain über wsSendNow(); ginge einer der beiden dorthin, reihte der Drain
        // endlos wieder ein statt zu senden.

        void Webserver::sendWebsocketMessage(const std::string& uri, const char* message, int fd)
        {
            if (!message) return;
            sendToClient(uri, fd, message, strlen(message));
        }

        // start() refuses without a ring, so isRunning() covers both. No direct-send
        // fallback on purpose -- a second fan-out path would behave differently.
        bool Webserver::sendToClient(const std::string& uri, int fd, const char* data, size_t len)
        {
            if (!isRunning() || !_txQueue.active()) return false;

            int route = routeIndex(uri.c_str());
            if (route < 0) return false;

            int16_t recFd = (fd >= 0) ? (int16_t)fd : WsTxQueue::FD_BROADCAST;
            WsQueueGuard guard(this);
            if (_txQueue.append(recFd, (uint8_t)route, nullptr, data, (uint32_t)len)) return true;

            logError("Webserver", "WS message to %s dropped: %u B > %u B",
                     uri.c_str(), (unsigned)len, (unsigned)_txQueue.maxRecord());
            return false;
        }

        // ── Client-An- und Abmeldung ──────────────────────────────────────────

        void Webserver::notifySocketConnect(const char* uri, int fd, bool connected)
        {
            WebSocketRoute* r = findSocketRoute(uri);

            if (connected)
            {
                // Cursor and client list under one lock, and the cursor necessarily
                // before the callback -- created later it would sit past its own data.
                const int route = routeIndex(uri);
                bool ok = false;
                {
                    WsQueueGuard guard(this);
                    ok = (route >= 0) && _txQueue.addClient(fd, (uint8_t)route);
                    if (ok && r) r->clients.push_back(fd);
                }
                // Unreachable while the cursor table covers the slot pool (static_assert
                // in Webserver.h); a client without a cursor would loop reconnecting.
                if (!ok)
                    logWarning("Webserver", "WS client %d has no cursor: send queue not ready or full", fd);
                return; // Zustellung übernimmt deliverPendingConnects()
            }

            // Trennen wird nicht verschoben: der Slot kann sofort neu belegt werden, und
            // eine nachlaufende Meldung räumte dann den Zustand des neuen Clients ab.
            bool delivered;
            {
                WsQueueGuard guard(this);
                if (r) r->clients.erase(std::remove(r->clients.begin(), r->clients.end(), fd), r->clients.end());
                delivered = _txQueue.removeClient(fd);
            }
            if (delivered && r && r->onConnect) r->onConnect(fd, false);
        }

        void Webserver::notifySocketMessage(const char* uri, int fd,
                                            uint8_t* data, int length, bool isBinary)
        {
            WebSocketRoute* r = findSocketRoute(uri);
            if (r && r->onMessage)
            {
                WebSocketFrame frame{data, length, isBinary};
                r->onMessage(fd, &frame);
            }
        }

        // Verbindungsmeldungen laufen bewusst über loop() statt über den Netzwerk-
        // Callback: dort ist es auf RP2040 der lwIP-Kontext, in dem nichts Längeres
        // passieren darf. So ist onConnect der natürliche Ort für Initialdaten je
        // Client, mit fd zur Hand und ohne Kontextregel.
        //
        // Known gap: on RP2040 the IRQ can report the disconnect nested inside
        // onConnect(true), so out of order -- still exactly once. See AGENTS.md.
        void Webserver::deliverPendingConnects()
        {
            int fds[WS_MAX_CLIENTS];
            uint8_t routes[WS_MAX_CLIENTS];
            int count = 0;

            {
                WsQueueGuard guard(this);
                for (int i = 0; i < _txQueue.clientCount() && count < WS_MAX_CLIENTS; i++)
                {
                    if (!_txQueue.takePendingConnect(i)) continue;
                    fds[count] = _txQueue.clientFd(i);
                    routes[count] = _txQueue.clientRoute(i);
                    count++;
                }
            }

            for (int i = 0; i < count; i++)
            {
                WebSocketRoute* r = findSocketRoute(routeUri(routes[i]));
                if (r && r->onConnect) r->onConnect(fds[i], true);
            }
        }

        void Webserver::drainTxQueue()
        {
            if (!_txQueue.active()) return;

            // Collect the clients to drop first: dropClient() ends in the module's
            // onConnect(false), which must not run under the lock -- a module taking its
            // own lock there would deadlock against it.
            int dropFd[WS_MAX_CLIENTS];
            const char* dropUri[WS_MAX_CLIENTS];
            int dropCount = 0;

            {
                WsQueueGuard guard(this);

                int i = 0;
                while (i < _txQueue.clientCount())
                {
                    const int fd = _txQueue.clientFd(i);
                    const char* uri = routeUri(_txQueue.clientRoute(i));
                    bool drop = false;

                    // Two reasons to disconnect, both meaning "stopped reading" -- not
                    // "slower than a busy producer". The producers throttle themselves.
                    if (_txQueue.overrun(i))
                    {
                        logWarning("Webserver", "WS client %d overtaken in the ring, disconnected", fd);
                        drop = true;
                    }
                    else
                    {
                        const uint8_t* data;
                        uint32_t len;
                        while (_txQueue.peek(i, data, len))
                        {
                            WsSendResult res = wsSendNow(uri, fd, (const char*)data, len);
                            if (res == WsSendResult::Ok)
                            {
                                _txQueue.advance(i);
                                continue;
                            }
                            // WouldBlock: cursor stays, retried next tick. Failed means
                            // never -- clean up, or the cursor parks on this record for
                            // good. Here and not in wsSendNow(), so both platforms share
                            // the behaviour.
                            drop = (res == WsSendResult::Failed);
                            break;
                        }

                        // After the loop, not before: peek() has taken the skipped foreign
                        // records along, so what is left is what this client refused.
                        if (!drop && _txQueue.stalled(i, millis(), 10000))
                        {
                            logWarning("Webserver", "WS client %d accepted nothing for 10 s, disconnected", fd);
                            drop = true;
                        }
                    }

                    if (drop && dropCount < WS_MAX_CLIENTS)
                    {
                        dropFd[dropCount] = fd;
                        dropUri[dropCount] = uri;
                        dropCount++;
                    }
                    i++;
                }

                // Free what everyone has read, or the ring fills up and the producers
                // that throttle on it starve.
                _txQueue.reclaim();
            }

            for (int k = 0; k < dropCount; k++)
                dropClient(dropUri[k], dropFd[k]);
        }


        // ── UI-Socket ─────────────────────────────────────────────────────────
        // Ein Socket für die gesamte Oberfläche, Nachrichten als "key:payload".
        // Die Firmware hält dabei keine Keytabelle: der Dispatcher splittet am ersten
        // ':' und reicht key/data/len an alle Registranten durch, die selbst per
        // strcmp entscheiden. Eingehend ist selten -- Konsolenbefehl, Prog-Klick,
        // Busmonitor-Schalter -- der Vergleich fällt also nicht ins Gewicht.

        void Webserver::Ui::sendMessage(const char* key, const char* data, size_t len)
        {
            Webserver& ws = openknxNetwork.webserver;
            if (ws._uiRouteIdx < 0 || !hasClients()) return;

            if (!ws._txQueue.active()) return;
            if (len == 0 && data) len = strlen(data);

            WsQueueGuard guard(&ws);
            if (!ws._txQueue.append(WsTxQueue::FD_BROADCAST, (uint8_t)ws._uiRouteIdx,
                                    key, data, (uint32_t)len))
            {
                // Too large means the caller must chunk. Reported once per key, not once
                // per boot -- otherwise the first hit swallows all the others.
                if (ws._uiOversizeKey != key)
                {
                    ws._uiOversizeKey = key;
                    logError("Webserver", "UI message '%s' dropped: %u B > %u B",
                             key, (unsigned)len, (unsigned)ws._txQueue.maxRecord());
                }
            }
        }

        bool Webserver::Ui::hasClients() const
        {
            const Webserver& ws = openknxNetwork.webserver;
            WsQueueGuard guard(&ws);
            return ws._uiRouteIdx >= 0 && !ws._sockets[ws._uiRouteIdx].clients.empty();
        }

        size_t Webserver::Ui::maxMessage() const
        {
            return openknxNetwork.webserver._txQueue.maxRecord();
        }

        void Webserver::Ui::receiveMessage(WebUiHandler cb)
        {
            if (cb) openknxNetwork.webserver._uiHandlers.push_back(cb);
        }

        void Webserver::notifyUiHandlers(const char* key, const char* data, size_t len)
        {
            for (auto& cb : _uiHandlers)
                cb(key, data, len);
        }

        void Webserver::setupUiSocket()
        {
            addSocket(UI_WS_URI,
                      [this](int, WebSocketFrame* frame) {
                          if (!frame || frame->length <= 0) return;
                          const char* raw = (const char*)frame->data;
                          int sep = -1;
                          for (int i = 0; i < frame->length; i++)
                              if (raw[i] == ':')
                              {
                                  sep = i;
                                  break;
                              }
                          if (sep <= 0) return;

                          // Key onto the stack -- the frame is not NUL-terminated. The
                          // handler thus gets a terminated key but raw data: len is its
                          // only bound. Keys of 24+ chars are dropped; they are protocol
                          // constants, not payload, so no heap for them.
                          char key[24];
                          if (sep >= (int)sizeof(key)) return;
                          memcpy(key, raw, sep);
                          key[sep] = 0;

                          // Reserved keys come from inside only: a client sending "connect"
                          // would run publishStatus() in the network callback (RP2040: the
                          // ethernet IRQ, ADC delay(1) included) on an open endpoint.
                          if (strcmp(key, "connect") == 0 || strcmp(key, "pong") == 0) return;

                          notifyUiHandlers(key, raw + sep + 1, (size_t)(frame->length - sep - 1));
                      },
                      [this](int, bool connected) {
                          // Reservierter Key: "ein Client ist da, schick deinen Stand".
                          // Von einem Reconnect nicht zu unterscheiden -- gewollt, der
                          // Client synchronisiert sich damit vollständig neu.
                          if (connected) notifyUiHandlers("connect", nullptr, 0);
                      });
            _uiRouteIdx = routeIndex(UI_WS_URI);

            ui.receiveMessage([this](const char* key, const char* data, size_t len) {
                // The browser's liveness check. The device detects dead clients itself
                // (WS ping, TCP keepalive); the other direction has nothing, so after a
                // reboot the page would stay unaware. The reply is deliberately empty --
                // the client only evaluates that something arrived. Safe in the network
                // callback: sendMessage() only enqueues and does not allocate.
                if (strcmp(key, "ping") == 0)
                {
                    ui.sendMessage("pong", nullptr, 0);
                    return;
                }
                if (strcmp(key, "connect") == 0)
                {
                    publishStatus(true);
                    return;
                }
                if (strcmp(key, "prog") == 0 && len > 0)
                {
                    // Record only, see _pendingProg -- this handler is also the example
                    // for the rule that applies to every registrant.
                    _pendingProg = (data[0] == 't') ? 2 : (data[0] != '0' ? 1 : 0);
                }
            });
        }

        // ── Statusdaten ───────────────────────────────────────────────────────
        // Kein Takt: gesendet wird beim Verbinden und wenn sich etwas geändert hat.
        // Die Änderungserkennung gehört dem Sender, nicht der Transportschicht --
        // verglichen werden die Skalare selbst, kein Payload und kein Hash.

        void Webserver::publishStatus(bool force)
        {
            if (!ui.hasClients()) return;

            const bool net = openknxNetwork.established();

            StatusSnapshot now;
            now.valid = true;
            now.prog = knx.progMode();
            now.configured = knx.configured();
            now.netUp = net;
            now.ip = net ? (uint32_t)openknxNetwork.localIP() : 0;
            now.mask = net ? (uint32_t)openknxNetwork.subnetMask() : 0;
            now.gw = net ? (uint32_t)openknxNetwork.gatewayIP() : 0;
            now.dns = net ? (uint32_t)openknxNetwork.nameServerIP() : 0;
            now.pa = openknx.info.individualAddress();

            // Sample memory and temperature only every 2 s: on RP2040 cpuTemperature()
            // is analogReadTemp() with a blocking delay(1), which capped the KNX loop at
            // ~1 kHz when measured per tick, and freeMemory() walks the free list. Whole
            // degrees because one ADC LSB is ~0.47 °C -- finer resolution would let noise
            // flip the comparison below on every tick.
            if (_lastStatus.valid && !delayCheck(_lastSample, 2000))
            {
                now.memKiB = _lastStatus.memKiB;
                now.cpuC = _lastStatus.cpuC;
            }
            else
            {
                _lastSample = millis();
                now.memKiB = (uint32_t)(freeMemory() / 1024);
                now.cpuC = (int16_t)openknx.hardware.cpuTemperature();
            }
#if defined(OPENKNX_TELEGRAMJSON) && defined(OPENKNX_WEBMONITOR) && MASK_VERSION != 0x091A
            now.busmon = openknxNetwork.tpUart().isMonitoring();
#endif

            if (!force && _lastStatus.valid &&
                now.prog == _lastStatus.prog && now.configured == _lastStatus.configured &&
                now.netUp == _lastStatus.netUp && now.busmon == _lastStatus.busmon &&
                now.ip == _lastStatus.ip && now.mask == _lastStatus.mask &&
                now.gw == _lastStatus.gw && now.dns == _lastStatus.dns &&
                now.pa == _lastStatus.pa && now.memKiB == _lastStatus.memKiB &&
                now.cpuC == _lastStatus.cpuC)
                return;

            _lastStatus = now;

            // Member: reset() keeps the capacity, so building it stops allocating.
            OpenKNX::Format::JSON::Writer& json = _statusJson;
            json.reset();
            json.beginObject();
            json.field("prog", now.prog);
            json.field("cfg", now.configured);
            json.field("pa", openknx.info.humanIndividualAddress());
            json.field("net", net);
            json.field("mem", (uint32_t)now.memKiB);
            // Unconditional: a missing field leaves the browser showing the old value.
            // Without a sensor the element does not exist, so it goes nowhere.
            json.field("cpu", (int32_t)now.cpuC);
            if (net)
            {
                json.field("ip", openknxNetwork.localIP().toString().c_str());
                json.field("mask", openknxNetwork.subnetMask().toString().c_str());
                json.field("gw", openknxNetwork.gatewayIP().toString().c_str());
                json.field("dns", openknxNetwork.nameServerIP().toString().c_str());
            }
#if defined(OPENKNX_TELEGRAMJSON) && defined(OPENKNX_WEBMONITOR) && MASK_VERSION != 0x091A
            json.field("busmon", now.busmon);
#endif
            // Nur beim Verbinden: danach zählt der Browser selbst weiter, sonst müsste
            // sekündlich etwas gesendet werden, nur damit die Uptime läuft.
            // uptime() statt millis()/1000 -- letzteres kippt nach 49,7 Tagen zurück.
            if (force) json.field("up", uptime());
            json.endObject();

            ui.sendMessage("status", json.c_str());
        }

        void Webserver::loop()
        {
            loopPlatform();

            if (_pendingProg >= 0)
            {
                const int8_t cmd = _pendingProg;
                _pendingProg = -1;
                if (cmd == 2)
                    knx.toggleProgMode();
                else
                    knx.progMode(cmd != 0);
                publishStatus(true);
            }

            deliverPendingConnects();
            publishStatus(false);
            drainTxQueue();
        }

        void Webserver::logRequest(const WebRequest& req, const WebResponse& res)
        {
            int statusCode = res.statusCode();
            if (statusCode < 400 && !_accessLog) return;

            // Get remote address as string
            uint32_t remoteIp = req.getRemoteAddr();
            uint16_t remotePort = req.getRemotePort();

            uint8_t a = (remoteIp >> 0) & 0xFF;
            uint8_t b = (remoteIp >> 8) & 0xFF;
            uint8_t c = (remoteIp >> 16) & 0xFF;
            uint8_t d = (remoteIp >> 24) & 0xFF;
            char clientIp[16];
            snprintf(clientIp, sizeof(clientIp), "%u.%u.%u.%u", a, b, c, d);

            // Get method string
            const char* method = "?";
            switch (req.method)
            {
                case WEB_GET: method = "GET"; break;
                case WEB_POST: method = "POST"; break;
                case WEB_PUT: method = "PUT"; break;
                case WEB_DELETE: method = "DELETE"; break;
            }

            if (statusCode >= 500)
                logError("Webserver", "%s %s - %s:%u - %d", method, req.getUri().c_str(), clientIp, remotePort, statusCode);
            else if (statusCode >= 400)
                logWarning("Webserver", "%s %s - %s:%u - %d", method, req.getUri().c_str(), clientIp, remotePort, statusCode);
            else
                logInfo("Webserver", "%s %s - %s:%u - %d", method, req.getUri().c_str(), clientIp, remotePort, statusCode);
        }

        void Webserver::logWebsocket(const WebRequest& req, int statusCode)
        {
            if (statusCode < 400 && !_accessLog) return;

            // Get remote address as string
            uint32_t remoteIp = req.getRemoteAddr();
            uint16_t remotePort = req.getRemotePort();

            uint8_t a = (remoteIp >> 0) & 0xFF;
            uint8_t b = (remoteIp >> 8) & 0xFF;
            uint8_t c = (remoteIp >> 16) & 0xFF;
            uint8_t d = (remoteIp >> 24) & 0xFF;
            char clientIp[16];
            snprintf(clientIp, sizeof(clientIp), "%u.%u.%u.%u", a, b, c, d);

            if (statusCode >= 500)
                logError("Webserver", "WS %s - %s:%u - %d", req.getUri().c_str(), clientIp, remotePort, statusCode);
            else if (statusCode >= 400)
                logWarning("Webserver", "WS %s - %s:%u - %d", req.getUri().c_str(), clientIp, remotePort, statusCode);
            else
                logInfo("Webserver", "WS %s - %s:%u - %d", req.getUri().c_str(), clientIp, remotePort, statusCode);
        }

    } // namespace Network
} // namespace OpenKNX

#endif // defined(OPENKNX_WEBSERVER) && (defined(KNX_IP_WIFI) || defined(KNX_IP_LAN))
