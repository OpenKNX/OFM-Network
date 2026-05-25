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
OPENKNX_WEBCLIENT              – enable the Webclient (openknxNetwork.webclient)
OPENKNX_WEBCLIENT_SLOTS        – parallel request slots (RP2040) / queue depth (ESP32) — default: 2
OPENKNX_WEBCLIENT_TIMEOUT      – total request timeout in ms — default: 10000
OPENKNX_WEBCLIENT_TASK_STACK   – ESP32 FreeRTOS worker task stack size in bytes — default: 6144
OPENKNX_WEBCLIENT_MAX_BODY     – maximum request body size in bytes — default: 4096
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
| `.insecure()` | Disable TLS certificate verification |
| `.ca(pemCert)` | PEM root CA for verified TLS (caller manages lifetime) |

### `send()` variants

```cpp
// Streaming: receive data chunks, get notified on completion
.send(DataCallback onData, DoneCallback onDone = nullptr);

// Fire-and-forget / HEAD: only care about success and HTTP status
.send(DoneCallback onDone);
```

**Callback signatures:**

```cpp
using DataCallback = std::function<bool(const uint8_t* data, size_t len)>;
//   return false → abort the request immediately

using DoneCallback = std::function<void(bool success, uint16_t httpStatus)>;
//   success=false → network error, timeout, or DataCallback returned false
//   success=true, httpStatus=0 → connection failed before headers were received
```

---

## Examples

### HTTPS GET with streaming

```cpp
openknxNetwork.webclient.get("https://api.example.com/data")
    .header("Authorization", "Bearer abc123")
    .insecure()
    .send(
        [](const uint8_t* data, size_t len) -> bool {
            // process chunk — return false to abort
            Serial.write(data, len);
            return true;
        },
        [](bool ok, uint16_t status) {
            logInfo("wclient", "HTTP %d ok=%d", status, ok);
        }
    );
```

### POST with JSON body

```cpp
openknxNetwork.webclient.post("https://api.example.com/event")
    .contentType("application/json")
    .body("{\"temperature\": 22.5}")
    .send([](bool ok, uint16_t status) {
        if (!ok) logError("wclient", "POST failed (HTTP %d)", status);
    });
```

### HEAD — check server reachability

```cpp
openknxNetwork.webclient.head("http://192.168.1.100/update")
    .send([](bool ok, uint16_t status) {
        if (ok && status == 200) startUpdate();
    });
```

### Verified TLS with CA certificate

```cpp
static const char* MY_ROOT_CA = R"(
-----BEGIN CERTIFICATE-----
...
-----END CERTIFICATE-----
)";

openknxNetwork.webclient.get("https://api.secure.example.com/data")
    .ca(MY_ROOT_CA)    // must remain valid for the lifetime of the request
    .send([](bool ok, uint16_t s) { });
```

---

## SSL / TLS

SSL/HTTPS works on all supported platforms:

| Platform | SSL library | Notes |
|----------|------------|-------|
| ESP32 (WiFi + LAN) | mbedTLS (ESP-IDF built-in) | Runs in FreeRTOS worker task; non-blocking from main thread |
| RP2040 WiFi (PicoW) | BearSSL (arduino-pico built-in) | Plain HTTP: fully non-blocking via lwIP raw `tcp_`. HTTPS: BearSSL state machine driven one step per `loop()` — non-blocking |
| RP2040 LAN (W5500) | BearSSL (arduino-pico built-in) | Same as WiFi. BearSSL runs directly over our lwIP raw `tcp_` connection — bypasses `WiFiClientSecure` which was unusable on W5500 |

Use `.insecure()` for most embedded use cases to skip certificate verification and reduce memory usage.

---

## Architecture

### ESP32

Requests are processed in a dedicated FreeRTOS worker task (Core 0, `OPENKNX_WEBCLIENT_TASK_STACK` bytes). The task handles DNS resolution, TCP/SSL connect, header and body streaming. All `DataCallback` and `DoneCallback` calls are dispatched back to the main thread via an internal result queue drained in `loop()` — **user callbacks never need to be thread-safe**.

Up to `OPENKNX_WEBCLIENT_SLOTS` requests can be queued. Additional requests beyond the queue depth are rejected with an error and `onDone(false, 0)`.

### RP2040 (WiFi + W5500)

Plain HTTP is fully non-blocking. DNS resolution uses `OpenKNX::Network::DNS::query()`. TCP connections use the lwIP raw `tcp_` API (`tcp_connect`, `tcp_write`, `tcp_output`, `tcp_recv` callback) — the same pattern as the MQTT client and RP2040 webserver. Data arrives via `lwipRecv` callback and is stored as a zero-copy pbuf chain directly in the slot. The chain is drained in `loop()` via `platformRead`, which advances the TCP receive window (`tcp_recved`) incrementally as bytes are consumed — this provides natural flow control without any intermediate buffer or data loss. The slot state machine advances (`RESOLVING → CONNECTING → SSL_HANDSHAKE → SENDING_HDR → SENDING_BODY → HEADERS → BODY`) entirely without blocking.

**HTTPS is fully non-blocking**: The BearSSL state machine is driven one step per `loop()` call in the `SSL_HANDSHAKE` state. Each iteration checks the engine state (`BR_SSL_SENDREC`/`BR_SSL_RECVREC`/`BR_SSL_SENDAPP`), sends or receives one batch of TLS records, and returns immediately. The W5500 background context delivers incoming TLS records via the `lwipRecv` pbuf chain between `loop()` calls. Handshake typically completes in 20–100 loop iterations.

Up to `OPENKNX_WEBCLIENT_SLOTS` requests can be active simultaneously. Additional requests are queued in an unbounded pending deque and started as slots become free.

---

## Limitations

- HTTP/1.1 with `Connection: close` — no keep-alive, one response per connection
- Both plain and chunked `Transfer-Encoding` responses are decoded transparently
- `HEAD`, `204 No Content`, and `304 Not Modified` responses never invoke `DataCallback`
- Request bodies larger than `OPENKNX_WEBCLIENT_MAX_BODY` should be avoided on RP2040 due to RAM constraints
- Aborting a request from within `DataCallback` (return `false`) causes immediate connection close; `DoneCallback` is called with `success=false`
