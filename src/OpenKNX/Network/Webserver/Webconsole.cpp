#if defined(OPENKNX_WEBCONSOLE) && (defined(KNX_IP_WIFI) || defined(KNX_IP_LAN))

#include "OpenKNX/Network/Webserver/Webconsole.h"
#include "OpenKNX.h"
#include "OpenKNX/Network/Module.h"
#include "OpenKNX/Charset.hpp"
#include "OpenKNX/Network/Webserver/Webserver.h"
#include "webassets.h"

#include <cstring>

namespace OpenKNX
{
    namespace Network
    {

        void Webconsole::setup()
        {
            // setup() kann bei einem Webserver-Neustart erneut laufen; der UI-Handler
            // darf sich dabei nicht aufstapeln.
            if (_initialized) return;
            _initialized = true;

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

            // Für "log" wird nichts registriert -- reiner Sender.
            openknxNetwork.webserver.ui.receiveMessage([this](const char* key, const char* data, size_t len) {
                if (strcmp(key, "cmd") != 0) return;

                // Queue command for loop() -- avoids running processCommand() inside
                // a network callback (lwIP on RP2040, httpd task on ESP32).
                // Never overwrite an occupied buffer: loop() copies it, and on RP2040
                // this callback preempts that copy. Dropping the second command beats
                // splicing it into the first.
                if (_hasPendingCmd) return;

                for (size_t i = 0; i < len; i++)
                {
                    uint8_t c = (uint8_t)data[i];
                    if ((c < 0x20 || c > 0x7E) && c != '\r' && c != '\n' && c != '\t') return;
                }
                size_t n = (len < sizeof(_pendingCmd) - 1) ? len : sizeof(_pendingCmd) - 1;
                memcpy(_pendingCmd, data, n);
                _pendingCmd[n] = '\0';
                _hasPendingCmd = true;
            });
        }

        void Webconsole::loop()
        {
            // Execute queued console command from the safe loop-task context.
            if (_hasPendingCmd)
            {
                char cmd[sizeof(_pendingCmd)];
                memcpy(cmd, _pendingCmd, sizeof(cmd));
                // The flag releases the buffer, not a zeroed content: a plain store to
                // _pendingCmd may sink past the volatile flag and clip the next command.
                _hasPendingCmd = false;
                // Echo der Eingabe via Logger → Serial + alle Webconsole-Clients.
                // Auch ein leerer Befehl wird geloggt und erzeugt so eine Leerzeile.
                openknx.logger.log(cmd);
                if (cmd[0] != '\0')
                    openknx.console.submitLine(cmd); // divert to an active FTC console tunnel, else run locally (lets `ftc <pa> con` work from the web console)
            }

            uint32_t writePos = openknx.logger.ringWritePos();

            if (!openknxNetwork.webserver.ui.hasClients())
            {
                // Ohne Clients mitlaufen lassen, damit der nächste kein Backlog bekommt.
                _readPos = writePos;
                // Don't report loss from a session nobody watches to the next client.
                _lostBytes = 0;
                return;
            }

            // Measured in ISO-8859-15, sent as UTF-8: encodeUtf8 makes up to three bytes
            // per character, hence the /3 -- otherwise a line of umlauts never fits.
            const uint32_t keyLen = 4; // "log:"
            uint32_t limit = (uint32_t)openknxNetwork.webserver.ui.maxMessage();
            uint32_t maxLine = (limit > keyLen) ? (limit - keyLen) / 3 : 1;
            if (maxLine > 512) maxLine = 512;

            // Fallen behind: the oldest lines are already overwritten in the logger ring,
            // typically because one processCommand() outgrew it.
            if (writePos - _readPos > Log::Logger::RING_SIZE)
            {
                _lostBytes += (writePos - _readPos) - Log::Logger::RING_SIZE;
                _readPos = writePos - Log::Logger::RING_SIZE;
            }

            Webserver& ws = openknxNetwork.webserver;

            // Report it instead of skipping silently. Outside the per-tick share, but like
            // every line only if it fits; the ANSI escape colours it yellow in the browser.
            if (_lostBytes && (millis() - _lostReported) >= 1000)
            {
                _lostReported = millis();
                char note[96];
                int n = snprintf(note, sizeof(note),
                                 "\x1B[33m[... %u Bytes Log verworfen, Puffer zu klein ...]\x1B[0m\n",
                                 (unsigned)_lostBytes);
                if (n > 0 && ws.txFits("log", (size_t)n))
                {
                    ws.ui.sendMessage("log", note, (size_t)n);
                    _lostBytes = 0; // only after enqueueing, or the report itself is a
                                    // silent loss path
                }
            }

            // Each line must fit without evicting; only the first may exceed the per-tick quarter.
            if (!ws.txFits("log", 0)) return; // full: don't build lines just to discard them
            uint32_t budget = (uint32_t)ws.txCapacity() / 4;
            bool firstLine = true;

            while (_readPos != writePos)
            {
                // Send only complete lines so the client-side ANSI parser never has
                // to carry state across messages. Fall back to a fixed chunk if no
                // newline within maxLine bytes (e.g. very long hex-dump lines).
                uint32_t pos = _readPos;
                const uint32_t scanEnd = (writePos - _readPos < maxLine) ? writePos : _readPos + maxLine;
                bool foundNewline = false;
                while (pos != scanEnd)
                {
                    if (openknx.logger.ringBuf()[pos % Log::Logger::RING_SIZE] == '\n')
                    {
                        foundNewline = true;
                        pos++;
                        break;
                    }
                    pos++;
                }
                if (!foundNewline && writePos - _readPos < maxLine) break;

                // Ring buffer is ISO-8859-15; WS text frame needs UTF-8.
                std::string raw;
                raw.reserve(pos - _readPos);
                for (uint32_t i = _readPos; i != pos; i++)
                    raw += openknx.logger.ringBuf()[i % Log::Logger::RING_SIZE];
                std::string line;
                line.reserve(raw.size());
                OpenKNX::Charset::encodeUtf8(line, raw.data(), raw.size());

                if (!ws.txFits("log", line.size())) break;
                const uint32_t cost = (uint32_t)ws.txCost("log", line.size());
                if (cost > budget && !firstLine) break;
                budget = (budget > cost) ? budget - cost : 0;
                firstLine = false;

                // The send queue holds the line now, so _readPos advances
                // unconditionally -- no retry, no per-client bookkeeping.
                ws.ui.sendMessage("log", line.data(), line.size());
                _readPos = pos;
            }
        }

    } // namespace Network
} // namespace OpenKNX

#endif // defined(OPENKNX_WEBCONSOLE) && (defined(KNX_IP_WIFI) || defined(KNX_IP_LAN))
