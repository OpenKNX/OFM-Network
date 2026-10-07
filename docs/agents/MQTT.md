# MQTT.md - MQTT-Client und -Broker

Teil von [`AGENTS.md`](../../AGENTS.md).
Verwandt: [`Module.md`](Module.md), [`TelegramJson.md`](TelegramJson.md), [`Knx.md`](Knx.md)

## `OpenKNX::Network::MQTT::Module` (`src/OpenKNX/Network/MQTT/`)
MQTT 3.1.1 client + broker, no external dependencies. Guard: `#ifdef OPENKNX_MQTT`. Accessed as `openknxNetwork.mqtt`.

`Packet` (wire format), `Client` (external broker), `Broker` (embedded), `Module` (public API). Broker/Client run in a FreeRTOS task on ESP32, in `loop()` on RP2040; `setup()` is deferred until `established()`.

**Rules that are not visible at the call site** — full reference in [`README.MQTT.md`](../../README.MQTT.md):

- **`publish()`/`publishP()` queue, they never write the socket.** `true` means *accepted*, not delivered. That is what makes them safe from timing-critical code such as the KNX loop — never publish inline.
- **`connected()` is a mirrored flag.** Never reach into `_broker`/`_client` from a caller; that would take `_mutex` from the wrong task.
- **`setup()` completes even when `begin()` fails**, otherwise it re-runs — with its log block — every tick. Only pre-`begin()` failures (`mqttTxReserve()` OOM) return early to be retried.
- **`mqttTxBuf` grows on demand**: pass `mqttTxBufSize` to the `build*()` functions, never a literal; grow only in the send path, never in `enqueue()`.
- **DNS via `OpenKNX::Network::DNS::query()`**, never `dns_gethostbyname()`. Its callback always comes from `DNS::loop()`, never inline and never from the network IRQ.
- **RP2040 client and broker: the lwIP callbacks only set flags and chain pbufs** — they run in the ethernet IRQ. Everything else runs in `loop()`; every pcb access sits under `LWIPMutex`, because `lwipErr` nulls it from the IRQ. Broker slots: the accept IRQ takes a slot with `!active && !pcb` and only sets `pcb`; `loop()` opens it (`openConn`), and `closeConn()` clears `active` last.
- `_mutex` is **recursive** — a subscription callback may re-enter `subscribe()` from the MQTT task.
- **The KNX-telegram feature is deliberately not here.** `MQTT::Module` knows nothing about KNX frames; `ParamNET_MQTTTPRawData` lives in `Network::Module::publishTelegram()` ([`TelegramJson.md`](TelegramJson.md)) and just calls `mqtt.publish()` like any other caller.
