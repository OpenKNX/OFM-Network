#ifdef OPENKNX_WEBFS

#include "OpenKNX/Network/Webserver/FileManager.h"
#include "OpenKNX/Network/Module.h"
#include "OpenKNX/Network/Webserver/Webserver.h"
#include <LittleFS.h>
#include <string>

namespace OpenKNX
{
    namespace Network
    {

        static const char filemanagerCss[] =
            ".right{text-align:right;white-space:nowrap}";

        static const char filemanagerJs[] =
            "async function uploadFile(){"
            "const file=document.getElementById('fi').files[0];"
            "if(!file)return;"
            "const base=currentDir==='/'?'':currentDir;"
            "const path=base+'/'+file.name;"
            "const CHUNK=2048;"
            "const bar=document.getElementById('bar');"
            "const st=document.getElementById('st');"
            "bar.style.display='inline';bar.value=0;st.textContent='';"
            "try{"
            "for(let off=0;off<file.size||off===0;off+=CHUNK){"
            "const end=Math.min(off+CHUNK,file.size);"
            "const r=await fetch('/filemanager/upload?path='+encodeURIComponent(path)+'&offset='+off,"
            "{method:'POST',body:file.slice(off,end),headers:{'Content-Type':'application/octet-stream'}});"
            "if(!r.ok){const msg=await r.text();throw new Error(r.status===409?'Datei existiert bereits \xe2\x80\x93 zuerst l\xc3\xb6schen':msg);}"
            "bar.value=Math.round(end/file.size*100);"
            "if(end>=file.size)break;"
            "}"
            "st.textContent='OK';"
            "setTimeout(()=>location.reload(),800);"
            "}catch(e){st.textContent='Fehler: '+e.message;bar.style.display='none';}"
            "}"
            "async function delFile(path){"
            "if(!confirm('Datei l\xc3\xb6schen: '+path+'?'))return;"
            "const r=await fetch('/filemanager/delete?path='+encodeURIComponent(path),{method:'POST'});"
            "if(r.ok)location.reload();else alert(await r.text());"
            "}"
            "async function delDir(path){"
            "if(!confirm('Ordner l\xc3\xb6schen: '+path+'?'))return;"
            "const r=await fetch('/filemanager/delete?path='+encodeURIComponent(path),{method:'POST'});"
            "if(r.ok)location.reload();else alert(await r.text());"
            "}"
            "async function mkDir(){"
            "const name=document.getElementById('nd').value.trim();"
            "if(!name)return;"
            "const base=currentDir==='/'?'':currentDir;"
            "const path=base+'/'+name;"
            "const r=await fetch('/filemanager/mkdir?path='+encodeURIComponent(path),{method:'POST'});"
            "if(r.ok)location.reload();else alert(await r.text());"
            "}";

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

        static std::string absPath(const std::string& dir, const std::string& entryName)
        {
            if (!entryName.empty() && entryName[0] == '/') return entryName;
            return (dir == "/" ? "" : dir) + "/" + entryName;
        }

        // ── setup ─────────────────────────────────────────────────────────────

        void FileManager::setup()
        {
            ensureFs();

            openknxNetwork.webserver.addMenuItem("Dateien", "/filemanager", 50);

            openknxNetwork.webserver.addRoute(WEB_GET, "/assets/filemanager.css",
                                              Webserver::Static("text/css", filemanagerCss));
            openknxNetwork.webserver.addRoute(WEB_GET, "/assets/filemanager.js",
                                              Webserver::Static("application/javascript", filemanagerJs));

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

            html += "<div class='container'><h1>Dateiverwaltung";
            if (dir != "/")
            {
                html += " &ndash; ";
                html += dir;
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
                            html += displayName;
                            html += "/</a></td><td class='right'></td><td class='right'>";
                            html += "<a href='#' onclick=\"delDir('";
                            html += jsStr(path);
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
                            html += displayName;
                            html += "</td><td class='right'>";
                            html += std::to_string(sz);
                            html += " B</td><td class='right'><a href='/filemanager/download?path=";
                            html += urlEncode(path);
                            html += "'>Download</a> | <a href='#' onclick=\"delFile('";
                            html += jsStr(path);
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
            html += "<input type='text' id='nd' placeholder='Ordnername'>&nbsp;"
                    "<button onclick='mkDir()'>Anlegen</button>";

            html += "<h2>Datei hochladen</h2>";
            html += "<input type='file' id='fi'>&nbsp;"
                    "<button onclick='uploadFile()'>Hochladen</button>&nbsp;"
                    "<progress id='bar' value='0' max='100' style='display:none;width:160px'></progress>&nbsp;"
                    "<span id='st'></span>";

            // Nur currentDir ist dynamisch – der Rest steckt in /assets/filemanager.js
            html += "<script>const currentDir='";
            html += jsStr(dir);
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

            size_t written = f.write(req.body(), req.bodyLength());
            f.close();

            if (written != req.bodyLength())
            {
                res.setStatus(500);
                res.send("Write incomplete");
                return;
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
