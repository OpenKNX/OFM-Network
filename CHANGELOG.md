# Changes

## Unreleased

* fix: ETS "Standardparameter" (reset to defaults) aborted on every app with this module since 0.7.0: the MQTT prefix default is empty, but its pattern required at least one character (ETS log: `ValueOutOfRangeException` in `ResetToDefault`). The pattern now accepts the empty value, which the firmware already maps to the serial number, and rejects line breaks. An app takes the fix over with its next version; devices in a project need "update application" to that version

## ec/v0.9.0-beta.2: 2026-10-29

A WiFi fix and web interface changes on top of `ec/v0.9.0-beta.1`. OAM-IP-Interface and OAM-IP-Router call `setDefaultPage()` and need this version.

* fix: the WiFi routes are registered once; before the web server's first start the setup access point added the WiFi menu item and its 13 routes on every loop pass and drained the heap (ESP32-S3: mDNS failed, boot loop). Affects every build with `OPENKNX_WIFI_AP`
* feature: `webserver.setDefaultPage(uri)` answers `/` with a redirect (302) to that page when a GET route for it is registered; without it `/` shows the overview as before. The overview is also served at `/overview`, and the menu item "Übersicht" points there
* feature: `webserver.ui.trySendBinaryMessage(key, prefix, prefixLen, data, len)` sends `key:` + prefix + data as a binary WebSocket frame to every client of the UI socket, on RP2040/RP2350 and ESP32. It never waits: with no client, the ESP32 queue lock taken, a record above the queue maximum or a record that would leave less than the reserve free (1/8 of the queue, at most 512 bytes), it returns `false` and the caller drops that sample. `base.js` hands binary payloads to the page handlers as `Uint8Array`
* feature: `OpenKNX.need(key, ask)` in `base.js` covers the page content with "Verbinde …" until the socket is open and with "Lade Daten …" until the page calls the returned function; while the socket is open and nothing for `key` arrived for 1.5 s, it calls `ask` again
* feature (phone): the menu stays on one line; entries that do not fit, and the wiki and forum links, move behind a burger button
* change: the navigation is 180 px wide instead of 220 px; the menu priority of the device console is 60 (before 100), of WiFi 30 (before 0)
* change: overridden phone rules removed from `base.css`, 20 bytes less in the gzipped asset

## ec/v0.9.0-beta.1: 2026-10-25

### From upstream v1dev (merged)

