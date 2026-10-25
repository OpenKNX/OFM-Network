#if defined(KNX_IP_WIFI) || defined(KNX_IP_LAN)
#pragma once
#include "OpenKNX.h"
#include "OpenKNX/Led/FunctionManager.h"

// KNX-IP-Status LED function id (value 11 in the SLEDFunc dropdown). Guarded so a future
// central definition in OGM-Common/Led/FunctionManager.h takes precedence if added there.
#ifndef OPENKNX_LEDFUNC_KNXIP_STATE
#define OPENKNX_LEDFUNC_KNXIP_STATE 11
#endif
// WiFi-Status LED function id (value 12, entry from NetworkWifi.share.xml)
#ifndef OPENKNX_LEDFUNC_WIFI_STATE
#define OPENKNX_LEDFUNC_WIFI_STATE 12
#endif

// Opt-in WiFi tools: OPENKNX_WIFI_SCAN (console "net wifi scan"), OPENKNX_WIFI_AP (open setup AP + page)
#if defined(OPENKNX_WIFI_AP) && !defined(OPENKNX_WIFI_SCAN)
    #define OPENKNX_WIFI_SCAN // the setup page lists the scanned networks
#endif
#if defined(OPENKNX_WIFI_SCAN) && !defined(KNX_IP_WIFI)
    #error "OPENKNX_WIFI_SCAN / OPENKNX_WIFI_AP need KNX_IP_WIFI"
#endif
#if defined(OPENKNX_WIFI_AP) && !defined(OPENKNX_WEBSERVER)
    #error "OPENKNX_WIFI_AP needs OPENKNX_WEBSERVER for the setup page"
#endif

// Interval of the periodic IGMP re-report, OPT-IN: 0 = off. Only earns its keep behind a snooping switch
// without a querier; set it per product, well under the switch's group timer (3:14 seen on a TP-Link).
#ifndef NET_IGMP_REPORT_MS
#define NET_IGMP_REPORT_MS 0
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
#if defined(OPENKNX_WIFI_AP) && defined(DEVICE_DISPLAY_MODULE)
class Widget;
#endif
#if defined(ARDUINO_ARCH_RP2040) && defined(KNX_IP_WIFI)
struct netif *__getCYW43Netif(); // arduino-pico (lwIP_CYW43): the client interface, null before the first begin()
#endif

#if defined(KNX_IP_LAN) && (defined(ARDUINO_ARCH_RP2040) || defined(ARDUINO_ARCH_ESP32))
#include "driver/EthLinkManager.h" // ETH link mode + auto-fallback ladder (self-guarded: W5500 LAN only)
#endif

#if defined(ARDUINO_ARCH_ESP32)
#include <ESPmDNS.h>
#include <Preferences.h>
#include <atomic>
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
// 8 MHz is what the bus has actually been running at: the health probe re-clocked it and the Wiznet
// driver never sets a clock of its own, so the old 28 MHz default was never in effect after boot.
// Raising it is a separate change that needs its own soak test.
#define OPENKNX_NET_SPI_SPEED 8000000
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
#if defined(ARDUINO_ARCH_RP2040) && defined(KNX_IP_LAN) && defined(OPENKNX_ETH_W5500) && defined(OPENKNX_ETH_W5500_MAINLOOP_RX)
            /** @brief Drains the W5500 for the KNX stack before it sends (knx pollReceive); skipped in power save and OTA. */
            void pollEthernetRx();
#endif
            void loop(bool configured) override;
            void setup(bool configured) override;
            void resetNetwork();

            void savePower() override;
            bool restorePower() override;
            void showInformations() override;
            bool processCommand(const std::string cmd, bool debugKo);
            void showHelp() override;
            void showNetworkHelp(); // 'net ?': every 'net ...' subcommand (the main help only points here)
            void showNetworkInformations(bool console = false);

            bool processFunctionProperty(uint8_t objectIndex, uint8_t propertyId, uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength) override;

