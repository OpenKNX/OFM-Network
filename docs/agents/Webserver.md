# Webserver.md - Webserver: Flags, Start, Layout

Teil von [`AGENTS.md`](../../AGENTS.md).
Verwandt: [`UiSocket.md`](UiSocket.md), [`WsTxQueue.md`](WsTxQueue.md), [`WebserverPlatform.md`](WebserverPlatform.md), [`Webconsole.md`](Webconsole.md), [`GroupMonitor.md`](GroupMonitor.md), [`FileManager.md`](FileManager.md), [`Build.md`](Build.md)

Full API reference: [`README.Webserver.md`](../../README.Webserver.md)

## Compile Flags

| Flag | Effect |
|------|--------|
| `OPENKNX_WEBSERVER` | Enables HTTP server, routing, overview page, logo asset, all routes |
| [`OPENKNX_WEBCONSOLE`](Webconsole.md) | Enables the `/console` page (streaming over the shared UI socket) **and** the logger ring buffer in OGM-Common |
| `OPENKNX_WEBCONSOLE_BUFFER` | Total ring buffer size in bytes (optional, default: 4096, max: 65535) |
| [`OPENKNX_WEBMONITOR`](GroupMonitor.md) | Enables the `/groupmonitor` page, live KNX telegram stream over the shared UI socket. Requires `OPENKNX_WEBSERVER`; needs a TP-UART, additionally gated on `MASK_VERSION == 0x07B0 \|\| 0x091A` |
| [`OPENKNX_WEBFS`](FileManager.md) | Enables `/filemanager` page + its download/upload/delete/mkdir routes |
| `OPENKNX_SDCARD` | *Not owned by this module* — when set, the file manager offers the SD card as a second drive via `sd::fileStore` (OFM-SDCard) |
| `OPENKNX_EXTFLASH` | *Not owned by this module* — when set, the file manager offers the external flash chip as a drive via `efc::fileStore` (OFM-ExternalFlash) |

`OPENKNX_WEBCONSOLE` requires `OPENKNX_WEBSERVER`.
Without `OPENKNX_WEBSERVER` no webserver code is compiled at all — no class, no stubs, no `webserver` member in the module.

## Platform Support

| Platform | Stack | Implementation |
|----------|-------|----------------|
| **ESP32** | FreeRTOS + esp-idf `httpd` | `Webserver_ESP32.cpp` |
| **RP2040** | lwIP `NO_SYS=1`, single-threaded | `Webserver_RP2040.cpp` |

Both in detail: [`WebserverPlatform.md`](WebserverPlatform.md).

## Startup: `setup()` then `start()`

Two calls, and the split is load-bearing. `Module::loop()` runs
`webserver.setup(); webconsole.setup(); filemanager.setup(); groupmonitor.setup(); webserver.start();`

- **`setup()` registers**, `start()` opens the port. On ESP32 `httpd` serves from its own task, so a
  port open before the feature modules have registered leaves `handleRequest()` iterating `_routes`
  while a `push_back` reallocates it — a use-after-free over `std::string`/`std::function`. With an
  always-open UI socket ([`UiSocket.md`](UiSocket.md)) the browser reconnects within a second, which makes that window easy to hit.
- **The `_initialized` guard covers registration only.** `start()` must stay repeatable: a failed
  `tcp_listen()` (RP2040 with the default of 5 TCP PCBs) or `httpd_start()` is otherwise permanent and
  the device has no web interface until reboot. `Module::loop()` retries while `isRunning()` is false;
  `start()` backs off from 1 s to 10 s per failure. The backoff is there for the log as much as for
  the attempt — the platform start reports every failure, so a fixed interval would turn a lasting
  PCB or heap shortage into one error line per interval. Capped at 10 s rather than the 30 s the browser uses: the failure clears when another service releases a PCB or some heap, and a device without a web interface for half a minute is worse than a few more log lines.
- **Every feature `setup()` needs its own `_initialized` guard** for the same reason — on a retry they
  all run again, and without it menu entries, routes and asset tags stack up.
- **The send queue ([`WsTxQueue.md`](WsTxQueue.md)) is allocated in `start()`, before the platform start**, and a failure aborts the
  start. That keeps "server running" and "socket can send" the same state instead of producing a
  server whose WebSocket silently never delivers.

## Access Log

`logRequest()`/`logWebsocket()` log `>= 500` as error and `400–499` as warning unconditionally; everything below (2xx/3xx, WS `101`) only when the runtime toggle `Webserver::accessLog()` is on — off after boot, switched via console `webserver log`. Rationale: one page view is a request per asset, so a permanently-on 2xx log buries the console and costs serial time inside the loop on RP2040. See [README.Webserver.md § Access Log](../../README.Webserver.md#access-log).

## Character Encoding

The webserver is UTF-8 end to end; the KNX bus and the device's storage (filenames) are ISO-8859-15. Mind the boundary — convert with `OpenKNX::Charset::encodeUtf8`/`decodeUtf8` (`OGM-Common/src/OpenKNX/Charset.hpp`) wherever a bus/filesystem string reaches the webserver. See [README.Webserver.md § Character Encoding](../../README.Webserver.md#character-encoding) for why and the concrete call sites.

## Asset Management (`addStylesheet` / `addJavaScript`)

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
- `base.css` and `base.js` ([`Build.md`](Build.md)) are registered via these functions during `setup()`. **base.js must stay
  the first registered script**: it defines `window.OpenKNX`, and every page script (`console.js`,
  `groupmonitor.js`) uses it. Scripts load with `defer`, which preserves document order.

**Implementation:**
- `Webserver._stylesheets` → `std::vector<std::string>` iterated in `buildHeader`
- `Webserver._scripts` → `std::vector<std::string>` iterated in `buildFooter`

## Response Assembly (Segments)

The platform transport code knows nothing about layout. `Webserver::handleRequest()` routes the request and then, unless the response is streaming, applies the layout and flattens everything into a segment list on the `WebResponse`:

- `setLayoutChrome()` moves the `buildHeader()`/`buildFooter()` strings into the response (no copy)
- `finalizeSegments()` builds `ResponseSegment[]` = header + body + footer (max 3, empty parts skipped)
- Segments are plain `{data, len}` views — **the memory always belongs to the `WebResponse`**, so no ownership flag and nothing to free per segment

Platform code then only iterates `response.segments()`:

- **ESP32**: one segment → `httpd_resp_send()` (keeps `Content-Length`); several → `httpd_resp_send_chunk()` each
- **RP2040**: `ConnSlot` holds only the send position (`txSegIdx`/`txSent`); `tcpSendChunk()` walks the segments from `onSent()` and `loopPlatform()`, after the HTTP header in `g_pending` — see [`WebserverPlatform.md`](WebserverPlatform.md)

Nothing is copied to assemble a page — no merged header+body+footer buffer on either platform. Because RP2040 sends asynchronously, the response must outlive `doHttpDispatch()`: it lives in `static WebResponse g_response[MAX_CONN]`, indexed by slot. Deliberately **not** inside `ConnSlot` — `onAccept()` clears that with `memset`, which would wreck the `std::string`/`std::vector` members. `releaseSlot()` calls `reset()` on it.

## Layout Convention: `.container`

`buildHeader()` emits only `<nav>` + `<main>` — no wrapping `<div class='container'>`.
Each page decides independently whether to use the container div.

- **With container** (`max-width: 960px`): wrap page content in `<div class='container'>...</div>`
- **Without container** (fullscreen): emit content directly into `main`; `main` is a flex-column (`flex:1`), so direct children with `flex:1` fill the full height. No C++ flag needed — controlled purely via CSS.
