#if defined(KNX_IP_WIFI) || defined(KNX_IP_LAN)
#pragma once
#include "NetworkConsole.h"
#include "OpenKNX.h"
#include "OpenKNX//Led/FunctionManager.h"
#include "strings.h"
#include <functional>

// KNX-IP-Status LED function id (value 11 in the SLEDFunc dropdown). Guarded so a future
// central definition in OGM-Common/Led/FunctionManager.h takes precedence if added there.
#ifndef OPENKNX_LEDFUNC_KNXIP_STATE
#define OPENKNX_LEDFUNC_KNXIP_STATE 11
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

#include "driver/EthLinkManager.h" // ETH link mode + auto-fallback ladder (self-guarded: W5500 LAN only)

namespace OpenKNX::Led
{
    extern uint32_t g_ipLedActivity;
}

typedef std::function<void(bool)> NetworkChangeCallback;

// Locally-editable config that can override the ETS/property IP settings. Persisted in the
// NetworkModule module-flash block. No valid block (fresh flash / size==0) -> defaults below:
// override inactive, ETS/property behaviour unchanged.
// linkMode: 0=auto-negotiation, 1=10 Mbit, 2=100 Mbit (matches EthLinkManager::setMode()).
#ifdef DEVICE_DISPLAY_MODULE // on-device IP override is a DeviceDisplay-menu-only feature (set via the network settings menu)
struct NetworkLocalOverride
{
    bool overrideActive = false; // false -> ignore this block, keep ETS/property config
    bool useStaticIP = false;    // true -> use ip/subnet/gateway/dns below; false -> DHCP
    uint8_t ip[4] = {0, 0, 0, 0};
    uint8_t subnet[4] = {0, 0, 0, 0};
    uint8_t gateway[4] = {0, 0, 0, 0};
    uint8_t dns[4] = {0, 0, 0, 0};
    uint8_t linkMode = 0; // 0=auto, 1=10 Mbit, 2=100 Mbit
    uint8_t version = 1;  // on-flash format version of this block
};
#endif

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

#ifdef DEVICE_DISPLAY_MODULE
    // Setters only mutate the in-RAM _localOverride; call applyLocalNetworkConfig() afterwards to persist +
    // apply. Enabling static IP activates the override and pre-fills any still-zero quartet from the live lease.
    void setLocalStaticIpEnabled(bool enabled);        // true -> static IP from the ip/subnet/gateway/dns setters; false -> DHCP
    void setLocalIp(IPAddress ip);                      // host address for static IP mode
    void setLocalSubnet(IPAddress subnet);              // subnet mask for static IP mode
    void setLocalGateway(IPAddress gateway);            // default gateway for static IP mode
    void setLocalDns(IPAddress dns);                    // DNS server for static IP mode
    void applyLocalNetworkConfig(bool rebootToTakeEffect); // persist + apply (live re-init or planned reboot)
    bool localOverrideActive();                         // true while the local override supersedes ETS/property config
    // Currently intended IP mode: the override's useStaticIP when active, otherwise the mode resolved from
    // ETS/property config (_useStaticIP). Drives menu field visibility/lock (static -> editable, DHCP -> locked).
    bool localUsesStaticIp();

    // OTA status for the display's system-priority OTA overlay.
    bool otaActive() const { return _otaActive; }       // true between OTA onStart and onEnd/onError
    uint8_t otaProgress() const { return _otaPercent; } // fine OTA progress 0..100 (every chunk)
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
    uint8_t _knxIpLedState = 0;
    OpenKNX::Led::FunctionGroup *_knxIpLedFunc = nullptr;

#ifdef ARDUINO_ARCH_ESP32
    const uint16_t _otaPort = 3232;
    const char *_otaPortString = "3232";
#else
    const uint16_t _otaPort = 2040;
    const char *_otaPortString = "2040";
#endif
    uint8_t _otaProgress = 0; // last %-step logged (gates 10% heartbeat log)
    bool _otaActive = false;  // true while an OTA update runs (onStart..onEnd/onError)
    uint8_t _otaPercent = 0;  // fine OTA progress 0..100, updated on every onProgress
    IPAddress _staticLocalIP;
    IPAddress _staticSubnetMask;
    IPAddress _staticGatewayIP;
    IPAddress _staticNameServerIP;
    IPAddress _boundIp; // last IP the KNX multicast socket was bound to (rebind only on change)

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
    void checkKnxIpStatus(); // drives the KNX-IP-Status LED (func 11): green=KNXnet/IP running, orange=link up but KNX-IP down, red=no link
    bool knxIpEnabled();     // is the KNXnet/IP datalink layer currently enabled?

#if defined(ARDUINO_ARCH_RP2040) && defined(OPENKNX_ETH_W5500)
    // ---- W5500 clean self-heal (no MCU reboot) ----
    // The W5500 sometimes doesn't answer right after (re)boot, and can wedge after a clean start. At boot we
    // retry begin() with an RSTn reset between tries; at runtime checkEthHealth() probes VERSIONR and, on a
    // wedge, recoverEth() re-inits. Never the driver's end() alone (busy-waits forever on a stuck chip ->
    // brick). Heals as soon as the chip answers; no reboot needed.
    bool _ethDegraded = false;   // true: W5500 not up yet, loop() is running the self-heal retry
    uint32_t _lastEthHeal = 0;   // millis() of the last self-heal attempt (throttle)
    uint32_t _lastEthHealth = 0; // millis() of the last runtime chip-health probe (throttle)
    uint8_t _ethBadProbes = 0;   // consecutive VERSIONR != 0x04 reads (debounce before recovery)
    void hwResetPhy();           // pulse PIN_ETH_RES (RSTn low >=500us) + settle before any SPI access
    bool tryBeginEth();          // one clean bring-up: HW reset + KNX_NETIF.begin(), VERSIONR-classify on fail
    bool beginEthWithRetry();    // boot: tryBeginEth() up to ETH_BEGIN_TRIES with backoff
