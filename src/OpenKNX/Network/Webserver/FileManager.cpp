#if defined(OPENKNX_WEBFS) && (defined(KNX_IP_WIFI) || defined(KNX_IP_LAN))

#include "OpenKNX/Network/Webserver/FileManager.h"
#include "OpenKNX/Network/Module.h"
#include "OpenKNX/Network/Webserver/Webserver.h"
#include "webassets.h" // generiert von OGM-Common/scripts/pio/prepare.py aus web/assets/
#include <LittleFS.h>
#include <string>

namespace OpenKNX
{
    namespace Network
    {

        // filemanager.css/filemanager.js liegen in web/assets/ und werden dort
        // minifiziert + gzip-komprimiert in webassets.h eingebettet.

        // ── LittleFS Streaming-Kontext ────────────────────────────────────────

        struct FmFileCtx
        {
            File f;
        };

        static size_t fmFileRead(void* ctx, uint8_t* buf, size_t maxLen)
        {
            return static_cast<FmFileCtx*>(ctx)->f.read(buf, maxLen);
        }

        static void fmFileCleanup(void* ctx)
        {
            FmFileCtx* fc = static_cast<FmFileCtx*>(ctx);
            if (fc->f) fc->f.close();
            delete fc;
        }

        // ── Hilfsmethoden ────────────────────────────────────────────────────

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
            if (raw.empty() || raw.find("..") != std::string::npos) return "";
            return (raw[0] == '/') ? raw : ("/" + raw);
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

        // ── setup ─────────────────────────────────────────────────────────────

