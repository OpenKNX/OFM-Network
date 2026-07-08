#if defined(KNX_IP_WIFI) || defined(KNX_IP_LAN)
#pragma once
#include "NetworkConsole.h"
#include "OpenKNX.h"
#include "OpenKNX//Led/FunctionManager.h"
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

#include "driver/EthLinkManager.h" // ETH link mode + auto-fallback ladder (self-guarded: W5500 LAN only)

namespace OpenKNX::Led
{
    extern uint32_t g_ipLedActivity;
}

typedef std::function<void(bool)> NetworkChangeCallback;

// NetworkConsole owns the console command parsing/help for this module and needs
// access to a few private members/methods (see friend declaration below).
class NetworkConsole;

class NetworkModule : public OpenKNX::Module
{
    // Console command handling lives in NetworkConsole; grant it access to the
    // private ETH-link state/methods and the other members it drives.
    friend class NetworkConsole;

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
#if MASK_VERSION == 0x091A || MASK_VERSION == 0x57B0
    void setMulticastAddress(IPAddress address, bool rebootToTakeEffect);
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
    uint32_t _ipStableSince = 0; // link-established timestamp; debounces the "established" + IP-info output
    bool _useStaticIP = false;
    bool _otaAllowed = false;
    bool _otaHandle = false;
    uint8_t _ipLedState = 0;
    OpenKNX::Led::FunctionGroup *_ipLedFunc = nullptr;

#ifdef ARDUINO_ARCH_ESP32
    const uint16_t _otaPort = 3232;
    const char *_otaPortString = "3232";
#else
    const uint16_t _otaPort = 2040;
    const char *_otaPortString = "2040";
#endif
    uint8_t _otaProgress = 0;
    IPAddress _staticLocalIP;
    IPAddress _staticSubnetMask;
    IPAddress _staticGatewayIP;
    IPAddress _staticNameServerIP;

#ifdef ARDUINO_ARCH_ESP32
    bool espConnected = false;
#endif

    char _hostName[25] = {};

    bool _currentLinkState = false;
    uint32_t _lastLinkCheck = 0;
    uint32_t _restartTimer = 0;

    void initPhy();
    void initIp();
    void loadSettings();
    void checkLinkStatus();
    void checkIpStatus();
    void loadCallbacks(bool state);
    void handleOTA();
    void controlKnxIp(bool state);

    // ---- ETH link mode control ----
    // Owned by EthLinkManager (fixed-speed override + auto-fallback ladder), which drives the raw link
    // drivers in driver/. NetworkModule just forwards its Module hooks + the ~2 Hz link tick to it;
    // NetworkConsole reaches it as a friend. See driver/EthLinkManager.{h,cpp}.
#if defined(KNX_IP_LAN) && (defined(ARDUINO_ARCH_RP2040) || defined(ARDUINO_ARCH_ESP32))
    EthLinkManager _ethLink{*this};
#if defined(ARDUINO_ARCH_RP2040)
    // OpenKNX::Module persistence + startup-delay hooks (forwarded to _ethLink; RP2040 uses module flash).
    uint16_t flashSize() override;
    void writeFlash() override;
    void readFlash(const uint8_t *data, const uint16_t size) override;
    void processAfterStartupDelay() override;
#endif
#endif

#ifdef KNX_IP_WIFI
    char _wifiSSID[33] = {};
    char _wifiPassphrase[64] = {};
#endif

    std::vector<NetworkChangeCallback> _callback;

    // Console command handling is delegated to NetworkConsole (see NetworkConsole.{h,cpp}).
    NetworkConsole _console{*this};
};

extern NetworkModule openknxNetwork;
#endif