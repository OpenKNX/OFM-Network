# TelegramJson.md - JSON-Writer, TelegramJson, GATable

Teil von [`AGENTS.md`](../../AGENTS.md).
Verwandt: [`MQTT.md`](MQTT.md), [`GroupMonitor.md`](GroupMonitor.md), [`Module.md`](Module.md)

## `OpenKNX::Format::JSON` (`src/OpenKNX/Format/JSON/`)

`Writer.h/.cpp` and `Reader.h/.cpp` — **use these instead of assembling JSON by hand.** `Writer` is a fluent builder (`beginObject()`/`field(k, v)`/`endObject()`, overloads for `const char*`, `std::string`, `int32_t`, `uint32_t`, `float`, `bool`, plus `null()`); it escapes keys and values itself, so no manual quoting at call sites. `reset()` clears the content but keeps the internal `std::string` capacity — a `Writer` held as a member and reset per message therefore stops allocating once warmed up, which is why `GroupMonitor` and `MQTT::Module` each keep one for their per-telegram broadcast.

## `OpenKNX::Network::TelegramJson` (`src/OpenKNX/Network/TelegramJson.h/.cpp`)

`buildTelegramJson(TPUart::Frame&, JSON::Writer&)` decodes one TP telegram, shared by `GroupMonitor` ([`GroupMonitor.md`](GroupMonitor.md), WebSocket) and `Network::Module::publishTelegram()` ([`MQTT.md`](MQTT.md)) so both payloads stay identical by construction. Field reference: [`README.MQTT.md`](../../README.MQTT.md). Caller does `frame.isFrame()` and `json.reset()`. JSON only — reaching the TP-UART is `Network::Module::tpUart()`'s job.

- **Guard `OPENKNX_TELEGRAMJSON`**, defined by `TelegramJson.h` itself. Everything decoder-dependent keys off that one macro — `gatable`, the `groupmonitor` member, the `OPENKNX_CHARSET` auto-define — instead of repeating the condition. The header is included unconditionally, being pure preprocessor when off.
- `hex` is the APDU **from the APCI octet on**, so its first byte is not payload. `raw` is the whole frame.
- Don't use `frame.human*()` for addresses: zero-pads and returns `std::string` by value.

**`Network::Module::tpUart()`** abstracts the BAU accessor difference: `Bau07B0` via `getDataLinkLayer()`, `Bau091A` via `getSecondaryDataLinkLayer()` (there `getPrimaryDataLinkLayer()` is the IP side). `Bau57B0` has no TP-UART, so nothing here compiles in for it. Never touch `knx.bau()` directly for this.

`GATable` (member `openknxNetwork.gatable`) maps GA → DPT from `/openknx_ga.tsv` (LittleFS): sorted POD vector, 4 B per GA, **no strings**, binary search, `begin()` idempotent and called every tick from `loop()`. Value decode uses the knx stack's `KNX_Decode_Value()`.