* feature: The web interface now keeps **one WebSocket open on every page** (`/openknx/ws`) instead of one per feature. Values that change on the device — programming mode, KNX address, network data, free memory — update live, without reloading. The programming-mode button in the navigation switches over the socket; without JavaScript the previous form post still works
* change: `OPENKNX_WEBSERVER_MAX_CONN` is one flag for both platforms now (ESP32 7; RP2040 3, or 6 with `-D__LWIP_MEMMULT=2`) and limits the sum of HTTP and WebSocket connections; the mix is free, so a slot goes to whoever asks first. The separate WebSocket cap on ESP32 is gone — it reduced the number of possible visitors to guard a state that resolves itself on the next page reload
* fix: the group monitor no longer depends on the file manager for group-address names. It serves `/openknx_ga.tsv` from its own route, so the Name column also works on devices built without `OPENKNX_WEBFS` — previously the page requested `/filemanager/download` and got a 404. A device without a group-address table answers with an empty table rather than an error, so opening the page no longer writes a warning to the log
* fix: the web console reports when log lines were lost instead of skipping them silently — if a command produces more output than the logger ring holds, a marker naming the discarded byte count appears in the stream
* fix: a device that goes away is noticed by the browser within about three seconds. A rebooted or unplugged device sends neither FIN nor RST, so the page kept an open socket until it happened to send something itself; the page now pings, and treats an unanswered ping as a disconnect immediately instead of waiting for the socket's own timeout
* feature: the navigation shows the state of that connection (`Verbunden` / `Getrennt`), so a page that has silently lost the device is recognisable without reading the log. After a disconnect, the console's gap marker and the tooltip on `Getrennt` say how long the connection lasted and who ended it — the browser after an unanswered ping, or the device closing the socket
* feature: Modules can use that socket: `webserver.ui.sendMessage(key, data)` sends, `webserver.ui.receiveMessage(cb)` receives, and the reserved key `connect` asks every producer for its current state whenever a client connects — a reconnect resynchronises a page completely
* change: **WebSocket sends are queued.** A frame used to be discarded when a client's TCP buffer was momentarily full; now the transport buffers it (`OPENKNX_WEBSOCKET_TX_BUFFER`, default 4 KB, one ring for all clients) and delivers it on the next loop pass. This applies to every endpoint, including those registered by other modules. A client that falls so far behind that it is overtaken in the ring, or that has stopped acknowledging for ten seconds, is disconnected and reconnects on its own. Producers such as the console and the group monitor wait or skip instead of overtaking a slow client; modules can do the same with `webserver.txFits(key, len)`
* change: the device console and group monitor use the shared socket; their own endpoints `/console`, `/groupmonitor` and the route `/groupmonitor/state` are gone. The pages themselves are unchanged. The console now reconnects automatically and marks the gap in the log instead of asking for a page reload
* fix (ESP32): the HTTP port is opened only after all pages have registered. Previously the server was already answering while console, file manager and group monitor were still adding their routes, and a request landing in that window could crash the device
* fix (RP2040): a streaming download of an empty file left the connection hanging until it timed out — the response header was written but never flushed, which also held a connection slot for about twenty seconds
* fix: the busmonitor switch is executed from the main loop instead of the network callback, where it could corrupt the TP-UART's command queue on RP2040
* fix (RP2040): a WebSocket frame larger than the receive buffer now drops the connection instead of being truncated and delivered anyway — a too-long console command used to be executed in half. ESP32 already behaved this way
* fix (RP2040): cancelling a page load in the browser could crash the device — the network interrupt freed the connection while its response was still being built. Responses are now built and sent with the network stack locked
* fix (RP2040): file downloads no longer read the file system from inside the network interrupt, where they could collide with other file access; a chunk the network stack refused is sent again instead of being lost, so a download under memory pressure is no longer corrupted
* fix (RP2040): a page or download no longer claims the network stack's whole memory at once (at most two TCP segments in flight per connection), which starved the other connections — the console's WebSocket among them; a response header that could not be queued is retried instead of the body going out without it
* fix (RP2040): a closed connection could still receive callbacks and tear down the next connection that reused its slot, and leaked receive buffers — for example after a browser asked for a WebSocket path the device does not serve
* fix (RP2040): a failed start of the webserver leaked a TCP control block on every retry, until MQTT and OTA could no longer connect either
* fix (ESP32): the HTTP client no longer reads freed memory when it reports a result — the request was deleted before its ID was copied into the result, so a callback could fire for the wrong request or not at all
* fix (ESP32): starting a new request from inside an HTTP client callback (`onDone`, `onData`) no longer corrupts the list of pending requests
* fix (RP2040): the MQTT client no longer runs its protocol handling and your subscription callbacks inside the network interrupt, where they could collide with the main loop working on the same connection. Received data is now acknowledged only once it has been processed, so a slow loop throttles the broker instead of losing messages
* fix (RP2040): the MQTT broker no longer shares its receive buffers and subscription lists unprotected between the network interrupt and the main loop, and no longer discards data it had already acknowledged when a client sent faster than the loop processed it
* fix (RP2040): an HTTPS response from a server that closes the connection right after sending no longer loses its last part
* fix: an incoming MQTT message larger than the client's 1 KB receive buffer is skipped with a warning. On RP2040 it used to be cut silently, which garbled the following messages; on ESP32 it caused a reconnect
* fix (RP2040): the HTTP client could crash when a response arrived while the main loop was reading the previous part of it — both worked on the same receive chain without a lock. A server closing the connection no longer frees it under a send in progress, and a momentarily full send buffer delays the request instead of failing it
* fix (RP2040): the NTP client no longer reports a time when the server did not answer — the device then took the unset clock (1970) as valid. It logs an error instead and tries again an hour later
* fix (RP2040): host names for MQTT, the HTTP client and ping are resolved without running the follow-up (connecting, sending the ping) inside the network interrupt
* change (RP2040): a device project that sets `-D__LWIP_MEMMULT=2` gets **6 webserver connection slots** instead of 3, so three visitors can be served at once — each one occupies a WebSocket plus an HTTP slot while loading. Without the flag the default stays at 3 — enough for one visitor — because lwIP would otherwise run out of TCP control blocks and the last connection would fail silently. `platformio.network.ini` carries the snippet

