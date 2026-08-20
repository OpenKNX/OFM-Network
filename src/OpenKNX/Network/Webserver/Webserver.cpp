#include "OpenKNX/Network/Webserver/Webserver.h"

#if defined(OPENKNX_WEBSERVER) && (defined(KNX_IP_WIFI) || defined(KNX_IP_LAN))

#include "OpenKNX.h"
#include "OpenKNX/Network/Module.h"
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
                    "<span class='prog-dot ";
            html += knx.configured() ? "dot-green" : "dot-off";
            html += "'></span>Adresse: ";
            html += openknx.info.humanIndividualAddress();
            // Formular statt Link — /prog ist bewusst POST-only
            html += "</div>"
                    "<form class='prog-status' method='post' action='/prog?mode=";
            html += knx.progMode() ? "0" : "1";
            html += "'>"
                    "<button type='submit' class='prog-status-btn'>"
                    "<span class='prog-dot ";
            html += knx.progMode() ? "dot-red" : "dot-off";
            html += "'></span>";
            html += knx.progMode() ? "Prog-Modus aktiv" : "Prog-Modus inaktiv";
            html += "</button>"
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
            auto row = [&page](const char* label, const std::string& val) {
                page += "<tr><td>";
                page += label;
                page += "</td><td>";
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
                    snprintf(buf, sizeof(buf), "%s (%.1f °C)", cpuMode, temp);
                    row("CPU-Modus", buf);
                }
                else
                    row("CPU-Modus", cpuMode);
            }
            page += "</tbody></table>";

            // ── Applikation ────────────────────────────────────────────────────
            page += "<h2>Applikation</h2><table class='attribute-table'><tbody>";
            {
                std::string addr = openknx.info.humanIndividualAddress();
                addr += knx.configured()
                            ? " <span class='green'>(Konfiguriert)</span>"
                            : " <span class='gray'>(Nicht konfiguriert)</span>";
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
                uint32_t s = millis() / 1000;
                uint32_t d = s / 86400;
                s %= 86400;
                uint32_t h = s / 3600;
                s %= 3600;
                uint32_t m = s / 60;
                s %= 60;
                snprintf(buf, sizeof(buf), "%ud %02u:%02u:%02u", d, h, m, s);
                row("Uptime", buf);
            }
            {
                float fm = freeMemory() / 1024.0f;
                float fmMin = openknx.common.freeMemoryMin() / 1024.0f;
                snprintf(buf, sizeof(buf), "%.3f KiB (min. %.3f KiB)", fm, fmMin);
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
                                              : "<span class='gray'>–</span>");
                row("Subnetz", net ? openknxNetwork.subnetMask().toString().c_str()
                                           : "<span class='gray'>–</span>");
                row("Gateway", net ? openknxNetwork.gatewayIP().toString().c_str()
                                           : "<span class='gray'>–</span>");
                row("DNS", net ? openknxNetwork.nameServerIP().toString().c_str()
                                       : "<span class='gray'>–</span>");
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

        // ── Common setup ───────────────────────────────────────────────────

#ifdef OPENKNX_WEBSERVER
        void Webserver::setup()
        {
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

        std::vector<int> Webserver::connectedClientFds(const std::string& uri) const
        {
#ifdef ARDUINO_ARCH_ESP32
            // Snapshot under the WS state lock — the list is mutated from the httpd task
            // (upgrade) and the loop task (disconnect) while it is read here.
            wsStateLock();
            const WebSocketRoute* r = findSocketRoute(uri.c_str());
            std::vector<int> result = r ? r->clients : std::vector<int>{};
            wsStateUnlock();
            return result;
#else
            const WebSocketRoute* r = findSocketRoute(uri.c_str());
            return r ? r->clients : std::vector<int>{};
#endif
        }

        bool Webserver::hasClients(const std::string& uri) const
        {
#ifdef ARDUINO_ARCH_ESP32
            wsStateLock();
            const WebSocketRoute* r = findSocketRoute(uri.c_str());
            bool has = r && !r->clients.empty();
            wsStateUnlock();
            return has;
#else
            const WebSocketRoute* r = findSocketRoute(uri.c_str());
            return r && !r->clients.empty();
#endif
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
