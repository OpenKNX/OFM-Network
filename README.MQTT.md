# MQTT — OFM-Network

Built-in MQTT 3.1.1 implementation with no external dependencies. Supports two modes: **Client** (connects to an external broker) and **Broker** (acts as a minimal embedded broker). Both modes are available on ESP32 and RP2040.

---

## Enabling MQTT

Add to `platformio.ini` or `platformio.custom.ini`:

```ini
build_flags =
    -DOPENKNX_MQTT
```

Without `OPENKNX_MQTT`, all MQTT code is excluded from compilation.

To show the MQTT section in ETS, set the following in the device's share.xml:

```xml
<op:config name="%NET_ServiceMQTT%" value="1" />
```

MQTT is only active at runtime when enabled in ETS **and** `OPENKNX_MQTT` is defined.

---

## Compile Flags

```
OPENKNX_MQTT                     – enable MQTT (required)
OPENKNX_MQTT_STATUS_TIME         – status publish interval in ms (default: 10000)
OPENKNX_MQTT_TASK_STACK          – ESP32 FreeRTOS task stack size in bytes (default: 4096)
OPENKNX_MQTT_BROKER_MAX_CLIENTS  – max simultaneous broker clients (default: 5 ESP32, 3 RP2040)
OPENKNX_MQTT_BROKER_MAX_RETAINED – max retained messages stored by broker (default: 32)
OPENKNX_MQTT_BROKER_MAX_SUBS     – max subscriptions per broker client (default: 16)
OPENKNX_MQTT_MAX_QUEUE           – max messages waiting to be sent; entries are allocated on demand, so this is a count cap, not reserved memory (default: 32)
OPENKNX_MQTT_TXBUF_MIN           – start size of the shared TX scratch buffer, Broker and Client; grows on demand (default: 512)
OPENKNX_MQTT_TXBUF_MAX           – ceiling for that growth; larger messages are rejected (default: 2048, never below _MIN)
```

---

## KNX Parameters (ETS)

| Parameter | Description |
|-----------|-------------|
| `ParamNET_MQTT` | Enable MQTT |
| `ParamNET_MQTTMode` | Mode: 0 = Client, 1 = Broker |
| `ParamNET_MQTTServer` | Broker host FQDN or IP (Client mode, max 20 chars) |
| `ParamNET_MQTTPort` | TCP port (default: 1883) |
| `ParamNET_MQTTUsername` | Username (max 20 chars, empty = no auth) |
| `ParamNET_MQTTPassword` | Password (max 20 chars) |
| `ParamNET_MQTTCustomPrefix` | Enable custom device prefix |
| `ParamNET_MQTTPrefix` | Custom prefix string (max 20 chars) |
| `ParamNET_MQTTTPRawData` | Publish KNX TP raw frames (ESP32 TP-UART only, mask 0x07B0) |

---

## Topic Scheme

All topics published by the device use a **device prefix**:

```
openknx/<prefix>/
```

The prefix is determined as follows (in order):
1. If KNX is configured **and** `ParamNET_MQTTCustomPrefix` is set: `openknx/<MQTTPrefix>/`
2. Otherwise: `openknx/<last8charsOfSerialNumber>/`

### Built-in Topics

| Topic | Payload | Retained | Description |
|-------|---------|----------|-------------|
| `openknx/<prefix>/status` | `1` | yes | Published every `OPENKNX_MQTT_STATUS_TIME` ms while connected |
| `openknx/<prefix>/status` | `0` | yes | LWT (Last Will & Testament) — published by broker when device disconnects |
| `openknx/<prefix>/uptime` | seconds | no | Seconds since boot, published alongside status |
| `openknx/<prefix>/knxtp/raw` | binary | no | KNX TP raw frames (ESP32 TP-UART, requires `ParamNET_MQTTTPRawData`) |

## API

The MQTT module is accessed via `openknxNetwork.mqtt`.

### Publish