#if defined(ARDUINO_ARCH_RP2040) && defined(KNX_IP_LAN)
    void registerOpenknxMdns();  // (re)register the _openknx mDNS service+TXT via lwIP; idempotent
    bool _mdnsWasEstablished = false; // rising-edge tracking for the re-registration in checkLinkStatus()
#endif
#if defined(ARDUINO_ARCH_RP2040) && defined(KNX_IP_LAN) && defined(OPENKNX_ETH_W5500_MAINLOOP_RX)
    void pumpEthernet();         // drive W5500 RX + lwIP timers from the main loop (INT off), called from loop()
    void setEthOtaPump(bool on); // OTA on/off: hand RX to the async IRQ pump so the blocking OTA read isn't loop-starved
#endif
    void ethSelfHeal();          // loop(): throttled tryBeginEth() while _ethDegraded, clears it on success
    void checkEthHealth();       // loop(): periodic VERSIONR probe; triggers recoverEth() on repeated failure
    void recoverEth();           // runtime: HW-reset -> safe end() -> hand to ethSelfHeal() (no reboot/brick)
    static constexpr uint8_t ETH_BEGIN_TRIES = 4;
    static constexpr uint16_t ETH_BEGIN_BACKOFF_MS = 150;
    static constexpr uint32_t ETH_HEAL_INTERVAL_MS = 5000;
    static constexpr uint32_t ETH_HEALTH_INTERVAL_MS = 5000;
    static constexpr uint8_t ETH_BAD_PROBE_LIMIT = 3;
#endif

#if defined(ARDUINO_ARCH_ESP32) && defined(OPENKNX_ETH_W5500) && defined(KNX_IP_LAN)
    // W5500-on-ESP hard-wedge recovery (checkEthHealthEsp): link-based, since esp_eth owns the SPI bus and
    // we cannot raw-read VERSIONR like the RP2040 path. Native-EMAC boards (REG1/LAN8720) do not define it.
    void checkEthHealthEsp();
    uint32_t _lastEthDownEsp = 0;       // millis() the link first went down (0 = up/established)
    uint32_t _lastEthHealEsp = 0;       // throttle between recovery attempts
    bool _ethWasEstablishedEsp = false; // sticky: a real link+IP came up at least once this session
    uint8_t _ethHealBackoffStep = 0;    // exponential backoff index for recovery attempts
    static constexpr uint32_t ETH_ESP_WEDGE_MS = 30000;              // established-then-lost this long -> recover
    static constexpr uint32_t ETH_ESP_HEAL_INTERVAL_MS = 30000;      // base gap; backed off 30->60->120s (capped)
    static constexpr uint32_t ETH_ESP_HEAL_INTERVAL_MAX_MS = 120000; // backoff cap
#endif

    // ---- ETH link mode control ----
    // Owned by EthLinkManager (fixed-speed override + auto-fallback ladder), which drives the raw link
    // drivers in driver/. NetworkModule just forwards its Module hooks + the ~2 Hz link tick to it;
    // NetworkConsole reaches it as a friend. See driver/EthLinkManager.{h,cpp}.
#if defined(KNX_IP_LAN) && (defined(ARDUINO_ARCH_RP2040) || defined(ARDUINO_ARCH_ESP32))
    EthLinkManager _ethLink{*this};
#if defined(ARDUINO_ARCH_RP2040)
    // Startup-delay hook is forwarded to _ethLink (re-applies a persisted fixed link mode once flash loaded).
    void processAfterStartupDelay() override;
#endif
#endif

    // Module-flash persistence. On RP2040+KNX_IP_LAN this block ALSO carries the EthLinkManager fixed-mode
    // bytes: writeFlash()/readFlash() chain _ethLink first, then the override block. size==0 -> defaults.
    uint16_t flashSize() override;
    void writeFlash() override;
    void readFlash(const uint8_t *data, const uint16_t size) override;

#ifdef DEVICE_DISPLAY_MODULE
    // Held in RAM; loaded from / stored to module flash by the callbacks above.
    NetworkLocalOverride _localOverride;
    static constexpr uint16_t NET_OVERRIDE_FLASH_SIZE = 20; // version + 2 flags + 4*4 addr bytes + linkMode
    void writeLocalOverrideFlash();                         // append the override block to the module-flash stream
    void readLocalOverrideFlash(const uint8_t *data, const uint16_t size); // parse it back (defaults if absent/short)
    // Copy the given live value into a still-all-zero override quartet (leaves an already-edited quartet
    // untouched), so DHCP->static pre-fills from the acquired lease without clobbering user input.
    static void prefillZeroQuartet(uint8_t (&quartet)[4], IPAddress live);
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
