# Release Notes

## 0.6.1
### Breaking Changes
- none

### Feature
- feature: Add `PingHandler` — non-blocking ICMP ping with queue and parallel slot management
- Up to `OPENKNX_PING_PARALLEL` (default: 5) concurrent pings, unlimited queue
- Configurable timeout via `OPENKNX_PING_TIMEOUT` (default: 1000 ms)
- Callback signature: `void(IPAddress, bool reachable, uint32_t rttMs)`
- RP2040: lwip raw API (`raw_sendto_if_src`), accurate RTT via callback timestamp
- ESP32: lwip BSD socket API, thread-safe DNS via FreeRTOS queue
- DNS resolution support (`ping("hostname", callback)`) on both platforms
- Console command: `ping <ip|hostname>` with 2 s timeout
- API via `openknxNetwork.ping(target, callback [, timeoutMs])`

### Bug
- none

## 0.6.0
### Breaking Changes
- none


### Feature
- Initial Changelog started
- enhancment: use new function property wrapper

### Bug
- fix: ensure NTP server is stopped only if enabled and adjust sync mode
- fix: update WiFi settings handling to prevent disconnect on reboot
- fix: ensure NTP server is stopped only if enabled and adjust sync mode

