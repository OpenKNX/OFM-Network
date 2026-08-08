#if defined(OPENKNX_WEBMONITOR) && (MASK_VERSION == 0x07B0) && (defined(KNX_IP_WIFI) || defined(KNX_IP_LAN))

#include "OpenKNX/Network/Webserver/GroupMonitor.h"
#include "OpenKNX.h"
#include "OpenKNX/Network/Module.h"
#include "OpenKNX/Charset.hpp"
#include "OpenKNX/Network/Webserver/Webserver.h"
#include "webassets.h" // generiert von OGM-Common/scripts/pio/prepare.py aus web/assets/

#include <knx/dptconvert.h> // KNX_Decode_Value, Dpt, KNXValue (bereits gelinkt)

#include <cstdio>
#include <cstring>
#include <string>

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
            "<label title='Im Busmonitor-Modus werden keine KNX-Telegramme mehr verarbeitet!'>"
            "<input type='checkbox' id='gm-busmon'>"
            " <span class='gm-busmon-warn'>&#x26A0; Busmonitor</span></label>"
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
                                                  bool mon = knx.bau().getDataLinkLayer()->getTPUart().isMonitoring();
                                                  res.send(mon ? "{\"monitoring\":true}" : "{\"monitoring\":false}");
                                              });

            openknxNetwork.webserver.addSocket("/groupmonitor",
                                               [](int, WebSocketFrame* frame) {
                                                   if (!frame || frame->length == 0) return;
                                                   std::string cmd((const char*)frame->data, frame->length);
                                                   auto& dll = knx.bau().getDataLinkLayer()->getTPUart();
                                                   if (cmd == "busmonitor:on")
                                                       dll.startMonitoring();
                                                   else if (cmd == "busmonitor:off")
                                                       dll.reset();
                                               },
                                               nullptr);

            // Paralleler Tap am KNX-Stack vorbei: jedes vom TP-UART empfangene Frame
            // landet im onFrame()-Callback (loop-Kontext, gleicher Task wie der Webserver).
            knx.bau().getDataLinkLayer()->getTPUart().registerReceivedFrame(
                [](TPUart::Frame& f) { openknxNetwork.groupmonitor.onFrame(f); });
        }

        // ── Wert-Dekodierung (knx-Stack) ────────────────────────────────────────────

        // Hängt s JSON-escaped an json an (nur " \ und Steuerzeichen relevant).
        static void appendJsonEscaped(std::string& json, const char* s)
        {
            for (const char* p = s; *p; ++p)
            {
                const char c = *p;
                if (c == '"' || c == '\\')
                {
                    json += '\\';
                    json += c;
                }
                else if ((uint8_t)c >= 0x20)
                    json += c;
                // Steuerzeichen (< 0x20) werden verworfen
            }
        }

        // Dekodiert die Telegramm-Payload anhand des DPT über den knx-Stack in einen
        // lesbaren Wert (Stack-Buffer, keine Heap-Strings). out bleibt leer bei
        // unbekanntem/nicht unterstütztem DPT → Browser zeigt dann die Rohbytes.
        static void decodeValue(uint8_t dpt, uint8_t sub, const char* d, uint16_t meta,
                                uint8_t len, uint16_t total, char* out, size_t outLen)
        {
            out[0] = '\0';

            uint8_t buf[16] = {};
            size_t vlen;
            if (len <= 1)
            {
                // ≤6-bit-Wert steckt im APCI-Low-Oktett (DPT 1/2/3)
                if (meta < 1 || meta > total) return;
                buf[0] = (uint8_t)d[meta - 1] & 0x3F;
                vlen = 1;
            }
            else
            {
                // apduSize() kommt aus dem Längenfeld und kann bei verstümmelten Frames
                // grösser sein als der Puffer.
                vlen = (size_t)(len - 1);
                if (vlen > sizeof(buf)) vlen = sizeof(buf);
                if (meta >= total) return;
                const size_t avail = (size_t)(total - meta);
                if (vlen > avail) vlen = avail;
                if (vlen == 0) return;
                for (size_t i = 0; i < vlen; i++)
                    buf[i] = (uint8_t)d[meta + i];
            }

            KNXValue v("");
            Dpt type(dpt, sub);
            if (!KNX_Decode_Value(buf, vlen, type, v)) return;

            switch (dpt)
            {
                case 1: snprintf(out, outLen, "%u", (bool)v ? 1u : 0u); break;
                case 5:
                    if (sub == 1) // 5.001: Stack liefert roh 0–255 → Prozent
                        snprintf(out, outLen, "%u%%", (unsigned)(((uint16_t)(uint8_t)v * 100 + 127) / 255));
                    else
                        snprintf(out, outLen, "%u", (unsigned)(uint8_t)v);
                    break;
                case 6: snprintf(out, outLen, "%d", (int)(int8_t)v); break;
                case 7: snprintf(out, outLen, "%u", (unsigned)(uint16_t)v); break;
                case 8: snprintf(out, outLen, "%d", (int)(int16_t)v); break;
                case 9:
                case 14: snprintf(out, outLen, "%.2f", (double)v); break;
                case 12: snprintf(out, outLen, "%lu", (unsigned long)(uint32_t)v); break;
                case 13: snprintf(out, outLen, "%ld", (long)(int32_t)v); break;
                case 16: snprintf(out, outLen, "%s", (const char*)v); break;
                case 17:
                case 18: snprintf(out, outLen, "%u", (unsigned)(uint8_t)v); break;
                default: out[0] = '\0'; break;
            }
        }

        // ── Frame → JSON → Broadcast ────────────────────────────────────────────────

        void GroupMonitor::onFrame(TPUart::Frame& frame)
        {
            if (!frame.isFrame()) return;
            if (!openknxNetwork.webserver.hasClients("/groupmonitor")) return;

            const char* d = frame.data();
            const uint16_t total = frame.size();
            const uint8_t meta = frame.metadataSize(); // 8 (Standard) / 9 (Extended), inkl. CRC
            const bool ga = frame.isGroupAddress();
            const uint8_t len = frame.apduSize();      // Anzahl Payload-Bytes ab meta-1

            // APCI-Typ nur für Gruppentelegramme sinnvoll.
            // ASCII placeholder, not an em dash -- avoids clashing with val's Latin-15->UTF-8 conversion below.
            const char* apci = "-";
            bool hasValue = false;             // Write/Response tragen einen Wert
            if (ga && total >= meta)
            {
                const uint16_t a = (((uint8_t)d[meta - 2] & 0x03) << 8) | (uint8_t)d[meta - 1];
                switch (a & 0x03C0)
                {
                    case 0x000: apci = "Read"; break;
                    case 0x040: apci = "Response"; hasValue = true; break;
                    case 0x080: apci = "Write"; hasValue = true; break;
                    default: apci = "other"; break;
                }
            }

            // Wert über DPT (aus GA-Tabelle) dekodieren – nur Stack-Buffer, keine Heap-Strings.
            const uint16_t addr = frame.destination();
            char dptStr[12] = {};
            char val[40] = {};
            if (ga && hasValue)
            {
                uint8_t dptMain, dptSub;
                if (openknxNetwork.gatable.getDpt(addr, dptMain, dptSub) && dptSub > 0)
                {
                    snprintf(dptStr, sizeof(dptStr), "%u.%03u", dptMain, dptSub);
                    decodeValue(dptMain, dptSub, d, meta, len, total, val, sizeof(val));
                }
            }

            // Payload-Bytes (len Bytes ab meta-1) als Hex.
            std::string hex;
            hex.reserve(len * 2);
            const uint16_t start = (meta >= 1) ? (meta - 1) : 0;
            for (uint16_t i = 0; i < len && (start + i) < total; i++)
            {
                char b[3];
                snprintf(b, sizeof(b), "%02X", (uint8_t)d[start + i]);
                hex += b;
            }

            std::string json = "{\"src\":\"";
            json += frame.humanSource();
            json += "\",\"dst\":\"";
            json += frame.humanDestination();
            json += "\",\"ga\":";
            json += ga ? "1" : "0";
            json += ",\"apci\":\"";
            json += apci;
            json += "\",\"len\":";
            json += std::to_string(len);
            char flags[10];
            flags[0] = frame.isTransmitted() ? 'T' : '_';
            flags[1] = frame.isAddressed()   ? 'D' : '_';
            flags[2] = frame.isInvalid()     ? 'I' : '_';
            flags[3] = frame.isExtended()    ? 'E' : '_';
            flags[4] = frame.isRepeated()    ? 'R' : '_';
            flags[5] = frame.isFiltered()    ? 'F' : '_';
            flags[6] = frame.isBusy()        ? 'B' : '_';
            flags[7] = frame.isNack()        ? 'N' : '_';
            flags[8] = frame.isAck()         ? 'A' : '_';
            flags[9] = '\0';

            if (ga)
            {
                json += ",\"addr\":";
                json += std::to_string(addr); // numerische GA für Namens-Lookup im Browser
            }
            json += ",\"dpt\":\"";
            json += dptStr;
            json += "\",\"val\":\"";
            {
                // val is raw DPT16 bus data (ISO-8859-15); WS text frame needs UTF-8.
                std::string valUtf8;
                OpenKNX::Charset::encodeUtf8(valUtf8, val, strlen(val));
                appendJsonEscaped(json, valUtf8.c_str());
            }
            json += "\",\"hex\":\"";
            json += hex;
            json += "\",\"flags\":\"";
            json += flags;
            json += "\"}";

            openknxNetwork.webserver.sendWebsocketMessage("/groupmonitor", json.c_str());
        }

    } // namespace Network
} // namespace OpenKNX

#endif // OPENKNX_WEBMONITOR && MASK_VERSION == 0x07B0 && (KNX_IP_WIFI || KNX_IP_LAN)
