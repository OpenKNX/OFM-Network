# Build.md - Build, Tests, Web-Assets

Teil von [`AGENTS.md`](../../AGENTS.md).
Verwandt: [`Webserver.md`](Webserver.md), [`Style.md`](Style.md)

## Build & Test

No standalone `platformio.ini` — the module is included as a library in parent OpenKNX device projects. `platformio.network.ini` is empty (template for projects).

For testing: integrate into an OpenKNX device project that references this module.

## Web Assets (`web/assets/`, generated `webassets.h`)

All CSS/JS/SVG served under `/assets/*` are **not** hand-minified C++ string literals — they live as clean, readable source files in `web/assets/` at the repo root. `OGM-Common/scripts/pio/prepare.py` runs on every OAM (device project) build, collects a `web/assets/` folder from every included module **and the project itself**, minifies + gzip-compresses each file, and writes `include/webassets.h`. This module is documented here in depth as the first consumer of that generator; `OGM-Common/AGENTS.md` only notes that it exists.

- **Files**: `web/assets/base.css`, `base.js`, `favicon.svg`, `logo.svg`, `console.css`, `console.js`, `groupmonitor.css`, `groupmonitor.js`, `filemanager.css`, `filemanager.js` — flat, no subfolder. The module's own repo root is already the namespace; a per-module prefix inside `web/assets/` would be redundant.
- **Processing**: `.css`/`.js`/`.svg` are minified (whitespace/comments stripped, conservatively per format — JS deliberately does **not** strip `//` comments via regex, since a comment can't be reliably told apart from one inside a string/regex literal that way) and then gzip-compressed (`compresslevel=9`, `mtime=0` for reproducible bytes). `.jpg`/`.jpeg`/`.png` are gzip-compressed **only** — minifying an already-compressed binary format is meaningless, and the real-world gzip gain there is ~0%.
- **Identifiers**: flat, **no** module prefix — derived from the relative path under `web/assets/` (`/` and `.` → `_`, e.g. `base.css` → `base_css`). The module itself is already the namespace (one repo = one `web/assets/` root); an extra prefix would be redundant.
- **No duplicates, no exception**: if an identifier collides between two sources — including between a module and the project itself — the build aborts with an error (`raise SystemExit(1)`). No override, not even for the project.
- **No file without assets**: if no `web/assets/` folder exists anywhere in a build, `include/webassets.h` is neither generated nor left over from a previous build (removed if stale) — code referencing `WebAssets::*` then fails to compile, same as any other missing generated header (`versions.h`, `knxprod.h`).
- **Generated symbols** per file, in `namespace WebAssets`:
  ```cpp
  inline const uint8_t base_css_gz[] = { /* gzip bytes */ };
  inline const char* const base_css_mime = "text/css"; // from the generator's one MIME table
  ```
  **Deliberately no separate `_gz_len` constant** — the size is already known at the call site via `sizeof(WebAssets::base_css_gz)` (fixed-bound array), a manually-tracked extra number would be redundant. Important: the data is **not** NUL-terminated — `strlen()` must never be used here, gzip bytes are binary and virtually always contain a `0x00` somewhere in the stream; a `strlen()`-based length would truncate the content at a random position.
- **Every asset is global**: `addStylesheet()`/`addJavaScript()` put the tag on *every* page, there is no per-page registration. A page-specific stylesheet must therefore scope any `body`/`main`/element-selector rule to its own page (`main:has(#gm-table) { … }`), and a page-specific script must bail out early when its root element is missing (`if (!body) return;`) — otherwise it runs on the overview page too and, in the case of the monitor/console scripts, opens a WebSocket there and burns a connection slot.
- **Registration** (see [`Webserver.md`](Webserver.md)): `addRoute(WEB_GET, "/assets/base.css", Asset(WebAssets::base_css_mime, WebAssets::base_css_gz, sizeof(WebAssets::base_css_gz)));` — `Webserver::Asset()` builds the route handler, delegating to `WebResponse::sendAsset()` for the actual header/body logic (`Content-Type`, `Cache-Control`, `Content-Encoding: gzip`, static body — no copy).
- **Always gzip, no negotiation**: the server never checks `Accept-Encoding` — every client gets the gzip response. Not a concern for a browser-only audience; `curl /assets/base.css` without `--compressed` shows binary, not CSS.
- **`/assets/logo.svg`**, not `/assets/logo/black.svg` — flattened together with the rest of the migration.
