# OFM-Network – Agent Instructions

## Project Context

**OFM-Network** is a network function module in the **OpenKNX** ecosystem — an open-source framework for KNX building automation on embedded hardware. The module provides IP connectivity (WiFi & Ethernet), OTA updates, mDNS, NTP time sync, and ICMP ping for OpenKNX devices.

- **Branch convention**:
  - `v1` – public release branch (stable)
  - `v1dev` – active development branch
  - Additional branches (e.g. `v1dev-ping`) are feature or fix branches merged into `v1dev`
- **Language**: C++17, Arduino framework
- **Developer expertise**: Advanced level in Arduino, RP2040 (arduino-pico by Earle Philhower), ESP32, and networking — do not explain the basics of these platforms, get straight to the point.

Full documentation: [`doc/Applikationsbeschreibung-Netzwerk.md`](doc/Applikationsbeschreibung-Netzwerk.md)

---

## Hardware Platforms

| Platform | Connectivity | Stack | Compile Define |
|----------|-------------|-------|----------------|
| **ESP32** | WiFi | `WiFi.h` (Arduino IDF) | `KNX_IP_WIFI` |
| **ESP32** | Ethernet | `ETH.h` (Arduino IDF) | `KNX_IP_LAN` |
| **RP2040** (PicoW) | WiFi | arduino-pico by Earle Philhower | `KNX_IP_WIFI` |
| **RP2040** | Ethernet | W5500lwIP (`W5500lwIP.h`) via SPI | `KNX_IP_LAN` |

**Important platform notes:**
- RP2040 uses **arduino-pico** by Earle Philhower (not the official Arduino Mbed Core) — lwIP in single-thread mode (`NO_SYS=1`), no FreeRTOS
- ESP32 has FreeRTOS; DNS must be called via the TCPIP task callback (see `PingHandler_ESP32.cpp`)
- W5500 SPI speed: `OPENKNX_NET_SPI_SPEED` (default 28 MHz), credentials in LittleFS (`/WIFI.TXT`)
- ESP32 WiFi credentials via `Preferences` API

---

## Architecture & Key Classes

### `NetworkModule` (`src/NetworkModule.h/.cpp`)
Main module, inherits from `OpenKNX::Module`. Manages:
- PHY init (`initPhy`) → IP init (`initIp`) → mDNS → OTA → NTP
- DHCP vs. static IP (from KNX parameters)
- WiFi credential management (save/read/erase)
- Network state LED via `OpenKNX::Led::FunctionGroup` (when `OPENKNX_LED_IP` is defined)
- Console commands: `net`, `net reset`, `ping`, `wifi SSID PSK`, `erase wifi`, `net mc`
- Callback registration for network changes (`registerCallback`)

### `PingHandler` (`src/OpenKNX/Network/PingHandler.*`)
Non-blocking ICMP ping with queue + parallel slots:
- **Pending queue**: unlimited, FIFO
- **Active slots**: `OPENKNX_PING_PARALLEL` (default 5) concurrent pings
- DNS resolution transparently before ping
- Callback signature: `void(IPAddress target, bool reachable, uint32_t rttMs)`
- `loop()` must be called from the Arduino loop
- Platform implementations:
  - **ESP32**: BSD socket API (`SOCK_RAW`, `IPPROTO_ICMP`), non-blocking via `fcntl(O_NONBLOCK)`, FreeRTOS queue for thread-safe DNS
  - **RP2040**: lwIP raw API (`raw_pcb`, `raw_sendto_if_src`), interrupt callback `icmpRecvCallback`

### `NtpTimeProvider` (`src/NtpTimeProvider.h/.cpp`)
- Inherits from `OpenKNX::Time::TimeProvider`
- ESP32: `esp_sntp_*` API (non-blocking, smooth mode)
- RP2040: Arduino NTP library, sync every 3600 s
- NTP server via KNX parameter `ParamNET_NTPServer` (default: `pool.ntp.org`)

### `OpenKNX::Network::MQTT::Module` (`src/OpenKNX/Network/MQTT/`)
MQTT 3.1.1 client + broker, no external dependencies. Guard: `#ifdef OPENKNX_MQTT`. Accessed as `openknxNetwork.mqtt`.

**Files:**
- `Packet.h/.cpp` — MQTT 3.1.1 packet serialization/deserialization, topic matching (`+`/`#` wildcards)
- `Client.h/.cpp` — connects to an external MQTT broker
- `Broker.h/.cpp` — embedded MQTT broker (auth, retained messages, QoS 0+1, keep-alive)
- `Module.h/.cpp` — OpenKNX::Module subclass with public publish/subscribe API

