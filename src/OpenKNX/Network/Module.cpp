#if defined(KNX_IP_WIFI) || defined(KNX_IP_LAN)
#include <algorithm>
#include <ArduinoOTA.h>
#include <iostream>
#include <sstream>

#include "DNS.h"
#include "Module.h"

// ESP32's prebuilt IDF ships autoip.c compiled out, so a -D cannot enable AutoIP -- guard against
// reporting a method the stack can't do. See doc/TODO-esp32-custom-idf.md. RP2040 builds lwIP from source.
#if defined(ARDUINO_ARCH_ESP32) && defined(OPENKNX_NETWORK_AUTOIP)
    #include <lwip/opt.h>
    #if !LWIP_AUTOIP
        #error "OPENKNX_NETWORK_AUTOIP requires an ESP-IDF built with CONFIG_LWIP_AUTOIP=y - see OFM-Network/doc/TODO-esp32-custom-idf.md"
    #endif
#endif
#ifdef OPENKNX_SSL_STEP_PROFILE
    #include <LittleFS.h>
#endif
#ifdef OPENKNX_WEBCLIENT_TEST
    #include "Webserver/FileManager.h" // drive routing for "http get" -- /flash, sd/, efc/
#endif
#include "ModuleVersionCheck.h"
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
#include <lwip/apps/mdns.h> // RP2040 mDNS via the lwIP API directly (SimpleMDNS.addServiceTxt broken on arduino-pico)
#include <lwip/netif.h>
// OPENKNX_ETH_W5500_MAINLOOP_RX: build the driver WITHOUT the INT pin so RX is NOT IRQ-driven; we pump the chip
// from the main loop (pumpEthernet) instead. Fixes two W5500+lwIP bugs with one root -- RX/timers running in a
// preemptive IRQ under a non-fair lock: the tunnel-flood watchdog reboot and the 30 s mDNS loop stall.
#if defined(PIN_ETH_INT) && !defined(OPENKNX_ETH_W5500_MAINLOOP_RX)
Wiznet5500lwIP KNX_NETIF(PIN_ETH_SS, ETH_SPI_INTERFACE, PIN_ETH_INT);
#else
Wiznet5500lwIP KNX_NETIF(PIN_ETH_SS, ETH_SPI_INTERFACE);
#endif

// mDNS TXT state (file scope so the lwIP announce callback can read it). Idempotent re-registration keeps
// slot+netif to del-before-add. See Module::registerOpenknxMdns().
static char _openknxMdnsTxt[7][48]; // "key=value", <= 255 B per DNS TXT item
static uint8_t _openknxMdnsTxtCount = 0;
static s8_t _openknxMdnsSlot = -1;
static struct netif *_openknxMdnsNetif = nullptr;
static void _openknxMdnsTxtCb(struct mdns_service *service, void *txt_userdata)
{
    (void)txt_userdata;
    for (uint8_t i = 0; i < _openknxMdnsTxtCount; i++)
        mdns_resp_add_service_txtitem(service, _openknxMdnsTxt[i], (u8_t)strlen(_openknxMdnsTxt[i]));
}
#elif defined(KNX_IP_WIFI)
#else
#pragma warn "Missing KNX_IP_LAN or KNX_IP_WIFI"
#endif

WiFiUDP Udp;
#else
#pragma warn "Unsupported platform"
#endif

// Total LAN packet counters. Consumers: the DeviceDisplay net-speed widget (declares it `extern`) and the
// `net` console output. Bytes aren't available (lwIP MIB2 is off in the prebuilt core), so packets are the
// honest whole-interface figure. RP2040 + wired LAN only -- the ESP32 ETH class exposes no such counters.
#if defined(ARDUINO_ARCH_RP2040) && defined(KNX_IP_LAN)
void openknxLanTraffic(uint32_t &rxPackets, uint32_t &txPackets)
{
    rxPackets = KNX_NETIF.packetsReceived();
    txPackets = KNX_NETIF.packetsSent();
}
#endif

// mDNS "otamode" TXT value from the ETS OTA-Update setting (ParamNET_OTAUpdate): prog | always | off.
// An unconfigured device has no ETS params -> reports "always" (mirrors handleOTA). File scope so both the
// ESP (ESPmDNS) and RP2040 (lwIP) mDNS paths read it.
static const char *otaModeStr()
{
    if (!knx.configured()) return "always";
    if (ParamNET_OTAUpdate == 2) return "off";
    if (ParamNET_OTAUpdate == 0) return "prog";
    return "always";
}

namespace OpenKNX
{
    namespace Network
    {

        const std::string Module::name()
        {
            return "Network";
        }

        // You can also give it a version
        // will be displayed in Command Infos
        const std::string Module::version()
        {
            return MODULE_Network_Version;
        }

        /*
         * Internal use: Developer only
         */
        void Module::resetNetwork()
        {
            logInfoP("Reset network adapter");
            logIndentUp();
            controlKnxIp(false);

#if defined(ARDUINO_ARCH_RP2040) && defined(OPENKNX_ETH_W5500)
            _ethLink.resetLink(); // bounce the W5500 PHY (cable re-plug): no reboot, no driver end()/begin()
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

        void Module::initPhy()
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
            // initalize SPI
            ETH_SPI_INTERFACE.setRX(PIN_ETH_MISO);
            ETH_SPI_INTERFACE.setTX(PIN_ETH_MOSI);
            ETH_SPI_INTERFACE.setSCK(PIN_ETH_SCK);
            ETH_SPI_INTERFACE.setCS(PIN_ETH_SS);
            logDebugP("Ethernet SPI GPIO: RX/MISO: %d, TX/MOSI: %d, SCK/SCLK: %d, CSn/SS: %d", PIN_ETH_MISO, PIN_ETH_MOSI, PIN_ETH_SCK, PIN_ETH_SS);
#endif

            logIndentDown();
        }

        void Module::loadSettings()
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
                    // length is an INPUT here: the bau discards the write if it is shorter than elements * ElementSize().
                    length = sizeof(friendlyNameWrite);
                    knx.bau().propertyValueWrite(OT_IP_PARAMETER, 0, PID_FRIENDLY_NAME, elements, 1, friendlyNameWrite, length);
                }

                delete[] friendlyNameRead; // propertyValueRead() allocates the buffer, we have to free it
            }

#ifdef DEVICE_DISPLAY_MODULE
            // Local on-device override (DeviceDisplay menu) wins over the ETS/property result computed above.
            if (_localOverride.overrideActive)
            {
                _useStaticIP = _localOverride.useStaticIP;
                logInfoP("Local network override active (%s)", _useStaticIP ? "static IP" : "DHCP");
                if (_useStaticIP)
                {
                    _staticLocalIP = IPAddress(_localOverride.ip[0], _localOverride.ip[1], _localOverride.ip[2], _localOverride.ip[3]);
                    _staticSubnetMask = IPAddress(_localOverride.subnet[0], _localOverride.subnet[1], _localOverride.subnet[2], _localOverride.subnet[3]);
                    _staticGatewayIP = IPAddress(_localOverride.gateway[0], _localOverride.gateway[1], _localOverride.gateway[2], _localOverride.gateway[3]);
                    _staticNameServerIP = IPAddress(_localOverride.dns[0], _localOverride.dns[1], _localOverride.dns[2], _localOverride.dns[3]);
                }
                else
                {
                    // DHCP: clear any previously computed static config so initIp() requests a lease.
                    _staticLocalIP = IPAddress();
                    _staticSubnetMask = IPAddress();
                    _staticGatewayIP = IPAddress();
                    _staticNameServerIP = IPAddress();
                }
            }
#endif

            // PID_IP_CAPABILITIES = which AUTOMATIC assignment methods this IP stack can do (03_08_03 2.5.7: bit0 BootP,
            // bit1 DHCP, bit2 AutoIP; manual is always implicit and has no bit). Set UNCONDITIONALLY: capabilities are
            // fixed and do not depend on the current config.
#ifdef OPENKNX_NETWORK_AUTOIP
            // AutoIP build: lwIP runs the DHCP<->AutoIP coop, so the stack advertises DHCP (bit1) AND AutoIP (bit2) = 6.
            SetByteProperty(PID_IP_CAPABILITIES, 6); // DHCP + AutoIP
#else
            // This stack does DHCP only (lwIP, no BootP, no AutoIP) -> value 2.
            SetByteProperty(PID_IP_CAPABILITIES, 2); // DHCP only
#endif

            // PID_CURRENT_IP_ASSIGNMENT_METHOD reflects the FINAL resolved assignment method actually IN USE (override included).
            if (_useStaticIP)
                SetByteProperty(PID_CURRENT_IP_ASSIGNMENT_METHOD, 1); // manual / static
            else
                SetByteProperty(PID_CURRENT_IP_ASSIGNMENT_METHOD, 4); // DHCP (03_08_03 2.5.5: 1=manual, 2=BootP, 4=DHCP, 8=AutoIP)

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
#if defined(ESP_IDF_VERSION) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
            ::Network.onEvent([](arduino_event_id_t event) -> void { openknxNetwork.esp32NetworkEvent(event); });
#else
            WiFi.onEvent([](WiFiEvent_t event) -> void { openknxNetwork.esp32NetworkEvent(event); });
#endif
#endif
        }

        void Module::init()
        {
            logInfoP("Initialize IP stack");
            logIndentUp();
            initPhy();
            loadSettings();
            initIp();
            logIndentDown();
        }

