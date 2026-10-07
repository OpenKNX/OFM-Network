# Module.md - Netzwerkmodul, Ping, NTP

Teil von [`AGENTS.md`](../../AGENTS.md).
Verwandt: [`Platform.md`](Platform.md), [`Knx.md`](Knx.md), [`MQTT.md`](MQTT.md), [`TelegramJson.md`](TelegramJson.md)

## `NetworkModule` (`src/NetworkModule.h/.cpp`)
Main module, inherits from `OpenKNX::Module`. Manages:
- PHY init (`initPhy`) → IP init (`initIp`) → mDNS → OTA → NTP
- DHCP vs. static IP (from KNX parameters)
- WiFi credential management (save/read/erase)
- Network state LED via `OpenKNX::Led::FunctionGroup` (when `OPENKNX_LED_IP` is defined)
- Console commands: `net`, `net reset`, `ping`, `wifi SSID PSK`, `erase wifi`, `net mc`
- Callback registration for network changes (`registerCallback`)

**KNX-IP datalink gating (`controlKnxIp`)** — a mask-dependent one-liner over the stack: `getPrimaryDataLinkLayer()->enabled()` on `0x091A`, `getDataLinkLayer()->enabled()` on `0x57B0`, and an **empty body on every other mask** (so the whole mechanism is inert on TP-only devices). `IpDataLinkLayer::enabled(bool)` joins/leaves the KNX routing multicast group (`setupMultiCast`/`closeMultiCast`) and gates `sendBytes()`; it is guarded by its own `_enabled` flag, so repeated calls with the same value are no-ops. Two consequences worth knowing:

- **The multicast address is read from the property at `enabled(true)`**, and the socket binds to the IP present at that moment. A rebind therefore requires a full `false` → `true` cycle; a second `enabled(true)` alone does nothing.
- **A rebind costs telegrams**, so it must happen on a real IP change and only then. `enabled(true)` on an already-enabled layer is a no-op, so the socket used to keep the *old* binding after an IP change — silently off the bus until a restart. `_boundIp` holds the IP the socket is bound to, making the `GOT_IP` handler able to tell a real change from a plain lease renewal; it is cleared inside `controlKnxIp(false)` itself, deliberately not at the call sites, so "socket closed ⇒ binding invalid" cannot be forgotten.
- **The reconcile against `established()` (`connected() && localIP() != 0`) in `checkLinkStatus()` is ESP32-only, on purpose.** There the datalink hangs off the event chain, and a link flap in which `GOT_IP` never re-fires leaves it off for good; the 500 ms reconcile closes that gap for two comparisons per tick. RP2040 has no such events and therefore no gap — `knx.start()` enables the datalink once and it stays enabled. Gating it there would newly make KNX-IP depend on `isLinked()`/`isConnected()`, where a single false negative takes the device off the bus for at least 500 ms, with no demonstrated problem being solved.

## `PingHandler` (`src/OpenKNX/Network/PingHandler.*`)
Non-blocking ICMP ping with queue + parallel slots:
- **Pending queue**: unlimited, FIFO
- **Active slots**: `OPENKNX_PING_PARALLEL` (default 5) concurrent pings
- DNS resolution transparently before ping
- Callback signature: `void(IPAddress target, bool reachable, uint32_t rttMs)`
- `loop()` must be called from the Arduino loop
- Platform implementations:
  - **ESP32**: BSD socket API (`SOCK_RAW`, `IPPROTO_ICMP`), non-blocking via `fcntl(O_NONBLOCK)`, FreeRTOS queue for thread-safe DNS. All slots share **one** socket, so `platformCheckReply()` drains it completely and files every echo reply under its own slot (the ICMP `id` carries the slot index) instead of discarding whatever doesn't match the slot being polled — otherwise parallel pings lose each other's replies and run into false timeouts.
  - **RP2040**: lwIP raw API (`raw_pcb`, `raw_sendto_if_src`), interrupt callback `icmpRecvCallback`

## `NtpTimeProvider` (`src/NtpTimeProvider.h/.cpp`)
- Inherits from `OpenKNX::Time::TimeProvider`
- ESP32: `esp_sntp_*` API (non-blocking, smooth mode)
- RP2040: Arduino NTP library, sync every 3600 s. Deliberately blocking: `waitSet()` waits up to 10 s but keeps `openknx.loop()` running every 100 ms; after the first success lwIP's SNTP keeps the clock current and `waitSet()` returns at once. `timeSet()` only when `waitSet()` returned `true` — otherwise the unset clock (1970) would be reported as valid
- NTP server via KNX parameter `ParamNET_NTPServer` (default: `pool.ntp.org`)
