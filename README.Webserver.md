# Webserver — OFM-Network

Integrierter HTTP/WebSocket-Server für OpenKNX-Geräte.

---

## Compile-Flags

```
OPENKNX_WEBSERVER           – HTTP-Server, Routing, Übersichtsseite (/), Logo
OPENKNX_WEBCONSOLE          – /console-Seite, WS /console, Logger-Streaming
OPENKNX_WEBCONSOLE_BUFFER   – Gesamtgröße des Log-Ring-Buffers in Bytes (default: 4096)
```

`OPENKNX_WEBCONSOLE` setzt `OPENKNX_WEBSERVER` voraus.  
Ohne `OPENKNX_WEBSERVER` wird keinerlei Webserver-Code compiliert.

---

## Startbedingung

Der Webserver startet sobald das Netzwerk verfügbar ist und eine der folgenden Bedingungen erfüllt ist:

- `ParamNET_HTTP == true` — Benutzer hat den Webserver via ETS-Parameter aktiviert
- **Gerät nicht konfiguriert** (`!knx.configured()`) — der Webserver startet immer, unabhängig vom Parameter

Das erlaubt Erstzugriff auf ein frisch geflashtes oder unkonfiguriertes Gerät ohne ETS-Programmierung.

---

## API

### Routen registrieren

```cpp
// GET /status
openknxNetwork.webserver.addRoute(WEB_GET, "/status", [](WebRequest& req, WebResponse& res) {
    res.setContentType("application/json");
    res.send("{\"ok\":true}");
});

// POST /config
openknxNetwork.webserver.addRoute(WEB_POST, "/config", handler);

// Wildcard-Prefix: matcht /files/ und alles darunter
openknxNetwork.webserver.addRoute(WEB_GET, "/files/*", handler);
```

Unterstützte Methoden: `WEB_GET`, `WEB_POST`, `WEB_PUT`, `WEB_DELETE`.

### Static-Helpers

```cpp
// Redirect
openknxNetwork.webserver.addRoute(WEB_GET, "/old",
    Webserver::Redirect("/new"));

// Inline-Text (z. B. CSS, JSON)
openknxNetwork.webserver.addRoute(WEB_GET, "/robots.txt",
    Webserver::Static("text/plain", "User-agent: *\nDisallow: /"));

// Binärdaten aus PROGMEM
extern const uint8_t icon_png[];
extern const int     icon_png_len;
openknxNetwork.webserver.addRoute(WEB_GET, "/favicon.ico",
    Webserver::Static("image/png", icon_png, icon_png_len));
```

### WebSocket-Endpunkte

```cpp
openknxNetwork.webserver.addSocket("/ws",
    // onMessage
    [](int clientId, WebSocketFrame* frame) {
        std::string msg((char*)frame->data, frame->length);
        openknxNetwork.webserver.sendWebsocketMessage("/ws", "pong", clientId);
    },
    // onConnect (optional)
    [](int clientId, bool connected) {
        // connected == true: neuer Client
        // connected == false: Client getrennt
    }
);

// Broadcast an alle Clients auf /ws
openknxNetwork.webserver.sendWebsocketMessage("/ws", "hello");

// Unicast an einen bestimmten Client
openknxNetwork.webserver.sendWebsocketMessage("/ws", "hello", clientId);

// Unicast mit Erfolgs-Rückgabe (false = Client getrennt)
bool ok = openknxNetwork.webserver.sendToClient("/ws", clientId, data, len);

// Snapshot der verbundenen Client-IDs für eine URI
std::vector<int> fds = openknxNetwork.webserver.connectedClientFds("/ws");
```

Auf **RP2040** darf `sendWebsocketMessage` / `sendToClient` nur aus `loop()` aufgerufen werden (single-threaded lwIP).  
Auf **ESP32** sind beide Methoden aus jedem Task aufrufbar (non-blocking).

### Navigationsmenü

```cpp
openknxNetwork.webserver.addMenuItem("Status",  "/status");
openknxNetwork.webserver.addMenuItem("Geräte",  "/devices");
```

### Layout einbetten

```cpp
void MyModule::buildMyPage(WebResponse& res) {
    std::string html = "<h2>Meine Seite</h2>";
    html += "<p>Inhalt...</p>";
    
    res.setContentType("text/html");
    res.setLayout(true);
    res.send(html.c_str());
}

// Mit explizitem aktiven Menüpunkt (für Seiten ohne eigenen Menü-Eintrag)
void MyModule::buildDetailPage(WebResponse& res) {
    std::string html = "<h2>Detailseite</h2>";
    
    res.setContentType("text/html");
    res.setLayout(true);
    res.setActiveMenu("/mypage");   // Markiere "/mypage" als aktiv
    res.send(html.c_str());
}
```

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
    void addHeader(const char* name, const char* value);
    
    // Dynamische Daten — wird kopiert, für jede Anfrage neu allokiert
    void send(const char* text);
    void send(const uint8_t* data, int length);
    
    // Statische Daten im Flash — kein Kopie, Zeiger wird direkt übergeben
    // Nutzen bei: static const char[], PROGMEM-Arrays, Embedded Assets
    void sendStatic(const char* text);
    void sendStatic(const uint8_t* data, int length);
};
```

**Hinweis:** `sendStatic()` darf nur mit Zeigern auf statische Daten aufgerufen werden, die für die gesamte
Anwendungslaufzeit gültig bleiben. Dies ist ideal für Embedded Assets (CSS, SVG, HTML-Templates), die im
`static const char[]` deklariert sind. Die Daten werden nicht in RAM kopiert und sparen damit Speicher,
insbesondere auf RP2040 mit limitiertem RAM.

---

## Eingebaute Seiten

| URI | Flag | Inhalt |
|-----|------|--------|
| `/` | `OPENKNX_WEBSERVER` | Übersicht: Gerät, Firmware, Netzwerk, Laufzeit, Versionen |
| `/console` | `OPENKNX_WEBCONSOLE` | WebSocket-Terminal — spiegelt den seriellen Logger |
| `/assets/logo/black.svg` | `OPENKNX_WEBSERVER` | OpenKNX-Logo (inline SVG) |

### Console-Seite (`OPENKNX_WEBCONSOLE`)

- Streamt den seriellen Logger live per WebSocket
- ANSI-Farben werden in CSS-Klassen umgewandelt

| ANSI-Code | CSS-Klasse |
|-----------|------------|
| `\x1B[31m` | `.red` |
| `\x1B[32m` | `.green` |
| `\x1B[33m` | `.yellow` |
| `\x1B[90m` | `.gray` |

- Neue Clients erhalten ab Verbindungszeitpunkt neue Log-Zeilen (kein Backlog)
- Befehle können als Text-Frame gesendet werden; nur printable ASCII (`0x20–0x7E`, `\r\n\t`) wird akzeptiert