#ifdef KNX_IP_WIFI
            void saveWifiSettings(const char *ssid, const char *passphrase, bool scheduleRebootToTakeEffect);
            void readWifiSettings();
            bool setWifiFromConsole(const std::string &arg); // "<SEP><ssid><SEP><psk>"
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

#ifdef OPENKNX_WIFI_AP
            bool wifiApActive() const { return _wifiApActive; } // device runs its own AP (setup or operating mode)
            bool wifiSetupPathAllowed(const std::string &path) const; // open setup AP: only the setup page

            enum class WifiState : uint8_t
            {
                Offline, // client configured, no connection
                Client,
                SetupAp,
                OwnAp
            };
            /**
             * @brief WiFi state for the display: values cached by the loop, no radio access.
             */
            struct WifiInfo
            {
                WifiState state;
                const char *ssid; // client network or running AP
                int8_t rssi;      // client only
                uint8_t channel;
                uint8_t security; // client and own AP, as WifiScanNet::security
                uint8_t clients;  // AP only
            };
            WifiInfo wifiInfo() const;
#else
            bool wifiApActive() const { return false; }
            bool wifiSetupPathAllowed(const std::string &) const { return true; }
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
            IPAddress _boundIp; // ESP32: address of the last join by REBIND or the link check; a GOT_IP for it skips the rebind while the endpoint is up

#ifdef ARDUINO_ARCH_ESP32
            bool espConnected = false;

            // KNX-IP requests from the network event task, applied by loop(): IpDataLinkLayer is not thread-safe.
            static constexpr uint32_t KNXIP_REQ_DOWN = 0x01;   // disable (ETH START, LOST_IP, DISCONNECTED, STOP)
            static constexpr uint32_t KNXIP_REQ_REBIND = 0x02; // GOT_IP: re-bind on an address change or a closed endpoint
            std::atomic<uint32_t> _knxIpRequest{0};
            void requestKnxIp(bool rebind);
            void applyKnxIpRequest();
#endif

            char _hostName[25] = {};

            char *_mDNSHttpServiceName = nullptr;
            char *_mDNSDeviceServiceName = nullptr;
            char *_mDNSDeviceServiceNameTXT = nullptr;
            bool _currentLinkState = false;
            bool linkCarrier(); // carrier for the link edge logic, debounced where the read can glitch
#if defined(ARDUINO_ARCH_RP2040) && defined(OPENKNX_ETH_W5500) && defined(KNX_IP_LAN)
            // The carrier comes from a PHYCFGR read over the same SPI bus the driver uses. A single garbled
            // read was taken at face value and tore the lwIP link down, which drops the DHCP binding -- the
            // device then sat without an address until a reboot while the PHY never lost carrier.
            // checkEthHealth() debounces VERSIONR for exactly this reason; this path did not.
            bool _carrierStable = false; // accepted carrier state
            uint8_t _carrierSamples = 0; // consecutive reads disagreeing with _carrierStable
            static constexpr uint8_t LINK_DEBOUNCE_SAMPLES = 3; // 3 x 500ms tick -> 1.5s to accept a change

            // Link up but no usable address: escalate instead of waiting forever. Renew first (no
            // interruption), then re-initialise the interface -- what a warm reboot does for the network,
            // without restarting the device.
            void checkIpWatchdog(bool carrier);
            uint32_t _noIpSince = 0; // millis() the "carrier up, no usable address" state began (0 = none)
            uint8_t _noIpStage = 0;  // 0 = nothing done yet, 1 = renewed, 2 = interface re-initialised
            bool _hadIpEver = false; // gate: a network without a DHCP server must not be churned
            static constexpr uint32_t NO_IP_RENEW_MS = 60000;
            static constexpr uint32_t NO_IP_REBIND_MS = 180000;
