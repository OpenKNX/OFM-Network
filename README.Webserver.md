# Webserver — OFM-Network

Built-in HTTP/WebSocket server for OpenKNX devices.

---

## Compile Flags

```
OPENKNX_WEBSERVER           – HTTP server, routing, overview page (/), logo
OPENKNX_WEBCONSOLE          – /console page (streams over the shared UI socket), logger ring buffer
OPENKNX_WEBCONSOLE_BUFFER   – Total size of the log ring buffer in bytes (default: 4096)
OPENKNX_WEBSERVER_MAX_CONN  – total webserver connection slots, shared between HTTP and WebSocket (ESP32 7; RP2040 3, or 6 when the project sets -D__LWIP_MEMMULT=2). A visitor takes two slots, so 3 carries one visitor and 6 carries three
OPENKNX_WEBSOCKET_TX_BUFFER – Send queue: one ring for all clients, in bytes (default 4096, rounded down to a power of two)
OPENKNX_WEBSOCKET_RX_CAP    – ESP32 only: per-WS accumulation buffer in bytes (default 2048, overflow → disconnect)
OPENKNX_WEBSERVER_RX_CAP    – RP2040 only: per-slot receive buffer in bytes, used for HTTP headers and WS payloads alike (default 1024; headers over it → HTTP 431, WS payloads over it → connection dropped, same as ESP32). Max 65535; below ~1 KB a normal browser request no longer fits.
OPENKNX_WEBMONITOR        – /groupmonitor page, live KNX telegram stream over the shared UI socket (TP only, MASK_VERSION == 0x07B0)
OPENKNX_WEBFS               – /filemanager page and its routes (browse, download, upload, delete, mkdir)
```

`OPENKNX_WEBCONSOLE` requires `OPENKNX_WEBSERVER`.  
`OPENKNX_WEBMONITOR` requires `OPENKNX_WEBSERVER` and a device with TP connection (`MASK_VERSION == 0x07B0`); on other masks the feature compiles to nothing.  
Without `OPENKNX_WEBSERVER` no webserver code is compiled at all.

---

## Start Condition

The webserver starts as soon as the network is available and one of the following conditions is met:

- `ParamNET_HTTP == true` — user has enabled the webserver via ETS parameter
- **Device not configured** (`!knx.configured()`) — the webserver always starts, regardless of the parameter

This allows first access to a freshly flashed or unconfigured device without ETS programming.

> **Important for OAM developers:** For the ETS parameter `ParamNET_HTTP` to be visible to the user, the corresponding channel must be explicitly enabled in the OAM XML:
> ```xml
> <op:config name="%NET_ServiceHTTP%" value="1" />
> ```
> Without this entry the parameter remains hidden in ETS and the webserver only runs in the unconfigured state — which is undesirable in production devices.

---

## Access Log

Every request (and every WebSocket upgrade) passes through `Webserver::logRequest()` / `logWebsocket()`, but only failures are logged by default:

- **`>= 500`** → `logError`, **`400–499`** → `logWarning` — always logged
- **`< 400`** (2xx/3xx, WS `101`) → `logInfo`, only when the access log is enabled

The toggle is runtime-only, off after boot: console command `webserver log`, API `webserver.accessLog()` / `webserver.accessLog(bool)`. Serving one page produces a request per asset, so leaving 2xx on permanently drowns the console — and on RP2040 every line costs serial time inside the loop.

---

## Character Encoding

Everything the webserver sends or receives is **UTF-8** — HTTP `Content-Type` charsets, `<meta charset>`, WebSocket text frames, JSON. Not a stylistic choice: several of these have no charset override at all — WebSocket text frames (RFC 6455) must be valid UTF-8 with no negotiation, `fetch().text()`/`.json()` decode as UTF-8 unconditionally per the Fetch spec regardless of `Content-Type`, and `encodeURIComponent()` always percent-encodes as UTF-8. `<meta charset>` also loses to the HTTP header when both are present, so the two must agree anyway. Serving ISO-8859-15 instead would only move the problem, not remove it — every WS/`fetch()` call site would still need its own decode step.

The KNX bus (DPT16 strings) and the device's storage (FAT32/LittleFS filenames) are **ISO-8859-15**, not UTF-8. Wherever such a value crosses into the webserver, it is converted with `OpenKNX::Charset` (`OGM-Common/src/OpenKNX/Charset.hpp`):

