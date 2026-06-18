#if defined(OPENKNX_WEBMONITOR) && (MASK_VERSION == 0x07B0) && (defined(KNX_IP_WIFI) || defined(KNX_IP_LAN))

#include "OpenKNX/Network/Webserver/GroupMonitor.h"
#include "OpenKNX.h"
#include "OpenKNX/Network/Module.h"
#include "OpenKNX/Network/Webserver/Webserver.h"

#include <knx/dptconvert.h> // KNX_Decode_Value, Dpt, KNXValue (bereits gelinkt)

#include <cstdio>
#include <string>

namespace OpenKNX
{
    namespace Network
    {
        // ── Assets (eigene Dateien, kein Inline-CSS/JS in der Seite) ──────────────

        static const char gmCss[] =
            "html,body{height:100%;overflow:hidden}"
            "main{display:flex;flex-direction:column;height:100vh;padding:1em;gap:.5em}"
            ".gm-bar{display:flex;align-items:center;gap:1em;flex:none}"
            ".gm-bar label{display:flex;align-items:center;gap:.3em;cursor:pointer}"
            ".gm-spacer{flex:1}"
            ".gm-status{font-weight:bold}"
            ".gm-status-sm{font-size:.55em;font-weight:normal;vertical-align:middle}"
            ".gm-status.ok{color:#449841}"
            ".gm-status.err{color:#c0392b}"
            "#gm-clear{padding:.4em 1em;background:#888;color:#fff;border:none;"
            "border-radius:4px;cursor:pointer}"
            "#gm-clear:hover{background:#666}"
            // Heller Scroll-Container; die Tabelle selbst erbt das Standard-Styling aus base.css
            ".gm-tablewrap{flex:1;overflow-y:auto;border:1px solid #ddd;border-radius:4px}"
            "#gm-table{margin-bottom:0;font-family:monospace;font-size:1em;width:100%}"
            "#gm-table thead th{position:sticky;top:0}"
            "#gm-table td.gm-data{word-break:break-all}"
            ".gm-w{color:#1565c0}.gm-r{color:#b8860b}.gm-rsp{color:#2e7d32}"
            ".gm-flag{color:#888;font-size:.85em}"
            ".gm-sysrow td{border-top:1px solid #ddd;font-family:inherit;font-size:inherit}"
            ".gm-sysmsg{color:#555}"
            ".gm-sys-ok{color:#357a31}"
            ".gm-sys-err{color:#c0392b}"
            "#gm-startstop{padding:.4em 1em;border:none;border-radius:4px;cursor:pointer;color:#fff}"
            "#gm-startstop.running{background:#c0392b}"
            "#gm-startstop.running:hover{background:#a93226}"
            "#gm-startstop.stopped{background:#449841}"
            "#gm-startstop.stopped:hover{background:#357a31;}"
            ".gm-busmon-warn{color:#b8860b;font-size:.85em;font-weight:bold}"
            "input[type=checkbox]:disabled+span{color:#aaa}";

