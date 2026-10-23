# OFM-Network

This module provides the network functionality for the OpenKNX stack.

## Platforms

* ARDUINO_ARCH_ESP32
* ARDUINO_ARCH_RP2040

## Defines

| Arch   | Type | Stack      | Define      | Note                                                          |
| ------ | ---- | ---------- | ----------- | ------------------------------------------------------------- |
| ESP32  | WiFi | Integrated | KNX_IP_WIFI |                                                               |
| ESP32  | LAN  | Integrated | KNX_IP_LAN  | You need to set right board and presumably CONFIG_ETH_ENABLED |
| RP2040 | WiFi | Integrated | KNX_IP_WIFI |                                                               |
| RP2040 | LAN  | Integrated | KNX_IP_LAN  |                                                               |


| Define                   | Default | Description                          | Note                                         |
|--------------------------|---------|--------------------------------------|----------------------------------------------|
| OPENKNX_LED_IP           |         | LED used for IP state                | Set to info2Led to use IP LED feature        |
| OPENKNX_PING             |         | Enable ping functionality            | Requires inclusion of Header in setup        |
| OPENKNX_PING_TIMEOUT     | 1000    | Default ping timeout in milliseconds | Can be overridden per call                   |
| OPENKNX_PING_PARALLEL    | 5       | Max concurrent pings                 | Additional requests are queued automatically |
| OPENKNX_WEBCLIENT        |         | Enable HTTP(S) client                | See below for API                            |
| OPENKNX_WEBCLIENT_SLOTS  | 2       | Parallel request slots (RP2040) / queue depth (ESP32) | |
| OPENKNX_WEBCLIENT_TIMEOUT| 10000   | Request timeout in milliseconds      |                                              |
| OPENKNX_WEBCLIENT_TASK_STACK | 6144| ESP32 FreeRTOS task stack size (bytes) | SSL handshake requires ~4 KB             |
| OPENKNX_WEBCLIENT_MAX_BODY | 4096  | Max request body size (bytes)        |                                              |
| OPENKNX_WIFI_SCAN        |         | Console `net wifi scan`: lists visible WiFi networks (async) | KNX_IP_WIFI only |
| OPENKNX_WIFI_AP          |         | Open setup AP `<hostname>` + page `/wifi`, never together with the client: without WiFi credentials, when an unprogrammed device has no client link 30 s after boot, or 5 s after the prog button without a link | Implies OPENKNX_WIFI_SCAN, needs OPENKNX_WEBSERVER; saving needs the AP or programming mode |
| OPENKNX_OTA_EXCLUSIVE    |         | Dev/test aid: pause KNX and all other modules during OTA | Not for production; needs the OGM-Common version that provides it, see OTA below |

## IP LED

If OPENKNX_LED_IP is defined, the LED is representing the state of the network. 
Possible values for OPENKNX_LED_IP: info1Led, info2Led, info3Led. Recommended value: info2Led

| State                         | LED           | RGB-LED            | Note                                   |
|-------------------------------|---------------|--------------------|----------------------------------------|
| WLAN configuration missing    | Fast flashing | Red fast flashing  | Only for HW with WLA                   |
| No connection to the network  | Off           | Red                |                                        |
| No IP adress                  | Slow flashing | Yellow             |                                        |
| IP adress assigned            | On            | Green              |                                        |

## Ping

The module provides a non-blocking ping API with an internal queue, parallel slot management, and optional retry logic.

Full documentation: [`README.Ping.md`](README.Ping.md)

## Webclient

`#define OPENKNX_WEBCLIENT` enables a non-blocking HTTP/HTTPS client accessible via `openknxNetwork.webclient`.

Full documentation: [`README.Webclient.md`](README.Webclient.md)

## OTA

The network module provides OTA (Over the Air) firmware update functionality.
OTA must first be enabled on the device.
This can be done by pressing the PROG button or via the console using the `ota` command.

