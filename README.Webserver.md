# Webserver — OFM-Network

Built-in HTTP/WebSocket server for OpenKNX devices.

---

## Compile Flags

```
OPENKNX_WEBSERVER           – HTTP server, routing, overview page (/), logo
OPENKNX_WEBCONSOLE          – /console page, WS /console, logger streaming
OPENKNX_WEBCONSOLE_BUFFER   – Total size of the log ring buffer in bytes (default: 4096)
OPENKNX_WEBSOCKET_MAX       – ESP32 only: max concurrent WebSocket connections (default 3, excess → HTTP 503)
OPENKNX_WEBSERVER_MAX_CONN  – RP2040 only: total webserver connection slots, shared HTTP+WS (default 3); raising requires more lwIP PCBs
OPENKNX_WEBSOCKET_RX_CAP    – Per-WS RX cap in bytes (ESP32 accumulation buffer default 2048, overflow → disconnect; RP2040 payload buffer default 512, overflow → truncate)
OPENKNX_WEBMONITOR        – /groupmonitor page, WS /groupmonitor, live KNX telegram stream (TP only, MASK_VERSION == 0x07B0)
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

// Unicast with success return value (false = client disconnected)
bool ok = openknxNetwork.webserver.sendToClient("/ws", clientId, data, len);

// Snapshot of connected client IDs for a URI
std::vector<int> fds = openknxNetwork.webserver.connectedClientFds("/ws");
```

On **RP2040** `sendWebsocketMessage` / `sendToClient` must only be called from `loop()` (single-threaded lwIP).  
On **ESP32** both methods are callable from any task (non-blocking); the registered `onMessage` callback runs in the main loop task.

> **ESP32 concurrency:** WebSocket connections are detached from httpd (`httpd_req_async_handler_begin`) and serviced non-blocking from `Webserver::loop()` — **no extra FreeRTOS task per connection**. Multiple clients (e.g. several browser tabs on `/console`) work in parallel. The number of simultaneous connections is capped by `OPENKNX_WEBSOCKET_MAX` (default 3); beyond that, upgrade requests are answered with **HTTP 503**. Raise the limit via the compile flag, and increase `CONFIG_LWIP_MAX_SOCKETS` in the parent project if you push it substantially higher.

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

- Streams the serial logger live via WebSocket
- CSS and JS live in `web/assets/console.{css,js}` and are served from the generated `webassets.h` like every other asset; the route only emits the HTML skeleton
- **The device sends raw log lines, ANSI escapes included — it never generates markup.** The browser's own JS parses the escapes and renders each segment via `textContent` (never `innerHTML`), mapping codes to CSS classes:

| ANSI code | CSS class |
|-----------|-----------|
| `\x1B[31m` | `.red` |
| `\x1B[32m` | `.green` |
| `\x1B[33m` | `.yellow` |
| `\x1B[90m` | `.gray` |

- New clients receive log lines from the moment of connection onward (no backlog)
- Commands can be sent as text frames; only printable ASCII (`0x20–0x7E`, `\r\n\t`) is accepted

### Group Monitor Page (`OPENKNX_WEBMONITOR`, TP only)

ETS-style live view of all KNX telegrams on the TP bus. Read-only — no telegrams can be sent.

- Taps **every** received frame in parallel, bypassing the KNX stack, via the TP-UART `registerReceivedFrame` hook (the same mechanism MQTT raw-frame publishing uses). No filtering — group and individual addressed frames, transmitted and repeated frames are all shown.
- Each telegram is decoded to a table row: arrival time (client-side), flags, source individual address, destination address, APCI type (`Read` / `Write` / `Response` for group telegrams), **name**, **value**, **DPT**, raw payload as hex, and payload length. Flags are the 9-char string from the TP-UART (`T`=transmitted, `D`=addressed, `I`=invalid, `E`=extended, `R`=repeated, `F`=filtered, `B`=busy, `N`=nack, `A`=ack), `_` where not set.
- The device decodes each `TPUart::Frame` and broadcasts it as compact JSON over the `/groupmonitor` WebSocket; the browser renders the rows. New clients see only telegrams arriving after connect (no backlog). Toolbar offers autoscroll, a Start/Stop recording toggle, clear, and a Busmonitor switch (puts the TP-UART into bus-monitor mode — telegrams are no longer processed; deactivating does a TP reset). The table is capped at 1000 rows.

#### Name, Value and DPT — optional `/openknx_ga.tsv`

Name, value and DPT come from an optional tab-separated file `/openknx_ga.tsv` on LittleFS (columns `ga`, `dpt`, `subset`, `hauptgruppe`, `mittelgruppe`, `name`; first line is a header). It is the same export an ETS group-address dump produces. Upload it via the File Manager. The work is split to keep the device's RAM footprint minimal:

- **Value and DPT (backend):** at startup the device loads the TSV into a compact in-RAM table `GA → (DPT, subtype)` (4 bytes per GA, no strings; `openknxNetwork.gatable`). For each Write/Response telegram it decodes the payload with the KNX stack's own DPT converter and sends the formatted value plus the DPT. Supported value types: DPT 1, 5 (5.001 as `%`), 6, 7, 8, 9, 12, 13, 14, 16, 17, 18; other DPTs show the raw hex only.
- **Names (browser):** the page loads the TSV itself once and resolves names client-side, so the device never has to hold the (arbitrarily long) name strings in RAM.
- If the file is missing, Name/Value/DPT simply show `-` and everything else keeps working.
- Only available on devices with a TP connection (`MASK_VERSION == 0x07B0`); on other masks the page and its WebSocket are not registered.

### File Manager Page (`OPENKNX_WEBFS`)

Browse, download, upload and delete files on the device's storage. Directory and file names are HTML- and JS-escaped in the listing; `..` in a path is rejected outright (`sanitizePath()`).

- **Chunked upload:** the browser slices the file and sends **one POST per chunk**, each with an `offset` query parameter — the device never has to hold a whole file in RAM. `offset=0` on an existing file is refused with `409` so an upload cannot silently overwrite. After every chunk the resulting file size is compared against `offset + length`: LittleFS only writes on `close()` and `write()` always reports the buffer size, so this is the one reliable way to detect a full filesystem (→ `413`, partial file removed).
- **Listing is streamed, not collected:** the directory is walked twice (directories pass, files pass) and rows are appended straight to the HTML. Nothing is buffered in a container — 250 entries in a `std::vector<std::string>` would cost ~10 KB heap, which RP2040 cannot spare next to lwIP and the connection slots. Listings are capped at 250 entries per pass, with a note in the table when the cap is hit.
- **Downloads stream** via `WebResponse::sendStream()`; on ESP32 this runs synchronously in the httpd task (see the note under Platform Support).
- **A missing drive or file answers `404`** on every route. `400` is reserved for a malformed path, `409`/`413` for an upload that would overwrite or does not fit, `500` only for an operation that actually failed (write error, non-empty directory). A directory that does not exist is not an error — it renders as an empty listing.
