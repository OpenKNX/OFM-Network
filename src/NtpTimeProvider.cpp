#if defined(KNX_IP_WIFI) || defined(KNX_IP_LAN)
#include "NtpTimeProvider.h"
#include "NetworkModule.h"

#ifdef ParamNET_NTP
#ifdef ARDUINO_ARCH_ESP32
#if defined(ESP_IDF_VERSION) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#define SNTP_GETREACHABILITY esp_sntp_getreachability
#else
#define SNTP_GETREACHABILITY sntp_getreachability
#endif
#include "esp_sntp.h"

NtpTimeProvider* NtpTimeProvider::currentInstance = nullptr;
#endif

void NtpTimeProvider::logInformation()
{
    if (!_initialized || strlen((const char*)ParamNET_NTPServer) == 0)
    {
        logInfoP("Timeprovider: NTP (%s)", "unconfigured");
        return;
    };

    logInfoP("Timeprovider: NTP (%s)", ParamNET_NTPServer);
#ifdef ARDUINO_ARCH_ESP32
    logIndentUp();
    switch (sntp_get_sync_status())
    {
        case sntp_sync_status_t::SNTP_SYNC_STATUS_COMPLETED:
            logInfoP("Time synchronized");
            break;
        case sntp_sync_status_t::SNTP_SYNC_STATUS_IN_PROGRESS:
            logInfoP("Time synchronization in progress");
            break;
        case sntp_sync_status_t::SNTP_SYNC_STATUS_RESET:
            logInfoP("Time synchronization reset");
            break;
    }
    bool serverFound = false;
    bool reachable = false;
    for (size_t i = 0; esp_sntp_getservername(i); i++)
    {
        if ((int)SNTP_GETREACHABILITY(i))
            reachable = true;
        serverFound = true;
    }
    if (serverFound)
        logInfoP("NTP Server %s: %s", ParamNET_NTPServer, reachable ? "reachable" : "not reachable or not used");
    else
        logErrorP("No NTP server configured");

    logIndentDown();
#endif
}

const std::string NtpTimeProvider::logPrefix()
{
    return "Time<NTP>";
}

void NtpTimeProvider::setup()
{
    if (strlen((const char*)ParamNET_NTPServer) > 0)
    {
#ifdef ARDUINO_ARCH_ESP32
        // Configure the NTP server 3 times because the DNS should be queried multible times to return differnt IP addresses
        esp_sntp_stop();
        esp_sntp_setservername(0, (const char*)ParamNET_NTPServer);
        esp_sntp_setservername(1, (const char*)ParamNET_NTPServer);
        esp_sntp_setservername(2, (const char*)ParamNET_NTPServer);

        currentInstance = this;
        esp_sntp_set_time_sync_notification_cb([](struct timeval* tv) {
            currentInstance->timeSet();
        });
        esp_sntp_set_sync_mode(sntp_sync_mode_t::SNTP_SYNC_MODE_SMOOTH);
        esp_sntp_setoperatingmode(esp_sntp_operatingmode_t::ESP_SNTP_OPMODE_POLL);
        esp_sntp_init();
#endif
        _initialized = true;
    }
    else
    {
        logErrorP("No NTP server configured");
    }
}

void NtpTimeProvider::loop()
{
#ifdef ARDUINO_ARCH_RP2040
    if (_syncInProgress) return;
    if (!_initialized) return;

    if (openknxNetwork.established() && (!_lastSync || delayCheck(_lastSync, 3600000))) // check every 1 hour
    {
        _syncInProgress = true;
        logInfoP("Synchronize time with ntp server");
        openknx.common.skipLooptimeWarning();
        openknx.loop();
        NTP.begin((const char*)ParamNET_NTPServer, (const char*)ParamNET_NTPServer); // not working on setup()
        NTP.waitSet(
            []() {
                openknx.common.skipLooptimeWarning();
                openknx.loop();
            });
        openknx.common.skipLooptimeWarning();
        timeSet();
        _lastSync = millis();
        _syncInProgress = false;
    }
#endif
}

NtpTimeProvider::~NtpTimeProvider()
{
    if (!_initialized) return;

#ifdef ARDUINO_ARCH_ESP32
    esp_sntp_stop();
    currentInstance = nullptr;
#endif
}

// #endif
#endif
#endif