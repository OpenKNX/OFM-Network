#pragma once
#include "OpenKNX.h"
#include "UsbExchangeModule.h"
#include "strings.h"
#include <functional>

#if defined(KNX_IP_W5500)
    #include <W5500lwIP.h>
    #include <lwip/dhcp.h>
#elif defined(KNX_IP_WIFI)
    #include <WiFi.h>
#elif defined(KNX_IP_GENERIC)

#else
    #error "no Ethernet stack specified, #define KNX_IP_WIFI or KNX_IP_W5500"
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
    void fillNetworkFile(UsbExchangeFile *file);
    void registerCallback(NetworkChangeCallback callback);

    bool connected();
    bool established();
    IPAddress localIP();
    IPAddress subnetMask();
    IPAddress gatewayIP();
    IPAddress nameServerIP();
    void macAddress(uint8_t *address);

  private:
    IPAddress GetIpProperty(uint8_t PropertyId);
    void SetIpProperty(uint8_t PropertyId, IPAddress IPAddress);
    uint8_t GetByteProperty(uint8_t PropertyId);
    void SetByteProperty(uint8_t PropertyId, uint8_t value);

    bool _powerSave = false;
    bool _ipShown = false;
    bool _useStaticIP = 0;
    IPAddress _staticLocalIP = 0;
    IPAddress _staticSubnetMask = 0;
    IPAddress _staticGatewayIP = 0;
    IPAddress _staticNameServerIP = 0;

    char *_hostName = nullptr;
    char *_mDNSHttpServiceName = nullptr;
    char *_mDNSDeviceServiceName = nullptr;
    uint8_t _mac[6] = {};
    bool _currentLinkState = false;
    uint32_t _lastLinkCheck = false;

    void initPhy();
    void prepareSettings();
    void checkLinkStatus();
    void checkIpStatus();
    void loadCallbacks(bool state);
    void handleMDNS();

    std::vector<NetworkChangeCallback> _callback;
};

extern NetworkModule openknxNetwork;