**Platform behaviour:**
- **ESP32**: Broker/Client loop in FreeRTOS task (Core 0). `publish()` and `subscribe()` are mutex-protected.
- **RP2040**: Broker/Client loop in main `loop()` (NO_SYS=1). lwIP callbacks (`tcp_accept/recv/err/poll`) only buffer data — all protocol processing in `loop()`.

**Setup lifecycle:** Deferred — `mqtt.setup()` is called from `Network::Module::loop()` only when `established()` is true.

**API:**
```cpp
openknxNetwork.mqtt.publish("abs/topic", payload, len, qos, retain);
openknxNetwork.mqtt.publishP("rel/topic", payload);  // prepends devicePrefix()
openknxNetwork.mqtt.subscribe("openknx/+/cmd", callback, qos);
openknxNetwork.mqtt.connected();
openknxNetwork.mqtt.devicePrefix();  // e.g. "openknx/abc12345/"
```

Full reference: [`README.MQTT.md`](README.MQTT.md)

Full reference: [`README.MQTT.md`](README.MQTT.md)

---

## KNX Concepts & Integration

**OpenKNX modules** implement an interface with: `init()`, `setup()`, `loop()`, `processCommand()`, `readFlash()`, `writeFlash()`.

**KNX parameters** are read from the generated `knxprod.h` via macros like `ParamNET_*`. Parameter definitions are in [`src/Network.share.xml`](src/Network.share.xml).

**KNX IP properties** (BAU interface):
- `PID_IP_ADDRESS`, `PID_SUBNET_MASK`, `PID_DEFAULT_GATEWAY`, `PID_IP_ASSIGNMENT_METHOD`
- `PID_IP_CAPABILITIES` → advertise DHCP+AutoIP (value: 6)
- Routing multicast: `PID_ROUTING_MULTICAST_ADDRESS` (mask 0x091A / 0x57B0)

**Function property** (object 160, property 5): transfers WiFi credentials from ETS via `Network.script.js`.

**Compile defines (important):**
```
OPENKNX_LED_IP          – LED for network status (optional)
OPENKNX_PING_TIMEOUT    – Ping timeout in ms (default 1000)
OPENKNX_PING_PARALLEL   – Max concurrent pings (default 5)
OPENKNX_NET_SPI_SPEED   – W5500 SPI speed Hz (default 28000000)
OPENKNX_MQTT            – Enable MQTT client/broker
OPENKNX_MQTT_STATUS_TIME     – Status publish interval ms (default 10000)
OPENKNX_MQTT_TASK_STACK      – ESP32 task stack bytes (default 4096)
OPENKNX_MQTT_BROKER_MAX_CLIENTS – Max broker clients (default 5/3)
```

---

## Coding Conventions

- **No `delay()`** — everything non-blocking, state machines with `millis()` checks
- **Platform guards**: `#ifdef ARDUINO_ARCH_ESP32` / `#ifdef ARDUINO_ARCH_RP2040`
- **Connectivity guards**: `#ifdef KNX_IP_WIFI` / `#ifdef KNX_IP_LAN`
- No external libraries in `library.json` — only Arduino framework builtins and OpenKNX core
- Comments in German or English (mixed is OK, but consistent per file)
- OTA port: ESP32 = 3232, RP2040 = 2040 (both platforms support OTA, implementation is identical)
- mDNS hostname: auto-generated as `OpenKNX-XXXXXXXX` (8 hex chars from serial number)

### Formatting (`.clang-format`)

Based on **LLVM style** with the following deviations — strictly follow when generating code:

- **Curly braces** always on their **own line** (Allman style) — applies to classes, functions, `if`, `else`, `for`, `while`, `case`, `enum`, `struct`, `namespace`, `extern`
- **No column limit** (`ColumnLimit: 0`) — no artificial line breaks
- **4 spaces** indentation, **no tabs** (`UseTab: Never`)
- `namespace` content is indented (`NamespaceIndentation: All`)
- `case` labels are indented (`IndentCaseLabels: true`)
- Short `case` labels, enums, lambdas, and functions may remain single-line
- `if` without braces only allowed for a **single** statement (`OnlyFirstIf`) — no single-line `else`
- Pointer alignment derived from existing code (`DerivePointerAlignment: true`)
- Preprocessor directives **not** indented (`IndentPPDirectives: None`)

---

