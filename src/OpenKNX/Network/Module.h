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
#include "OpenKNX/Network/TelegramJson.h" // defines OPENKNX_TELEGRAMJSON
// buildTelegramJson() converts the decoded value from Latin-15 to UTF-8.
#if defined(OPENKNX_WEBSERVER) || defined(OPENKNX_TELEGRAMJSON)
    #ifndef OPENKNX_CHARSET
        #define OPENKNX_CHARSET
    #endif
#endif
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
#endif
#ifdef OPENKNX_TELEGRAMJSON
#include "OpenKNX/Network/GATable.h" // DPT lookup for the decoded value
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

// The USB-exchange hooks are optional in two independent ways: a product may compile the module out
// with OPENKNX_USB_EXCHANGE_IGNORE, or drop the library from lib/ entirely. Either one has to silence
// them here -- HAS_USB alone only says the board has native USB, not that the module is around.
#if defined(HAS_USB) && !defined(OPENKNX_USB_EXCHANGE_IGNORE) && __has_include("UsbExchangeModule.h")
    #define OPENKNX_NETWORK_USB_EXCHANGE
#endif

#ifdef OPENKNX_NETWORK_USB_EXCHANGE
#include "UsbExchangeModule.h"
#endif

namespace OpenKNX::Led
{
    extern uint32_t g_ipLedActivity;
}

#if MASK_VERSION == 0x07B0 || MASK_VERSION == 0x091A
namespace TPUart
{
    class DataLinkLayer;
}
#endif

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

#ifdef OPENKNX_NETWORK_USB_EXCHANGE
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

            // Connection age + reconnect history, in uptime() seconds (monotonic; millis() wraps at 49.7 d).
            uint32_t netUptimeSec() const;      // since the link came up; 0 = link down
            uint32_t netDowntimeSec() const;    // since the link went down; 0 = link up or never down
            uint32_t netEstablishedSec() const; // since IP was established; 0 = not established
            uint32_t netLastDowntimeSec() const { return _lastDowntimeSec; }
            uint16_t netReconnects() const { return _reconnects; }
#if MASK_VERSION == 0x091A || MASK_VERSION == 0x57B0
            void setMulticastAddress(IPAddress address, bool rebootToTakeEffect);
#endif
#if MASK_VERSION == 0x07B0 || MASK_VERSION == 0x091A
            // Abstracts the BAU accessor difference: a plain TP device (0x07B0)
            // reaches its TP-UART via getDataLinkLayer(); a TP+IP router (0x091A)
            // via getSecondaryDataLinkLayer() -- getDataLinkLayer() there is the IP side.
            TPUart::DataLinkLayer &tpUart();
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
#if defined(OPENKNX_TELEGRAMJSON) && defined(OPENKNX_WEBMONITOR)
            OpenKNX::Network::GroupMonitor groupmonitor;
#endif
#ifdef OPENKNX_TELEGRAMJSON
            OpenKNX::Network::GATable gatable;
#endif
#ifdef OPENKNX_MQTT
            OpenKNX::Network::MQTT::Module mqtt;
#endif
#ifdef OPENKNX_WEBCLIENT
#ifdef OPENKNX_WEBCLIENT_TEST
            // One URL download at a time, shared by the console command and the web interface so the
            // two cannot feed the same file from two directions. The transfer runs in webclient.loop();
            // start() only arms it and returns, which is what keeps the web request non-blocking.
            struct HttpFetch
            {
                enum class State : uint8_t { IDLE, RUNNING, DONE, FAILED };
                State state = State::IDLE;
                int drive = 0;               // FileManager drive id
                std::string path;            // path on that drive
                std::string error;           // reason when state == FAILED
                uint32_t bytes = 0;
                uint32_t startedMs = 0;
                uint16_t status = 0;         // last HTTP status
                uint8_t hops = 0;            // redirects followed
            };
            bool httpFetchStart(const std::string &url, const std::string &rawPath);
            const HttpFetch &httpFetch() const { return _fetch; }
