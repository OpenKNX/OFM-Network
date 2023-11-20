#include "NetworkModule.h"

#if defined(KNX_IP_W5500)
Wiznet5500lwIP KNX_NETIF(PIN_ETH_SS, ETH_SPI_INTERFACE);
#endif

WiFiUDP Udp;

// Give your Module a name
// it will be displayed when you use the method log("Hello")
//  -> Log     Hello
const std::string NetworkModule::name()
{
    return "Network";
}

// You can also give it a version
// will be displayed in Command Infos
const std::string NetworkModule::version()
{
    return MODULE_Network_Version;
}

void NetworkModule::initPhy()
{
#if defined(KNX_IP_W5500)
    // Hardreset of W5500 ToDo
    // Ethernet.setRstPin(PIN_ETH_RES);
    // Ethernet.hardreset();

    ETH_SPI_INTERFACE.setRX(PIN_ETH_MISO);
    ETH_SPI_INTERFACE.setTX(PIN_ETH_MOSI);
    ETH_SPI_INTERFACE.setSCK(PIN_ETH_SCK);
    ETH_SPI_INTERFACE.setCS(PIN_ETH_SS);

    logDebugP("Ethernet SPI GPIO: RX/MISO: %d, TX/MOSI: %d, SCK/SCLK: %d, CSn/SS: %d", PIN_ETH_MISO, PIN_ETH_MOSI, PIN_ETH_SCK, PIN_ETH_SS);
#elif defined(KNX_IP_WIFI)
    // TODO WLAN
    #pragma warn "Implementation for WiFi missing"
#endif
}

void NetworkModule::loadIpSettings()
{
    if (!knx.configured()) return;

#if MASK_VERSION == 0x091A
    _staticGatewayIP = GetIpProperty(PID_DEFAULT_GATEWAY);
    _staticSubnetMask = GetIpProperty(PID_SUBNET_MASK);
    _staticLocalIP = GetIpProperty(PID_IP_ADDRESS);
    _useStaticIP = GetByteProperty(PID_IP_ASSIGNMENT_METHOD) == 1; // see 2.5.6 of 03_08_03
#else
    if (ParamNET_CustomHostname)
    {
        memcpy(_hostName, ParamNET_HostName, 24);
    }

    _staticLocalIP = htonl(ParamNET_HostAddress);
    _staticSubnetMask = htonl(ParamNET_SubnetMask);
    _staticGatewayIP = htonl(ParamNET_GatewayAddress);
    _staticDns1IP = htonl(ParamNET_NameserverAddress1);
    _staticDns2IP = htonl(ParamNET_NameserverAddress2);
    _useStaticIP = ParamNET_StaticIP;
#endif
}

void NetworkModule::init()
{
    logInfoP("Init IP Stack");
    logIndentUp();

    SetByteProperty(PID_IP_CAPABILITIES, 6); // AutoIP + DHCP

    _hostName = (char *)malloc(25);
    memset(_hostName, 0, 25);
    memcpy(_hostName, "OpenKNX-", 8);
    memcpy(_hostName + 8, openknx.info.humanSerialNumber().c_str() + 5, 8);

    initPhy();
    loadIpSettings();

    if (_useStaticIP)
    {
        logInfoP("Using static IP");
        logIndentUp();
        if (!KNX_NETIF.config(_staticLocalIP, _staticGatewayIP, _staticSubnetMask, _staticDns1IP, _staticDns2IP))
        {
            logErrorP("Invalid IP settings");
        }
        logIndentDown();
        SetByteProperty(PID_CURRENT_IP_ASSIGNMENT_METHOD, 1);
    }
    else
    {
        logInfoP("Using DHCP");
        SetByteProperty(PID_CURRENT_IP_ASSIGNMENT_METHOD, 2); // ToDo
    }

    logInfoP("Hostname: %s", _hostName);
    logIndentUp();
    if (!KNX_NETIF.hostname(_hostName))
    {
        logErrorP("Hostname not applied");
    }
    logIndentDown();

    if (!KNX_NETIF.begin())
    {
        openknx.hardware.fatalError(1, "Error communicating with W5500 Ethernet chip");
    }

    logIndentDown();
}