#ifdef ARDUINO_ARCH_ESP32
        void Module::esp32NetworkEvent(CALLBACK_EVENT event)
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
                {
                    logDebugP("Event: Got IP");
                    // Rebind kostet Telegramme, daher nur bei echtem IP-Wechsel.
                    IPAddress ip = localIP();
                    if (ip != _boundIp)
                    {
                        controlKnxIp(false);
                        controlKnxIp(true);
                        _boundIp = ip;
                    }
                    break;
                }
                case ARDUINO_EVENT_ETH_LOST_IP:
                case ARDUINO_EVENT_WIFI_STA_LOST_IP:
                    logDebugP("Event: Lost IP");
                    controlKnxIp(false);
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

        void Module::initIp()
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
#if defined(KNX_IP_LAN)
            _ethLink.applyBeforeBegin(); // apply the persisted (NVS) / build-flag fixed link mode BEFORE begin()
#endif
            KNX_NETIF.begin();
            KNX_NETIF.config(_staticLocalIP, _staticGatewayIP, _staticSubnetMask, _staticNameServerIP); // Stupid: Nedd to be after begin an also DHCP!
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

            // Bounded W5500 bring-up (setSPISpeed + begin live inside tryBeginEth); on failure go degraded and
            // let loop()->ethSelfHeal() keep retrying a clean bring-up -- no reboot, no fatalError brick.
            if (beginEthWithRetry())
            {
                _ethLink.applyEtsMode(); // ETS "LAN-Modus": apply it as soon as the PHY is reachable
            }
            else
            {
                _ethDegraded = true;
                logIndentUp();
                logErrorP("W5500 unreachable at boot -> self-heal every %lums, no reboot", (unsigned long)ETH_HEAL_INTERVAL_MS);
                logIndentDown();
            }
#endif
#endif
        }

        void Module::setup(bool configured)
        {
#ifdef OPENKNX_NETWORK_USB_EXCHANGE
            openknxUsbExchangeModule.onLoad("Network.txt", [this](UsbExchangeFile *file) { this->fillNetworkFile(file); });
#ifdef KNX_IP_WIFI
            openknxUsbExchangeModule.onLoad("Wifi.txt", [this](UsbExchangeFile *file) { this->fillWifiFile(file); });
            openknxUsbExchangeModule.onEject("Wifi.txt", [this](UsbExchangeFile *file) { return this->readWifiFile(file); });
#endif
#endif

            registerCallback([this](bool state) { if (state) this->showNetworkInformations(false); });

            _ipLedFunc = openknx.ledFunctions.get(OPENKNX_LEDFUNC_NET_STATE);
            _knxIpLedFunc = openknx.ledFunctions.get(OPENKNX_LEDFUNC_KNXIP_STATE);

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
                MDNS.addServiceTxt("openknx", "tcp", "otamode", otaModeStr());
#else
                registerOpenknxMdns(); // RP2040: register via lwIP (SimpleMDNS.addServiceTxt broken); reused by checkLinkStatus()
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
                _otaActive = true; // display OTA overlay takes over (SYSTEM priority)
                _otaPercent = 0;
#if defined(ARDUINO_ARCH_RP2040) && defined(KNX_IP_LAN) && defined(OPENKNX_ETH_W5500_MAINLOOP_RX)
                setEthOtaPump(true); // hand W5500 RX to the async IRQ pump so the blocking OTA read isn't loop-starved
#endif
                if (ArduinoOTA.getCommand() == U_FLASH)
                    logInfo("OTA", "Start updating firmware");
                else // U_SPIFFS
                    logInfo("OTA", "Start updating filesystem");
            });
            ArduinoOTA.onEnd([&]() {
                _otaActive = false;
#if defined(ARDUINO_ARCH_RP2040) && defined(KNX_IP_LAN) && defined(OPENKNX_ETH_W5500_MAINLOOP_RX)
                setEthOtaPump(false); // restore the main-loop RX pump (defensive: a restart below could be deferred)
#endif
                _otaPercent = 100;
                logIndentUp();
                logInfo("OTA", "Update complete");
                logIndentDown();
                openknx.restart();
            });
            ArduinoOTA.onProgress([&](unsigned int progress, unsigned int total) {
                int percent = (total > 0) ? (int)((uint64_t)progress * 100u / total) : 0; // div-by-zero-safe
                _otaPercent = (percent < 0) ? 0 : (percent > 100 ? 100 : percent);         // fine bar for the display overlay
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
                _otaActive = false; // release the display overlay so normal operation resumes
#if defined(ARDUINO_ARCH_RP2040) && defined(KNX_IP_LAN) && defined(OPENKNX_ETH_W5500_MAINLOOP_RX)
                setEthOtaPump(false); // restore the main-loop RX pump -- device keeps running after an OTA error
#endif
                logIndentUp();
                if (error == OTA_AUTH_ERROR)
                    logError("OTA", "Auth error");
                else if (error == OTA_BEGIN_ERROR)
                    logErrorP("OTA", "Begin error");
                else if (error == OTA_CONNECT_ERROR)
                    logError("OTA", "Connect error");
                else if (error == OTA_RECEIVE_ERROR)
                    logError("OTA", "Receive error");
                else if (error == OTA_END_ERROR)
                    logError("OTA", "End error");
                logIndentDown();
            });

#if defined(OPENKNX_TELEGRAMJSON) && defined(OPENKNX_MQTT)
            // setup() runs once, so registerReceivedFrame() cannot stack up (it appends
            // to a vector). knx.start() comes later; the callback just stays idle until then.
            if (configured && ParamNET_MQTT && ParamNET_MQTTTPRawData)
            {
                _telegramTopic = mqtt.devicePrefix() + "knx/tp/telegramm";
                tpUart().registerReceivedFrame(
                    [](TPUart::Frame &frame) { openknxNetwork.publishTelegram(frame); });
            }
#endif
        }


#ifdef OPENKNX_NETWORK_USB_EXCHANGE
        void Module::fillNetworkFile(UsbExchangeFile *file)
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
                // writeLineToFile(file, "Mode: %s", phyMode().c_str()); Currently not supported
            }
        }

#ifdef KNX_IP_WIFI
        void Module::fillWifiFile(UsbExchangeFile *file)
        {
            writeLineToFile(file, "SSID");
            writeLineToFile(file, "Password");
        }

        static std::string trimString(const std::string &str)
        {
            auto start = std::find_if_not(str.begin(), str.end(), ::isspace);
            auto end = std::find_if_not(str.rbegin(), str.rend(), ::isspace).base();
            return (start < end ? std::string(start, end) : "");
        }

        bool Module::readWifiFile(UsbExchangeFile *file)
        {
            openknx.watchdog.loop();
            std::string ssid;
            std::string password;

            char tmp[100] = {};
            if (file->fgets(tmp, 99) > 0) ssid = tmp;

            memset(tmp, 0x0, 100);
            if (file->fgets(tmp, 99) > 0) password = tmp;

            ssid = trimString(ssid);
            password = trimString(password);

            logInfoP("Read Wifi settings from Wifi.txt");
            logIndentUp();
            if (ssid.length() == 0 || ssid.length() > 32 || ssid == "SSID")
            {
                logErrorP("SSID is invalid");
                return false;
            }
            if (password.length() == 0 || password.length() > 63)
            {
                logErrorP("Password is invalid");
                return false;
            }

            logInfoP("SSID: %s", ssid.c_str());
            logIndentDown();

            saveWifiSettings(ssid.c_str(), password.c_str(), true);
            return true;
        }
#endif
#endif

        // Establish debounce: only print "Network established" + fire the establish callbacks once the link has
        // genuinely held this long, not on every brief establish during the ETH auto-fallback search / flapping.
        static constexpr uint32_t NET_INFO_STABLE_MS = 3000;

#if defined(ARDUINO_ARCH_RP2040) && defined(KNX_IP_LAN)
        // arduino-pico's W5500 driver never calls netif_set_link_up/down, so lwIP misses cable changes and
        // DHCP stays on the stale lease. Feed both edges so dhcp_discover() re-runs and DHCP/AutoIP re-arm.
        void Module::setLwipLinkState(bool up)
        {
            netif *intf = KNX_NETIF.getNetIf();
            if (intf == nullptr) return;

            ethernet_arch_lwip_begin();
            if (up)
                netif_set_link_up(intf);
            else
                netif_set_link_down(intf);
            ethernet_arch_lwip_end();
        }
#endif

#ifdef OPENKNX_NETWORK_AUTOIP
        // AutoIP: lwIP self-assigns 169.254/16 when no DHCP answers. Report the method actually in use in
        // PID_CURRENT_IP_ASSIGNMENT_METHOD (03_08_03 2.5.5: 8=AutoIP, 4=DHCP); driven by the address, not the edge.
        void Module::checkIpAssignmentMethod()
        {
            if (_useStaticIP) return; // static/manual config is resolved in loadSettings()

            const IPAddress ip = localIP();

            // say it once; "link up, no address" is normal until the DHCP DISCOVERs time out (~2 min).
            if (ip == IPAddress())
            {
                if (_currentLinkState && !_autoIpWaitLogged && netUptimeSec() >= 20)
                {
                    _autoIpWaitLogged = true;
                    logInfoP("No DHCP answer yet - waiting, link-local fallback follows if none arrives");
                }
                return;
            }
            _autoIpWaitLogged = false;

            if (ip == _reportedIp) return;
            _reportedIp = ip;

            const bool linkLocal = (ip[0] == 169 && ip[1] == 254);
            SetByteProperty(PID_CURRENT_IP_ASSIGNMENT_METHOD, linkLocal ? 8 : 4);
            logInfoP("IP-Address: %s (%s)", ip.toString().c_str(), linkLocal ? "AutoIP" : "DHCP");
        }
