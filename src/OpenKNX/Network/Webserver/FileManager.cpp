#if defined(OPENKNX_WEBFS) && (defined(KNX_IP_WIFI) || defined(KNX_IP_LAN))

#include "OpenKNX/Network/Webserver/FileManager.h"
#include "OpenKNX/Network/Module.h"
#include "OpenKNX/Charset.hpp"
#include "OpenKNX/Network/Webserver/Webserver.h"
#include "webassets.h" // generiert von OGM-Common/scripts/pio/prepare.py aus web/assets/
#include <LittleFS.h>
#include <string>
#ifdef OPENKNX_SDCARD
#include "SdFileStore.h"
#endif
#ifdef OPENKNX_EXTFLASH
#include "EfcFileStore.h"
#endif

namespace OpenKNX
{
    namespace Network
    {

        bool FileManager::ensureFs()
        {
            static bool mounted = false;
            if (mounted) return true;
#ifdef ARDUINO_ARCH_ESP32
            mounted = LittleFS.begin(false);
            if (!mounted) mounted = LittleFS.begin(true);
#else
            mounted = LittleFS.begin();
#endif
            return mounted;
        }

        std::string FileManager::sanitizePath(const std::string& raw)
        {
            if (raw.empty()) return "";

            // Browser sends UTF-8, filesystem is ISO-8859-15; '?' fallback -> '_'.
            std::string path;
            OpenKNX::Charset::decodeUtf8(path, raw.data(), raw.size());
            for (char& c : path)
                if (c == '?') c = '_';

            if (path.empty() || path.find("..") != std::string::npos) return "";
            return (path[0] == '/') ? path : ("/" + path);
        }

        std::string FileManager::urlEncode(const std::string& s)
        {
            std::string result;
            result.reserve(s.size());
            for (unsigned char c : s)
            {
                if (c == '/' || c == '.' || c == '-' || c == '_' || std::isalnum(c))
                    result += (char)c;
                else
                {
                    char buf[4];
                    snprintf(buf, sizeof(buf), "%%%02X", c);
                    result += buf;
                }
            }
            return result;
        }

        static std::string jsStr(const std::string& s)
        {
            std::string r;
            r.reserve(s.size());
            for (char c : s)
            {
                if (c == '\'') r += "\\'";
                else if (c == '\\')
                    r += "\\\\";
                else
                    r += c;
            }
            return r;
        }

        // Wie jsStr(), aber zusätzlich für die Verwendung als Inhalt eines <script>-Blocks:
        // '<' wird JS-escaped, damit ein "</script>" im Wert (z.B. aus dir) den umgebenden
        // <script>-Tag nicht vorzeitig beendet. Der Browser dekodiert HTML-Entities in
        // Script-Rohtext nicht, htmlEscape() greift hier also nicht — das '<' muss auf
        // JS-Ebene neutralisiert werden.
        static std::string jsStrScriptBody(const std::string& s)
        {
            std::string r = jsStr(s);
            std::string out;
            out.reserve(r.size());
            for (char c : r)
            {
                if (c == '<') out += "\\x3C";
                else
                    out += c;
            }
            return out;
        }

        // Escaped für HTML-Textinhalt/Attribute. dir/displayName kommen aus dem
        // Query-String bzw. aus Dateinamen, die der Upload-Handler ungeprüft annimmt —
        // ohne dies bricht z.B. ein Verzeichnis- oder Dateiname mit '<' aus dem
        // umgebenden Markup bzw. aus dem <script>-Block in buildOverviewPage aus.
        static std::string htmlEscape(const std::string& s)
        {
            std::string r;
            r.reserve(s.size());
            for (char c : s)
            {
                switch (c)
                {
                    case '<': r += "&lt;"; break;
                    case '>': r += "&gt;"; break;
                    case '&': r += "&amp;"; break;
                    case '"': r += "&quot;"; break;
                    case '\'': r += "&#39;"; break;
                    default: r += c; break;
                }
            }
            return r;
        }

        static std::string absPath(const std::string& dir, const std::string& entryName)
        {
            if (!entryName.empty() && entryName[0] == '/') return entryName;
            return (dir == "/" ? "" : dir) + "/" + entryName;
        }

