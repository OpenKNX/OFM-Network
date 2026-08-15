#include "OpenKNX/Network/TelegramJson.h" // defines OPENKNX_TELEGRAMJSON
#if defined(OPENKNX_TELEGRAMJSON) && defined(OPENKNX_WEBMONITOR)

#include "OpenKNX/Network/Webserver/GroupMonitor.h"
#include "OpenKNX.h"
#include "OpenKNX/Network/Module.h"
#include "OpenKNX/Network/Webserver/Webserver.h"
#include "webassets.h" // generiert von OGM-Common/scripts/pio/prepare.py aus web/assets/

namespace OpenKNX
{
    namespace Network
    {
        // groupmonitor.css/groupmonitor.js liegen in web/assets/ und werden dort
        // minifiziert + gzip-komprimiert in webassets.h eingebettet. Der HTML-
        // Seitenrumpf (gmPage) bleibt hier — kein separat abrufbares Asset.

        static const char gmPage[] =
            "<h1>Gruppenmonitor <span id='gm-status' class='gm-status gm-status-sm'>Verbinde&hellip;</span></h1>"
            "<div class='gm-bar'>"
            "<label><input type='checkbox' id='gm-scroll' checked> Autoscroll</label>"
#if MASK_VERSION != 0x091A
            // Not on a router: busmonitor mode takes the TP-UART out of normal
            // processing, which on 0x091A would stop TP<->IP routing, not just monitoring.
            "<label title='Im Busmonitor-Modus werden keine KNX-Telegramme mehr verarbeitet!'>"
            "<input type='checkbox' id='gm-busmon'>"
            " <span class='gm-busmon-warn'>&#x26A0; Busmonitor</span></label>"
#endif
            "<span class='gm-spacer'></span>"
            "<button type='button' id='gm-startstop'></button>"
            "<button type='button' id='gm-clear'>Leeren</button>"
            "</div>"
            "<div class='gm-tablewrap'>"
            "<table id='gm-table'>"
            "<thead><tr><th>Zeit</th><th>Flags</th><th>Quelle</th><th>Ziel</th>"
            "<th>Typ</th><th>Hauptgruppe</th><th>Mittelgruppe</th><th>Name</th><th>Value</th><th>DPT</th><th>Daten (hex)</th><th>Len</th></tr></thead>"
            "<tbody id='gm-body'></tbody>"
            "</table>"
            "</div>";

        // ── Setup ─────────────────────────────────────────────────────────────────

        void GroupMonitor::setup()
        {
            // Nur einmal registrieren – setup() kann bei Webserver-Neustart erneut
            // aufgerufen werden, der TP-Callback darf sich aber nicht aufstapeln.
            if (_initialized) return;
            _initialized = true;

            openknxNetwork.webserver.addMenuItem("Gruppenmonitor", "/groupmonitor", 110);

            openknxNetwork.webserver.addRoute(WEB_GET, "/assets/groupmonitor.css",
                                              Webserver::Asset(WebAssets::groupmonitor_css_mime, WebAssets::groupmonitor_css_gz, sizeof(WebAssets::groupmonitor_css_gz)));
            openknxNetwork.webserver.addRoute(WEB_GET, "/assets/groupmonitor.js",
                                              Webserver::Asset(WebAssets::groupmonitor_js_mime, WebAssets::groupmonitor_js_gz, sizeof(WebAssets::groupmonitor_js_gz)));
            openknxNetwork.webserver.addStylesheet("/assets/groupmonitor.css");
            openknxNetwork.webserver.addJavaScript("/assets/groupmonitor.js");

            openknxNetwork.webserver.addRoute(WEB_GET, "/groupmonitor",
                                              [](WebRequest&, WebResponse& res) {
                                                  res.setContentType("text/html");
                                                  res.setLayout(true);
                                                  res.sendStatic(gmPage);
                                              });

            openknxNetwork.webserver.addRoute(WEB_GET, "/groupmonitor/state",
                                              [](WebRequest&, WebResponse& res) {
                                                  res.setContentType("application/json");
                                                  bool mon = openknxNetwork.tpUart().isMonitoring();
                                                  res.send(mon ? "{\"monitoring\":true}" : "{\"monitoring\":false}");
                                              });

            openknxNetwork.webserver.addSocket("/groupmonitor",
                                               [](int, WebSocketFrame* frame) {
                                                   if (!frame || frame->length == 0) return;
#if MASK_VERSION != 0x091A
                                                   std::string cmd((const char*)frame->data, frame->length);
                                                   auto& dll = openknxNetwork.tpUart();
                                                   if (cmd == "busmonitor:on")
                                                       dll.startMonitoring();
                                                   else if (cmd == "busmonitor:off")
                                                       dll.reset();
#endif
                                               },
                                               nullptr);

            // Paralleler Tap am KNX-Stack vorbei: jedes vom TP-UART empfangene Frame
            // landet im onFrame()-Callback (loop-Kontext, gleicher Task wie der Webserver).
            openknxNetwork.tpUart().registerReceivedFrame(
                [](TPUart::Frame& f) { openknxNetwork.groupmonitor.onFrame(f); });
        }

        // ── Frame → JSON → Broadcast ────────────────────────────────────────────────

        void GroupMonitor::onFrame(TPUart::Frame& frame)
        {
            if (!frame.isFrame()) return;
            if (!openknxNetwork.webserver.hasClients("/groupmonitor")) return;

            // _json keeps its capacity across calls (reset() only clears content), so
            // building the message here does not allocate once warmed up.
            _json.reset();
            OpenKNX::Network::buildTelegramJson(frame, _json);

            openknxNetwork.webserver.sendWebsocketMessage("/groupmonitor", _json.c_str());
        }

    } // namespace Network
} // namespace OpenKNX

#endif // OPENKNX_TELEGRAMJSON && OPENKNX_WEBMONITOR
