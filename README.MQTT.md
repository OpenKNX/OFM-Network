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
OPENKNX_MQTT                    – enable MQTT (required)
OPENKNX_MQTT_STATUS_TIME        – status publish interval in ms (default: 10000)
OPENKNX_MQTT_TASK_STACK         – ESP32 FreeRTOS task stack size in bytes (default: 4096)
OPENKNX_MQTT_BROKER_MAX_CLIENTS – max simultaneous broker clients (default: 5 ESP32, 3 RP2040)
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

---

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

Wildcards `+` (single level) and `#` (multi level) are supported.

On **ESP32**, `subscribe()` is mutex-protected and can be called from any task.  
On **RP2040**, call only from the main Arduino loop (no lwIP callback context).

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
- QoS 0 and QoS 1
- Keep-alive per connection with timeout
- Local observer subscriptions (the device itself can subscribe to messages routed through its own broker via `openknxNetwork.mqtt.subscribe()`)

---