void NetworkModule::setup(bool configured)
{
    openknxUsbExchangeModule.onLoad("Network.txt", [this](UsbExchangeFile *file) { this->fillNetworkFile(file); });

    registerCallback([this](bool state) { if (state) this->showNetworkInformations(false); });

#ifndef ParamNET_mDNS
    #define ParamNET_mDNS true
#endif
    if (!configured || ParamNET_mDNS)
    {
        if (!MDNS.begin(_hostName)) logErrorP("Hostname not applied (mDNS)");
        MDNS.addService("http", "tcp", 80);
        MDNS.addService("device-info", "tcp", -1);
        MDNS.addServiceTxt("device-info", "tcp", "serial", openknx.info.humanSerialNumber().c_str());
        MDNS.addServiceTxt("device-info", "tcp", "firmware", openknx.info.humanFirmwareVersion().c_str());
        if (configured)
        {
            MDNS.addServiceTxt("device-info", "tcp", "address", openknx.info.humanIndividualAddress().c_str());
            MDNS.addServiceTxt("device-info", "tcp", "application", openknx.info.humanApplicationVersion().c_str());
        }
        registerCallback([this](bool state) { if (state) MDNS.notifyAPChange(); });
    }
}

void NetworkModule::fillNetworkFile(UsbExchangeFile *file)
{
    writeLineToFile(file, "OpenKNX Network");
    writeLineToFile(file, "-----------------");
    writeLineToFile(file, "");
    writeLineToFile(file, "Hostname: %s", _hostName);
    writeLineToFile(file, "Network: %s", connected() ? "Established" : "Disconnected");
    if (connected())
    {
        writeLineToFile(file, "IP-Address: %s", localIP().toString().c_str());
        writeLineToFile(file, "Netmask: %s", subnetMask().toString().c_str());
        writeLineToFile(file, "Gateway: %s", gatewayIP().toString().c_str());
        writeLineToFile(file, "DNS1: %s", dns1IP().toString().c_str());
        writeLineToFile(file, "DNS2: %s", dns2IP().toString().c_str());
    }
}

void NetworkModule::checkIpStatus()
{
    if (_ipShown || !connected()) return;
    logBegin();
    logInfoP("Network established");
    logIndentUp();
    loadCallbacks(true);
    logIndentDown();
    logEnd();
    _ipShown = true;
}

void NetworkModule::checkLinkStatus()
{
    if (!delayCheckMillis(_lastLinkCheck, 500))
        return;

#if defined(KNX_IP_W5500)
    uint8_t newLinkState = KNX_NETIF.isLinked();

    // got link
    if (newLinkState && !_currentLinkState)
    {
        logInfoP("Link connected");
        netif_set_link_up(KNX_NETIF.getNetIf());
        if (_useStaticIP)
            netif_set_ipaddr(KNX_NETIF.getNetIf(), _staticLocalIP);
        else
            dhcp_network_changed_link_up(KNX_NETIF.getNetIf());
    }

    // lost link
    else if (!newLinkState && _currentLinkState)
    {
        _ipShown = false;
        netif_set_ipaddr(KNX_NETIF.getNetIf(), 0);
        netif_set_link_down(KNX_NETIF.getNetIf());
        loadCallbacks(false);
        logInfoP("Link disconnected");
    }

    _currentLinkState = newLinkState;
#endif

    _lastLinkCheck = millis();
}
void NetworkModule::loop(bool configured)
{
    checkLinkStatus();
    checkIpStatus();
    MDNS.update();
}

IPAddress NetworkModule::GetIpProperty(uint8_t PropertyId)
{
    uint8_t NoOfElem = 1;
    uint8_t *data;
    uint32_t length;
    knx.bau().propertyValueRead(OT_IP_PARAMETER, 0, PropertyId, NoOfElem, 1, &data, length);
    IPAddress ret = (data[3] << 24) + (data[2] << 16) + (data[1] << 8) + data[0];
    delete[] data;
    return ret;
}