```cpp
#include "OpenKNX/Charset.hpp"

std::string utf8;
OpenKNX::Charset::encodeUtf8(utf8, iso.data(), iso.size());   // ISO-8859-15 -> UTF-8, never fails

std::string iso;
bool lossless = OpenKNX::Charset::decodeUtf8(iso, utf8.data(), utf8.size());
// lossless == false: at least one character had no Latin-15 equivalent and was replaced with '?'
```

- **`encodeUtf8`** — always succeeds; every byte 0–255 is a valid Unicode code point.
- **`decodeUtf8`** — can lose information; a character outside the Latin-15 repertoire (e.g. most emoji, CJK) becomes `?`. The `bool` return lets a caller react differently instead of silently accepting the fallback.
- Header-only, gated behind `#ifdef OPENKNX_CHARSET` — a no-op include otherwise. `Network/Module.h` derives `OPENKNX_CHARSET` automatically from `OPENKNX_WEBSERVER`, so no separate build flag is needed.

**Where it's used today:**
- `GroupMonitor.cpp` — the DPT16 value decoded from a bus telegram is `encodeUtf8`'d before it goes into the JSON WebSocket message.
- `Webconsole.cpp` — each line pulled from the logger ring buffer is `encodeUtf8`'d before being sent over the `/console` WebSocket.
- `FileManager.cpp` — `sanitizePath()` runs `decodeUtf8` on every incoming path (browser → device, since `encodeURIComponent()` is always UTF-8) and maps any `?` fallback to `_` (safer than `?` on a filesystem); `handleList()` runs `encodeUtf8` on every filename read back from storage before it appears in the HTML listing, a link `href`, or the `currentDir` JS variable.

**Legacy filenames:** files created via WebFS before this conversion existed have their non-ASCII characters stored as raw UTF-8 bytes, not ISO-8859-15. `encodeUtf8`/`decodeUtf8` are exact inverses for *any* byte sequence, not just "real" ISO-8859-15 text, so such a name still round-trips correctly (display and delete both work) — the on-disk bytes are just a different (also valid) sequence than a freshly-created file with the same visible name would get.

---

## API

### Registering Routes

```cpp
// GET /status
openknxNetwork.webserver.addRoute(WEB_GET, "/status", [](WebRequest& req, WebResponse& res) {
    res.setContentType("application/json");
    res.send("{\"ok\":true}");
});

// POST /config
openknxNetwork.webserver.addRoute(WEB_POST, "/config", handler);

// Wildcard prefix: matches /files/ and everything below
openknxNetwork.webserver.addRoute(WEB_GET, "/files/*", handler);
```

Supported methods: `WEB_GET`, `WEB_POST`, `WEB_PUT`, `WEB_DELETE`.

### Static Helpers

```cpp
// Redirect
openknxNetwork.webserver.addRoute(WEB_GET, "/old",
    Webserver::Redirect("/new"));

// Inline text (e.g. CSS, JSON)
openknxNetwork.webserver.addRoute(WEB_GET, "/robots.txt",
    Webserver::Static("text/plain", "User-agent: *\nDisallow: /"));

// Binary data from PROGMEM
extern const uint8_t icon_png[];
extern const int     icon_png_len;
openknxNetwork.webserver.addRoute(WEB_GET, "/favicon.ico",
    Webserver::Static("image/png", icon_png, icon_png_len));
```

### WebSocket Endpoints

```cpp
openknxNetwork.webserver.addSocket("/ws",
    // onMessage
    [](int clientId, WebSocketFrame* frame) {
        std::string msg((char*)frame->data, frame->length);
        openknxNetwork.webserver.sendWebsocketMessage("/ws", "pong", clientId);
    },
    // onConnect (optional)
    [](int clientId, bool connected) {
        // connected == true: new client connected
        // connected == false: client disconnected
    }
);

// Broadcast to all clients on /ws
openknxNetwork.webserver.sendWebsocketMessage("/ws", "hello");

// Unicast to a specific client
openknxNetwork.webserver.sendWebsocketMessage("/ws", "hello", clientId);

// Are there any clients on this endpoint?
bool any = openknxNetwork.webserver.hasClients("/ws");
```

`sendWebsocketMessage()` **queues** — it never writes the socket. The send queue drains in `loop()`, so a
client whose TCP buffer is momentarily full no longer loses the frame. `true` from `sendToClient()`
therefore means *accepted*, not *delivered*.

