#if defined(OPENKNX_WEBCONSOLE) && (defined(KNX_IP_WIFI) || defined(KNX_IP_LAN))

#include "OpenKNX/Network/Webserver/Webconsole.h"
#include "OpenKNX.h"
#include "OpenKNX/Network/Module.h"
#include "OpenKNX/Charset.hpp"
#include "OpenKNX/Network/Webserver/Webserver.h"
#include "webassets.h"

#include <cstdint>
#include <cstring>
#ifdef ARDUINO_ARCH_ESP32
#include <esp_heap_caps.h>
#endif

namespace OpenKNX
{
    namespace Network
    {
        namespace
        {
            constexpr size_t kHeapFloor = 1024; // left free beside a line incl. its block header: operator new on an empty heap ends in a restart

            /** @brief Largest block a line may take now: ESP32 the largest free block, RP2040 the free heap (the core reports no largest block). */
            size_t heapForLine()
            {
#if defined(ARDUINO_ARCH_ESP32)
                return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
#elif defined(ARDUINO_ARCH_RP2040)
                if (Webserver::heapShort()) return 0; // the free heap may be split into blocks smaller than one line
                const int heap = rp2040.getFreeHeap();
                return heap > 0 ? (size_t)heap : 0;
#else
                return SIZE_MAX;
#endif
            }
        } // namespace

        void Webconsole::setup()
        {
            // setup() kann bei einem Webserver-Neustart erneut laufen; der UI-Handler
            // darf sich dabei nicht aufstapeln.
            if (_initialized) return;
            _initialized = true;

            openknxNetwork.webserver.addMenuItem("Gerätekonsole", "/console", 60);

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
                _noMemBytes = 0;
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

            if (_noMemBytes && (millis() - _lostReported) >= 1000)
            {
                _lostReported = millis();
                char note[80];
                int n = snprintf(note, sizeof(note), "\x1B[33m[... %u bytes of log dropped, no memory ...]\x1B[0m\n", (unsigned)_noMemBytes);
                if (n > 0 && ws.txFits("log", (size_t)n))
                {
                    ws.ui.sendMessage("log", note, (size_t)n); // the queue is a fixed ring: no heap
                    _noMemBytes = 0;
                }
            }

            // Each line must fit without evicting; only the first may exceed the per-tick quarter.
            if (!ws.txFits("log", 0)) return; // full: don't build lines just to discard them
            uint32_t budget = (uint32_t)ws.txCapacity() / 4;
            bool firstLine = true;
            size_t heap = 0; // largest block, read once per pass at the first line: each line is freed before the next
            bool heapRead = false;
            const char* ring = openknx.logger.ringBuf();

            while (_readPos != writePos)
            {
                // Send only complete lines so the client-side ANSI parser never has
                // to carry state across messages. Fall back to a fixed chunk if no
                // newline within maxLine bytes (e.g. very long hex-dump lines).
                uint32_t pos = _readPos;
                uint32_t high = 0; // bytes >= 0x80: two or three bytes in UTF-8
                const uint32_t scanEnd = (writePos - _readPos < maxLine) ? writePos : _readPos + maxLine;
                bool foundNewline = false;
                while (pos != scanEnd)
                {
                    const char c = ring[pos % Log::Logger::RING_SIZE];
                    pos++;
                    if (c == '\n')
                    {
                        foundNewline = true;
                        break;
                    }
                    high += (uint8_t)c >= 0x80;
                }
                if (!foundNewline && writePos - _readPos < maxLine) break;

                // Ring buffer is ISO-8859-15; WS text frame needs UTF-8: one allocation of the upper bound, no growth.
                const uint32_t len = pos - _readPos;
                const size_t utf8Max = (size_t)len + 2 * (size_t)high;
                if (!heapRead)
                    heap = heapForLine(), heapRead = true;
                if (utf8Max + 1 + kHeapFloor > heap)
                {
                    _noMemBytes += len; // dropped, reported once a second above
                    _readPos = pos;
                    continue;
                }
                std::string line;
                line.reserve(utf8Max);
                const uint32_t at = _readPos % Log::Logger::RING_SIZE;
                const uint32_t first = (len <= Log::Logger::RING_SIZE - at) ? len : Log::Logger::RING_SIZE - at;
                OpenKNX::Charset::encodeUtf8(line, ring + at, first);
                if (first < len)
                    OpenKNX::Charset::encodeUtf8(line, ring, len - first); // the line wraps at the ring end

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
