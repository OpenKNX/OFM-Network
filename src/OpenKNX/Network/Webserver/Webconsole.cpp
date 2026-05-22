#if defined(OPENKNX_WEBCONSOLE) && (defined(KNX_IP_WIFI) || defined(KNX_IP_LAN))

#include "OpenKNX/Network/Webserver/Webconsole.h"
#include "OpenKNX.h"
#include "OpenKNX/Network/Module.h"
#include "OpenKNX/Network/Webserver/Webserver.h"

#include <algorithm>

namespace OpenKNX
{
    namespace Network
    {

        void Webconsole::setup()
        {
            openknxNetwork.webserver.addMenuItem("Console", "/console");

            openknxNetwork.webserver.addRoute(WEB_GET, "/console",
                                              [](WebRequest&, WebResponse& res) {
                                                  static const char content[] =
                                                      "<style>"
                                                      "html,body{height:100%;overflow:hidden}"
                                                      "main{display:flex;flex-direction:column;height:100vh;padding:1em;gap:.5em}"
                                                      "#console-wrap{flex:1;display:flex;flex-direction:column;min-height:0;gap:.5em}"
                                                      "#console-out{flex:1;background:#0d0d0d;color:#fff;font-family:monospace;"
                                                      "font-size:1em;padding:1em;border-radius:4px 4px 0 0;overflow-y:auto;"
                                                      "white-space:pre-wrap;border:1px solid #333}"
                                                      "#console-inp-row{display:flex;gap:.5em;flex:none}"
                                                      "#console-inp{flex:1;background:#0d0d0d;color:#fff;font-family:monospace;"
                                                      "font-size:1em;padding:.5em .75em;border:1px solid #333;border-top:none;"
                                                      "border-radius:0 0 0 4px;outline:none}"
                                                      "#console-inp::placeholder{color:#555}"
                                                      "#console-send{padding:.5em 1.2em;background:#449841;color:#fff;border:none;"
                                                      "border-radius:0 0 4px 0;cursor:pointer;font-size:1em}"
                                                      "#console-send:hover{background:#357a31}"
                                                      "</style>"
                                                      "<div id='console-wrap'>"
                                                      "<div id='console-out'>Verbinde...</div>"
                                                      "<div id='console-inp-row'>"
                                                      "<input id='console-inp' type='text' placeholder='Befehl eingeben...' autocomplete='off'>"
                                                      "<button id='console-send' type='button'>Senden</button>"
                                                      "</div>"
                                                      "</div>"
                                                      "<script>"
                                                      "const out=document.getElementById('console-out');"
                                                      "const inp=document.getElementById('console-inp');"
                                                      "const btn=document.getElementById('console-send');"
                                                      "const MAX_LINES=500;"
                                                      "function appendOut(html){"
                                                      "out.insertAdjacentHTML('beforeend',html);"
                                                      "while(out.childNodes.length>MAX_LINES)out.removeChild(out.firstChild);"
                                                      "out.scrollTop=out.scrollHeight;"
                                                      "}"
                                                      "let ws;"
                                                      "function connect(){"
                                                      "ws=new WebSocket('ws://'+location.host+'/console');"
                                                      "ws.onopen=()=>{appendOut('<span class=\"green\">[verbunden]\\n</span>');};"
                                                      "ws.onmessage=e=>{appendOut(e.data);};"
                                                      "ws.onclose=()=>{appendOut('<span class=\"red\">[Verbindung getrennt &mdash; Seite neu laden]\\n</span>');};"
                                                      "}"
                                                      "connect();"
                                                      "function send(){"
                                                      "const v=inp.value.trim();inp.value='';"
                                                      "if(ws&&ws.readyState===1)ws.send(v);"
                                                      "}"
                                                      "btn.onclick=send;"
                                                      "inp.addEventListener('keydown',e=>{if(e.key==='Enter'){e.preventDefault();send();}});"
                                                      "</script>";

                                                  res.setContentType("text/html");
                                                  res.setLayout(true);
                                                  res.sendStatic(content);
                                              });

            openknxNetwork.webserver.addSocket("/console", [this](int clientId, WebSocketFrame* f) {
                    // Queue command for loop() — avoids running processCommand() inside
                    // a network callback (lwIP on RP2040, httpd task on ESP32).
                    if (f->length > 0)
                    {
                        bool valid = true;
                        for (int i = 0; i < f->length; i++)
                        {
                            uint8_t c = f->data[i];
                            if ((c < 0x20 || c > 0x7E) && c != '\r' && c != '\n' && c != '\t')
                            {
                                valid = false;
                                break;
                            }
                        }
                        if (valid)
                        {
                            size_t len = (f->length < (int)sizeof(_pendingCmd) - 1)
                                             ? (size_t)f->length
                                             : sizeof(_pendingCmd) - 1;
                            memcpy(_pendingCmd, f->data, len);
                            _pendingCmd[len] = '\0';
                        }
                    } }, nullptr);
        }

