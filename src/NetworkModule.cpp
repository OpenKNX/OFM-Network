#include "NetworkModule.h"

#define MDNS_DEBUG_PORT Serial

#ifdef KNX_IP_WIFI
    // TODO WLAN
    #pragma error "Implementation for WiFi missing"
#endif

#ifdef KNX_IP_GENERIC
    #include <Ethernet_Generic.h>
    #include <MDNS_Generic.h>
EthernetUDP udp;
MDNS mdns(udp);
#else
    #ifdef KNX_IP_W5500
Wiznet5500lwIP KNX_NETIF(PIN_ETH_SS, ETH_SPI_INTERFACE);
    #endif
WiFiUDP Udp;
#endif

extern "C" void cyw43_hal_generate_laa_mac(__unused int idx, uint8_t buf[6]);

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
#if defined(ETH_SPI_INTERFACE)
    // initalize SPI
    ETH_SPI_INTERFACE.setRX(PIN_ETH_MISO);
    ETH_SPI_INTERFACE.setTX(PIN_ETH_MOSI);
    ETH_SPI_INTERFACE.setSCK(PIN_ETH_SCK);
    ETH_SPI_INTERFACE.setCS(PIN_ETH_SS);
    logDebugP("Ethernet SPI GPIO: RX/MISO: %d, TX/MOSI: %d, SCK/SCLK: %d, CSn/SS: %d", PIN_ETH_MISO, PIN_ETH_MOSI, PIN_ETH_SCK, PIN_ETH_SS);
#endif

#ifdef KNX_IP_GENERIC
    #ifdef PIN_ETH_RES
    Ethernet.setRstPin(PIN_ETH_RES);
    Ethernet.hardreset();
    #endif
    Ethernet.init(PIN_ETH_SS);
#endif
}

void NetworkModule::prepareSettings()
{
    // build hostname
    _hostName = (char *)malloc(25);
    memset(_hostName, 0, 25);
    memcpy(_hostName, "OpenKNX-", 8);
    memcpy(_hostName + 8, openknx.info.humanSerialNumber().c_str() + 5, 8);

    // build mac
    cyw43_hal_generate_laa_mac(0, _mac);

#if defined(KNX_IP_GENERIC)
    _mDNSHttpServiceName = (char *)malloc(strlen(_hostName) + 7);
    _mDNSDeviceServiceName = (char *)malloc(strlen(_hostName) + 14);
    snprintf(_mDNSHttpServiceName, strlen(_hostName) + 7, "%s._http", _hostName);
    snprintf(_mDNSDeviceServiceName, strlen(_hostName) + 14, "%s._device-info", _hostName);
#endif

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
    #ifdef ParamNET_NameserverAddress1
    _staticDnsIP = htonl(ParamNET_NameserverAddress1);
    #else
    _staticDnsIP = htonl(ParamNET_NameserverAddress);
    #endif
    _useStaticIP = ParamNET_StaticIP;
#endif
}

void NetworkModule::init()
{
    logInfoP("Init IP Stack");
    logIndentUp();

    initPhy();
    prepareSettings();

    if (_useStaticIP)
    {
        logInfoP("Using static IP");
        logIndentUp();
#if defined(KNX_IP_W5500) || defined(KNX_IP_WIFI)
        if (!KNX_NETIF.config(_staticLocalIP, _staticGatewayIP, _staticSubnetMask, _staticDnsIP))
        {
            logErrorP("Invalid IP settings");
        }
#endif
        logIndentDown();
        SetByteProperty(PID_CURRENT_IP_ASSIGNMENT_METHOD, 1);
    }
    else
    {
        logInfoP("Using DHCP");
        SetByteProperty(PID_IP_CAPABILITIES, 6);              // AutoIP + DHCP
        SetByteProperty(PID_CURRENT_IP_ASSIGNMENT_METHOD, 2); // ToDo
    }

    logInfoP("Hostname: %s", _hostName);
    logIndentUp();

#if defined(KNX_IP_W5500) || defined(KNX_IP_WIFI)
    if (!KNX_NETIF.hostname(_hostName))
    {
        logErrorP("Hostname not applied");
    }
#elif defined(KNX_IP_GENERIC)
    KNX_NETIF.setHostname(_hostName);
#endif
    logIndentDown();

#if defined(KNX_IP_W5500)
    if (!KNX_NETIF.begin((const uint8_t *)_mac))
    {
        openknx.hardware.fatalError(7, "Error communicating with W5500 Ethernet chip");
    }
#elif defined(KNX_IP_GENERIC)
    Ethernet.begin(_mac, &ETH_SPI_INTERFACE, 1000);

    if (Ethernet.hardwareStatus() == EthernetNoHardware)
    {
        openknx.hardware.fatalError(7, "Error communicating with Ethernet chip");
    }
    else if (Ethernet.hardwareStatus() == EthernetW5100)
    {
        logDebugP("W5100 Ethernet controller detected.");
    }
    else if (Ethernet.hardwareStatus() == EthernetW5200)
    {
        logDebugP("W5200 Ethernet controller detected.");
    }
    else if (Ethernet.hardwareStatus() == EthernetW5500)
    {
        logDebugP("W5500 Ethernet controller detected.");
    }
#endif

    logIndentDown();
}

