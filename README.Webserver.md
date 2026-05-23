# Webserver — OFM-Network

Built-in HTTP/WebSocket server for OpenKNX devices.

---

## Compile Flags

```
OPENKNX_WEBSERVER           – HTTP server, routing, overview page (/), logo
OPENKNX_WEBCONSOLE          – /console page, WS /console, logger streaming
OPENKNX_WEBCONSOLE_BUFFER   – Total size of the log ring buffer in bytes (default: 4096)
```

`OPENKNX_WEBCONSOLE` requires `OPENKNX_WEBSERVER`.  
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
On **ESP32** both methods are callable from any task (non-blocking).

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
};
```

**Note:** `sendStatic()` must only be called with pointers to static data that remain valid for the entire
application lifetime. Ideal for embedded assets (CSS, SVG, HTML templates) declared as `static const char[]`.
The data is not copied into RAM, saving memory — especially on RP2040 with limited RAM.

---

## Built-in Pages

| URI | Flag | Content |
|-----|------|---------|
| `/` | `OPENKNX_WEBSERVER` | Overview: device, firmware, network, uptime, versions |
| `/console` | `OPENKNX_WEBCONSOLE` | WebSocket terminal — mirrors the serial logger |
| `/assets/logo/black.svg` | `OPENKNX_WEBSERVER` | OpenKNX logo (inline SVG) |

### Console Page (`OPENKNX_WEBCONSOLE`)

- Streams the serial logger live via WebSocket
- ANSI colors are converted to CSS classes

| ANSI code | CSS class |
|-----------|-----------|
| `\x1B[31m` | `.red` |
| `\x1B[32m` | `.green` |
| `\x1B[33m` | `.yellow` |
| `\x1B[90m` | `.gray` |

- New clients receive log lines from the moment of connection onward (no backlog)
- Commands can be sent as text frames; only printable ASCII (`0x20–0x7E`, `\r\n\t`) is accepted
