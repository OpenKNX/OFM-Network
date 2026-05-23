#include "OpenKNX/Network/Webserver/Webserver.h"

#if defined(OPENKNX_WEBSERVER) && (defined(KNX_IP_WIFI) || defined(KNX_IP_LAN))

#include "OpenKNX.h"
#include "OpenKNX/Network/Module.h"
#include <algorithm>
#include <cstring>
#include <string>

namespace OpenKNX
{
    namespace Network
    {

        // ── Layout ─────────────────────────────────────────────────────────

        static const char baseCss[] =
            "*{box-sizing:border-box;margin:0;padding:0}"
            "button,input,select,textarea{font-family:inherit;font-size:inherit;padding:6px;margin:revert;border:revert;background:revert;color:revert}"
            "body{display:flex;min-height:100vh;font-family:sans-serif;background:#fff;color:#111}"
            "nav{width:220px;min-width:220px;background:#000;display:flex;flex-direction:column;"
            "align-items:center;padding:6px 0;height:100vh;position:sticky;top:0;overflow-y:auto;"
            "box-shadow:4px 0 18px rgba(0,0,0,.45);z-index:1}"
            ".logo{width:180px;padding:0 24px;margin-top:12px;margin-bottom:12px}"
            ".logo img{width:100%;height:auto}"
            "nav a.menu{display:block;width:100%;padding:11px 24px;color:#aaa;text-decoration:none;"
            "font-size:.9em;border-left:3px solid transparent;transition:all .15s}"
            "nav a.menu:hover{background:#1a1a1a;color:#fff;border-left-color:#449841}"
            "a.menu.active{color:#fff;border-left-color:#449841;background:#111}"
            ".logo a{display:block;padding:0}"
            ".nav-wiki{margin-top:auto;width:100%;padding:8px 0 8px;text-align:center}"
            ".nav-wiki a,.nav-wiki a:hover{color:#555;font-size:.8em;padding:0;border:none;"
            "background:none;display:inline;width:auto;text-decoration:none}"
            ".nav-wiki a:hover{color:#888}"
            ".menu-container{width:100%;margin:12px 0;display:flex;flex-direction:column}"
            ".nav-footer{width:100%;padding:12px 24px 12px 27px;border-top:1px solid #222;"
            "display:flex;flex-direction:column;gap:10px}"
            ".nav-footer a{color:#666;text-decoration:none}"
            ".prog-status{display:flex;align-items:center;color:#666;font-size:.8em;gap:8px}"
            ".prog-dot{display:inline-block;width:8px;height:8px;border-radius:50%;flex-shrink:0}"
            ".dot-green{background:#449841}.dot-red{background:#ff5555}.dot-off{background:#555}"
            ".nav-firm{font-weight:bold;font-size:.8em;color:#aaa}"
            ".nav-info{display:flex;align-items:center;color:#666;font-size:.8em;gap:8px}"
            "main{flex:1;padding:2em;min-width:0;display:flex;flex-direction:column}"
            ".meta{color:#666;font-size:.8em;margin-bottom:1.5em}"
            ".red{color:#ff5555}.green{color:#449841}.yellow{color:#ffff55}.gray{color:#888}"
            "h1{font-size:1.25em;font-weight:600;margin-bottom:1.5em;color:#111}"
            "h2{font-size:.95em;font-weight:600;margin:1.5em 0 .75em;color:#449841;"
            "text-transform:uppercase;letter-spacing:.08em}"
            "table{width:100%;border-collapse:collapse;margin-bottom:.5em;font-size:.88em}"
            "td,th{text-align:left;padding:7px 14px}"
            "th{background:#eee;color:#000;border-bottom:1px solid #bbb}"
            "td{border-top:1px solid #eee}"
            "tr:last-child td{border-bottom:1px solid #eee}"
            ".attribute-table td:first-child{color:#888;white-space:nowrap;width:200px}"
            "main a{color:#449841;text-decoration:none;transition:color .15s}"
            "main a:hover{color:#5ab857;text-decoration:underline}"
            ".container{max-width:960px}";

        static const char baseJs[] = "/* OpenKNX */";

