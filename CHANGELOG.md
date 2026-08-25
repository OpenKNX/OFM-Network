# Changes

## upcoming releases

* feature: The web interface now keeps **one WebSocket open on every page** (`/openknx/ws`) instead of one per feature. Values that change on the device — programming mode, KNX address, network data, free memory — update live, without reloading. The programming-mode button in the navigation switches over the socket; without JavaScript the previous form post still works
* change: `OPENKNX_WEBSERVER_MAX_CONN` is one flag for both platforms now (ESP32 7, RP2040 6) and limits the sum of HTTP and WebSocket connections; the mix is free, so a slot goes to whoever asks first. The separate WebSocket cap on ESP32 is gone — it reduced the number of possible visitors to guard a state that resolves itself on the next page reload
* fix: the group monitor no longer depends on the file manager for group-address names. It serves `/openknx_ga.tsv` from its own route, so the Name column also works on devices built without `OPENKNX_WEBFS` — previously the page requested `/filemanager/download` and got a 404. A device without a group-address table answers with an empty table rather than an error, so opening the page no longer writes a warning to the log
* fix: the web console reports when log lines were lost instead of skipping them silently — if a command produces more output than the logger ring holds, a marker naming the discarded byte count appears in the stream
* fix: a device that goes away is noticed by the browser within about three seconds. A rebooted or unplugged device sends neither FIN nor RST, so the page kept an open socket — and a green status dot — until it happened to send something itself; the page now pings, and treats an unanswered ping as a disconnect immediately instead of waiting for the socket's own timeout
* feature: the navigation shows the state of that connection (`Verbunden` / `Getrennt`), so a page that has silently lost the device is recognisable without reading the log
* feature: Modules can use that socket: `webserver.ui.sendMessage(key, data)` sends, `webserver.ui.receiveMessage(cb)` receives, and the reserved key `connect` asks every producer for its current state whenever a client connects — a reconnect resynchronises a page completely
* change: **WebSocket sends are queued.** A frame used to be discarded when a client's TCP buffer was momentarily full; now the transport buffers it (`OPENKNX_WEBSOCKET_TX_BUFFER`, default 4 KB, one ring for all clients) and delivers it on the next loop pass. This applies to every endpoint, including those registered by other modules. A client that falls so far behind that it is overtaken in the ring is disconnected, and reconnects on its own
* change: the device console and group monitor use the shared socket; their own endpoints `/console`, `/groupmonitor` and the route `/groupmonitor/state` are gone. The pages themselves are unchanged. The console now reconnects automatically and marks the gap in the log instead of asking for a page reload
* change (RP2040): WebSocket sends and the queue's bookkeeping are guarded with the core's `LWIPMutex`, because on boards with an Ethernet interrupt pin lwIP callbacks preempt the main loop — without it a disconnect arriving mid-send could advance the wrong client's read position
* fix (ESP32): the HTTP port is opened only after all pages have registered. Previously the server was already answering while console, file manager and group monitor were still adding their routes, and a request landing in that window could crash the device
* fix (RP2040): a streaming download of an empty file left the connection hanging until it timed out — the response header was written but never flushed, which also held a connection slot for about twenty seconds
* fix: the busmonitor switch is executed from the main loop instead of the network callback, where it could corrupt the TP-UART's command queue on RP2040
* fix (RP2040): a WebSocket frame larger than the receive buffer now drops the connection instead of being truncated and delivered anyway — a too-long console command used to be executed in half. ESP32 already behaved this way
* change (RP2040): a device project that sets `-D__LWIP_MEMMULT=2` gets **6 webserver connection slots** instead of 3, so three visitors can be served at once — each one occupies a WebSocket plus an HTTP slot while loading. Without the flag the default stays at 3 — enough for one visitor — because lwIP would otherwise run out of TCP control blocks and the last connection would fail silently. `platformio.network.ini` carries the snippet

## 0.8.0: 2026-08-17

* change: the **webserver access log** only prints failures by default — `4xx` as warning, `5xx` as error. Successful requests (`2xx`/`3xx`, WebSocket upgrades) are logged only after enabling them at runtime with the console command `webserver log`
* fix (ETS): the **WiFi assistant** has its own checkbox again (parameter `01004`) — it used to share the MQTT device-prefix parameter, so the assistant page was only reachable by enabling that prefix. The MQTT prefix keeps its released slot, so existing projects are unaffected

## 0.7.0: 2026-08-14

* feature: Add a built-in **web server** (`OPENKNX_WEBSERVER`) with an overview page — available on both ESP32 and RP2040
* feature: Add a browser-based **device console** (`OPENKNX_WEBCONSOLE`) — run console commands and view the log from the browser
* feature: Add a **file manager** (`OPENKNX_WEBFS`) for the device's flash storage — browse, upload, and delete files from the browser (directory and file names are HTML/JS-escaped in the listing)
* feature: The file manager can additionally serve an **SD card** (`OPENKNX_SDCARD`) and an **external flash chip** (`OPENKNX_EXTFLASH`) — a drive selector appears above the listing as soon as one of them is present. Requires the file-store providers from OFM-SDCard / OFM-ExternalFlash; without those defines only the internal flash is compiled in, unchanged
* feature: Add an **MQTT client and broker** (`OPENKNX_MQTT`) — no external dependencies, available on both ESP32 and RP2040
* feature: Add an **HTTP(S) client** (`OPENKNX_WEBCLIENT`) for the device to call out to other services, with certificate verification
* feature: Add a **group monitor** (`OPENKNX_WEBMONITOR`) — live, ETS-style view of KNX telegrams in the browser (TP devices only)
* change: **OTA** is always allowed while the device is unconfigured — the ETS gate (`OTA-Update`: prog mode / always / off) only applies once the device is configured. Previously an unconfigured device evaluated erased parameter memory and could lock itself out of OTA, which is exactly the state you want to re-flash from (e.g. after the KNX flash was erased)
* feature: Add **ping support** — check host reachability by IP or hostname, from the console (`ping <target>`) or via API
* feature: Memory optimizations — use PSRAM for buffers where available, reducing RAM pressure on the webserver and MQTT features
* fix (ESP32): the KNX multicast socket is now rebound when the device's IP address changes — it used to stay bound to the old address, which silently took the device off the KNX bus until the next restart. A plain DHCP lease renewal still leaves the socket untouched, so no telegrams are lost for nothing. The datalink is additionally reconciled against the actual link state every 500 ms, which recovers a link flap in which `GOT_IP` never fires again

## 0.6.0: 2026-05-15

* Initial Changelog started
* fix: ensure NTP server is stopped only if enabled and adjust sync mode
* fix: update WiFi settings handling to prevent disconnect on reboot
* fix: ensure NTP server is stopped only if enabled and adjust sync mode
* enhancment: use new function property wrapper
   