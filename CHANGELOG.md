# Changes

## 0.6.1: comming

* feature: Add web **group monitor** (`OPENKNX_WEBMONITOR`) — ETS-style live KNX telegram view at `/groupmonitor`
  * TP only, gated on `MASK_VERSION == 0x07B0`; requires `OPENKNX_WEBSERVER`
  * Taps all bus frames in parallel via the TP-UART `registerReceivedFrame` hook, bypassing the KNX stack (no filtering)
  * Decodes source/destination, APCI type (Read/Write/Response) and payload hex, broadcasts as JSON over WS `/groupmonitor`
  * Read-only; no own buffer/loop — frame callback broadcasts directly; new clients see telegrams from connect-time on
* feature: Add `PingHandler` — non-blocking ICMP ping with queue and parallel slot management
  * Up to `OPENKNX_PING_PARALLEL` (default: 5) concurrent pings, unlimited queue
  * Configurable timeout via `OPENKNX_PING_TIMEOUT` (default: 1000 ms)
  * Callback signature: `void(IPAddress, bool reachable, uint32_t rttMs)`
  * RP2040: lwip raw API (`raw_sendto_if_src`), accurate RTT via callback timestamp
  * ESP32: lwip BSD socket API, thread-safe DNS via FreeRTOS queue
  * DNS resolution support (`ping("hostname", callback)`) on both platforms
  * Console command: `ping <ip|hostname>` with 2 s timeout
  * API via `openknxNetwork.ping(target, callback [, timeoutMs])`

## 0.6.0: 2026-05-15

* Initial Changelog started
* fix: ensure NTP server is stopped only if enabled and adjust sync mode
* fix: update WiFi settings handling to prevent disconnect on reboot
* fix: ensure NTP server is stopped only if enabled and adjust sync mode
* enhancment: use new function property wrapper
   