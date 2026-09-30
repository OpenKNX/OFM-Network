# Webconsole.md - Webconsole: Ringpuffer, Befehle, Seite

Teil von [`AGENTS.md`](../../AGENTS.md).
Verwandt: [`UiSocket.md`](UiSocket.md), [`WsTxQueue.md`](WsTxQueue.md), [`Webserver.md`](Webserver.md)

## Logger Ring Buffer (`OPENKNX_WEBCONSOLE`)

The ring buffer lives in `OpenKNX::Log::Logger` (OGM-Common), **not** in the webserver. It fills regardless of whether WebSocket clients are connected.

- **Type**: plain byte ring, no entries/framing — `char _ringBuf[RING_SIZE]`, written one character at a time via `appendWebconsoleBuffer()` (`_ringBuf[_ringWritePos++ % RING_SIZE] = c`)
- **Size**: `RING_SIZE = OPENKNX_WEBCONSOLE_BUFSIZE` bytes
- **API**: `ringBuf()` (raw buffer pointer) and `ringWritePos()` (monotonically increasing write cursor, never wraps itself — callers index with `% RING_SIZE`)
- No sequence numbers, no sentinels, no line framing on the logger side — `Webconsole::loop()` (below) is the one that walks the buffer and splits it into lines

## Command Processing (Decoupling from Network Callbacks)

Platform-neutral, lives in `Webconsole` (`_pendingCmd[256]` / `_hasPendingCmd`), not per-platform.

**Problem:** `processCommand()` calls many log functions that generate serial TX output. Running it inside a network callback (lwIP `onRecv` on RP2040; the httpd task on ESP32) would block and could truncate output or stall the callback's task.

**Solution:** Queue-and-defer pattern:
- The UI handler (`ui.receiveMessage()` in `Webconsole::setup()`, filtering for the `cmd` key) only copies the command into `_pendingCmd` and sets `_hasPendingCmd = true`
- `Webconsole::loop()` (called from `Network::Module::loop()`, clean stack) picks it up, echoes it via the logger, and calls `openknx.console.processCommand()`

**Limitation:** There is only one `_pendingCmd` slot. With multiple simultaneous WS clients the last received message overwrites the previous one. This is sufficient for a single console session.

**Binary frame filter:** WS frames from the browser containing bytes outside printable ASCII (`0x20–0x7E`, `\r`, `\n`, `\t`) are silently discarded. Protects against garbage from malformed or protocol-internal frames (e.g. browser reconnect artifacts).

## No History for New WebSocket Clients

`Webconsole` keeps **one** read position for all clients (`_readPos`), not one per client: a log line
is relevant to every connected console, and the send queue ([`WsTxQueue.md`](WsTxQueue.md)) holds it until each client has taken
it. With no clients connected `_readPos` simply tracks `ringWritePos()`, so the next client to arrive
starts at "now" and gets no backlog — the same behaviour the per-client map used to provide, without
the bookkeeping.

**A ring overrun is reported, not swallowed.** When `writePos - _readPos` exceeds `RING_SIZE` the
oldest lines are already overwritten in the logger ring — typically because a single
`processCommand()` produced more than `OPENKNX_WEBCONSOLE_BUFFER`. The snap therefore emits a
`[... N Bytes Log verworfen ...]` line first, as an ordinary `log` record with an ANSI colour, so it
needs no client-side support. The other loss path — a client overtaken in the send queue — surfaces
as a disconnect and `console.js` writes its own gap marker.

`loop()` enqueues complete lines only (so the browser's ANSI parser stays stateless) under two
bounds:

- **Every line must fit without evicting** — `webserver.txFits("log", len)`: its exact cost
  (header and wrap padding included, `txCost()`) plus a reserve for `status`/`tp`/`pong` must be
  free. This holds for the first line of a tick too. The first version exempted that line from
  every bound; with a client stalled for a moment (RP2040 `ERR_MEM`, a closed window) and the log
  running, one line per tick filled the ring, overtook the client and dropped it — the reconnect
  loop this throttle exists to prevent. Waiting is the lesser evil: the lines sit in the logger ring,
  a truly stuck client is removed by the queue's stall/overrun rules, and a ring overrun is reported
  as a gap line.
- **At most `txCapacity() / 4` per tick** — a quarter of the *actual* ring, which `setup()` rounds
  down to a power of two; a single `processCommand()` like `showInformations` produces far more than
  one tick should push. Only the first line of a tick may exceed this share, or a line tripling as
  UTF-8 would stall `_readPos` for good. With the default 4 KB ring every line fits eventually; with
  `OPENKNX_WEBSOCKET_TX_BUFFER` ≤ 2048 a near-maximum line can wait until other traffic moves the
  write position past the ring end, because the padding record nearly doubles its cost there.

When not even an empty line fits, `loop()` returns before building any line. `maxLine` is
additionally clamped to `maxWebsocketPayload()` *minus the `log:` key*, otherwise the longest line
would be permanently unsendable on RP2040.

## Web UI: Console Page

- **Reconnect is automatic** (it lives in `base.js` and serves every page), but the console writes a visible marker on disconnect and reconnect — a gap in the log must be seen, not silently accepted
- That is what the earlier no-reconnect rule was protecting: scrambled or unnoticed log output. The marker addresses it without leaving every other page dead after a brief network hiccup
- **The firmware sends raw log lines**, ANSI escapes included — no markup is generated on the device. The browser maps the codes to the CSS classes `red`/`green`/`yellow`/`gray` and inserts every segment via `textContent`, so log text is never parsed as HTML and the WS payload equals the raw line length
- Chunks are bounded by `Webserver::maxWebsocketPayload()` (RP2040: 1496 B fixed frame buffer, ESP32: unbounded) — a larger payload could never be sent and the drain loop's retry would block the console forever
- Commands go out as `cmd:<text>` over the UI socket ([`UiSocket.md`](UiSocket.md)), log lines come back as `log:<line>`
