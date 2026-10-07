# WsTxQueue.md - WS-Sendequeue

Teil von [`AGENTS.md`](../../AGENTS.md).
Verwandt: [`UiSocket.md`](UiSocket.md), [`Webserver.md`](Webserver.md), [`WebserverPlatform.md`](WebserverPlatform.md), [`Webconsole.md`](Webconsole.md), [`GroupMonitor.md`](GroupMonitor.md)

## WS Send Queue (`WsTxQueue`)

Sits **below** the UI layer ([`UiSocket.md`](UiSocket.md)), so every `addSocket()` endpoint benefits: `sendWebsocketMessage()` and
`sendToClient()` now queue instead of writing the socket, and only the drain in `Webserver::loop()`
sends (through the internal `wsSendNow()` — routing the drain through the public API would make it
re-queue endlessly). Before this, a frame was simply dropped whenever a client's TCP buffer was
momentarily full, and `Webconsole` was the only feature that worked around it.

- **One shared record ring for all clients** (`OPENKNX_WEBSOCKET_TX_BUFFER`, default 4 KB via
  `PSRAM_MALLOC`), plus a 16-byte cursor entry per client. RAM does not scale with the client count and a
  broadcast is stored once, not once per client.
- **Length lives in the ring**, not in a separate index: message sizes are unknown, so there is
  exactly one limit (bytes) and no second resource that could run out first. `_oldest` is carried
  along rather than reconstructed — it always points at a header, because eviction advances it by
  exactly `HDR_SIZE + len`. `HDR_SIZE` is a constant `5`, never `sizeof` (padding would make it 6 and
  silently shift the whole ring).
- **Evicting means `_oldest += HDR_SIZE + len`** — nothing is moved or copied. `append()` frees whole
  records *before* writing, so a half-overwritten record is impossible by construction.
- **The payload never wraps**: if it would cross the ring end, a padding record (`ROUTE_PAD`) is
  inserted first. That keeps the drain able to hand the socket a pointer straight into the ring
  instead of reassembling into a scratch buffer. The 5-byte header may wrap and is read byte-wise.
- **Skipping advances the cursor** — load-bearing, not an optimisation to remove. A client on a
  foreign endpoint walks to the write position every tick and therefore stays at the front; without
  it, traffic that does not concern it would overtake it and get it disconnected.
- **`stalled()` is evaluated *after* the peek loop**, never before. Only there does `hasPending()`
  mean "did not accept"; before the loop it is also true for records this client does not own and
  peek merely skips, which put a foreign-endpoint client 10 s of unrelated UI traffic away from being
  dropped.
- **Positions are monotonic `uint32_t`** and the ring size is forced to a power of two, so the mask
  stays continuous across the counter's own wraparound. All comparisons are differences, never `<`.
- **`maxRecord()` is `min(size/2 - HDR_SIZE, maxWebsocketPayload(), 65535)`.** All three matter: half
  the ring so one message cannot evict everything, the frame limit because RP2040 can never send more
  than 1496 B and the drain would otherwise park a cursor on an unsendable record forever, and 65535
  because the header carries the length in 16 bits.
- **Locking is not the same mechanism on both platforms** ([`WebserverPlatform.md`](WebserverPlatform.md))**.** ESP32 uses the recursive WS-state mutex.
  RP2040 is *not* plain single-thread: with `PIN_ETH_INT` the W5500 hangs off an interrupt and
  `lwip_callback()` runs `onRecv`/`onErr`/`onPoll` inside it, so they preempt `loop()` mid-drain.
  `WsQueueGuard` therefore holds an `LWIPMutex` there — the core's own lock, which raises `__inLWIP`
  so the IRQ defers instead of firing. Without it the swap-remove in `removeClient()` can land
  between a `peek()` and its `advance()` and move the wrong client's cursor into a payload.
- **`Failed` is handled by the drain, not by `wsSendNow()`** — otherwise only ESP32 would clean up,
  and on RP2040 an unsendable record would stall that client silently.
