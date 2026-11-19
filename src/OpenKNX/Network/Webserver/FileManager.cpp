#if defined(OPENKNX_WEBFS) && (defined(KNX_IP_WIFI) || defined(KNX_IP_LAN))

#include "OpenKNX/Network/Webserver/FileManager.h"
#include "OpenKNX/Network/Module.h"
#include "OpenKNX/Charset.hpp"
#include "OpenKNX/Network/Webserver/Webserver.h"
#include "OpenKNX/Network/Webserver/BoundedText.h"
#include <cstdlib>
#include <cstring>
#include "webassets.h" // generiert von OGM-Common/scripts/pio/prepare.py aus web/assets/
#include <LittleFS.h>
#ifdef ARDUINO_ARCH_ESP32
#include <sys/stat.h>
#endif
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
            // Mounted -- and formatted if need be -- by OGM-Common's Hardware::initFilesystem();
            // this only confirms it and never formats on its own.
            static bool mounted = false;
            if (mounted) return true;
            mounted = LittleFS.begin();
            return mounted;
        }

        std::string FileManager::sanitizePath(const std::string& raw, bool utf8Fs)
        {
            if (raw.empty()) return "";

            // Browser sends UTF-8; LittleFS names are ISO-8859-15 ('?' fallback -> '_'), SdFat with UTF-8 names keeps them.
            std::string path;
            if (utf8Fs)
                path = raw;
            else
                OpenKNX::Charset::decodeUtf8(path, raw.data(), raw.size());
            for (char& c : path)
                if (c == '?') c = '_';

            if (path.empty() || path.find("..") != std::string::npos) return "";
            return (path[0] == '/') ? path : ("/" + path);
        }

        // SD names are UTF-8 when SdFat is built with UTF-8 long names; LittleFS and efc keep ISO-8859-15
        static bool utf8Names(int d)
        {
#if defined(USE_UTF8_LONG_NAMES) && USE_UTF8_LONG_NAMES
            return d == 1; // DSD
#else
            (void)d;
            return false;
#endif
        }

        // a file system name for display, links and scripts (UTF-8)
        static void nameToUtf8(int d, std::string& out, const char* p, size_t n)
        {
            if (utf8Names(d))
                out.assign(p, n);
            else
                OpenKNX::Charset::encodeUtf8(out, p, n);
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
        static_assert(DSD == 1, "utf8Names() compares with the value of DSD");

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

        // Public face of the drive routing above -- see FileManager.h.
        int FileManager::driveFromPath(const std::string& path, std::string& rel)
        {
            if (path.compare(0, 3, "sd/") == 0)  { rel = path.substr(2); return DSD; }
            if (path.compare(0, 4, "efc/") == 0) { rel = path.substr(3); return DEFC; }
            rel = (path.empty() || path[0] != '/') ? "/" + path : path;
            return DINT;
        }

        bool FileManager::writeChunk(int drive, const char* path, uint32_t offset,
                                     const uint8_t* data, size_t len)
        {
            return drvWriteChunk(drive, path, offset, data, len);
        }

        static File streamFile;

        bool FileManager::streamChunk(int drive, const char* path, uint32_t offset,
                                      const uint8_t* data, size_t len)
        {
            if (drive != DINT)
                return drvWriteChunk(drive, path, offset, data, len);
            if (offset == 0)
            {
                streamFile.close();
                streamFile = LittleFS.open(path, "w");
            }
            return streamFile && streamFile.write(data, len) == len;
        }

        void FileManager::streamEnd()
        {
            streamFile.close();
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
        static constexpr uint64_t FM_SIZE_UNKNOWN = UINT64_MAX; // ESP32 LittleFS walk: the size is read for a shown row only

        // Callback statt Ergebnisliste: das Listing wird direkt ins HTML gestreamt.
        // Ein Container mit 250 Einträgen wären ~10 KB Heap plus Realloc-Peak —
        // auf RP2040 neben lwIP und den Webserver-Slots zu viel. Dafür wird das
        // Verzeichnis zweimal durchlaufen (Ordner-Pass, Datei-Pass).
        // fn gets the entry's name without its directory.
        // Rückgabe: true, wenn bei FM_MAX_ENTRIES abgebrochen wurde.
        template <typename Fn>
        static bool drvForEach(int d, const std::string& dir, Fn fn)
        {
            size_t seen = 0;
            auto base = [](const char* p) {
                const char* sl = strrchr(p, '/');
                return sl ? sl + 1 : p;
            };
            if (d == DINT)
            {
                // No entry is opened: openNextFile() opens and stats each one, about 25 ms per entry on an ESP32 (measured).
#ifdef ARDUINO_ARCH_ESP32
                File root = LittleFS.open(dir.c_str(), "r");
                if (!root) return false;
                bool isDir = false;
                for (String p = root.getNextFileName(&isDir); p.length() || !p.c_str(); p = root.getNextFileName(&isDir))
                {
                    if (!p.c_str() || seen++ >= FM_MAX_ENTRIES) // no memory for the name: the list ends here, marked as cut
                    {
                        root.close();
                        return true;
                    }
                    const char* nm = base(p.c_str());
                    if (!*nm) // the name did not fit into the String: cut, not an empty row
                    {
                        root.close();
                        return true;
                    }
                    fn(nm, isDir ? 0 : FM_SIZE_UNKNOWN, isDir); // readdir has no size: emit reads it for a shown row
                }
                root.close();
                return false;
#else
                FSStat st;
                if (!LittleFS.stat(dir.c_str(), &st) || !st.isDir) return false; // openDir() of a file lists its prefix matches
                Dir dd = LittleFS.openDir(dir.c_str());
                while (dd.next())
                {
                    if (seen++ >= FM_MAX_ENTRIES) return true;
                    const String nm = dd.fileName();
                    if (!nm.c_str()) return true; // the name got no memory: the list ends here, marked as cut
                    fn(base(nm.c_str()), dd.isDirectory() ? 0 : (uint64_t)dd.fileSize(), dd.isDirectory());
                }
                return false;
#endif
            }
#if defined(OPENKNX_SDCARD) || defined(OPENKNX_EXTFLASH)
            // sd/efc: one pass with its own handle, no snapshot on the heap; the store cursor of a running FTC listing stays untouched
            struct Walk
            {
                Fn* fn;
                size_t seen;
                bool cut;
            } w{&fn, seen, false};
            bool (*cb)(void*, const char*, bool, uint64_t) = [](void* c, const char* n, bool isDir, uint64_t sz) -> bool {
                Walk* w = static_cast<Walk*>(c);
                if (w->seen++ >= FM_MAX_ENTRIES)
                {
                    w->cut = true;
                    return false;
                }
                const char* sl = strrchr(n, '/');
                (*w->fn)(sl ? sl + 1 : n, sz, isDir);
                return true;
            };
    #ifdef OPENKNX_SDCARD
            if (d == DSD)
            {
                sd::fileStore.forEach(dir.c_str(), cb, &w, &w.cut); // also cut: the card went away during the listing
                return w.cut;
            }
    #endif
    #ifdef OPENKNX_EXTFLASH
            if (d == DEFC)
            {
                efc::fileStore.forEach(dir.c_str(), cb, &w);
                return w.cut;
            }
    #endif
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
#ifdef OPENKNX_WEBCLIENT_TEST
            openknxNetwork.webserver.addLoopRoute(WEB_POST, "/filemanager/fetch",
                                              [this](WebRequest& q, WebResponse& r) { handleFetch(q, r); });
            openknxNetwork.webserver.addLoopRoute(WEB_GET, "/filemanager/fetch/status",
                                              [this](WebRequest& q, WebResponse& r) { handleFetchStatus(q, r); });
#endif
        }

        // ── Verzeichnis-Listing ──────────────────────────────────────

        // Entries per page and the largest page buffer. A row that no longer fits ends the page early; "Weiter" goes on from it.
#ifndef OPENKNX_FILEMANAGER_PAGE
    #ifdef ARDUINO_ARCH_RP2040
        #define OPENKNX_FILEMANAGER_PAGE 25
    #else
        #define OPENKNX_FILEMANAGER_PAGE 100
    #endif
#endif
#ifndef OPENKNX_FILEMANAGER_BUF
    #ifdef ARDUINO_ARCH_RP2040
        #define OPENKNX_FILEMANAGER_BUF 8192
    #else
        #define OPENKNX_FILEMANAGER_BUF 32768
    #endif
#endif

        // Unter der GB-Grenze wird auf 32 Bit gerechnet — die u64-Division
        // zieht sonst auf RP2040 unnötig Laufzeit-Hilfsroutinen ins Flash.
        static void fmtBytes(char* buf, size_t len, uint64_t b)
        {
            if (b >= 1024ULL * 1024 * 1024)
                snprintf(buf, len, "%.1f GB", (double)b / 1073741824.0);
            else if (b >= 1024 * 1024)
                snprintf(buf, len, "%.1f MB", (uint32_t)b / 1048576.0f);
            else if (b >= 1024)
                snprintf(buf, len, "%.1f KB", (uint32_t)b / 1024.0f);
            else
                snprintf(buf, len, "%lu B", (unsigned long)b);
        }

        // htmlEscape() straight into the page
        static void addHtml(BoundedText& t, const std::string& s)
        {
            for (char c : s)
            {
                switch (c)
                {
                    case '<': t += "&lt;"; break;
                    case '>': t += "&gt;"; break;
                    case '&': t += "&amp;"; break;
                    case '"': t += "&quot;"; break;
                    case '\'': t += "&#39;"; break;
                    default: t += c; break;
                }
            }
        }

        // an HTML-escaped JS string (\' and \\ escaped) straight into the page
        static void addJsHtml(BoundedText& t, const std::string& s)
        {
            for (char c : s)
            {
                switch (c)
                {
                    case '\'': t += "\\&#39;"; break;
                    case '\\': t += "\\\\"; break;
                    case '<': t += "&lt;"; break;
                    case '>': t += "&gt;"; break;
                    case '&': t += "&amp;"; break;
                    case '"': t += "&quot;"; break;
                    default: t += c; break;
                }
            }
        }

        // URL-encoded straight into the page
        static void addUrl(BoundedText& t, const std::string& s)
        {
            for (unsigned char c : s)
            {
                if (c == '/' || c == '.' || c == '-' || c == '_' || std::isalnum(c))
                    t += (char)c;
                else
                    t.addf("%%%02X", c);
            }
        }

        // A JS string inside a <script> block: \' and \\ escaped, '<' as \x3C so no "</script>" in the value ends the block
        static void addJsScript(BoundedText& t, const std::string& s)
        {
            for (char c : s)
            {
                if (c == '\'' || c == '\\') t += '\\';
                if (c == '<') t += "\\x3C";
                else t += c;
            }
        }

        // Length of the directory after the rows: twice in a link (addUrl), once in the script (addJsScript)
        static size_t tailDirLen(const std::string& s)
        {
            size_t n = 0;
            for (unsigned char c : s)
                n += 2 * ((c == '/' || c == '.' || c == '-' || c == '_' || std::isalnum(c)) ? 1 : 3) + (c == '<' ? 4 : (c == '\'' || c == '\\') ? 2 : 1);
            return n;
        }

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
            std::string dir = sanitizePath(req.getQueryParam("dir"), utf8Names(d));
            if (dir.empty()) dir = "/";
            size_t from = 0;
            {
                const std::string f = req.getQueryParam("from");
                char* end = nullptr;
                const unsigned long v = strtoul(f.c_str(), &end, 10);
                if (!f.empty() && end != f.c_str() && *end == '\0' && v < FM_MAX_ENTRIES * 2) from = (size_t)v;
            }

            // a smaller block when the heap has no large one: the page then ends earlier and "Weiter" goes on
            BoundedText html(OPENKNX_FILEMANAGER_BUF, 4096);
            if (html.failed())
            {
                res.sendOutOfMemory();
                return;
            }
            char num[24];

            // dir stays ISO-8859-15 for drvForEach below; dirUtf8 for display/links/JS.
            std::string dirUtf8;
            nameToUtf8(d, dirUtf8, dir.data(), dir.size());
            // What follows the rows: at most 1.5 KB of text (literals, numbers, sizes) plus the directory in two links and
            // the script, and room for the placeholder row of a name too long for the page.
            const size_t tailRoom = 1664 + tailDirLen(dirUtf8);

            html += "<div class='container'><h1>Dateiverwaltung";
            if (dir != "/")
            {
                html += " &ndash; ";
                addHtml(html, dirUtf8);
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
                nameToUtf8(d, parentUtf8, parent.data(), parent.size());
                html += "<tr><td colspan='3'><a href='/filemanager?fs=";
                html += fs;
                html += "&dir=";
                addUrl(html, parentUtf8);
                html += "'>..</a></td></tr>";
            }
            if (html.cut() || html.room() < tailRoom)
            {
                logError("Webserver", "file list: no room for the page");
                res.sendOutOfMemory();
                return;
            }

            // Every request walks the directory again and skips the first `from` entries, folders before files; no
            // cursor is kept across requests.
            size_t idx = 0;   // entries walked, folders then files
            size_t shown = 0; // rows on this page
            bool full = false;
            bool wantDirs = true;

            auto emit = [&](const char* name, uint64_t size, bool isDir) { // 64 bit: exFAT files above 4 GiB
                if (isDir != wantDirs) return;
                const size_t i = idx++;
                if (i < from || full) return;
                if (shown >= OPENKNX_FILEMANAGER_PAGE)
                {
                    full = true;
                    return;
                }
#ifdef ARDUINO_ARCH_ESP32
                if (!isDir && size == FM_SIZE_UNKNOWN)
                {
                    std::string raw = dir;
                    if (raw.empty() || raw.back() != '/') raw += '/';
                    raw += name;
                    std::string full = LittleFS.mountpoint() ? LittleFS.mountpoint() : "";
                    full += raw;
                    struct stat st;
                    size = ::stat(full.c_str(), &st) == 0 ? (uint64_t)st.st_size : 0; // stat only, no FILE and no file cache

                }
#endif
                const size_t mark = html.size();
                // name is ISO-8859-15 (filesystem); convert for display/links.
                std::string nameUtf8;
                nameToUtf8(d, nameUtf8, name, strlen(name));
                std::string path = absPath(dirUtf8, nameUtf8);
                if (isDir)
                {
                    html += "<tr><td><a href='/filemanager?fs=";
                    html += fs;
                    html += "&dir=";
                    addUrl(html, path);
                    html += "'>";
                    addHtml(html, nameUtf8);
                    html += "/</a></td><td class='right'></td><td class='right'>";
                    html += "<a href='#' onclick=\"delDir('";
                    addJsHtml(html, path);
                    html += "'); return false;\">L&ouml;schen</a>";
                    html += "</td></tr>";
                }
                else
                {
                    html += "<tr><td>";
                    addHtml(html, nameUtf8);
                    html += "</td><td class='right'>";
                    fmtBytes(num, sizeof(num), size);
                    html += num;
                    html += "</td><td class='right'><a href='/filemanager/download?fs=";
                    html += fs;
                    html += "&path=";
                    addUrl(html, path);
                    html += "'>Download</a> | <a href='#' onclick=\"delFile('";
                    addJsHtml(html, path);
                    html += "'); return false;\">L&ouml;schen</a>";
                    html += "</td></tr>";
                }
                if (!html.cut() && html.room() >= tailRoom)
                {
                    shown++;
                    return;
                }
                html.rollback(mark);
                if (shown == 0)
                {
                    // a single row larger than the page: named, skipped, so "Weiter" still moves on
                    html += "<tr><td colspan='3'><em>Name zu lang f&uuml;r die Anzeige</em></td></tr>";
                    shown = 1;
                }
                full = true;
            };

            bool truncated = drvForEach(d, dir, emit);
            wantDirs = false;
            truncated |= drvForEach(d, dir, emit);
            const size_t total = idx;

            if (total > 0 && from >= total)
            {
                // past the end, e.g. after deleting the last entry of the last page: to the last page
                html.rollback(0); // the block carries the target instead of the page
                html.addf("/filemanager?fs=%s&dir=", fs);
                addUrl(html, dirUtf8);
                html.addf("&from=%u", (unsigned)((total - 1) / OPENKNX_FILEMANAGER_PAGE * OPENKNX_FILEMANAGER_PAGE));
                res.setStatus(302);
                res.setHeader("Location", html.c_str());
                res.sendStatic("");
                return;
            }

            if (total == 0 && !truncated)
                html += "<tr><td colspan='3'><em>Leer</em></td></tr>";
            if (truncated)
                html += "<tr><td colspan='3'><em>Liste gek&uuml;rzt: h&ouml;chstens 250 Eintr&auml;ge</em></td></tr>";

            html += "</tbody></table>";

            const size_t next = from + shown;
            if (total > 0 && (from > 0 || next < total))
            {
                html += "<div class='fm-pages'>";
                if (from > 0)
                {
                    html += "<a href='/filemanager?fs=";
                    html += fs;
                    html += "&dir=";
                    addUrl(html, dirUtf8);
                    html.addf("&from=%u'>Zur&uuml;ck</a>",
                              (unsigned)(from > OPENKNX_FILEMANAGER_PAGE ? from - OPENKNX_FILEMANAGER_PAGE : 0));
                }
                html.addf("<span>Eintr&auml;ge %u&ndash;%u von %u</span>", (unsigned)(from + 1), (unsigned)next,
                          (unsigned)total);
                if (next < total)
                {
                    html += "<a href='/filemanager?fs=";
                    html += fs;
                    html += "&dir=";
                    addUrl(html, dirUtf8);
                    html.addf("&from=%u'>Weiter</a>", (unsigned)next);
                }
                html += "</div>";
            }

            html += "<h2>Ordner anlegen</h2>";
            html += "<div class='fm-row'>"
                    "<input type='text' id='nd' placeholder='Ordnername'>"
                    "<button onclick='mkDir()'>Anlegen</button>"
                    "</div>";

#ifdef OPENKNX_WEBCLIENT_TEST
            // The device fetches the URL itself -- the file never travels through the browser.
            html += "<h2>Von URL laden</h2>";
            html += "<div class='fm-row'>"
                    "<input type='text' id='fu' placeholder='https://...'>"
                    "<button id='fb' onclick='fetchUrl()'>Laden</button>"
                    "</div>"
                    "<div class='fm-upload-status'><span id='fst'></span></div>";
#endif

            html += "<h2>Datei hochladen</h2>";

            {
                uint64_t total = 0, freeBytes = 0;
                drvSpace(d, freeBytes, total);
                if (total > 0)
                {
                    html += "<p class='fm-storage'>Speicher: <strong>";
                    fmtBytes(num, sizeof(num), freeBytes);
                    html += num;
                    html += "</strong> frei von ";
                    fmtBytes(num, sizeof(num), total);
                    html += num;
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
            addJsScript(html, dirUtf8);
            html += "';const currentFs='";
            html += fs;
            html += "';</script></div>";

            if (html.cut())
            {
                logError("Webserver", "file list: page buffer too small");
                res.sendOutOfMemory();
                return;
            }
            res.setLayout(true);
            res.setActiveMenu("/filemanager");
            int len = 0;
            uint8_t* body = html.take(len);
            res.sendOwned(body, len);
        }

        // ── Download (Streaming) ─────────────────────────────────────

        void FileManager::handleDownload(WebRequest& req, WebResponse& res)
        {
            int d = drvParse(req);
            std::string path = sanitizePath(req.getQueryParam("path"), utf8Names(d));
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
            // closes the file if an allocation below throws before sendStream() takes the context
            struct CtxGuard { FmCtx* c; ~CtxGuard() { if (c) fmCleanup(c); } } guard{ctx};
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
                guard.c = nullptr;
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
            guard.c = nullptr; // the response owns the context now
        }

        // ── Upload (chunked: jeder Chunk ist ein separater POST) ─────

        void FileManager::handleUpload(WebRequest& req, WebResponse& res)
        {
            int d = drvParse(req);
            std::string path = sanitizePath(req.getQueryParam("path"), utf8Names(d));
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
            std::string path = sanitizePath(req.getQueryParam("path"), utf8Names(d));
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

#ifdef OPENKNX_WEBCLIENT_TEST
        // "Load from URL" in the file manager. Arms the download in the loop task (addLoopRoute) and answers at once;
        // the transfer is not waited for. The browser polls /fetch/status.
        void FileManager::handleFetch(WebRequest& req, WebResponse& res)
        {
            int d = drvParse(req);
            // The URL travels in the BODY: a real https link plus the encoded path overruns the
            // request-line buffer of the RP2040 server (128 B) and the request dies as 414.
            std::string url;
            if (req.body() && req.bodyLength()) url.assign((const char*)req.body(), req.bodyLength());
            std::string name = sanitizePath(req.getQueryParam("path"), utf8Names(d));
            if (url.size() > FETCH_URL_MAX)
            {
                res.setStatus(400);
                res.send("URL zu lang");
                return;
            }
            if (url.empty() || name.empty())
            {
                res.setStatus(400);
                res.send("URL und Ziel-Pfad erforderlich");
                return;
            }
            if (!drvAvail(d) || (d == DINT && !ensureFs()))
            {
                res.setStatus(404);
                res.send("Laufwerk nicht verfügbar");
                return;
            }
            // Hand the drive back as a prefix so the module resolves it exactly like the console does.
            std::string raw = (d == DSD) ? "sd" + name : (d == DEFC) ? "efc" + name : name;
            res.setContentType("text/plain");
            if (openknxNetwork.httpFetchStart(url, raw)) res.send("OK");
            else
            {
                res.setStatus(409);
                res.send(openknxNetwork.httpFetchRefusal());
            }
        }

        void FileManager::handleFetchStatus(WebRequest& req, WebResponse& res)
        {
            const auto &f = openknxNetwork.httpFetch();
            const char *st = f.state == Module::HttpFetch::State::RUNNING ? "running"
                           : f.state == Module::HttpFetch::State::DONE    ? "done"
                           : f.state == Module::HttpFetch::State::FAILED  ? "failed" : "idle";
            char buf[256];
            snprintf(buf, sizeof(buf),
                     "{\"state\":\"%s\",\"bytes\":%u,\"status\":%u,\"hops\":%u,\"path\":\"%s\",\"error\":\"%s\"}",
                     st, (unsigned)f.bytes, (unsigned)f.status, (unsigned)f.hops,
                     f.path.c_str(), f.error.c_str());
            res.setContentType("application/json");
            res.send(buf);
        }
#endif

        void FileManager::handleMkdir(WebRequest& req, WebResponse& res)
        {
            int d = drvParse(req);
            std::string path = sanitizePath(req.getQueryParam("path"), utf8Names(d));
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