## Build & Test

No standalone `platformio.ini` — the module is included as a library in parent OpenKNX device projects. `platformio.network.ini` is empty (template for projects).

For testing: integrate into an OpenKNX device project that references this module.

---

## Webserver

Full API reference: [`README.Webserver.md`](README.Webserver.md)

### Compile Flags

| Flag | Effect |
|------|--------|
| `OPENKNX_WEBSERVER` | Enables HTTP server, routing, overview page, logo asset, all routes |
| `OPENKNX_WEBCONSOLE` | Enables `/console` page, WS endpoint `/console`, **and** the logger ring buffer in OGM-Common |
| `OPENKNX_WEBCONSOLE_BUFFER` | Total ring buffer size in bytes (optional, default: 4096, max: 65535) |

`OPENKNX_WEBCONSOLE` requires `OPENKNX_WEBSERVER`.
Without `OPENKNX_WEBSERVER` no webserver code is compiled at all — no class, no stubs, no `webserver` member in the module.

### Platform Support

| Platform | Stack | Implementation |
|----------|-------|----------------|
| **ESP32** | FreeRTOS + esp-idf `httpd` | `Webserver_ESP32.cpp` |
| **RP2040** | lwIP `NO_SYS=1`, single-threaded | `Webserver_RP2040.cpp` |

### Asset Management (`addStylesheet` / `addJavaScript`)

The webserver provides a flexible system for managing CSS and JavaScript assets:

**API:**
```cpp
openknxNetwork.webserver.addStylesheet("/assets/custom.css");
openknxNetwork.webserver.addJavaScript("/assets/custom.js");
```

**Behavior:**
- `addStylesheet()` → registers URI, inserted in `<head>` (via `buildHeader()`)
- `addJavaScript()` → registers URI, inserted before `</body>` (via `buildFooter()`)
- **Cache buster**: automatic query parameter `YYYYMMDDHHII` (e.g. `?202605231435`) from build time
- **Defer attribute**: JavaScript tags receive `defer` for non-blocking execution
- `base.css` and `base.js` are registered via these functions during `setup()`

**Implementation:**
- `Webserver._stylesheets` → `std::vector<std::string>` iterated in `buildHeader`
- `Webserver._scripts` → `std::vector<std::string>` iterated in `buildFooter`

### Layout Convention: `.container`

`buildHeader()` emits only `<nav>` + `<main>` — no wrapping `<div class='container'>`.
Each page decides independently whether to use the container div.

- **With container** (`max-width: 960px`): wrap page content in `<div class='container'>...</div>`
- **Without container** (fullscreen): emit content directly into `main`; `main` is a flex-column (`flex:1`), so direct children with `flex:1` fill the full height. No C++ flag needed — controlled purely via CSS.

### Logger Ring Buffer (`OPENKNX_WEBCONSOLE`)

The ring buffer lives in `OpenKNX::Log::Logger` (OGM-Common), **not** in the webserver. It fills regardless of whether WebSocket clients are connected — new clients automatically receive the history.

- **Type**: byte-addressed ring buffer with variable entry length (no fixed slots)
- **Size**: `OPENKNX_WEBCONSOLE_BUFFER` bytes total (default: 4096)
- **Entry format**: `[uint32_t seq][uint16_t len][char text[len]]` = `LOG_ENTRY_HDR`(6) + len bytes
- **Sentinel**: when a new entry no longer fits at the end, the remaining bytes are zeroed (seq=0 = sentinel) and `_logTail` resets to 0
- **Overflow**: when a new entry would overwrite the head, old entries are evicted from `_logHead`
- **Sequence numbers**: monotonically increasing (`_logNextSeq`), starting at 1; seq=0 is reserved for sentinels/unwritten areas
- **Per-line accumulator**: `_lineAccum[LOG_LINE_MAX]` (256 chars) — filled on every log call and committed to the buffer after `\n` via `commitLineToRing()`
- **API**: `openknx.logger.getLogEntryAfter(lastSeq, buf, size, &outSeq)` — returns the first entry with `seq > lastSeq`; iterates from `_logHead`, skips sentinels automatically

**Logger implementation:**
- `beforeLog()`: sets `_lineAccumLen = 0`
- Every print call (`printMessage`, `printPrefix`, `printTimestamp`, etc.) also calls `appendWebconsoleBuffer()`
- `afterLog()`: appends `\n`, calls `commitLineToRing()`

### RP2040 Architecture (lwIP `NO_SYS=1`)