#endif

        // Link-down housekeeping. Also called by the W5500 recovery, which bypasses checkLinkStatus() and
        // would otherwise leave lwIP's link-up flag set and the DHCP/AutoIP handover unarmed.
        void Module::onLinkLost()
        {
            // idempotent: both the down edge and recoverEth() call it; running twice would restart the outage timer.
            if (!_currentLinkState) return;

#if defined(ARDUINO_ARCH_RP2040) && defined(KNX_IP_LAN)
            setLwipLinkState(false); // must run while the netif still exists (before any end())
#endif
            _currentLinkState = false;
            _ipShown = false;
            _ipStableSince = 0; // restart the establish debounce on link loss
            _linkDownSinceSec = uptime();
            _linkUpSinceSec = 0;
            _establishedSinceSec = 0;
            _linkEverDown = true;
            loadCallbacks(false);
        }

        void Module::checkIpStatus()
        {
            if (_ipShown || !established()) return;

            if (_ipStableSince == 0) _ipStableSince = millis();
            if ((uint32_t)(millis() - _ipStableSince) < NET_INFO_STABLE_MS) return;

            _establishedSinceSec = uptime();

            logBegin();
            logInfoP("Network established");
            logIndentUp();
            loadCallbacks(true);
            logIndentDown();
            logEnd();

            _ipShown = true;
        }

        void Module::checkLinkStatus()
        {
            if (!delayCheckMillis(_lastLinkCheck, 500)) return;

            // Honest KNX-IP-Status LED (func 11): reflects the KNXnet/IP datalink, separate from the physical-link LED below.
            checkKnxIpStatus();

            // Get current network state
            bool establishedState = established();

#ifdef ARDUINO_ARCH_ESP32
            // Fängt einen Link-Flap auf, nach dem GOT_IP ausbleibt. Nur ESP32: RP2040 hat
            // keine Events und damit keine Lücke.
            controlKnxIp(establishedState);
#endif

            bool newLinkState;
            if (establishedState)
                newLinkState = true;
            else
                newLinkState = connected();

#if defined(KNX_IP_LAN) && defined(OPENKNX_ETH_AUTO_FALLBACK)
            _ethLink.tick(newLinkState, establishedState); // ~2 Hz link tick; walks the auto-fallback ladder
#endif
#if defined(ARDUINO_ARCH_RP2040) && defined(KNX_IP_LAN)
            // re-register mDNS on the link-up edge (the service is lost with the old netif on a W5500 recovery)
            if (establishedState && !_mdnsWasEstablished) registerOpenknxMdns();
            _mdnsWasEstablished = establishedState;
#endif

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
                // keine vollständigen Zugangsdaten hinterlegt
                if (_wifiSSID[0] == 0 || _wifiPassphrase[0] == 0)
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
#if defined(ARDUINO_ARCH_RP2040) && defined(KNX_IP_LAN)
                setLwipLinkState(true); // DHCP: REBOOTING (reclaims the lease) -> DISCOVER -> AutoIP
#endif
                _linkUpSinceSec = uptime();
                if (_linkEverDown)
                {
                    _lastDowntimeSec = _linkUpSinceSec - _linkDownSinceSec;
                    if (_reconnects < 0xFFFF) _reconnects++;
                    logInfoP("Link connected (down %us, reconnect #%u)",
                             (unsigned)_lastDowntimeSec, (unsigned)_reconnects);
                }
                else
                {
                    logInfoP("Link connected");
                }
                _linkDownSinceSec = 0;
                // #if defined(KNX_IP_W5500)
                // ethernet_arch_lwip_begin();
                // netif_set_link_up(KNX_NETIF.getNetIf());
                // if (_useStaticIP)
                // netif_set_ipaddr(KNX_NETIF.getNetIf(), _staticLocalIP);
                // else
                // dhcp_network_changed_link_up(KNX_NETIF.getNetIf());
                // ethernet_arch_lwip_end();
                // #elif defined(KNX_IP_GENERIC)
                //     if (!established()) KNX_NETIF.maintain();
                // #endif
            }

            // lost link
            else if (!newLinkState && _currentLinkState)
            {
                onLinkLost();
                logInfoP("Link disconnected");
            }

            _currentLinkState = newLinkState;
            _lastLinkCheck = millis();

            if (_currentLinkState) checkIpStatus();

#ifdef OPENKNX_NETWORK_AUTOIP
            // every tick, not just the edge: AutoIP takes over ~1 min into the discover phase.
            if (_currentLinkState) checkIpAssignmentMethod();
#endif

            // set network bit for heartbeat
            if (established())
                openknx.common.extendedHeartbeatValue |= 0b10000000;
            else
                openknx.common.extendedHeartbeatValue &= 0b01111111;
        }

        void Module::loop(bool configured)
        {
            if (_restartTimer > 0 && delayCheck(_restartTimer, 2000))
            {
                openknx.restart();
            }

            if (_powerSave) return;

#if defined(ARDUINO_ARCH_RP2040) && defined(KNX_IP_LAN) && defined(OPENKNX_ETH_W5500_MAINLOOP_RX)
            if (!_otaActive) pumpEthernet(); // drive W5500 RX + lwIP timers from the main loop (INT off);
                                             // while _otaActive the async IRQ pump owns RX (see setEthOtaPump)
#endif
            // During OTA the async IRQ pump owns W5500 RX; skip the SPI health-checks (checkEthHealth/
            // checkLinkStatus, and the ping/DNS/webclient loops below) so they don't contend for the SPI bus +
            // lwIP lock in the windows between flash commits. The device is dedicated to the update anyway.
            if (_otaActive) return;
#if defined(ARDUINO_ARCH_RP2040) && defined(OPENKNX_ETH_W5500)
            if (_ethDegraded)
            {
                ethSelfHeal(); // W5500 not up yet: keep retrying a clean bring-up; skip normal link handling
                return;
            }
            checkEthHealth(); // runtime W5500 wedge watchdog -> clean recovery
#elif defined(ARDUINO_ARCH_ESP32) && defined(OPENKNX_ETH_W5500) && defined(KNX_IP_LAN)
            checkEthHealthEsp(); // ESP+W5500 hard-wedge recovery (link-based); native-EMAC (REG1) excluded
#endif

#ifdef OPENKNX_PING
            _pingHandler.loop();
#endif
#if defined(ARDUINO_ARCH_RP2040) || defined(ARDUINO_ARCH_ESP32)
            OpenKNX::Network::DNS::loop();
#endif
#ifdef OPENKNX_WEBCLIENT
            if (established()) webclient.loop();
#endif

            checkLinkStatus();
            handleOTA();

#ifdef OPENKNX_TELEGRAMJSON
            // Independent of the webserver and of established(): the table lives on
            // LittleFS, already mounted by the time any loop() runs. begin() is
            // idempotent and self-logs, so no extra guard/flag needed here.
            gatable.begin();
#endif

#ifdef OPENKNX_WEBSERVER
            if (ParamNET_HTTP || !knx.configured())
            {
                if (!webserver.isRunning() && established())
                {
                    webserver.setup();
#ifdef OPENKNX_WEBCONSOLE
                    webconsole.setup();
#endif
#ifdef OPENKNX_WEBFS
                    filemanager.setup();
#endif
#if defined(OPENKNX_TELEGRAMJSON) && defined(OPENKNX_WEBMONITOR)
                    groupmonitor.setup();
#endif
                }
                if (webserver.isRunning())
                {
                    webserver.loop();
#ifdef OPENKNX_WEBCONSOLE
                    webconsole.loop();
#endif
#ifdef OPENKNX_WEBFS
                    filemanager.loop();
#endif
                }
            }
#endif
#ifdef OPENKNX_MQTT
            if (established())
            {
                mqtt.setup(configured);
                mqtt.loop(configured);
            }
#endif
        }

#if defined(OPENKNX_TELEGRAMJSON) && defined(OPENKNX_MQTT)
        void Module::publishTelegram(TPUart::Frame &frame)
        {
            // connected() first: while MQTT is down, decoding would be thrown away.
            if (!frame.isFrame() || !mqtt.connected()) return;

            _telegramJson.reset();
            buildTelegramJson(frame, _telegramJson);

            const std::string &json = _telegramJson.str();
            // Queues and returns -- never blocks this callback, see MQTT::Module::publish().
            mqtt.publish(_telegramTopic.c_str(), json.c_str(), json.size());
        }
