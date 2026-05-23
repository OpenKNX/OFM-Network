# OFM-Network – Claude Instructions

## Project

**OFM-Network** is an **OpenKNX Function Module (OFM)** — a library, not a standalone application. It provides IP connectivity (WiFi & Ethernet), OTA updates, mDNS, NTP time sync, and ICMP ping for OpenKNX devices. It has no own `platformio.ini`; it is included as a library in parent OpenKNX device projects.

- **Language**: C++17, Arduino framework
- **Developer level**: Advanced — no basics on Arduino, RP2040, ESP32, or networking needed. Get straight to the point.
- **Branch convention**: `v1` = stable release, `v1dev` = active development, feature branches merge into `v1dev`

---

## Hardware Platforms

| Platform | Connectivity | Stack | Compile Define |
|----------|-------------|-------|----------------|
| **ESP32** | WiFi | `WiFi.h` (Arduino IDF) | `KNX_IP_WIFI` |
| **ESP32** | Ethernet | `ETH.h` (Arduino IDF) | `KNX_IP_LAN` |
| **RP2040** (PicoW) | WiFi | arduino-pico (Earle Philhower) | `KNX_IP_WIFI` |
| **RP2040** | Ethernet | W5500lwIP (`W5500lwIP.h`) via SPI | `KNX_IP_LAN` |

- RP2040: **arduino-pico** by Earle Philhower, lwIP single-thread (`NO_SYS=1`), no FreeRTOS
- ESP32: FreeRTOS; DNS must be called via TCPIP task callback
- W5500 SPI speed: `OPENKNX_NET_SPI_SPEED` (default 28 MHz), WiFi credentials in LittleFS `/WIFI.TXT`
- ESP32 WiFi credentials via `Preferences` API

---

## Architecture & Key Classes

### `NetworkModule` (`src/NetworkModule.h/.cpp`)
Main module, inherits `OpenKNX::Module`. Manages PHY init → IP init → mDNS → OTA → NTP, DHCP vs. static IP, WiFi credential management, network LED (`OPENKNX_LED_IP`), console commands (`net`, `ping`, `wifi SSID PSK`, `erase wifi`, `net mc`), and callback registration for network changes.

### `PingHandler` (`src/OpenKNX/Network/PingHandler.*`)
Non-blocking ICMP ping with queue + parallel slots. Pending queue: unlimited FIFO. Active slots: `OPENKNX_PING_PARALLEL` (default 5). DNS resolved transparently before ping. Callback: `void(IPAddress target, bool reachable, uint32_t rttMs)`. `loop()` must be called from the Arduino loop.
- **ESP32**: BSD socket API (`SOCK_RAW`, `IPPROTO_ICMP`), non-blocking via `fcntl(O_NONBLOCK)`, FreeRTOS queue for thread-safe DNS
- **RP2040**: lwIP raw API (`raw_pcb`, `raw_sendto_if_src`), interrupt callback `icmpRecvCallback`

### `NtpTimeProvider` (`src/NtpTimeProvider.h/.cpp`)
Inherits `OpenKNX::Time::TimeProvider`. ESP32: `esp_sntp_*` (non-blocking, smooth mode). RP2040: Arduino NTP library, sync every 3600 s. Server via `ParamNET_NTPServer` (default: `pool.ntp.org`).

---

## KNX Concepts & Integration

OpenKNX modules implement: `init()`, `setup()`, `loop()`, `processCommand()`, `readFlash()`, `writeFlash()`.

KNX parameters from `knxprod.h` via macros `ParamNET_*`. Definitions in `src/Network.share.xml`.

KNX IP properties: `PID_IP_ADDRESS`, `PID_SUBNET_MASK`, `PID_DEFAULT_GATEWAY`, `PID_IP_ASSIGNMENT_METHOD`, `PID_IP_CAPABILITIES` (value 6 = DHCP+AutoIP), routing multicast: `PID_ROUTING_MULTICAST_ADDRESS`.

Function property (object 160, property 5): transfers WiFi credentials from ETS via `Network.script.js`.

**Compile defines:**
```
OPENKNX_LED_IP           – LED for network status (optional)
OPENKNX_PING_TIMEOUT     – Ping timeout ms (default 1000)
OPENKNX_PING_PARALLEL    – Max concurrent pings (default 5)
OPENKNX_NET_SPI_SPEED    – W5500 SPI speed Hz (default 28000000)
```

---

## Embedded Constraints

This code runs on microcontrollers with severe resource limits — treat every byte and cycle as precious:

- **RAM**: RP2040 has 264 KB total (shared with stack, heap, lwIP, KNX stack). ESP32 has ~320 KB free heap typical. No dynamic allocation in hot paths.
- **Flash**: Use `const` for read-only data — the linker places `.rodata` in flash automatically. `PROGMEM` does not exist on ESP32/RP2040. Avoid duplicating string literals.
- **No heap churn**: No `new`/`delete` or `std::string` construction in `loop()` — use fixed buffers, stack locals, or pre-allocated members.
- **No STL bloat**: Avoid `std::map`, `std::function`, `std::stringstream` — prefer arrays, raw function pointers, `snprintf`.
- **Stack depth**: RP2040 has a single stack (no RTOS). Keep recursion and large stack frames out of callbacks.
- **CPU**: Single-core RP2040 @ 125 MHz, no FPU on Cortex-M0+. Avoid float where integer math suffices.
- When in doubt: measure before adding, and prefer the smaller solution.

---

## Coding Conventions

