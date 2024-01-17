#pragma once
#include "OpenKNX.h"
#include "strings.h"
#include <functional>

#ifndef ARDUINO_ARCH_ESP32
    #include "UsbExchangeModule.h"
#endif

#if defined(KNX_IP_W5500)
    #include <W5500lwIP.h>
    #include <lwip/dhcp.h>
#elif defined(KNX_IP_WIFI)
    #include <WiFi.h>
#elif defined(KNX_IP_GENERIC)

#else
    #error "no Ethernet stack specified, #define KNX_IP_WIFI or KNX_IP_W5500"
#endif

#define OPENKNX_NETWORK_MAGIC 4150479753

typedef std::function<void(bool)> NetworkChangeCallback;

class NetworkModule : public OpenKNX::Module
{
  public:
    const std::string name() override;
    const std::string version() override;
    void init() override;
    void loop(bool configured) override;
    void setup(bool configured) override;
    // uint16_t flashSize() override;
    // void writeFlash() override;
    // void readFlash(const bool configured, const uint8_t *data, const uint16_t size) override;
    void writeToFlash();
    void readFromFlash();

    void savePower() override;
    bool restorePower() override;
    void showInformations() override;
    bool processCommand(const std::string cmd, bool debugKo);
    void showHelp() override;
    void showNetworkInformations(bool console = false);
#ifndef ARDUINO_ARCH_ESP32
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
    void macAddress(uint8_t *address);

  private:
    OpenKNX::Flash::Driver _flash;

    IPAddress GetIpProperty(uint8_t PropertyId);
    void SetIpProperty(uint8_t PropertyId, IPAddress IPAddress);
    uint8_t GetByteProperty(uint8_t PropertyId);
    void SetByteProperty(uint8_t PropertyId, uint8_t value);

    bool _powerSave = false;
    bool _ipShown = false;
    bool _useStaticIP = false;
    bool _useMDNS = false;
    uint8_t _lanMode = 0;
    uint8_t _networkType = 0;
    IPAddress _staticLocalIP;
    IPAddress _staticSubnetMask;
    IPAddress _staticGatewayIP;
    IPAddress _staticNameServerIP;

    uint8_t _mac[6] = {};
    char _hostName[25] = {};
#if defined(KNX_IP_WIFI) || true
    char _wifiSSID[33] = {};
    char _wifiPassword[64] = {};
#endif

    char *_mDNSHttpServiceName = nullptr;
    char *_mDNSDeviceServiceName = nullptr;
    char *_mDNSDeviceServiceNameTXT = nullptr;
    bool _currentLinkState = false;
    uint32_t _lastLinkCheck = false;

    void initPhy();
    void initIp();
    void loadSettings();
    void checkLinkStatus();
    void checkIpStatus();
    void loadCallbacks(bool state);
    void handleMDNS();

    std::vector<NetworkChangeCallback> _callback;
};

extern NetworkModule openknxNetwork;