### Not released upstream yet

Everything below is on `ec/v1dev-ec` and has not been released upstream yet.

**KNXnet/IP**
* feature: KNX-IP status LED — green, orange or red for the state of the KNXnet/IP datalink, so a link problem is visible without a console. Reads the IP datalink on a `0x07B0` interface as well, not only on the router
* fix: IP capabilities and the current assignment method are reported per 03_08_03 — ETS read values that did not describe how the device actually got its address
* fix: the KNX multicast socket is rebound on an IP change and the link state reconciled, restored after the namespace refactor dropped it
* fix: the RP2040 W5500 robustness layer was lost in the refactor and is restored
* feature: AutoIP fallback, lwIP link state on the W5500, and connection-age plus reconnect statistics
* fix (ESP32): KNX-IP is switched only from the main loop. The network event handler runs on its own task and opened or closed the KNXnet/IP endpoint while the loop was using it; it now records the request and the loop applies it. The handler is registered once, so a network setting applied from the display no longer makes every later event run it twice
* fix (ESP32): after a link flap that the link check recovers, the GOT_IP that follows for the same address no longer closes and reopens the endpoint
* feature (RP2040, W5500): the KNX stack drains the W5500 before it sends a routing frame, so a ROUTING_BUSY waiting in the chip is seen first; a ROUTING_INDICATION is not sent while a ROUTING_BUSY waits in the chip's receive buffer, the router keeps it queued instead. On an interface (mask 07B0) the refused frame is not queued: it is confirmed negatively and counted in PID 72

**Display widgets**
* Feature: `WidgetNetSpeed` ships with this module now, where the packet counters live, instead of being copied into every product
* Feature: `WidgetNetInfo` shows host name, IP with its assignment method, link state and link age -- taken off the system-info widget of OGM-Common, which had to reach into this module for them
* Change: both register from `loop()`, not `setup()`: this module is number 7 and DeviceDisplay is 10, so in `setup()` there is no widget manager yet
* feature (ESP32, wired LAN): the LAN speed widget, the overview page (`Pakete RX/TX`) and `net` show the packet counters of the whole interface. The ESP32 ETH class exposes none, so the module counts at the lwIP netif, whose input and output functions it wraps in the tcpip task. Built only for a device with a display, or with `OPENKNX_NET_PACKET_COUNT`; before, the widget showed "n/a" on ESP32. Measured on an ESP32 interface: 20 echo requests raised the counters by 48 and 41
* fix: the network speed widget keeps the rate values clear of the RX arrow and the right edge; a value that does not fit its half is drawn in the small font, and the unit is always shown
* fix: the network speed widget takes a new rate base after start and resume -- the first rate after a rotation covered the whole time the widget was off and added one history bar for it; the values show "-" until the first rate is measured

