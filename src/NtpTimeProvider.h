#if defined(KNX_IP_WIFI) || defined(KNX_IP_LAN)
#pragma once
#include "OpenKNX.h"
#include "knxprod.h"

#ifdef ParamNET_NTP
class NtpTimeProvider : public OpenKNX::Time::TimeProvider
{
    static NtpTimeProvider* currentInstance;

  protected:
    bool _initialized = false;
#ifdef ARDUINO_ARCH_RP2040
    int32_t _lastSync = 0;
    bool _syncInProgress = false;
#endif

    const std::string logPrefix() override;
    void setup() override;
    void loop() override;
    void logInformation() override;
    ~NtpTimeProvider() override;
};
#endif
#endif