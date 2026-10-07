# Knx.md - KNX-Anbindung und Compile-Defines

Teil von [`AGENTS.md`](../../AGENTS.md).
Verwandt: [`Module.md`](Module.md), [`Webserver.md`](Webserver.md)

## KNX Concepts & Integration

**OpenKNX modules** implement an interface with: `init()`, `setup()`, `loop()`, `processCommand()`, `readFlash()`, `writeFlash()`.

**KNX parameters** are read from the generated `knxprod.h` via macros like `ParamNET_*`. Parameter definitions are in [`src/Network.share.xml`](../../src/Network.share.xml).

**KNX IP properties** (BAU interface):
- `PID_IP_ADDRESS`, `PID_SUBNET_MASK`, `PID_DEFAULT_GATEWAY`, `PID_IP_ASSIGNMENT_METHOD`
- `PID_IP_CAPABILITIES` → advertise DHCP+AutoIP (value: 6)
- Routing multicast: `PID_ROUTING_MULTICAST_ADDRESS` (mask 0x091A / 0x57B0)

**Function property** (object 160, property 5): transfers WiFi credentials from ETS via `Network.script.js`.

**Compile defines (important):**
```
OPENKNX_LED_IP          – LED for network status (optional)
OPENKNX_PING_TIMEOUT    – Ping timeout in ms (default 1000)
OPENKNX_PING_PARALLEL   – Max concurrent pings (default 5)
OPENKNX_NET_SPI_SPEED   – W5500 SPI speed Hz (default 28000000)
OPENKNX_MQTT            – Enable MQTT client/broker
OPENKNX_MQTT_STATUS_TIME     – Status publish interval ms (default 10000)
OPENKNX_MQTT_TASK_STACK      – ESP32 task stack bytes (default 4096)
OPENKNX_MQTT_BROKER_MAX_CLIENTS – Max broker clients (default 5/3)
OPENKNX_MQTT_BROKER_MAX_RETAINED – Max retained messages in broker (default 32)
OPENKNX_MQTT_BROKER_MAX_SUBS – Max subscriptions per broker client (default 16)
OPENKNX_WEBSOCKET_TX_BUFFER  – WS send queue, one ring for all clients (default 4096)
OPENKNX_WEBSERVER_MAX_CONN   – Webserver slots, HTTP + WS share them (ESP32 7, RP2040 3, or 6 with -D__LWIP_MEMMULT=2)
OPENKNX_WEBSERVER_RX_CAP     – RP2040: static buffer per connection slot, HTTP headers *and* WS payloads (default 1024)
OPENKNX_WEBSOCKET_RX_CAP     – ESP32: heap buffer per WS session, WS only (httpd buffers HTTP itself) (default 2048)
```

The `OPENKNX_WEB*` feature flags and what each one pulls in: [`Webserver.md`](Webserver.md). The
queue and slot sizes above: [`WsTxQueue.md`](WsTxQueue.md) and [`WebserverPlatform.md`](WebserverPlatform.md).
MQTT defines: [`MQTT.md`](MQTT.md).