`onConnect(fd, true)` is delivered from `loop()`, not from the network callback, and is the place to
push a client its initial state (`sendWebsocketMessage(uri, data, fd)`). A reconnect is
indistinguishable from a first connect, so the client resynchronises completely. Because it runs in
the loop context you may build an arbitrarily large JSON there — unlike the disconnect notification,
which still fires immediately from the network callback and must stay short (the slot can be reused
right away, and a late notification would tear down the *new* client's state).

On **RP2040** these methods must only be called from `loop()`.  
On **ESP32** they are callable from any task; the registered `onMessage` callback runs in the main loop task.

> **ESP32 concurrency:** WebSocket connections are detached from httpd (`httpd_req_async_handler_begin`) and serviced non-blocking from `Webserver::loop()` — **no extra FreeRTOS task per connection**. Multiple clients (e.g. several browser tabs on `/console`) work in parallel. The number of simultaneous connections is bounded by `OPENKNX_WEBSERVER_MAX_CONN` (default 7), which HTTP and WebSocket share; a request that finds no free slot simply does not get one. Raise the limit via the compile flag, and increase `CONFIG_LWIP_MAX_SOCKETS` in the parent project if you push it substantially higher.

### Callback context

**Every receive handler runs inside a network callback and must not block.** That holds for
`addSocket()`'s `onMessage` and for `webserver.ui.receiveMessage()` alike:

| | ESP32 | RP2040 |
|---|---|---|
| `onMessage` / `receiveMessage()` | loop task | **lwIP callback — an interrupt** on boards with `PIN_ETH_INT` |
| `onConnect(fd, true)` | loop task | loop task |
| `onConnect(fd, false)` | loop task | **lwIP callback** |

On RP2040 the W5500 drives lwIP from its interrupt pin, so a receive handler preempts the main loop.
Do not allocate, log, run a console command or build a large JSON there — record what you need and do
the work in your own `loop()`. All three in-tree handlers do exactly that: `Webconsole` buffers the
command into `_pendingCmd`, the `prog` key sets `_pendingProg`, and `busmon` sets
`GroupMonitor::_pendingBusmon` — the last one because `startMonitoring()`/`reset()` queue a control
byte into a lock-free ring whose producer and consumer must be the same context.

Only `onConnect(fd, true)` is delivered from `loop()` and may do as much as it likes — that is where
initial per-client state belongs. Its counterpart, the disconnect notification, deliberately stays
immediate so a recycled slot cannot have the *new* client's state torn down by a late callback.

### Navigation Menu

```cpp
openknxNetwork.webserver.addMenuItem("Status",  "/status");
openknxNetwork.webserver.addMenuItem("Devices", "/devices");
```

### Assets (Stylesheets and Scripts)

```cpp
// Register a CSS file in <head>
openknxNetwork.webserver.addStylesheet("/assets/custom.css");

// Register a JavaScript file before </body> (with defer attribute)
openknxNetwork.webserver.addJavaScript("/assets/custom.js");
```

Registered assets are automatically included in the HTML layout:
- **Stylesheets** are inserted in `<head>`
- **Scripts** are inserted before the `</body>` tag (with `defer` for non-blocking execution)
- Both automatically receive a **cache-buster** query parameter in the format `YYYYMMDDHHII` (e.g. `?202605231435`), generated from build time — prevents caching issues after updates

### Built-in Assets (`web/assets/`, generated at build time)

This module's own CSS/JS/SVG (`base.css`, `groupmonitor.js`, the logo, ...) are **not** hand-minified C++ string literals — they're plain, readable source files under `web/assets/` at the repo root. `OGM-Common/scripts/pio/prepare.py` runs on every build, collects `web/assets/` from every included module (and the project itself), minifies and gzip-compresses each file, and writes `include/webassets.h` with, per file:

```cpp
namespace WebAssets {
    inline const uint8_t base_css_gz[] = { /* gzip bytes */ };
    inline const char* const base_css_mime = "text/css";
}
```

Serve one with `Webserver::Asset()`, which delegates to `WebResponse::sendAsset()`:

```cpp
addRoute(WEB_GET, "/assets/custom.css",
    Asset(WebAssets::custom_css_mime, WebAssets::custom_css_gz, sizeof(WebAssets::custom_css_gz)));
```

No `_gz_len` symbol — the array size is known at compile time via `sizeof()`. There is no content negotiation: the response always carries `Content-Encoding: gzip`, on the assumption that every real browser accepts it (true for years now). Fetching such a URL with a plain `curl` (no `--compressed`) returns binary, not the source text.

```cpp
void MyModule::buildMyPage(WebResponse& res) {
    std::string html = "<div class='container'>";
    html += "<h1>My Page</h1>";
    html += "<p>Content...</p>";
    html += "</div>";

    res.setContentType("text/html");
    res.setLayout(true);
    res.send(html.c_str());
}

// With an explicit active menu item (for pages without their own menu entry)
void MyModule::buildDetailPage(WebResponse& res) {
    std::string html = "<div class='container'><h1>Detail</h1></div>";

    res.setContentType("text/html");
    res.setLayout(true);
    res.setActiveMenu("/mypage");   // mark "/mypage" as active
    res.send(html.c_str());
}
```

---

## UI Socket

One WebSocket for the whole web interface, open on **every** page: `/openknx/ws`. It replaces the
former per-feature endpoints (`/console`, `/groupmonitor`) — those pages still exist, only their own
sockets are gone. The URI is reserved; `addSocket()` matches exactly and has no dedup, so a module
registering `/openknx/ws` itself would silently lose to the first entry.

### Wire format

`key:payload` — split at the **first** colon, everything after it stays raw. Nothing is escaped and
no envelope is built, so a log line costs one key plus one byte on the wire and the payload may be
JSON without double-encoding.

### Firmware API

```cpp
// Sending: always broadcast. len = 0 means strlen().
openknxNetwork.webserver.ui.sendMessage("tp", json);
openknxNetwork.webserver.ui.sendMessage("log", line.data(), line.size());

// Receiving: every registrant sees every incoming message and checks the key itself
openknxNetwork.webserver.ui.receiveMessage([](const char* key, const char* data, size_t len) {
    if (strcmp(key, "cmd") != 0) return;
    …
});

// Is anyone looking? Cheaper than hasClients(UI_WS_URI) — no std::string per call
if (!openknxNetwork.webserver.ui.hasClients()) return;

// Largest message the queue accepts in one piece — for "key:data" together,
// so subtract strlen(key) + 1 from your own payload budget
size_t cap = openknxNetwork.webserver.ui.maxMessage();
```

Everything specific to this socket lives on `ui`. The send queue underneath serves every
`addSocket()` endpoint, so its `webserver.txCapacity()` / `webserver.txFree()` stay on the webserver.

- **A receive handler is a network callback — keep it short.** See *Callback context* above; it
  applies to `receiveMessage()` exactly as it does to `addSocket()`. The `prog` handler in
  `Webserver.cpp` is the in-tree example: it only records the request and `loop()` acts on it.
- **`key` is NUL-terminated, `data` is not.** `data` points straight into the connection's receive
  buffer, so `len` is the only bound — a `strcmp(data, …)` reads past the frame into whatever the
  slot held before. Compare with `memcmp`/`len`, or copy out first. The in-tree handlers use `len`
  (`cmd`) or a single byte (`prog`, `busmon`).
- **A key is at most 23 characters** on the inbound path; longer ones are dropped without notice.
  Sending is not limited, so a longer key would go out and never come back.
- **Who only sends, registers nothing.** `tp` and `log` have no handler at all.
- **The firmware stores no keys** — no lookup table, no "unknown key" handling. The dispatcher
  splits at the first `:` and hands `key`/`data`/`len` to every registrant. Incoming traffic is rare
  (a console command, a prog click, the busmonitor switch), so the `strcmp` per registrant does not
  matter. Same pattern as `Network::Module::registerCallback()`.
- **`connect` is a reserved key**: `cb("connect", nullptr, 0)` means *a client connected, send your
  current state*. A reconnect is indistinguishable from a first connect — that is the point, the
  client resynchronises completely without anything having changed on the device.
- **No periodic sending, and no change detection in the layer.** There is no tick; when to send is
  the producer's decision (its own hook, or a comparison in `loop()`). On an idle device the socket
  stays silent; keepalive is handled by what already exists (the WS ping from RP2040's `onPoll`,
  TCP keepalives on ESP32).
- **No unicast here.** The only candidate was the initial sync, and that works just as well as a
  broadcast: one `connect` triggers a single ~200 B `status` for everyone, once per page load. So no
  producer ever sees an `fd`. Unicast stays available one layer down
  (`sendWebsocketMessage(uri, msg, fd)`).

### Keys

| Key | Direction | Payload | Flag |
|-----|-----------|---------|------|
| `connect` | internal | reserved: "a client connected", sent to all handlers — **not accepted from a client** | `OPENKNX_WEBSERVER` |
| `ping` / `pong` | in / out | liveness check, empty payload — see below; `pong` is outbound only | `OPENKNX_WEBSERVER` |
| `status` | out | device state as JSON — **only** on `connect` and on change | `OPENKNX_WEBSERVER` |
| `prog` | in | `1` / `0` / `t` (toggle) → `knx.progMode()` | `OPENKNX_WEBSERVER` |
| `log` | out | raw log line (UTF-8, ANSI escapes included) | `OPENKNX_WEBCONSOLE` |
| `cmd` | in | console command | `OPENKNX_WEBCONSOLE` |
| `tp` | out | one KNX telegram as JSON (`buildTelegramJson`) | `OPENKNX_WEBMONITOR` |
| `busmon` | in | `1` / `0` → `startMonitoring()` / `reset()` | `OPENKNX_WEBMONITOR`, mask ≠ `0x091A` |

`connect` and `pong` are **reserved for the firmware's own use and dropped on the inbound path**. The
endpoint is unauthenticated, and a client sending `connect:` would otherwise run the full
`publishStatus()` inside the network callback — on RP2040 the ethernet interrupt, `delay(1)` of the
temperature ADC included.

`status` carries `prog`, `cfg`, `pa`, `net`, `mem` (KiB), `cpu`, the four IP strings and `busmon`.
Two fields need care so the comparison does not fire on every tick: **`up` (uptime) is only included
on `connect`** and the browser counts on from there, and `mem`/`cpu` are coarsely rounded. Values go
out raw; formatting is the browser's job.

### Browser API (`base.js`)

```js
OpenKNX.on('log', line => { … });      // register for a key
OpenKNX.off('log', fn);
OpenKNX.send('cmd', 'help');           // sends "cmd:help"
OpenKNX.onState(s => { … });           // 'open' | 'closed'
OpenKNX.connected                      // bool
```

**Liveness.** The device detects dead clients by itself (a WebSocket ping from RP2040's `onPoll`,
TCP keepalives on ESP32), but nothing covers the other direction: a rebooted device sends neither FIN
nor RST, so the browser would sit on an open socket — status dot green — until it next sends
something itself. `base.js` therefore sends `ping:` after a second of silence and closes the socket 2 s later if no
answer arrived, which triggers the normal reconnect — so a reboot shows up in about three seconds.

**It only ever concludes from an unanswered question, never from silence alone.** A background tab
gets its `setInterval` throttled to about once a minute; judging by elapsed time would then tear down
a perfectly healthy connection once per minute. The check therefore measures how long the timer
itself slept and, if that is far more than its interval (throttled tab, suspended machine), only
re-bases and draws no conclusion. Returning to the foreground re-bases too, so the first tick after
throttling does not judge a stale interval. The firmware answers
`ping` with an empty `pong`.

That costs 5 bytes each way per second while the device is idle, and nothing at all while it has
traffic, because every received message resets the timer. The `pong` is a broadcast, so it also
resets the timer in every other open tab — the rate stays at roughly one ping per second in total,
however many pages are open.

`base.js` is registered first and loaded with `defer` on every page, so `window.OpenKNX` exists
before any page script runs. It opens one socket, derives the scheme from `location.protocol`
(`wss:` under HTTPS) and reconnects with a 1 s → 30 s backoff. A throwing handler is caught per
callback so it cannot take the socket down with it.

---

## Send Queue

The WS transport keeps an outbound queue, so **no endpoint loses frames** any more — the former
behaviour was to drop a frame when the client's TCP send buffer was momentarily full
(RP2040 `tcp_sndbuf()`, ESP32 `EAGAIN` → `WsSendResult::WouldBlock`). This sits *below* the UI layer,
so a module with its own `addSocket()` endpoint benefits without doing anything.

- **One shared record ring for all clients**, plus a 16-byte cursor entry each: memory does not scale with
  the client count, and a broadcast is stored once instead of once per client. Size via
  `OPENKNX_WEBSOCKET_TX_BUFFER` (default 4 KB), allocated once from PSRAM where available.
- **A stalled client blocks nobody.** Eviction happens at the oldest end, the farthest point from
  the write position; a client that keeps up sits close to the write position and cannot be caught
  by it. Skipping records addressed to someone else advances that client's cursor too, so a client
  on a quiet endpoint never falls behind because of foreign traffic.
- **A client is disconnected only when it stops reading**, for one of two reasons: it was overtaken
  in the ring, or it accepted nothing for 10 s while records were waiting for it. Being merely slower
  than a producer that is writing a lot is *not* a reason — producers throttle on the queue's free
  space instead of filling it. Both in-tree producers do: the console enqueues at most a quarter of
  the ring per tick and never more than is currently free (its lines are held in the logger ring
  anyway and can wait), and the group monitor drops a telegram outright once free space falls below
  a quarter of the ring. Without that, a burst would fill the ring, overtake a briefly slow browser
  and tear it down — a reconnect loop instead of a dropped frame.

  The console budget is a *per-tick* allowance, not a per-message limit: the first line of a tick
  always goes out, however large. Otherwise a single line whose UTF-8 form exceeds the allowance
  could never be sent and the read position would stand still for the rest of the boot.
- **A record is capped at `min(ring/2 - 5, maxWebsocketPayload(), 65535)`** — query it via
  `webserver.ui.maxMessage()`. With the default 4 KB ring that is 1496 B on RP2040 (the fixed WS
  frame buffer) and 2043 B on ESP32 (half the ring, since its frame size is unbounded). All three
  bounds matter: half the ring so one message cannot evict everything, the frame limit because a
  larger record could never be sent and the drain would park on it forever, and 65535 because the
  record header carries the length in 16 bits.

  **The limit covers `key:data` together**, so subtract `strlen(key) + 1` from your own payload
  budget — the console's `maxLine` does exactly that with the 4 bytes of `log:`. Anything larger is
  rejected and logged once per key; a producer that can generate unbounded payloads must chunk them
  itself, as the console does.
- **The allocation is a precondition for the server**, not an optional extra: `start()` refuses to
  open the port when the ring is missing, so `Module::loop()` retries (at most once a second)
  instead of leaving a running server whose socket can never send.
- If the ring cannot be allocated at startup the device logs an error and WebSocket sending stays
  off — there is deliberately no second, differently-behaving direct-send path.
- The sending primitive used by the drain is internal. `sendWebsocketMessage()` and `sendToClient()`
  both queue; only the drain writes sockets, and it is also the place that tears down a client whose
  send fails permanently, so both platforms behave the same.

---

## Layout Convention: `.container`

`buildHeader()` emits only `<nav>` + `<main>` — **no** wrapping `<div class='container'>`.
Each page decides independently whether to use the container div.

### Page with bounded width (default case)

Content inside `<div class='container'>` is limited to `max-width: 960px`:

```cpp
std::string html = "<div class='container'>";
html += "<h1>My Page</h1>";
html += "<p>Content...</p>";
html += "</div>";
```

### Page without container (fullscreen)

Pages like the web console that should fill the entire `<main>` area omit the container div.
`main` is a flex-column container (`flex:1`), so direct children with `flex:1` take the full height:

```cpp
// CSS (inline <style> or a separate .css file):
// main{height:100vh}  – pins the height
// #my-wrap{flex:1}    – fills the remaining space

std::string html = "<div id='my-wrap'>...</div>";
```

No C++ flag required — controlled purely via CSS.

---

## WebRequest / WebResponse

```cpp
class WebRequest {
    std::string uri;
    uint8_t     method;   // WEB_GET, WEB_POST, WEB_PUT, WEB_DELETE
};

class WebResponse {
    void setStatus(uint16_t code);          // default: 200
    void setContentType(const char* mime);  // default: "text/html"
    void setHeader(const char* name, const char* value);

    // Dynamic data — copied, allocated fresh per request
    void send(const char* text);
    void send(const uint8_t* data, int length);

    // Static data in flash — no copy, pointer is passed directly
    // Use for: static const char[], PROGMEM arrays, embedded assets
    void sendStatic(const char* text);
    void sendStatic(const uint8_t* data, int length);

    // Gzip-compressed generated web asset (webassets.h) — sets Content-Type,
    // Cache-Control and Content-Encoding: gzip, body is a static reference
    void sendAsset(const char* mimeType, const uint8_t* data, size_t length);

    // 301 redirect, empty body
    void sendRedirect(const std::string& target);
};
```

**Note:** `sendStatic()` must only be called with pointers to static data that remain valid for the entire
application lifetime. Ideal for embedded assets (CSS, SVG, HTML templates) declared as `static const char[]`.
The data is not copied into RAM, saving memory — especially on RP2040 with limited RAM. On RP2040 the static
body is handed to lwIP **zero-copy** (`tcp_write` without `TCP_WRITE_FLAG_COPY`): lwIP keeps a `PBUF_REF` to the
flash data until ACK instead of copying it into the pbuf pool — which is why the pointer must stay valid for the
whole application lifetime.

**Note on `setLayout(true)`:** header, body and footer are **not** concatenated into one buffer. `Webserver::handleRequest()`
builds a segment list (`ResponseSegment[]`, max 3: header/body/footer) on the `WebResponse`; the platform sends each
segment directly — one `httpd_resp_send_chunk()` (or `tcp_write()`) per segment, no merge allocation on either platform,
regardless of whether layout is active.

---

## Built-in Pages

| URI | Flag | Content |
|-----|------|---------|
| `/` | `OPENKNX_WEBSERVER` | Overview: device, firmware, network, uptime, versions |
| `/prog` (**POST**) | `OPENKNX_WEBSERVER` | Toggles the KNX programming mode, then `303` back to `/` |
| `/console` | `OPENKNX_WEBCONSOLE` | WebSocket terminal — mirrors the serial logger |
| `/groupmonitor` | `OPENKNX_WEBMONITOR` | Live KNX group/telegram monitor (TP only) |
| `/filemanager` | `OPENKNX_WEBFS` | Directory listing and upload form |
| `/filemanager/download` | `OPENKNX_WEBFS` | Streams one file as `application/octet-stream` |
| `/filemanager/upload` (**POST**) | `OPENKNX_WEBFS` | One chunk per request (`offset` query param) |
| `/filemanager/delete` (**POST**) | `OPENKNX_WEBFS` | Deletes a file or an empty directory |
| `/filemanager/mkdir` (**POST**) | `OPENKNX_WEBFS` | Creates a directory |
| `/assets/logo.svg` | `OPENKNX_WEBSERVER` | OpenKNX logo (generated, gzip) |

`/prog` is deliberately **POST only** and is rendered as a form in the nav footer, not as a link. As a `GET` it would be triggerable from any foreign page via `<img src="http://device/prog?mode=1">`. The `mode` value still travels in the query string, so `getQueryParam()` keeps working; only the method changed.

### Console Page (`OPENKNX_WEBCONSOLE`)

- Streams the serial logger live over the shared UI socket (`log` out, `cmd` in) — the page has no
  WebSocket code of its own. **Reconnect is automatic** (it lives in `base.js` and every page needs
  it), but a gap is made visible: the console writes `[Verbindung getrennt — Lücke im Log]` and
  `[verbunden]` as marker lines, so silently missing output stays impossible
- CSS and JS live in `web/assets/console.{css,js}` and are served from the generated `webassets.h` like every other asset; the route only emits the HTML skeleton
- **The device sends raw log lines, ANSI escapes included — it never generates markup.** The browser's own JS parses the escapes and renders each segment via `textContent` (never `innerHTML`), mapping codes to CSS classes:

| ANSI code | CSS class |
|-----------|-----------|
| `\x1B[31m` | `.red` |
| `\x1B[32m` | `.green` |
| `\x1B[33m` | `.yellow` |
| `\x1B[90m` | `.gray` |

- **Losing log is made visible.** If the device writes more into the logger ring than the console has
  drained — a `processCommand()` whose output exceeds `OPENKNX_WEBCONSOLE_BUFFER` overruns it before
  anyone can read it — the console emits a yellow `[... N Bytes Log verworfen ...]` marker in the
  stream instead of silently skipping ahead. The other way to lose lines, a client so far behind that
  the send queue overtakes it, ends in a disconnect and is marked by `console.js` as a gap
- New clients receive log lines from the moment of connection onward (no backlog). There is one read
  position for all clients now — a log line is relevant to every one of them, and the send queue holds
  it. Per tick the console enqueues at most half the queue, so a burst like `showInformations` cannot
  flood it; the rest follows next tick out of the logger ring
- Commands arrive as `cmd:<text>`; only printable ASCII (`0x20–0x7E`, `\r\n\t`) is accepted

### Group Monitor Page (`OPENKNX_WEBMONITOR`, TP only)

ETS-style live view of all KNX telegrams on the TP bus. Read-only — no telegrams can be sent.

- Taps **every** received frame in parallel, bypassing the KNX stack, via the TP-UART `registerReceivedFrame` hook (the same mechanism MQTT raw-frame publishing uses). No filtering — group and individual addressed frames, transmitted and repeated frames are all shown.
- Each telegram is decoded to a table row: arrival time (client-side), flags, source individual address, destination address, APCI type (`Read` / `Write` / `Response` for group telegrams), **name**, **value**, **DPT**, raw payload as hex, and payload length. Flags are the 9-char string from the TP-UART (`T`=transmitted, `D`=addressed, `I`=invalid, `E`=extended, `R`=repeated, `F`=filtered, `B`=busy, `N`=nack, `A`=ack), `_` where not set.
- The device decodes each `TPUart::Frame` and broadcasts it as compact JSON under the `tp` key of the UI socket; the browser renders the rows. New clients see only telegrams arriving after connect (no backlog). Toolbar offers autoscroll, a Start/Stop recording toggle, clear, and — on a plain TP device (mask `0x07B0`) only — a Busmonitor switch (puts the TP-UART into bus-monitor mode — telegrams are no longer processed; deactivating does a TP reset). On a TP+IP router (mask `0x091A`) the switch is not shown and the `busmon` key is not registered, since bus-monitor mode there would stop TP↔IP routing, not just this feature. The current monitoring state is a field in `status`, so a freshly connected page shows it without asking for it. The table is capped at 1000 rows.

#### Name, Value and DPT — optional `/openknx_ga.tsv`

Name, value and DPT come from an optional tab-separated file `/openknx_ga.tsv` on LittleFS (columns `ga`, `dpt`, `subset`, `hauptgruppe`, `mittelgruppe`, `name`; first line is a header). It is the same export an ETS group-address dump produces. Upload it via the File Manager. The work is split to keep the device's RAM footprint minimal:

- **Value and DPT (backend):** at startup the device loads the TSV into a compact in-RAM table `GA → (DPT, subtype)` (4 bytes per GA, no strings; `openknxNetwork.gatable`). For each Write/Response telegram it decodes the payload with the KNX stack's own DPT converter and sends the formatted value plus the DPT. Supported value types: DPT 1, 5 (5.001 as `%`), 6, 7, 8, 9, 12, 13, 14, 16, 17, 18; other DPTs show the raw hex only.
- **Names (browser):** the page loads the TSV itself once and resolves names client-side, so the device never has to hold the (arbitrarily long) name strings in RAM. It is served by the group monitor's own route `/groupmonitor/ga.tsv` (streamed from LittleFS, never read into RAM) — deliberately not through the file manager, which sits behind `OPENKNX_WEBFS` and would leave the name columns empty whenever that flag is off. **Without the file the route answers `200` with an empty body**, not `404`: the columns show `-` either way, and a 404 would be logged as a warning on every page view (see [Access Log](#access-log)).
- If the file is missing, Name/Value/DPT simply show `-` and everything else keeps working.
- Only available on devices with a TP connection (`MASK_VERSION == 0x07B0`); on other masks the page and its WebSocket are not registered.

### File Manager Page (`OPENKNX_WEBFS`)

Browse, download, upload and delete files on the device's storage. Directory and file names are HTML- and JS-escaped in the listing; `..` in a path is rejected outright (`sanitizePath()`).

- **Chunked upload:** the browser slices the file and sends **one POST per chunk**, each with an `offset` query parameter — the device never has to hold a whole file in RAM. `offset=0` on an existing file is refused with `409` so an upload cannot silently overwrite. After every chunk the resulting file size is compared against `offset + length`: LittleFS only writes on `close()` and `write()` always reports the buffer size, so this is the one reliable way to detect a full filesystem (→ `413`, partial file removed).
- **Listing is streamed, not collected:** the directory is walked twice (directories pass, files pass) and rows are appended straight to the HTML. Nothing is buffered in a container — 250 entries in a `std::vector<std::string>` would cost ~10 KB heap, which RP2040 cannot spare next to lwIP and the connection slots. Listings are capped at 250 entries per pass, with a note in the table when the cap is hit.
- **Downloads stream** via `WebResponse::sendStream()`; on ESP32 this runs synchronously in the httpd task (see the note under Platform Support).
- **A missing drive or file answers `404`** on every route. `400` is reserved for a malformed path, `409`/`413` for an upload that would overwrite or does not fit, `500` only for an operation that actually failed (write error, non-empty directory). A directory that does not exist is not an error — it renders as an empty listing.