#endif
            uint32_t _lastLinkCheck = false;
            uint32_t _lastIgmpReport = 0;
            uint32_t _ipFaultSince = 0; // millis() the IP network first looked unreachable; 0 = it does not
            bool _ipFaultReported = false; // last value written to the IP-fault bit; edge-only, keeps the 2 Hz tick allocation-free
            uint32_t _restartTimer = 0;

            // uptime() seconds; up/down comes from _currentLinkState/_ipShown, not a zero stamp.
            uint32_t _linkUpSinceSec = 0;      // last link-up edge
            uint32_t _linkDownSinceSec = 0;    // last link-down edge
            uint32_t _establishedSinceSec = 0; // moment "Network established" was reached
            uint32_t _lastDowntimeSec = 0;     // length of the last outage
            uint32_t _lastLinkUptimeSec = 0;   // how long the link had been up when it last went down
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
#if defined(ARDUINO_ARCH_RP2040) && defined(OPENKNX_ETH_W5500) && defined(KNX_IP_LAN)
            bool _ethDegraded = false;   // true: W5500 not up yet, loop() is running the self-heal retry
            uint32_t _lastEthHeal = 0;   // millis() of the last self-heal attempt (throttle)
            uint32_t _lastEthHealth = 0; // millis() of the last runtime chip-health probe (throttle)
            uint8_t _ethBadProbes = 0;   // consecutive bad health probes (VERSIONR or Sn_SR) before recovery
            // W5500 socket 0 status. VERSIONR alone cannot see a wedged MACRAW socket: the chip answers on
            // SPI and the PHY keeps link, yet no packet moves. Registers per datasheet: socket register block
            // 0x08, Sn_SR at 0x0003, SOCK_MACRAW = 0x42 (the driver opens socket 0 in MACRAW).
            static constexpr uint8_t W5500_BLOCK_SOCKET0 = 0x08;
            static constexpr uint16_t W5500_REG_SN_SR = 0x0003;
            static constexpr uint8_t W5500_SOCK_MACRAW = 0x42;
            uint8_t readSocketStatusLocked(); // Sn_SR under the lwIP lock
            void hwResetPhy();           // pulse PIN_ETH_RES (RSTn low >=500us) + settle before any SPI access
            bool tryBeginEth();          // one clean bring-up: HW reset + KNX_NETIF.begin(), VERSIONR-classify on fail
            bool beginEthWithRetry();    // boot: tryBeginEth() up to ETH_BEGIN_TRIES with backoff
            void ethSelfHeal();          // loop(): stepped, non-blocking bring-up retry while _ethDegraded
            bool beginEthAfterReset();   // setSPISpeed + begin() + VERSIONR classification (no RSTn pulse)
            uint32_t healIntervalMs() const; // retry spacing, widens with _healFailures
            void checkEthHealth();       // loop(): periodic VERSIONR probe; recoverEth() on repeated failure
            void recoverEth();           // runtime: HW-reset -> safe end() -> hand to ethSelfHeal() (no reboot/brick)
            static constexpr uint8_t ETH_BEGIN_TRIES = 4;
            static constexpr uint16_t ETH_BEGIN_BACKOFF_MS = 150;
            static constexpr uint32_t ETH_HEAL_INTERVAL_MS = 5000;
            static constexpr uint32_t ETH_HEALTH_INTERVAL_MS = 5000;
            static constexpr uint8_t ETH_BAD_PROBE_LIMIT = 3;
            // RSTn pulse shape, shared by the blocking boot path and the stepped self-heal. Datasheet 5.5.1:
            // TRC (reset cycle) >= 500us, TPL (RSTn to internal PLL lock) <= 1ms -- both are generous here.
            static constexpr uint16_t ETH_RESET_LOW_MS = 2;
            static constexpr uint16_t ETH_RESET_SETTLE_MS = 60;

            // The self-heal used to burn ETH_RESET_LOW_MS + ETH_RESET_SETTLE_MS inside one loop() pass, every
            // 5s, forever. On a chip that cannot be healed by RSTn at all that starved the TPUart RX path
            // (bus protocol errors) and chopped up the console. It now steps through the pulse across loop
            // passes and widens the retry spacing the longer the chip stays dead.
            enum class HealStep : uint8_t
            {
                Wait,        // between attempts
                ResetLow,    // RSTn low, waiting out ETH_RESET_LOW_MS
                ResetSettle, // RSTn released, waiting out ETH_RESET_SETTLE_MS
            };
            HealStep _healStep = HealStep::Wait;
            uint32_t _healAt = 0;      // millis() of the next step
            uint16_t _healFailures = 0; // consecutive failed bring-ups (drives the backoff)
            static constexpr uint32_t ETH_HEAL_INTERVAL_SLOW_MS = 30000;
            static constexpr uint32_t ETH_HEAL_INTERVAL_MAX_MS = 120000;
            static constexpr uint16_t ETH_HEAL_FAST_TRIES = 3;  // a transient hiccup heals within these
            static constexpr uint16_t ETH_HEAL_SLOW_TRIES = 10; // then 30s, then 120s

            // ---- console PHY diagnostics ('net phy ...') ----
            // 'reset' and 'pin' both drive RSTn for far longer than a loop() pass may block, so they run as a
            // state machine. While one is active the self-heal and the health probe are skipped: nothing else
            // may touch RSTn or the SPI bus, or the measurement is worthless.
            enum class PhyTool : uint8_t
            {
                None,
                ResetLow,    // RSTn held low, waiting for _phyToolAt
                ResetSettle, // RSTn released, waiting out the PLL settle
                PinToggle    // 1 Hz square wave on RSTn for the DMM
            };
            PhyTool _phyTool = PhyTool::None;
            uint32_t _phyToolAt = 0;       // millis() of the next state step
            uint32_t _phyToolEnd = 0;      // PinToggle: end of the toggle window
            uint16_t _phyToolLowMs = 0;    // ResetLow: how long RSTn is held low
            uint8_t _phyToolVerBefore = 0; // VERSIONR read before the pulse, for the before/after line
            bool _phyToolLevel = false;    // PinToggle: current RSTn level
            bool phyToolActive() const { return _phyTool != PhyTool::None; }
            void phyToolLoop();                     // loop(): drive the state machine, hand back via recoverEth()
            void phyToolStartReset(uint16_t lowMs); // 'net phy reset [ms]'
            void phyToolStartPin(uint16_t seconds); // 'net phy pin [s]'
            void showPhyStatus();                   // 'net phy'
            void phyToolAbort();                    // give up a running diagnostic (power save / OTA)
            uint8_t readVersionLocked(uint32_t hz); // VERSIONR under the lwIP lock (never race the SPI pump)
            static constexpr uint32_t PHY_TOOL_SETTLE_MS = 200; // >> TPL (1 ms, datasheet 5.5.1)
            static constexpr uint16_t PHY_TOOL_RESET_MS_DEFAULT = 100;
            static constexpr uint16_t PHY_TOOL_RESET_MS_MAX = 1000;
            static constexpr uint16_t PHY_TOOL_PIN_S_DEFAULT = 30;
            static constexpr uint16_t PHY_TOOL_PIN_S_MAX = 120;