        static const char gmJs[] =
            "(function(){"
            "const body=document.getElementById('gm-body');"
            "const st=document.getElementById('gm-status');"
            "const cbScroll=document.getElementById('gm-scroll');"
            "const cbBusmon=document.getElementById('gm-busmon');"
            "const btnSS=document.getElementById('gm-startstop');"
            "const wrap=document.querySelector('.gm-tablewrap');"
            "const MAX_ROWS=1000;"
            "let running=true;"
            // GA-Namen client-seitig aus der TSV (Backend hält keine Namen vor)
            "let gaNames=null;"
            "fetch('/filemanager/download?path=%2Fopenknx_ga.tsv')"
            ".then(r=>r.ok?r.text():Promise.reject())"
            ".then(t=>{gaNames=new Map();"
            "t.split('\\n').slice(1).forEach(l=>{const c=l.split('\\t');"
            "if(c.length>=6){const a=parseInt(c[0],10);if(!isNaN(a))gaNames.set(a,{hg:c[3].trim(),mg:c[4].trim(),nm:c[5].trim()});}});})"
            ".catch(()=>{});"
            // Start/Stop toggle
            "function updateBtn(){"
            "if(running){btnSS.textContent='Stop';btnSS.className='running';}"
            "else{btnSS.textContent='Start';btnSS.className='stopped';}"
            "}"
            "btnSS.onclick=()=>{"
            "running=!running;updateBtn();"
            "statusRow(running?'Aufzeichnung gestartet':'Aufzeichnung gestoppt',running?'gm-sys-ok':'gm-sys-err');"
            "};"
            "updateBtn();"
            "document.getElementById('gm-clear').onclick=()=>{body.innerHTML='';};"
            // Busmonitor checkbox
            "cbBusmon.onchange=function(){"
            "if(this.checked){"
            "if(!confirm('Im Busmonitor-Modus werden keine KNX-Telegramme mehr verarbeitet und das Ger\\u00E4t kann nicht mehr am Bus teilnehmen.')){"
            "this.checked=false;return;"
            "}"
            "if(ws&&ws.readyState===1)ws.send('busmonitor:on');"
            "statusRow('Busmonitor-Modus aktiviert &mdash; Telegramme werden nicht mehr verarbeitet','gm-sys-err');"
            "}else{"
            "if(ws&&ws.readyState===1)ws.send('busmonitor:off');"
            "statusRow('Busmonitor-Modus deaktiviert (TP-Reset)','gm-sys-ok');"
            "}"
            "};"
            // Helper
            "function p(n){return(n<10?'0':'')+n;}"
            "function ts(){const d=new Date();return p(d.getHours())+':'+p(d.getMinutes())+':'"
            "+p(d.getSeconds())+'.'+String(d.getMilliseconds()).padStart(3,'0');}"
            "const TC={Write:'gm-w',Read:'gm-r',Response:'gm-rsp'};"
            "function esc(s){return String(s).replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));}"
            "function addRow(m){"
            "if(!running)return;"
            "const tr=document.createElement('tr');"
            "const ga=(gaNames&&m.addr!=null)?gaNames.get(m.addr):null;"
            "tr.innerHTML='<td>'+ts()+'</td><td class=\"gm-flag\">'+(m.flags||'')+'</td>'"
            "+'<td>'+m.src+'</td><td>'+m.dst+'</td>'"
            "+'<td class=\"'+(TC[m.apci]||'')+'\">'+m.apci+'</td>'"
            "+'<td>'+(ga?esc(ga.hg):'-')+'</td>'"
            "+'<td>'+(ga?esc(ga.mg):'-')+'</td>'"
            "+'<td>'+(ga?esc(ga.nm):'-')+'</td>'"
            "+'<td>'+(m.val?esc(m.val):'-')+'</td>'"
            "+'<td>'+(m.dpt||'-')+'</td>'"
            "+'<td class=\"gm-data\">'+(m.hex||'')+'</td>'"
            "+'<td>'+m.len+'</td>';"
            "body.appendChild(tr);"
            "while(body.childNodes.length>MAX_ROWS)body.removeChild(body.firstChild);"
            "if(cbScroll.checked)wrap.scrollTop=wrap.scrollHeight;"
            "}"
            "let ws,reconnTimer;"
            "function statusRow(msg,cls){"
            "const tr=document.createElement('tr');"
            "tr.className='gm-sysrow';"
            "tr.innerHTML='<td>'+ts()+'</td><td colspan=\"11\" class=\"gm-sysmsg '+(cls||'')+'\">'+msg+'</td>';"
            "body.appendChild(tr);"
            "if(cbScroll.checked)wrap.scrollTop=wrap.scrollHeight;"
            "}"
            "function connect(){"
            "ws=new WebSocket('ws://'+location.host+'/groupmonitor');"
            "ws.onopen=function(){"
            "st.textContent='Verbunden';st.className='gm-status ok';reconnDelay=2000;"
            "statusRow('Verbunden','gm-sys-ok');"
            // Query busmonitor state after connect
            "fetch('/groupmonitor/state').then(r=>r.json()).then(s=>{"
            "cbBusmon.checked=!!s.monitoring;"
            "if(s.monitoring)statusRow('Busmonitor-Modus ist aktiv &mdash; Telegramme werden nicht verarbeitet','gm-sys-err');"
            "}).catch(()=>{});"
            "};"
            "ws.onmessage=e=>{try{addRow(JSON.parse(e.data));}catch(err){}};"
            "ws.onclose=()=>{st.textContent='Getrennt — verbinde neu…';st.className='gm-status err';"
            "statusRow('Verbindung getrennt — verbinde neu…','gm-sys-err');"
            "clearTimeout(reconnTimer);reconnTimer=setTimeout(connect,reconnDelay);"
            "reconnDelay=Math.min(reconnDelay*2,30000);};"
            "}"
            "let reconnDelay=2000;"
            "connect();"
            "})();";

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
                                              Webserver::Static("text/css", gmCss));
            openknxNetwork.webserver.addRoute(WEB_GET, "/assets/groupmonitor.js",
                                              Webserver::Static("application/javascript", gmJs));
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
                                uint8_t len, char* out, size_t outLen)
        {
            out[0] = '\0';

            uint8_t buf[16] = {};
            size_t vlen;
            if (len <= 1)
            {
                // ≤6-bit-Wert steckt im APCI-Low-Oktett (DPT 1/2/3)
                buf[0] = (uint8_t)d[meta - 1] & 0x3F;
                vlen = 1;
            }
            else
            {
                vlen = (size_t)(len - 1);
                if (vlen > sizeof(buf)) vlen = sizeof(buf);
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
            if (openknxNetwork.webserver.connectedClientFds("/groupmonitor").empty()) return;

            const char* d = frame.data();
            const uint16_t total = frame.size();
            const uint8_t meta = frame.metadataSize(); // 8 (Standard) / 9 (Extended), inkl. CRC
            const bool ga = frame.isGroupAddress();
            const uint8_t len = frame.apduSize();      // Anzahl Payload-Bytes ab meta-1

            // APCI-Typ nur für Gruppentelegramme sinnvoll.
            const char* apci = "\xE2\x80\x94"; // em dash
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
                    decodeValue(dptMain, dptSub, d, meta, len, val, sizeof(val));
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
            appendJsonEscaped(json, val);
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
