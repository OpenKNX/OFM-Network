# Changes

## upcoming releases

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
   