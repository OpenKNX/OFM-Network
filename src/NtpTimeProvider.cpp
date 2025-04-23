#if defined(KNX_IP_WIFI) || defined(KNX_IP_LAN)
#include "NtpTimeProvider.h"

#ifdef ParamNET_NTP
#ifdef ARDUINO_ARCH_ESP32
#if defined(ESP_IDF_VERSION) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#define SNTP_GETREACHABILITY esp_sntp_getreachability
#else
#define SNTP_GETREACHABILITY sntp_getreachability
#endif
#include "esp_sntp.h"

NtpTimeProvider* NtpTimeProvider::currentInstance = nullptr;

void NtpTimeProvider::logInformation()
{
    logInfoP("Timeprovider: NTP");
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
}

const std::string NtpTimeProvider::logPrefix()
{
    return "Time<NTP>";
}

void NtpTimeProvider::setup()
{
    esp_sntp_stop();
    u8_t index = 0;
    if (strlen((const char*)ParamNET_NTPServer) > 0)
    {
        // Configure the NTP server 3 times because the DNS should be queried multible times to return differnt IP addresses
        esp_sntp_setservername(index++, (const char*)ParamNET_NTPServer);
        esp_sntp_setservername(index++, (const char*)ParamNET_NTPServer);
        esp_sntp_setservername(index++, (const char*)ParamNET_NTPServer);

        // esp_netif_sntp_init(&config);
    }
    if (index > 0)
    {
        currentInstance = this;
        esp_sntp_set_time_sync_notification_cb([](struct timeval* tv) {
            currentInstance->timeSet();
        });
        esp_sntp_set_sync_mode(sntp_sync_mode_t::SNTP_SYNC_MODE_SMOOTH);
        esp_sntp_setoperatingmode(esp_sntp_operatingmode_t::ESP_SNTP_OPMODE_POLL);
        esp_sntp_init();
    }
    else
    {
        logErrorP("No NTP server configured");
    }
}

NtpTimeProvider::~NtpTimeProvider()
{
    esp_sntp_stop();
    currentInstance = nullptr;
}

#endif
#endif
#endif