        static const char faviconSvg[] =
            "<svg width='32' height='32' viewBox='0 0 32 32' fill='none' xmlns='http://www.w3.org/2000/svg'>"
            "<line x1='7' y1='20' x2='7' y2='15' stroke='#449841' stroke-width='2'/>"
            "<line x1='25' y1='17' x2='25' y2='12' stroke='#449841' stroke-width='2'/>"
            "<line y1='16' x2='32' y2='16' stroke='#449841' stroke-width='2'/>"
            "<rect x='2' y='21' width='10' height='10' fill='#449841'/>"
            "<rect x='20' y='1' width='10' height='10' fill='#449841'/>"
            "<rect x='22' y='23' width='6' height='6' stroke='black' stroke-width='2'/>"
            "<rect x='4' y='3' width='6' height='6' stroke='black' stroke-width='2'/>"
            "</svg>";

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
                "<title>OpenKNX</title>"
                "<link rel='icon' type='image/svg+xml' href='/assets/favicon.svg?" +
                _assetCacheBuster + "'>";

            // Add registered stylesheets
            for (const auto& stylesheet : _stylesheets)
            {
                html += "<link rel='stylesheet' href='" + stylesheet + "?" + _assetCacheBuster + "'>";
            }

            html += "</head><body>"
                    "<nav>"
                    "<div class='logo'>"
                    "<a href='https://www.openknx.de' target='_blank' rel='noopener noreferrer'>"
                    "<img src='/assets/logo/black.svg?" +
                    _assetCacheBuster + "' alt='OpenKNX'>"
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
            html += "</div>"
                    "<a class='prog-status' href='/prog?mode=";
            html += knx.progMode() ? "0" : "1";
            html += "'>"
                    "<span class='prog-dot ";
            html += knx.progMode() ? "dot-red" : "dot-off";
            html += "'></span>";
            html += knx.progMode() ? "Prog-Modus aktiv" : "Prog-Modus inaktiv";
            html += "</a>"
                    "</div>";
            html += "</nav><main>";
            return html;
        }

        std::string Webserver::buildFooter()
        {
            std::string html = "</main>";

            // Add registered scripts before closing body
            for (const auto& script : _scripts)
            {
                html += "<script src='" + script + "?" + _assetCacheBuster + "' defer></script>";
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
            auto row = [](const char* label, const std::string& val) -> std::string {
                return std::string("<tr><td>") + label + "</td><td>" + val + "</td></tr>";
            };

            char buf[64];
            std::string page = "<div class='container'><h1>Übersicht</h1>";

            // ── Gerät ──────────────────────────────────────────────────────────
            page += "<h2>Gerät</h2><table class='attribute-table'><tbody>";
#ifdef DEVICE_ID
            page += row("ID", DEVICE_ID);
#endif
#ifdef DEVICE_NAME
            page += row("Name", DEVICE_NAME);
#elif defined(HARDWARE_NAME)
            page += row("Name", HARDWARE_NAME);
#endif
            page += row("Seriennummer", openknx.info.humanSerialNumber());
            page += "</tbody></table>";

            // ── Firmware ───────────────────────────────────────────────────────
            page += "<h2>Firmware</h2><table class='attribute-table'><tbody>";
            page += row("Name", openknx.info.firmwareName());
            page += row("Version", openknx.info.humanFirmwareVersion(true));
            page += row("Nummer", openknx.info.humanFirmwareNumber());
#if MASK_VERSION == 0x07B0
            page += row("KNX-Typ", "TP (07B0)");
#elif MASK_VERSION == 0x57B0
            page += row("KNX-Typ", "IP (57B0)");
#elif MASK_VERSION == 0x091A
            page += row("KNX-Typ", "Router (091A)");
#else
            snprintf(buf, sizeof(buf), "%04X", MASK_VERSION);
            page += row("KNX-Typ", buf);
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
                    page += row("CPU-Modus", buf);
                }
                else
                    page += row("CPU-Modus", cpuMode);
            }
            page += "</tbody></table>";