#endif
#if defined(ARDUINO_ARCH_RP2040)
            // RP2040 mDNS via the lwIP API directly (SimpleMDNS.addServiceTxt is broken on arduino-pico ->
            // TXT silently dropped). Idempotent so a link flap / W5500 recovery re-registers cleanly.
            void registerOpenknxMdns();
            void forgetOpenknxMdns(); // the interface (and its services) goes away: the next registration starts fresh
            bool _mdnsWasEstablished = false; // rising edge for the re-registration in checkLinkStatus()
#endif
#if defined(ARDUINO_ARCH_RP2040) && defined(KNX_IP_LAN) && defined(OPENKNX_ETH_W5500) && defined(OPENKNX_ETH_W5500_MAINLOOP_RX)
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

#if defined(ARDUINO_ARCH_RP2040) && defined(KNX_IP_WIFI)
            void startWifiJoin();     // requests the client join
            void clearWifiLinkFlag(); // client link flag down until the driver reports the next association
#ifdef OPENKNX_NETWORK_AUTOIP
            void stopStaleWifiAutoIp(); // AutoIP only with a link and never over a DHCP address
#endif
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
            void refreshMulticastMembership(bool linkEstablished); // periodic IGMP re-report; see the comment at the definition
            void checkIpStatus();
            void loadCallbacks(bool state);
            void handleMDNS();
            void handleOTA();
            void controlKnxIp(bool state);
            void checkKnxIpStatus(); // drives the KNX-IP-Status LED (func 11): green=KNXnet/IP running, orange=link up but KNX-IP down, red=no link
            void checkKnxIpDeviceState(bool established); // feeds the IP-fault bit of PID_KNXNETIP_DEVICE_STATE
            bool knxIpEnabled();     // is the KNXnet/IP datalink layer currently enabled?
