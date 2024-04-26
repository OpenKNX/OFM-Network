#ifndef OPENKNX_NET_IGNORE 
#include "NetworkModule.h"
#include "ModuleVersionCheck.h"

#define MDNS_DEBUG_PORT Serial
#define OPENKNX_MDNS_FULL

#ifdef KNX_IP_GENERIC
    #include <Ethernet_Generic.h>
    #include <MDNS_Generic.h>
EthernetUDP udp;
MDNS mdns(udp);
#else
    #include "LEAmDNS.h"
    #ifdef KNX_IP_W5500
        #ifdef PIN_ETH_INT
Wiznet5500lwIP KNX_NETIF(PIN_ETH_SS, ETH_SPI_INTERFACE, PIN_ETH_INT);
        #else
Wiznet5500lwIP KNX_NETIF(PIN_ETH_SS, ETH_SPI_INTERFACE);
        #endif
    #endif
WiFiUDP Udp;
#endif

#ifndef ParamNET_mDNS
    #define ParamNET_mDNS true
#endif

// TODO: Build fallback for cyw43_hal_generate_laa_mac to generate 5E:84:XX:XX:XX:XX on ESP
// extern "C" void cyw43_hal_generate_laa_mac(__unused int idx, uint8_t buf[6]);

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
    logDebugP("Initialize network adapter");
    logIndentUp();
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
    KNX_NETIF.setRstPin(PIN_ETH_RES);
    KNX_NETIF.hardreset();
    #endif
    KNX_NETIF.init(PIN_ETH_SS, &ETH_SPI_INTERFACE);
#endif

#ifdef KNX_IP_GENERIC
    if (KNX_NETIF.hardwareStatus() == EthernetNoHardware)
    {
        openknx.hardware.fatalError(7, "Error communicating with Ethernet chip");
    }
    else if (KNX_NETIF.hardwareStatus() == EthernetW5100)
    {
        logDebugP("W5100 Ethernet controller detected.");
    }
    else if (KNX_NETIF.hardwareStatus() == EthernetW5200)
    {
        logDebugP("W5200 Ethernet controller detected.");
    }
    else if (KNX_NETIF.hardwareStatus() == EthernetW5500)
    {
        logDebugP("W5500 Ethernet controller detected.");
    }
    else
    {
        openknx.hardware.fatalError(7, "Unsupported Ethernet chip");
    }
#endif
    logIndentDown();
}