        enum
        {
            DINT = 0,
            DSD = 1,
            DEFC = 2
        };

        static int drvParse(WebRequest& req)
        {
            std::string s = req.getQueryParam("fs");
#ifdef OPENKNX_SDCARD
            if (s == "sd") return DSD;
#endif
#ifdef OPENKNX_EXTFLASH
            if (s == "efc") return DEFC;
#endif
            (void)s;
            return DINT;
        }

        static const char* drvId(int d)
        {
#ifdef OPENKNX_SDCARD
            if (d == DSD) return "sd";
#endif
#ifdef OPENKNX_EXTFLASH
            if (d == DEFC) return "efc";
#endif
            (void)d;
            return "int";
        }

        static bool drvAvail(int d)
        {
            if (d == DINT) return true;
#ifdef OPENKNX_SDCARD
            if (d == DSD) return sd::fileStore.available();
#endif
#ifdef OPENKNX_EXTFLASH
            if (d == DEFC) return efc::fileStore.available();
#endif
            return false;
        }

        static bool drvExists(int d, const char* p)
        {
            if (d == DINT) return LittleFS.exists(p);
#ifdef OPENKNX_SDCARD
            if (d == DSD) return sd::fileStore.exists(p);
#endif
#ifdef OPENKNX_EXTFLASH
            if (d == DEFC) return efc::fileStore.exists(p);
#endif
            return false;
        }

        static bool drvIsDir(int d, const char* p)
        {
#ifdef OPENKNX_SDCARD
            if (d == DSD) return sd::fileStore.isDir(p);
#endif
#ifdef OPENKNX_EXTFLASH
            if (d == DEFC) return efc::fileStore.isDir(p);
#endif
            (void)d;
            (void)p;
            return false;
        }

        static bool drvBusy(int d)
        {
#ifdef OPENKNX_SDCARD
            if (d == DSD) return sd::fileStore.busy();
#endif
#ifdef OPENKNX_EXTFLASH
            if (d == DEFC) return efc::fileStore.busy();
#endif
            (void)d;
            return false;
        }

        static uint32_t drvSize(int d, const char* p)
        {
            if (d == DINT)
            {
                File f = LittleFS.open(p, "r");
                uint32_t s = f ? (uint32_t)f.size() : 0;
                if (f) f.close();
                return s;
            }
#ifdef OPENKNX_SDCARD
            if (d == DSD)
            {
                int32_t s = sd::fileStore.open(p);
                sd::fileStore.close();
                return s < 0 ? 0 : (uint32_t)s;
            }
#endif
#ifdef OPENKNX_EXTFLASH
            if (d == DEFC)
            {
                int32_t s = efc::fileStore.open(p);
                efc::fileStore.close();
                return s < 0 ? 0 : (uint32_t)s;
            }
#endif
            return 0;
        }

        static bool drvRemove(int d, const char* p, bool isDir)
        {
            if (d == DINT) return isDir ? LittleFS.rmdir(p) : LittleFS.remove(p);
#ifdef OPENKNX_SDCARD
            if (d == DSD) return isDir ? sd::fileStore.rmdir(p) : sd::fileStore.remove(p);
#endif
#ifdef OPENKNX_EXTFLASH
            if (d == DEFC) return isDir ? efc::fileStore.rmdir(p) : efc::fileStore.remove(p);
#endif
            return false;
        }

        static bool drvMkdir(int d, const char* p)
        {
            if (d == DINT) return LittleFS.mkdir(p);
#ifdef OPENKNX_SDCARD
            if (d == DSD) return sd::fileStore.mkdir(p);
#endif
#ifdef OPENKNX_EXTFLASH
            if (d == DEFC) return efc::fileStore.mkdir(p);
#endif
            return false;
        }

