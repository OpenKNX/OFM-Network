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

> **Not self-contained:** OFM-Network cannot be compiled or run on its
> own. The majority of the base infrastructure (module system, KNX
> stack integration, logging, flash storage, time API) lives in
> **OGM-Common**, at `../OGM-Common/AGENTS.md` (one directory up) —
> conventions and Claude skills documented there apply here too. When
> tracing types like `OpenKNX::Module`, `OpenKNX::Log::Logger`, or
> `OpenKNX::Time::TimeProvider`, look there first.

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

**KNX-IP datalink gating (`controlKnxIp`)** — a mask-dependent one-liner over the stack: `getPrimaryDataLinkLayer()->enabled()` on `0x091A`, `getDataLinkLayer()->enabled()` on `0x57B0`, and an **empty body on every other mask** (so the whole mechanism is inert on TP-only devices). `IpDataLinkLayer::enabled(bool)` joins/leaves the KNX routing multicast group (`setupMultiCast`/`closeMultiCast`) and gates `sendBytes()`; it is guarded by its own `_enabled` flag, so repeated calls with the same value are no-ops. Two consequences worth knowing:

- **The multicast address is read from the property at `enabled(true)`**, and the socket binds to the IP present at that moment. A rebind therefore requires a full `false` → `true` cycle; a second `enabled(true)` alone does nothing.
- **A rebind costs telegrams**, so it must happen on a real IP change and only then. `enabled(true)` on an already-enabled layer is a no-op, so the socket used to keep the *old* binding after an IP change — silently off the bus until a restart. `_boundIp` holds the IP the socket is bound to, making the `GOT_IP` handler able to tell a real change from a plain lease renewal; it is cleared inside `controlKnxIp(false)` itself, deliberately not at the call sites, so "socket closed ⇒ binding invalid" cannot be forgotten.
- **The reconcile against `established()` (`connected() && localIP() != 0`) in `checkLinkStatus()` is ESP32-only, on purpose.** There the datalink hangs off the event chain, and a link flap in which `GOT_IP` never re-fires leaves it off for good; the 500 ms reconcile closes that gap for two comparisons per tick. RP2040 has no such events and therefore no gap — `knx.start()` enables the datalink once and it stays enabled. Gating it there would newly make KNX-IP depend on `isLinked()`/`isConnected()`, where a single false negative takes the device off the bus for at least 500 ms, with no demonstrated problem being solved.

### `PingHandler` (`src/OpenKNX/Network/PingHandler.*`)
Non-blocking ICMP ping with queue + parallel slots:
- **Pending queue**: unlimited, FIFO
- **Active slots**: `OPENKNX_PING_PARALLEL` (default 5) concurrent pings
- DNS resolution transparently before ping
- Callback signature: `void(IPAddress target, bool reachable, uint32_t rttMs)`
- `loop()` must be called from the Arduino loop
- Platform implementations:
  - **ESP32**: BSD socket API (`SOCK_RAW`, `IPPROTO_ICMP`), non-blocking via `fcntl(O_NONBLOCK)`, FreeRTOS queue for thread-safe DNS. All slots share **one** socket, so `platformCheckReply()` drains it completely and files every echo reply under its own slot (the ICMP `id` carries the slot index) instead of discarding whatever doesn't match the slot being polled — otherwise parallel pings lose each other's replies and run into false timeouts.
  - **RP2040**: lwIP raw API (`raw_pcb`, `raw_sendto_if_src`), interrupt callback `icmpRecvCallback`

### `NtpTimeProvider` (`src/NtpTimeProvider.h/.cpp`)
- Inherits from `OpenKNX::Time::TimeProvider`
- ESP32: `esp_sntp_*` API (non-blocking, smooth mode)
- RP2040: Arduino NTP library, sync every 3600 s
- NTP server via KNX parameter `ParamNET_NTPServer` (default: `pool.ntp.org`)

### `OpenKNX::Network::MQTT::Module` (`src/OpenKNX/Network/MQTT/`)
MQTT 3.1.1 client + broker, no external dependencies. Guard: `#ifdef OPENKNX_MQTT`. Accessed as `openknxNetwork.mqtt`.

`Packet` (wire format), `Client` (external broker), `Broker` (embedded), `Module` (public API). Broker/Client run in a FreeRTOS task on ESP32, in `loop()` on RP2040; `setup()` is deferred until `established()`.

**Rules that are not visible at the call site** — full reference in [`README.MQTT.md`](README.MQTT.md):

- **`publish()`/`publishP()` queue, they never write the socket.** `true` means *accepted*, not delivered. That is what makes them safe from timing-critical code such as the KNX loop — never publish inline.
- **`connected()` is a mirrored flag.** Never reach into `_broker`/`_client` from a caller; that would take `_mutex` from the wrong task.
- **`setup()` completes even when `begin()` fails**, otherwise it re-runs — with its log block — every tick. Only pre-`begin()` failures (`mqttTxReserve()` OOM) return early to be retried.
- **`mqttTxBuf` grows on demand**: pass `mqttTxBufSize` to the `build*()` functions, never a literal; grow only in the send path, never in `enqueue()`.
- **DNS via `OpenKNX::Network::DNS::query()`**, never `dns_gethostbyname()`. Its callback can fire *inline*, so nothing may touch client state after the call.
- `_mutex` is **recursive** — a subscription callback may re-enter `subscribe()` from the MQTT task.
- **The KNX-telegram feature is deliberately not here.** `MQTT::Module` knows nothing about KNX frames; `ParamNET_MQTTTPRawData` lives in `Network::Module::publishTelegram()` and just calls `mqtt.publish()` like any other caller.

### `OpenKNX::Format::JSON` (`src/OpenKNX/Format/JSON/`)

`Writer.h/.cpp` and `Reader.h/.cpp` — **use these instead of assembling JSON by hand.** `Writer` is a fluent builder (`beginObject()`/`field(k, v)`/`endObject()`, overloads for `const char*`, `std::string`, `int32_t`, `uint32_t`, `float`, `bool`, plus `null()`); it escapes keys and values itself, so no manual quoting at call sites. `reset()` clears the content but keeps the internal `std::string` capacity — a `Writer` held as a member and reset per message therefore stops allocating once warmed up, which is why `GroupMonitor` and `MQTT::Module` each keep one for their per-telegram broadcast.

### `OpenKNX::Network::TelegramJson` (`src/OpenKNX/Network/TelegramJson.h/.cpp`)

