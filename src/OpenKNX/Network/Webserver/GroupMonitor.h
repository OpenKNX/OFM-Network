#pragma once

#ifdef OPENKNX_WEBMONITOR

#include "OpenKNX/Format/JSON/Writer.h"

// Forward declaration – die vollständige Definition kommt in der .cpp über OpenKNX.h.
namespace TPUart
{
    class Frame;
}

namespace OpenKNX
{
    namespace Network
    {

        // Streamt vom TP-UART empfangene Telegramme live per WebSocket an /groupmonitor.
        // Read-only auf dem Bus.
        class GroupMonitor
        {
          public:
            void setup();
            void onFrame(TPUart::Frame& frame); // TP-UART-Callback, loop-Kontext

          private:
            bool _initialized = false;

            // Reused across calls: reset() keeps the string's capacity, so after the
            // first few telegrams building the JSON no longer allocates.
            OpenKNX::Format::JSON::Writer _json;
        };

    } // namespace Network
} // namespace OpenKNX

#endif // OPENKNX_WEBMONITOR
