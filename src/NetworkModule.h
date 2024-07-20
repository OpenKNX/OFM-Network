#pragma once
#include "OpenKNX.h"
#include "strings.h"
#include <functional>

#if defined(ARDUINO_ARCH_ESP32)
    #include <ESPmDNS.h>
    #include <Preferences.h>
    #include <vector>
    #if defined(KNX_IP_LAN)
        #include <ETH.h>
        #define KNX_NETIF ETH
    #elif defined(KNX_IP_WIFI)
        #include <WiFi.h>
    #endif
#elif defined(ARDUINO_ARCH_RP2040)
    #ifndef OPENKNX_USB_EXCHANGE_IGNORE
        #define HAS_USB
    #endif

    #ifndef OPENKNX_NET_SPI_SPEED
        #define OPENKNX_NET_SPI_SPEED 28000000
    #endif

    #if defined(KNX_IP_LAN)
        #include <W5500lwIP.h>
        #include <lwip/dhcp.h>
    #elif defined(KNX_IP_WIFI)
        #include "LittleFS.h"
        #include <WiFi.h>
    #endif
#else
    #pragma warn "Unsupported platform"
#endif

#ifdef HAS_USB
    #include "UsbExchangeModule.h"
#endif

typedef std::function<void(bool)> NetworkChangeCallback;

class NetworkModule : public OpenKNX::Module
{
  public:
    const std::string name() override;
    const std::string version() override;
    void init() override;
    void loop(bool configured) override;
    void setup(bool configured) override;

    void savePower() override;
    bool restorePower() override;
    void showInformations() override;
    bool processCommand(const std::string cmd, bool debugKo);
    void showHelp() override;
    void showNetworkInformations(bool console = false);

    bool processFunctionProperty(uint8_t objectIndex, uint8_t propertyId, uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength) override;

#ifdef KNX_IP_WIFI
    void saveWifiSettings(const char *ssid, const char *passphrase);
    void readWifiSettings();
#endif

#ifdef HAS_USB
    void fillNetworkFile(UsbExchangeFile *file);
#endif

    void registerCallback(NetworkChangeCallback callback);

    bool connected();
    bool established();
    IPAddress localIP();
    IPAddress subnetMask();
    IPAddress gatewayIP();
    IPAddress nameServerIP();
    std::string phyMode();

#ifdef ARDUINO_ARCH_ESP32
    void esp32WifiEvent(WiFiEvent_t event);
#endif

  private:
    IPAddress GetIpProperty(uint8_t PropertyId);
    void SetIpProperty(uint8_t PropertyId, IPAddress IPAddress);
    uint8_t GetByteProperty(uint8_t PropertyId);
    void SetByteProperty(uint8_t PropertyId, uint8_t value);

    bool _powerSave = false;
    bool _ipShown = false;
    bool _useStaticIP = false;
    bool _useMDNS = false;
    IPAddress _staticLocalIP;
    IPAddress _staticSubnetMask;
    IPAddress _staticGatewayIP;
    IPAddress _staticNameServerIP;

#ifdef ARDUINO_ARCH_ESP32
    bool espConnected = false;
#endif

    char _hostName[25] = {};

    char *_mDNSHttpServiceName = nullptr;
    char *_mDNSDeviceServiceName = nullptr;
    char *_mDNSDeviceServiceNameTXT = nullptr;
    bool _currentLinkState = false;
    uint32_t _lastLinkCheck = false;
    uint32_t _restartTimer = 0;

    void initPhy();
    void initIp();
    void loadSettings();
    void checkLinkStatus();
    void checkIpStatus();
    void loadCallbacks(bool state);
    void handleMDNS();

#ifdef KNX_IP_WIFI
    char _wifiSSID[33] = {};
    char _wifiPassphrase[64] = {};
#endif

    std::vector<NetworkChangeCallback> _callback;
};

extern NetworkModule openknxNetwork;