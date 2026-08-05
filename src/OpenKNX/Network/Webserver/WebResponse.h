#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace OpenKNX
{
    namespace Network
    {

        using WebStreamReadFn = size_t (*)(void* ctx, uint8_t* buf, size_t maxLen);
        using WebStreamCleanupFn = void (*)(void* ctx);

        // Ein zu sendender Block. Nur eine Sicht — der Speicher gehört immer der
        // WebResponse, deshalb kein Ownership-Flag.
        struct ResponseSegment
        {
            const uint8_t* data;
            int len;
        };

        class WebResponse
        {
          public:
            WebResponse();

            // Der Body-Zeiger wird besessen; eine Kopie würde ihn doppelt freigeben.
            WebResponse(const WebResponse&) = delete;
            WebResponse& operator=(const WebResponse&) = delete;

            void setStatus(uint16_t code);
            void setContentType(const char* mimeType);
            void setHeader(const char* name, const char* value);
            void setLayout(bool useLayout) { _useLayout = useLayout; }
            void setActiveMenu(const std::string& uri) { _activeMenuUri = uri; }

            void send(const char* text);
            void send(const uint8_t* data, int length);
            void sendStatic(const char* text);
            void sendStatic(const uint8_t* data, int length);

            // Generiertes Web-Asset (webassets.h), gzip, ohne Content-Negotiation.
            void sendAsset(const char* mimeType, const uint8_t* data, size_t length);

            // 301-Redirect auf target, leerer Body.
            void sendRedirect(const std::string& target);

            // Streaming-Response: Plattform ruft readFn wiederholt auf; cleanupFn nach EOF/Fehler
            void sendStream(size_t totalLength,
                            WebStreamReadFn readFn, WebStreamCleanupFn cleanupFn, void* ctx);

            uint16_t statusCode() const { return _statusCode; }
            const char* contentType() const { return _contentType; }
            const char* body() const { return (const char*)_body; }
            int bodyLength() const { return _bodyLength; }
            bool isStatic() const { return !_bodyOwned; }

            bool useLayout() const { return _useLayout; }
            const std::string& activeMenuUri() const { return _activeMenuUri; }

            const std::vector<std::pair<std::string, std::string>>& responseHeaders() const { return _headers; }

            // Streaming-Accessoren für Plattform-Code
            bool isStreaming() const { return _streaming; }
            size_t streamTotal() const { return _streamTotal; }
            WebStreamReadFn streamReadFn() const { return _streamReadFn; }
            WebStreamCleanupFn streamCleanupFn() const { return _streamCleanupFn; }
            void* streamCtx() const { return _streamCtx; }
            size_t readStreamChunk(uint8_t* buf, size_t maxLen);
            void cleanupStream();

            // ── Segmente ─────────────────────────────────────────────────────
            // Befüllt von Webserver::handleRequest(), danach nur noch gelesen.

            void setLayoutChrome(std::string header, std::string footer);
            void finalizeSegments();

            const ResponseSegment* segments() const { return _segments; }
            int segmentCount() const { return _segCount; }
            int totalLength() const;

            void reset(); // RP2040: eine Instanz pro Verbindungsslot, wiederverwendet

            ~WebResponse();

          private:
            uint16_t _statusCode = 200;
            char _contentType[40] = "text/html; charset=utf-8"; // Kopie statt const char* — kein dangling auf Temporäres
            uint8_t* _body = nullptr;
            int _bodyLength = 0;
            bool _bodyOwned = true;
            bool _useLayout = false;
            std::string _activeMenuUri;
            std::vector<std::pair<std::string, std::string>> _headers;

            // Streaming
            bool _streaming = false;
            size_t _streamTotal = 0;
            WebStreamReadFn _streamReadFn = nullptr;
            WebStreamCleanupFn _streamCleanupFn = nullptr;
            void* _streamCtx = nullptr;

            // Segmente zeigen in _layoutHeader/_body/_layoutFooter, alles dieser Instanz
            std::string _layoutHeader;
            std::string _layoutFooter;
            static constexpr int MAX_SEGMENTS = 3; // Header + Body + Footer
            ResponseSegment _segments[MAX_SEGMENTS] = {};
            uint8_t _segCount = 0;
        };

    } // namespace Network
} // namespace OpenKNX