        static bool drvWriteChunk(int d, const char* p, uint32_t offset, const uint8_t* data, size_t len)
        {
            if (d == DINT)
            {
                File f = LittleFS.open(p, offset == 0 ? "w" : "r+");
                if (!f) return false;
                if (offset && !f.seek(offset))
                {
                    f.close();
                    return false;
                }
                f.write(data, len);
                f.close();
                return true;
            }
#ifdef OPENKNX_SDCARD
            if (d == DSD)
            {
                if (!sd::fileStore.sinkOpen(p, offset)) return false;
                int w = sd::fileStore.sinkWrite(data, (uint16_t)len);
                sd::fileStore.sinkClose();
                return w == (int)len;
            }
#endif
#ifdef OPENKNX_EXTFLASH
            if (d == DEFC)
            {
                if (!efc::fileStore.sinkOpen(p, offset)) return false;
                int w = efc::fileStore.sinkWrite(data, (uint16_t)len);
                efc::fileStore.sinkClose();
                return w == (int)len;
            }
#endif
            return false;
        }

        static void drvSpace(int d, uint64_t& freeB, uint64_t& totalB)
        {
            freeB = 0;
            totalB = 0;
            if (d == DINT)
            {
#ifdef ARDUINO_ARCH_ESP32
                totalB = LittleFS.totalBytes();
                freeB = totalB - LittleFS.usedBytes();
#else
                FSInfo fi;
                LittleFS.info(fi);
                totalB = fi.totalBytes;
                freeB = fi.totalBytes - fi.usedBytes;
#endif
                return;
            }
#ifdef OPENKNX_SDCARD
            if (d == DSD)
            {
                totalB = sd::fileStore.totalBytes();
                freeB = sd::fileStore.freeBytes();
                return;
            }
#endif
#ifdef OPENKNX_EXTFLASH
            if (d == DEFC)
            {
                totalB = efc::fileStore.totalBytes();
                freeB = efc::fileStore.freeBytes();
                return;
            }
#endif
        }

        static const size_t FM_MAX_ENTRIES = 250;

        // Callback statt Ergebnisliste: das Listing wird direkt ins HTML gestreamt.
        // Ein Container mit 250 Einträgen wären ~10 KB Heap plus Realloc-Peak —
        // auf RP2040 neben lwIP und den Webserver-Slots zu viel. Dafür wird das
        // Verzeichnis zweimal durchlaufen (Ordner-Pass, Datei-Pass).
        // Rückgabe: true, wenn bei FM_MAX_ENTRIES abgebrochen wurde.
        template <typename Fn>
        static bool drvForEach(int d, const std::string& dir, Fn fn)
        {
            size_t seen = 0;
            if (d == DINT)
            {
                File root = LittleFS.open(dir.c_str(), "r");
                if (!root) return false;
                for (File e = root.openNextFile(); e; e = root.openNextFile())
                {
                    if (seen++ >= FM_MAX_ENTRIES)
                    {
                        e.close();
                        root.close();
                        return true;
                    }
                    std::string p = absPath(dir, e.name());
                    fn(p.substr(p.rfind('/') + 1), (uint32_t)e.size(), e.isDirectory());
                    e.close();
                }
                root.close();
                return false;
            }
#ifdef OPENKNX_SDCARD
            if (d == DSD)
            {
                if (!sd::fileStore.dirOpen(dir.c_str())) return false;
                char nm[64];
                uint32_t sz = 0;
                for (uint8_t t; (t = sd::fileStore.dirNext(nm, sizeof(nm), &sz)) != 0;)
                {
                    if (seen++ >= FM_MAX_ENTRIES)
                    {
                        sd::fileStore.dirClose();
                        return true;
                    }
                    std::string p = absPath(dir, nm);
                    fn(p.substr(p.rfind('/') + 1), sz, t == 2);
                }
                sd::fileStore.dirClose();
                return false;
            }
#endif
#ifdef OPENKNX_EXTFLASH
            if (d == DEFC)
            {
                if (!efc::fileStore.dirOpen(dir.c_str())) return false;
                char nm[64];
                uint32_t sz = 0;
                for (uint8_t t; (t = efc::fileStore.dirNext(nm, sizeof(nm), &sz)) != 0;)
                {
                    if (seen++ >= FM_MAX_ENTRIES)
                    {
                        efc::fileStore.dirClose();
                        return true;
                    }
                    std::string p = absPath(dir, nm);
                    fn(p.substr(p.rfind('/') + 1), sz, t == 2);
                }
                efc::fileStore.dirClose();
                return false;
            }
#endif
            return false;
        }