void NetworkModule::setup(bool configured)
{
    openknxUsbExchangeModule.onLoad("Network.txt", [this](UsbExchangeFile *file) { this->fillNetworkFile(file); });

    registerCallback([this](bool state) { if (state) this->showNetworkInformations(false); });
#ifdef KNX_IP_GENERIC
    registerCallback([this](bool state) {
        if (state)
        {
            logInfoP("Start mDNS %s, %s,", _mDNSHttpServiceName, _mDNSDeviceServiceName);
            mdns.begin(KNX_NETIF.localIP(), _hostName);
            mdns.addServiceRecord(_mDNSHttpServiceName, 80, MDNSServiceTCP);
            mdns.addServiceRecord(_mDNSDeviceServiceName, -1, MDNSServiceTCP);
        }
        else
        {
            logInfoP("Stop mDNS");
            mdns.removeAllServiceRecords();
        }
    });
#endif
    //
    // mdns.addServiceRecord("_http", 80, MDNSServiceTCP);
    // if (!MDNS.begin(_hostName)) logErrorP("Hostname not applied (mDNS)");
    // MDNS.addService("http", "tcp", 80);
    // MDNS.addService("device-info", "tcp", -1);
    // MDNS.addServiceTxt("device-info", "tcp", "serial", openknx.info.humanSerialNumber().c_str());
    // MDNS.addServiceTxt("device-info", "tcp", "firmware", openknx.info.humanFirmwareVersion().c_str());

#ifndef ParamNET_mDNS
    #define ParamNET_mDNS true
#endif
    if (!configured || ParamNET_mDNS)
    {
        // logInfoP("MDNS");
        // mdns.addServiceRecord("test._device-info", 0, MDNSServiceTCP);
        // mdns.addServiceRecord("test._http", 80, MDNSServiceTCP);
        // mdns.begin(Ethernet.localIP(), _hostName);

        //         if (!MDNS.begin(_hostName)) logErrorP("Hostname not applied (mDNS)");
        //         MDNS.addService("http", "tcp", 80);
        //         MDNS.addService("device-info", "tcp", -1);
        //         MDNS.addServiceTxt("device-info", "tcp", "serial", openknx.info.humanSerialNumber().c_str());
        //         MDNS.addServiceTxt("device-info", "tcp", "firmware", openknx.info.humanFirmwareVersion().c_str());
        // #ifdef HARDWARE_NAME
        //         MDNS.addServiceTxt("device-info", "tcp", "hardware", HARDWARE_NAME);
        // #endif
        //         if (configured)
        //         {
        //             MDNS.addServiceTxt("device-info", "tcp", "address", openknx.info.humanIndividualAddress().c_str());
        //             MDNS.addServiceTxt("device-info", "tcp", "application", openknx.info.humanApplicationVersion().c_str());
        //         }
        //         registerCallback([this](bool state) { if (state) MDNS.notifyAPChange(); });
    }

    // NTP.begin("pool.ntp.org", "time.nist.gov");
    // NTP.waitSet(3000);
    // logInfoP("NTP done");

    // time_t now = time(nullptr);
    // struct tm timeinfo;
    // gmtime_r(&now, &timeinfo);
    // logInfoP("Time: %i",now);
    // logInfoP("Time: %s", asctime(&timeinfo));
}