```cpp
// Publish to an absolute topic
openknxNetwork.mqtt.publish("my/topic", "payload");
openknxNetwork.mqtt.publish("my/topic", binaryData, len, /*qos=*/0, /*retain=*/false);

// Publish with device prefix prepended (openknx/<prefix>/my/topic)
openknxNetwork.mqtt.publishP("my/topic", "payload");
openknxNetwork.mqtt.publishP("my/topic", binaryData, len);
```

Returns `false` if not connected or (ESP32) if the mutex cannot be acquired.

### Subscribe

```cpp
openknxNetwork.mqtt.subscribe(
    "openknx/+/cmd",
    [](const char *topic, const void *payload, size_t len) {
        // handle message
    },
    /*qos=*/0
);
```

Wildcards `+` (single level) and `#` (multi level) follow MQTT 3.1.1 §4.7: `+` stands for exactly one — possibly empty — level, so `a/+` matches `a/`; `#` must be the last character and covers the parent level too, so `a/#` matches `a` as well as `a/b/c`. Both are only wildcards as a complete level — `a+b` is a literal topic.

On **ESP32**, `subscribe()` is mutex-protected and can be called from any task.  
On **RP2040**, call only from the main Arduino loop (no lwIP callback context).

In **Client mode**, subscribed callbacks are also triggered when the device itself publishes to a matching topic (local echo). No separate API is required. The echo fires when the message is actually sent, not when `publish()` returns — so it arrives a few milliseconds later, and on ESP32 in the MQTT task rather than the publishing one.

### Status

```cpp
bool ok = openknxNetwork.mqtt.connected();
std::string prefix = openknxNetwork.mqtt.devicePrefix(); // e.g. "openknx/abc12345/"
```

---

## Architecture

### Setup & Lifecycle

MQTT setup is **deferred**: `Network::Module::loop()` calls `mqtt.setup()` only once `established()` is true (IP address obtained). This ensures TCP stack is ready before any broker socket or client connection is attempted.

### ESP32

The broker/client loop runs in a dedicated **FreeRTOS task** (Core 0, `OPENKNX_MQTT_TASK_STACK` bytes). `publish()` and `subscribe()` are protected by a FreeRTOS mutex, making them safe to call from any task or the main loop.

### RP2040

The broker/client loop runs in the main Arduino **`loop()`** (single-threaded, `NO_SYS=1`). The broker uses lwIP callbacks (`tcp_accept`, `tcp_recv`, `tcp_err`, `tcp_poll`) only for buffering incoming data — all MQTT protocol processing happens in `loop()`, never inside a callback.

### Send Queue

`publish()` and `publishP()` do **not** write to the socket — they copy the message into an outbound FIFO and return immediately, so they never block and are safe to call from timing-critical code. A `true` return means the message was accepted for sending, not that it was delivered.

The actual send happens from the MQTT loop (the FreeRTOS task on ESP32, `loop()` on RP2040), in order. A send that fails — typically lwIP's `tcp_write()` reporting a full buffer under load — leaves its entry queued and is retried, so momentary back-pressure costs latency rather than data.

Entries are allocated on demand and sized to the actual topic + payload, so an idle queue costs nothing; `OPENKNX_MQTT_MAX_QUEUE` (default 32) is a cap on the number of pending messages, not reserved memory. Messages are dropped only once that cap is reached — i.e. if sending cannot keep up over a sustained period — and the count is logged once per status interval.

### Shared TX Buffer

`Broker` and `Client` both need a scratch buffer for outgoing packets, but `Module` only ever runs one of the two at a time (`ParamNET_MQTTMode`, see `cfgBroker()`). Rather than each owning its own, both use one shared buffer, `mqttTxBuf` (`OPENKNX_MQTT_TXBUF_MIN`, declared in `Packet.h`) — a free function, not a member, so it's not duplicated per class either way.

