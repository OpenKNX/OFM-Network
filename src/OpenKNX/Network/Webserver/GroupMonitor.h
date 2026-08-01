#pragma once

#ifdef OPENKNX_WEBMONITOR

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
        // Read-only, kein eigener Puffer.
        class GroupMonitor
        {
          public:
            void setup();
            void onFrame(TPUart::Frame& frame); // TP-UART-Callback, loop-Kontext

          private:
            bool _initialized = false;
        };

    } // namespace Network
} // namespace OpenKNX

#endif // OPENKNX_WEBMONITOR