**Network configuration and diagnostics**
* feature: mDNS announces the product, the KNX order number and the board — a scanner can name a device instead of showing a bare serial, and a tool can tell before an upload whether the image belongs to it
* feature: the link mode comes from ETS instead of from flash — the mode set in the product now decides, so a device no longer keeps a mode that was written once and never changed
* feature: whole-interface packet counters, so throughput and loss can be read per interface rather than guessed from the bus side
* feature: the TLS chain is validated against a root certificate; the HTTPS client accepted any chain before
* feature: a file can be downloaded from a URL straight onto the device, without a PC in between
* feature: the USB-exchange hooks are optional, so a product without that module compiles without them
* feature: `net ?` lists every `net ...` subcommand this build has; the main console help only points at it instead of listing them all
* feature: `net phy` reads the W5500 out over SPI -- VERSIONR, PHYCFGR, the RSTn pin and the SPI clock; `net phy ver` re-reads VERSIONR at 1 MHz to separate a silent chip from a marginal clock; `net phy reset [ms]` pulses RSTn and reports VERSIONR before and after; `net phy pin [s]` puts a 1 Hz square wave on RSTn so a meter can show whether the pulse reaches the chip. RP2040 only -- on ESP32 the SPI bus belongs to `esp_eth`
* feature: when the W5500 stops answering at runtime, the log records how long it had lived, the link age, the packet counters and the reconnect count, so a failure that takes hours can be correlated with load afterwards
* fix: the W5500 self-heal no longer blocks the loop -- the retry spent 62 ms of `delay()` in a single pass every 5 s, which starved the TPUart receive path (KNX bus protocol errors) and chopped up the console for as long as the chip stayed down; the RSTn pulse is stepped across loop passes now
* change: the self-heal retry spacing widens with the failure count -- 5 s for the first three attempts, 30 s up to the tenth, 120 s after that, instead of retrying every 5 s forever on a chip that RSTn cannot revive
* doc: the W5500 and LAN8720A datasheets are in `doc/datasheets/`, indexed with revision, vendor URL and the pages the driver depends on
* fix: a single garbled PHYCFGR read no longer takes the network down -- the link check acted on one read, and a garbled one produced a link-down edge that dropped the DHCP binding and closed every open tunnel, leaving the device without an address until a reboot while the PHY never lost carrier. A change is now accepted only after three consecutive reads agree, and the whole 500 ms tick derives from one reading instead of re-reading the register for each decision
* feature: link up but no address is no longer waited out -- a DHCP renew after 60 s, then a re-initialisation of the interface after 180 s, which is what a warm reboot does for the network without restarting the device. Both stages log, so the fault is recorded without a console attached; AutoIP (169.254/16) counts as no address, and a segment that never had a DHCP server is left alone
* fix: the KNXnet/IP endpoint is rebuilt when the link returns -- the module reports the event and the BAU decides whether it owns one, so discovery survives a cable change instead of dying until the next reboot
* fix: the W5500 recovery cannot hang the device any more. `recoverEth()` resets the PHY before anything can transmit, because the IGMP leave goes out through `sendFrame()` and `end()` polls twice, all unbounded on a chip that no longer answers; the bring-up probes VERSIONR before `begin()`, whose `setSn_CR()` has the same unbounded wait
* fix: the health probe also reads socket 0. A wedged MACRAW socket leaves VERSIONR and the PHY link healthy while no packet moves, which the old probe could not see
* fix: the KNXnet/IP datalink is re-enabled after a recovery, and `net reset` no longer disables it on a path that returns before the re-enable -- on a router that left the device without routing or discovery until a reboot
* fix: the SPI clock has one source. The health probe re-clocked the bus and the Wiznet driver never sets a clock of its own, so the configured 28 MHz was not in effect after the first probe; probes now use the driver clock and only a deliberate diagnostic rate is restored. The default is the 8 MHz the bus has actually been running at -- raising it is a separate change that needs its own soak test
* fix: the post-mortem line logs a link duration that still exists at that moment and a rollover-safe uptime
* change (RP2040, W5500): the module runs its own copy of the arduino-pico W5500 driver (`src/driver/W5500Drv`, BSD-3, differences listed in `W5500Drv.md`). Register and frame data move in SPI blocks, a frame's SENDOK is awaited before the next send instead of after its own, and every wait on the chip has a bound, so a chip that stops answering no longer hangs the main loop in `sendFrame()`, `end()` or `setSn_CR()`; a corrupt received frame length is dropped instead of moving the receive pointer
* feature: `net` shows the W5500 socket reopens, close timeouts and command timeouts since boot
* change (RP2040): the module hands its lwIP interface to the knx platform (`setNetif()`); it needs the matching knx version
* fix (RP2040, W5500): a W5500 that does not answer at boot no longer puts the device into a watchdog loop. lwIP was initialised only when the chip came up, so mDNS ran on an uninitialised stack and panicked; after five resets the watchdog auto-erase cleared the KNX programming. The device now starts without network and the self-heal keeps trying
* fix (RP2040): the NTP sync no longer blocks the main loop -- every hour it waited in a synchronous DNS lookup and up to 10 s for the answer, so with an unreachable server a network update stalled until espota gave up. lwIP SNTP now resolves the server and sets the clock in the background; "No answer from" is logged when the first answer is missing after 60 s
* change: `net phy`, `net tls`, the webserver log and `http get` are built only with `OPENKNX_CONSOLE_DEV`; the setup, status and link commands (`net`, `net reset`, `net wifi ...`, `net mc`, `net eth ...`, `ping`) stay
* fix: the LAN mode parameter opens its ETS help page `NET-LAN-Modus`; it referenced a placeholder as help context

