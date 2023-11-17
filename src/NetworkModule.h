#include "LEAmDNS.h"
#include "OpenKNX.h"
#include "UsbExchangeModule.h"
#include "strings.h"

#if defined(KNX_IP_W5500)
    #include <W5500lwIP.h>
    #include <lwip/dhcp.h>
#elif defined(KNX_IP_WIFI)
    #include <WiFi.h>
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
    void showInformations() override;
    bool processCommand(const std::string cmd, bool debugKo);
    void showHelp() override;
    void showNetworkInformations(bool console = false);
    void fillNetworkFile(UsbExchangeFile *file);
    void registerCallback(NetworkChangeCallback callback);

    bool connected();
    IPAddress localIP();
    IPAddress subnetMask();
    IPAddress gatewayIP();
    IPAddress dns1IP();
    IPAddress dns2IP();
    void macAddress(uint8_t *address);

    MDNSResponder _mdns;
  private:
    IPAddress GetIpProperty(uint8_t PropertyId);
    void SetIpProperty(uint8_t PropertyId, IPAddress IPAddress);
    uint8_t GetByteProperty(uint8_t PropertyId);
    void SetByteProperty(uint8_t PropertyId, uint8_t value);

    bool _ipShown = false;
    bool _useStaticIP = 0;
    IPAddress _staticLocalIP = 0;
    IPAddress _staticSubnetMask = 0;
    IPAddress _staticGatewayIP = 0;
    IPAddress _staticDns1IP = 0;
    IPAddress _staticDns2IP = 0;

    char *_hostName = nullptr;
    bool _currentLinkState = 0;
    uint32_t _lastLinkCheck = false;

    void initPhy();
    void loadIpSettings();
    void checkLinkStatus();
    void checkIpStatus();
    void loadCallbacks(bool state);

    std::vector<NetworkChangeCallback> _callback;
};

extern NetworkModule openknxNetwork;