#endif

            OpenKNX::Network::Webclient::Handler webclient;
#ifdef OPENKNX_WEBCLIENT_TEST
            HttpFetch _fetch;
            std::function<void(std::string)> _fetchArm;
#endif
#endif

#ifdef ARDUINO_ARCH_ESP32
            void esp32NetworkEvent(arduino_event_id_t event);
#endif

          private:
            IPAddress GetIpProperty(uint8_t PropertyId);
            void SetIpProperty(uint8_t PropertyId, IPAddress IPAddress);
            uint8_t GetByteProperty(uint8_t PropertyId);
            void SetByteProperty(uint8_t PropertyId, uint8_t value);

#if defined(OPENKNX_TELEGRAMJSON) && defined(OPENKNX_MQTT)
            // ParamNET_MQTTTPRawData: every received TP telegram as JSON on
            // <prefix>/knx/tp/telegramm. Lives here, not in MQTT::Module -- that one is
            // the MQTT API and has no business knowing about KNX frames.
            void publishTelegram(TPUart::Frame &frame); // TP-UART callback
            std::string _telegramTopic;                 // absolute, built once in setup()
            OpenKNX::Format::JSON::Writer _telegramJson; // reset() keeps its capacity
#endif

            bool _powerSave = false;
            bool _ipShown = false;
            uint32_t _ipStableSince = 0; // link-established timestamp; debounces the "established" + IP-info output
            bool _useStaticIP = false;
#ifdef DEVICE_DISPLAY_MODULE
            bool _netSpeedWidgetAdded = false;
#endif
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
            IPAddress _boundIp; // IP, auf die der KNX-Multicast-Socket gebunden ist; leer = nicht gebunden

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

            // uptime() seconds; up/down comes from _currentLinkState/_ipShown, not a zero stamp.
            uint32_t _linkUpSinceSec = 0;      // last link-up edge
            uint32_t _linkDownSinceSec = 0;    // last link-down edge
            uint32_t _establishedSinceSec = 0; // moment "Network established" was reached
            uint32_t _lastDowntimeSec = 0;     // length of the last outage
            uint16_t _reconnects = 0;          // link-up edges after the first one
            bool _linkEverDown = false;        // the first link-up is a connect, not a reconnect
#if defined(KNX_IP_LAN) && (defined(ARDUINO_ARCH_RP2040) || defined(ARDUINO_ARCH_ESP32))
            EthLinkManager _ethLink{*this}; // ETH link-speed override + auto-fallback ladder (W5500 LAN)
#endif

            // ---- W5500 clean self-heal (no MCU reboot) ----
            // The W5500 sometimes doesn't answer right after (re)boot and can wedge after a clean start. At boot
            // we retry begin() with an RSTn reset between tries; at runtime checkEthHealth() probes VERSIONR and,
            // on a wedge, recoverEth() re-inits (never the driver's end() alone -> it busy-waits forever on a
            // stuck chip -> brick). Heals as soon as the chip answers; no reboot needed.
