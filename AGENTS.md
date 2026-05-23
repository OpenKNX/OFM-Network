# OFM-Network – Agent Instructions

## Projekt-Kontext

**OFM-Network** ist ein Netzwerk-Funktionsmodul im **OpenKNX**-Ökosystem – einem Open-Source-Framework für KNX-Gebäudeautomation auf Embedded-Hardware. Das Modul stellt IP-Konnektivität (WiFi & Ethernet), OTA-Updates, mDNS, NTP-Zeitsync und ICMP-Ping für OpenKNX-Geräte bereit.

- **Branch-Konvention**:
  - `v1` – öffentlicher Release-Branch (stable)
  - `v1dev` – aktiver Entwicklungsbranch
  - Weitere Branches (z.B. `v1dev-ping`) sind Feature- oder Fix-Branches, die in `v1dev` gemergt werden
- **Sprache**: C++17, Arduino-Framework
- **Entwickler-Expertise**: Fortgeschrittenes Niveau in Arduino, RP2040 (arduino-pico by Earle Philhower), ESP32 und Netzwerktechnik – Grundlagen dieser Plattformen nicht erklären, direkt auf den Punkt kommen.

Vollständige Dokumentation: [`doc/Applikationsbeschreibung-Netzwerk.md`](doc/Applikationsbeschreibung-Netzwerk.md)

---

## Hardware-Plattformen

| Plattform | Konnektivität | Stack | Compile-Define |
|-----------|--------------|-------|----------------|
| **ESP32** | WiFi | `WiFi.h` (Arduino IDF) | `KNX_IP_WIFI` |
| **ESP32** | Ethernet | `ETH.h` (Arduino IDF) | `KNX_IP_LAN` |
| **RP2040** (PicoW) | WiFi | arduino-pico by Earle Philhower | `KNX_IP_WIFI` |
| **RP2040** | Ethernet | W5500lwIP (`W5500lwIP.h`) via SPI | `KNX_IP_LAN` |

**Wichtige Eigenheiten:**
- RP2040 verwendet **arduino-pico** von Earle Philhower (nicht den offiziellen Arduino Mbed Core) – lwip im Single-Thread-Modus (`NO_SYS=1`), kein FreeRTOS
- ESP32 hat FreeRTOS, DNS muss via TCPIP-Task-Callback aufgerufen werden (siehe `PingHandler_ESP32.cpp`)
- W5500 SPI-Speed: `OPENKNX_NET_SPI_SPEED` (default 28 MHz), Credentials in LittleFS (`/WIFI.TXT`)
- ESP32 WiFi-Credentials via `Preferences` API

---

## Architektur & Schlüsselklassen

### `NetworkModule` (`src/NetworkModule.h/.cpp`)
Hauptmodul, erbt von `OpenKNX::Module`. Verwaltet:
- PHY-Init (`initPhy`) → IP-Init (`initIp`) → mDNS → OTA → NTP
- DHCP vs. Static IP (aus KNX-Parametern)
- WiFi-Credential-Management (save/read/erase)
- Network-State-LED via `OpenKNX::Led::FunctionGroup` (wenn `OPENKNX_LED_IP` definiert)
- Console-Commands: `net`, `net reset`, `ping`, `wifi SSID PSK`, `erase wifi`, `net mc`
- Callback-Registrierung für Netzwerkänderungen (`registerCallback`)

### `PingHandler` (`src/OpenKNX/Network/PingHandler.*`)
Non-blocking ICMP-Ping mit Queue + parallelen Slots:
- **Pending-Queue**: unbegrenzt, FIFO
- **Active Slots**: `OPENKNX_PING_PARALLEL` (default 5) gleichzeitige Pings
- DNS-Auflösung transparent vor dem Ping
- Callback-Signatur: `void(IPAddress target, bool reachable, uint32_t rttMs)`
- `loop()` muss im Arduino-Loop aufgerufen werden
- Plattform-Implementierungen:
  - **ESP32**: BSD-Socket-API (`SOCK_RAW`, `IPPROTO_ICMP`), non-blocking via `fcntl(O_NONBLOCK)`, FreeRTOS-Queue für Thread-safe DNS
  - **RP2040**: lwip raw API (`raw_pcb`, `raw_sendto_if_src`), Interrupt-Callback `icmpRecvCallback`

### `NtpTimeProvider` (`src/NtpTimeProvider.h/.cpp`)
- Erbt von `OpenKNX::Time::TimeProvider`
- ESP32: `esp_sntp_*` API (non-blocking, smooth mode)
- RP2040: Arduino-NTP-Library, sync alle 3600 s
- NTP-Server via KNX-Parameter `ParamNET_NTPServer` (default: `pool.ntp.org`)

---