#endif

        void Module::handleOTA()
        {
            bool allowed = true;
            // An unconfigured device has no ETS params -> ParamNET_OTAUpdate reads erased param memory. Honouring
            // it here can lock OTA out permanently (you would need ETS to enable the very mechanism you want to
            // flash with). Only apply the ETS gate once configured; an unconfigured device keeps OTA open.
            if (knx.configured())
            {
                if (ParamNET_OTAUpdate == 2) allowed = false;
                if (ParamNET_OTAUpdate == 0) allowed = knx.progMode();
            }

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

        IPAddress Module::GetIpProperty(uint8_t PropertyId)
        {
            uint8_t NoOfElem = 1;
            uint8_t *data;
            uint32_t length;
            knx.bau().propertyValueRead(OT_IP_PARAMETER, 0, PropertyId, NoOfElem, 1, &data, length);
            IPAddress ret = (length == 4) ? (data[3] << 24) + (data[2] << 16) + (data[1] << 8) + data[0] : 0;
            delete[] data;
            return ret;
        }

        void Module::SetIpProperty(uint8_t PropertyId, IPAddress IPAddress)
        {
            uint8_t NoOfElem = 1;
            uint8_t data[4];

            data[0] = IPAddress[0];
            data[1] = IPAddress[1];
            data[2] = IPAddress[2];
            data[3] = IPAddress[3];

            // length is the payload we pass; the bau rejects a write shorter than NoOfElem*ElementSize() (0 = silent drop).
            knx.bau().propertyValueWrite(OT_IP_PARAMETER, 0, PropertyId, NoOfElem, 1, data, sizeof(data));
        }

        uint8_t Module::GetByteProperty(uint8_t PropertyId)
        {
            uint8_t NoOfElem = 1;
            uint8_t *data = nullptr;
            uint32_t length = 0;
            knx.bau().propertyValueRead(OT_IP_PARAMETER, 0, PropertyId, NoOfElem, 1, &data, length);
            uint8_t ret = (data != nullptr && length >= 1) ? data[0] : 0;
            delete[] data;
            return ret;
        }

        void Module::SetByteProperty(uint8_t PropertyId, uint8_t value)
        {
            uint8_t NoOfElem = 1;
            uint8_t data[1];

            data[0] = value;

            // see SetIpProperty: the payload length is an INPUT the bau bounds the write against, not an output.
            knx.bau().propertyValueWrite(OT_IP_PARAMETER, 0, PropertyId, NoOfElem, 1, data, sizeof(data));
        }

        void Module::registerCallback(NetworkChangeCallback cb)
        {
            _callback.push_back(cb);
        }

        void Module::loadCallbacks(bool state)
        {
            for (int i = 0; i < _callback.size(); i++)
            {
                _callback[i](state);
            }
        }

        void Module::showInformations()
        {
            openknx.logger.logWithPrefixAndValues("Hostname", "%s", _hostName);
            if (established())
                openknx.logger.logWithPrefixAndValues("Network", "Established (%s)", localIP().toString().c_str());
            else
                openknx.logger.logWithPrefix("Network", "Disconnected");
        }

        bool Module::processCommand(const std::string cmd, bool debugKo)
        {
            if (debugKo) return false;

            if (!debugKo && (cmd == "n" || cmd == "net"))
            {
                showNetworkInformations(true);
                return true;
            }

#if MASK_VERSION == 0x091A || MASK_VERSION == 0x57B0
            else if (cmd == "net mc")
            {
                IPAddress address = GetIpProperty(PID_ROUTING_MULTICAST_ADDRESS);
                openknx.logger.logWithPrefixAndValues("Network", "Multicast address is set to %s", address.toString().c_str());
                return true;
            }

            else if (cmd.compare(0, 7, "net mc ") == 0)
            {
                IPAddress new_address;
                std::string new_address_str = cmd.length() >= 7 ? cmd.substr(7) : "reset";

                if (new_address_str == "reset") new_address_str = "0.0.0.0";

                new_address.fromString(new_address_str.c_str());

                setMulticastAddress(new_address, true);
                return true;
            }
#endif

#ifdef OPENKNX_SSL_STEP_PROFILE
            // Measurement helper for the TLS handshake stall (see doc/CONCEPT-OFM-EMail.md, V1).
            // "net tls <host> [port] [cert]" -- "cert" validates against /tls-root.pem on LittleFS.
            else if (cmd.compare(0, 8, "net tls ") == 0)
            {
                static std::string rootPem; // must outlive the request: the slot keeps a raw pointer
                std::string arg = cmd.substr(8);
                bool withCert = false;
                size_t certPos = arg.find(" cert");
                if (certPos != std::string::npos)
                {
                    withCert = true;
                    arg = arg.substr(0, certPos);
                }
                uint16_t port = 443;
                size_t sp = arg.find(' ');
                if (sp != std::string::npos)
                {
                    port = (uint16_t)atoi(arg.substr(sp + 1).c_str());
                    arg = arg.substr(0, sp);
                }
                if (arg.empty() || port == 0)
                {
                    logErrorP("net tls <host> [port] [cert]");
                    return true;
                }

                std::string url = "https://" + arg + ":" + std::to_string(port) + "/";
                auto req = webclient.get(url);
                if (withCert)
                {
                    rootPem.clear();
                    File f = LittleFS.open("/tls-root.pem", "r");
                    if (!f)
                    {
                        logErrorP("net tls: /tls-root.pem not found");
                        return true;
                    }
                    while (f.available())
                        rootPem.push_back((char)f.read());
                    f.close();
                    req.verifyCertificate(rootPem.c_str());
                }
                logInfoP("net tls: %s, certificate check %s", url.c_str(), withCert ? "on" : "off");
                req.ignoreBody().onDone([](const OpenKNX::Network::Webclient::Response &r) {
                       logInfo("SslProfile", "request finished: success=%d status=%u", (int)r.success(), r.status());
                   }).send();
                return true;
            }
#endif
            else if (cmd == "net reset")
            {
                resetNetwork();
                return true;
            }

#ifdef KNX_IP_LAN
            // 'net eth' / 'net eth <auto|10|100> [half|full]' -- all platform specifics live in EthLinkManager.
            else if (cmd == "net eth" || cmd.compare(0, 8, "net eth ") == 0)
            {
                std::string arg = (cmd.length() > 7) ? cmd.substr(7) : "";
                while (!arg.empty() && arg[0] == ' ')
                    arg.erase(0, 1); // trim leading spaces

                if (arg.empty())
                {
                    _ethLink.logStatus();
                    return true;
                } // no arg -> show current mode

                std::string speed = arg, dup;
                size_t sp = arg.find(' ');
                if (sp != std::string::npos)
                {
                    speed = arg.substr(0, sp);
                    dup = arg.substr(sp + 1);
                    while (!dup.empty() && dup[0] == ' ')
                        dup.erase(0, 1);
                }

                uint8_t mode;
                if (speed == "auto") mode = 0;
                else if (speed == "10")
                    mode = 1;
                else if (speed == "100")
                    mode = 2;
                else
                {
                    logErrorP("Usage: net eth <auto|10|100> [half|full]");
                    return true;
                }

                bool full = false;
                if (mode != 0)
                {
                    if (dup == "full") full = true;
                    else if (dup.empty() || dup == "half")
                        full = false;
                    else
                    {
                        logErrorP("Duplex must be 'half' or 'full'");
                        return true;
                    }
                }

                _ethLink.setMode(mode, full); // set + persist + apply live (per-platform, inside the manager)
                return true;
            }
#endif
#ifdef OPENKNX_WEBSERVER
            else if (cmd == "webserver log")
            {
                bool enabled = !webserver.accessLog();
                webserver.accessLog(enabled);
                logInfoP("Webserver access log %s", enabled ? "enabled" : "disabled (4xx/5xx only)");
                return true;
            }
#endif

#ifdef KNX_IP_WIFI
            else if (cmd == "erase wifi")
            {
                saveWifiSettings("", "", true); // triggers the restart timer
                logInfoP("Wifi settings erased");
                return true;
            }
            else if (cmd.compare(0, 4, "wifi") == 0 && cmd.length() > 6)
            {
                // Command: "wifi<SEP><ssid><SEP><psk>" with <SEP>=" " compatible to previous command
                char delimiter = cmd[4];
                std::string ssid_psk = cmd.substr(5);
                size_t pos = ssid_psk.find(delimiter);

                if (pos != std::string::npos)
                {
                    std::string ssid = ssid_psk.substr(0, pos);
                    std::string psk = ssid_psk.substr(pos + 1);

                    // Grenzen wie in readWifiFile() und Network.share.xml
                    if (ssid.length() > 32 || psk.length() > 63)
                    {
                        logErrorP("SSID max. 32, PSK max. 63 Zeichen");
                        return false;
                    }

                    if (ssid.empty())
                        logInfoP("Wifi settings erased");
                    else
                        logInfoP("WLAN SSID: %s", ssid.c_str());
                    saveWifiSettings(ssid.c_str(), psk.c_str(), true); // triggers the restart timer

                    return true;
                }
                else
                {
                    logErrorP("Expected format: wifi<Sep><SSID><Sep><PSK> with <Sep> single char not in <SSID>");
                    return false;
                }
            }

// else if (cmd == "net recon" && strlen(_wifiSSID) > 0)
// {
//     logInfoP("Connecting to WiFi \"%s\"", _wifiSSID);
//     KNX_NETIF.disconnect();
//     KNX_NETIF.begin(_wifiSSID, _wifiPassphrase);
//     return true;
// }
// #else
// else if (!_useStaticIP && cmd == "net renew")
// {
//     if (!connected())
//     {
//         logErrorP("not connected");
//         return true;
//     }

//     #ifdef KNX_IP_GENERIC
//     KNX_NETIF.maintain();
//     #elif defined(ARDUINO_ARCH_ESP32)
//             // TODO
//     #else
//     dhcp_renew(KNX_NETIF.getNetIf());
//     #endif
//     return true;
// }
#endif

#ifdef OPENKNX_WEBCLIENT_TEST
            // "http get <url> [path]" -- same job the web interface arms, see httpFetchStart().
            else if (cmd.compare(0, 9, "http get ") == 0)
            {
                std::string arg = cmd.substr(9);
                size_t sp = arg.find(' ');
                std::string url = (sp == std::string::npos) ? arg : arg.substr(0, sp);
                std::string raw = (sp == std::string::npos) ? "download.bin" : arg.substr(sp + 1);
                if (url.empty()) { logErrorP("http get <url> [path]  (path: /flash, sd/, efc/)"); return true; }
                if (!httpFetchStart(url, raw)) logErrorP("%s", _fetch.error.c_str());
                return true;
            }
#endif

#ifdef OPENKNX_PING
            else if (cmd.compare(0, 5, "ping ") == 0)
            {
                ping(cmd.substr(5), [](IPAddress ip, bool reachable, uint32_t rttMs) {
                    if (reachable)
                        openknx.logger.logWithPrefixAndValues("Network", "Reply from %s in %lu ms", ip.toString().c_str(), rttMs);
                    else
                        openknx.logger.logWithPrefixAndValues("Network", "No reply from %s", ip.toString().c_str());
                });
                return true;
            }
#endif

            return false;
        }

#if MASK_VERSION == 0x091A || MASK_VERSION == 0x57B0

        void Module::setMulticastAddress(IPAddress address, bool rebootToTakeEffect)
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

        void Module::showNetworkInformations(bool console)
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
                // 169.254 = DHCP gave up and lwIP self-assigned; "Using DHCP" would misreport it.
                const IPAddress ip = localIP();
                if (_useStaticIP)
                    logInfoP("Using static IP");
                else if (ip[0] == 169 && ip[1] == 254)
                    logInfoP("Using AutoIP (no DHCP server answered)");
                else
                    logInfoP("Using DHCP");
                logInfoP("IP-Address: %s", localIP().toString().c_str());
                logInfoP("Netmask: %s", subnetMask().toString().c_str());
                logInfoP("Gateway: %s", gatewayIP().toString().c_str());
                logInfoP("DNS: %s", nameServerIP().toString().c_str());
#ifdef KNX_IP_LAN
                _ethLink.logLinkInfo(); // "ETH link: X Mbit Y-duplex (fixed/auto)"
#endif
#if defined(ARDUINO_ARCH_RP2040) && defined(KNX_IP_LAN)
                {
                    // Whole-interface packet totals since boot. Compact units: the raw counters reach
                    // eight digits within days on a busy segment.
                    uint32_t rxPackets = 0, txPackets = 0;
                    openknxLanTraffic(rxPackets, txPackets);
                    logInfoP("Packets RX/TX: %s / %s", humanCount(rxPackets).c_str(),
                             humanCount(txPackets).c_str());
                }
#endif
                // logInfoP("Mode: %s", phyMode().c_str()); currently not supported

#ifdef KNX_IP_WIFI
                std::string wifiInfo = std::string(_wifiSSID) + " (" + std::to_string(KNX_NETIF.RSSI()) + "dBm)";
                logInfoP("Wifi: %s", wifiInfo.c_str());
#endif
            }

            if (console)
            {
                if (_currentLinkState)
                {
                    // counts from the carrier edge, shown even while disconnected (the DHCP/AutoIP wait state).
                    logInfoP("Link-Uptime: %s", humanDuration(netUptimeSec()).c_str());
                    // only when the IP arrived noticeably after the link (slow DHCP / late renew).
                    const uint32_t established = netEstablishedSec();
                    if (established > 0 && (netUptimeSec() - established) >= 5)
                        logInfoP("IP established: %s", humanDuration(established).c_str());
                }
                else if (_linkEverDown)
                {
                    logInfoP("Down since: %s", humanDuration(netDowntimeSec()).c_str());
                }

                if (_reconnects > 0)
                    logInfoP("Reconnects: %u (last down %s)", (unsigned)_reconnects,
                             humanDuration(_lastDowntimeSec).c_str());

                logIndentDown();
            }
            logEnd();
        }

        void Module::showHelp()
        {
            openknx.console.printHelpLine("net, n", "Show network informations");

#ifdef KNX_IP_WIFI

            openknx.console.printHelpLine("wifi SSID PSK", "Set SSID and PSK");
            openknx.console.printHelpLine("erase wifi", "Erase WiFi settings");

// if (strlen(_wifiSSID) > 0) openknx.console.printHelpLine("net recon", "Reconnect to network");
#else
// if (!_useStaticIP) openknx.console.printHelpLine("net renew", "Renew DHCP Address");
#endif
#if MASK_VERSION == 0x091A || MASK_VERSION == 0x57B0
            // openknx.console.printHelpLine("net mc [address|reset]", "Get/Set multicast address");
#endif
            openknx.console.printHelpLine("net reset", "Reset network adapter");
#ifdef OPENKNX_WEBCLIENT_TEST
            openknx.console.printHelpLine("http get <url> [path]", "Download to /flash, sd/ or efc/ (test aid)");
#endif
#ifdef OPENKNX_SSL_STEP_PROFILE
            openknx.console.printHelpLine("net tls <host> [port] [cert]", "TLS handshake stall measurement");
#endif
#ifdef KNX_IP_LAN
            openknx.console.printHelpLine("net eth", "Show the current ETH link mode");
#ifdef OPENKNX_ETH_AUTO_FALLBACK
            openknx.console.printHelpLine("net eth auto", "Auto-negotiation with auto-fallback ladder");
#else
            openknx.console.printHelpLine("net eth auto", "Auto-negotiation");
#endif
            openknx.console.printHelpLine("net eth 10 [half|full]", "Force a fixed 10 Mbit link (default half)");
            openknx.console.printHelpLine("net eth 100 [half|full]", "Force a fixed 100 Mbit link (default half)");
#endif
#ifdef OPENKNX_WEBSERVER
            openknx.console.printHelpLine("webserver log", "Toggle webserver access log (2xx/3xx)");
#endif
#ifdef OPENKNX_PING
            openknx.console.printHelpLine("ping x.x.x.x", "Ping an IP address");
#endif
        }

        // Link status
        bool Module::connected()
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
        bool Module::established()
        {
            if (!connected()) return false;

            return localIP() != IPAddress();
        }

        uint32_t Module::netUptimeSec() const
        {
            return _currentLinkState ? (uptime() - _linkUpSinceSec) : 0;
        }

        uint32_t Module::netDowntimeSec() const
        {
            return (!_currentLinkState && _linkEverDown) ? (uptime() - _linkDownSinceSec) : 0;
        }

        uint32_t Module::netEstablishedSec() const
        {
            return _ipShown ? (uptime() - _establishedSinceSec) : 0;
        }

        IPAddress Module::localIP()
        {
            return KNX_NETIF.localIP();
        }

        IPAddress Module::subnetMask()
        {
            return KNX_NETIF.subnetMask();
        }

        IPAddress Module::gatewayIP()
        {
            return KNX_NETIF.gatewayIP();
        }

        IPAddress Module::nameServerIP()
        {
#if defined(ARDUINO_ARCH_RP2040)
            return IPAddress(dns_getserver(0));
#elif defined(ARDUINO_ARCH_ESP32)
            return KNX_NETIF.dnsIP();
#endif
        }

        inline std::string Module::phyMode()
        {
            return "Auto";
        }