        void FileManager::setup()
        {
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

        // ── Verzeichnis-Listing ───────────────────────────────────────────────

        void FileManager::handleList(WebRequest& req, WebResponse& res)
        {
            if (!ensureFs())
            {
                res.setStatus(500);
                res.send("LittleFS nicht verfügbar");
                return;
            }

            std::string dir = sanitizePath(req.getQueryParam("dir"));
            if (dir.empty()) dir = "/";

            std::string html;
            html.reserve(2048);

            auto fmtBytes = [](size_t b) -> std::string {
                char buf[24];
                if (b >= 1024 * 1024)
                    snprintf(buf, sizeof(buf), "%.1f MB", b / 1048576.0f);
                else if (b >= 1024)
                    snprintf(buf, sizeof(buf), "%.1f KB", b / 1024.0f);
                else
                    snprintf(buf, sizeof(buf), "%zu B", b);
                return buf;
            };

            html += "<div class='container'><h1>Dateiverwaltung";
            if (dir != "/")
            {
                html += " &ndash; ";
                html += htmlEscape(dir);
            }
            html += "</h1>";

            html += "<h2>Inhalt</h2>";
            html += "<table><thead><tr>"
                    "<th>Name</th><th class='right'>Gr&ouml;&szlig;e</th><th class='right'>Aktionen</th>"
                    "</tr></thead><tbody>";

            if (dir != "/")
            {
                std::string parent = dir.substr(0, dir.rfind('/'));
                if (parent.empty()) parent = "/";
                html += "<tr><td colspan='3'><a href='/filemanager?dir=";
                html += urlEncode(parent);
                html += "'>..</a></td></tr>";
            }

            bool hasEntries = false;

            // Erster Pass: Ordner
            {
                File root = LittleFS.open(dir.c_str(), "r");
                if (root)
                {
                    File entry = root.openNextFile();
                    while (entry)
                    {
                        if (entry.isDirectory())
                        {
                            hasEntries = true;
                            std::string path = absPath(dir, entry.name());
                            std::string displayName = path.substr(path.rfind('/') + 1);

                            html += "<tr><td><a href='/filemanager?dir=";
                            html += urlEncode(path);
                            html += "'>";
                            html += htmlEscape(displayName);
                            html += "/</a></td><td class='right'></td><td class='right'>";
                            html += "<a href='#' onclick=\"delDir('";
                            html += htmlEscape(jsStr(path));
                            html += "'); return false;\">L&ouml;schen</a>";
                            html += "</td></tr>";
                        }
                        entry.close();
                        entry = root.openNextFile();
                    }
                    root.close();
                }
            }

            // Zweiter Pass: Dateien
            {
                File root = LittleFS.open(dir.c_str(), "r");
                if (root)
                {
                    File entry = root.openNextFile();
                    while (entry)
                    {
                        if (!entry.isDirectory())
                        {
                            hasEntries = true;
                            std::string path = absPath(dir, entry.name());
                            std::string displayName = path.substr(path.rfind('/') + 1);
                            size_t sz = entry.size();

                            html += "<tr><td>";
                            html += htmlEscape(displayName);
                            html += "</td><td class='right'>";
                            html += fmtBytes(sz);
                            html += "</td><td class='right'><a href='/filemanager/download?path=";
                            html += urlEncode(path);
                            html += "'>Download</a> | <a href='#' onclick=\"delFile('";
                            html += htmlEscape(jsStr(path));
                            html += "'); return false;\">L&ouml;schen</a>";
                            html += "</td></tr>";
                        }
                        entry.close();
                        entry = root.openNextFile();
                    }
                    root.close();
                }
            }

            if (!hasEntries)
                html += "<tr><td colspan='3'><em>Leer</em></td></tr>";

            html += "</tbody></table>";

            html += "<h2>Ordner anlegen</h2>";
            html += "<div class='fm-row'>"
                    "<input type='text' id='nd' placeholder='Ordnername'>"
                    "<button onclick='mkDir()'>Anlegen</button>"
                    "</div>";

            html += "<h2>Datei hochladen</h2>";

            {
#ifdef ARDUINO_ARCH_ESP32
                size_t total = LittleFS.totalBytes();
                size_t freeBytes = total - LittleFS.usedBytes();
#else
                FSInfo fsInfo;
                LittleFS.info(fsInfo);
                size_t total = (size_t)fsInfo.totalBytes;
                size_t freeBytes = (size_t)(fsInfo.totalBytes - fsInfo.usedBytes);
#endif
                html += "<p class='fm-storage'>Speicher: <strong>";
                html += fmtBytes(freeBytes);
                html += "</strong> frei von ";
                html += fmtBytes(total);
                html += "</p>";
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

            // Nur currentDir ist dynamisch – der Rest steckt in /assets/filemanager.js
            html += "<script>const currentDir='";
            html += jsStrScriptBody(dir);
            html += "';</script></div>";

            res.setLayout(true);
            res.setActiveMenu("/filemanager");
            res.send(html.c_str());
        }

        // ── Download (Streaming) ──────────────────────────────────────────────

        void FileManager::handleDownload(WebRequest& req, WebResponse& res)
        {
            std::string path = sanitizePath(req.getQueryParam("path"));
            if (path.empty())
            {
                res.setStatus(400);
                res.send("Bad path");
                return;
            }

            if (!ensureFs() || !LittleFS.exists(path.c_str()))
            {
                res.setStatus(404);
                res.send("Not found");
                return;
            }

            FmFileCtx* ctx = new FmFileCtx{LittleFS.open(path.c_str(), "r")};
            if (!ctx->f)
            {
                delete ctx;
                res.setStatus(500);
                res.send("Open failed");
                return;
            }

            std::string name = path.substr(path.rfind('/') + 1);
            std::string cd = "attachment; filename=\"" + name + "\"";
            res.setContentType("application/octet-stream");
            res.setHeader("Content-Disposition", cd.c_str());
            res.sendStream(ctx->f.size(), fmFileRead, fmFileCleanup, ctx);
        }

        // ── Upload (chunked: jeder Chunk ist ein separater POST) ──────────────

        void FileManager::handleUpload(WebRequest& req, WebResponse& res)
        {
            std::string path = sanitizePath(req.getQueryParam("path"));
            if (path.empty() || req.bodyLength() == 0)
            {
                res.setStatus(400);
                res.send("Bad request");
                return;
            }

            if (!ensureFs())
            {
                res.setStatus(500);
                res.send("LittleFS nicht verfügbar");
                return;
            }

            std::string offsetStr = req.getQueryParam("offset");
            size_t offset = offsetStr.empty() ? 0 : (size_t)atol(offsetStr.c_str());

            if (offset == 0 && LittleFS.exists(path.c_str()))
            {
                res.setStatus(409);
                res.send("Datei existiert bereits");
                return;
            }

            File f = LittleFS.open(path.c_str(), offset == 0 ? "w" : "r+");
            if (!f)
            {
                res.setStatus(500);
                res.send("Cannot open file");
                return;
            }
            if (offset > 0 && !f.seek(offset))
            {
                f.close();
                res.setStatus(500);
                res.send("Seek failed");
                return;
            }

            size_t needed = req.bodyLength();

            f.write(req.body(), needed);
            f.close();

            // LittleFS schreibt erst beim close() auf Flash — write() gibt immer
            // die Puffergröße zurück, auch wenn kein Platz mehr da ist.
            // Einzige zuverlässige Prüfung: Dateigröße nach dem Schließen vergleichen.
            {
                File verify = LittleFS.open(path.c_str(), "r");
                size_t actualSize = verify ? verify.size() : 0;
                if (verify) verify.close();

                if (actualSize != offset + needed)
                {
                    LittleFS.remove(path.c_str());
                    res.setStatus(413);
                    res.send("Dateisystem voll");
                    return;
                }
            }

            res.setContentType("text/plain");
            res.send("OK");
        }

        // ── Delete (Datei oder Ordner) ────────────────────────────────────────

        void FileManager::handleDelete(WebRequest& req, WebResponse& res)
        {
            std::string path = sanitizePath(req.getQueryParam("path"));
            if (path.empty())
            {
                res.setStatus(400);
                res.send("Bad path");
                return;
            }

            if (!ensureFs())
            {
                res.setStatus(500);
                res.send("LittleFS nicht verfügbar");
                return;
            }

            File f = LittleFS.open(path.c_str(), "r");
            bool isDir = f && f.isDirectory();
            if (f) f.close();

            res.setContentType("text/plain");
            bool ok = isDir ? LittleFS.rmdir(path.c_str()) : LittleFS.remove(path.c_str());
            if (ok)
                res.send("OK");
            else
            {
                res.setStatus(500);
                res.send(isDir ? "Ordner nicht leer oder Fehler" : "Delete failed");
            }
        }

        // ── Mkdir ─────────────────────────────────────────────────────────────

        void FileManager::handleMkdir(WebRequest& req, WebResponse& res)
        {
            std::string path = sanitizePath(req.getQueryParam("path"));
            if (path.empty())
            {
                res.setStatus(400);
                res.send("Bad path");
                return;
            }

            if (!ensureFs())
            {
                res.setStatus(500);
                res.send("LittleFS nicht verfügbar");
                return;
            }

            res.setContentType("text/plain");
            if (LittleFS.mkdir(path.c_str()))
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
