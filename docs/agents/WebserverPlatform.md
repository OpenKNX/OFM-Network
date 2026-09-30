# WebserverPlatform.md - Webserver auf RP2040 und ESP32

Teil von [`AGENTS.md`](../../AGENTS.md).
Verwandt: [`Webserver.md`](Webserver.md), [`WsTxQueue.md`](WsTxQueue.md), [`Webconsole.md`](Webconsole.md), [`FileManager.md`](FileManager.md)

## RP2040 Architecture (lwIP `NO_SYS=1`)

Critical characteristic: lwIP runs single-threaded. Callbacks (`onAccept`, `onRecv`, `onSent`, `onPoll`, `onErr`) must **not block** and must not perform long operations.

**Two contexts, one rule.** With `PIN_ETH_INT` (every W5500 board in OGM-HardwareConfig) the callbacks run in the W5500 interrupt and preempt `loop()`. They keep to bookkeeping, header/frame parsing and RAM-only sending; everything that touches the filesystem runs in `loopPlatform()`. Concretely:

- **`loopPlatform()` works each slot under `LWIPMutex`** — dispatch, refill and close as one unit. Without it `onErr`/`onRecv(FIN)` could release the slot in the middle of `doHttpDispatch()`: `s->pcb` turned NULL under `tcp_sndbuf(s->pcb)` (a HardFault on an aborted page load) and `g_response` was reset while `handleRequest()` was writing it. The price: **route handlers run with lwIP locked on RP2040** and must not wait for the network (blocking DNS, a synchronous client) — the IRQ that would deliver the answer is deferred until they return. None in-tree does. The lock is only taken when a slot has work; an idle tick peeks unlocked, because every release runs the async context's workers.
- **Streams are refilled from `loopPlatform()` only**, never from `onSent()`: the reader is a LittleFS/SD read. Their cleanup (file close) goes the same way — `releaseSlot()` parks it in `g_streamGc[idx]`, the loop runs it outside the lock, and `freeSlot()` skips a slot with one pending so a new connection cannot overwrite it.
- **Plain responses are still refilled from `onSent()`** (RAM only, lowest latency) *and* from the loop — after an `ERR_MEM` with nothing in flight no ACK arrives, so `onSent()` alone would leave the response hanging until `onPoll` aborts it after ~20 s.
- **At most `2 × TCP_MSS` in flight per HTTP connection** (`txRoom()`). Uncapped, one page wrote up to `TCP_SND_BUF` (~11.6 KB) in a single `tcp_write()` against a 16 KB lwIP heap — which then failed for the WS sends too.
- **The HTTP header goes through `g_pending[idx]`**, like the body through the segments, so an `ERR_MEM` is retried instead of sending the body bare. A stream chunk that `tcp_write()` refuses after it was read from the file is parked there as well — dropping it corrupted the download.
- **Every close goes through `closeSlot()`**, which `detach()`es all callbacks *including* `tcp_err` before `tcp_close()` (and aborts if that fails). The one exception is the HTTP idle abort in `onPoll` (~20 s without progress), which runs `endSlot()` + `detach()` + `tcp_abort()` and returns `ERR_ABRT`. A pcb lingering in FIN_WAIT/TIME_WAIT with its callbacks still set reached the next connection's slot: `onErr` tore that connection down, and `onRecv`'s stale branch returned `ERR_OK` without freeing the pbuf. The WS-404 path after the per-feature endpoints were removed hit exactly that. Stale callbacks now detach the pcb and free the pbuf.

**Connection slots:**
```
static constexpr int MAX_CONN = OPENKNX_WEBSERVER_MAX_CONN; // 3, or 6 with __LWIP_MEMMULT>=2
static ConnSlot g_slots[MAX_CONN];
```

