#if defined(KNX_IP_WIFI) || defined(KNX_IP_LAN)
#pragma once
#include "OpenKNX.h"
#include "OpenKNX/Led/FunctionManager.h"

// KNX-IP-Status LED function id (value 11 in the SLEDFunc dropdown). Guarded so a future
// central definition in OGM-Common/Led/FunctionManager.h takes precedence if added there.
#ifndef OPENKNX_LEDFUNC_KNXIP_STATE
#define OPENKNX_LEDFUNC_KNXIP_STATE 11
#endif
#include "OpenKNX/Network/Ping/Handler.h"
#ifdef OPENKNX_WEBSERVER
#include "OpenKNX/Network/Webserver/Webserver.h"
#endif
#ifdef OPENKNX_WEBCONSOLE
#include "OpenKNX/Network/Webserver/Webconsole.h"
#endif
#ifdef OPENKNX_WEBFS
#include "OpenKNX/Network/Webserver/FileManager.h"
#endif
#ifdef OPENKNX_WEBMONITOR
#include "OpenKNX/Network/Webserver/GroupMonitor.h"
#include "OpenKNX/Network/GATable.h"
#endif
#ifdef OPENKNX_MQTT
#include "OpenKNX/Network/MQTT/Module.h"
#endif
#ifdef OPENKNX_WEBCLIENT
#include "OpenKNX/Network/Webclient/Handler.h"
#endif
#include "strings.h"
#include <functional>

#if defined(KNX_IP_LAN) && (defined(ARDUINO_ARCH_RP2040) || defined(ARDUINO_ARCH_ESP32))
#include "driver/EthLinkManager.h" // ETH link mode + auto-fallback ladder (self-guarded: W5500 LAN only)
#endif

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

namespace OpenKNX::Led
{
    extern uint32_t g_ipLedActivity;
}

typedef std::function<void(bool)> NetworkChangeCallback;

namespace OpenKNX
{
    namespace Network
    {

#ifdef DEVICE_DISPLAY_MODULE // on-device IP override: a DeviceDisplay network-settings-menu-only feature
        struct NetworkLocalOverride
        {
            bool overrideActive = false; // false -> ignore this block, keep ETS/property config
            bool useStaticIP = false;    // true -> use ip/subnet/gateway/dns below; false -> DHCP
            uint8_t ip[4] = {0, 0, 0, 0};
            uint8_t subnet[4] = {0, 0, 0, 0};
            uint8_t gateway[4] = {0, 0, 0, 0};
            uint8_t dns[4] = {0, 0, 0, 0};
            uint8_t linkMode = 0; // 0=auto, 1=10 Mbit, 2=100 Mbit (matches EthLinkManager::setMode())
            uint8_t version = 1;  // on-flash format version of this block
        };
#endif

        class Module : public OpenKNX::Module
        {
          public:
            const std::string name() override;
            const std::string version() override;
            void init() override;
            void loop(bool configured) override;
            void setup(bool configured) override;
            void resetNetwork();

            void savePower() override;
            bool restorePower() override;
            void showInformations() override;
            bool processCommand(const std::string cmd, bool debugKo);
            void showHelp() override;
            void showNetworkInformations(bool console = false);

            bool processFunctionProperty(uint8_t objectIndex, uint8_t propertyId, uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength) override;

#ifdef KNX_IP_WIFI
            void saveWifiSettings(const char *ssid, const char *passphrase, bool scheduleRebootToTakeEffect);
            void readWifiSettings();
#endif

#ifdef HAS_USB
            void fillNetworkFile(UsbExchangeFile *file);
#ifdef KNX_IP_WIFI
            void fillWifiFile(UsbExchangeFile *file);
            bool readWifiFile(UsbExchangeFile *file);
#endif
#endif

            void registerCallback(NetworkChangeCallback callback);

