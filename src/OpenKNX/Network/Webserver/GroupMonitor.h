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

        // Gruppenmonitor: streamt alle vom TP-UART empfangenen Telegramme live per
        // WebSocket an die /groupmonitor-Seite. Read-only, ohne eigenen Puffer – der
        // TP-Callback dekodiert und broadcastet direkt (siehe GroupMonitor.cpp).
        class GroupMonitor
        {
          public:
            void setup();
            // Vom TP-UART registerReceivedFrame-Callback aufgerufen (loop-Kontext).
            void onFrame(TPUart::Frame& frame);

          private:
            bool _initialized = false;
        };

    } // namespace Network
} // namespace OpenKNX

#endif // OPENKNX_WEBMONITOR
