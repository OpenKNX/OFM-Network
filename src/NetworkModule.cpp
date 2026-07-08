#if defined(KNX_IP_WIFI) || defined(KNX_IP_LAN)
#include <ArduinoOTA.h>
#include <string>

#include "ModuleVersionCheck.h"
#include "NetworkModule.h"
#include "NtpTimeProvider.h"

#ifndef ParamNET_mDNS
#define ParamNET_mDNS true
#endif

#if defined(ARDUINO_ARCH_ESP32)
#if defined(ESP_IDF_VERSION) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#define CALLBACK_CLASS Network
#define CALLBACK_EVENT arduino_event_id_t
#else
#define CALLBACK_CLASS WiFi
#define CALLBACK_EVENT WiFiEvent_t
#endif

#if defined(KNX_IP_LAN)
#include <ETH.h>
#define KNX_NETIF ETH
#elif defined(KNX_IP_WIFI)
#define KNX_NETIF WiFi
#else
#pragma warn "Missing KNX_IP_LAN or KNX_IP_WIFI"
#endif
#elif defined(ARDUINO_ARCH_RP2040)
#include <SimpleMDNS.h>

#if defined(KNX_IP_LAN)
#include "W5500lwIP.h"
#ifdef PIN_ETH_INT
Wiznet5500lwIP KNX_NETIF(PIN_ETH_SS, ETH_SPI_INTERFACE, PIN_ETH_INT);
#else
Wiznet5500lwIP KNX_NETIF(PIN_ETH_SS, ETH_SPI_INTERFACE);
#endif
#elif defined(KNX_IP_WIFI)
#else
#pragma warn "Missing KNX_IP_LAN or KNX_IP_WIFI"
#endif

WiFiUDP Udp;
#else
#pragma warn "Unsupported platform"
#endif

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

/*
 * Internal use: Developer only
 */
void NetworkModule::resetNetwork()
{
    logInfoP("Reset network adapter");
    logIndentUp();
    controlKnxIp(false);

#if defined(ARDUINO_ARCH_RP2040) && defined(KNX_IP_LAN)
    _ethLink.resetLink(); // bounce the W5500 PHY (like a cable re-plug): no reboot, no driver end()/begin()
    logIndentDown();
    return;
#endif

#if defined(ARDUINO_ARCH_ESP32) && defined(KNX_IP_LAN) && defined(OPENKNX_ETH_AUTO_FALLBACK)
    _ethLink.resetLadder(); // restart the auto-fallback search (the re-init below re-links at autoneg)
#endif

#if defined(KNX_IP_WIFI) && defined(ARDUINO_ARCH_ESP32)
    KNX_NETIF.disconnect(true, true);
    KNX_NETIF.mode(WIFI_OFF);
#else
    KNX_NETIF.end();
#endif
    delay(500);
    initPhy();
    initIp();
    controlKnxIp(true);

    logInfoP("Complete");
    logIndentDown();
}

void NetworkModule::initPhy()
{
    logDebugP("Initialize network adapter");
    logIndentUp();
#if defined(PIN_ETH_RES)
    logDebugP("Resetting Ethernet Phy...");
    pinMode(PIN_ETH_RES, OUTPUT);
    digitalWrite(PIN_ETH_RES, LOW);
    delay(600);
    digitalWrite(PIN_ETH_RES, HIGH);
#endif

#if defined(ETH_SPI_INTERFACE)
    // initialize SPI
    ETH_SPI_INTERFACE.setRX(PIN_ETH_MISO);
    ETH_SPI_INTERFACE.setTX(PIN_ETH_MOSI);
    ETH_SPI_INTERFACE.setSCK(PIN_ETH_SCK);
    ETH_SPI_INTERFACE.setCS(PIN_ETH_SS);
    logDebugP("Ethernet SPI GPIO: RX/MISO: %d, TX/MOSI: %d, SCK/SCLK: %d, CSn/SS: %d", PIN_ETH_MISO, PIN_ETH_MOSI, PIN_ETH_SCK, PIN_ETH_SS);
#endif

    logIndentDown();
}