#if defined(ARDUINO_ARCH_RP2040) && defined(OPENKNX_ETH_W5500)
            bool _ethDegraded = false;   // true: W5500 not up yet, loop() is running the self-heal retry
            uint32_t _lastEthHeal = 0;   // millis() of the last self-heal attempt (throttle)
            uint32_t _lastEthHealth = 0; // millis() of the last runtime chip-health probe (throttle)
            uint8_t _ethBadProbes = 0;   // consecutive VERSIONR != 0x04 reads (debounce before recovery)
            void hwResetPhy();           // pulse PIN_ETH_RES (RSTn low >=500us) + settle before any SPI access
            bool tryBeginEth();          // one clean bring-up: HW reset + KNX_NETIF.begin(), VERSIONR-classify on fail
            bool beginEthWithRetry();    // boot: tryBeginEth() up to ETH_BEGIN_TRIES with backoff
            void ethSelfHeal();          // loop(): throttled tryBeginEth() while _ethDegraded, clears it on success
            void checkEthHealth();       // loop(): periodic VERSIONR probe; recoverEth() on repeated failure
            void recoverEth();           // runtime: HW-reset -> safe end() -> hand to ethSelfHeal() (no reboot/brick)
            static constexpr uint8_t ETH_BEGIN_TRIES = 4;
            static constexpr uint16_t ETH_BEGIN_BACKOFF_MS = 150;
            static constexpr uint32_t ETH_HEAL_INTERVAL_MS = 5000;
            static constexpr uint32_t ETH_HEALTH_INTERVAL_MS = 5000;
            static constexpr uint8_t ETH_BAD_PROBE_LIMIT = 3;
#endif
#if defined(ARDUINO_ARCH_RP2040) && defined(KNX_IP_LAN)
            // RP2040 mDNS via the lwIP API directly (SimpleMDNS.addServiceTxt is broken on arduino-pico ->
            // TXT silently dropped). Idempotent so a link flap / W5500 recovery re-registers cleanly.
            void registerOpenknxMdns();
            bool _mdnsWasEstablished = false; // rising edge for the re-registration in checkLinkStatus()
#endif
#if defined(ARDUINO_ARCH_RP2040) && defined(KNX_IP_LAN) && defined(OPENKNX_ETH_W5500_MAINLOOP_RX)
            void pumpEthernet();         // drive W5500 RX + lwIP timers from the main loop (INT off), called from loop()
            void setEthOtaPump(bool on); // OTA: hand RX to the async IRQ pump so the blocking OTA read isn't loop-starved
#endif
#if defined(ARDUINO_ARCH_ESP32) && defined(OPENKNX_ETH_W5500) && defined(KNX_IP_LAN)
            // W5500-on-ESP hard-wedge recovery (link-based: esp_eth owns the SPI bus, we cannot raw-read VERSIONR).
            void checkEthHealthEsp();
            uint32_t _lastEthDownEsp = 0;       // millis() the link first went down (0 = up/established)
            uint32_t _lastEthHealEsp = 0;       // throttle between recovery attempts
            bool _ethWasEstablishedEsp = false; // sticky: a real link+IP came up at least once this session
            uint8_t _ethHealBackoffStep = 0;    // exponential backoff index for recovery attempts
            static constexpr uint32_t ETH_ESP_WEDGE_MS = 30000;
            static constexpr uint32_t ETH_ESP_HEAL_INTERVAL_MS = 30000;
            static constexpr uint32_t ETH_ESP_HEAL_INTERVAL_MAX_MS = 120000;
#endif

#if defined(ARDUINO_ARCH_RP2040) && defined(KNX_IP_LAN)
            // Feed PHY link edges to lwIP; arduino-pico's W5500 driver doesn't, leaving DHCP on a stale lease.
            void setLwipLinkState(bool up);
#endif
#ifdef OPENKNX_NETWORK_AUTOIP
            // last address reported; it can change while the link stays up (AutoIP/late DHCP).
            IPAddress _reportedIp;
            bool _autoIpWaitLogged = false; // "no DHCP answer yet" said once per wait, not every tick
            void checkIpAssignmentMethod();
#endif
            // Link-loss bookkeeping, shared by the normal down edge and the W5500 recovery.
            void onLinkLost();
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

#if defined(ARDUINO_ARCH_RP2040) && defined(KNX_IP_LAN)
// Whole-interface packet totals since boot (global scope, defined in Module.cpp). RP2040 + wired LAN only:
// the ESP32 ETH class exposes no such counters, and lwIP's MIB2 byte counters are off in the prebuilt core.
void openknxLanTraffic(uint32_t &rxPackets, uint32_t &txPackets);
#endif
#endif
