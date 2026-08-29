# Changes

## upcoming releases

Everything below is on `ec/v1dev-ec` and has not been released upstream yet.

**KNXnet/IP**
* feature: KNX-IP status LED — green, orange or red for the state of the KNXnet/IP datalink, so a link problem is visible without a console. Reads the IP datalink on a `0x07B0` interface as well, not only on the router
* fix: IP capabilities and the current assignment method are reported per 03_08_03 — ETS read values that did not describe how the device actually got its address
* fix: the KNX multicast socket is rebound on an IP change and the link state reconciled, restored after the namespace refactor dropped it
* fix: the RP2040 W5500 robustness layer was lost in the refactor and is restored
* feature: AutoIP fallback, lwIP link state on the W5500, and connection-age plus reconnect statistics

**Display widgets**
* Feature: `WidgetNetSpeed` ships with this module now, where the packet counters live, instead of being copied into every product
* Feature: `WidgetNetInfo` shows host name, IP with its assignment method, link state and link age -- taken off the system-info widget of OGM-Common, which had to reach into this module for them
* Change: both register from `loop()`, not `setup()`: this module is number 7 and DeviceDisplay is 10, so in `setup()` there is no widget manager yet

**Network configuration and diagnostics**
* feature: the link mode comes from ETS instead of from flash — the mode set in the product now decides, so a device no longer keeps a mode that was written once and never changed
* feature: whole-interface packet counters, so throughput and loss can be read per interface rather than guessed from the bus side
* feature: the TLS chain is validated against a root certificate; the HTTPS client accepted any chain before
* feature: a file can be downloaded from a URL straight onto the device, without a PC in between
* feature: the USB-exchange hooks are optional, so a product without that module compiles without them

**Post-merge audit**
* fix: OTA gate, establish debounce, KNX-IP LED and OTA RX contention — four defects found by re-tracing the paths after the upstream merge
* fix: `propertyValueRead` freed the buffer it allocated
* doc: how to contribute a webserver page from another module


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
   