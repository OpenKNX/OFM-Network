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
            void loop();
            void onFrame(TPUart::Frame& frame); // TP-UART-Callback, loop-Kontext

          private:
            bool _initialized = false;
            // The busmon handler is a network callback (RP2040: the ethernet IRQ), while
            // startMonitoring()/reset() queue a control byte into a ring whose producer
            // and consumer must share one context. So record here, switch in loop().
            // Unguarded by MASK_VERSION on purpose -- one byte, no header dependency.
            volatile int8_t _pendingBusmon = -1; // -1 = nothing, 0 = off, 1 = on

            // Reused across calls: reset() keeps the string's capacity, so after the
            // first few telegrams building the JSON no longer allocates.
            OpenKNX::Format::JSON::Writer _json;
        };

    } // namespace Network
} // namespace OpenKNX

#endif // OPENKNX_WEBMONITOR
