#if defined(OPENKNX_WEBCONSOLE) && (defined(KNX_IP_WIFI) || defined(KNX_IP_LAN))

#include "OpenKNX/Network/Webserver/Webconsole.h"
#include "OpenKNX.h"
#include "OpenKNX/Network/Module.h"
#include "OpenKNX/Charset.hpp"
#include "OpenKNX/Network/Webserver/Webserver.h"
#include "webassets.h"

#include <algorithm>

namespace OpenKNX
{
    namespace Network
    {

        void Webconsole::setup()
        {
            openknxNetwork.webserver.addMenuItem("Gerätekonsole", "/console", 100);

            openknxNetwork.webserver.addRoute(WEB_GET, "/assets/console.css",
                                              Webserver::Asset(WebAssets::console_css_mime, WebAssets::console_css_gz, sizeof(WebAssets::console_css_gz)));
            openknxNetwork.webserver.addRoute(WEB_GET, "/assets/console.js",
                                              Webserver::Asset(WebAssets::console_js_mime, WebAssets::console_js_gz, sizeof(WebAssets::console_js_gz)));
            openknxNetwork.webserver.addStylesheet("/assets/console.css");
            openknxNetwork.webserver.addJavaScript("/assets/console.js");

            openknxNetwork.webserver.addRoute(WEB_GET, "/console",
                                              [](WebRequest&, WebResponse& res) {
                                                  static const char content[] =
                                                      "<div id='console-wrap'>"
                                                      "<div id='console-out'>Verbinde...</div>"
                                                      "<div id='console-inp-row'>"
                                                      "<input id='console-inp' type='text' placeholder='Befehl eingeben...' autocomplete='off'>"
                                                      "<button id='console-send' type='button'>Senden</button>"
                                                      "</div>"
                                                      "</div>";

                                                  res.setContentType("text/html");
                                                  res.setLayout(true);
                                                  res.sendStatic(content);
                                              });

            openknxNetwork.webserver.addSocket("/console", [this](int clientId, WebSocketFrame* f) {
                    // Queue command for loop() — avoids running processCommand() inside
                    // a network callback (lwIP on RP2040, httpd task on ESP32).
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
                        _hasPendingCmd = true;
                    } },
                [this](int clientId, bool connected) {
                    // sonst erbt ein neuer Client mit gleichem fd/Slot die alte Leseposition
                    if (!connected) _consoleReadPos.erase(clientId);
                });
        }

        void Webconsole::loop()
        {
            // Execute queued console command from the safe loop-task context.
            if (_hasPendingCmd)
            {
                char cmd[sizeof(_pendingCmd)];
                memcpy(cmd, _pendingCmd, sizeof(cmd));
                _pendingCmd[0] = '\0';
                _hasPendingCmd = false;
                // Echo der Eingabe via Logger → Serial + alle Webconsole-Clients.
                // Auch ein leerer Befehl wird geloggt und erzeugt so eine Leerzeile.
                openknx.logger.log(cmd);
                if (cmd[0] != '\0')
                    openknx.console.submitLine(cmd); // divert to an active FTC console tunnel, else run locally (lets `ftc <pa> con` work from the web console)
            }

            if (!openknxNetwork.webserver.hasClients("/console")) return;
            std::vector<int> clients = openknxNetwork.webserver.connectedClientFds("/console");

            uint32_t writePos = openknx.logger.ringWritePos();

            // Eine Payload, die nicht in einen WS-Frame passt, wäre dauerhaft unsendbar
            // und würde die Retry-Schleife unten endlos blockieren.
            uint32_t maxLine = 512;
            const size_t maxPayload = openknxNetwork.webserver.maxWebsocketPayload();
            if (maxPayload < maxLine) maxLine = (uint32_t)maxPayload;

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
                    // Send only complete lines so the client-side ANSI parser never has
                    // to carry state across messages. Fall back to a fixed chunk if no
                    // newline within maxLine bytes (e.g. very long hex-dump lines).
                    uint32_t pos = readPos;
                    uint32_t limit = (writePos - readPos < maxLine) ? writePos : readPos + maxLine;
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
                    if (!foundNewline && writePos - readPos < maxLine) break;

                    // Ring buffer is ISO-8859-15; WS text frame needs UTF-8.
                    std::string raw;
                    raw.reserve(pos - readPos);
                    for (uint32_t i = readPos; i < pos; i++)
                        raw += openknx.logger.ringBuf()[i % Log::Logger::RING_SIZE];
                    std::string line;
                    line.reserve(raw.size());
                    OpenKNX::Charset::encodeUtf8(line, raw.data(), raw.size());

                    if (!openknxNetwork.webserver.sendToClient("/console", fd, line.c_str(), line.size())) break;
                    readPos = pos;
                }
            }
        }

    } // namespace Network
} // namespace OpenKNX

#endif // defined(OPENKNX_WEBCONSOLE) && (defined(KNX_IP_WIFI) || defined(KNX_IP_LAN))