void NetworkModule::loadSettings()
{
#ifdef HAS_WIFI
    #ifdef HAS_USB
    readWifiSettingsFromFile();
    #endif
#endif

    // build default hostname
    memcpy(_hostName, "OpenKNX-", 8);
    memcpy(_hostName + 8, openknx.info.humanSerialNumber().c_str() + 5, 8);
    logTraceP("Default hostname: %s", _hostName);

    // build default mac
    _mac[0] = 0x5e;
    _mac[1] = 0x84;
    uint32_t serial = htonl(openknx.info.serialNumber());
    memcpy(_mac + 2, &serial, 4);

    if (knx.configured())
    {
        // custom hostname
#ifdef ParamNET_CustomHostname
        if (ParamNET_CustomHostname)
        {
            logDebugP("Read hostname from parameters");
            memcpy(_hostName, ParamNET_HostName, 24);
        }
#endif

#if !defined(ParamNET_HostAddress) || !defined(ParamNET_SubnetMask) || !defined(ParamNET_GatewayAddress) || !defined(ParamNET_NameserverAddress) || !defined(ParamNET_StaticIP) || defined(OPENKNX_NETWORK_USEIPPROP)

        logInfoP("Read ip settings from properties");
        _staticGatewayIP = GetIpProperty(PID_DEFAULT_GATEWAY);
        _staticSubnetMask = GetIpProperty(PID_SUBNET_MASK);
        _staticLocalIP = GetIpProperty(PID_IP_ADDRESS);
        _useStaticIP = GetByteProperty(PID_IP_ASSIGNMENT_METHOD) == 1; // see 2.5.6 of 03_08_03
#else
        logInfoP("Read ip settings from parameters");
        _staticLocalIP = htonl(ParamNET_HostAddress);
        _staticSubnetMask = htonl(ParamNET_SubnetMask);
        _staticGatewayIP = htonl(ParamNET_GatewayAddress);
        _staticNameServerIP = htonl(ParamNET_NameserverAddress);
        _useStaticIP = ParamNET_StaticIP;
#endif
    }
    else
    {
        // PID_FRIENDLY_NAME is used to identify the device over Search Request from ETS. If not configured, PID_FRIENDLY_NAME is empty and so is the Name in the SearchReqest.
        // set PID_FRIENDLY_NAME to the _hostname in this case, so "OpenKNX-XXXXXX" is display in the ETS
        uint8_t NoOfElem = 30;
        uint32_t length = 0;
        uint8_t *friendlyName = new uint8_t[30];
        memcpy(friendlyName, _hostName, 25);
        knx.bau().propertyValueWrite(OT_IP_PARAMETER, 0, PID_FRIENDLY_NAME, NoOfElem, 1, friendlyName, length);
    }

    if (_useStaticIP)
    {
        SetByteProperty(PID_CURRENT_IP_ASSIGNMENT_METHOD, 1);
    }
    else
    {
        SetByteProperty(PID_IP_CAPABILITIES, 6);              // AutoIP + DHCP
        SetByteProperty(PID_CURRENT_IP_ASSIGNMENT_METHOD, 2); // ToDo
    }

#ifdef KNX_IP_GENERIC
    _mDNSDeviceServiceName = (char *)malloc(strlen(_hostName) + 14);
    snprintf(_mDNSDeviceServiceName, strlen(_hostName) + 14, "%s._device-info", _hostName);

    #ifdef OPENKNX_MDNS_FULL
    _mDNSDeviceServiceNameTXT = (char *)malloc(256);
    _mDNSHttpServiceName = (char *)malloc(strlen(_hostName) + 7);
    snprintf(_mDNSHttpServiceName, strlen(_hostName) + 7, "%s._http", _hostName);

    char buffer[50] = {};
    std::string bufferTXT = "";
    bufferTXT.reserve(50);

    snprintf(buffer, 50, "serial=%s\x0", openknx.info.humanSerialNumber().c_str());
    bufferTXT.append(1, static_cast<char>(strlen(buffer)));
    bufferTXT.append(buffer);

    snprintf(buffer, 50, "firmware=%s\x0", openknx.info.humanFirmwareNumber().c_str());
    bufferTXT.append(1, static_cast<char>(strlen(buffer)));
    bufferTXT.append(buffer);

    snprintf(buffer, 50, "version=%s\x0", openknx.info.humanFirmwareVersion().c_str());
    bufferTXT.append(1, static_cast<char>(strlen(buffer)));
    bufferTXT.append(buffer);

    snprintf(buffer, 50, "pa=%s\x0", openknx.info.humanIndividualAddress().c_str());
    bufferTXT.append(1, static_cast<char>(strlen(buffer)));
    bufferTXT.append(buffer);

    memcpy(_mDNSDeviceServiceNameTXT, bufferTXT.c_str(), bufferTXT.size());
    #else
    _mDNSDeviceServiceNameTXT = (char *)malloc(1);
    #endif
#endif
}

void NetworkModule::init()
{
    logInfoP("Initialize IP stack");
    logIndentUp();
    initPhy();
    loadSettings();
    initIp();
    logIndentDown();
}