`buildTelegramJson(TPUart::Frame&, JSON::Writer&)` decodes one TP telegram, shared by `GroupMonitor` (WebSocket) and `Network::Module::publishTelegram()` (MQTT) so both payloads stay identical by construction. Field reference: [`README.MQTT.md`](README.MQTT.md). Caller does `frame.isFrame()` and `json.reset()`. JSON only — reaching the TP-UART is `Network::Module::tpUart()`'s job.

- **Guard `OPENKNX_TELEGRAMJSON`**, defined by `TelegramJson.h` itself. Everything decoder-dependent keys off that one macro — `gatable`, the `groupmonitor` member, the `OPENKNX_CHARSET` auto-define — instead of repeating the condition. The header is included unconditionally, being pure preprocessor when off.
- `hex` is the APDU **from the APCI octet on**, so its first byte is not payload. `raw` is the whole frame.
- Don't use `frame.human*()` for addresses: zero-pads and returns `std::string` by value.

**`Network::Module::tpUart()`** abstracts the BAU accessor difference: `Bau07B0` via `getDataLinkLayer()`, `Bau091A` via `getSecondaryDataLinkLayer()` (there `getPrimaryDataLinkLayer()` is the IP side). `Bau57B0` has no TP-UART, so nothing here compiles in for it. Never touch `knx.bau()` directly for this.

`GATable` (member `openknxNetwork.gatable`) maps GA → DPT from `/openknx_ga.tsv` (LittleFS): sorted POD vector, 4 B per GA, **no strings**, binary search, `begin()` idempotent and called every tick from `loop()`. Value decode uses the knx stack's `KNX_Decode_Value()`.

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
OPENKNX_MQTT_BROKER_MAX_RETAINED – Max retained messages in broker (default 32)
OPENKNX_MQTT_BROKER_MAX_SUBS – Max subscriptions per broker client (default 16)
OPENKNX_WEBSOCKET_TX_BUFFER  – WS send queue, one ring for all clients (default 4096)
OPENKNX_WEBSERVER_MAX_CONN   – Webserver slots, HTTP + WS share them (ESP32 7, RP2040 3, or 6 with -D__LWIP_MEMMULT=2)
OPENKNX_WEBSERVER_RX_CAP     – RP2040: static buffer per connection slot, HTTP headers *and* WS payloads (default 1024)
OPENKNX_WEBSOCKET_RX_CAP     – ESP32: heap buffer per WS session, WS only (httpd buffers HTTP itself) (default 2048)
```

---

## Coding Conventions

- **No `delay()`** — everything non-blocking, state machines with `millis()` checks
- **Platform guards**: `#ifdef ARDUINO_ARCH_ESP32` / `#ifdef ARDUINO_ARCH_RP2040`
- **Connectivity guards**: `#ifdef KNX_IP_WIFI` / `#ifdef KNX_IP_LAN`
- No external libraries in `library.json` — only Arduino framework builtins and OpenKNX core
- **Comments in English.** Older files still carry German comments; convert them when you touch the surrounding code rather than in a sweep of their own
- Comment sparingly and briefly — one line for the non-obvious *why*, nothing more. Long rationale blocks go stale faster than the code they sit next to; put them in the commit message or a README instead
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

### Web Assets (`web/assets/`, generated `webassets.h`)

All CSS/JS/SVG served under `/assets/*` are **not** hand-minified C++ string literals — they live as clean, readable source files in `web/assets/` at the repo root. `OGM-Common/scripts/pio/prepare.py` runs on every OAM (device project) build, collects a `web/assets/` folder from every included module **and the project itself**, minifies + gzip-compresses each file, and writes `include/webassets.h`. This module is documented here in depth as the first consumer of that generator; `OGM-Common/AGENTS.md` only notes that it exists.