        void Webconsole::loop()
        {
            // Execute queued console command from the safe loop-task context.
            if (_pendingCmd[0] != '\0')
            {
                char cmd[sizeof(_pendingCmd)];
                memcpy(cmd, _pendingCmd, sizeof(cmd));
                _pendingCmd[0] = '\0';
                openknx.logger.log(cmd);
                openknx.console.processCommand(cmd);
            }

            std::vector<int> clients = openknxNetwork.webserver.connectedClientFds("/console");
            if (clients.empty()) return;

            uint32_t writePos = openknx.logger.ringWritePos();

            // Remove stale entries for disconnected clients
            for (auto it = _consoleReadPos.begin(); it != _consoleReadPos.end();)
            {
                if (std::find(clients.begin(), clients.end(), it->first) == clients.end())
                    it = _consoleReadPos.erase(it);
                else
                    ++it;
            }

            for (int fd : clients)
            {
                // New client: start from current write position (no backlog)
                if (_consoleReadPos.find(fd) == _consoleReadPos.end())
                    _consoleReadPos[fd] = writePos;

                uint32_t& readPos = _consoleReadPos[fd];

                // Snap to oldest valid data if client fell too far behind
                if (writePos - readPos > Log::Logger::RING_SIZE)
                    readPos = writePos - Log::Logger::RING_SIZE;

                while (readPos < writePos)
                {
                    // Send only complete lines to avoid splitting ANSI sequences
                    // across ansiToHtml() calls. Fall back to a fixed chunk if no
                    // newline within MAX_LINE bytes (e.g. very long hex-dump lines).
                    static constexpr uint32_t MAX_LINE = 512;
                    uint32_t pos = readPos;
                    uint32_t limit = (writePos - readPos < MAX_LINE) ? writePos : readPos + MAX_LINE;
                    bool foundNewline = false;
                    while (pos < limit)
                    {
                        if (openknx.logger.ringBuf()[pos % Log::Logger::RING_SIZE] == '\n')
                        {
                            foundNewline = true;
                            pos++;
                            break;
                        }
                        pos++;
                    }
                    if (!foundNewline && writePos - readPos < MAX_LINE) break;

                    std::string line;
                    line.reserve(pos - readPos);
                    for (uint32_t i = readPos; i < pos; i++)
                        line += openknx.logger.ringBuf()[i % Log::Logger::RING_SIZE];

                    std::string html = ansiToHtml(line);
                    if (!openknxNetwork.webserver.sendToClient("/console", fd, html.c_str(), html.size())) break;
                    readPos = pos;
                }
            }
        }

        std::string Webconsole::ansiToHtml(const std::string& s)
        {
            static const struct
            {
                int code;
                const char* cls;
            } colorMap[] = {
                {31, "red"}, {32, "green"}, {33, "yellow"}, {90, "gray"}};

            std::string out;
            bool spanOpen = false;
            size_t i = 0;
            while (i < s.size())
            {
                if (s[i] == '\x1B' && i + 1 < s.size() && s[i + 1] == '[')
                {
                    size_t j = i + 2;
                    while (j < s.size() && s[j] != 'm')
                        j++;
                    if (j < s.size())
                    {
                        int code = 0;
                        for (size_t k = i + 2; k < j; k++)
                            code = code * 10 + (s[k] - '0');
                        if (spanOpen)
                        {
                            out += "</span>";
                            spanOpen = false;
                        }
                        if (code != 0)
                        {
                            for (auto& e : colorMap)
                            {
                                if (e.code == code)
                                {
                                    out += "<span class='";
                                    out += e.cls;
                                    out += "'>";
                                    spanOpen = true;
                                    break;
                                }
                            }
                        }
                        i = j + 1;
                        continue;
                    }
                }
                char c = s[i];
                if (c == '<') out += "&lt;";
                else if (c == '>')
                    out += "&gt;";
                else if (c == '&')
                    out += "&amp;";
                else
                    out += c;
                i++;
            }
            if (spanOpen) out += "</span>";
            return out;
        }

    } // namespace Network
} // namespace OpenKNX

#endif // defined(OPENKNX_WEBCONSOLE) && (defined(KNX_IP_WIFI) || defined(KNX_IP_LAN))
