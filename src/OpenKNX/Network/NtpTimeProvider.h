#if defined(KNX_IP_WIFI) || defined(KNX_IP_LAN)
#pragma once
#include "OpenKNX.h"
#include "knxprod.h"

#ifdef ParamNET_NTP
namespace OpenKNX
{
    namespace Network
    {

        class NtpTimeProvider : public OpenKNX::Time::TimeProvider
        {
            static NtpTimeProvider* currentInstance;

          protected:
            bool _initialized = false;
#ifdef ARDUINO_ARCH_RP2040
            enum class State : uint8_t { Idle, Waiting, WaitingLogged, Synced };
            State _state = State::Idle;
            uint32_t _timer = 0;
#endif

            const std::string logPrefix() override;
            void setup() override;
            void loop() override;
            void logInformation() override;
            ~NtpTimeProvider() override;
        };

    } // namespace Network
} // namespace OpenKNX

using NtpTimeProvider = OpenKNX::Network::NtpTimeProvider;
#endif
#endif