void NetworkModule::loadSettings()
{
#ifdef KNX_IP_WIFI
    readWifiSettings();
#endif

    // build default hostname
    memcpy(_hostName, "OpenKNX-", 8);
    memcpy(_hostName + 8, openknx.info.humanSerialNumber().c_str() + 5, 8);
    logTraceP("Default hostname: %s", _hostName);

    if (knx.configured())
    {
        // custom hostname
#ifdef ParamNET_CustomHostname
        if (ParamNET_CustomHostname && strlen((char *)ParamNET_HostName) > 0)
        {
            logDebugP("Read hostname from parameters");
            memcpy(_hostName, ParamNET_HostName, 24);
        }
#endif

#if !defined(ParamNET_HostAddress) || !defined(ParamNET_SubnetMask) || !defined(ParamNET_GatewayAddress) || !defined(ParamNET_NameserverAddress) || !defined(ParamNET_StaticIP) || defined(OPENKNX_NETWORK_USEIPPROP)

        logInfoP("Read ip settings from properties");
        _useStaticIP = GetByteProperty(PID_IP_ASSIGNMENT_METHOD) == 1; // see 2.5.6 of 03_08_03
        if (_useStaticIP)
        {
            _staticGatewayIP = GetIpProperty(PID_DEFAULT_GATEWAY);
            _staticSubnetMask = GetIpProperty(PID_SUBNET_MASK);
            _staticLocalIP = GetIpProperty(PID_IP_ADDRESS);
        }
#else
        logInfoP("Read ip settings from parameters");
        _useStaticIP = ParamNET_StaticIP;
        if (_useStaticIP)
        {
            _staticLocalIP = htonl(ParamNET_HostAddress);
            _staticSubnetMask = htonl(ParamNET_SubnetMask);
            _staticGatewayIP = htonl(ParamNET_GatewayAddress);
            _staticNameServerIP = htonl(ParamNET_NameserverAddress);
        }
#endif
    }
    else
    {
        // PID_FRIENDLY_NAME is used to identify the device over Search Request from ETS. If not configured, PID_FRIENDLY_NAME is empty and so is the Name in the SearchReqest.
        // set PID_FRIENDLY_NAME to the _hostname in this case, so "OpenKNX-XXXXXX" is display in the ETS
        // since the friendlyName can be set while being unconfigured (is set while assigning PA), check if 0
        uint8_t elements = 30;               // request 30 items - update by read
        uint32_t length = 0;                 // update by read
        uint8_t *friendlyNameRead = nullptr; // new init by read
        knx.bau().propertyValueRead(OT_IP_PARAMETER, 0, PID_FRIENDLY_NAME, elements, 1, &friendlyNameRead, length);
        if (elements == 0)
        {
            uint8_t friendlyNameWrite[30] = {};
            memcpy(friendlyNameWrite, _hostName, strlen(_hostName));
            elements = 30;
            length = 0; // will update by write
            knx.bau().propertyValueWrite(OT_IP_PARAMETER, 0, PID_FRIENDLY_NAME, elements, 1, friendlyNameWrite, 0);
        }
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

#ifdef KNX_IP_WIFI
    if (strlen(_wifiSSID) == 0)
        logErrorP("No WiFI Settings found!");
#endif

    logInfoP(_useStaticIP ? "Using static IP" : "Using DHCP");

    // Hostname
    logInfoP("Hostname: %s", _hostName);
#if defined(ARDUINO_ARCH_ESP32)
    KNX_NETIF.setHostname(_hostName);
#else
    KNX_NETIF.hostname(_hostName);
#endif

#ifdef ARDUINO_ARCH_ESP32
    CALLBACK_CLASS.onEvent([](CALLBACK_EVENT event) -> void { openknxNetwork.esp32NetworkEvent(event); });
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

#ifdef ARDUINO_ARCH_ESP32
void NetworkModule::esp32NetworkEvent(CALLBACK_EVENT event)
{
    switch (event)
    {
        case ARDUINO_EVENT_WIFI_READY:
        case ARDUINO_EVENT_WIFI_STA_START:
            // do nothing
            break;
        case ARDUINO_EVENT_ETH_START:
        case ARDUINO_EVENT_WIFI_AP_START:
            logDebugP("Event: Start");
            // The hostname must be set after the interface is started, but needs
            // to be set before DHCP, so set it from the event handler thread.
            KNX_NETIF.setHostname(_hostName);
            controlKnxIp(false);

            break;
        case ARDUINO_EVENT_ETH_CONNECTED:
        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            logDebugP("Event: Connected");
            espConnected = true;
            break;
        case ARDUINO_EVENT_ETH_GOT_IP:
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            logDebugP("Event: Got IP");
            controlKnxIp(true);
            break;
        case ARDUINO_EVENT_ETH_DISCONNECTED:
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            logDebugP("Event: Disconnected");
            controlKnxIp(false);
            espConnected = false;
            break;
        case ARDUINO_EVENT_ETH_STOP:
        case ARDUINO_EVENT_WIFI_STA_STOP:
            logDebugP("Event: Stop");
            controlKnxIp(false);
            espConnected = false;
            break;
        default:
            logDebugP("Event: Ignored %i", event);
            break;
    }
}
#endif

void NetworkModule::initIp()
{
#ifdef ARDUINO_ARCH_ESP32
#ifdef KNX_IP_WIFI
    KNX_NETIF.config(_staticLocalIP, _staticGatewayIP, _staticSubnetMask, _staticNameServerIP);
    KNX_NETIF.mode(WIFI_AP_STA);
    KNX_NETIF.setAutoReconnect(true);
    if (strlen(_wifiSSID) > 0)
        KNX_NETIF.begin(_wifiSSID, _wifiPassphrase);
    else
        KNX_NETIF.begin();
#else
    // ETH link mode: auto-negotiation by default. A fixed speed can be forced (autoneg off) either at
    // build time (-D OPENKNX_ETH_FORCE_SPEED=10|100 [+ -D OPENKNX_ETH_FORCE_FULLDUPLEX]) or at runtime
    // via the console (`net eth 10|100 [half|full]`, persisted in NVS). Must run before begin().
    // Workaround for cheap/unmanaged switches that flap on 100M autoneg with the W5500.
#if defined(KNX_IP_LAN)
    _ethLink.applyBeforeBegin();
#endif
    KNX_NETIF.begin();
    KNX_NETIF.config(_staticLocalIP, _staticGatewayIP, _staticSubnetMask, _staticNameServerIP); // must run after begin(): applies to the started interface (a static IP here also disables DHCP)
#endif

#elif ARDUINO_ARCH_RP2040
#ifdef KNX_IP_WIFI
    if (strlen(_wifiSSID) > 0)
    {
        KNX_NETIF.config(_staticLocalIP, _staticNameServerIP, _staticGatewayIP, _staticSubnetMask);
        logInfoP("Connecting to WiFi \"%s\"", _wifiSSID);
        KNX_NETIF.setTimeout(2000); // 2000 ms = 4 seconds (timeout is multiplied by 2)
        KNX_NETIF.begin(_wifiSSID, _wifiPassphrase);
    }
#else

    if (!KNX_NETIF.config(_staticLocalIP, _staticGatewayIP, _staticSubnetMask, _staticNameServerIP))
    {
        logIndentUp();
        logErrorP("Invalid IP settings");
        logIndentDown();
    }

    KNX_NETIF.setSPISpeed(OPENKNX_NET_SPI_SPEED);
    if (!KNX_NETIF.begin())
        openknx.hardware.fatalError(FATAL_NETWORK, "Error communicating with W5500 Ethernet chip");
#endif
#endif
}

void NetworkModule::setup(bool configured)
{
#ifdef HAS_USB
    openknxUsbExchangeModule.onLoad("Network.txt", [this](UsbExchangeFile *file) { this->fillNetworkFile(file); });
#ifdef KNX_IP_WIFI
    // TODO: USB Wifi.txt provisioning not implemented (fillWifiFile/readWifiFile don't exist) -> would break RP2040 WiFi builds
    // openknxUsbExchangeModule.onLoad("Wifi.txt", [this](UsbExchangeFile *file) { this->fillWifiFile(file); });
    // openknxUsbExchangeModule.onEject("Wifi.txt", [this](UsbExchangeFile *file) { return this->readWifiFile(file); });
#endif
#endif

    registerCallback([this](bool state) { if (state) this->showNetworkInformations(false); });

    _ipLedFunc = openknx.ledFunctions.get(OPENKNX_LEDFUNC_NET_STATE);

    if (!configured || ParamNET_mDNS)
    {
        logDebugP("Start mDNS");
        if (!MDNS.begin(_hostName)) logErrorP("Hostname not applied (mDNS)");

#ifdef ARDUINO_ARCH_ESP32
        MDNS.addService("openknx", "tcp", -1);
        MDNS.addServiceTxt("openknx", "tcp", "serial", openknx.info.humanSerialNumber().c_str());
        MDNS.addServiceTxt("openknx", "tcp", "version", openknx.info.humanFirmwareVersion().c_str());
        MDNS.addServiceTxt("openknx", "tcp", "firmware", openknx.info.humanFirmwareNumber().c_str());
        MDNS.addServiceTxt("openknx", "tcp", "address", openknx.info.humanIndividualAddress().c_str());
        MDNS.addServiceTxt("openknx", "tcp", "ota", _otaPortString);
        MDNS.addServiceTxt("openknx", "tcp", "configured", knx.configured() ? "1" : "0");
#else
        hMDNSService service = MDNS.addService("openknx", "tcp", -1);
        MDNS.addServiceTxt(service, "serial", openknx.info.humanSerialNumber().c_str());
        MDNS.addServiceTxt(service, "version", openknx.info.humanFirmwareVersion().c_str());
        MDNS.addServiceTxt(service, "firmware", openknx.info.humanFirmwareNumber().c_str());
        MDNS.addServiceTxt(service, "address", openknx.info.humanIndividualAddress().c_str());
        MDNS.addServiceTxt(service, "ota", _otaPortString);
        MDNS.addServiceTxt(service, "configured", (uint8_t)knx.configured());
#endif
        MDNS.enableArduino(_otaPort /* default port for ota */, false /* AUTH true / false */);
    }

#ifdef ParamNET_NTP
    if (ParamNET_NTP)
    {
        openknx.time.setTimeProvider(new NtpTimeProvider());
    }
#endif

#ifdef ARDUINO_ARCH_ESP32
    ArduinoOTA.setMdnsEnabled(false); // handle global
#endif
    ArduinoOTA.setPort(_otaPort);
    ArduinoOTA.setRebootOnSuccess(false);
    ArduinoOTA.onStart([&]() {
        if (ArduinoOTA.getCommand() == U_FLASH)
            logInfo("OTA", "Start updating firmware");
        else // U_SPIFFS
            logInfo("OTA", "Start updating filesystem");
    });
    ArduinoOTA.onEnd([&]() {
        logIndentUp();
        logInfo("OTA", "Update complete");
        logIndentDown();
        openknx.restart();
    });
    ArduinoOTA.onProgress([&](unsigned int progress, unsigned int total) {
        int percent = (int)progress / (total / 100.0);
        if (percent % 10 == 0 && _otaProgress != percent)
        {
            logIndentUp();
            logInfo("OTA", "Progress: %d%%", percent);
            logIndentDown();
            _otaProgress = percent;
        }
        openknx.loop();
    });
    ArduinoOTA.onError([&](ota_error_t error) {
        logIndentUp();
        if (error == OTA_AUTH_ERROR)
            logError("OTA", "Auth error");
        else if (error == OTA_BEGIN_ERROR)
            logError("OTA", "Begin error");
        else if (error == OTA_CONNECT_ERROR)
            logError("OTA", "Connect error");
        else if (error == OTA_RECEIVE_ERROR)
            logError("OTA", "Receive error");
        else if (error == OTA_END_ERROR)
            logError("OTA", "End error");
        logIndentDown();
    });
}

#ifdef HAS_USB
void NetworkModule::fillNetworkFile(UsbExchangeFile *file)
{
    writeLineToFile(file, "OpenKNX Network");
    writeLineToFile(file, "-----------------");
    writeLineToFile(file, "");
    writeLineToFile(file, "Hostname: %s", _hostName);
    writeLineToFile(file, "Network: %s", established() ? "Established" : "Disconnected");
    writeLineToFile(file, _useStaticIP ? "Using static IP" : "Using DHCP");
    if (established())
    {
        writeLineToFile(file, "IP-Address: %s", localIP().toString().c_str());
        writeLineToFile(file, "Netmask: %s", subnetMask().toString().c_str());
        writeLineToFile(file, "Gateway: %s", gatewayIP().toString().c_str());
        writeLineToFile(file, "DNS: %s", nameServerIP().toString().c_str());
    }
}
#endif

// Only announce the connection once it has been stable this long, so a flapping link (e.g. during the
// ETH auto-fallback search) does not spam "Network established" + IP info on every brief reconnect.
static constexpr uint32_t NET_INFO_STABLE_MS = 3000;

void NetworkModule::checkIpStatus()
{
    if (_ipShown || !established()) return;

    // Debounce: wait until the link has genuinely held before printing, instead of on every brief
    // establish during flapping. Shown a few seconds after a stable connect - not only when locked.
    if (_ipStableSince == 0) _ipStableSince = millis();
    if ((uint32_t)(millis() - _ipStableSince) < NET_INFO_STABLE_MS) return;

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

    // Get current network state
    bool establishedState = established();
    bool newLinkState;
    if (establishedState)
        newLinkState = true;
    else
        newLinkState = connected();

    if (newLinkState && established())
    {
        if (_ipLedState != 1)
        {
            _ipLedFunc->color(OpenKNX::Led::Color::Green);
            _ipLedFunc->activity(OpenKNX::Led::g_ipLedActivity, true);

            _ipLedState = 1;
        }
    }
    else if (newLinkState)
    {
        if (_ipLedState != 2)
        {
            _ipLedFunc->color(OpenKNX::Led::Color::Yellow);
            _ipLedFunc->on(OpenKNX::Led::Capability::COLOR);
            _ipLedFunc->blinking(1000, OpenKNX::Led::Capability::MONOCHROME);

            _ipLedState = 2;
        }
    }
    else
    {
#ifdef KNX_IP_WIFI
        if (_wifiSSID[0] == 0 || _wifiPassphrase[0])
        {
            if (_ipLedState != 4)
            {
                _ipLedFunc->color(OpenKNX::Led::Color::Red);
                _ipLedFunc->blinking(500);
                _ipLedState = 4;
            }
        }
        else
#endif
            if (_ipLedState != 3)
        {

            _ipLedFunc->color(OpenKNX::Led::Color::Red);
            _ipLedFunc->on(OpenKNX::Led::Capability::COLOR);
            _ipLedFunc->off(OpenKNX::Led::Capability::MONOCHROME);
            _ipLedState = 3;
        }
    }

    // got link
    if (newLinkState && !_currentLinkState)
    {
        logInfoP("Link connected");
    }

    // lost link
    else if (!newLinkState && _currentLinkState)
    {
        _ipShown = false;
        _ipStableSince = 0; // restart the establish debounce on link loss
        loadCallbacks(false);
        logInfoP("Link disconnected");
    }

    _currentLinkState = newLinkState;
    _lastLinkCheck = millis();

#if defined(KNX_IP_LAN) && (defined(ARDUINO_ARCH_RP2040) || defined(ARDUINO_ARCH_ESP32)) && defined(OPENKNX_ETH_AUTO_FALLBACK)
    _ethLink.tick(newLinkState, establishedState);
#endif

    if (_currentLinkState) checkIpStatus();

    // set network bit for heartbeat
    if (established())
        openknx.common.extendedHeartbeatValue |= 0b10000000;
    else
        openknx.common.extendedHeartbeatValue &= 0b01111111;
}

void NetworkModule::loop(bool configured)
{
    if (_restartTimer > 0 && delayCheck(_restartTimer, 2000))
    {
        openknx.restart();
    }

    if (_powerSave) return;

    checkLinkStatus();
    handleOTA();
}

void NetworkModule::handleOTA()
{
    bool allowed = true;
    if (ParamNET_OTAUpdate == 2) allowed = false;
    if (ParamNET_OTAUpdate == 0) allowed = knx.progMode();

    if (_otaAllowed != allowed) // allowed changed
    {
        _otaAllowed = allowed;
        if (_otaAllowed)
#ifdef ARDUINO_ARCH_ESP32
            ArduinoOTA.begin();
#else
            ArduinoOTA.begin(false);
#endif
        else
            ArduinoOTA.end();
    }

    if (_otaAllowed && !_otaHandle)
    {
        _otaHandle = true; // prevent recursion
        ArduinoOTA.handle();
        _otaHandle = false;
    }
}

IPAddress NetworkModule::GetIpProperty(uint8_t PropertyId)
{
    uint8_t NoOfElem = 1;
    uint8_t *data;
    uint32_t length;
    knx.bau().propertyValueRead(OT_IP_PARAMETER, 0, PropertyId, NoOfElem, 1, &data, length);
    IPAddress ret = (length == 4) ? (data[3] << 24) + (data[2] << 16) + (data[1] << 8) + data[0] : 0;
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
    // Console command handling lives in NetworkConsole (see NetworkConsole.{h,cpp}).
    return _console.processCommand(cmd, debugKo);
}

#if MASK_VERSION == 0x091A || MASK_VERSION == 0x57B0

void NetworkModule::setMulticastAddress(IPAddress address, bool rebootToTakeEffect)
{
    // set default if address is 0.0.0.0
    if ((uint32_t)address == 0) address = (uint32_t)0x0C1700E0; // 224.0.23.12

    openknx.logger.logWithPrefixAndValues("Network", "Set multicast address to %s", address.toString().c_str());
    SetIpProperty(PID_ROUTING_MULTICAST_ADDRESS, address);
    knx.bau().writeMemory();

    if (rebootToTakeEffect)
        openknx.restart();
}
#endif

void NetworkModule::showNetworkInformations(bool console)
{
    openknx.common.skipLooptimeWarning();
    logBegin();
    if (console)
    {
        logInfoP("Hostname: %s", _hostName);
        logInfoP("Connection: %s", established() ? "Established" : "Disconnected");
        logIndentUp();
    }

    if (established())
    {
        logInfoP(_useStaticIP ? "Using static IP" : "Using DHCP");
        logInfoP("IP-Address: %s", localIP().toString().c_str());
        logInfoP("Netmask: %s", subnetMask().toString().c_str());
        logInfoP("Gateway: %s", gatewayIP().toString().c_str());
        logInfoP("DNS: %s", nameServerIP().toString().c_str());
#ifdef KNX_IP_LAN
        _ethLink.logLinkInfo(); // "ETH link: X Mbit Y-duplex (fixed/auto)"
#endif

#ifdef KNX_IP_WIFI
        std::string wifiInfo = std::string(_wifiSSID) + " (" + std::to_string(KNX_NETIF.RSSI()) + "dBm)";
        logInfoP("Wifi: %s", wifiInfo.c_str());
#endif
    }

    if (console)
    {
        logIndentDown();
    }
    logEnd();
}

void NetworkModule::showHelp()
{
    // Keep the general help compact; the detailed command list is printed by 'net ?'
    // (see NetworkConsole::showSubHelp()).
    openknx.console.printHelpLine("net, n", "Network info & commands (see 'net ? / n ?')");
}

#if defined(ARDUINO_ARCH_RP2040) && defined(KNX_IP_LAN)
// OpenKNX::Module hooks - forwarded to the EthLinkManager (RP2040 persists the manual fixed mode in flash).
void NetworkModule::processAfterStartupDelay() { _ethLink.onStartupDelay(); }
uint16_t NetworkModule::flashSize() { return _ethLink.flashSize(); }
void NetworkModule::writeFlash() { _ethLink.writeFlash(); }
void NetworkModule::readFlash(const uint8_t *data, const uint16_t size) { _ethLink.readFlash(data, size); }
#endif

// Link status
bool NetworkModule::connected()
{
    if (_powerSave) return false;

#if defined(ARDUINO_ARCH_RP2040)
#if defined(KNX_IP_LAN)
    return KNX_NETIF.isLinked();
#elif defined(KNX_IP_WIFI)
    return KNX_NETIF.isConnected();
#endif
#elif defined(ARDUINO_ARCH_ESP32)
    return espConnected;
#endif
}

// IP Status
bool NetworkModule::established()
{
    if (!connected()) return false;

    return localIP() != IPAddress();
}

IPAddress NetworkModule::localIP()
{
    return KNX_NETIF.localIP();
}

IPAddress NetworkModule::subnetMask()
{
    return KNX_NETIF.subnetMask();
}

IPAddress NetworkModule::gatewayIP()
{
    return KNX_NETIF.gatewayIP();
}

IPAddress NetworkModule::nameServerIP()
{
#if defined(ARDUINO_ARCH_RP2040)
    return IPAddress(dns_getserver(0));
#elif defined(ARDUINO_ARCH_ESP32)
    return KNX_NETIF.dnsIP();
#endif
}

inline std::string NetworkModule::phyMode()
{
    return "Auto";
}

void NetworkModule::savePower()
{
    _powerSave = true;
#if defined(PIN_ETH_RES)
    digitalWrite(PIN_ETH_RES, LOW);
#endif
}

void NetworkModule::macAddress(uint8_t *addr)
{
    KNX_NETIF.macAddress(addr);
}

bool NetworkModule::restorePower()
{
// TODO check all platforms
#if defined(PIN_ETH_RES)
    digitalWrite(PIN_ETH_RES, HIGH);
#endif
    _powerSave = false;
    return true;
}

#ifdef KNX_IP_WIFI
void NetworkModule::saveWifiSettings(const char *ssid, const char *passphrase, bool scheduleRebootToTakeEffect)
{
    logDebugP("Write WiFi settings");

#ifdef ARDUINO_ARCH_RP2040
    if (!scheduleRebootToTakeEffect)
        KNX_NETIF.disconnect();

    File file = LittleFS.open("/WIFI.TXT", "w");
    file.println(ssid);
    file.println(passphrase);
    file.close();

#elif ARDUINO_ARCH_ESP32
    if (!scheduleRebootToTakeEffect)
        KNX_NETIF.disconnect(true, true);

    Preferences preferences;
    preferences.begin("WIFI", false);
    preferences.putString("SSID", ssid);
    preferences.putString("PSK", passphrase);
    preferences.end();

#endif
    if (scheduleRebootToTakeEffect)
        _restartTimer = delayTimerInit();
}

void NetworkModule::readWifiSettings()
{
    logDebugP("Read WiFi settings");
    uint8_t found = 0;

#ifdef ARDUINO_ARCH_RP2040
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

    logIndentDown();
#elif ARDUINO_ARCH_ESP32

    Preferences preferences;
    if (preferences.begin("WIFI", true))
    {
        auto ssid = preferences.getString("SSID");
        auto passphrase = preferences.getString("PSK");
        if (ssid.length() > 0 && passphrase.length() > 0)
        {
            memcpy(_wifiSSID, ssid.c_str(), ssid.length());
            memcpy(_wifiPassphrase, passphrase.c_str(), passphrase.length());
            found = 2;
        }
        preferences.end();
    }
#endif

    if (found > 0) logDebugP("SSID: %s", _wifiSSID);
    if (found == 2) logDebugP("Passphrase with %i chars", strlen(_wifiPassphrase));
    if (found == 1) logErrorP("No wifi passphrase found");
}
#endif

bool NetworkModule::processFunctionProperty(uint8_t objectIndex, uint8_t propertyId, uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength)
{
    if (!knx.configured()) return false;
    if (objectIndex != 160) return false;
    if (propertyId != 5) return false;

    logHexTraceP(data, length);

    switch (data[0])
    {
        case 1:
        {
#ifdef KNX_IP_WIFI
            const uint8_t ssidLen = data[1];
            // const uint8_t pskLen = data[2];
            logInfoP("Received wifi settings for %s", data + 3);
            saveWifiSettings((char *)data + 3, (char *)data + 3 + ssidLen + 1, true);
            resultData[0] = 0;
#else
            logErrorP("Unsupported: Received wifi settings");
            resultData[0] = 2;
#endif
            resultLength = 1;
            return true;
        }
    }

    resultData[0] = 1;
    resultLength = 1;
    return false;
}

void NetworkModule::controlKnxIp(bool enable)
{
#if MASK_VERSION == 0x091A
    knx.bau().getPrimaryDataLinkLayer()->enabled(enable);
#elif MASK_VERSION == 0x57B0
    knx.bau().getDataLinkLayer()->enabled(enable);
#endif
}

NetworkModule openknxNetwork;
#endif