#if defined(ARDUINO_ARCH_RP2040) && defined(OPENKNX_ETH_W5500) && defined(KNX_IP_LAN)
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
#ifdef OPENKNX_WIFI_SCAN
            struct WifiScanNet
            {
                std::string ssid;
                int8_t rssi;
                uint8_t channel;
                uint8_t security; // 0 open, 1 WEP, 2 WPA, 3 WPA2, 4 WPA/WPA2, 5 WPA3, 6 WPA2/WPA3, 7 enterprise, 255 unknown
            };
            std::vector<WifiScanNet> _wifiScanNets; // last result: strongest entry per SSID, sorted by RSSI
            bool _wifiScanRunning = false;
            bool _wifiScanFailed = false;
            bool _wifiScanToConsole = false;
            uint32_t _wifiScanStartedAt = 0; // for the given-up limit of loopWifiScan()
            volatile bool _wifiScanRequested = false; // /wifi/scan?start=1, picked up by the link tick
            bool startWifiScan(bool toConsole);
            void loopWifiScan();
            static void formatBssid(const uint8_t *bssid, char (&out)[18]);
            uint8_t wifiScanResult(uint8_t *out, uint8_t maxLen); // function property 160/5 command 4
#endif
#ifdef OPENKNX_WIFI_AP
            bool _wifiApChecked = false;
            bool _wifiApActive = false;
            bool _wifiOwnAp = false;         // AP is the operating mode (ETS), not the setup AP
            bool _wifiApAutoChannel = false; // a scan picks the channel of the own AP
            bool _wifiApAuto5GHz = false;    // that scan picks among the 5 GHz channels
            uint32_t _wifiApSince = 0;       // start of the current client attempt, for the setup AP fallback
            bool _wifiApByProg = false;      // setup AP opened on demand (prog button, console 'net wifi ap')
            uint32_t _wifiApIdleSince = 0;   // AP start or last time a station was connected
            uint32_t _wifiApPollAt = 0;      // last station count read
            uint8_t _wifiApClients = 0; // at most WIFI_AP_STA_MAX
            static constexpr uint8_t WIFI_AP_STA_MAX = 8; // stations listed; the RP2040 AP has 8 DHCP leases
            struct WifiApStation
            {
                uint8_t mac[6];
                uint32_t ip; // lwIP byte order, 0 = not known
            };
            WifiApStation _wifiApStas[WIFI_AP_STA_MAX] = {}; // first _wifiApClients valid
            uint8_t _wifiApSecurity = 3; // own AP, as WifiScanNet::security: WPA2, RP2040 fallback WPA/WPA2
            void pollWifiApStations();
            OpenKNX::Led::FunctionGroup *_wifiLedFunc = nullptr; // "WiFi-Status" LED function
            uint8_t _wifiLedState = 0;
            void updateWifiLed(bool up);
#ifdef OPENKNX_WIFI_LED_PIN
            uint8_t _wifiBoardLed = 0xFF; // last level written to the onboard WiFi LED, 0xFF = not written yet
            void updateWifiBoardLed(bool up);
