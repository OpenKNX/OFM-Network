@../OGM-Common/CLAUDE.md

---

# OFM-Network – Additional Instructions

## Project

**OFM-Network** is an **OpenKNX Function Module (OFM)** — a library, not a standalone application. It provides IP connectivity (WiFi & Ethernet), OTA updates, mDNS, NTP time sync, and ICMP ping for OpenKNX devices. It has no own `platformio.ini`; it is included as a library in parent OpenKNX device projects.

> **Not self-contained:** OFM-Network cannot be compiled or run on its own. The majority of the base infrastructure (module system, KNX stack integration, logging, flash storage, time API) lives in **OGM-Common**, located at `../OGM-Common` (one directory up). When tracing types like `OpenKNX::Module`, `OpenKNX::Log::Logger`, or `OpenKNX::Time::TimeProvider`, look there first.

- **Branch convention**: `v1` = stable release, `v1dev` = active development, feature branches merge into `v1dev`

---

## Hardware Platforms

| Platform | Connectivity | Stack | Compile Define |
|----------|-------------|-------|----------------|
| **ESP32** | WiFi | `WiFi.h` (Arduino IDF) | `KNX_IP_WIFI` |
| **ESP32** | Ethernet | `ETH.h` (Arduino IDF) | `KNX_IP_LAN` |
| **RP2040** (PicoW) | WiFi | arduino-pico (Earle Philhower) | `KNX_IP_WIFI` |
| **RP2040** | Ethernet | W5500lwIP (`W5500lwIP.h`) via SPI | `KNX_IP_LAN` |

- RP2040: lwIP single-thread (`NO_SYS=1`), no FreeRTOS
- ESP32: FreeRTOS; DNS must be called via TCPIP task callback
- W5500 SPI speed: `OPENKNX_NET_SPI_SPEED` (default 28 MHz), WiFi credentials in LittleFS `/WIFI.TXT`
- ESP32 WiFi credentials via `Preferences` API

---

## Architecture & Key Classes

### `NetworkModule` (`src/OpenKNX/Network/Module.h/.cpp`)
Main module, inherits `OpenKNX::Module`. Manages PHY init → IP init → mDNS → OTA → NTP, DHCP vs. static IP, WiFi credential management, network LED (`OPENKNX_LED_IP`), console commands, and callback registration for network changes.

### `PingHandler` (`src/OpenKNX/Network/Ping/Handler.*`)
Non-blocking ICMP ping with queue + parallel slots. Pending queue: unlimited FIFO. Active slots: `OPENKNX_PING_PARALLEL` (default 5). DNS resolved transparently before ping. `loop()` must be called from the Arduino loop.
- **ESP32**: BSD socket API (`SOCK_RAW`, `IPPROTO_ICMP`), non-blocking via `fcntl(O_NONBLOCK)`, FreeRTOS queue for thread-safe DNS
- **RP2040**: lwIP raw API (`raw_pcb`, `raw_sendto_if_src`), interrupt callback `icmpRecvCallback`

**Ping API (two variants):**
```cpp
// Standard — with RTT in callback
void ping(IPAddress|string target, void(IPAddress, bool, uint32_t rttMs) callback, uint32_t timeoutMs);

// Retry — bool-only callback, automatic retries
void ping(IPAddress|string target, void(IPAddress, bool) callback, uint8_t retries = 2, uint32_t timeoutMs);
```

### `NtpTimeProvider` (`src/OpenKNX/Network/NtpTimeProvider.h/.cpp`)
Inherits `OpenKNX::Time::TimeProvider`. ESP32: `esp_sntp_*` (non-blocking, smooth mode). RP2040: Arduino NTP library, sync every 3600 s. Server via `ParamNET_NTPServer` (default: `pool.ntp.org`).