        // ── Streaming-Kontext für Download ───────────────────────────

        struct FmCtx
        {
            int drive;
            File f;
            uint32_t off;
        };

        static size_t fmRead(void* ctx, uint8_t* buf, size_t maxLen)
        {
            FmCtx* x = static_cast<FmCtx*>(ctx);
            if (x->drive == DINT) return x->f.read(buf, maxLen);
            size_t n = 0;
#ifdef OPENKNX_SDCARD
            if (x->drive == DSD)
                while (n < maxLen)
                {
                    uint8_t r = sd::fileStore.read(x->off, buf + n, (uint8_t)(maxLen - n > 255 ? 255 : maxLen - n));
                    if (!r) break;
                    n += r;
                    x->off += r;
                }
#endif
#ifdef OPENKNX_EXTFLASH
            if (x->drive == DEFC)
                while (n < maxLen)
                {
                    uint8_t r = efc::fileStore.read(x->off, buf + n, (uint8_t)(maxLen - n > 255 ? 255 : maxLen - n));
                    if (!r) break;
                    n += r;
                    x->off += r;
                }
#endif
            return n;
        }

        static void fmCleanup(void* ctx)
        {
            FmCtx* x = static_cast<FmCtx*>(ctx);
            if (x->drive == DINT)
            {
                if (x->f) x->f.close();
            }
#ifdef OPENKNX_SDCARD
            else if (x->drive == DSD)
                sd::fileStore.close();
#endif
#ifdef OPENKNX_EXTFLASH
            else if (x->drive == DEFC)
                efc::fileStore.close();
#endif
            delete x;
        }

        // ── setup ────────────────────────────────────────────────────

        void FileManager::setup()
        {
            // Like Webconsole and GroupMonitor: setup() runs again when the server start
            // fails and Module::loop() retries it, stacking menu, routes and assets.
            if (_initialized) return;
            _initialized = true;

            ensureFs();

            openknxNetwork.webserver.addMenuItem("Dateimanager", "/filemanager", 50);

            openknxNetwork.webserver.addRoute(WEB_GET, "/assets/filemanager.css",
                                              Webserver::Asset(WebAssets::filemanager_css_mime, WebAssets::filemanager_css_gz, sizeof(WebAssets::filemanager_css_gz)));
            openknxNetwork.webserver.addRoute(WEB_GET, "/assets/filemanager.js",
                                              Webserver::Asset(WebAssets::filemanager_js_mime, WebAssets::filemanager_js_gz, sizeof(WebAssets::filemanager_js_gz)));

            openknxNetwork.webserver.addStylesheet("/assets/filemanager.css");
            openknxNetwork.webserver.addJavaScript("/assets/filemanager.js");

            openknxNetwork.webserver.addRoute(WEB_GET, "/filemanager",
                                              [this](WebRequest& req, WebResponse& res) { handleList(req, res); });

            openknxNetwork.webserver.addRoute(WEB_GET, "/filemanager/download",
                                              [this](WebRequest& req, WebResponse& res) { handleDownload(req, res); });

            openknxNetwork.webserver.addRoute(WEB_POST, "/filemanager/upload",
                                              [this](WebRequest& req, WebResponse& res) { handleUpload(req, res); });

            openknxNetwork.webserver.addRoute(WEB_POST, "/filemanager/delete",
                                              [this](WebRequest& req, WebResponse& res) { handleDelete(req, res); });

            openknxNetwork.webserver.addRoute(WEB_POST, "/filemanager/mkdir",
                                              [this](WebRequest& req, WebResponse& res) { handleMkdir(req, res); });
        }

        // ── Verzeichnis-Listing ──────────────────────────────────────