- **A new client starts at the write position**: no backlog, and on RP2040 — where `fd` is the slot
  index and gets reused — a new client in a recycled slot cannot inherit its predecessor's pending
  unicasts.
- **`addClient()` returns a bool and the caller must look at it.** A client in `clients` without a
  cursor would receive nothing and loop reconnecting in silence, so `notifySocketConnect()` adds it to
  neither on failure and logs. A `static_assert` ties the cursor table to the connection pool, which
  is what makes the case unreachable in the first place.
- **Disconnecting means "stopped reading", not "slower than the producer".** Two triggers: overtaken
  in the ring, or nothing accepted for 10 s with records pending (`WsTxQueue::stalled()`) **while the
  peer was silent that long** (`wsPeerIdleMs()`). The second condition is load-bearing on RP2040:
  lwIP's 16 KB heap is shared with MQTT, OTA and downloads, so `tcp_write()` can return `ERR_MEM` for
  seconds — indistinguishable from a full send buffer at our API — while the browser is acknowledging
  normally. Dropping on the accept result alone disconnected healthy clients there. This only covers
  the device's verdict: on the UI socket `base.js` still gives up after ~3 s without a `pong`, so an
  `ERR_MEM` stall longer than that ends in a browser-side reconnect regardless; ESP32 never showed
  it because its TCP keepalive (~9 s) removes genuinely dead peers before the stall timer expires, and
  it therefore reports `UINT32_MAX` (always silent), keeping its behaviour unchanged. Producers
  must throttle rather than fill the ring, and the check is `Webserver::txFits(key, len)`: the
  record's exact cost (`WsTxQueue::cost()`, header and ring-end padding included) plus a reserve
  (an eighth of the ring, ≤ 512 B) for `status`/`tp`/`pong` must be free. An estimate is not enough —
  the padding record can nearly double a message near the ring end. `Webconsole::loop()`
  ([`Webconsole.md`](Webconsole.md)) applies it to every line and additionally caps itself at a
  quarter of the ring per tick; `GroupMonitor::onFrame()` ([`GroupMonitor.md`](GroupMonitor.md))
  pre-checks a quarter free before decoding and `txFits()` after. Only the small, rare messages
  (`status`, `pong`) append unconditionally — they are what the reserve is for. The
  first version filled the ring instead, which overtook briefly-slow browsers under a log burst and
  disconnected them; with two tabs reconnecting simultaneously their upgrades then collided with
  the shared connection limit and found no free slot. No gap event, no dropped counter: the browser
  reconnects on its own and resyncs through `connect`, which is exactly where a "snap the cursor"
  would have put it.
- **Drop callbacks fire outside the lock.** The drain collects the clients it wants to tear down and
  calls `dropClient()` only after releasing the guard — otherwise a module whose `onConnect(fd,false)`
  takes a lock of its own (MQTT does) could deadlock against a task that holds that lock and waits for
  the WS state mutex. The code this replaced carried the same rule as a comment.
- **`onConnect(fd, true)` is delivered from `loop()`**, not from the network callback — so a module
  may build an arbitrarily large JSON there and has the `fd` when it needs it. The cursor is created
  in `notifySocketConnect()` and therefore exists *before* delivery; otherwise the initial data would
  land behind the client's own cursor. **Disconnect stays immediate** on purpose: the slot can be
  reused at once, and a late notification would tear down the *new* client's state. If a client
  leaves before its connect notification was delivered, both are suppressed — the module never
  learned about it.
- **Known gap, deliberately open:** on RP2040 the ethernet interrupt can deliver a disconnect
  *nested inside* the connect delivery, i.e. before it returns. It is still delivered exactly once —
  `takePendingConnect()` has cleared the flag, so `removeClient()` reports it — only out of order. A
  post-check after the callback would make it a *second* notification, and fixing the order needs a
  deferred disconnect path. Only a module that builds per-client state in `onConnect` is affected,
  and none in-tree does (the console registers no `onConnect`, the monitor passes `nullptr`).