            // ── Applikation ────────────────────────────────────────────────────
            page += "<h2>Applikation</h2><table class='attribute-table'><tbody>";
            {
                std::string addr = openknx.info.humanIndividualAddress();
                addr += knx.configured()
                            ? " <span class='green'>(Konfiguriert)</span>"
                            : " <span class='gray'>(Nicht konfiguriert)</span>";
                page += row("KNX-Adresse", addr);
            }
            if (openknx.info.applicationNumber() > 0)
            {
                page += row("Version", openknx.info.humanApplicationVersion());
                page += row("Nummer", openknx.info.humanApplicationNumber());
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
                page += row("Uptime", buf);
            }
            {
                float fm = freeMemory() / 1024.0f;
                float fmMin = openknx.common.freeMemoryMin() / 1024.0f;
                snprintf(buf, sizeof(buf), "%.3f KiB (min. %.3f KiB)", fm, fmMin);
                page += row("Freier Speicher", buf);
            }
#ifdef OPENKNX_WATCHDOG
            if (openknx.watchdog.active())
            {
                snprintf(buf, sizeof(buf), "Running (%is)", (int)openknx.watchdog.maxPeriod());
                page += row("Watchdog", buf);
            }
            else
                page += row("Watchdog", "Disabled");
#else
            page += row("Watchdog", "Unsupported");
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
                page += row("Hostname", openknxNetwork.hostName());
                page += row("MAC", macStr);
                page += row("IP-Adresse", net ? openknxNetwork.localIP().toString().c_str()
                                              : "<span class='gray'>–</span>");
                page += row("Subnetz", net ? openknxNetwork.subnetMask().toString().c_str()
                                           : "<span class='gray'>–</span>");
                page += row("Gateway", net ? openknxNetwork.gatewayIP().toString().c_str()
                                           : "<span class='gray'>–</span>");
                page += row("DNS", net ? openknxNetwork.nameServerIP().toString().c_str()
                                       : "<span class='gray'>–</span>");
                page += "</tbody></table>";
            }

            // ── Versionen ──────────────────────────────────────────────────────
            page += "<h2>Versionen</h2><table class='attribute-table'><tbody>";
            page += row("This Firmware", openknx.info.humanFirmwareVersion(true));
#ifdef KNX_Version
            page += row("KNX", KNX_Version);
#endif
#ifdef MODULE_Common_Version
            page += row(openknx.common.logPrefix().c_str(), MODULE_Common_Version);
#endif
            for (uint8_t i = 0; i < openknx.modules.count; i++)
            {
                if (openknx.modules.list[i]->version().empty()) continue;
                page += row(openknx.modules.list[i]->name().c_str(),
                            openknx.modules.list[i]->version());
            }
            page += "<tr><td colspan='2' style='padding-top:1em'></td></tr>";
            page += row("Build-Datum", __DATE__);
            page += row("Build-Zeit", __TIME__);
            page += "</tbody></table>";

            page += "</div>";
            res.setContentType("text/html");
            res.setLayout(true);
            res.send(page.c_str());
        }
#endif // OPENKNX_WEBSERVER

        // ── Routing ────────────────────────────────────────────────────────

        bool Webserver::hasSocket(const std::string& uri) const
        {
            for (auto& s : _sockets)
                if (s.uri == uri) return true;
            return false;
        }

        // ── Common setup ───────────────────────────────────────────────────