## KNX-Konzepte & Integration

**OpenKNX-Module** implementieren ein Interface mit: `init()`, `setup()`, `loop()`, `processCommand()`, `readFlash()`, `writeFlash()`.

**KNX-Parameter** werden aus dem generierten `knxprod.h` gelesen via Makros wie `ParamNET_*`. Parameter-Definition in [`src/Network.share.xml`](src/Network.share.xml).

**KNX IP-Properties** (BAU-Interface):
- `PID_IP_ADDRESS`, `PID_SUBNET_MASK`, `PID_DEFAULT_GATEWAY`, `PID_IP_ASSIGNMENT_METHOD`
- `PID_IP_CAPABILITIES` → advertise DHCP+AutoIP (value: 6)
- Routing Multicast: `PID_ROUTING_MULTICAST_ADDRESS` (Mask 0x091A / 0x57B0)

**Function Property** (Object 160, Property 5): Überträgt WiFi-Credentials aus ETS via `Network.script.js`.

**Compile-Defines (wichtig):**
```
OPENKNX_LED_IP          – LED für Netzwerkstatus (optional)
OPENKNX_PING_TIMEOUT    – Ping-Timeout in ms (default 1000)
OPENKNX_PING_PARALLEL   – Max. parallele Pings (default 5)
OPENKNX_NET_SPI_SPEED   – W5500 SPI-Speed Hz (default 28000000)
```

---

## Coding-Konventionen

- **Kein `delay()`** – alles non-blocking, State-Machines mit `millis()`-Checks
- **Plattform-Guards**: `#ifdef ARDUINO_ARCH_ESP32` / `#ifdef ARDUINO_ARCH_RP2040`
- **Connectivity-Guards**: `#ifdef KNX_IP_WIFI` / `#ifdef KNX_IP_LAN`
- Keine externen Bibliotheken in `library.json` – nur Arduino-Framework-Builtins und OpenKNX-Core
- Kommentare auf Deutsch oder Englisch (gemischt ist OK, aber konsistent pro Datei)
- OTA-Port: ESP32 = 3232, RP2040 = 2040 (beide Plattformen unterstützen OTA, Implementierung ist identisch)
- mDNS-Hostname: auto-generiert als `OpenKNX-XXXXXXXX` (8 Hex-Zeichen aus Seriennummer)

### Formatierung (`.clang-format`)

Basiert auf **LLVM-Style** mit folgenden Abweichungen – bei Code-Generierung strikt einhalten:

- **Geschweifte Klammern** immer auf **eigener Zeile** (Allman-Style) – gilt für Klassen, Funktionen, `if`, `else`, `for`, `while`, `case`, `enum`, `struct`, `namespace`, `extern`
- **Kein Zeilenlimit** (`ColumnLimit: 0`) – keine künstlichen Umbrüche
- **4 Spaces** Einrückung, **kein Tab** (`UseTab: Never`)
- `namespace`-Inhalt wird eingerückt (`NamespaceIndentation: All`)
- `case`-Labels werden eingerückt (`IndentCaseLabels: true`)
- Kurze `case`-Labels, Enums, Lambdas und Funktionen dürfen einzeilig bleiben
- `if` ohne Block nur erlaubt wenn **einziges** Statement (`OnlyFirstIf`) – kein `else` einzeilig
- Pointer-Alignment wird aus dem Code abgeleitet (`DerivePointerAlignment: true`)
- Präprozessor-Direktiven **nicht** eingerückt (`IndentPPDirectives: None`)

---

## Build & Test

Kein eigenes `platformio.ini` – das Modul wird als Library in übergeordnete OpenKNX-Projekte eingebunden. `platformio.network.ini` ist leer (Vorlage für Projekte).

Zum Testen: Integration in ein OpenKNX-Geräteprojekt, das dieses Modul referenziert.

---

## Webserver

Vollständige API-Referenz: [`README.Webserver.md`](README.Webserver.md)

### Compile-Flags

| Flag | Effekt |
|------|--------|
| `OPENKNX_WEBSERVER` | Aktiviert HTTP-Server, Routing, Übersichtsseite, Logo-Asset, alle Routen |
| `OPENKNX_WEBCONSOLE` | Aktiviert `/console`-Seite, WS-Endpoint `/console`, **und** den Logger-Ring-Buffer in OGM-Common |
| `OPENKNX_WEBCONSOLE_BUFFER` | Gesamtgröße des Ring-Buffers in Bytes (optional, default: 4096, max: 65535) |

`OPENKNX_WEBCONSOLE` setzt `OPENKNX_WEBSERVER` voraus.
Ohne `OPENKNX_WEBSERVER` wird keinerlei Webserver-Code compiliert — keine Klasse, keine Stubs, kein `webserver`-Member im Module.