#endif
            uint32_t _wifiProgPendingSince = 0; // prog mode without a link: start of the wait for the setup AP, 0 = none
            bool _wifiProgWasOn = false;     // prog mode in the previous link tick (switch-on edge)
            uint32_t _wifiRetryAt = 0;       // last client join or last time connected
            bool _wifiRejoinLogged = false;  // the outage's rejoins are logged once
            uint32_t _wifiInfoAt = 0;        // last RSSI/channel read
            int8_t _wifiRssi = 0;
            uint8_t _wifiChannel = 0;
            uint8_t _wifiBssid[6] = {}; // AP of the client link, zero = none read yet
            uint8_t _wifiSecurity = 255;
            void noteWifiBssid(const uint8_t *bssid); // cache refresh; logs an AP change
#ifdef ARDUINO_ARCH_RP2040
            bool _wifiJoinPending = false; // left, finishWifiJoinCycle() joins once the leave settled and no scan runs
            uint32_t _wifiLeftAt = 0;      // start of the pending join cycle (its leave)
            uint32_t _wifiFailedSince = 0; // first tick with a failure driver state, 0 = none
            void startWifiJoinCycle();
            void finishWifiJoinCycle();
#endif
            static constexpr uint8_t WIFI_HIST_LEN = 60;
            int8_t _wifiRssiHist[WIFI_HIST_LEN] = {}; // one sample per 10 s, 0 = no connection
            uint8_t _wifiRssiHead = 0;
            uint8_t _wifiRssiCount = 0;
            uint32_t _wifiRssiAt = 0;
#ifdef OPENKNX_PING
            uint16_t _wifiPingHist[WIFI_HIST_LEN] = {}; // gateway RTT in ms, one per second, 0xFFFF = no answer
            uint8_t _wifiPingHead = 0;
            uint8_t _wifiPingCount = 0;
            uint32_t _wifiPingAt = 0;
            volatile uint32_t _wifiPingPolledAt = 0; // last /wifi/status request: the ping runs only while the page is open
            bool _wifiPingBusy = false;
            void loopWifiPing();
#endif
#ifdef DEVICE_DISPLAY_MODULE
            Widget *_wifiWidget = nullptr;
            uint32_t _wifiWidgetShowSince = 0; // setup AP started: bring the widget up once, 0 = done
#endif
            void checkWifiProgAp(bool up);
            void loopWifiClient(bool up);
            void rejoinWifiDriver(); // RP2040: non-blocking join on the existing client interface
            void showWifiStatus();   // console 'net wifi'
            void reconnectWifi();    // console 'net wifi reconnect'
            void openWifiSetupAp();  // console 'net wifi ap'
            void beginWifiClient();
            void leaveWifiAp();
            void stopWifiAp();
            char _wifiApSsid[33] = {};       // name of the running AP, for info/net
            void showWifiApInformation(bool console);
            bool wifiOwnApConfigured() const;
            void loopWifiAp();
            void startWifiAp();
            void startOwnAp(uint8_t channel);
            void joinApMulticast();
            void setupWifiRoutes();
            bool _wifiRoutesAdded = false; // setupWifiRoutes() ran
            bool saveWifiApPassword(const char *psk);
#endif

            std::vector<NetworkChangeCallback> _callback;
        };

    } // namespace Network
} // namespace OpenKNX

using NetworkModule = OpenKNX::Network::Module;
extern NetworkModule openknxNetwork;

// Whole-interface packet totals since boot (global scope, defined in Module.cpp). On RP2040 the W5500 driver
// counts them. The ESP32 ETH class exposes none, so with wired LAN the module counts at the lwIP netif; that
// is built only for a device with a display, or with OPENKNX_NET_PACKET_COUNT.
#if (defined(ARDUINO_ARCH_RP2040) && defined(KNX_IP_LAN)) || \
    (defined(ARDUINO_ARCH_ESP32) && defined(KNX_IP_LAN) && (defined(DEVICE_DISPLAY_MODULE) || defined(OPENKNX_NET_PACKET_COUNT)))
#define OPENKNX_NET_HAVE_LAN_TRAFFIC 1
void openknxLanTraffic(uint32_t &rxPackets, uint32_t &txPackets);
#endif
#endif
