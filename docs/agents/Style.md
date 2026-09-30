# Style.md - Codestil und Formatierung

Teil von [`AGENTS.md`](../../AGENTS.md).
Verwandt: [`Build.md`](Build.md)

## Coding Conventions

- **No `delay()`** — everything non-blocking, state machines with `millis()` checks
- **Platform guards**: `#ifdef ARDUINO_ARCH_ESP32` / `#ifdef ARDUINO_ARCH_RP2040`
- **Connectivity guards**: `#ifdef KNX_IP_WIFI` / `#ifdef KNX_IP_LAN`
- No external libraries in `library.json` — only Arduino framework builtins and OpenKNX core
- **Comments in English.** Older files still carry German comments; convert them when you touch the surrounding code rather than in a sweep of their own
- Comment sparingly and briefly — one line for the non-obvious *why*, nothing more. Long rationale blocks go stale faster than the code they sit next to; put them in the commit message or a README instead
- OTA port: ESP32 = 3232, RP2040 = 2040 (both platforms support OTA, implementation is identical)
- mDNS hostname: auto-generated as `OpenKNX-XXXXXXXX` (8 hex chars from serial number)

### Formatting (`.clang-format`)

Based on **LLVM style** with the following deviations — strictly follow when generating code:

- **Curly braces** always on their **own line** (Allman style) — applies to classes, functions, `if`, `else`, `for`, `while`, `case`, `enum`, `struct`, `namespace`, `extern`
- **No column limit** (`ColumnLimit: 0`) — no artificial line breaks
- **4 spaces** indentation, **no tabs** (`UseTab: Never`)
- `namespace` content is indented (`NamespaceIndentation: All`)
- `case` labels are indented (`IndentCaseLabels: true`)
- Short `case` labels, enums, lambdas, and functions may remain single-line
- `if` without braces only allowed for a **single** statement (`OnlyFirstIf`) — no single-line `else`
- Pointer alignment derived from existing code (`DerivePointerAlignment: true`)
- Preprocessor directives **not** indented (`IndentPPDirectives: None`)