void NetworkModule::initIp()
{

    // Hostname
    logInfoP("Hostname: %s", _hostName);
#ifdef KNX_IP_GENERIC
    KNX_NETIF.setHostname(_hostName);
#else
    KNX_NETIF.hostname(_hostName);
#endif

    if (_useStaticIP)
    {
        logInfoP("Using static IP");
    }
    else
    {
        logInfoP("Using DHCP");
    }

#if defined(KNX_IP_W5500)
    if (_useStaticIP)
    {
        if (!KNX_NETIF.config(_staticLocalIP, _staticGatewayIP, _staticSubnetMask, _staticNameServerIP))
        {
            logIndentUp();
            logErrorP("Invalid IP settings");
            logIndentDown();
        }
    }

    if (!KNX_NETIF.begin((const uint8_t *)_mac))
    {
        openknx.hardware.fatalError(7, "Error communicating with W5500 Ethernet chip");
    }
#elif defined(KNX_IP_WIFI)
    if (_useStaticIP)
    {
        KNX_NETIF.config(_staticLocalIP, _staticGatewayIP, _staticSubnetMask, _staticNameServerIP);
    }

    if (strlen(_wifiSSID) > 0)
    {
        logInfoP("Connecting to WiFi \"%s\"", _wifiSSID);
        KNX_NETIF.begin(_wifiSSID, _wifiPassphrase);
    }
    else
    {
        logErrorP("No WiFI Settings found!");
    }
#elif defined(KNX_IP_GENERIC)
    // Lan mode only for w5500 available
    if (KNX_NETIF.hardwareStatus() == EthernetW5500)
    {
        switch (ParamNET_LanMode)
        {
            case 1:
                W5100.phyMode(FULL_DUPLEX_100);
                break;
            case 2:
                W5100.phyMode(HALF_DUPLEX_100);
                break;
            case 3:
                W5100.phyMode(FULL_DUPLEX_10);
                break;
            case 4:
                W5100.phyMode(HALF_DUPLEX_10);
                break;

            default:
                break;
        }
    }

    if (_useStaticIP)
    {
        KNX_NETIF.begin(_mac, _staticLocalIP, _staticNameServerIP, _staticGatewayIP, _staticSubnetMask);
    }
    else
    {
        logInfoP("Request DHCP, please wait...");
        KNX_NETIF.begin(_mac, 5000);
        if (localIP() == IPAddress(0))
        {
            logIndentUp();
            logErrorP("Timeout");
            logIndentDown();
        }
    }
#endif
}

void NetworkModule::setup(bool configured)
{
#ifdef HAS_USB
    openknxUsbExchangeModule.onLoad("Network.txt", [this](UsbExchangeFile *file) { this->fillNetworkFile(file); });
    #ifdef HAS_WIFI
    // openknxUsbExchangeModule.onLoad("Wifi.txt", [this](UsbExchangeFile *file) { this->fillWifiFile(file); });
    // openknxUsbExchangeModule.onEject("Wifi.txt", [this](UsbExchangeFile *file) { return this->readWifiFile(file); });
    #endif
#endif

    registerCallback([this](bool state) { if (state) this->showNetworkInformations(false); });

    if (!configured || ParamNET_mDNS)
    {
#ifdef KNX_IP_GENERIC
        registerCallback([this](bool state) {
            if (state)
            {
                logDebugP("Start mDNS");
                mdns.begin(KNX_NETIF.localIP(), _hostName);
                mdns.addServiceRecord(_mDNSDeviceServiceName, -1, MDNSServiceTCP, _mDNSDeviceServiceNameTXT);
            }
            else
            {
                mdns.removeAllServiceRecords();
            }
        });
#else
        logDebugP("Start mDNS");
        if (!MDNS.begin(_hostName)) logErrorP("Hostname not applied (mDNS)");
        MDNS.addService("device-info", "tcp", -1);
    #ifdef OPENKNX_MDNS_FULL
        MDNS.addServiceTxt("device-info", "tcp", "serial", openknx.info.humanSerialNumber().c_str());
        MDNS.addServiceTxt("device-info", "tcp", "version", openknx.info.humanFirmwareVersion().c_str());
        MDNS.addServiceTxt("device-info", "tcp", "firmware", openknx.info.humanFirmwareNumber().c_str());
        MDNS.addServiceTxt("device-info", "tcp", "pa", openknx.info.humanIndividualAddress().c_str());
    #endif
        registerCallback([this](bool state) { if (state) MDNS.notifyAPChange(); });
#endif
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

#ifdef HAS_USB
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
        writeLineToFile(file, "DNS: %s", nameServerIP().toString().c_str());
        writeLineToFile(file, "Mode: %s", phyMode().c_str());
    }
}
    #ifdef HAS_WIFI
void NetworkModule::readWifiSettingsFromFile()
{
    logInfoP("Read Wifi settings from WIFI.TXT");
    logIndentUp();

    File file = LittleFS.open("/WIFI.TXT", "r");
    if (!file)
    {
        logErrorP("File /WIFI.TXT not found!");
        logIndentDown();
        return;
    }

    int pos = 0;
    char buffer[100] = {};
    uint8_t found = 0;
    while (file.available())
    {
        uint8_t value = 0;
        file.read(&value, 1);

        // Copy char
        if (value != '\n' && value != '\r')
        {
            buffer[pos] = value;
            pos++;
        }

        if (value == '\n' || value == '\r' || !file.available())
        {
            if (pos > 0)
            {
                found++;
                if (found == 1) memcpy(_wifiSSID, buffer, pos);
                if (found == 2) memcpy(_wifiPassphrase, buffer, pos);
                logTraceP("%i -> %i: %s", found, pos, buffer);

                // Reset buffer
                memset(buffer, 0x0, 100);
                pos = 0;
            }
        }

        // ssid and passphrase found
        if (found >= 2) break;
    }

    if (found > 0) logDebugP("SSID: %s", _wifiSSID);
    if (found == 2) logDebugP("Passphrase with %i chars", strlen(_wifiPassphrase));
    if (found == 1) logErrorP("No wifi phassphrase found");

    logIndentDown();
}
    #endif