#ifdef OPENKNX_WEBSERVER
        void Webserver::setup()
        {
            // Create cache buster from build time: YYYYMMDDHHII format
            // __DATE__ = "May 23 2026", __TIME__ = "14:35:42"
            static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
            std::string dateStr = __DATE__;
            std::string timeStr = __TIME__;

            int month = 1;
            std::string monthStr = dateStr.substr(0, 3);
            for (int i = 0; i < 12; i++)
            {
                if (monthStr == months[i])
                {
                    month = i + 1;
                    break;
                }
            }

            char buf[16];
            snprintf(buf, sizeof(buf), "%s%02d%s%s",
                     dateStr.substr(7, 4).c_str(), // YYYY
                     month,                        // MM (01-12)
                     dateStr.substr(4, 2).c_str(), // DD
                     timeStr.substr(0, 5).c_str()  // HHMM
            );
            _assetCacheBuster = buf;

            addMenuItem("Übersicht", "/", -127);

            static const char logoSvg[] =
                "<svg viewBox='0 0 402 242' fill='none' xmlns='http://www.w3.org/2000/svg'>"
                "<path d='M1 50.0049C1 34.6777 5.11523 22.689 13.3457 14.0386C21.5762 5.34619 32.2002 1 45.2178 1C53.7422 1 61.4268 3.03662 68.2715 7.10986C75.1162 11.1831 80.3232 16.873 83.8926 24.1797C87.5039 31.4443 89.3096 39.6958 89.3096 48.9341C89.3096 58.2983 87.4199 66.6758 83.6406 74.0664C79.8613 81.457 74.5073 87.063 67.5786 90.8843C60.6499 94.6636 53.1753 96.5532 45.1548 96.5532C36.4624 96.5532 28.6938 94.4536 21.8491 90.2544C15.0044 86.0552 9.81836 80.3232 6.29102 73.0586C2.76367 65.7939 1 58.1094 1 50.0049ZM13.5977 50.1938C13.5977 61.3218 16.5791 70.0981 22.542 76.5229C28.5469 82.9058 36.0635 86.0972 45.0918 86.0972C54.2881 86.0972 61.8467 82.8638 67.7676 76.397C73.7305 69.9302 76.7119 60.7549 76.7119 48.8711C76.7119 41.3545 75.4312 34.8037 72.8696 29.2188C70.3501 23.5918 66.6338 19.2456 61.7207 16.1802C56.8496 13.0728 51.3696 11.519 45.2808 11.519C36.6304 11.519 29.1768 14.5005 22.9199 20.4634C16.7051 26.3843 13.5977 36.2944 13.5977 50.1938Z' fill='white'/>"
                "<path d='M103.671 120.978V28.085H114.001V36.7773C116.437 33.376 119.187 30.8354 122.252 29.1558C125.318 27.4341 129.034 26.5732 133.401 26.5732C139.112 26.5732 144.151 28.043 148.519 30.9824C152.886 33.9219 156.182 38.0791 158.408 43.4541C160.633 48.7871 161.746 54.645 161.746 61.0278C161.746 67.8726 160.507 74.0454 158.03 79.5464C155.594 85.0054 152.025 89.2046 147.322 92.144C142.661 95.0415 137.748 96.4902 132.583 96.4902C128.803 96.4902 125.402 95.6924 122.378 94.0967C119.397 92.501 116.94 90.4854 115.009 88.0498V120.978H103.671ZM113.938 61.9097C113.938 70.5181 115.681 76.8799 119.166 80.9951C122.651 85.1104 126.872 87.168 131.827 87.168C136.866 87.168 141.17 85.0474 144.739 80.8062C148.351 76.5229 150.156 69.9092 150.156 60.9648C150.156 52.4404 148.393 46.0576 144.865 41.8164C141.38 37.5752 137.202 35.4546 132.331 35.4546C127.501 35.4546 123.218 37.7222 119.481 42.2573C115.786 46.7505 113.938 53.3013 113.938 61.9097Z' fill='white'/>"
                "<path d='M221.27 73.4365L232.986 74.8853C231.138 81.73 227.716 87.042 222.719 90.8213C217.722 94.6006 211.339 96.4902 203.57 96.4902C193.786 96.4902 186.018 93.4878 180.265 87.4829C174.554 81.436 171.698 72.9746 171.698 62.0986C171.698 50.8447 174.596 42.1104 180.391 35.8955C186.186 29.6807 193.702 26.5732 202.94 26.5732C211.885 26.5732 219.191 29.6177 224.86 35.7065C230.529 41.7954 233.364 50.3618 233.364 61.4058C233.364 62.0776 233.343 63.0854 233.301 64.4292H183.414C183.834 71.7778 185.913 77.4048 189.65 81.3101C193.387 85.2153 198.048 87.168 203.633 87.168C207.791 87.168 211.339 86.0762 214.278 83.8926C217.218 81.709 219.548 78.2236 221.27 73.4365ZM184.044 55.1069H221.396C220.892 49.48 219.464 45.2598 217.113 42.4463C213.501 38.0791 208.819 35.8955 203.066 35.8955C197.859 35.8955 193.471 37.6382 189.902 41.1235C186.375 44.6089 184.422 49.27 184.044 55.1069Z' fill='white'/>"
                "<path d='M247.284 94.9785V28.085H257.488V37.5962C262.401 30.2476 269.498 26.5732 278.778 26.5732C282.81 26.5732 286.505 27.3081 289.864 28.7778C293.266 30.2056 295.806 32.0952 297.486 34.4468C299.166 36.7983 300.341 39.5908 301.013 42.8242C301.433 44.9238 301.643 48.5981 301.643 53.8472V94.9785H290.305V54.2881C290.305 49.6689 289.864 46.2256 288.982 43.958C288.101 41.6484 286.526 39.8218 284.258 38.478C282.033 37.0923 279.408 36.3994 276.385 36.3994C271.556 36.3994 267.377 37.9321 263.85 40.9976C260.365 44.063 258.622 49.8789 258.622 58.4453V94.9785H247.284Z' fill='white'/>"
                "<path d='M103.671 242.478V137.969H124.773V184.378L167.404 137.969H195.777L156.425 178.675L197.916 242.478H170.612L141.882 193.431L124.773 210.897V242.478H103.671Z' fill='white'/>"
                "<path d='M209.108 242.478V137.969H229.639L272.413 207.76V137.969H292.017V242.478H270.844L228.712 174.326V242.478H209.108Z' fill='white'/>"
                "<path d='M303.78 242.478L339.496 187.942L307.13 137.969H331.796L352.755 171.546L373.287 137.969H397.739L365.231 188.726L400.947 242.478H375.497L352.328 206.335L329.087 242.478H303.78Z' fill='white'/>"
                "<line x1='-0.00111127' y1='116' x2='97.763' y2='115.978' stroke='#449841' stroke-width='10'/>"
                "<line x1='120.764' y1='115.978' x2='403' y2='115.978' stroke='#449841' stroke-width='10'/>"
                "<line x1='352.764' y1='98.9783' x2='352.764' y2='110.978' stroke='#449841' stroke-width='10'/>"
                "<rect x='318.764' y='27.9783' width='67' height='67' fill='#449841'/>"
                "<line x1='45.7642' y1='132.978' x2='45.7642' y2='120.978' stroke='#449841' stroke-width='10'/>"
                "<rect x='79.7642' y='204.978' width='67' height='67' transform='rotate(-180 79.7642 204.978)' fill='#449841'/>"
                "</svg>";
            addRoute(WEB_GET, "/assets/base.css", Static("text/css", baseCss));
            addRoute(WEB_GET, "/assets/base.js", Static("text/javascript", baseJs));
            addRoute(WEB_GET, "/assets/logo/black.svg", Static("image/svg+xml", logoSvg));
            addRoute(WEB_GET, "/assets/favicon.svg", Static("image/svg+xml", faviconSvg));

            addStylesheet("/assets/base.css");
            addJavaScript("/assets/base.js");

            addRoute(WEB_GET, "/", [this](WebRequest&, WebResponse& res) {
                buildOverviewPage(res);
            });

            addRoute(WEB_GET, "/prog", [](WebRequest& req, WebResponse& res) {
                // Parse mode parameter from query string
                size_t pos = req.uri.find("mode=");
                if (pos != std::string::npos)
                {
                    int mode = std::stoi(req.uri.substr(pos + 5));
                    knx.progMode(mode != 0);
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
            auto it = _socketClients.find(uri);
            if (it == _socketClients.end()) return {};
            return it->second;
        }

        bool Webserver::handleRequest(WebRequest& req, WebResponse& res)
        {
            // Extract path without query string for routing
            std::string path = req.uri;
            size_t qpos = path.find('?');
            if (qpos != std::string::npos)
                path = path.substr(0, qpos);

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
                    return true;
                }
            }

            res.setStatus(404);
            res.setContentType("text/html");
            res.setLayout(true);
            res.send("<h2>404 &ndash; Seite nicht gefunden</h2>"
                     "<p class='meta'>Die angeforderte Seite existiert nicht.</p>");
            return false;
        }

        // ── Route-Helpers ──────────────────────────────────────────────────

        WebRouteHandler Webserver::Redirect(const std::string& target)
        {
            return [target](WebRequest& req, WebResponse& res) {
                res.setStatus(301);
                res.setHeader("Location", target.c_str());
                res.send("");
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

        void Webserver::logRequest(const WebRequest& req, const WebResponse& res)
        {
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

            // Get status string (just the code)
            int statusCode = res.statusCode();

            logInfo("Webserver", "%s %s - %s:%u - %d", method, req.getUri().c_str(), clientIp, remotePort, statusCode);
        }

        void Webserver::logWebsocket(const WebRequest& req, int statusCode)
        {
            // Get remote address as string
            uint32_t remoteIp = req.getRemoteAddr();
            uint16_t remotePort = req.getRemotePort();

            uint8_t a = (remoteIp >> 0) & 0xFF;
            uint8_t b = (remoteIp >> 8) & 0xFF;
            uint8_t c = (remoteIp >> 16) & 0xFF;
            uint8_t d = (remoteIp >> 24) & 0xFF;
            char clientIp[16];
            snprintf(clientIp, sizeof(clientIp), "%u.%u.%u.%u", a, b, c, d);

            logInfo("Webserver", "WS %s - %s:%u - %d", req.getUri().c_str(), clientIp, remotePort, statusCode);
        }

    } // namespace Network
} // namespace OpenKNX

#endif // defined(OPENKNX_WEBSERVER) && (defined(KNX_IP_WIFI) || defined(KNX_IP_LAN))
