# OFM-Network – Agent Instructions

**OFM-Network** ist ein Netzwerk-Funktionsmodul im **OpenKNX**-Ökosystem und liefert IP-Anbindung
(WiFi & Ethernet), OTA, mDNS, NTP, ICMP-Ping, MQTT und das Webinterface für OpenKNX-Geräte.
C++17, Arduino-Framework, Zielplattformen ESP32 und RP2040.

- **Branches**: `v1` (Release), `v1dev` (Entwicklung), daneben Feature-/Fix-Branches (z. B.
  `v1dev-ping`), die nach `v1dev` gehen
- **Der Anwender ist fortgeschritten** in Arduino, RP2040 (arduino-pico von Earle Philhower), ESP32
  und Netzwerktechnik — keine Grundlagen erklären, direkt zur Sache
- **Nicht eigenständig baubar**: Modulsystem, KNX-Stack-Anbindung, Logging, Flash, Zeit-API liegen in
  **OGM-Common**, `../OGM-Common/AGENTS.md` (eine Ebene höher). Dessen Konventionen und Claude-Skills
  gelten hier mit; Typen wie `OpenKNX::Module`, `OpenKNX::Log::Logger` oder
  `OpenKNX::Time::TimeProvider` zuerst dort suchen
- Anwenderdokumentation: [`doc/Applikationsbeschreibung-Netzwerk.md`](doc/Applikationsbeschreibung-Netzwerk.md),
  [`README.Webserver.md`](README.Webserver.md), [`README.MQTT.md`](README.MQTT.md)

## Teildateien

Die Einzelheiten stehen in `docs/agents/` — lies die passende Datei, bevor du in ihrem Bereich etwas
änderst. Bewusst nicht per `@` eingebunden, damit sie nur bei Bedarf in den Kontext kommen.

| Datei | Inhalt |
|---|---|
| [`docs/agents/Platform.md`](docs/agents/Platform.md) | Hardware-Plattformen, Stacks, Connectivity-Defines |
| [`docs/agents/Style.md`](docs/agents/Style.md) | Codestil, Kommentare, `.clang-format` |
| [`docs/agents/Module.md`](docs/agents/Module.md) | `NetworkModule`, KNX-IP-Gating, `PingHandler`, `NtpTimeProvider` |
| [`docs/agents/MQTT.md`](docs/agents/MQTT.md) | MQTT-Client und -Broker, Regeln, die am Aufrufort nicht sichtbar sind |
| [`docs/agents/TelegramJson.md`](docs/agents/TelegramJson.md) | `JSON::Writer`/`Reader`, `buildTelegramJson()`, `tpUart()`, `GATable` |
| [`docs/agents/Knx.md`](docs/agents/Knx.md) | KNX-Parameter, IP-Properties, Function Property, Compile-Defines |
| [`docs/agents/Build.md`](docs/agents/Build.md) | Build und Test, Web-Assets (`web/assets/` → `webassets.h`) |
| [`docs/agents/Webserver.md`](docs/agents/Webserver.md) | Compile-Flags, `setup()`/`start()`, Access-Log, Zeichensatz, Assets, Segmente, Layout |
| [`docs/agents/UiSocket.md`](docs/agents/UiSocket.md) | `/openknx/ws`: `sendMessage`/`receiveMessage`, `connect`, `ping`/`pong`, Status |
| [`docs/agents/WsTxQueue.md`](docs/agents/WsTxQueue.md) | Sendequeue: Ring, Cursor, Verdrängung, Trennregeln, Sperren |
| [`docs/agents/WebserverPlatform.md`](docs/agents/WebserverPlatform.md) | RP2040 (lwIP `NO_SYS=1`, Slots) und ESP32 (httpd), WS-Protokollumfang |
| [`docs/agents/Webconsole.md`](docs/agents/Webconsole.md) | Logger-Ringpuffer, Befehlsverarbeitung, Konsolenseite |
| [`docs/agents/GroupMonitor.md`](docs/agents/GroupMonitor.md) | Gruppenmonitor (`OPENKNX_WEBMONITOR`) |
| [`docs/agents/FileManager.md`](docs/agents/FileManager.md) | Dateimanager (`OPENKNX_WEBFS`), Laufwerke |

Immer gilt: [`docs/agents/Style.md`](docs/agents/Style.md) vor der ersten Codezeile, und was sich am
Verhalten ändert, gehört in die passende Teildatei und — sofern ausgeliefert — in
[`CHANGELOG.md`](CHANGELOG.md), nicht in diese Datei zurück.
