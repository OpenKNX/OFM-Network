#if defined(KNX_IP_WIFI) || defined(KNX_IP_LAN)
#pragma once
#include "OpenKNX.h"
#include "knxprod.h"

#ifdef ParamNET_NTP
#ifdef ARDUINO_ARCH_ESP32
class NtpTimeProvider : public OpenKNX::Time::TimeProvider
{
    static NtpTimeProvider* currentInstance;

  protected:
    const std::string logPrefix() override;
    void setup() override;
    void logInformation() override;
    ~NtpTimeProvider() override;
};
#endif
#endif
#endif