void NetworkModule::SetIpProperty(uint8_t PropertyId, IPAddress IPAddress)
{
    uint8_t NoOfElem = 1;
    uint8_t data[4];

    data[0] = IPAddress[0];
    data[1] = IPAddress[1];
    data[2] = IPAddress[2];
    data[3] = IPAddress[3];

    knx.bau().propertyValueWrite(OT_IP_PARAMETER, 0, PropertyId, NoOfElem, 1, data, 0);
}

uint8_t NetworkModule::GetByteProperty(uint8_t PropertyId)
{
    uint8_t NoOfElem = 1;
    uint8_t *data;
    uint8_t ret;
    uint32_t length;
    knx.bau().propertyValueRead(OT_IP_PARAMETER, 0, PropertyId, NoOfElem, 1, &data, length);
    ret = data[0];
    delete[] data;
    return ret;
}

void NetworkModule::SetByteProperty(uint8_t PropertyId, uint8_t value)
{
    uint8_t NoOfElem = 1;
    uint8_t data[1];

    data[0] = value;

    knx.bau().propertyValueWrite(OT_IP_PARAMETER, 0, PropertyId, NoOfElem, 1, data, 0);
}

void NetworkModule::registerCallback(NetworkChangeCallback cb)
{
    _callback.push_back(cb);
}

void NetworkModule::loadCallbacks(bool state)
{
    for (int i = 0; i < _callback.size(); i++)
    {
        _callback[i](state);
    }
}

void NetworkModule::showInformations()
{
    openknx.logger.logWithPrefixAndValues("Hostname", "%s", _hostName);
    if (connected())
        openknx.logger.logWithPrefixAndValues("Network", "Established (%s)", localIP().toString().c_str());
    else
        openknx.logger.logWithPrefix("Network", "Disconnected");
}

bool NetworkModule::processCommand(const std::string cmd, bool debugKo)
{
    if (!debugKo && (cmd == "n" || cmd == "net"))
    {
        showNetworkInformations(true);
        return true;
    }

    if (!_useStaticIP && cmd == "net renew")
    {
        if (_currentLinkState) dhcp_renew(KNX_NETIF.getNetIf());
        return true;
    }
    return false;
}

void NetworkModule::showNetworkInformations(bool console)
{
    logBegin();
    if (console)
    {
        uint8_t mac[6] = {};
        macAddress(mac);

        logInfoP("Hostname: %s", _hostName);
        logInfoP("MAC-Address: %02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        logInfoP("Connection: %s", connected() ? "Established" : "Disconnected");
        logIndentUp();
    }

    if (connected())
    {
        logInfoP("IP-Address: %s", localIP().toString().c_str());
        logInfoP("Netmask: %s", gatewayIP().toString().c_str());
        logInfoP("Gateway: %s", subnetMask().toString().c_str());
        logInfoP("DNS1: %s", dns1IP().toString().c_str());
        logInfoP("DNS2: %s", dns2IP().toString().c_str());
    }

#if defined(KNX_IP_WIFI)
    logInfoP("WLAN-SSID: %s", KNX_NETIF.SSID());
#endif

    if (console)
    {
        logIndentDown();
    }
    logEnd();
}

void NetworkModule::showHelp()
{
    openknx.console.printHelpLine("net, n", "Show network informations");
    if (!_useStaticIP)
        openknx.console.printHelpLine("net renew", "Renew DHCP Address");
}

inline bool NetworkModule::connected()
{
    return KNX_NETIF.connected();
}

inline IPAddress NetworkModule::localIP()
{
    return KNX_NETIF.localIP();
}

inline IPAddress NetworkModule::subnetMask()
{
    return KNX_NETIF.subnetMask();
}

inline IPAddress NetworkModule::gatewayIP()
{
    return KNX_NETIF.gatewayIP();
}

inline IPAddress NetworkModule::dns1IP()
{
    return IPAddress(dns_getserver(0));
}

inline IPAddress NetworkModule::dns2IP()
{
    return IPAddress(dns_getserver(1));
}

inline void NetworkModule::macAddress(uint8_t *address)
{
#if defined(KNX_IP_W5500)
    memcpy(address, KNX_NETIF.getNetIf()->hwaddr, 6);
#elif defined(KNX_IP_WIFI)
    KNX_NETIF.macAddress(address);
#endif
}

NetworkModule openknxNetwork;