void NetworkModule::fillNetworkFile(UsbExchangeFile *file)
{
    writeLineToFile(file, "OpenKNX Network");
    writeLineToFile(file, "-----------------");
    writeLineToFile(file, "");
    writeLineToFile(file, "Hostname: %s", _hostName);
    writeLineToFile(file, "Network: %s", established() ? "Established" : "Disconnected");
    if (established())
    {
        writeLineToFile(file, "IP-Address: %s", localIP().toString().c_str());
        writeLineToFile(file, "Netmask: %s", subnetMask().toString().c_str());
        writeLineToFile(file, "Gateway: %s", gatewayIP().toString().c_str());
        writeLineToFile(file, "DNS: %s", dnsIP().toString().c_str());
    }
}

void NetworkModule::checkIpStatus()
{
    if (_ipShown || !established()) return;
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

    // Get current link state
    bool newLinkState = connected();

    // got link
    if (newLinkState && !_currentLinkState)
    {
        logInfoP("Link connected");
#if defined(KNX_IP_W5500)
        netif_set_link_up(KNX_NETIF.getNetIf());
        if (_useStaticIP)
            netif_set_ipaddr(KNX_NETIF.getNetIf(), _staticLocalIP);
        else
            dhcp_network_changed_link_up(KNX_NETIF.getNetIf());
#elif defined(KNX_IP_GENERIC)
        if (!established()) KNX_NETIF.maintain();
#endif
    }

    // lost link
    else if (!newLinkState && _currentLinkState)
    {
        _ipShown = false;
#if defined(KNX_IP_W5500)
        netif_set_ipaddr(KNX_NETIF.getNetIf(), 0);
        netif_set_link_down(KNX_NETIF.getNetIf());
#endif
        loadCallbacks(false);
        logInfoP("Link disconnected");
    }

    _currentLinkState = newLinkState;
    _lastLinkCheck = millis();

    logIndentUp();
    if (_currentLinkState) checkIpStatus();
    logIndentDown();
}

void NetworkModule::loop(bool configured)
{
    checkLinkStatus();

    if (!configured || ParamNET_mDNS)
        handleMDNS();
}

void NetworkModule::handleMDNS()
{
#ifdef KNX_IP_GENERIC
    mdns.run();
#else
#endif
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
    if (established())
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
#if defined(KNX_IP_W5500) || defined(KNX_IP_WIFI)
    if (!_useStaticIP && cmd == "net renew")
    {
        if (_currentLinkState) dhcp_renew(KNX_NETIF.getNetIf());
        return true;
    }
#endif
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
        logInfoP("Connection: %s", established() ? "Established" : "Disconnected");
        logIndentUp();
    }

    if (established())
    {
        logInfoP("IP-Address: %s", localIP().toString().c_str());
        logInfoP("Netmask: %s", gatewayIP().toString().c_str());
        logInfoP("Gateway: %s", subnetMask().toString().c_str());
        logInfoP("DNS: %s", dnsIP().toString().c_str());
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

// Link status
inline bool NetworkModule::connected()
{
#if defined(KNX_IP_W5500)
    return KNX_NETIF.isLinked();
#else
    return KNX_NETIF.linkStatus() == LinkON;
#endif
}

// IP Status
inline bool NetworkModule::established()
{
    if (!connected()) return false;

    return localIP() != IPAddress(0);
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

inline IPAddress NetworkModule::dnsIP()
{
#if defined(KNX_IP_W5500) || defined(KNX_IP_WIFI)
    return IPAddress(dns_getserver(0));
#elif defined(KNX_IP_GENERIC)
    return KNX_NETIF.dnsServerIP();
#endif
}

inline void NetworkModule::macAddress(uint8_t *address)
{
#if defined(KNX_IP_W5500)
    memcpy(address, KNX_NETIF.getNetIf()->hwaddr, 6);
#elif defined(KNX_IP_WIFI)
    KNX_NETIF.macAddress(address);
#elif defined(KNX_IP_GENERIC)
    KNX_NETIF.MACAddress(address);
#endif
}

NetworkModule openknxNetwork;