### `OpenKNX::Network::MQTT::Module` (`src/OpenKNX/Network/MQTT/Module.h/.cpp`)
MQTT 3.1.1 client + broker, no external dependencies. Accessed via `openknxNetwork.mqtt`. Guard: `#ifdef OPENKNX_MQTT`. Setup is deferred to `loop()` when `established()` is true.
- **ESP32**: Broker/Client loop in FreeRTOS task (Core 0, `OPENKNX_MQTT_TASK_STACK` bytes). `publish()`/`subscribe()` mutex-protected.
- **RP2040**: Broker/Client loop in main `loop()`. Broker uses lwIP callbacks (`tcp_accept/recv/err/poll`) for data buffering only; all protocol processing in `loop()`.
- **Device prefix**: `openknx/<MQTTPrefix>/` (custom) or `openknx/<last8serialHex>/` (auto)
- **Built-in publishes**: `status` = "1" every 10 s, `uptime` = seconds; LWT sets `status` = "0"
- **Broker**: auth, retained messages, QoS 0+1, keep-alive, local observer subscriptions

---

## KNX Concepts & Integration

KNX parameters from `knxprod.h` via macros `ParamNET_*`. Definitions in `src/Network.share.xml`.

KNX IP properties: `PID_IP_ADDRESS`, `PID_SUBNET_MASK`, `PID_DEFAULT_GATEWAY`, `PID_IP_ASSIGNMENT_METHOD`, `PID_IP_CAPABILITIES` (value 6 = DHCP+AutoIP), routing multicast: `PID_ROUTING_MULTICAST_ADDRESS`.

Function property (object 160, property 5): transfers WiFi credentials from ETS via `Network.script.js`.

**KNX Parameters (`ParamNET_*`):**
```
ParamNET_StaticIP            – force static IP (inverted: 0 = DHCP)
ParamNET_HostAddress         – static IP address
ParamNET_SubnetMask          – subnet mask
ParamNET_GatewayAddress      – default gateway
ParamNET_NameserverAddress   – DNS server
ParamNET_CustomHostname      – enable custom hostname
ParamNET_HostName            – custom hostname (max 24 chars)
ParamNET_mDNS                – enable mDNS (default: on)
ParamNET_HTTP                – enable webserver
ParamNET_NTP                 – enable NTP client
ParamNET_NTPServer           – NTP server FQDN (default: pool.ntp.org, max 50 chars)
ParamNET_OTAUpdate           – OTA mode: 0=prog-mode only, 1=always, 2=disabled
ParamNET_LanMode             – LAN speed: 0=auto, 1=100 Mbit, 3=10 Mbit power-save
ParamNET_WifiSSID            – WiFi SSID (max 32 chars)
ParamNET_WifiPassword        – WiFi PSK (max 63 chars)
ParamNET_MQTT                – enable MQTT
ParamNET_MQTTMode            – mode: 0=Client, 1=Broker
ParamNET_MQTTServer          – broker FQDN or IP (Client mode, max 20 chars)
ParamNET_MQTTPort            – TCP port (default 1883)
ParamNET_MQTTUsername        – username (max 20 chars)
ParamNET_MQTTPassword        – password (max 20 chars)
ParamNET_MQTTCustomPrefix    – enable custom device prefix
ParamNET_MQTTPrefix          – custom prefix string (max 20 chars)
ParamNET_MQTTTPRawData       – publish KNX TP raw frames (ESP32 TP-UART, mask 0x07B0)
```