- **Files**: `web/assets/base.css`, `base.js`, `favicon.svg`, `logo.svg`, `console.css`, `console.js`, `groupmonitor.css`, `groupmonitor.js`, `filemanager.css`, `filemanager.js` — flat, no subfolder. The module's own repo root is already the namespace; a per-module prefix inside `web/assets/` would be redundant.
- **Processing**: `.css`/`.js`/`.svg` are minified (whitespace/comments stripped, conservatively per format — JS deliberately does **not** strip `//` comments via regex, since a comment can't be reliably told apart from one inside a string/regex literal that way) and then gzip-compressed (`compresslevel=9`, `mtime=0` for reproducible bytes). `.jpg`/`.jpeg`/`.png` are gzip-compressed **only** — minifying an already-compressed binary format is meaningless, and the real-world gzip gain there is ~0%.
- **Identifiers**: flat, **no** module prefix — derived from the relative path under `web/assets/` (`/` and `.` → `_`, e.g. `base.css` → `base_css`). The module itself is already the namespace (one repo = one `web/assets/` root); an extra prefix would be redundant.
- **No duplicates, no exception**: if an identifier collides between two sources — including between a module and the project itself — the build aborts with an error (`raise SystemExit(1)`). No override, not even for the project.
- **No file without assets**: if no `web/assets/` folder exists anywhere in a build, `include/webassets.h` is neither generated nor left over from a previous build (removed if stale) — code referencing `WebAssets::*` then fails to compile, same as any other missing generated header (`versions.h`, `knxprod.h`).
- **Generated symbols** per file, in `namespace WebAssets`:
  ```cpp
  inline const uint8_t base_css_gz[] = { /* gzip bytes */ };
  inline const char* const base_css_mime = "text/css"; // from the generator's one MIME table
  ```
  **Deliberately no separate `_gz_len` constant** — the size is already known at the call site via `sizeof(WebAssets::base_css_gz)` (fixed-bound array), a manually-tracked extra number would be redundant. Important: the data is **not** NUL-terminated — `strlen()` must never be used here, gzip bytes are binary and virtually always contain a `0x00` somewhere in the stream; a `strlen()`-based length would truncate the content at a random position.
- **Every asset is global**: `addStylesheet()`/`addJavaScript()` put the tag on *every* page, there is no per-page registration. A page-specific stylesheet must therefore scope any `body`/`main`/element-selector rule to its own page (`main:has(#gm-table) { … }`), and a page-specific script must bail out early when its root element is missing (`if (!body) return;`) — otherwise it runs on the overview page too and, in the case of the monitor/console scripts, opens a WebSocket there and burns a connection slot.
- **Registration**: `addRoute(WEB_GET, "/assets/base.css", Asset(WebAssets::base_css_mime, WebAssets::base_css_gz, sizeof(WebAssets::base_css_gz)));` — `Webserver::Asset()` builds the route handler, delegating to `WebResponse::sendAsset()` for the actual header/body logic (`Content-Type`, `Cache-Control`, `Content-Encoding: gzip`, static body — no copy).
- **Always gzip, no negotiation**: the server never checks `Accept-Encoding` — every client gets the gzip response. Not a concern for a browser-only audience; `curl /assets/base.css` without `--compressed` shows binary, not CSS.
- **`/assets/logo.svg`**, not `/assets/logo/black.svg` — flattened together with the rest of the migration.

---

## Webserver

Full API reference: [`README.Webserver.md`](README.Webserver.md)

### Compile Flags

| Flag | Effect |
|------|--------|
| `OPENKNX_WEBSERVER` | Enables HTTP server, routing, overview page, logo asset, all routes |
| `OPENKNX_WEBCONSOLE` | Enables the `/console` page (streaming over the shared UI socket) **and** the logger ring buffer in OGM-Common |
| `OPENKNX_WEBCONSOLE_BUFFER` | Total ring buffer size in bytes (optional, default: 4096, max: 65535) |
| `OPENKNX_WEBMONITOR` | Enables the `/groupmonitor` page, live KNX telegram stream over the shared UI socket. Requires `OPENKNX_WEBSERVER`; needs a TP-UART, additionally gated on `MASK_VERSION == 0x07B0 \|\| 0x091A` |
| `OPENKNX_WEBFS` | Enables `/filemanager` page + its download/upload/delete/mkdir routes |
| `OPENKNX_SDCARD` | *Not owned by this module* — when set, the file manager offers the SD card as a second drive via `sd::fileStore` (OFM-SDCard) |
| `OPENKNX_EXTFLASH` | *Not owned by this module* — when set, the file manager offers the external flash chip as a drive via `efc::fileStore` (OFM-ExternalFlash) |

`OPENKNX_WEBCONSOLE` requires `OPENKNX_WEBSERVER`.
Without `OPENKNX_WEBSERVER` no webserver code is compiled at all — no class, no stubs, no `webserver` member in the module.

### Platform Support

| Platform | Stack | Implementation |
|----------|-------|----------------|
| **ESP32** | FreeRTOS + esp-idf `httpd` | `Webserver_ESP32.cpp` |
| **RP2040** | lwIP `NO_SYS=1`, single-threaded | `Webserver_RP2040.cpp` |

### Startup: `setup()` then `start()`

Two calls, and the split is load-bearing. `Module::loop()` runs
`webserver.setup(); webconsole.setup(); filemanager.setup(); groupmonitor.setup(); webserver.start();`

- **`setup()` registers**, `start()` opens the port. On ESP32 `httpd` serves from its own task, so a
  port open before the feature modules have registered leaves `handleRequest()` iterating `_routes`
  while a `push_back` reallocates it — a use-after-free over `std::string`/`std::function`. With an
  always-open UI socket the browser reconnects within a second, which makes that window easy to hit.
- **The `_initialized` guard covers registration only.** `start()` must stay repeatable: a failed
  `tcp_listen()` (RP2040 with the default of 5 TCP PCBs) or `httpd_start()` is otherwise permanent and
  the device has no web interface until reboot. `Module::loop()` retries while `isRunning()` is false;
  `start()` backs off from 1 s to 10 s per failure. The backoff is there for the log as much as for
  the attempt — the platform start reports every failure, so a fixed interval would turn a lasting
  PCB or heap shortage into one error line per interval. Capped at 10 s rather than the 30 s the browser uses: the failure clears when another service releases a PCB or some heap, and a device without a web interface for half a minute is worse than a few more log lines.
- **Every feature `setup()` needs its own `_initialized` guard** for the same reason — on a retry they
  all run again, and without it menu entries, routes and asset tags stack up.
- **The send queue is allocated in `start()`, before the platform start**, and a failure aborts the
  start. That keeps "server running" and "socket can send" the same state instead of producing a
  server whose WebSocket silently never delivers.

### Access Log

`logRequest()`/`logWebsocket()` log `>= 500` as error and `400–499` as warning unconditionally; everything below (2xx/3xx, WS `101`) only when the runtime toggle `Webserver::accessLog()` is on — off after boot, switched via console `webserver log`. Rationale: one page view is a request per asset, so a permanently-on 2xx log buries the console and costs serial time inside the loop on RP2040. See [README.Webserver.md § Access Log](README.Webserver.md#access-log).

### Character Encoding

The webserver is UTF-8 end to end; the KNX bus and the device's storage (filenames) are ISO-8859-15. Mind the boundary — convert with `OpenKNX::Charset::encodeUtf8`/`decodeUtf8` (`OGM-Common/src/OpenKNX/Charset.hpp`) wherever a bus/filesystem string reaches the webserver. See [README.Webserver.md § Character Encoding](README.Webserver.md#character-encoding) for why and the concrete call sites.

### UI Socket (`/openknx/ws`)

One WebSocket for the entire web interface, open on **every** page — it replaced the per-feature
endpoints (`/console`, `/groupmonitor`); those pages remain, only their own sockets are gone.
Full reference: [`README.Webserver.md` § UI Socket](README.Webserver.md#ui-socket).

Two functions, and that is deliberately all:

```cpp
openknxNetwork.webserver.ui.sendMessage(key, data, len = 0);   // always broadcast
openknxNetwork.webserver.ui.receiveMessage(cb);                // cb(key, data, len)
```

- **The firmware stores no keys.** No table, no lookup, no "unknown key" branch: the dispatcher splits
  at the first `:` and hands `key`/`data`/`len` to every registrant, which filters by `strcmp` itself.
  Incoming traffic is rare, so the cost is a few comparisons per command. **Who only sends registers
  nothing** — `tp` and `log` have no handler at all.
- **A receive handler is a network callback and must not block** — on RP2040 that is the lwIP
  interrupt. Record and defer, the way `Webconsole` uses `_pendingCmd` and the `prog` key uses
  `_pendingProg`. Deferring the dispatch inside the layer was tried and dropped again: it would have
  forced a copy, a size cap and a "second message in the same tick is lost" case onto an API whose
  handlers are trivial anyway. See README.Webserver.md § Callback context for the full table.
- **`ping`/`pong` covers the direction the device cannot.** RP2040's `onPoll` ping and ESP32's TCP
  keepalives detect a dead *browser*; a rebooted *device* sends neither FIN nor RST, so the page
  would keep a green dot until it happened to send something. `base.js` pings after a second of
  silence and gives up 2 s after an *unanswered* ping; the firmware answers with an empty `pong`, so
  a reboot surfaces in about three seconds. **Giving up must not mean `close()` and wait for
  `onclose`** — a dead peer answers nothing, so the socket only reaches CLOSING and the close event
  arrives whenever the TCP timeout does (Firefox waits 20 s). The verdict is therefore final: detach
  the handlers, report `closed` and schedule the reconnect straight away. Without that the page kept
  a green dot for another 20 s on both platforms, which is exactly the failure the check exists for. **Never conclude from silence alone** — a background tab's
  timer is throttled to ~1/min, and the first version did exactly that and tore down healthy
  connections once a minute. The check measures how long the timer slept and re-bases instead of
  judging when that exceeds its interval. Answering happens in the network callback, which is fine here —
  `sendMessage()` only enqueues and does not allocate. Since `pong` is a broadcast it resets every
  other tab's timer too, so the rate stays at ~one ping per second however many pages are open.
- **`onState()` is edge-triggered.** With the network down every reconnect attempt fails again, and
  reporting each one as a fresh event filled the console with a gap marker per try — at the 30 s
  backoff ceiling, forever. `fire()` therefore drops a repeat of the state already reported, and a
  late registrant is handed the current state instead of waiting for the next change.
- **The navigation carries the socket's own state** (`#ws-dot`/`#ws-text`, updated from
  `OpenKNX.onState()`), rendered server-side as "Verbinde…" because the page arrives over HTTP before
  the socket exists. Without JavaScript it stays that way, which is accurate — there is no connection.
- **`connect` is a reserved key** delivered to all handlers when a client appears, meaning "send your
  current state". A reconnect is indistinguishable from a first connect — intentionally, that is what
  makes a client resynchronise completely. `connect` and `pong` are therefore **rejected on the
  inbound path**: the endpoint is unauthenticated, and a client sending `connect:` would otherwise run
  the whole `publishStatus()` — on RP2040 inside the ethernet interrupt, `delay(1)` of the temperature
  ADC included.
- **No tick, and no change detection in the layer.** The layer transports; *when* to send is the
  producer's decision. `publishStatus()` therefore compares the handful of scalars it already holds
  (~20 B) rather than hashing or copying a payload. `up` (uptime) travels only when a full state is pushed (`connect`, prog change) and the
  browser counts on from there — otherwise something would have to be sent every second just to keep
  it moving. `mem`/`cpu` are coarsely rounded for the same reason.
- **No unicast.** The only candidate was the initial sync, and a broadcast does that just as well
  (~200 B once per page load), so no producer ever sees an `fd`. Unicast stays available one layer
  down via `sendWebsocketMessage(uri, msg, fd)`.
- `UI_WS_URI` is a class constant, not a macro, and deliberately not `/ws`: `addSocket()` matches
  exactly and has no dedup, so a module registering `/ws` itself would silently lose to the first
  entry.

### WS Send Queue (`WsTxQueue`)

Sits **below** the UI layer, so every `addSocket()` endpoint benefits: `sendWebsocketMessage()` and
`sendToClient()` now queue instead of writing the socket, and only the drain in `Webserver::loop()`
sends (through the internal `wsSendNow()` — routing the drain through the public API would make it
re-queue endlessly). Before this, a frame was simply dropped whenever a client's TCP buffer was
momentarily full, and `Webconsole` was the only feature that worked around it.

- **One shared record ring for all clients** (`OPENKNX_WEBSOCKET_TX_BUFFER`, default 4 KB via
  `PSRAM_MALLOC`), plus a 16-byte cursor entry per client. RAM does not scale with the client count and a
  broadcast is stored once, not once per client.
- **Length lives in the ring**, not in a separate index: message sizes are unknown, so there is
  exactly one limit (bytes) and no second resource that could run out first. `_oldest` is carried
  along rather than reconstructed — it always points at a header, because eviction advances it by
  exactly `HDR_SIZE + len`. `HDR_SIZE` is a constant `5`, never `sizeof` (padding would make it 6 and
  silently shift the whole ring).
- **Evicting means `_oldest += HDR_SIZE + len`** — nothing is moved or copied. `append()` frees whole
  records *before* writing, so a half-overwritten record is impossible by construction.
- **The payload never wraps**: if it would cross the ring end, a padding record (`ROUTE_PAD`) is
  inserted first. That keeps the drain able to hand the socket a pointer straight into the ring
  instead of reassembling into a scratch buffer. The 5-byte header may wrap and is read byte-wise.
- **Skipping advances the cursor** — load-bearing, not an optimisation to remove. A client on a
  foreign endpoint walks to the write position every tick and therefore stays at the front; without
  it, traffic that does not concern it would overtake it and get it disconnected.
- **`stalled()` is evaluated *after* the peek loop**, never before. Only there does `hasPending()`
  mean "did not accept"; before the loop it is also true for records this client does not own and
  peek merely skips, which put a foreign-endpoint client 10 s of unrelated UI traffic away from being
  dropped.
- **Positions are monotonic `uint32_t`** and the ring size is forced to a power of two, so the mask
  stays continuous across the counter's own wraparound. All comparisons are differences, never `<`.
- **`maxRecord()` is `min(size/2 - HDR_SIZE, maxWebsocketPayload(), 65535)`.** All three matter: half
  the ring so one message cannot evict everything, the frame limit because RP2040 can never send more
  than 1496 B and the drain would otherwise park a cursor on an unsendable record forever, and 65535
  because the header carries the length in 16 bits.
- **Locking is not the same mechanism on both platforms.** ESP32 uses the recursive WS-state mutex.
  RP2040 is *not* plain single-thread: with `PIN_ETH_INT` the W5500 hangs off an interrupt and
  `lwip_callback()` runs `onRecv`/`onErr`/`onPoll` inside it, so they preempt `loop()` mid-drain.
  `WsQueueGuard` therefore holds an `LWIPMutex` there — the core's own lock, which raises `__inLWIP`
  so the IRQ defers instead of firing. Without it the swap-remove in `removeClient()` can land
  between a `peek()` and its `advance()` and move the wrong client's cursor into a payload.
- **`Failed` is handled by the drain, not by `wsSendNow()`** — otherwise only ESP32 would clean up,
  and on RP2040 an unsendable record would stall that client silently.
- **A new client starts at the write position**: no backlog, and on RP2040 — where `fd` is the slot
  index and gets reused — a new client in a recycled slot cannot inherit its predecessor's pending
  unicasts.
- **`addClient()` returns a bool and the caller must look at it.** A client in `clients` without a
  cursor would receive nothing and loop reconnecting in silence, so `notifySocketConnect()` adds it to
  neither on failure and logs. A `static_assert` ties the cursor table to the connection pool, which
  is what makes the case unreachable in the first place.
- **Disconnecting means "stopped reading", not "slower than the producer".** Two triggers: overtaken
  in the ring, or nothing accepted for 10 s with records pending (`WsTxQueue::stalled()`). Producers
  must throttle on `freeBytes()` rather than fill the ring — `Webconsole::loop()` caps itself at a
  quarter of the ring *and* at what is free, keeping a small reserve for `status`/`tp`/`pong`, and
  `GroupMonitor::onFrame()` drops the telegram once less than a quarter of the ring is free. The
  first version filled the ring instead, which overtook briefly-slow browsers under a log burst and
  disconnected them; with two tabs reconnecting simultaneously their upgrades then collided with
  the shared connection limit and found no free slot. No gap event, no dropped counter: the browser
  reconnects on its own and resyncs through `connect`, which is exactly where a "snap the cursor"
  would have put it.
- **Drop callbacks fire outside the lock.** The drain collects the clients it wants to tear down and
  calls `dropClient()` only after releasing the guard — otherwise a module whose `onConnect(fd,false)`
  takes a lock of its own (MQTT does) could deadlock against a task that holds that lock and waits for
  the WS state mutex. The code this replaced carried the same rule as a comment.
- **`onConnect(fd, true)` is delivered from `loop()`**, not from the network callback — so a module
  may build an arbitrarily large JSON there and has the `fd` when it needs it. The cursor is created
  in `notifySocketConnect()` and therefore exists *before* delivery; otherwise the initial data would
  land behind the client's own cursor. **Disconnect stays immediate** on purpose: the slot can be
  reused at once, and a late notification would tear down the *new* client's state. If a client
  leaves before its connect notification was delivered, both are suppressed — the module never
  learned about it.
- **Known gap, deliberately open:** on RP2040 the ethernet interrupt can deliver a disconnect
  *nested inside* the connect delivery, i.e. before it returns. It is still delivered exactly once —
  `takePendingConnect()` has cleared the flag, so `removeClient()` reports it — only out of order. A
  post-check after the callback would make it a *second* notification, and fixing the order needs a
  deferred disconnect path. Only a module that builds per-client state in `onConnect` is affected,
  and none in-tree does (the console registers no `onConnect`, the monitor passes `nullptr`).

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
- `base.css` and `base.js` are registered via these functions during `setup()`. **base.js must stay
  the first registered script**: it defines `window.OpenKNX`, and every page script (`console.js`,
  `groupmonitor.js`) uses it. Scripts load with `defer`, which preserves document order.

**Implementation:**
- `Webserver._stylesheets` → `std::vector<std::string>` iterated in `buildHeader`
- `Webserver._scripts` → `std::vector<std::string>` iterated in `buildFooter`

### Response Assembly (Segments)

The platform transport code knows nothing about layout. `Webserver::handleRequest()` routes the request and then, unless the response is streaming, applies the layout and flattens everything into a segment list on the `WebResponse`:

- `setLayoutChrome()` moves the `buildHeader()`/`buildFooter()` strings into the response (no copy)
- `finalizeSegments()` builds `ResponseSegment[]` = header + body + footer (max 3, empty parts skipped)
- Segments are plain `{data, len}` views — **the memory always belongs to the `WebResponse`**, so no ownership flag and nothing to free per segment

Platform code then only iterates `response.segments()`:

- **ESP32**: one segment → `httpd_resp_send()` (keeps `Content-Length`); several → `httpd_resp_send_chunk()` each
- **RP2040**: `ConnSlot` holds only the send position (`txSegIdx`/`txSent`); `tcpSendChunk()` walks the segments across `onSent()` ticks

Nothing is copied to assemble a page — no merged header+body+footer buffer on either platform. Because RP2040 sends asynchronously, the response must outlive `doHttpDispatch()`: it lives in `static WebResponse g_response[MAX_CONN]`, indexed by slot. Deliberately **not** inside `ConnSlot` — `onAccept()` clears that with `memset`, which would wreck the `std::string`/`std::vector` members. `releaseSlot()` calls `reset()` on it.

### Layout Convention: `.container`

`buildHeader()` emits only `<nav>` + `<main>` — no wrapping `<div class='container'>`.
Each page decides independently whether to use the container div.

- **With container** (`max-width: 960px`): wrap page content in `<div class='container'>...</div>`
- **Without container** (fullscreen): emit content directly into `main`; `main` is a flex-column (`flex:1`), so direct children with `flex:1` fill the full height. No C++ flag needed — controlled purely via CSS.

### Logger Ring Buffer (`OPENKNX_WEBCONSOLE`)

The ring buffer lives in `OpenKNX::Log::Logger` (OGM-Common), **not** in the webserver. It fills regardless of whether WebSocket clients are connected.

- **Type**: plain byte ring, no entries/framing — `char _ringBuf[RING_SIZE]`, written one character at a time via `appendWebconsoleBuffer()` (`_ringBuf[_ringWritePos++ % RING_SIZE] = c`)
- **Size**: `RING_SIZE = OPENKNX_WEBCONSOLE_BUFSIZE` bytes
- **API**: `ringBuf()` (raw buffer pointer) and `ringWritePos()` (monotonically increasing write cursor, never wraps itself — callers index with `% RING_SIZE`)
- No sequence numbers, no sentinels, no line framing on the logger side — `Webconsole::loop()` (below) is the one that walks the buffer and splits it into lines

### RP2040 Architecture (lwIP `NO_SYS=1`)

Critical characteristic: lwIP runs single-threaded. Callbacks (`onAccept`, `onRecv`, `onSent`, `onPoll`, `onErr`) must **not block** and must not perform long operations.

**Connection slots:**
```
static constexpr int MAX_CONN = OPENKNX_WEBSERVER_MAX_CONN; // 3, or 6 with __LWIP_MEMMULT>=2
static ConnSlot g_slots[MAX_CONN];
```

`OPENKNX_WEBSERVER_MAX_CONN` is now one flag for both platforms (ESP32 7; RP2040 3, or 6 when the parent project sets `-D__LWIP_MEMMULT=2`) and caps the **sum** of HTTP and WebSocket connections. Since a visitor occupies two slots, an RP2040 project **without** that flag serves one visitor — the always-open UI socket is what makes the flag worth setting — the mix is deliberately not constrained. 6× HTTP, 5× HTTP + 1 WS, 6× WS: all fine, whoever asks first gets a slot and whoever finds none does not get one. Only the total is limited, so the device's other services (NTP, MQTT, OTA, KNX-IP) keep sockets of their own. An earlier version capped WebSockets separately so HTTP could not starve; that was dropped as the wrong trade for an embedded device — it lowered the visitor count to protect a failure mode that heals itself on the next reload. Note a visitor occupies two slots: the permanently open WebSocket plus an HTTP slot while a page loads.

**The RP2040 default follows the lwIP pool instead of demanding a flag.** arduino-pico sets `MEMP_NUM_TCP_PCB = __LWIP_MEMMULT * 5`, so by default the *whole device* has 5 active TCP PCBs — including MQTT, the webclient, OTA and the TIME_WAIT states of freshly closed connections, and the last accept would fail silently inside lwIP. `Webserver.h` therefore keys the default off the flag: 3 slots without it, 6 (three visitors) with `-D__LWIP_MEMMULT=2`. `MEMP_NUM_TCP_PCB` itself is unusable there — `Webserver.h` does not include `lwipopts.h` — so the flag stands in for it. Six slots are never promised on a pool that cannot hold them, and a project that sets `OPENKNX_WEBSERVER_MAX_CONN` too high for its pool anyway still gets the `#warning` in `Webserver_RP2040.cpp` (a `#error` would break existing OAM builds). The flag does take effect under PlatformIO: lwIP is compiled from source through the wrappers in `cores/rp2040/lwip/`, not linked from a prebuilt archive. `platformio.network.ini` carries the snippet, commented out. `OPENKNX_WEBSERVER_RX_CAP` (RP2040, default 1024) follows the same naming rule for the same reason — the per-slot receive buffer serves HTTP headers *and* WS payloads, so there is one flag for both. ESP32's `OPENKNX_WEBSOCKET_RX_CAP` (default 2048) keeps the `WEBSOCKET_` prefix because it really is WebSocket-only — httpd buffers the HTTP side itself. **The scope differs by necessity, the behaviour does not:** on ESP32 httpd parses HTTP headers with its own buffers, so ours cannot be shared there — but an oversized WS payload now drops the connection on *both* platforms. RP2040 used to truncate and still deliver the frame, which turned a too-long `cmd:` into half a console command that then ran, and an absurd declared length kept the connection consuming bytes indefinitely. The length field is `uint64_t` on both platforms because RFC 6455's `127` form carries eight length bytes — the comparison has to see the real wire value. Narrowing it would make the accumulate loop drop the high bytes silently, so `0x1_0000_0200` would arrive as 512, pass the check, and desynchronise the parser on the remaining stream. 8 bytes per slot is the right price for that.

Each `ConnSlot` holds: **one** receive buffer (`OPENKNX_WEBSERVER_RX_CAP` B), HTTP TX pointer, WS state machine state. The webconsole's read position into the logger ring is a single `Webconsole::_readPos`, not per client and not in `ConnSlot` — see below. Per-client send state lives in `WsTxQueue`, also outside `ConnSlot`: `onAccept()` clears that with `memset`, which would wreck any non-trivial member.

**`rxBuf` serves both phases** — WS code casts to `uint8_t*` where it needs bytes instead of `char*`. A connection is in exactly one protocol at a time (`ConnState`), so the phases never overlap: filled only in `CS_HTTP_RECV`, then written only from `wsProcessData()`, which requires `CS_WS`. Both need reassembly across TCP segments — that is why a per-connection buffer exists at all; the WS *send* path needs none and uses a stack frame in `wsTcpSend()`.

The one ordering constraint this creates lives in `doHttpDispatch()`: the `Sec-WebSocket-Key` is not copied out of the request either — `wsKeyOff` is an offset into `rxBuf` (0 = header absent) — so `wsComputeAccept()` reads `rxBuf` and must stay **before** `s->state = CS_WS`. Anything added to the upgrade path after that line is looking at WS payload memory, not at the request.

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

**TCP send:** `wsTcpSend()` checks `tcp_sndbuf()` and reports `WouldBlock` when the buffer is full (slow client). It is called only from the send queue's drain, which then leaves that client's cursor where it is and retries next tick — nothing is dropped. The connection remains open.

**Ping/keepalive:** `onPoll` sends a WS ping (opcode 0x09) every ~60 s. If the browser does not respond, lwIP's retransmit timeout detects the dead socket and fires `onErr`.

### Command Processing (Decoupling from Network Callbacks)

Platform-neutral, lives in `Webconsole` (`_pendingCmd[256]` / `_hasPendingCmd`), not per-platform.

**Problem:** `processCommand()` calls many log functions that generate serial TX output. Running it inside a network callback (lwIP `onRecv` on RP2040; the httpd task on ESP32) would block and could truncate output or stall the callback's task.

**Solution:** Queue-and-defer pattern:
- The UI handler (`ui.receiveMessage()` in `Webconsole::setup()`, filtering for the `cmd` key) only copies the command into `_pendingCmd` and sets `_hasPendingCmd = true`
- `Webconsole::loop()` (called from `Network::Module::loop()`, clean stack) picks it up, echoes it via the logger, and calls `openknx.console.processCommand()`

**Limitation:** There is only one `_pendingCmd` slot. With multiple simultaneous WS clients the last received message overwrites the previous one. This is sufficient for a single console session.

**Binary frame filter:** WS frames from the browser containing bytes outside printable ASCII (`0x20–0x7E`, `\r`, `\n`, `\t`) are silently discarded. Protects against garbage from malformed or protocol-internal frames (e.g. browser reconnect artifacts).

### No History for New WebSocket Clients

`Webconsole` keeps **one** read position for all clients (`_readPos`), not one per client: a log line
is relevant to every connected console, and the send queue below holds it until each client has taken
it. With no clients connected `_readPos` simply tracks `ringWritePos()`, so the next client to arrive
starts at "now" and gets no backlog — the same behaviour the per-client map used to provide, without
the bookkeeping.

**A ring overrun is reported, not swallowed.** When `writePos - _readPos` exceeds `RING_SIZE` the
oldest lines are already overwritten in the logger ring — typically because a single
`processCommand()` produced more than `OPENKNX_WEBCONSOLE_BUFFER`. The snap therefore emits a
`[... N Bytes Log verworfen ...]` line first, as an ordinary `log` record with an ANSI colour, so it
needs no client-side support. The other loss path — a client overtaken in the send queue — surfaces
as a disconnect and `console.js` writes its own gap marker.

`loop()` enqueues complete lines only (so the browser's ANSI parser stays stateless) and stops after
`txCapacity() / 4` bytes per tick -- a quarter of the *actual* ring, which `setup()` rounds down to a
power of two. That cap matters: a single `processCommand()` like
`showInformations` produces far more than one tick should push, and the alternative — waiting for
free space — would let one stalled client hold up the log for everyone. `maxLine` is additionally
clamped to `maxWebsocketPayload()` *minus the `log:` key*, otherwise the longest line would be
permanently unsendable on RP2040.

### ESP32 Architecture

- `httpd` runs in its own FreeRTOS task
- **WebSockets use no per-connection task.** On upgrade the socket is detached from httpd via `httpd_req_async_handler_begin()` so httpd stops polling it; it is then serviced non-blocking from `Webserver::loop()` (driven by the main loop, just like RP2040). The earlier `httpd_sess_set_recv_override` endless-loop monopolized the single httpd task and limited the device to one working WebSocket (a second tab couldn't connect); the task-per-session variant fixed concurrency but cost ~4.3 KB RAM per connection — both are gone.
- `Webserver::loop()` per session: `recv(MSG_DONTWAIT)` drains available bytes into the session's `rx` buffer, then `wsParseFrames()` dispatches every complete frame (partial frames wait for the next tick). On EOF/error/oversize → cleanup: `notifySocketConnect(false)` → `httpd_req_async_handler_complete()` (httpd closes the socket) → remove from registry → free.
- `onMessage` runs in the loop task; for the console it only buffers the command into `_pendingCmd`, which `Webconsole::loop()` then runs via `processCommand()`.
- Concurrency bounded by `OPENKNX_WEBSERVER_MAX_CONN` (default 7), shared with HTTP; there is no WebSocket-specific cap. `config.lru_purge_enable = true` frees idle HTTP keep-alives so new connections find a socket slot — a live WebSocket is safe from it, because its async handler stays open (see `httpd_req_async_handler_complete()`).
- Shared state (`_socketClients` + the WS session registry) guarded by a recursive mutex (`Webserver::wsStateLock/Unlock`), reachable from both `Webserver_ESP32.cpp` and the platform-agnostic `Webserver.cpp` (`hasClients`, `ui.hasClients()`, `notifySocketConnect`, the queue drain). `wsSendText()`/`wsRawSend()` use `MSG_DONTWAIT` and are serialized by a TX mutex so frames from different tasks never interleave.
- TCP keepalive: 3 s idle, 2 s interval, 3 probes → dead connections detected after ~9 s
- **Streaming downloads (`FileManager::handleDownload`) run synchronously in the httpd task** — `platformHttpHandler()` reads the whole file from LittleFS and calls `httpd_resp_send_chunk()` in a blocking loop. Since httpd has a single task, no other HTTP request (including WS upgrades) is served until the download finishes. Acceptable for the small config/log files WEBFS is meant for; would need reworking (spread across `Webserver::loop()` ticks like RP2040 already does) before serving large files.

### WebSocket Protocol Scope

Both platforms parse only what the bundled browser clients actually send: masked, unfragmented text/binary/ping/pong/close frames. A payload larger than the platform's RX cap drops the connection on both — never a truncated frame handed to the handlers. Frames without a mask (disallowed by RFC 6455 for client→server) and fragmented messages (opcode `0x00` continuation, `FIN=0`) are **not validated or reassembled** — not a concern as long as the only clients are this module's own pages, whose `ws.send(string)` calls never produce either. Revisit if the WS endpoints are ever exposed to third-party clients.

### Web UI: Console Page

- **Reconnect is automatic** (it lives in `base.js` and serves every page), but the console writes a visible marker on disconnect and reconnect — a gap in the log must be seen, not silently accepted
- That is what the earlier no-reconnect rule was protecting: scrambled or unnoticed log output. The marker addresses it without leaving every other page dead after a brief network hiccup
- **The firmware sends raw log lines**, ANSI escapes included — no markup is generated on the device. The browser maps the codes to the CSS classes `red`/`green`/`yellow`/`gray` and inserts every segment via `textContent`, so log text is never parsed as HTML and the WS payload equals the raw line length
- Chunks are bounded by `Webserver::maxWebsocketPayload()` (RP2040: 1496 B fixed frame buffer, ESP32: unbounded) — a larger payload could never be sent and the drain loop's retry would block the console forever
- Commands go out as `cmd:<text>` over the UI socket, log lines come back as `log:<line>`

### Group Monitor (`OPENKNX_WEBMONITOR`, TP only)

`GroupMonitor` (`src/OpenKNX/Network/Webserver/GroupMonitor.{h,cpp}`, member `openknxNetwork.groupmonitor`) — ETS-style live KNX telegram viewer, read-only. Whole feature gated on `#if defined(OPENKNX_TELEGRAMJSON) && defined(OPENKNX_WEBMONITOR)` (device with a TP-UART — plain TP device or TP+IP router alike); class header is `#ifdef OPENKNX_WEBMONITOR` only, the member/setup-call/`.cpp` body add `OPENKNX_TELEGRAMJSON`.

- **Parallel tap, bypassing the stack:** registers `knx.bau().getDataLinkLayer()->getTPUart().registerReceivedFrame(...)` — the same hook `MQTT::Module`'s raw-telegram publish uses (`MQTT/Module.cpp`). The callback fires in `processRxFrameBuffer()` (loop task, after repetition filter, before stack processing) for **every** received frame — no filtering.
- **Read-only on the bus, but not stateless:** the module never sends a telegram. It does accept the `busmon` key (`1` / `0`), which switches the TPUart between monitoring mode and normal operation (`startMonitoring()` / `reset()`); the current state travels back as a field in `status`, so `/groupmonitor/state` is gone. In monitoring mode the device stops processing KNX telegrams — same as ETS busmonitor. Unauthenticated, like every other webserver endpoint. **Compiled out on 0x091A** (`#if MASK_VERSION != 0x091A`, checkbox markup and the `busmon` handler both) — on a router, monitoring mode would stop TP↔IP routing, not just this device's own bus participation. The JS null-checks `#gm-busmon` since the same script serves both masks.
- **Deliberately no ring buffer** (unlike Webconsole, whose buffer exists because the logger fills independently of clients). `onFrame()` checks `isFrame()`/`ui.hasClients()` and the queue's free space, then delegates the actual decode to [`buildTelegramJson()`](#openknxnetworktelegramjson-srcopenknxnetworktelegramjsonh) with its own persistent [`JSON::Writer`](#openknxformatjson-srcopenknxformatjson) member (`reset()` keeps its capacity, so no per-telegram heap growth), and broadcasts via `webserver.ui.sendMessage("tp", …)`. Safe from that callback because it shares the loop task with the webserver, and the send queue's own guard covers the ring on both platforms. **`loop()` exists only for `busmon`**: the receive handler runs in the network callback — on RP2040 the ethernet interrupt — while `startMonitoring()`/`reset()` queue a control byte into a lock-free ring whose producer and consumer must be the same context. So the handler only records into `_pendingBusmon` and `loop()` switches, the same record-and-defer as `cmd` and `prog`.
- **A telegram is dropped, not buffered, under pressure.** `onFrame()` returns early once free ring space falls below a quarter, because the TP-UART hands over several buffered frames after a slow tick while the drain runs once per `loop()`. Filling the ring instead would overtake a healthy client and disconnect it — much worse than one missing row.
- **Decoding, JSON keys, GA table:** see [`TelegramJson`](#openknxnetworktelegramjson-srcopenknxnetworktelegramjsonh) — shared with `MQTT::Module`'s `knx/tp/telegramm` publish, so both stay in sync by construction.
- **Assets** `/assets/groupmonitor.css` + `.js` via `Asset()` (generated `webassets.h`, see [Web Assets](#web-assets-webassets-generated-webassetsh)) + `addStylesheet()`/`addJavaScript()`; client timestamps on arrival; autoscroll/pause/clear; 1000-row cap. `setup()` guarded with `_initialized` so a webserver restart never double-registers the frame callback.
- **Names stay client-side:** the browser fetches `/groupmonitor/ga.tsv`, builds an `addr→name` Map and fills the Name column; the backend never handles name strings. The route is the group monitor's own and streams the file from LittleFS via `sendStream()` — *not* `/filemanager/download`, which depends on `OPENKNX_WEBFS` and produced a 404 with empty name columns whenever that flag was off. **File absent → `200` with an empty body**, not `404`: the columns end up showing `-` either way, but a 404 is logged as a warning unconditionally (see [Access Log](#access-log)) — that would be one warning per page view on every device without a GA table. The empty response also exercises the zero-length streaming path, which is why `tcpSendStreamChunk()` flushes on its early exits.

### File Manager (`OPENKNX_WEBFS`)

`FileManager` (`src/OpenKNX/Network/Webserver/FileManager.{h,cpp}`) — browse/download/upload/delete/mkdir over HTTP. Untrusted input is escaped at every output site (`htmlEscape`, `jsStr`, `jsStrScriptBody`, `urlEncode`); `sanitizePath()` rejects any path containing `..`.

- **Upload is one POST per chunk** (`offset` query param), so no whole file is ever held in RAM. `offset == 0` on an existing path → `409`. After each chunk the file size is compared against `offset + length` — LittleFS writes only on `close()` and `write()` always returns the buffer size, so this comparison is the only reliable full-filesystem detection (→ `413`, partial file removed).
- **A missing drive or file answers `404`** on every route (`drvAvail()` / the `open()` in `handleDelete()`), `400` only for a malformed path, `409`/`413` for overwrite/no-space on upload, `500` only for a genuinely failed operation (write error, non-empty directory). `handleList()` deliberately does **not** probe the directory: a `LittleFS.exists()` pre-check would make every subdirectory depend on `exists()` answering `true` for directories, and a wrong answer there kills navigation entirely. A nonexistent directory therefore renders as an empty listing, as before.
- **The listing is streamed, never collected.** `drvForEach(d, dir, fn)` is a template taking a callback; `handleList()` walks the directory twice (directories, then files, via a `wantDirs` flag captured by the lambda) and appends rows straight into `html`. Deliberately **no** result container: 250 entries as `std::vector<{std::string,uint32_t,bool}>` are ~10 KB heap plus realloc peak, which RP2040 cannot spare next to lwIP and the connection slots. Capped at `FM_MAX_ENTRIES` (250) visited entries per pass, with a note rendered in the table when the cap is hit.

#### Multi-drive (`fs` query parameter)

Internal flash is always available; SD and external flash appear only if their define is set **and** the store reports `available()`. All routes accept `fs=int|sd|efc`, default and fallback `int` — old links without the parameter keep working. A tab bar is rendered above the listing as soon as more than the internal drive exists, and `currentFs` is injected into the page for the JS fetches.

- Dispatch is a flat set of `static` functions (`drvParse`, `drvId`, `drvAvail`, `drvExists`, `drvSize`, `drvRemove`, `drvMkdir`, `drvWriteChunk`, `drvSpace`, `drvForEach`), **not** a virtual interface — no vtable, and with both defines off every foreign branch is preprocessed away, leaving the internal-only code size unchanged.
- **External dependency**: `sd::fileStore` / `efc::fileStore` come from OFM-SDCard / OFM-ExternalFlash (`SdFileStore.h` / `EfcFileStore.h`). Neither is part of this module; with the defines unset nothing of it is compiled.
- **`handleDelete` takes a `dir=1` hint from the client** because the stores expose no `isDirectory()`. On the internal drive the hint is overridden by a server-side `File::isDirectory()` check.
- **The stores are singletons with one global cursor.** `handleDownload` keeps the store open across many `Webserver::loop()` ticks, so two concurrent downloads from the *same* store interfere. LittleFS is unaffected (one `File` per request).
- `fmtBytes()` formats B/KB/MB/GB; below the GB threshold it deliberately casts to 32 bit, since the u64 division would otherwise pull runtime helper routines into flash on RP2040.

---

Changelog: [`CHANGELOG.md`](CHANGELOG.md)