            bool connected();
            bool established();
            IPAddress localIP();
            IPAddress subnetMask();
            IPAddress gatewayIP();
            IPAddress nameServerIP();
            std::string phyMode();
            void macAddress(uint8_t *addr);
            const char *hostName() const { return _hostName; }
#if MASK_VERSION == 0x091A || MASK_VERSION == 0x57B0
            void setMulticastAddress(IPAddress address, bool rebootToTakeEffect);
#endif

#ifdef DEVICE_DISPLAY_MODULE
            // On-device IP override (DeviceDisplay network-settings menu). Setters only mutate the in-RAM
            // _localOverride; call applyLocalNetworkConfig() afterwards to persist + apply.
            void setLocalStaticIpEnabled(bool enabled); // true -> static IP from the setters below; false -> DHCP
            void setLocalIp(IPAddress ip);
            void setLocalSubnet(IPAddress subnet);
            void setLocalGateway(IPAddress gateway);
            void setLocalDns(IPAddress dns);
            void applyLocalNetworkConfig(bool rebootToTakeEffect); // persist + apply (live re-init or planned reboot)
            bool localOverrideActive();                            // true while the local override supersedes ETS config
            bool localUsesStaticIp();                              // intended mode: override's when active, else _useStaticIP
            // OTA status for the display's system-priority OTA overlay.
            bool otaActive() const { return _otaActive; }       // true between OTA onStart and onEnd/onError
            uint8_t otaProgress() const { return _otaPercent; } // fine OTA progress 0..100 (every chunk)
#endif

#ifdef OPENKNX_PING
            void ping(IPAddress target, std::function<void(IPAddress, bool, uint32_t)> callback = nullptr,
                      uint32_t timeoutMs = OPENKNX_PING_TIMEOUT);
            void ping(IPAddress target, std::function<void(IPAddress, bool)> callback,
                      uint8_t retries = 2, uint32_t timeoutMs = OPENKNX_PING_TIMEOUT);
            void ping(const std::string &host, std::function<void(IPAddress, bool, uint32_t)> callback = nullptr,
                      uint32_t timeoutMs = OPENKNX_PING_TIMEOUT);
            void ping(const std::string &host, std::function<void(IPAddress, bool)> callback,
                      uint8_t retries = 2, uint32_t timeoutMs = OPENKNX_PING_TIMEOUT);
#endif

#ifdef OPENKNX_WEBSERVER
            OpenKNX::Network::Webserver webserver;
#endif
#ifdef OPENKNX_WEBCONSOLE
            OpenKNX::Network::Webconsole webconsole;
#endif
#ifdef OPENKNX_WEBFS
            OpenKNX::Network::FileManager filemanager;
#endif
#if defined(OPENKNX_WEBMONITOR) && (MASK_VERSION == 0x07B0)
            OpenKNX::Network::GroupMonitor groupmonitor;
            OpenKNX::Network::GATable gatable;
#endif
#ifdef OPENKNX_MQTT
            OpenKNX::Network::MQTT::Module mqtt;
#endif
#ifdef OPENKNX_WEBCLIENT
            OpenKNX::Network::Webclient::Handler webclient;
#endif

#ifdef ARDUINO_ARCH_ESP32
            void esp32NetworkEvent(arduino_event_id_t event);
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
            bool _otaAllowed = false;
            bool _otaHandle = false;
            uint8_t _ipLedState = 0;
            OpenKNX::Led::FunctionGroup *_ipLedFunc = nullptr;
            uint8_t _knxIpLedState = 0;
            OpenKNX::Led::FunctionGroup *_knxIpLedFunc = nullptr;

#ifdef ARDUINO_ARCH_ESP32
            const uint16_t _otaPort = 3232;
            const char *_otaPortString = "3232";
#else
            const uint16_t _otaPort = 2040;
            const char *_otaPortString = "2040";
#endif
            uint8_t _otaProgress = 0; // last %-step logged (gates the 10% heartbeat log)
            bool _otaActive = false;  // true while an OTA update runs (onStart..onEnd/onError) -> display overlay
            uint8_t _otaPercent = 0;  // fine OTA progress 0..100, updated on every onProgress
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
#if defined(KNX_IP_LAN) && (defined(ARDUINO_ARCH_RP2040) || defined(ARDUINO_ARCH_ESP32))
            EthLinkManager _ethLink{*this}; // ETH link-speed override + auto-fallback ladder (W5500 LAN)
#endif

            void initPhy();
            void initIp();
            void loadSettings();
            void checkLinkStatus();
            void checkIpStatus();
            void loadCallbacks(bool state);
            void handleMDNS();
            void handleOTA();
            void controlKnxIp(bool state);
            void checkKnxIpStatus(); // drives the KNX-IP-Status LED (func 11): green=KNXnet/IP running, orange=link up but KNX-IP down, red=no link
            bool knxIpEnabled();     // is the KNXnet/IP datalink layer currently enabled?
#if defined(ARDUINO_ARCH_RP2040) && defined(OPENKNX_ETH_W5500)
            void processAfterStartupDelay() override; // EthLinkManager re-applies the persisted fixed link mode
#endif
            // Module-flash persistence. writeFlash()/readFlash() chain the EthLink fixed-mode bytes first
            // (RP2040+W5500 only), then the local-override block (DEVICE_DISPLAY only). size==0 -> defaults.
            uint16_t flashSize() override;
            void writeFlash() override;
            void readFlash(const uint8_t *data, const uint16_t size) override;
#ifdef DEVICE_DISPLAY_MODULE
            NetworkLocalOverride _localOverride;                    // RAM copy; (de)serialized to module flash
            static constexpr uint16_t NET_OVERRIDE_FLASH_SIZE = 20; // version + 2 flags + 4*4 addr bytes + linkMode
            void writeLocalOverrideFlash();
            void readLocalOverrideFlash(const uint8_t *data, const uint16_t size);
            static void prefillZeroQuartet(uint8_t (&quartet)[4], IPAddress live);
#endif

#ifdef OPENKNX_PING
            OpenKNX::Network::Ping::Handler _pingHandler;
#endif

#ifdef KNX_IP_WIFI
            char _wifiSSID[33] = {};
            char _wifiPassphrase[64] = {};
#endif

            std::vector<NetworkChangeCallback> _callback;
        };

    } // namespace Network
} // namespace OpenKNX

using NetworkModule = OpenKNX::Network::Module;
extern NetworkModule openknxNetwork;
#endif
