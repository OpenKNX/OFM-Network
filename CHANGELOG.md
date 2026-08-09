# Changes

## 0.7.0

* feature: Add a built-in **web server** (`OPENKNX_WEBSERVER`) with an overview page — available on both ESP32 and RP2040
* feature: Add a browser-based **device console** (`OPENKNX_WEBCONSOLE`) — run console commands and view the log from the browser
* feature: Add a **file manager** (`OPENKNX_WEBFS`) for the device's flash storage — browse, upload, and delete files from the browser (directory and file names are HTML/JS-escaped in the listing)
* feature: The file manager can additionally serve an **SD card** (`OPENKNX_SDCARD`) and an **external flash chip** (`OPENKNX_EXTFLASH`) — a drive selector appears above the listing as soon as one of them is present. Requires the file-store providers from OFM-SDCard / OFM-ExternalFlash; without those defines only the internal flash is compiled in, unchanged
* feature: Add an **MQTT client and broker** (`OPENKNX_MQTT`) — no external dependencies, available on both ESP32 and RP2040
* feature: Add an **HTTP(S) client** (`OPENKNX_WEBCLIENT`) for the device to call out to other services, with certificate verification
* feature: Add a **group monitor** (`OPENKNX_WEBMONITOR`) — live, ETS-style view of KNX telegrams in the browser (TP devices only)
* feature: Add **ping support** — check host reachability by IP or hostname, from the console (`ping <target>`) or via API
* feature: Memory optimizations — use PSRAM for buffers where available, reducing RAM pressure on the webserver and MQTT features
* fix (ESP32): the KNX multicast socket is now rebound when the device's IP address changes — it used to stay bound to the old address, which silently took the device off the KNX bus until the next restart. A plain DHCP lease renewal still leaves the socket untouched, so no telegrams are lost for nothing. The datalink is additionally reconciled against the actual link state every 500 ms, which recovers a link flap in which `GOT_IP` never fires again
* fix: the programming-mode toggle on the overview page (`/prog`) now requires POST instead of GET, so it can no longer be triggered by a plain `<img src>` on an unrelated page
* fix: the MQTT broker now always grants and delivers QoS 0 outbound (it tracks no message IDs/acks, so a higher QoS would be invalid on the wire); QoS 2 publishes are still accepted from clients but routed at-least-once rather than held for exactly-once delivery
* fix: MQTT topic wildcard matching (`+`/`#`) now follows MQTT 3.1.1 §4.7 exactly (e.g. `a/+` matches `a/`, `a/#` matches `a`)
* note: none of the webserver endpoints (overview, console, file manager, group monitor) require authentication yet — treat the device as trusted-network-only until an auth layer is added
* fix: `Webserver.cpp` now explicitly includes `buildtime.h` for `BUILD_DATETIME`/`BUILD_TIMESTAMP` (moved there from `versions.h` in OGM-Common)
* enhancement (RP2040): each webserver connection slot now holds a single receive buffer instead of separate HTTP and WebSocket ones — a connection is only ever in one of the two protocols, so the second buffer was never live at the same time. The size is now one flag, `OPENKNX_WEBSERVER_RX_CAP` (default 1024, was 1536 for HTTP + 512 for WS); `OPENKNX_WEBSOCKET_RX_CAP` is ESP32-only from now on. Saves ~2.3 KB RAM at the default of 3 slots. Side effect: inbound WS payloads may be up to 1024 B before truncation instead of 512; the smaller HTTP header buffer still fits a normal browser request but leaves less room for large cookies/headers than before

## 0.6.0: 2026-05-15

* Initial Changelog started
* fix: ensure NTP server is stopped only if enabled and adjust sync mode
* fix: update WiFi settings handling to prevent disconnect on reboot
* fix: ensure NTP server is stopped only if enabled and adjust sync mode
* enhancment: use new function property wrapper
   