#endif

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
    if (!delayCheckMillis(_lastLinkCheck, 500)) return;

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

    if (_currentLinkState) checkIpStatus();

    // set network bit for heartbeat
    if (established())
        openknx.common.extendedHeartbeatValue |= 0b10000000;
    else
        openknx.common.extendedHeartbeatValue &= 0b01111111;
}

void NetworkModule::loop(bool configured)
{
    if (_powerSave) return;
    checkLinkStatus();

    if (!configured || ParamNET_mDNS) handleMDNS();
}

void NetworkModule::handleMDNS()
{
#ifdef KNX_IP_GENERIC
    mdns.run();
#else
    MDNS.update();
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

#ifdef HAS_WIFI
    else if (cmd == "net recon" && strlen(_wifiSSID) > 0)
    {
        logInfoP("Connecting to WiFi \"%s\"", _wifiSSID);
        // KNX_NETIF.disconnect();
        KNX_NETIF.begin(_wifiSSID, _wifiPassphrase);
        return true;
    }
#else
    else if (!_useStaticIP && cmd == "net renew")
    {
        if (!connected())
        {
            logErrorP("not connected");
            return true;
        }

    #ifdef KNX_IP_GENERIC
        KNX_NETIF.maintain();
    #else
        dhcp_renew(KNX_NETIF.getNetIf());
    #endif
        return true;
    }
#endif

    return false;
}

void NetworkModule::showNetworkInformations(bool console)
{
    openknx.common.skipLooptimeWarning();
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
        logInfoP("Netmask: %s", subnetMask().toString().c_str());
        logInfoP("Gateway: %s", gatewayIP().toString().c_str());
        logInfoP("DNS: %s", nameServerIP().toString().c_str());
        logInfoP("Mode: %s", phyMode().c_str());
    }

#ifdef HAS_WIFI
    std::string wifiInfo = std::string(_wifiSSID) + " (" + std::to_string(KNX_NETIF.RSSI()) + "dBm)";
    logInfoP("Wifi: %s", wifiInfo.c_str());
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

#ifdef HAS_WIFI
    if (strlen(_wifiSSID) > 0) openknx.console.printHelpLine("net recon", "Reconnect to network");
#else
    if (!_useStaticIP) openknx.console.printHelpLine("net renew", "Renew DHCP Address");
#endif
}

// Link status
inline bool NetworkModule::connected()
{
    if (_powerSave) return false;

#if defined(KNX_IP_W5500)
    return KNX_NETIF.isLinked();
#elif defined(KNX_IP_WIFI)
    return KNX_NETIF.isConnected();
#elif defined(KNX_IP_GENERIC)
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

inline IPAddress NetworkModule::nameServerIP()
{
#if defined(KNX_IP_W5500) || defined(KNX_IP_WIFI)
    return IPAddress(dns_getserver(0));
#elif defined(KNX_IP_GENERIC)
    return KNX_NETIF.dnsServerIP();
#endif
}

inline std::string NetworkModule::phyMode()
{
#ifdef KNX_IP_GENERIC
    std::string mode = std::to_string(KNX_NETIF.speed()) + " MBit/s";
    switch (KNX_NETIF.duplex())
    {
        case 1:
            mode.append(" (Half duplex)");
            break;
        case 2:
            mode.append("");
            break;
    }

    return mode;
#else
    return "Auto";
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

void NetworkModule::savePower()
{
    _powerSave = true;
#if defined(PIN_ETH_RES)
    digitalWrite(PIN_ETH_RES, LOW);
#endif
}

bool NetworkModule::restorePower()
{
    delay(1000);
    return false;
}

NetworkModule openknxNetwork;
#endif