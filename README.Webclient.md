# Webclient — OFM-Network

Non-blocking HTTP/HTTPS client with SSL support, streaming response callbacks, and a fluent builder API. Available on ESP32 (WiFi + LAN) and RP2040 (WiFi + W5500).

---

## Enabling

Add to `platformio.ini` or `platformio.custom.ini`:

```ini
build_flags =
    -DOPENKNX_WEBCLIENT
```

Without `OPENKNX_WEBCLIENT`, all Webclient code is excluded from compilation.

---

## Compile Flags

```
OPENKNX_WEBCLIENT                  – enable the Webclient (openknxNetwork.webclient)
OPENKNX_WEBCLIENT_SLOTS            – parallel request slots (RP2040) / queue depth (ESP32) — default: 2
OPENKNX_WEBCLIENT_TIMEOUT          – total request timeout in ms — default: 10000
OPENKNX_WEBCLIENT_TASK_STACK       – ESP32 FreeRTOS worker task stack size in bytes — default: 6144
OPENKNX_WEBCLIENT_MAX_BODY         – default max response body size buffered into Response.body() — default: 4096
OPENKNX_WEBCLIENT_BUF_SIZE         – RP2040 per-slot TX/RX buffer size in bytes — default: 512
```

---

## API

Access via `openknxNetwork.webclient`.

### Factory methods

```cpp
openknxNetwork.webclient.get   (url)   // HTTP GET
openknxNetwork.webclient.post  (url)   // HTTP POST
openknxNetwork.webclient.put   (url)   // HTTP PUT
openknxNetwork.webclient.del   (url)   // HTTP DELETE
openknxNetwork.webclient.patch (url)   // HTTP PATCH
openknxNetwork.webclient.head  (url)   // HTTP HEAD
```

Each returns a `Request` object on which you chain builder modifiers before calling `send()`.

### Builder modifiers

| Method | Description |
|--------|-------------|
| `.header(name, value)` | Add a custom HTTP request header |
| `.body(text)` | Set the request body from a `std::string` |
| `.body(data, len)` | Set the request body from a byte buffer |
| `.contentType(ct)` | Shorthand for `.header("Content-Type", ct)` |
| `.verifyCertificate(pemCert)` | Verify TLS with a PEM root CA certificate (caller manages lifetime) |
| `.onData(cb)` | Set streaming data callback — called for each body chunk |
| `.onDone(cb)` | Set completion callback — receives a `Response` object |
| `.maxBodySize(maxSize)` | Override the response body buffer limit (default: `OPENKNX_WEBCLIENT_MAX_BODY`) |
| `.ignoreBody()` | Disable response body buffering — `res.body()` stays empty; use when only the status matters or the body is evaluated via `onData` |
| `.ignoreHeaders()` | Discard response headers — `res.header()` always returns `""` (saves heap; headers are stored by default) |

### `send()`

```cpp
bool send();
```

Enqueues the request. Returns `false` if the queue is full or the URL is invalid — in that case `onDone` is called with an empty `Response` (`success() == false`, `status() == 0`).

### Body reading

The response body is always read to completion and the connection is gracefully closed. `onData` is called for each chunk whenever it is set, regardless of buffering.

By default the body is buffered into `Response.body()` up to `OPENKNX_WEBCLIENT_MAX_BODY` bytes. When `Content-Length` is known from the response headers, the internal buffer is pre-reserved to avoid heap fragmentation.

| Builder call | `res.body()` populated | Heap pre-reserved | `onData` called |
|---|---|---|---|
| *(none)* | Yes (up to `OPENKNX_WEBCLIENT_MAX_BODY`) | If Content-Length known | Yes, if set |
| `.maxBodySize(maxSize)` | Yes (up to `maxSize`) | If Content-Length known | Yes, if set |
| `.ignoreBody()` | No | — | Yes, if set |

If the body exceeds the active `maxSize` limit, body buffering stops and `bodyIncomplete()` is set to `true` in `onDone`. `success()` is unaffected — it reflects only the HTTP status and network result.