`OPENKNX_WEBSERVER_MAX_CONN` is now one flag for both platforms (ESP32 7; RP2040 3, or 6 when the parent project sets `-D__LWIP_MEMMULT=2`) and caps the **sum** of HTTP and WebSocket connections. Since a visitor occupies two slots, an RP2040 project **without** that flag serves one visitor — the always-open UI socket is what makes the flag worth setting — the mix is deliberately not constrained. 6× HTTP, 5× HTTP + 1 WS, 6× WS: all fine, whoever asks first gets a slot and whoever finds none does not get one. Only the total is limited, so the device's other services (NTP, MQTT, OTA, KNX-IP) keep sockets of their own. An earlier version capped WebSockets separately so HTTP could not starve; that was dropped as the wrong trade for an embedded device — it lowered the visitor count to protect a failure mode that heals itself on the next reload. Note a visitor occupies two slots: the permanently open WebSocket plus an HTTP slot while a page loads.

**The RP2040 default follows the lwIP pool instead of demanding a flag.** arduino-pico sets `MEMP_NUM_TCP_PCB = __LWIP_MEMMULT * 5`, so by default the *whole device* has 5 active TCP PCBs — including MQTT, the webclient, OTA and the TIME_WAIT states of freshly closed connections, and the last accept would fail silently inside lwIP. `Webserver.h` therefore keys the default off the flag: 3 slots without it, 6 (three visitors) with `-D__LWIP_MEMMULT=2`. `MEMP_NUM_TCP_PCB` itself is unusable there — `Webserver.h` does not include `lwipopts.h` — so the flag stands in for it. Six slots are never promised on a pool that cannot hold them, and a project that sets `OPENKNX_WEBSERVER_MAX_CONN` too high for its pool anyway still gets the `#warning` in `Webserver_RP2040.cpp` (a `#error` would break existing OAM builds). The flag does take effect under PlatformIO: lwIP is compiled from source through the wrappers in `cores/rp2040/lwip/`, not linked from a prebuilt archive. `platformio.network.ini` carries the snippet, commented out. `OPENKNX_WEBSERVER_RX_CAP` (RP2040, default 1024) follows the same naming rule for the same reason — the per-slot receive buffer serves HTTP headers *and* WS payloads, so there is one flag for both. ESP32's `OPENKNX_WEBSOCKET_RX_CAP` (default 2048) keeps the `WEBSOCKET_` prefix because it really is WebSocket-only — httpd buffers the HTTP side itself. **The scope differs by necessity, the behaviour does not:** on ESP32 httpd parses HTTP headers with its own buffers, so ours cannot be shared there — but an oversized WS payload now drops the connection on *both* platforms. RP2040 used to truncate and still deliver the frame, which turned a too-long `cmd:` into half a console command that then ran, and an absurd declared length kept the connection consuming bytes indefinitely. The length field is `uint64_t` on both platforms because RFC 6455's `127` form carries eight length bytes — the comparison has to see the real wire value. Narrowing it would make the accumulate loop drop the high bytes silently, so `0x1_0000_0200` would arrive as 512, pass the check, and desynchronise the parser on the remaining stream. 8 bytes per slot is the right price for that.

Each `ConnSlot` holds: **one** receive buffer (`OPENKNX_WEBSERVER_RX_CAP` B), HTTP TX pointer, WS state machine state. The webconsole's read position into the logger ring is a single `Webconsole::_readPos`, not per client and not in `ConnSlot` — siehe [`Webconsole.md`](Webconsole.md). Per-client send state lives in `WsTxQueue`, also outside `ConnSlot`: `onAccept()` clears that with `memset`, which would wreck any non-trivial member.

**`rxBuf` serves both phases** — WS code casts to `uint8_t*` where it needs bytes instead of `char*`. A connection is in exactly one protocol at a time (`ConnState`), so the phases never overlap: filled only in `CS_HTTP_RECV`, then written only from `wsProcessData()`, which requires `CS_WS`. Both need reassembly across TCP segments — that is why a per-connection buffer exists at all; the WS *send* path needs none and uses a stack frame in `wsTcpSend()`.

The one ordering constraint this creates lives in `doHttpDispatch()`: the `Sec-WebSocket-Key` is not copied out of the request either — `wsKeyOff` is an offset into `rxBuf` (0 = header absent) — so `wsComputeAccept()` reads `rxBuf` and must stay **before** `s->state = CS_WS`. Anything added to the upgrade path after that line is looking at WS payload memory, not at the request.