        void FileManager::handleList(WebRequest& req, WebResponse& res)
        {
            int d = drvParse(req);
            if (!drvAvail(d) || (d == DINT && !ensureFs()))
            {
                res.setStatus(404);
                res.send("Laufwerk nicht verfügbar");
                return;
            }

            const char* fs = drvId(d);
            std::string q = std::string("fs=") + fs;
            std::string dir = sanitizePath(req.getQueryParam("dir"));
            if (dir.empty()) dir = "/";

            std::string html;
            html.reserve(2048);

            // Unter der GB-Grenze wird auf 32 Bit gerechnet — die u64-Division
            // zieht sonst auf RP2040 unnötig Laufzeit-Hilfsroutinen ins Flash.
            auto fmtBytes = [](uint64_t b) -> std::string {
                char buf[24];
                if (b >= 1024ULL * 1024 * 1024)
                    snprintf(buf, sizeof(buf), "%.1f GB", (double)b / 1073741824.0);
                else if (b >= 1024 * 1024)
                    snprintf(buf, sizeof(buf), "%.1f MB", (uint32_t)b / 1048576.0f);
                else if (b >= 1024)
                    snprintf(buf, sizeof(buf), "%.1f KB", (uint32_t)b / 1024.0f);
                else
                    snprintf(buf, sizeof(buf), "%lu B", (unsigned long)b);
                return buf;
            };

            // dir stays ISO-8859-15 for drvForEach below; dirUtf8 for display/links/JS.
            std::string dirUtf8;
            OpenKNX::Charset::encodeUtf8(dirUtf8, dir.data(), dir.size());

            html += "<div class='container'><h1>Dateiverwaltung";
            if (dir != "/")
            {
                html += " &ndash; ";
                html += htmlEscape(dirUtf8);
            }
            html += "</h1>";

            bool multi = false;
#ifdef OPENKNX_SDCARD
            if (sd::fileStore.available()) multi = true;
#endif
#ifdef OPENKNX_EXTFLASH
            if (efc::fileStore.available()) multi = true;
#endif
            if (multi)
            {
                auto tab = [&](const char* id, const char* label, bool active) {
                    html += "<a class='fm-tab";
                    if (active) html += " active";
                    html += "' href='/filemanager?fs=";
                    html += id;
                    html += "&dir=%2F'>";
                    html += label;
                    html += "</a>";
                };
                html += "<div class='fm-tabs'>";
                tab("int", "Intern", d == DINT);
#ifdef OPENKNX_SDCARD
                if (sd::fileStore.available()) tab("sd", "SD-Karte", d == DSD);
#endif
#ifdef OPENKNX_EXTFLASH
                if (efc::fileStore.available()) tab("efc", "Ext. Flash", d == DEFC);
#endif
                html += "</div>";
            }

            html += "<h2>Inhalt</h2>";
            html += "<table><thead><tr>"
                    "<th>Name</th><th class='right'>Gr&ouml;&szlig;e</th><th class='right'>Aktionen</th>"
                    "</tr></thead><tbody>";

            if (dir != "/")
            {
                std::string parent = dir.substr(0, dir.rfind('/'));
                if (parent.empty()) parent = "/";
                std::string parentUtf8;
                OpenKNX::Charset::encodeUtf8(parentUtf8, parent.data(), parent.size());
                html += "<tr><td colspan='3'><a href='/filemanager?";
                html += q;
                html += "&dir=";
                html += urlEncode(parentUtf8);
                html += "'>..</a></td></tr>";
            }

            bool hasEntries = false;
            bool wantDirs = true;

            auto emit = [&](const std::string& name, uint32_t size, bool isDir) {
                if (isDir != wantDirs) return;
                hasEntries = true;
                // name is ISO-8859-15 (filesystem); convert for display/links.
                std::string nameUtf8;
                OpenKNX::Charset::encodeUtf8(nameUtf8, name.data(), name.size());
                std::string path = absPath(dirUtf8, nameUtf8);
                if (isDir)
                {
                    html += "<tr><td><a href='/filemanager?";
                    html += q;
                    html += "&dir=";
                    html += urlEncode(path);
                    html += "'>";
                    html += htmlEscape(nameUtf8);
                    html += "/</a></td><td class='right'></td><td class='right'>";
                    html += "<a href='#' onclick=\"delDir('";
                    html += htmlEscape(jsStr(path));
                    html += "'); return false;\">L&ouml;schen</a>";
                    html += "</td></tr>";
                    return;
                }
                html += "<tr><td>";
                html += htmlEscape(nameUtf8);
                html += "</td><td class='right'>";
                html += fmtBytes(size);
                html += "</td><td class='right'><a href='/filemanager/download?";
                html += q;
                html += "&path=";
                html += urlEncode(path);
                html += "'>Download</a> | <a href='#' onclick=\"delFile('";
                html += htmlEscape(jsStr(path));
                html += "'); return false;\">L&ouml;schen</a>";
                html += "</td></tr>";
            };

            bool truncated = drvForEach(d, dir, emit);
            wantDirs = false;
            truncated |= drvForEach(d, dir, emit);

            if (!hasEntries)
                html += "<tr><td colspan='3'><em>Leer</em></td></tr>";
            if (truncated)
                html += "<tr><td colspan='3'><em>Liste gek&uuml;rzt (max 250 Eintr&auml;ge)</em></td></tr>";

            html += "</tbody></table>";

            html += "<h2>Ordner anlegen</h2>";
            html += "<div class='fm-row'>"
                    "<input type='text' id='nd' placeholder='Ordnername'>"
                    "<button onclick='mkDir()'>Anlegen</button>"
                    "</div>";

            html += "<h2>Datei hochladen</h2>";

            {
                uint64_t total = 0, freeBytes = 0;
                drvSpace(d, freeBytes, total);
                if (total > 0)
                {
                    html += "<p class='fm-storage'>Speicher: <strong>";
                    html += fmtBytes(freeBytes);
                    html += "</strong> frei von ";
                    html += fmtBytes(total);
                    html += "</p>";
                }
            }

            html += "<div class='fm-row'>"
                    "<input type='file' id='fi' class='fm-file-hidden'>"
                    "<div class='fm-file-ctrl' id='fm-picker'>"
                    "<span class='fm-file-btn' id='fi-btn' role='button' tabindex='0'>Durchsuchen &hellip;</span>"
                    "<span class='fm-file-name' id='fi-name'>Keine Datei ausgew&auml;hlt.</span>"
                    "</div>"
                    "<progress id='bar' class='fm-hidden' value='0' max='100'></progress>"
                    "<button onclick='uploadFile()'>Hochladen</button>"
                    "</div>"
                    "<div class='fm-upload-status'><span id='st'></span></div>";

            html += "<script>const currentDir='";
            html += jsStrScriptBody(dirUtf8);
            html += "';const currentFs='";
            html += fs;
            html += "';</script></div>";

            res.setLayout(true);
            res.setActiveMenu("/filemanager");
            res.send(html.c_str());
        }

