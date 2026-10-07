#pragma once

#if defined(ARDUINO_ARCH_RP2040) || defined(ARDUINO_ARCH_ESP32)

#include <IPAddress.h>
#include <functional>
#include <string>

namespace OpenKNX
{
    namespace Network
    {
        namespace DNS
        {
            // Resolves a hostname asynchronously. Callback receives 0.0.0.0 on failure and
            // always runs from loop(), never inline and never in a network callback.
            void query(const std::string& name, std::function<void(IPAddress)> callback);

            // Called from Network::Module::loop() — delivers queued results.
            void loop();
        } // namespace DNS
    } // namespace Network
} // namespace OpenKNX

#endif // defined(ARDUINO_ARCH_RP2040) || defined(ARDUINO_ARCH_ESP32)
