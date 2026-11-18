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

        // A block to send. A view only: the bytes belong to the response or to the frame given to setLayoutChrome().
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
            void sendOwned(uint8_t* data, int length); // takes a block from malloc, frees it after sending
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

            /** @brief Frame around the body; head, tail and foot stay owned by the caller until the response is sent.
             *  activeAt >= 0 inserts " active" at that offset of head (the current menu item). */
            void setLayoutChrome(const char* head, int headLen, int activeAt, const char* tail, int tailLen,
                                 const char* foot, int footLen);
            void finalizeSegments();

            /** @brief Platform buffer for the per-request part of the frame; reset() keeps it. */
            void setScratch(char* buf, uint16_t cap)
            {
                _scratch = buf;
                _scratchCap = cap;
            }
            char* scratch() const { return _scratch; }
            uint16_t scratchCap() const { return _scratchCap; }

            const ResponseSegment* segments() const { return _segments; }
            int segmentCount() const { return _segCount; }
            int totalLength() const;

            void reset(); // RP2040: eine Instanz pro Verbindungsslot, wiederverwendet
            /** @brief After a failed allocation: drops body, stream and layout use and answers 503 without allocating.
             *  The caller runs finalizeSegments() unless handleRequest() still does. */
            void sendOutOfMemory();

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

            // Segments point into the frame of setLayoutChrome() and into _body
            const char* _chromeHead = nullptr;
            const char* _chromeTail = nullptr;
            const char* _chromeFoot = nullptr;
            uint16_t _chromeHeadLen = 0;
            uint16_t _chromeTailLen = 0;
            uint16_t _chromeFootLen = 0;
            int16_t _chromeActiveAt = -1;
            char* _scratch = nullptr;
            uint16_t _scratchCap = 0;
            static constexpr int MAX_SEGMENTS = 6; // head up to the active item, " active", head rest, tail, body, foot
            ResponseSegment _segments[MAX_SEGMENTS] = {};
            uint8_t _segCount = 0;
        };

    } // namespace Network
} // namespace OpenKNX