**Compile defines:**
```
OPENKNX_LED_IP               – LED for network status (optional)
OPENKNX_PING                 – enable ICMP ping handler
OPENKNX_PING_TIMEOUT         – ping timeout ms (default 1000)
OPENKNX_PING_PARALLEL        – max concurrent pings (default 5)
OPENKNX_NET_SPI_SPEED        – W5500 SPI speed Hz (default 28000000)
OPENKNX_WEBSERVER            – enable HTTP server
OPENKNX_WEBCONSOLE           – enable /console page + WS logger (requires WEBSERVER)
OPENKNX_WEBCONSOLE_BUFFER    – logger ring buffer size bytes (default 4096, max 65535)
OPENKNX_WEBFS                – enable FileManager (requires WEBSERVER)
OPENKNX_WEBSERVER_MAX_BODY   – max POST body: 4 KB (RP2040), 32 KB (ESP32)
OPENKNX_WEBSOCKET_MAX        – ESP32 only: max concurrent WebSocket connections (default 3, excess → HTTP 503)
OPENKNX_WEBSERVER_MAX_CONN   – RP2040 only: total webserver connection slots, shared HTTP+WS (default 3); raising requires more lwIP PCBs
OPENKNX_WEBSOCKET_RX_CAP     – per-WebSocket RX cap bytes (ESP32 accumulation buffer default 2048, overflow → disconnect; RP2040 payload buffer default 512, overflow → truncate)
OPENKNX_WEBMONITOR         – enable /groupmonitor page + WS telegram stream (requires WEBSERVER; TP only, gated on MASK_VERSION == 0x07B0)
HAS_USB                      – enable USB-Exchange support (RP2040)
OPENKNX_MQTT                 – enable MQTT client/broker
OPENKNX_MQTT_STATUS_TIME     – status publish interval ms (default 10000)
OPENKNX_MQTT_TASK_STACK      – ESP32 FreeRTOS task stack size bytes (default 4096)
OPENKNX_MQTT_BROKER_MAX_CLIENTS – max simultaneous broker clients (default 5 ESP32, 3 RP2040)
OPENKNX_MQTT_BROKER_MAX_RETAINED – max retained messages stored by broker (default 32)
OPENKNX_MQTT_BROKER_MAX_SUBS – max subscriptions per broker client (default 16)
OPENKNX_WEBCLIENT            – enable HTTP(S) client (openknxNetwork.webclient)
OPENKNX_WEBCLIENT_SLOTS      – parallel slots / queue depth (default 2)
OPENKNX_WEBCLIENT_TIMEOUT    – request timeout ms (default 10000)
OPENKNX_WEBCLIENT_TASK_STACK – ESP32 FreeRTOS task stack bytes (default 6144, SSL needs ~4 KB)
OPENKNX_WEBCLIENT_MAX_BODY   – max request body size bytes (default 4096)
```

---

## Additional Coding Conventions

- **Connectivity guards**: `#ifdef KNX_IP_WIFI` / `#ifdef KNX_IP_LAN`
- No external libraries in `library.json` — only Arduino framework builtins and OpenKNX core
- OTA port: ESP32 = 3232, RP2040 = 2040
- mDNS hostname: `OpenKNX-XXXXXXXX` (8 hex chars from serial number)

---

## Console Commands

```
net / n                        Show network info (IP, mask, gateway, DNS, WiFi RSSI)
net reset                      Reset PHY adapter
net mc [address|reset]         Get/set multicast address (KNX-IP only, mask 0x091A / 0x57B0)
ping <ip|hostname>             ICMP ping (2 s timeout in console)
wifi <SEP><SSID><SEP><PSK>     Set WiFi credentials (any delimiter, e.g. "wifi:mySSID:myPSK")
erase wifi                     Delete WiFi credentials (KNX_IP_WIFI only)
```

---

## Webserver

| Platform | Stack | Implementation |
|----------|-------|----------------|
| **ESP32** | FreeRTOS + esp-idf `httpd` | `Webserver_ESP32.cpp` |
| **RP2040** | lwIP `NO_SYS=1`, single-threaded | `Webserver_RP2040.cpp` |

### Built-in Routes

| Method | URI | Flag | Description |
|--------|-----|------|-------------|
| GET | `/` | — | Overview dashboard |
| GET | `/prog` | — | Prog-mode toggle (`?mode=0/1`) |
| GET | `/assets/base.css` | — | Base stylesheet (inline embedded) |
| GET | `/assets/base.js` | — | Base JavaScript (inline embedded) |
| GET | `/assets/logo/black.svg` | — | OpenKNX logo |
| GET | `/assets/favicon.svg` | — | Favicon |
| GET | `/console` | `WEBCONSOLE` | Web console UI |
| WS | `/console` | `WEBCONSOLE` | Logger streaming + commands |
| GET | `/filemanager` | `WEBFS` | Directory listing |
| GET | `/filemanager/download` | `WEBFS` | Streaming file download |
| POST | `/filemanager/upload` | `WEBFS` | Chunked file upload |
| POST | `/filemanager/delete` | `WEBFS` | Delete file |
| POST | `/filemanager/mkdir` | `WEBFS` | Create directory |
| GET | `/groupmonitor` | `GROUPMONITOR` | KNX group monitor UI (TP only) |
| WS | `/groupmonitor` | `GROUPMONITOR` | Live telegram stream (read-only) |