        // ── Download (Streaming) ─────────────────────────────────────

        void FileManager::handleDownload(WebRequest& req, WebResponse& res)
        {
            int d = drvParse(req);
            std::string path = sanitizePath(req.getQueryParam("path"));
            if (path.empty())
            {
                res.setStatus(400);
                res.send("Bad path");
                return;
            }
            if (!drvAvail(d) || (d == DINT && !ensureFs()))
            {
                res.setStatus(404);
                res.send("Laufwerk nicht verfügbar");
                return;
            }
            if (drvBusy(d))
            {
                res.setStatus(503);
                res.send("Laufwerk beschäftigt");
                return;
            }

            FmCtx* ctx = new FmCtx{d, File(), 0};
            int32_t size = -1;
            if (d == DINT)
            {
                ctx->f = LittleFS.open(path.c_str(), "r");
                if (ctx->f) size = (int32_t)ctx->f.size();
            }
#ifdef OPENKNX_SDCARD
            else if (d == DSD)
                size = sd::fileStore.open(path.c_str());
#endif
#ifdef OPENKNX_EXTFLASH
            else if (d == DEFC)
                size = efc::fileStore.open(path.c_str());
#endif
            if (size < 0)
            {
                delete ctx;
                res.setStatus(404);
                res.send("Datei nicht gefunden");
                return;
            }

            std::string name = path.substr(path.rfind('/') + 1);
            std::string cd = "attachment; filename=\"" + name + "\"";
            res.setContentType("application/octet-stream");
            res.setHeader("Content-Disposition", cd.c_str());
            res.sendStream((size_t)size, fmRead, fmCleanup, ctx);
        }