### Plattform-Unterstützung

| Plattform | Stack | Implementierung |
|-----------|-------|----------------|
| **ESP32** | FreeRTOS + esp-idf `httpd` | `Webserver_ESP32.cpp` |
| **RP2040** | lwIP `NO_SYS=1`, single-threaded | `Webserver_RP2040.cpp` |

### Asset-Management (`addStylesheet` / `addJavaScript`)

Der Webserver bietet ein flexibles System zur Verwaltung von CSS und JavaScript Assets:

**API:**
```cpp
openknxNetwork.webserver.addStylesheet("/assets/custom.css");
openknxNetwork.webserver.addJavaScript("/assets/custom.js");
```

**Verhalten:**
- `addStylesheet()` → registriert URI, wird im `<head>` eingefügt (via `buildHeader()`)
- `addJavaScript()` → registriert URI, wird vor `</body>` eingefügt (via `buildFooter()`)
- **Cache-Buster**: automatisch Query-Parameter `YYYYMMDDHHII` (z.B. `?202605231435`) aus Build-Zeitpunkt
- **Defer-Attribut**: JavaScript-Tags erhalten `defer` für non-blocking Ausführung
- `base.css` und `base.js` werden während `setup()` über diese Funktionen registriert

**Implementierung:**
- `Webserver._stylesheets` → `std::vector<std::string>` in buildHeader durchlaufen
- `Webserver._scripts` → `std::vector<std::string>` in buildFooter durchlaufen

### Logger-Ring-Buffer (`OPENKNX_WEBCONSOLE`)

Der Ring-Buffer lebt in `OpenKNX::Log::Logger` (OGM-Common), **nicht** im Webserver. Er füllt sich unabhängig davon, ob WS-Clients verbunden sind – neue Clients erhalten damit automatisch die History.

- **Typ**: Byte-adressierter Ringpuffer variabler Eintrags-Länge (keine Fixed-Slots)
- **Größe**: `OPENKNX_WEBCONSOLE_BUFFER` Bytes gesamt (default: 4096)
- **Eintragsformat**: `[uint32_t seq][uint16_t len][char text[len]]` = `LOG_ENTRY_HDR`(6) + len Bytes
- **Sentinel**: Wenn ein neuer Eintrag nicht mehr ans Ende passt, werden die verbleibenden Bytes auf 0 gesetzt (seq=0 = Sentinel), und `_logTail` wird auf 0 zurückgesetzt
- **Overflow**: Wenn der neue Eintrag den Head überschreiben würde, werden alte Einträge von `_logHead` her verdrängt
- **Sequenznummern**: monoton steigend (`_logNextSeq`), beginnen bei 1; seq=0 ist reserviert für Sentinels/ungeschriebene Bereiche
- **Per-Line-Accumulator**: `_lineAccum[LOG_LINE_MAX]` (256 Zeichen) – wird bei jedem Log-Aufruf befüllt und nach `\n` via `commitLineToRing()` in den Buffer geschrieben
- **API**: `openknx.logger.getLogEntryAfter(lastSeq, buf, size, &outSeq)` — gibt ersten Eintrag mit `seq > lastSeq` zurück; iteriert von `_logHead` aus, überspringt Sentinels automatisch

**Implementierung im Logger:**
- `beforeLog()`: setzt `_lineAccumLen = 0`
- Jeder Print-Aufruf (`printMessage`, `printPrefix`, `printTimestamp`, etc.) ruft zusätzlich `appendWebconsoleBuffer()` auf
- `afterLog()`: hängt `\n` an, ruft `commitLineToRing()` auf

### RP2040-Architektur (lwIP `NO_SYS=1`)

Kritische Eigenheit: lwIP läuft single-threaded. Callbacks (`onAccept`, `onRecv`, `onSent`, `onPoll`, `onErr`) dürfen **nicht blockieren** und keine langen Operationen ausführen.

**Connection Slots:**
```
static constexpr int MAX_CONN = 3;
static ConnSlot g_slots[MAX_CONN];
```

Jeder `ConnSlot` hält: HTTP-RX-Buffer (1536 B), HTTP-TX-Zeiger, WS-Statemachine-Zustand, `wsSentSeq` (wenn `OPENKNX_WEBCONSOLE`).

**Wichtig:** `onAccept` ruft `memset(s, 0, sizeof(*s))` auf. Das löscht `s->idx`. Deshalb wird `idx` **nach** dem memset per Pointer-Arithmetik gesetzt:
```cpp
int slotIdx = (int)(s - g_slots);
memset(s, 0, sizeof(*s));
s->idx = slotIdx;
```

