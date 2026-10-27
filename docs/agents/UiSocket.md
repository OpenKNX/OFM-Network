# UiSocket.md - UI-Socket /openknx/ws

Teil von [`AGENTS.md`](../../AGENTS.md).
Verwandt: [`Webserver.md`](Webserver.md), [`WsTxQueue.md`](WsTxQueue.md), [`Webconsole.md`](Webconsole.md), [`GroupMonitor.md`](GroupMonitor.md)

## UI Socket (`/openknx/ws`)

One WebSocket for the entire web interface, open on **every** page — it replaced the per-feature
endpoints (`/console`, `/groupmonitor`); those pages remain, only their own sockets are gone.
Full reference: [`README.Webserver.md` § UI Socket](../../README.Webserver.md#ui-socket).

Two functions, and that is deliberately all:

```cpp
openknxNetwork.webserver.ui.sendMessage(key, data, len = 0);   // always broadcast
openknxNetwork.webserver.ui.trySendBinaryMessage(key, prefix, prefixLen, data, len); // best-effort, always broadcast
openknxNetwork.webserver.ui.receiveMessage(cb);                // cb(key, data, len)
```

- **Binary output is best-effort and never waits for the ESP32 state lock.** The queued WebSocket
  binary payload is `key:` followed by the optional prefix and raw data. `base.js` splits that key
  and passes the remaining bytes to the handler as a `Uint8Array`. A busy lock or insufficient queue
  space returns `false`; producers should drop that sample rather than retry in a tight loop.
- **The firmware stores no keys.** No table, no lookup, no "unknown key" branch: the dispatcher splits
  at the first `:` and hands `key`/`data`/`len` to every registrant, which filters by `strcmp` itself.
  Incoming traffic is rare, so the cost is a few comparisons per command. **Who only sends registers
  nothing** — `tp` and `log` have no handler at all.
- **A receive handler is a network callback and must not block** — on RP2040 that is the lwIP
  interrupt. Record and defer, the way `Webconsole` ([`Webconsole.md`](Webconsole.md)) uses `_pendingCmd` and the `prog` key uses
  `_pendingProg`. Deferring the dispatch inside the layer was tried and dropped again: it would have
  forced a copy, a size cap and a "second message in the same tick is lost" case onto an API whose
  handlers are trivial anyway. See README.Webserver.md § Callback context for the full table.
- **`ping`/`pong` covers the direction the device cannot.** RP2040's `onPoll` ping and ESP32's TCP
  keepalives detect a dead *browser*; a rebooted *device* sends neither FIN nor RST, so the page
  would keep a green dot until it happened to send something. `base.js` pings after a second of
  silence and gives up 2 s after an *unanswered* ping; the firmware answers with an empty `pong`, so
  a reboot surfaces in about three seconds. **Giving up must not mean `close()` and wait for
  `onclose`** — a dead peer answers nothing, so the socket only reaches CLOSING and the close event
  arrives whenever the TCP timeout does (Firefox waits 20 s). The verdict is therefore final: detach
  the handlers, report `closed` and schedule the reconnect straight away. Without that the page kept
  a green dot for another 20 s on both platforms, which is exactly the failure the check exists for. **Never conclude from silence alone** — a background tab's
  timer is throttled to ~1/min, and the first version did exactly that and tore down healthy
  connections once a minute. The check measures how long the timer slept and re-bases instead of
  judging when that exceeds its interval. Answering happens in the network callback, which is fine here —
  `sendMessage()` only enqueues ([`WsTxQueue.md`](WsTxQueue.md)) and does not allocate. Since `pong` is a broadcast it resets every
  other tab's timer too, so the rate stays at ~one ping per second however many pages are open.
- **`onState()` is edge-triggered.** With the network down every reconnect attempt fails again, and
  reporting each one as a fresh event filled the console with a gap marker per try — at the 30 s
  backoff ceiling, forever. `fire()` therefore drops a repeat of the state already reported, and a
  late registrant is handed the current state instead of waiting for the next change.
- **`'closed'` carries a reason** (`cb(state, reason)`): connection duration plus *who* ended it —
  `kein Pong innerhalb 2 s` is the browser's own verdict, `vom Gerät geschlossen (1006)` is the device
  (or the network) dropping TCP. The console prints it in its gap marker, the navigation as tooltip.
  Without it a reconnect loop looked the same from both sides, and the device's own drop warning never
  reaches the page: it is logged after the client is gone, and the next client starts at "now".
- **The navigation carries the socket's own state** (`#ws-dot`/`#ws-text`, updated from
  `OpenKNX.onState()`), rendered server-side as "Verbinde…" because the page arrives over HTTP before
  the socket exists. Without JavaScript it stays that way, which is accurate — there is no connection.
- **`connect` is a reserved key** delivered to all handlers when a client appears, meaning "send your
  current state". A reconnect is indistinguishable from a first connect — intentionally, that is what
  makes a client resynchronise completely. `connect` and `pong` are therefore **rejected on the
  inbound path**: the endpoint is unauthenticated, and a client sending `connect:` would otherwise run
  the whole `publishStatus()` — on RP2040 inside the ethernet interrupt, `delay(1)` of the temperature
  ADC included.
- **No tick, and no change detection in the layer.** The layer transports; *when* to send is the
  producer's decision. `publishStatus()` therefore compares the handful of scalars it already holds
  (~20 B) rather than hashing or copying a payload. `up` (uptime) travels only when a full state is pushed (`connect`, prog change) and the
  browser counts on from there — otherwise something would have to be sent every second just to keep
  it moving. `mem`/`cpu` are coarsely rounded for the same reason.
- **No unicast.** The only candidate was the initial sync, and a broadcast does that just as well
  (~200 B once per page load), so no producer ever sees an `fd`. Unicast stays available one layer
  down via `sendWebsocketMessage(uri, msg, fd)`.
- `UI_WS_URI` is a class constant, not a macro, and deliberately not `/ws`: `addSocket()` matches
  exactly and has no dedup, so a module registering `/ws` itself would silently lose to the first
  entry.