Wildcard routing supported: `/path/*` matches all sub-paths.

### Asset Management
```cpp
openknxNetwork.webserver.addStylesheet("/assets/custom.css");
openknxNetwork.webserver.addJavaScript("/assets/custom.js");
```
- `addStylesheet()` → inserted in `<head>` via `buildHeader()`
- `addJavaScript()` → inserted before `</body>` via `buildFooter()`, with `defer`
- Cache buster: automatic query param `YYYYMMDDHHII` from build time

### Web UI Pages — Style Guidelines
- **No inline CSS** — never use `style="..."` attributes in generated HTML
- Page-specific styles go into a dedicated `.css` file served from LittleFS (e.g. `/assets/filemanager.css`), registered via `addStylesheet()` in the page's route handler or module init
- Global styles that apply to all pages belong in the shared base stylesheet, not per-page assets
- Same rule for JavaScript: page-specific logic → own `.js` file via `addJavaScript()`, not `<script>` blocks inline in the HTML response

### Layout: `.container`
`buildHeader()` emits only `<nav>` + `<main>` — no wrapping container div. Normal pages wrap content in `<div class="container">` (max-width 960px). Only omit when explicitly building a fullscreen/flex-column page.

### WebRequest / WebResponse
- `req.getQueryParam(name)` — reads URL query parameter (URL-decoded)
- `res.sendStatic(...)` — points directly to flash, no RAM copy (RP2040-optimized)
- Body size limit: ESP32 = 32 KB, RP2040 = 4 KB → HTTP 413 on overflow

### Logger Ring Buffer
Lives in `OpenKNX::Log::Logger` (OGM-Common). Fills regardless of WS clients connected — new clients get full history.
- Entry format: `[uint32_t seq][uint16_t len][char text[len]]` = 6-byte header + payload
- Sentinel: remaining bytes zeroed (seq=0) when entry doesn't fit at end, `_logTail` resets to 0
- API: `openknx.logger.getLogEntryAfter(lastSeq, buf, size, &outSeq)`

### RP2040 Webserver (lwIP `NO_SYS=1`)
Callbacks (`onAccept`, `onRecv`, `onSent`, `onPoll`, `onErr`) must not block. `MAX_CONN = OPENKNX_WEBSERVER_MAX_CONN` connection slots (default 3, shared HTTP+WS), each with 1536 B RX buffer. WS state machine: `WR_HDR → WR_EXTLEN2/WR_EXTLEN8 → WR_MASK → WR_PAYLOAD`. WS payload buffer `OPENKNX_WEBSOCKET_RX_CAP` (default 512 B, truncated on overflow). Commands dequeued in `loop()` (not in `onRecv`). Only 1 pending command slot (`_pendingCmd[256]`).

### ESP32 Webserver
`httpd` in its own FreeRTOS task. **WebSockets use no extra tasks**: on upgrade the socket is detached from httpd via `httpd_req_async_handler_begin()`, then serviced non-blocking from `Webserver::loop()` (`recv(MSG_DONTWAIT)` → per-session `rx` accumulation buffer → `wsParseFrames()`), mirroring the RP2040 design. `onMessage` runs in the loop task; for the console it only buffers the command, `Webconsole::loop()` runs `processCommand()`. Concurrency capped at `OPENKNX_WEBSOCKET_MAX` (default 3, excess → HTTP 503); `lru_purge_enable` frees idle HTTP sockets. Shared state (`_socketClients` + session registry) guarded by recursive mutex (`wsStateLock/Unlock`); sends serialized by a TX mutex (`wsRawSend`/`wsSendText`, `MSG_DONTWAIT`). TCP keepalive: 3 s idle, 2 s interval, 3 probes.