**OTA**
* fix (RP2040): after a network update the image the core staged in LittleFS is noted, and OGM-Common releases it at the next start once the bootloader has run it; it stayed and blocked the next upload on a 2 MB RP2040
* feature: with the dev/test switch `OPENKNX_OTA_EXCLUSIVE` a network update pauses KNX and the other modules through OGM-Common and logs that it does. Not for production

**WiFi**
* feature: devices with a WiFi interface (RP2040 Pico W / Pico 2 W, ESP32) get the ETS module NETW: a setup access point without credentials or from the prog button, an own access point as operating mode, a network scan with "Übernehmen" in the ETS, a client rejoin, the page `/wifi` (signal history, gateway ping, QR code), a display widget, the LED function `WiFi-Status` and the console commands `net wifi ...`
* feature: the ETS checks both WiFi password fields while typing: empty or 8 to 63 characters
* feature: `net wifi` and the network information show the BSSID of the access point the client is joined to, a change of access point is logged, and `net wifi scan` lists every access point with its BSSID
* fix (RP2040): a WPA2 client whose join failed is joined again -- the core reported the join as done once requested and set the link flag, so a failed join left the device without a connection; boot no longer blocks 4 s in the join
* fix (RP2040): a WiFi scan without an end event from the driver is given up after 15 s -- it stayed marked as running, which also stopped the client rejoin
* fix (RP2040): AutoIP no longer replaces an address from DHCP with 169.254/16 after a rejoin -- lwIP does not stop a running AutoIP check when DHCP binds; AutoIP is now also stopped while the client link is down
* fix (RP2040): the own access point announces WPA2 only -- the WiFi driver announces WPA and WPA2, and a Broadcom WiFi adapter under Windows refused to connect; if the switch does not read back, the access point restarts with WPA/WPA2 and the page, the display and `net wifi` show that
* feature: with an access point running, `net wifi` and the page `/wifi` (own access point) list the connected devices with IP and MAC address
* fix: the WiFi module builds on a TP device without KNX/IP (WiFi for the web interface only, e.g. 0x07B0 without tunnelling); the access point skips the KNX multicast join there

**Web UI**
* feature: the browser tab shows the hostname instead of "OpenKNX", so several devices can be told apart in tabs, bookmarks and history -- the name mDNS and DHCP already use, changeable in ETS
* fix: the hostname is HTML-escaped in the title and in the overview row -- it is the one value on those pages that comes from an ETS parameter
* change: the HTML escaper is one inline function in `Webserver.h` instead of a copy per page
* change: a menu item is inserted at its sorted place instead of sorting the whole list -- the same order, 8798 bytes less template code

**Display**
* change: the network widgets are named Network and NETWORK. The display stores widget settings by name, so the renamed widget starts once with its default order, visibility and duration

**Post-merge audit**
* fix: OTA gate, establish debounce, KNX-IP LED and OTA RX contention — four defects found by re-tracing the paths after the upstream merge
* fix: `propertyValueRead` freed the buffer it allocated
* doc: how to contribute a webserver page from another module

## 0.8.1: 2026-09-15

* fix (ESP32, security): a WiFi device no longer opens an **unsecured access point** (`ESP_xxxxxx`, 192.168.4.1) alongside its client connection. The interface ran in combined AP+STA mode without the AP ever being configured, so anyone in range could connect without credentials and reach the web interface, console and OTA. The device now runs as a plain WiFi client (#23)

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
   