`mqttTxBuf` stays `nullptr` until MQTT is actually enabled at runtime (`ParamNET_MQTT`): `Module::setup()` allocates it, from PSRAM if available, right before the first `Broker`/`Client::begin()` call, and never frees it — MQTT cannot be toggled off again without a restart. A firmware image that supports `OPENKNX_MQTT` but whose ETS project leaves it disabled therefore costs 0 bytes for this buffer.

The size is **not** fixed at compile time, because MQTT payloads differ far too much between installations for one number to fit them all. `mqttTxReserve(need)` grows the buffer in 128-byte steps up to `OPENKNX_MQTT_TXBUF_MAX` and never shrinks it, so it settles on the largest message this device actually sends and then stops allocating. `OPENKNX_MQTT_TXBUF_MIN` is only the start size. Two rules follow for anyone touching this code: pass **`mqttTxBufSize`** to the `build*()` functions, never a literal or `sizeof(mqttTxBuf)`; and call `mqttTxReserve()` only from the send path (`drainQueue()`, `Broker::routePublish()`) — `publish()` runs in arbitrary tasks and must not reallocate the buffer under the MQTT task.

### File Structure

```
src/OpenKNX/Network/MQTT/
  Packet.h/.cpp    – MQTT 3.1.1 packet serialization/deserialization, topic matching
  Client.h/.cpp    – MQTT client (connects to an external broker)
  Broker.h/.cpp    – MQTT broker (accepts client connections, routes messages)
  Module.h/.cpp    – OpenKNX::Module subclass, public API, platform integration
```

### Broker Features

- Authentication via username/password (from KNX parameters)
- Retained messages (in-memory `std::map`)
- Keep-alive per connection with timeout
- Local observer subscriptions (the device itself can subscribe to messages routed through its own broker via `openknxNetwork.mqtt.subscribe()`)

#### QoS in the broker

Inbound the broker speaks the full handshake for all three levels, outbound it delivers **only QoS 0** and grants QoS 0 in every SUBACK — it assigns no message IDs and tracks no outgoing acks, so anything above 0 would be protocol-violating on the wire.

QoS 2 inbound is accepted (PUBREC/PUBREL/PUBCOMP are exchanged) but the message is routed on arrival rather than held until PUBREL. That makes it **at-least-once, not exactly-once**: if the publisher retransmits because its PUBREC got lost, subscribers see the message twice. Fine for the KNX payloads this broker carries; do not rely on it for anything that must not be duplicated.

### Client Features

- Automatic reconnect, fixed 5 s retry (no backoff)
- QoS 0 and QoS 1
- Keep-alive with ping/pong
- Last Will & Testament (LWT)
- Local echo: `subscribe()` callbacks fire immediately on own publishes

`Client::begin()` reports success as soon as the client is **set up** — a failed initial TCP connect is runtime state, not a setup error, and is retried from `Client::loop()` every 5 seconds. Reporting it as failure used to break the retry entirely: `Module::setup()` keeps `_initialized` false, `Module::loop()` returns before `Client::loop()`, and the reconnect timer armed in `begin()` never fires. What actually retried was `setup()` itself, at tick rate, printing its whole log block each time. For the same reason `_initialized` is now set even when `Broker::begin()` fails — an inactive broker is a no-op in `loop()`, whereas repeating setup() is not.

The server may be a hostname or an IP address. On ESP32 `WiFiClient::connect()` resolves it; on RP2040 `tcpConnect()` goes through [`OpenKNX::Network::DNS::query()`](src/OpenKNX/Network/DNS.h) — the same asynchronous helper Ping and the Webclient use — and the client sits in `STATE_RESOLVING` until the callback arrives. Nothing blocks. Two details that state exists for: the 5 s retry in `loop()` must not fire a second lookup while one is in flight, and a result arriving from a superseded attempt is dropped via `_resolveId`. On a DNS cache hit the callback runs **inline**, still inside `tcpConnect()` — so the caller must not touch `_state` or `_reconnectTimer` after that call.

---