A section must be added to `platformio.custom.ini` as an OTA target,
specifying the upload protocol and the IP address or hostname of the device:

```ini
upload_protocol = espota
upload_port = XXX.XXX.XXX.XXX # IP Address or Hostname
```

Note: The device hostname can be set in ETS under the Network → mDNS section.

Dev/test aid `-D OPENKNX_OTA_EXCLUSIVE` (not for production): from OTA start to end or error, OGM-Common's `loop()`
runs only watchdog, LEDs and display; KNX and all other modules pause. OTA calls `openknx.loop()` after every received
chunk, so a module that blocks there stalls the transfer until espota aborts after 10 s. If OTA fails without the
switch and passes with it, a blocking module is the cause; fix that module, the switch is not the fix.

## WiFi module (NETW)

`src/NetworkWifi.share.xml` is a separate ETS module for products with a WiFi interface
(`<op:define prefix="NETW" ... share="../lib/OFM-Network/src/NetworkWifi.share.xml" />`), so products
without WiFi keep their parameter layout. Firmware side: `OPENKNX_WIFI_AP`.

- Stored: operating mode (client / own AP), setup AP on/off, AP name (empty = host name), AP channel (auto / 1-13).
- Not in the device parameters: client SSID/PSK and the AP password go online through function property 160/5
  (commands 1 and 2) into the device (RP2040: LittleFS files, ESP32: NVS). The ETS fields are project parameters
  that the script clears after a successful transfer; until then they are in the project (and in a ConfigTransfer
  export). RP2040: the files (`/WIFI.TXT`, `/WIFIAP.TXT`) are readable through the web file manager (`OPENKNX_WEBFS`) and, with OFM-FileTransferModule, an FTC download (reads are not access-controlled); documented, not blocked.
