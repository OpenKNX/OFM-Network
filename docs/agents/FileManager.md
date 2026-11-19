# FileManager.md - Dateimanager

Teil von [`AGENTS.md`](../../AGENTS.md).
Verwandt: [`Webserver.md`](Webserver.md), [`WebserverPlatform.md`](WebserverPlatform.md)

## File Manager (`OPENKNX_WEBFS`)

`FileManager` (`src/OpenKNX/Network/Webserver/FileManager.{h,cpp}`) — browse/download/upload/delete/mkdir over HTTP. Untrusted input is escaped at every output site (`htmlEscape`, `jsStr`, `jsStrScriptBody`, `urlEncode`); `sanitizePath()` rejects any path containing `..`.

- **Upload is one POST per chunk** (`offset` query param), so no whole file is ever held in RAM. `offset == 0` on an existing path → `409`. After each chunk the file size is compared against `offset + length` — LittleFS writes only on `close()` and `write()` always returns the buffer size, so this comparison is the only reliable full-filesystem detection (→ `413`, partial file removed).
- **A missing drive or file answers `404`** on every route (`drvAvail()` / the `open()` in `handleDelete()`), `400` only for a malformed path, `409`/`413` for overwrite/no-space on upload, `500` only for a genuinely failed operation (write error, non-empty directory). `handleList()` deliberately does **not** probe the directory: a `LittleFS.exists()` pre-check would make every subdirectory depend on `exists()` answering `true` for directories, and a wrong answer there kills navigation entirely. A nonexistent directory therefore renders as an empty listing, as before.
- **The listing is paged and written into one fixed block.** `drvForEach(d, dir, fn)` is a template taking a callback; `handleList()` walks the directory twice (directories, then files) and appends the rows of the page `from=N` (`OPENKNX_FILEMANAGER_PAGE`, RP2040 25, ESP32 100) into a `BoundedText` (`OPENKNX_FILEMANAGER_BUF`, halved down to 4 KB). Each request re-opens the directory and skips N entries; no cursor survives the request. A row that does not fit is rolled back and the page ends there; a single row larger than an empty page is shown as "Name zu lang" and skipped. `from` past the end of a non-empty directory answers 302 to the last page; `from` of 500 or more counts as 0. Capped at `FM_MAX_ENTRIES` (250) visited entries per pass. On `fs=sd` the walk goes through `sd::fileStore.forEach` (OFM-SDCard: one `openNextFile()` pass, no snapshot of the directory) and leaves the store's directory cursor alone; only the rows shown allocate their strings.

### Multi-drive (`fs` query parameter)

Internal flash is always available; SD and external flash appear only if their define is set **and** the store reports `available()`. All routes accept `fs=int|sd|efc`, default and fallback `int` — old links without the parameter keep working. A tab bar is rendered above the listing as soon as more than the internal drive exists, and `currentFs` is injected into the page for the JS fetches.

- Dispatch is a flat set of `static` functions (`drvParse`, `drvId`, `drvAvail`, `drvExists`, `drvSize`, `drvRemove`, `drvMkdir`, `drvWriteChunk`, `drvSpace`, `drvForEach`), **not** a virtual interface — no vtable, and with both defines off every foreign branch is preprocessed away, leaving the internal-only code size unchanged.
- **External dependency**: `sd::fileStore` / `efc::fileStore` come from OFM-SDCard / OFM-ExternalFlash (`SdFileStore.h` / `EfcFileStore.h`). Neither is part of this module; with the defines unset nothing of it is compiled.
- **`handleDelete` takes a `dir=1` hint from the client** because the stores expose no `isDirectory()`. On the internal drive the hint is overridden by a server-side `File::isDirectory()` check.
- **The stores are singletons with one global cursor.** `handleDownload` keeps the store open across many `Webserver::loop()` ticks ([`WebserverPlatform.md`](WebserverPlatform.md)), so two concurrent downloads from the *same* store interfere. LittleFS is unaffected (one `File` per request).
- `fmtBytes()` formats B/KB/MB/GB; below the GB threshold it deliberately casts to 32 bit, since the u64 division would otherwise pull runtime helper routines into flash on RP2040.
