# Platform.md - Hardware-Plattformen

Teil von [`AGENTS.md`](../../AGENTS.md).
Verwandt: [`Module.md`](Module.md), [`WebserverPlatform.md`](WebserverPlatform.md)

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