- Online scan: commands 3 (start) and 4 (state + up to 8 networks), shown on the ETS page with "Übernehmen".
- Own AP: WPA2 only, no client at the same time; the KNXnet/IP properties report the AP address.
- Regulatory domain: set `-D WIFICC=CYW43_COUNTRY_...` in the env (arduino-pico default is worldwide).
- Setup AP lifecycle: an unused fallback AP (no station for 5 min) leaves for a client attempt and comes back after 30 s without a link; switching prog mode on without a link opens it 5 s later, also on a programmed device (not when the ETS switched the setup AP off; only the switch-on arms it); it closes after 10 min without a station, on a configured device with the web server off by a restart (the AP started the server, which cannot be stopped). IP LED: pulsing (blue on RGB) while the setup AP runs, fast blinking in the 5 s before.
- Client rejoin: the RP2040 rejoins in the driver every 30 s without blocking while the client interface exists (the usual link loss; not while associated without an address, that is DHCP's/AutoIP's job; `connected()` also checks the interface's link flag, since during a join the core reports JOIN with the old address kept). Only when the interface is gone (failed first join, after an AP) does `begin()` rebuild it every 2 min, blocking 2 s, up to 4 s with DHCP, longer while the AP keeps timing out the key exchange. The ESP32 driver reconnects on its own.
- Page `/wifi` (`web/assets/wifi.css`, `wifi.js`): setup AP = the page alone without the menu; client = connection, RSSI history (10 min), gateway ping (1/s, only while the page is open, needs `OPENKNX_PING`), QR code of the page; own AP = state only. Routes `/wifi/status`, `/wifi/scan`, `/wifi/qr?t=` (QR from the qrcodegen library of OFM-DeviceDisplay, 404 when it is not in the build; fixed mask, `-D OPENKNX_QR_AUTO_MASK` for the best-scored one, ~15x slower; the last answer is cached), `/wifi/save`.
- Console: `net wifi` (state, signal, channel, address, reconnects, from the cached values), `net wifi reconnect` (RP2040 without blocking while the interface exists; closes an open setup AP), `net wifi scan`, `net wifi ap` (setup AP now, credentials kept, prog-button idle rule), `net wifi set<T>SSID<T>PSK`, `net wifi erase`; the former `wifi<T>SSID<T>PSK` and `erase wifi` are still accepted.
- LED function `WiFi-Status` (id 12, `OPENKNX_LEDFUNC_WIFI_STATE`, enum entry from NetworkWifi.share.xml): green / yellow / orange slow blink by signal level (good+ / fair / weak), red blink while rejoining, red without credentials or with an own AP that did not start, blue pulsing setup AP, blue fast blink in the 5 s before, cyan for the own AP (also during its channel scan). `Netzwerkstatus` (10) keeps the overall network state plus the blue setup-AP states.
- Onboard WiFi LED (`OPENKNX_WIFI_LED_PIN`, set by the hardware config, e.g. REG2 Pico W / Pico 2 W = `LED_BUILTIN` at the CYW43): fixed WiFi state, on with a link or an AP, blinking (500 ms) while joining or scanning, off without credentials and AP or when the own AP did not start. Written from the main-loop link tick on a change only, since each write is a command to the WiFi chip; no dimming, not an info LED.
- Display widget `WiFi` (`DEVICE_DISPLAY_MODULE`): client page; setup AP = QR code to join + info page, brought up once when the AP starts, if the rotation is on screen (not over the prog-mode screen); own AP page.
- 5 GHz prepared, off: memory holds band (2 bit) and channel (1 byte, 2.4 GHz 1-13 or 5 GHz 36-48). For 5 GHz hardware (e.g. ESP32-C5) set `<op:config name="%Wifi5GHz%" value="1" />` in the product XML (shows band and 5 GHz channel) and build with `-D OPENKNX_WIFI_5GHZ`; without the flag a 5 GHz selection falls back to 2.4 GHz. The 5 GHz radio setup itself is untested until such hardware exists.

## Webserver

The built-in HTTP/WebSocket server is documented in [`README.Webserver.md`](README.Webserver.md).

It also offers an optional **group monitor** (`OPENKNX_WEBMONITOR`) — an ETS-style live view of all KNX telegrams at `/groupmonitor`, available on devices with a TP connection (`MASK_VERSION == 0x07B0`). See [`README.Webserver.md`](README.Webserver.md).

## Web assets

CSS/JS/SVG/image files in a `web/assets/` folder are minified, gzip-compressed and embedded into the firmware (`include/webassets.h`) at build time, so the webserver serves them without a filesystem. The generator lives in `OGM-Common/scripts/pio/prepare.py` and runs only when a webserver is present (`OPENKNX_WEBSERVER`); it collects `web/assets/` from **any built module** (the `OFM-*` / `OGM-*` libraries) **and the application** (the OAM) — each opts in just by having the folder.

Supported: `.css .js .svg .jpg .jpeg .png`. `ota.css` becomes `WebAssets::ota_css_gz` + `ota_css_mime`; duplicate file names abort the build.

**Serving = getting it into the `.bin`.** An asset only ships if compiled code references its symbol — the linker (`--gc-sections`) drops unused ones. Register a route, gated by the feature's `#ifdef` so unused assets fall out automatically:

```cpp
addRoute(WEB_GET, "/assets/ota.css", Asset(WebAssets::ota_css_mime, WebAssets::ota_css_gz, sizeof(WebAssets::ota_css_gz)));
```

Each build prints what actually shipped, read from the ELF via `nm`:

```
MODULE_Network: firmware web assets
  base_css: 1016 B gz -> in firmware
  ota_css:  dropped (unused, linker-removed)   # embedded but no route references it
  total in firmware: 8/12 assets, 5668 B gz
```

`webassets.h` regenerates only when an asset changes — force it with `rm include/webassets.h` (`pio run -t clean` won't).

## MQTT

The built-in MQTT 3.1.1 client and broker is documented in [`README.MQTT.md`](README.MQTT.md).