        // ── Upload (chunked: jeder Chunk ist ein separater POST) ─────

        void FileManager::handleUpload(WebRequest& req, WebResponse& res)
        {
            int d = drvParse(req);
            std::string path = sanitizePath(req.getQueryParam("path"));
            if (path.empty() || req.bodyLength() == 0)
            {
                res.setStatus(400);
                res.send("Bad request");
                return;
            }
            if (!drvAvail(d) || (d == DINT && !ensureFs()))
            {
                res.setStatus(404);
                res.send("Laufwerk nicht verfügbar");
                return;
            }
            if (drvBusy(d))
            {
                res.setStatus(503);
                res.send("Laufwerk beschäftigt");
                return;
            }

            std::string offsetStr = req.getQueryParam("offset");
            uint32_t offset = offsetStr.empty() ? 0 : (uint32_t)atol(offsetStr.c_str());

            if (offset == 0 && drvExists(d, path.c_str()))
            {
                res.setStatus(409);
                res.send("Datei existiert bereits");
                return;
            }

            size_t needed = req.bodyLength();
            if (!drvWriteChunk(d, path.c_str(), offset, req.body(), needed))
            {
                res.setStatus(500);
                res.send("Schreibfehler");
                return;
            }

            // LittleFS schreibt erst beim close() auf Flash, write() meldet immer die
            // Puffergröße. Nur die Dateigröße danach erkennt ein volles Dateisystem.
            if (drvSize(d, path.c_str()) != offset + needed)
            {
                drvRemove(d, path.c_str(), false);
                res.setStatus(413);
                res.send("Dateisystem voll");
                return;
            }

            res.setContentType("text/plain");
            res.send("OK");
        }

        // ── Delete (Datei oder Ordner) ───────────────────────────────

        void FileManager::handleDelete(WebRequest& req, WebResponse& res)
        {
            int d = drvParse(req);
            std::string path = sanitizePath(req.getQueryParam("path"));
            if (path.empty())
            {
                res.setStatus(400);
                res.send("Bad path");
                return;
            }
            if (!drvAvail(d) || (d == DINT && !ensureFs()))
            {
                res.setStatus(404);
                res.send("Laufwerk nicht verfügbar");
                return;
            }

            // Typ wird serverseitig bestimmt: LittleFS über open(), die SD/EFC-Stores über ihr isDir().
            // Der dir=-Hinweis des Clients wird nicht mehr benutzt.
            bool isDir = false;
            if (d == DINT)
            {
                File f = LittleFS.open(path.c_str(), "r");
                if (!f)
                {
                    res.setStatus(404);
                    res.send("Nicht gefunden");
                    return;
                }
                isDir = f.isDirectory();
                f.close();
            }
            else
            {
                if (!drvExists(d, path.c_str()))
                {
                    res.setStatus(404);
                    res.send("Nicht gefunden");
                    return;
                }
                isDir = drvIsDir(d, path.c_str());
            }

            res.setContentType("text/plain");
            if (drvRemove(d, path.c_str(), isDir))
                res.send("OK");
            else
            {
                res.setStatus(500);
                res.send(isDir ? "Ordner nicht leer oder Fehler" : "Delete failed");
            }
        }

        // ── Mkdir ────────────────────────────────────────────────────

        void FileManager::handleMkdir(WebRequest& req, WebResponse& res)
        {
            int d = drvParse(req);
            std::string path = sanitizePath(req.getQueryParam("path"));
            if (path.empty())
            {
                res.setStatus(400);
                res.send("Bad path");
                return;
            }
            if (!drvAvail(d) || (d == DINT && !ensureFs()))
            {
                res.setStatus(404);
                res.send("Laufwerk nicht verfügbar");
                return;
            }

            res.setContentType("text/plain");
            if (drvMkdir(d, path.c_str()))
                res.send("OK");
            else
            {
                res.setStatus(500);
                res.send("Mkdir failed");
            }
        }

    } // namespace Network
} // namespace OpenKNX

#endif // OPENKNX_WEBFS