### Callback signatures

```cpp
using DataCallback = std::function<void(const uint8_t* data, size_t len)>;

using DoneCallback = std::function<void(const Response& response)>;
```

### `Response` object

```cpp
bool              res.success()          // true if no network error and HTTP 2xx — independent of body completeness
uint16_t          res.status()           // HTTP status code (0 if connection failed before headers)
uint32_t          res.bodySize()         // body size: Content-Length header if present, otherwise bytes received
bool              res.bodyIncomplete()   // true if body was truncated (maxSize exceeded, connection drop, or timeout) — always false when .ignoreBody() is used; does not affect success()
const std::string& res.body()            // buffered body; non-empty only if .maxBodySize() was used (or default buffering active)
std::string       res.header("name")     // case-insensitive header lookup; "" if not present or .ignoreHeaders() was set
```

Headers are stored by default. Call `.ignoreHeaders()` to discard them and save heap.

---

## Examples

### Fire-and-forget POST (no body buffering)

```cpp
openknxNetwork.webclient.post("https://api.example.com/event")
    .contentType("application/json")
    .body("{\"temperature\": 22.5}")
    .ignoreBody()
    .onDone([](const Response& res) {
        if (!res.success()) logError("wclient", "POST failed (HTTP %d)", res.status());
    })
    .send();
```

### GET with buffered response body

```cpp
openknxNetwork.webclient.get("http://192.168.1.1/config.json")
    .maxBodySize(2048)
    .onDone([](const Response& res) {
        if (res.success())
            logInfo("wclient", "Config (%u bytes): %s", res.bodySize(), res.body().c_str());
    })
    .send();
```

### GET with streaming chunks

```cpp
openknxNetwork.webclient.get("https://api.example.com/stream")
    .ignoreBody()
    .onData([](const uint8_t* data, size_t len) {
        // process chunk
        Serial.write(data, len);
    })
    .onDone([](const Response& res) {
        logInfo("wclient", "Done, HTTP %d, %u bytes", res.status(), res.bodySize());
    })
    .send();
```

### HEAD — check server reachability

```cpp
openknxNetwork.webclient.head("http://192.168.1.100/update")
    .onDone([](const Response& res) {
        if (res.success()) startUpdate();
    })
    .send();
```

### Response headers

```cpp
openknxNetwork.webclient.get("https://api.example.com/data")
    .maxBodySize(4096)
    .ignoreHeaders()
    .onDone([](const Response& res) {
        if (res.success())
            logInfo("wclient", "Type: %s  Body: %s",
                res.header("Content-Type").c_str(), res.body().c_str());
    })
    .send();
```

### Verified TLS with CA certificate

```cpp
static const char* MY_ROOT_CA = R"(
-----BEGIN CERTIFICATE-----
...
-----END CERTIFICATE-----
)";

openknxNetwork.webclient.get("https://api.secure.example.com/data")
    .verifyCertificate(MY_ROOT_CA)    // must remain valid for the lifetime of the request
    .maxBodySize(4096)
    .onDone([](const Response& res) { })
    .send();
```

---

## SSL / TLS

SSL/HTTPS works on all supported platforms:

| Platform | SSL library | Notes |
|----------|------------|-------|
| ESP32 (WiFi + LAN) | mbedTLS (ESP-IDF built-in) | Runs in FreeRTOS worker task; non-blocking from main thread. Supports `verifyCertificate()`. |
| RP2040 WiFi (PicoW) | BearSSL (arduino-pico built-in) | Plain HTTP: fully non-blocking via lwIP raw `tcp_`. HTTPS: BearSSL state machine driven one step per `loop()` — non-blocking. Supports `verifyCertificate()`. |
| RP2040 LAN (W5500) | BearSSL (arduino-pico built-in) | Same as WiFi. BearSSL runs directly over our lwIP raw `tcp_` connection — bypasses `WiFiClientSecure` which was unusable on W5500. Supports `verifyCertificate()`. |

Without a verify call, TLS connections skip certificate verification (insecure mode).

---

## Architecture

### ESP32