#if MASK_VERSION == 0x07B0 || MASK_VERSION == 0x091A
        TPUart::DataLinkLayer &Module::tpUart()
        {
            // a coupler (KNX_IS_ROUTER) keeps TP on the secondary link (its primary is IP); every other TP
            // build on the primary. The interface (0x07B0 + tunnelling) is not a router -> getDataLinkLayer().
            return KNX_TP_DLL->getTPUart();
        }
#endif

        void Module::savePower()
        {
            _powerSave = true;
#if defined(PIN_ETH_RES)
            digitalWrite(PIN_ETH_RES, LOW);
#endif
        }

        void Module::macAddress(uint8_t *addr)
        {
            KNX_NETIF.macAddress(addr);
        }

        bool Module::restorePower()
        {
// TODO check all platforms
#if defined(PIN_ETH_RES)
            digitalWrite(PIN_ETH_RES, HIGH);
#endif
            _powerSave = false;
            return true;
        }

#ifdef KNX_IP_WIFI
        // Kopiert höchstens dstSize-1 Bytes und terminiert immer
        static void copyBounded(char *dst, size_t dstSize, const char *src, size_t len)
        {
            if (dstSize == 0) return;
            if (len > dstSize - 1) len = dstSize - 1;
            memcpy(dst, src, len);
            dst[len] = '\0';
        }

        void Module::saveWifiSettings(const char *ssid, const char *passphrase, bool scheduleRebootToTakeEffect)
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

        void Module::readWifiSettings()
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

                if (value != '\n' && value != '\r')
                {
                    if (pos < (int)sizeof(buffer) - 1)
                        buffer[pos++] = value;
                }

                if (value == '\n' || value == '\r' || !file.available())
                {
                    if (pos > 0)
                    {
                        found++;
                        if (found == 1) copyBounded(_wifiSSID, sizeof(_wifiSSID), buffer, pos);
                        if (found == 2) copyBounded(_wifiPassphrase, sizeof(_wifiPassphrase), buffer, pos);
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
                    copyBounded(_wifiSSID, sizeof(_wifiSSID), ssid.c_str(), ssid.length());
                    copyBounded(_wifiPassphrase, sizeof(_wifiPassphrase), passphrase.c_str(), passphrase.length());
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

        bool Module::processFunctionProperty(uint8_t objectIndex, uint8_t propertyId, uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength)
        {
            if (!knx.configured()) return false;
            if (objectIndex != 160) return false;
            if (propertyId != 5) return false;

            if (length < 1) return false;

            logHexTraceP(data, length);

            switch (data[0])
            {
                case 1:
                {
#ifdef KNX_IP_WIFI
                    // [1]=SSID-Länge, [2]=PSK-Länge, ab [3] SSID, Trennbyte, PSK
                    if (length < 3)
                    {
                        logErrorP("Wifi settings: payload too short (%u)", length);
                        resultData[0] = 1;
                        resultLength = 1;
                        return false;
                    }

                    const uint8_t ssidLen = data[1];
                    const uint8_t pskLen = data[2];
                    const uint16_t needed = (uint16_t)3 + ssidLen + 1 + pskLen;
                    if (ssidLen == 0 || ssidLen > 32 || pskLen > 63 || needed > length)
                    {
                        logErrorP("Wifi settings: invalid lengths (ssid=%u psk=%u payload=%u)", ssidLen, pskLen, length);
                        resultData[0] = 1;
                        resultLength = 1;
                        return false;
                    }

                    // Bus-Daten sind nicht zwingend NUL-terminiert
                    char ssid[33] = {};
                    char psk[64] = {};
                    memcpy(ssid, data + 3, ssidLen);
                    memcpy(psk, data + 3 + ssidLen + 1, pskLen);

                    logInfoP("Received wifi settings for %s", ssid);
                    saveWifiSettings(ssid, psk, true);
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

        void Module::controlKnxIp(bool enable)
        {
            // NOTE: deliberately NOT extended to 0x07B0. On the interface controlKnxIp has always been a no-op
            // (Bau07B0IP owns the IP datalink lifecycle); toggling it with the link state is untested and could
            // tear down a live tunnel on a link flap. Router masks only.
#if MASK_VERSION == 0x091A
            knx.bau().getPrimaryDataLinkLayer()->enabled(enable);
#elif MASK_VERSION == 0x57B0
            knx.bau().getDataLinkLayer()->enabled(enable);
#endif
            if (!enable) _boundIp = IPAddress(); // Socket zu, Bindung ungültig
        }

        // True while the KNXnet/IP datalink layer is enabled (the honest "KNX-IP is running" signal,
        // distinct from the physical Ethernet/IP link). Counterpart to controlKnxIp().
        bool Module::knxIpEnabled()
        {
#if MASK_VERSION == 0x091A || MASK_VERSION == 0x07B0 // primary datalink = KNXnet/IP (091A router, 07B0 interface Bau07B0IP)
            return knx.bau().getPrimaryDataLinkLayer()->enabled();
#elif MASK_VERSION == 0x57B0
            return knx.bau().getDataLinkLayer()->enabled();
#else
            return false;
#endif
        }

        // KNX-IP-Status LED (func 11): reflects the KNXnet/IP datalink, not just the physical link.
        //   GREEN  = KNXnet/IP datalink really running
        //   ORANGE = link + IP up, but KNXnet/IP NOT running ("looks connected, KNX-IP dead")
        //   RED    = no link / no IP
        void Module::checkKnxIpStatus()
        {
            if (_knxIpLedFunc == nullptr || !_knxIpLedFunc->active()) return;

            uint8_t newState; // 1 = green, 2 = orange, 3 = red
            if (knxIpEnabled() && established())
                newState = 1; // KNXnet/IP running AND link/IP up
            else if (established())
                newState = 2; // link/IP up, but KNX-IP not running (router; on 0x07B0 knxIpEnabled is always true -> never here)
            else
                newState = 3; // no link / no IP (on 0x07B0: the honest "network down" state, was unreachable before)

            if (_knxIpLedState == newState) return;
            _knxIpLedState = newState;

            switch (newState)
            {
                case 1: // KNXnet/IP running
                    _knxIpLedFunc->color(OpenKNX::Led::Color::Green);
                    _knxIpLedFunc->activity(OpenKNX::Led::g_ipLedActivity, true);
                    break;
                case 2: // network up, KNX-IP not running
                    _knxIpLedFunc->color(OpenKNX::Led::Color::Orange);
                    _knxIpLedFunc->on(OpenKNX::Led::Capability::COLOR);
                    _knxIpLedFunc->blinking(1000, OpenKNX::Led::Capability::MONOCHROME);
                    break;
                default: // 3: no link / no IP
                    _knxIpLedFunc->color(OpenKNX::Led::Color::Red);
                    _knxIpLedFunc->on(OpenKNX::Led::Capability::COLOR);
                    _knxIpLedFunc->off(OpenKNX::Led::Capability::MONOCHROME);
                    break;
            }
        }

#if defined(ARDUINO_ARCH_RP2040) && defined(OPENKNX_ETH_W5500)
        // Startup-delay hook -> EthLinkManager re-applies a persisted fixed link mode (flash now loaded).
        void Module::processAfterStartupDelay() { _ethLink.onStartupDelay(); }
        static constexpr uint16_t NET_ETHLINK_FLASH_SIZE = 3; // EthLink owns [version=2][mode][fullDuplex]; keep in sync with EthLinkManager::flashSize()
#else
        static constexpr uint16_t NET_ETHLINK_FLASH_SIZE = 0; // no shared ETH-link block on this platform
#endif

        // Total module-flash footprint: shared ETH-link bytes (if any) + the local-override block (DEVICE_DISPLAY).
        uint16_t Module::flashSize()
        {
#ifdef DEVICE_DISPLAY_MODULE
            return NET_ETHLINK_FLASH_SIZE + NET_OVERRIDE_FLASH_SIZE;
#else
            return NET_ETHLINK_FLASH_SIZE;
#endif
        }

        // Write order MUST match read order: EthLink bytes first (unchanged), then the override block.
        void Module::writeFlash()
        {
#if defined(ARDUINO_ARCH_RP2040) && defined(OPENKNX_ETH_W5500)
            _ethLink.writeFlash();
#endif
#ifdef DEVICE_DISPLAY_MODULE
            writeLocalOverrideFlash();
#endif
        }

        void Module::readFlash(const uint8_t *data, const uint16_t size)
        {
#if defined(ARDUINO_ARCH_RP2040) && defined(OPENKNX_ETH_W5500)
            _ethLink.readFlash(data, size); // tolerates a shorter/older blob (keeps its own defaults)
#endif
#ifdef DEVICE_DISPLAY_MODULE
            if (size <= NET_ETHLINK_FLASH_SIZE)
                readLocalOverrideFlash(nullptr, 0); // no override bytes stored -> defined defaults
            else
                readLocalOverrideFlash(data + NET_ETHLINK_FLASH_SIZE, size - NET_ETHLINK_FLASH_SIZE);
#endif
        }

#ifdef DEVICE_DISPLAY_MODULE
        void Module::writeLocalOverrideFlash()
        {
            openknx.flash.writeByte(_localOverride.version);
            openknx.flash.writeByte(_localOverride.overrideActive ? 1 : 0);
            openknx.flash.writeByte(_localOverride.useStaticIP ? 1 : 0);
            for (uint8_t i = 0; i < 4; i++) openknx.flash.writeByte(_localOverride.ip[i]);
            for (uint8_t i = 0; i < 4; i++) openknx.flash.writeByte(_localOverride.subnet[i]);
            for (uint8_t i = 0; i < 4; i++) openknx.flash.writeByte(_localOverride.gateway[i]);
            for (uint8_t i = 0; i < 4; i++) openknx.flash.writeByte(_localOverride.dns[i]);
            openknx.flash.writeByte(_localOverride.linkMode);
        }

        void Module::readLocalOverrideFlash(const uint8_t *data, const uint16_t size)
        {
            _localOverride = NetworkLocalOverride(); // reset to defined defaults first
            if (data == nullptr || size < NET_OVERRIDE_FLASH_SIZE) return;
            if (data[0] != _localOverride.version) return; // unknown format version -> keep defaults

            uint16_t p = 0;
            _localOverride.version = data[p++];
            _localOverride.overrideActive = (data[p++] != 0);
            _localOverride.useStaticIP = (data[p++] != 0);
            for (uint8_t i = 0; i < 4; i++) _localOverride.ip[i] = data[p++];
            for (uint8_t i = 0; i < 4; i++) _localOverride.subnet[i] = data[p++];
            for (uint8_t i = 0; i < 4; i++) _localOverride.gateway[i] = data[p++];
            for (uint8_t i = 0; i < 4; i++) _localOverride.dns[i] = data[p++];
            _localOverride.linkMode = data[p++];
        }

        void Module::prefillZeroQuartet(uint8_t (&quartet)[4], IPAddress live)
        {
            if (quartet[0] || quartet[1] || quartet[2] || quartet[3]) return; // already set -> keep it
            for (uint8_t i = 0; i < 4; i++) quartet[i] = live[i];
        }

        void Module::setLocalStaticIpEnabled(bool enabled)
        {
            _localOverride.overrideActive = true;
            _localOverride.useStaticIP = enabled;
            if (enabled) // pre-fill any still-zero quartet from the live lease so the menu shows sane values
            {
                prefillZeroQuartet(_localOverride.ip, localIP());
                prefillZeroQuartet(_localOverride.subnet, subnetMask());
                prefillZeroQuartet(_localOverride.gateway, gatewayIP());
                prefillZeroQuartet(_localOverride.dns, nameServerIP());
            }
        }

        void Module::setLocalIp(IPAddress ip)
        {
            _localOverride.overrideActive = true;
            for (uint8_t i = 0; i < 4; i++) _localOverride.ip[i] = ip[i];
        }

        void Module::setLocalSubnet(IPAddress subnet)
        {
            _localOverride.overrideActive = true;
            for (uint8_t i = 0; i < 4; i++) _localOverride.subnet[i] = subnet[i];
        }

        void Module::setLocalGateway(IPAddress gateway)
        {
            _localOverride.overrideActive = true;
            for (uint8_t i = 0; i < 4; i++) _localOverride.gateway[i] = gateway[i];
        }

        void Module::setLocalDns(IPAddress dns)
        {
            _localOverride.overrideActive = true;
            for (uint8_t i = 0; i < 4; i++) _localOverride.dns[i] = dns[i];
        }

        bool Module::localOverrideActive() { return _localOverride.overrideActive; }

        bool Module::localUsesStaticIp()
        {
            return _localOverride.overrideActive ? _localOverride.useStaticIP : _useStaticIP;
        }

        void Module::applyLocalNetworkConfig(bool rebootToTakeEffect)
        {
            logInfoP("Apply local network override (%s, %s)", _localOverride.useStaticIP ? "static IP" : "DHCP",
                     rebootToTakeEffect ? "reboot" : "live");
            openknx.flash.save(true); // persist now -> triggers Module::writeFlash() (EthLink bytes + override block)
            if (rebootToTakeEffect)
            {
                _restartTimer = delayTimerInit(); // loop() reboots ~2 s later
                return;
            }
            loadSettings();
            resetNetwork();
        }
#endif // DEVICE_DISPLAY_MODULE

#if defined(ARDUINO_ARCH_RP2040) && defined(OPENKNX_ETH_W5500)
        // Hardware-reset the W5500 via RSTn. NOT the driver's end() (busy-waits forever on a stuck chip -> brick).
        void Module::hwResetPhy()
        {
#if defined(PIN_ETH_RES)
            pinMode(PIN_ETH_RES, OUTPUT);
            digitalWrite(PIN_ETH_RES, LOW);
            delay(2); // datasheet: RSTn low >= 500us
            digitalWrite(PIN_ETH_RES, HIGH);
            delay(60); // PLL lock + internal reset done before we touch SPI
#endif
        }

        // One bring-up attempt; on failure probe VERSIONR to classify: 0x04 = chip present but socket OPEN not
        // ready (transient, retry helps); anything else = not answering on SPI (wiring/power/dead).
        bool Module::tryBeginEth()
        {
            hwResetPhy();

            KNX_NETIF.setSPISpeed(OPENKNX_NET_SPI_SPEED);
            if (KNX_NETIF.begin()) return true;

            uint8_t ver = _ethLink.chipVersion();
            if (ver == 0x04)
                logErrorP("W5500 present (VERSIONR=0x04) but socket OPEN failed (Sn_SR != 0x42) - transient");
            else
                logErrorP("W5500 not answering on SPI (VERSIONR=0x%02X, expected 0x04)", ver);
            return false;
        }

        // Boot bring-up: a few attempts with backoff (worst case well within the active 16s watchdog window).
        bool Module::beginEthWithRetry()
        {
            for (uint8_t i = 1; i <= ETH_BEGIN_TRIES; i++)
            {
                if (tryBeginEth())
                {
                    if (i > 1) logInfoP("W5500 up on attempt %u/%u", i, ETH_BEGIN_TRIES);
                    return true;
                }
                logErrorP("W5500 begin attempt %u/%u failed", i, ETH_BEGIN_TRIES);
                delay(ETH_BEGIN_BACKOFF_MS);
            }
            return false;
        }

        // loop() hook while degraded: throttled bring-up retry; clears _ethDegraded once the chip answers,
        // then checkLinkStatus() takes back over. No reboot.
        void Module::ethSelfHeal()
        {
            // Hold the IP LED red while we are down (checkLinkStatus() - the usual LED driver - is skipped here).
            if (_ipLedFunc != nullptr && _ipLedState != 3)
            {
                _ipLedFunc->color(OpenKNX::Led::Color::Red);
                _ipLedFunc->on(OpenKNX::Led::Capability::COLOR);
                _ipLedFunc->off(OpenKNX::Led::Capability::MONOCHROME);
                _ipLedState = 3;
            }

            if (!delayCheckMillis(_lastEthHeal, ETH_HEAL_INTERVAL_MS)) return;
            _lastEthHeal = millis();

            logInfoP("W5500 self-heal: retrying bring-up...");
            if (tryBeginEth())
            {
                _ethDegraded = false;
                _ipLedState = 0; // force checkLinkStatus() to re-evaluate the LED from the real link next cycle
                _ethLink.applyEtsMode(); // the re-begin left the PHY at its default -> re-apply the ETS mode
                logInfoP("W5500 recovered - network back online");
            }
        }

        // Runtime watchdog: the W5500 can wedge after a clean start (a link-down misses it). Probe VERSIONR;
        // recover only after ETH_BAD_PROBE_LIMIT consecutive failures (debounce a single glitched read).
        void Module::checkEthHealth()
        {
            if (!delayCheckMillis(_lastEthHealth, ETH_HEALTH_INTERVAL_MS)) return;
            _lastEthHealth = millis();

            // Serialize the raw VERSIONR read against the Wiznet5500lwIP async SPI pump (the driver wraps its
            // own transactions in this same lock). Without it the reads interleave on the shared bus, VERSIONR
            // comes back garbage, and a healthy chip is mis-classified "dead" -> KNX-IP teardown.
            ethernet_arch_lwip_begin();
            const uint8_t ver = _ethLink.chipVersion();
            ethernet_arch_lwip_end();
            if (ver == 0x04)
            {
                _ethBadProbes = 0;
                return;
            }
            if (++_ethBadProbes < ETH_BAD_PROBE_LIMIT) return;

            logErrorP("W5500 stopped responding at runtime (VERSIONR=0x%02X != 0x04) after %u probes -> clean recovery", ver, _ethBadProbes);
            recoverEth();
        }

        // Brick-free runtime recovery (no reboot): detach KNX-IP, HW-reset FIRST so the driver's end() busy-wait
        // returns immediately (that unbounded wait is the old brick), end(), then hand to ethSelfHeal().
        void Module::recoverEth()
        {
            controlKnxIp(false);
            onLinkLost(); // before end(): the lwIP link-down has to reach a netif that still exists
            hwResetPhy();
            KNX_NETIF.end();
            _ethBadProbes = 0;
            _lastEthHeal = 0;    // let ethSelfHeal() attempt an immediate first re-begin
            _ethDegraded = true; // loop() -> ethSelfHeal(): bounded re-begin
        }
#endif

#if defined(ARDUINO_ARCH_ESP32) && defined(OPENKNX_ETH_W5500) && defined(KNX_IP_LAN)
        // ESP32 + W5500 hard-wedge recovery. esp_eth reacts to normal link up/down itself; this only rescues a
        // chip that stays down. Detection is link-based (established()) - unlike RP2040 we cannot raw-read
        // VERSIONR since esp_eth owns the W5500 SPI bus. After a long down: stop, pulse RSTn, re-apply link, begin.
        void Module::checkEthHealthEsp()
        {
            if (established())
            {
                _lastEthDownEsp = 0;
                _ethWasEstablishedEsp = true; // a genuine link+IP existed -> a later long-down IS a real wedge
                _ethHealBackoffStep = 0;      // healthy again -> reset the backoff ladder
                return;
            }

            // No cable / never-up is a normal idle state, not a wedge: only recover a link that was once
            // established and then died (recovering never-up was the no-cable boot thrash every ~5s).
            if (!_ethWasEstablishedEsp) return;

            uint32_t now = millis();
            if (_lastEthDownEsp == 0) _lastEthDownEsp = now;                  // established link just went down -> clock
            if ((uint32_t)(now - _lastEthDownEsp) < ETH_ESP_WEDGE_MS) return; // let esp_eth's own reconnect try first

            // Exponential backoff between attempts: 30 -> 60 -> 120s (capped) so a stuck link cannot thrash.
            uint32_t interval = ETH_ESP_HEAL_INTERVAL_MS << _ethHealBackoffStep;
            if (interval > ETH_ESP_HEAL_INTERVAL_MAX_MS) interval = ETH_ESP_HEAL_INTERVAL_MAX_MS;
            if (!delayCheckMillis(_lastEthHealEsp, interval)) return;
            _lastEthHealEsp = now;
            if (_ethHealBackoffStep < 2) _ethHealBackoffStep++; // 30 -> 60 -> 120, then hold

            openknx.common.skipLooptimeWarning(); // the ETH.end()/begin() below blocks ~100ms by design
            logErrorP("W5500 wedged (was established, link down > %lus) -> recovery: ETH stop + RSTn + re-begin", (unsigned long)(ETH_ESP_WEDGE_MS / 1000));
            controlKnxIp(false);
            KNX_NETIF.end();
            pinMode(PIN_ETH_RES, OUTPUT);
            digitalWrite(PIN_ETH_RES, LOW);
            delay(2);  // RSTn low >= 500us (datasheet)
            digitalWrite(PIN_ETH_RES, HIGH);
            delay(60); // PLL lock before SPI access
            _ethLink.applyBeforeBegin(); // re-apply forced/auto link mode before begin()
            KNX_NETIF.begin();
            KNX_NETIF.config(_staticLocalIP, _staticGatewayIP, _staticSubnetMask, _staticNameServerIP);
            _lastEthDownEsp = now; // restart the wedge clock so we wait a full window before the next attempt
        }
#endif

#if defined(ARDUINO_ARCH_RP2040) && defined(KNX_IP_LAN)
        // (Re)register the _openknx mDNS service + TXT via lwIP. Idempotent (del-before-add) so a link flap or
        // W5500 recovery re-registers cleanly. Called from setup() and the link-up edge in checkLinkStatus().
        void Module::registerOpenknxMdns()
        {
            if (!(!knx.configured() || ParamNET_mDNS)) return; // mDNS disabled

            // remove the previous registration if its netif still exists (skip after a recovery -> old netif gone)
            if (_openknxMdnsNetif != nullptr && _openknxMdnsSlot >= 0)
                for (struct netif *n = netif_list; n != nullptr; n = n->next)
                    if (n == _openknxMdnsNetif)
                    {
                        mdns_resp_del_service(_openknxMdnsNetif, (u8_t)_openknxMdnsSlot);
                        break;
                    }
            _openknxMdnsSlot = -1;
            _openknxMdnsNetif = nullptr;

            // rebuild TXT each time -- address/configured can change across a reprogram
            snprintf(_openknxMdnsTxt[0], sizeof(_openknxMdnsTxt[0]), "serial=%s", openknx.info.humanSerialNumber().c_str());
            snprintf(_openknxMdnsTxt[1], sizeof(_openknxMdnsTxt[1]), "version=%s", openknx.info.humanFirmwareVersion().c_str());
            snprintf(_openknxMdnsTxt[2], sizeof(_openknxMdnsTxt[2]), "firmware=%s", openknx.info.humanFirmwareNumber().c_str());
            snprintf(_openknxMdnsTxt[3], sizeof(_openknxMdnsTxt[3]), "address=%s", openknx.info.humanIndividualAddress().c_str());
            snprintf(_openknxMdnsTxt[4], sizeof(_openknxMdnsTxt[4]), "ota=%s", _otaPortString);
            snprintf(_openknxMdnsTxt[5], sizeof(_openknxMdnsTxt[5]), "configured=%u", knx.configured() ? 1u : 0u);
            snprintf(_openknxMdnsTxt[6], sizeof(_openknxMdnsTxt[6]), "otamode=%s", otaModeStr());
            _openknxMdnsTxtCount = 7;

            struct netif *n = netif_default ? netif_default : netif_list; // active iface (DHCP default; first at boot)
            if (n != nullptr)
            {
                s8_t slot = mdns_resp_add_service(n, _hostName, "_openknx", DNSSD_PROTO_TCP, 65535, _openknxMdnsTxtCb, nullptr);
                if (slot >= 0)
                {
                    _openknxMdnsSlot = slot;
                    _openknxMdnsNetif = n;
                    mdns_resp_announce(n);
                }
                else
                    logErrorP("mDNS: _openknx add_service failed (%d)", (int)slot);
            }
        }
#endif

#if defined(ARDUINO_ARCH_RP2040) && defined(KNX_IP_LAN) && defined(OPENKNX_ETH_W5500_MAINLOOP_RX)
        // Drive W5500 RX + lwIP timers from the main loop (the driver is built INT-less here). Idle the
        // framework async timer on the first pass (not in setup(), which would strand boot-time DHCP).
        void Module::pumpEthernet()
        {
            if (_ethDegraded) return;

            static bool asyncTimerIdled = false;
            if (!asyncTimerIdled)
            {
                lwipPollingPeriod(3600000); // effectively never
                asyncTimerIdled = true;
            }
            ethernet_arch_lwip_begin();
            KNX_NETIF.handlePackets(); // RX
            // lwIP timers are ms-granularity; throttle to ~1 ms so we don't burn TP-hot-path cycles at the kHz loop rate
            static uint32_t lastTimeoutMs = 0;
            uint32_t nowMs = millis();
            if (nowMs != lastTimeoutMs)
            {
                lastTimeoutMs = nowMs;
                sys_check_timeouts();
            }
            ethernet_arch_lwip_end();
        }

        // OTA RX handover. The framework OTA read (_runUpdate) is fully blocking and never returns to loop()
        // mid-transfer, so the once-per-loop pumpEthernet() cannot feed it and the socket RX buffer stalls
        // (glacial/failing OTA + display hitch). For the transfer we hand RX back to the framework async pump
        // (HW-alarm IRQ) which fires the W5500 RX drain even inside that blocking read; loop() then skips
        // pumpEthernet() while _otaActive. 2 ms poll = aggressive OTA RX; on=false restores idle.
        void Module::setEthOtaPump(bool on)
        {
            lwipPollingPeriod(on ? 2 : 3600000); // 2 ms = aggressive OTA RX; 3.6M ms = effectively never (idle)
            ethernet_arch_lwip_begin();
            ethernet_arch_lwip_end(); // context-leave -> re-arm the async timeout worker at the new period
        }
#endif

#ifdef OPENKNX_PING
        void Module::ping(IPAddress target, std::function<void(IPAddress, bool, uint32_t)> callback, uint32_t timeoutMs)
        {
            _pingHandler.ping(target, callback, timeoutMs);
        }

        void Module::ping(IPAddress target, std::function<void(IPAddress, bool)> callback, uint8_t retries, uint32_t timeoutMs)
        {
            _pingHandler.ping(target, callback, retries, timeoutMs);
        }

        void Module::ping(const std::string &host, std::function<void(IPAddress, bool, uint32_t)> callback, uint32_t timeoutMs)
        {
            _pingHandler.ping(host, callback, timeoutMs);
        }

        void Module::ping(const std::string &host, std::function<void(IPAddress, bool)> callback, uint8_t retries, uint32_t timeoutMs)
        {
            _pingHandler.ping(host, callback, retries, timeoutMs);
        }
#endif

#ifdef OPENKNX_WEBCLIENT_TEST
        /**
         * @brief Arm a URL download onto one of the drives. Returns immediately.
         * @details The bytes are handed to FileManager::writeChunk offset by offset -- the same sink the
         *          web upload feeds -- so nothing is buffered and the size is bounded by the drive, not
         *          by RAM. Up to three redirects are followed, which a GitHub release link needs.
         */
        bool Module::httpFetchStart(const std::string &url, const std::string &rawPath)
        {
            if (_fetch.state == HttpFetch::State::RUNNING)
            {
                _fetch.error = "a download is already running";
                return false;
            }
            if (url.empty())
            {
                _fetch = HttpFetch();
                _fetch.state = HttpFetch::State::FAILED;
                _fetch.error = "no url";
                return false;
            }

            std::string rel;
            int drive = FileManager::driveFromPath(rawPath, rel);
            _fetch = HttpFetch();
            _fetch.state = HttpFetch::State::RUNNING;
            _fetch.drive = drive;
            _fetch.path = rel;
            _fetch.startedMs = millis();

            _fetchArm = [this](std::string u) {
                webclient.get(u)
                    .maxBodySize(0) // stream: the body never sits in RAM
                    .onData([this](const uint8_t *d, size_t n) {
                        if (FileManager::writeChunk(_fetch.drive, _fetch.path.c_str(), _fetch.bytes, d, n))
                            _fetch.bytes += n;
                        else
                            _fetch.error = "write failed";
                    })
                    .onDone([this](const OpenKNX::Network::Webclient::Response &r) {
                        _fetch.status = r.status();
                        if (r.status() >= 301 && r.status() <= 308 && _fetch.hops < 3)
                        {
                            std::string loc = r.header("Location");
                            if (!loc.empty())
                            {
                                _fetch.hops++;
                                logInfoP("redirect %u -> %s", (unsigned)_fetch.hops, loc.c_str());
                                _fetchArm(loc);
                                return;
                            }
                        }
                        bool ok = r.success() && r.status() >= 200 && r.status() < 300 && _fetch.error.empty();
                        _fetch.state = ok ? HttpFetch::State::DONE : HttpFetch::State::FAILED;
                        if (!ok && _fetch.error.empty()) _fetch.error = "HTTP " + std::to_string(r.status());
                        logInfoP("%s: HTTP %u, %u bytes, %u ms", _fetch.path.c_str(), (unsigned)r.status(),
                                 (unsigned)_fetch.bytes, (unsigned)(millis() - _fetch.startedMs));
                    })
                    .send();
            };
            logInfoP("GET %s -> %s", url.c_str(), rawPath.c_str());
            _fetchArm(url);
            return true;
        }
#endif

    } // namespace Network
} // namespace OpenKNX

NetworkModule openknxNetwork;
#endif