- **No `delay()`** — everything non-blocking, state machines with `millis()`
- **Platform guards**: `#ifdef ARDUINO_ARCH_ESP32` / `#ifdef ARDUINO_ARCH_RP2040`
- **Connectivity guards**: `#ifdef KNX_IP_WIFI` / `#ifdef KNX_IP_LAN`
- No external libraries in `library.json` — only Arduino framework builtins and OpenKNX core
- Comments in German or English (mixed OK, but consistent per file)
- OTA port: ESP32 = 3232, RP2040 = 2040
- mDNS hostname: `OpenKNX-XXXXXXXX` (8 hex chars from serial number)

### Formatting (Allman style, `.clang-format`)
- **Always follow `.clang-format` exactly** — it is the authoritative style definition
- Curly braces always on their **own line** (classes, functions, `if`, `else`, `for`, `while`, `case`, `enum`, `struct`, `namespace`, `extern`)
- No column limit — no artificial line breaks
- 4 spaces indentation, no tabs
- `namespace` content indented, `case` labels indented
- `if` without braces only for a **single** statement (`OnlyFirstIf`) — no single-line `else`
- Preprocessor directives not indented
- Short functions, lambdas, enums, and `case` labels may stay on one line (as per `AllowShort*` rules)
- When writing new code, match the style of the surrounding file exactly

---

## Webserver

Compile flags:
- `OPENKNX_WEBSERVER` — enables HTTP server, routing, all routes
- `OPENKNX_WEBCONSOLE` — enables `/console` page, WS endpoint `/console`, logger ring buffer (requires `OPENKNX_WEBSERVER`)
- `OPENKNX_WEBCONSOLE_BUFFER` — ring buffer size bytes (default 4096, max 65535)

| Platform | Stack | Implementation |
|----------|-------|----------------|
| **ESP32** | FreeRTOS + esp-idf `httpd` | `Webserver_ESP32.cpp` |
| **RP2040** | lwIP `NO_SYS=1`, single-threaded | `Webserver_RP2040.cpp` |

### Asset Management
```cpp
openknxNetwork.webserver.addStylesheet("/assets/custom.css");
openknxNetwork.webserver.addJavaScript("/assets/custom.js");
```
- `addStylesheet()` → inserted in `<head>` via `buildHeader()`
- `addJavaScript()` → inserted before `</body>` via `buildFooter()`, with `defer`
- Cache buster: automatic query param `YYYYMMDDHHII` from build time
- `Webserver._stylesheets` / `Webserver._scripts` → `std::vector<std::string>`

### Layout: `.container`
`buildHeader()` emits only `<nav>` + `<main>` — no wrapping container div. Pages decide independently: wrap in `<div class='container'>` (max-width 960px) or emit directly into `<main>` (fullscreen, flex-column).

### Logger Ring Buffer
Lives in `OpenKNX::Log::Logger` (OGM-Common). Fills regardless of WS clients connected — new clients get full history.
- Type: byte-addressed ring buffer, variable entry length
- Entry format: `[uint32_t seq][uint16_t len][char text[len]]` = 6-byte header + payload
- Sentinel: remaining bytes zeroed (seq=0) when entry doesn't fit at end, `_logTail` resets to 0
- API: `openknx.logger.getLogEntryAfter(lastSeq, buf, size, &outSeq)`

### RP2040 Webserver (lwIP `NO_SYS=1`)
Callbacks (`onAccept`, `onRecv`, `onSent`, `onPoll`, `onErr`) must not block. 3 connection slots (`MAX_CONN = 3`), each with 1536 B RX buffer. `idx` set after `memset` via pointer arithmetic. WS state machine: `WR_HDR → WR_EXTLEN2/WR_EXTLEN8 → WR_MASK → WR_PAYLOAD`. Commands dequeued in `loop()` (not in `onRecv`) to avoid blocking USB-CDC. Only 1 pending command slot (`_pendingCmd[256]`).

### ESP32 Webserver
`httpd` in its own FreeRTOS task. `processCommand()` called directly in `wsFrameRecv`. `wsSendText()` uses `MSG_DONTWAIT`. `Webserver::loop()` is a no-op. TCP keepalive: 3 s idle, 2 s interval, 3 probes.

### Web UI Console
No automatic reconnect — on disconnect shows `[Connection lost — reload page]`. ANSI colors → CSS classes via `ansiToHtml()`. Commands as WS text frames (opcode 0x01).

---

## References

- [README.md](README.md) — module overview and quick start
- [README.Webserver.md](README.Webserver.md) — webserver API reference
- [README.Ping.md](README.Ping.md) — ping handler usage
- [doc/Applikationsbeschreibung-Netzwerk.md](doc/Applikationsbeschreibung-Netzwerk.md) — full KNX application documentation
- [CHANGELOG.md](CHANGELOG.md) — version history
- [AGENTS.md](AGENTS.md) — extended agent instructions (source of this content)

---

## Documentation Maintenance

After **any code change**, update all affected READMEs in the same response:

| Change type | Files to update |
|-------------|----------------|
| Public API / new feature | `README.md` + feature-specific README (e.g. `README.Webserver.md`, `README.Ping.md`) |
| Webserver routes, assets, layout | `README.Webserver.md` |
| Ping / ICMP behaviour | `README.Ping.md` |
| KNX parameters, compile defines | `README.md` + `doc/Applikationsbeschreibung-Netzwerk.md` |
| Bug fix (no API change) | No README update required |

Rules:
- Keep READMEs in sync with the code — never leave them stale after a change
- Do not create new README files unless the user explicitly asks
- Do not add a "Changed in this session" or similar meta-section — just update the relevant content inline