Requests are processed in a dedicated FreeRTOS worker task (Core 0, `OPENKNX_WEBCLIENT_TASK_STACK` bytes). The task handles DNS resolution, TCP/SSL connect, header and body streaming. All `DataCallback` and `DoneCallback` calls are dispatched back to the main thread via an internal result queue drained in `loop()` — **user callbacks never need to be thread-safe**.

The task always reads the response body to completion before signalling done. `DataCallback` entries are only queued when an `onData` callback is registered, avoiding unnecessary queue pressure for fire-and-forget requests. Body buffering (`.maxBodySize()`) is accumulated in the task and moved into the `Response` object before dispatch.

Up to `OPENKNX_WEBCLIENT_SLOTS` requests can be queued. Additional requests beyond the queue depth are rejected: `send()` returns `false` and `onDone` is called immediately with an empty `Response`.

### RP2040 (WiFi + W5500)

Plain HTTP is fully non-blocking. DNS resolution uses `OpenKNX::Network::DNS::query()`. TCP connections use the lwIP raw `tcp_` API (`tcp_connect`, `tcp_write`, `tcp_output`, `tcp_recv` callback) — the same pattern as the MQTT client and RP2040 webserver. Data arrives via `lwipRecv` callback and is stored as a zero-copy pbuf chain directly in the slot. The chain is drained in `loop()` via `platformRead`, which advances the TCP receive window (`tcp_recved`) incrementally as bytes are consumed — this provides natural flow control without any intermediate buffer or data loss. The slot state machine advances (`RESOLVING → CONNECTING → SSL_HANDSHAKE → SENDING_HDR → SENDING_BODY → HEADERS → BODY`) entirely without blocking.

**HTTPS is fully non-blocking**: The BearSSL state machine is driven one step per `loop()` call in the `SSL_HANDSHAKE` state. Each iteration checks the engine state (`BR_SSL_SENDREC`/`BR_SSL_RECVREC`/`BR_SSL_SENDAPP`), sends or receives one batch of TLS records, and returns immediately. The W5500 background context delivers incoming TLS records via the `lwipRecv` pbuf chain between `loop()` calls. Handshake typically completes in 20–100 loop iterations.

Up to `OPENKNX_WEBCLIENT_SLOTS` requests can be active simultaneously. Additional requests are queued in an unbounded pending deque and started as slots become free.

Each slot's TX/RX buffers (`OPENKNX_WEBCLIENT_BUF_SIZE` bytes each) are allocated in `startSlot()` and freed in `finishSlot()` — PSRAM if available (`PSRAM_CALLOC`, see [Helper.h](../OGM-Common/src/OpenKNX/Helper.h)), plain heap otherwise. They are not resident while a slot is idle. Allocation runs from `Handler::loop()`, before the connection is opened — never from an lwIP callback — so an allocation failure is caught synchronously and just fails the request (`onDone` with an empty `Response`).

---

## Limitations

- HTTP/1.1 with `Connection: close` — no keep-alive, one response per connection
- Both plain and chunked `Transfer-Encoding` responses are decoded transparently
- `HEAD`, `204 No Content`, and `304 Not Modified` responses never invoke `DataCallback`
- Request bodies larger than `OPENKNX_WEBCLIENT_MAX_BODY` should be avoided on RP2040 due to RAM constraints
- `.maxBodySize()` exceeding `maxSize` stops body buffering; `bodyIncomplete() == true` in `onDone`, but `success()` is unaffected
- `.ignoreHeaders()` discards all response headers — saves heap on memory-constrained RP2040 deployments

### Platform-specific Notes

**RP2040 — Loopback addresses (127.0.0.1)**

Do not send webclient requests to `127.0.0.1` (localhost) from RP2040. Both the webserver and webclient run in the same main event loop (lwIP `NO_SYS=1`). A loopback request will block waiting for a response while the webserver cannot process it in the same loop — deadlock. Use the device's actual IP address instead (e.g. `192.168.1.x`). This limitation does not apply to ESP32 (FreeRTOS task isolation).