### Web UI Console
No automatic reconnect — on disconnect shows `[Connection lost — reload page]`. ANSI colors → CSS classes via `ansiToHtml()`. Commands as WS text frames (opcode 0x01).

### Group Monitor (`OPENKNX_WEBMONITOR`, TP only)
`GroupMonitor` (`src/OpenKNX/Network/Webserver/GroupMonitor.{h,cpp}`) — live KNX telegram viewer, ETS-style. Read-only. Gated on `MASK_VERSION == 0x07B0` (device with TP connection).
- Taps **all** bus frames in parallel, bypassing the KNX stack, via `knx.bau().getDataLinkLayer()->getTPUart().registerReceivedFrame(...)` (same hook the MQTT raw-frame publish uses). Callback runs in loop context.
- **No own ring buffer / no `loop()`**: the frame callback decodes the `TPUart::Frame` (source IA, dest GA, group flag, APCI type Read/Write/Response, payload hex, TX/repeat flags) into compact JSON and broadcasts it directly via `webserver.sendWebsocketMessage("/groupmonitor", json)`. Slow-client drop is handled by the WS layer; new clients see telegrams from connect-time on (no backlog).
- Assets `/assets/groupmonitor.css` + `.js` served via `Static()` and registered with `addStylesheet()`/`addJavaScript()`; client-side timestamp, autoscroll/pause/clear, 1000-row cap.
- **Name/Value/DPT columns** from optional `/openknx_ga.tsv` (LittleFS, tab-separated: `ga\tdpt\tsubset\thauptgruppe\tmittelgruppe\tname`). Split of concerns:
  - **Backend** `GATable` (`src/OpenKNX/Network/GATable.{h,cpp}`, member `openknxNetwork.gatable`) loads the TSV once at setup into a compact POD index `addr → (dpt, sub)` (4 B/GA, sorted, binary search; PSRAM via `PsramAllocator` when available). **No strings in the index.** `getDpt(addr, dpt, sub)` is O(log n), no I/O. Same table is reusable by MQTT.
  - **Value** is decoded in `onFrame()` for Write/Response telegrams via the already-linked knx-stack `KNX_Decode_Value()` (`<knx/dptconvert.h>`) into a stack buffer (no heap), formatted for DPT 1/5/6/7/8/9/12/13/14/16/17/18 (5.001 → `%`). Sent as `val`; `dpt` as `"m.sss"`; numeric GA as `addr`.
  - **Names** are **not** held in the backend — the browser fetches the TSV itself (`/filemanager/download?path=%2Fopenknx_ga.tsv`) and maps `addr → name` client-side. File absent → Name/Value/DPT show `-`, no crash.

---

## References

- [README.md](README.md) — module overview and quick start
- [README.Webserver.md](README.Webserver.md) — webserver API reference
- [README.Ping.md](README.Ping.md) — ping handler usage
- [README.MQTT.md](README.MQTT.md) — MQTT client/broker usage
- [README.Webclient.md](README.Webclient.md) — HTTP(S) client usage
- [doc/Applikationsbeschreibung-Netzwerk.md](doc/Applikationsbeschreibung-Netzwerk.md) — full KNX application documentation
- [CHANGELOG.md](CHANGELOG.md) — version history

---

## Documentation Maintenance

After **any code change**, update all affected READMEs in the same response:

| Change type | Files to update |
|-------------|----------------|
| Public API / new feature | `README.md` + feature-specific README (e.g. `README.Webserver.md`, `README.Ping.md`, `README.MQTT.md`, `README.Webclient.md`) |
| Webserver routes, assets, layout | `README.Webserver.md` |
| Ping / ICMP behaviour | `README.Ping.md` |
| MQTT client/broker | `README.MQTT.md` |
| Webclient HTTP(S) | `README.Webclient.md` |
| KNX parameters, compile defines | `README.md` + `doc/Applikationsbeschreibung-Netzwerk.md` |
| Bug fix (no API change) | No README update required |

Rules:
- Keep READMEs in sync with the code — never leave them stale after a change
- Do not create new README files unless the user explicitly asks
- Do not add a "Changed in this session" or similar meta-section — just update the relevant content inline