**WS-State-Machine** (`wsProcessData`):
```
WR_HDR → WR_EXTLEN2 / WR_EXTLEN8 → WR_MASK → WR_PAYLOAD
```
Zustände werden vollständig in `ConnSlot` gehalten — kein globaler Parser-State.

**TCP-Send:** `wsTcpSend()` prüft `tcp_sndbuf()` — ist der Buffer voll (langsamer Client), wird der Frame gedroppt statt zu blockieren. Die Verbindung bleibt bestehen.

**Ping/Keepalive:** `onPoll` sendet alle ~60 s einen WS-Ping (Opcode 0x09). Antwortet der Browser nicht, erkennt lwIPs Retransmit-Timeout den toten Socket und feuert `onErr`.

### Befehlsverarbeitung auf RP2040 (Entkopplung von lwIP-Callbacks)

**Problem:** `processCommand()` ruft viele Log-Funktionen auf, die serielle TX-Ausgabe erzeugen. Innerhalb eines lwIP-Callbacks (`onRecv`) würde dies den USB-CDC-FIFO blockieren und die Ausgabe abschneiden.

**Lösung:** Queue-and-Defer-Pattern:
- WS-Message-Handler (läuft in `onRecv`) schreibt nur in `_pendingCmd[256]` und setzt `_hasPendingCmd = true`
- `Webserver::loop()` (sauberer Stack, außerhalb aller lwIP-Callbacks) liest `_pendingCmd`, setzt Flag zurück, ruft dann `processCommand()` auf

```cpp
// in loop():
if (_hasPendingCmd) {
    _hasPendingCmd = false;
    logInfo("", "> %s", _pendingCmd);
    openknx.console.processCommand(_pendingCmd);
}
```

**Einschränkung:** Es gibt nur einen `_pendingCmd`-Slot. Bei mehreren gleichzeitigen WS-Clients überschreibt die zuletzt empfangene Nachricht die vorherige. Für eine einzelne Konsolen-Session ist das ausreichend.

**Binär-Frame-Filter:** WS-Frames vom Browser mit Bytes außerhalb printable ASCII (`0x20–0x7E`, `\r`, `\n`, `\t`) werden still verworfen. Schützt vor Garbage durch fehlerhafte oder protokoll-interne Frames (z.B. Browser-Reconnect-Artefakte).

### History für neue WebSocket-Clients

Neue Clients verbinden sich mit `wsSentSeq = 0` (durch `memset` in `onAccept` automatisch). `loop()` ruft `getLogEntryAfter(0, ...)` auf und sendet alle 32 Ring-Buffer-Einträge sequenziell. Kein spezieller „History-Dump"-Code nötig — der normale Drain-Loop deckt es ab.

Per-Client-Tracking: `ConnSlot.wsSentSeq` enthält die zuletzt erfolgreich gesendete Sequenznummer. `loop()` sendet pro Tick so viele Einträge wie in den TCP-Buffer passen (bricht bei `wsTcpSend() == false` ab, Retry beim nächsten Tick).

### ESP32-Architektur

- `httpd` läuft in eigenem FreeRTOS-Task
- Pro WS-Session installiert `wsFrameRecv` sich als `httpd_sess_set_recv_override` — läuft in einer Endlosschleife im httpd-Task
- `processCommand()` wird direkt in `wsFrameRecv` aufgerufen (kein Dequeue nötig, FreeRTOS hat eigenen Stack)
- `wsSendText()` nutzt `MSG_DONTWAIT` — blockiert nie, EAGAIN = Frame gedroppt
- `Webserver::loop()` ist auf ESP32 ein No-Op (httpd-Task erledigt alles)
- TCP-Keepalive: 3 s idle, 2 s interval, 3 probes → tote Verbindungen nach ~9 s erkannt

**Wichtig:** ESP32 draint den Logger-Ring-Buffer derzeit **nicht** aus `loop()`. Auf ESP32 geht die Logger-Integration via `sendWebsocketMessage` aus einem Callback — oder kann künftig ebenfalls auf Ring-Buffer-Drain via `loop()` umgestellt werden.

### Web-UI: Console-Seite

- **Kein automatischer Reconnect** — bei Verbindungsverlust erscheint `[Verbindung getrennt — Seite neu laden]` und die Seite muss manuell neu geladen werden
- **Grund:** Automatischer Reconnect erzeugte race conditions im Log-Output und erschwerte das Debugging von Disconnect-Ursachen
- ANSI-Farben werden durch `ansiToHtml()` in CSS-Klassen umgewandelt (`red`, `green`, `yellow`, `gray`)
- Commands werden als Text-Frames (WS Opcode 0x01) gesendet, Antworten kommen als Ring-Buffer-Drain zurück

---

Changelog: [`CHANGELOG.md`](CHANGELOG.md)