**Important:** `onAccept` calls `memset(s, 0, sizeof(*s))`, which clears `s->idx`. Therefore `idx` is set **after** the memset via pointer arithmetic:
```cpp
int slotIdx = (int)(s - g_slots);
memset(s, 0, sizeof(*s));
s->idx = slotIdx;
```

**WS state machine** (`wsProcessData`):
```
WR_HDR → WR_EXTLEN2 / WR_EXTLEN8 → WR_MASK → WR_PAYLOAD
```
State is fully held in `ConnSlot` — no global parser state.

**TCP send:** `wsTcpSend()` checks `tcp_sndbuf()` and reports `WouldBlock` when the buffer is full (slow client). It is called only from the send queue's drain, which then leaves that client's cursor where it is and retries next tick — nothing is dropped. The connection remains open.

**Ping/keepalive:** `onPoll` sends a WS ping (opcode 0x09) every ~60 s. If the browser does not respond, lwIP's retransmit timeout detects the dead socket and fires `onErr`.

## ESP32 Architecture

- `httpd` runs in its own FreeRTOS task
- **WebSockets use no per-connection task.** On upgrade the socket is detached from httpd via `httpd_req_async_handler_begin()` so httpd stops polling it; it is then serviced non-blocking from `Webserver::loop()` (driven by the main loop, just like RP2040). The earlier `httpd_sess_set_recv_override` endless-loop monopolized the single httpd task and limited the device to one working WebSocket (a second tab couldn't connect); the task-per-session variant fixed concurrency but cost ~4.3 KB RAM per connection — both are gone.
- `Webserver::loop()` per session: `recv(MSG_DONTWAIT)` drains available bytes into the session's `rx` buffer, then `wsParseFrames()` dispatches every complete frame (partial frames wait for the next tick). On EOF/error/oversize → cleanup: `notifySocketConnect(false)` → `httpd_req_async_handler_complete()` (httpd closes the socket) → remove from registry → free.
- `onMessage` runs in the loop task; for the console it only buffers the command into `_pendingCmd`, which `Webconsole::loop()` then runs via `processCommand()`.
- Concurrency bounded by `OPENKNX_WEBSERVER_MAX_CONN` (default 7), shared with HTTP; there is no WebSocket-specific cap. `config.lru_purge_enable = true` frees idle HTTP keep-alives so new connections find a socket slot — a live WebSocket is safe from it, because its async handler stays open (see `httpd_req_async_handler_complete()`).
- Shared state (`_socketClients` + the WS session registry) guarded by a recursive mutex (`Webserver::wsStateLock/Unlock`), reachable from both `Webserver_ESP32.cpp` and the platform-agnostic `Webserver.cpp` (`hasClients`, `ui.hasClients()`, `notifySocketConnect`, the queue drain). `wsSendText()`/`wsRawSend()` use `MSG_DONTWAIT` and are serialized by a TX mutex so frames from different tasks never interleave.
- TCP keepalive: 3 s idle, 2 s interval, 3 probes → dead connections detected after ~9 s
- **Streaming downloads ([`FileManager.md`](FileManager.md), `FileManager::handleDownload`) run synchronously in the httpd task** — `platformHttpHandler()` reads the whole file from LittleFS and calls `httpd_resp_send_chunk()` in a blocking loop. Since httpd has a single task, no other HTTP request (including WS upgrades) is served until the download finishes. Acceptable for the small config/log files WEBFS is meant for; would need reworking (spread across `Webserver::loop()` ticks like RP2040's `pumpHttp()`) before serving large files.

## WebSocket Protocol Scope

Both platforms parse only what the bundled browser clients actually send: masked, unfragmented text/binary/ping/pong/close frames. A payload larger than the platform's RX cap drops the connection on both — never a truncated frame handed to the handlers. Frames without a mask (disallowed by RFC 6455 for client→server) and fragmented messages (opcode `0x00` continuation, `FIN=0`) are **not validated or reassembled** — not a concern as long as the only clients are this module's own pages, whose `ws.send(string)` calls never produce either. Revisit if the WS endpoints are ever exposed to third-party clients.