Critical characteristic: lwIP runs single-threaded. Callbacks (`onAccept`, `onRecv`, `onSent`, `onPoll`, `onErr`) must **not block** and must not perform long operations.

**Connection slots:**
```
static constexpr int MAX_CONN = 3;
static ConnSlot g_slots[MAX_CONN];
```

Each `ConnSlot` holds: HTTP RX buffer (1536 B), HTTP TX pointer, WS state machine state, `wsSentSeq` (when `OPENKNX_WEBCONSOLE`).

**Important:** `onAccept` calls `memset(s, 0, sizeof(*s))`, which clears `s->idx`. Therefore `idx` is set **after** the memset via pointer arithmetic:
```cpp
int slotIdx = (int)(s - g_slots);
memset(s, 0, sizeof(*s));
s->idx = slotIdx;
```

**WS state machine** (`wsProcessData`):
```
WR_HDR → WR_EXTLEN2 / WR_EXTLEN8 → WR_MASK → WR_PAYLOAD
```
State is fully held in `ConnSlot` — no global parser state.

**TCP send:** `wsTcpSend()` checks `tcp_sndbuf()` — if the buffer is full (slow client) the frame is dropped rather than blocking. The connection remains open.

**Ping/keepalive:** `onPoll` sends a WS ping (opcode 0x09) every ~60 s. If the browser does not respond, lwIP's retransmit timeout detects the dead socket and fires `onErr`.

### Command Processing on RP2040 (Decoupling from lwIP Callbacks)

**Problem:** `processCommand()` calls many log functions that generate serial TX output. Inside an lwIP callback (`onRecv`) this would block the USB-CDC FIFO and truncate the output.

**Solution:** Queue-and-defer pattern:
- The WS message handler (running in `onRecv`) only writes to `_pendingCmd[256]` and sets `_hasPendingCmd = true`
- `Webserver::loop()` (clean stack, outside all lwIP callbacks) reads `_pendingCmd`, clears the flag, then calls `processCommand()`

```cpp
// in loop():
if (_hasPendingCmd) {
    _hasPendingCmd = false;
    logInfo("", "> %s", _pendingCmd);
    openknx.console.processCommand(_pendingCmd);
}
```

**Limitation:** There is only one `_pendingCmd` slot. With multiple simultaneous WS clients the last received message overwrites the previous one. This is sufficient for a single console session.

**Binary frame filter:** WS frames from the browser containing bytes outside printable ASCII (`0x20–0x7E`, `\r`, `\n`, `\t`) are silently discarded. Protects against garbage from malformed or protocol-internal frames (e.g. browser reconnect artifacts).

### History for New WebSocket Clients

New clients connect with `wsSentSeq = 0` (automatically zeroed by `memset` in `onAccept`). `loop()` calls `getLogEntryAfter(0, ...)` and sends all ring buffer entries sequentially. No special "history dump" code needed — the normal drain loop covers it.

Per-client tracking: `ConnSlot.wsSentSeq` holds the last successfully sent sequence number. `loop()` sends as many entries per tick as fit in the TCP buffer (breaks on `wsTcpSend() == false`, retries next tick).

### ESP32 Architecture

- `httpd` runs in its own FreeRTOS task
- Per WS session, `wsFrameRecv` installs itself as `httpd_sess_set_recv_override` — runs in an endless loop in the httpd task
- `processCommand()` is called directly in `wsFrameRecv` (no dequeue needed, FreeRTOS has its own stack)
- `wsSendText()` uses `MSG_DONTWAIT` — never blocks; EAGAIN = frame dropped
- `Webserver::loop()` is a no-op on ESP32 (the httpd task handles everything)
- TCP keepalive: 3 s idle, 2 s interval, 3 probes → dead connections detected after ~9 s

**Note:** ESP32 currently does not drain the logger ring buffer from `loop()`. On ESP32, logger integration works via `sendWebsocketMessage` from a callback — or can be switched to ring-buffer drain via `loop()` in the future.

### Web UI: Console Page

- **No automatic reconnect** — on connection loss `[Connection lost — reload page]` is shown and the page must be reloaded manually
- **Reason:** Automatic reconnect caused race conditions in log output and made it harder to debug disconnect causes
- ANSI colors are converted to CSS classes by `ansiToHtml()` (`red`, `green`, `yellow`, `gray`)
- Commands are sent as text frames (WS opcode 0x01), responses come back as ring buffer drain

---

Changelog: [`CHANGELOG.md`](CHANGELOG.md)
