#if defined(KNX_IP_WIFI)
#include "OpenKNX/Network/Module.h"

#ifdef OPENKNX_WIFI_SCAN
#include <WiFi.h>
#include <algorithm>
#include <cstdarg>
#include <string>
#include <vector>
#ifdef ARDUINO_ARCH_RP2040
#include <cyw43.h>
#include <lwip_wrap.h> // LWIPMutex
#endif

static constexpr uint32_t WIFI_SCAN_MAX_MS = 15000; // a scan without an end event is given up after this long

#ifdef OPENKNX_WIFI_AP
#include "OpenKNX/Network/Webserver/Webserver.h"
#include "buildtime.h"
#include "webassets.h"
#include <DNSServer.h>
#if __has_include("qrcodegen.h")
#include "qrcodegen.h" // ships with OFM-DeviceDisplay
#ifdef OPENKNX_QR_AUTO_MASK // same switch as OFM-DeviceDisplay Widgets/QRCode.h
#define OPENKNX_QR_MASK qrcodegen_Mask_AUTO
#else
#define OPENKNX_QR_MASK qrcodegen_Mask_0
#endif
#endif
#ifdef DEVICE_DISPLAY_MODULE
#include "DeviceDisplay.h"
#endif
#ifdef ARDUINO_ARCH_RP2040
#include <LittleFS.h>
#include <cyw43.h>
#include <lwip/etharp.h>
#include <lwip/igmp.h>
#include <lwip/netif.h>
#include <lwip_wrap.h> // LWIPMutex
#else
#include <Preferences.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <lwip/netif.h>
#include <lwip/tcpip.h>
#endif

static DNSServer s_wifiApDns;                          // captive portal of the setup AP: every name resolves to the AP
static constexpr uint32_t WIFI_AP_FALLBACK_MS = 30000; // uptime without a client link before the setup AP fallback
static constexpr uint8_t WIFI_AP_FALLBACK_CHANNEL = 1; // own AP when the channel scan fails
static constexpr uint32_t WIFI_REJOIN_MS = 30000;        // RP2040: rejoin interval (from the join request or the last link) while the driver joins
static constexpr uint32_t WIFI_REJOIN_FAILED_MS = 10000; // RP2040: the same once the driver state reports a failure
static constexpr uint32_t WIFI_FAILED_HOLD_MS = 3000;    // RP2040: a failure state counts after this long (BADAUTH can pass, the driver's disassoc follows)
static constexpr uint32_t WIFI_LEAVE_SETTLE_MS = 500;    // RP2040: least time between leave and join, for the leave's driver events
static constexpr uint32_t WIFI_JOIN_DROP_MS = 10000;     // RP2040: a pending join is dropped when the link stays up this long after the leave
static constexpr uint32_t WIFI_RETRY_MS = 120000;        // RP2040: begin() when the interface is gone; ESP32: join
static constexpr uint32_t WIFI_AP_RETRY_MS = 300000;    // fallback setup AP without a station: try the client again
static constexpr uint32_t WIFI_AP_PROG_IDLE_MS = 600000; // setup AP from the prog button: closes after this long without a station
static constexpr uint32_t WIFI_PROG_AP_DELAY_MS = 5000; // prog mode without a link this long opens the setup AP
static constexpr uint32_t WIFI_INFO_MS = 2000;          // RSSI/channel cache refresh
static constexpr uint32_t WIFI_RSSI_SAMPLE_MS = 10000;  // RSSI history: 60 samples = 10 minutes
static constexpr uint32_t WIFI_PING_MS = 1000;          // gateway ping interval while /wifi is open
static constexpr uint32_t WIFI_PING_IDLE_MS = 10000;    // ping stops this long after the last /wifi/status request
static constexpr uint32_t WIFI_WIDGET_SHOW_MS = 30000;  // setup AP: keep trying to bring the widget up this long
#ifndef OPENKNX_WIFI_AP_IP
#define OPENKNX_WIFI_AP_IP 10, 42, 0, 1 // AP address, /24; clients get theirs from the AP's DHCP server
#endif

// AP mode with the fixed address; softAPConfig() stores AP values only while the mode is AP
static void wifiApMode()
{
    const IPAddress ip(OPENKNX_WIFI_AP_IP);
    WiFi.disconnect(); // client down first, otherwise beginAP() runs AP+STA
    WiFi.mode(WIFI_AP);
#ifdef ARDUINO_ARCH_ESP32
    // ESP32 DHCP announces a DNS server only when one is given: the AP itself, for the captive portal
    WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0), IPAddress(ip[0], ip[1], ip[2], ip[3] + 1), ip);
#else
    WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0));
#endif
}
#endif

namespace OpenKNX
{
    namespace Network
    {
        namespace
        {
#ifdef ARDUINO_ARCH_ESP32
            // /wifi/scan runs in the httpd task, the scan result is rebuilt in the loop task
            SemaphoreHandle_t s_scanLock = nullptr;
            struct ScanLock
            {
                ScanLock() { if (s_scanLock) xSemaphoreTake(s_scanLock, portMAX_DELAY); }
                ~ScanLock() { if (s_scanLock) xSemaphoreGive(s_scanLock); }
            };
#else
            struct ScanLock // RP2040: route handlers run in the main loop, nothing to lock
            {
                ScanLock() {} // user-provided: a guard object, not an unused variable (-Wunused-variable)
            };
#endif

#ifdef OPENKNX_WIFI_AP
            // Captive-portal probes (Android, iOS/macOS, Windows)
            const char *const kCaptiveProbes[] = {"/generate_204", "/gen_204", "/hotspot-detect.html", "/library/test/success.html",
                                                  "/connecttest.txt", "/ncsi.txt", "/redirect"};
#endif
#ifdef ARDUINO_ARCH_ESP32
            uint8_t authSecurity(wifi_auth_mode_t mode)
            {
                switch (mode)
                {
                    case WIFI_AUTH_OPEN: return 0;
                    case WIFI_AUTH_WEP: return 1;
                    case WIFI_AUTH_WPA_PSK: return 2;
                    case WIFI_AUTH_WPA2_PSK: return 3;
                    case WIFI_AUTH_WPA_WPA2_PSK: return 4;
                    case WIFI_AUTH_WPA3_PSK: return 5;
                    case WIFI_AUTH_WPA2_WPA3_PSK: return 6;
                    case WIFI_AUTH_WPA2_ENTERPRISE: return 7;
                    default: return 255;
                }
            }
#endif

            /**
             * @brief Security of scan entry i as WifiScanNet::security. The CYW43 driver only reports an RSN
             *        element, which WPA2 and WPA3 share, so the RP2040 reports both as 6 (WPA2/WPA3).
             */
            uint8_t wifiSecurity(uint8_t i)
            {
#ifdef ARDUINO_ARCH_ESP32
                return authSecurity(WiFi.encryptionType(i));
#else
                switch (WiFi.encryptionType(i))
                {
                    case ENC_TYPE_NONE: return 0;
                    case ENC_TYPE_TKIP: return 2; // WPA element only
                    case ENC_TYPE_CCMP: return 6; // RSN element: WPA2 or WPA3
                    case ENC_TYPE_AUTO: return 4; // WPA and RSN elements
                    default: return 255;          // privacy bit only (WEP) or unmapped
                }
#endif
            }

            const char *wifiSecurityText(uint8_t sec)
            {
                static const char *const names[] = {"open", "WEP", "WPA", "WPA2", "WPA/WPA2", "WPA3", "WPA2/WPA3", "Enterprise"};
                return sec < 8 ? names[sec] : "?";
            }

            // printf onto a string; one call per piece keeps the routes small (no to_string/operator+ temporaries)
            void addf(std::string &out, const char *fmt, ...)
            {
                char buf[160];
                va_list ap;
                va_start(ap, fmt);
                const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
                va_end(ap);
                if (n > 0) out.append(buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
            }

            void jsonEscape(std::string &out, const char *in)
            {
                for (; *in; in++)
                {
                    const unsigned char c = (unsigned char)*in;
                    if (c == '"' || c == '\\')
                    {
                        out += '\\';
                        out += (char)c;
                    }
                    else if (c < 0x20)
                    {
                        char buf[7];
                        snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    }
                    else
                        out += (char)c;
                }
            }
        } // namespace

        /**
         * @brief "AA:BB:CC:DD:EE:FF".
         */
        void Module::formatBssid(const uint8_t *bssid, char (&out)[18])
        {
            snprintf(out, sizeof(out), "%02X:%02X:%02X:%02X:%02X:%02X", bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
        }

        /**
         * @brief Starts an asynchronous WiFi scan; loopWifiScan() collects the result.
         * @return false when the scan could not be started.
         */
        bool Module::startWifiScan(bool toConsole)
        {
            if (_wifiScanRunning)
            {
                _wifiScanToConsole |= toConsole;
                return true;
            }
#ifdef ARDUINO_ARCH_ESP32
            WiFi.enableSTA(true); // the ESP32 scans only with the client interface up (AP -> AP+STA, no connect)
#else
            // a scan given up by loopWifiScan() may still run in the driver: a new start clears the list its callback fills
            if (cyw43_wifi_scan_active(&cyw43_state))
            {
                _wifiScanFailed = true;
                return false;
            }
#endif
            // -1: started (RP2040: 0 = error, ESP32: -2 = failed)
            if (WiFi.scanNetworks(true) != -1)
            {
                _wifiScanFailed = true;
                return false;
            }
            _wifiScanRunning = true;
            _wifiScanStartedAt = millis();
            _wifiScanFailed = false;
            _wifiScanToConsole = toConsole;
            if (toConsole) logInfoP("WiFi scan started");
            return true;
        }

        /**
         * @brief Polls the running scan; keeps the strongest entry per SSID, sorted by RSSI. A pending own AP
         *        gets the channel least covered by the networks around (20 MHz channel, +-4 neighbours).
         */
        void Module::loopWifiScan()
        {
            const int16_t n = WiFi.scanComplete();
            if (n == -1 && millis() - _wifiScanStartedAt < WIFI_SCAN_MAX_MS) return; // still running
            if (n < 0)
            {
                // not running any more: a rejoin held back by the scan goes on (loopWifiClient())
                _wifiScanRunning = false;
                _wifiScanFailed = true;
                _wifiScanToConsole = false;
                logErrorP(n == -1 ? "WiFi scan did not end, given up" : "WiFi scan failed");
#ifdef ARDUINO_ARCH_RP2040
                if (n == -1)
                {
                    // the driver ends a scan only on its final result event: release it, or startWifiScan() refuses every later scan;
                    // its result callback runs only in state 1 and under this lock, so no late entry reaches the list
                    LWIPMutex m;
                    cyw43_state.wifi_scan_state = 0;
                }
#endif
#ifdef OPENKNX_WIFI_AP
                if (_wifiApAutoChannel)
                {
                    _wifiApAutoChannel = false;
                    startOwnAp(_wifiApAuto5GHz ? 36 : WIFI_AP_FALLBACK_CHANNEL);
                }
#endif
                return;
            }

            struct ScanAp // console listing: every AP, not only the strongest per network
            {
                std::string ssid;
                int8_t rssi;
                uint8_t channel;
                uint8_t security;
                uint8_t bssid[6];
            };
            std::vector<ScanAp> aps;
            uint32_t score[14] = {};
            uint32_t score5[4] = {}; // 36, 40, 44, 48: 20 MHz channels without overlap
            std::vector<WifiScanNet> nets;
            for (int16_t i = 0; i < n; i++)
            {
                const int32_t rssi = WiFi.RSSI(i);
                const uint8_t ch = (uint8_t)WiFi.channel(i);
                if (ch >= 1 && ch <= 13)
                {
                    const uint32_t weight = (uint32_t)std::max<int32_t>(1, rssi + 100); // -40 dBm counts more than -90
                    for (uint8_t c = 1; c <= 13; c++)
                    {
                        const uint8_t d = c > ch ? c - ch : ch - c;
                        if (d <= 4) score[c] += weight * (5 - d);
                    }
                }

                else if (ch >= 36 && ch <= 48 && (ch - 36) % 4 == 0)
                    score5[(ch - 36) / 4] += (uint32_t)std::max<int32_t>(1, rssi + 100);

#ifdef ARDUINO_ARCH_RP2040
                const char *raw = WiFi.SSID(i);
                const std::string ssid(raw, strnlen(raw, 32)); // the driver's 32-byte field has no NUL at full length
#else
                const std::string ssid(WiFi.SSID(i).c_str());
#endif
                if (ssid.empty()) continue; // hidden network
                auto it = std::find_if(nets.begin(), nets.end(), [&ssid](const WifiScanNet &w) { return w.ssid == ssid; });
                const WifiScanNet net{ssid, (int8_t)std::max<int32_t>(-127, std::min<int32_t>(0, rssi)), ch, wifiSecurity(i)};
                if (it == nets.end())
                    nets.push_back(net);
                else if (net.rssi > it->rssi)
                    *it = net;
                if (_wifiScanToConsole)
                {
                    ScanAp ap{ssid, net.rssi, ch, net.security, {}};
                    WiFi.BSSID(i, ap.bssid);
                    aps.push_back(ap);
                }
            }
            WiFi.scanDelete();
            std::sort(nets.begin(), nets.end(), [](const WifiScanNet &a, const WifiScanNet &b) { return a.rssi > b.rssi; });
            {
                ScanLock lock;
                _wifiScanNets.swap(nets);
                _wifiScanFailed = false;
                _wifiScanRunning = false; // last: a reader that sees "done" also sees the new list
            }

#ifdef OPENKNX_WIFI_AP
            if (_wifiApAutoChannel)
            {
                _wifiApAutoChannel = false;
                uint8_t best = 1;
                if (_wifiApAuto5GHz)
                {
                    uint8_t idx = 0;
                    for (uint8_t k = 1; k < 4; k++)
                        if (score5[k] < score5[idx]) idx = k;
                    best = 36 + idx * 4;
                }
                else
                    for (const uint8_t c : {6, 11}) // non-overlapping 20 MHz channels 1/6/11
                        if (score[c] < score[best]) best = c;
                logInfoP("WiFi AP channel %u chosen from %d networks", (unsigned)best, (int)n);
                startOwnAp(best);
            }
#endif

            if (!_wifiScanToConsole) return;
            _wifiScanToConsole = false;
            std::sort(aps.begin(), aps.end(), [](const ScanAp &a, const ScanAp &b) { return a.rssi > b.rssi; });
            logInfoP("WiFi scan: %u networks, %u access points", (unsigned)_wifiScanNets.size(), (unsigned)aps.size());
            logIndentUp();
            for (const auto &a : aps)
            {
                char bssid[18];
                formatBssid(a.bssid, bssid);
                logInfoP("%4d dBm  ch %2u  %-10s  %s  %s", (int)a.rssi, (unsigned)a.channel, wifiSecurityText(a.security), bssid, a.ssid.c_str());
            }
            logIndentDown();
        }

        /**
         * @brief Function property answer: [state, count, (rssi, security, channel, len, ssid)...], up to 8 networks that fit.
         *        state 0 = done, 1 = running, 2 = failed; security as WifiScanNet::security.
         * @return number of bytes written to out.
         */
        uint8_t Module::wifiScanResult(uint8_t *out, uint8_t maxLen)
        {
            if (maxLen < 2) return 0;
            out[0] = _wifiScanRunning ? 1 : (_wifiScanFailed ? 2 : 0);
            out[1] = 0;
            uint8_t pos = 2;
            if (_wifiScanRunning || _wifiScanFailed) return pos;
            for (const auto &w : _wifiScanNets)
            {
                const uint8_t len = (uint8_t)std::min<size_t>(w.ssid.size(), 32);
                if (out[1] >= 8 || pos + 4 + len > maxLen) break;
                out[pos++] = (uint8_t)w.rssi;
                out[pos++] = w.security;
                out[pos++] = w.channel;
                out[pos++] = len;
                memcpy(out + pos, w.ssid.data(), len);
                pos += len;
                out[1]++;
            }
            return pos;
        }

#ifdef OPENKNX_WIFI_AP
        /**
         * @brief true when the ETS selected the own AP as operating mode.
         */
        bool Module::wifiOwnApConfigured() const
        {
#ifdef ParamNETW_Mode
            return knx.configured() && ParamNETW_Mode;
#else
            return false;
#endif
        }

        /**
         * @brief Either AP or client, never both (single radio).
         *        Own AP (ETS): AP only, channel fixed or picked by a scan.
         *        Setup AP: without credentials, or when an unprogrammed device has no client link 30 s after boot;
         *        the ETS can switch it off. A programmed device keeps retrying the client.
         */
        void Module::loopWifiAp()
        {
            if (_wifiApActive)
            {
                if (!_wifiOwnAp) s_wifiApDns.processNextRequest();
                const uint32_t now = millis();
                if (now - _wifiApPollAt < 1000) return;
                _wifiApPollAt = now;
                pollWifiApStations();
                if (_wifiApClients) _wifiApIdleSince = now;
#ifdef DEVICE_DISPLAY_MODULE
                if (_wifiWidgetShowSince != 0)
                {
                    WidgetsManager *manager = openknxDisplayModule.getWidgetManager();
                    if ((manager != nullptr && _wifiWidget != nullptr && manager->showDefaultWidget(_wifiWidget)) ||
                        now - _wifiWidgetShowSince >= WIFI_WIDGET_SHOW_MS)
                        _wifiWidgetShowSince = 0;
                }
#endif
                // Own AP, or setup AP without credentials: nothing to go back to
                if (_wifiOwnAp || _wifiSSID[0] == 0) return;
                if (now - _wifiApIdleSince < (_wifiApByProg ? WIFI_AP_PROG_IDLE_MS : WIFI_AP_RETRY_MS)) return;
                if (knx.configured() && !ParamNET_HTTP)
                {
                    // the AP started the web server, which cannot be stopped: a restart comes back without it
                    if (_restartTimer == 0)
                    {
                        logInfoP("WiFi: setup AP unused, restart");
                        _restartTimer = delayTimerInit();
                    }
                    return;
                }
                logInfoP("WiFi: setup AP unused, joining \"%s\" again", _wifiSSID);
                leaveWifiAp();
                return;
            }

            if (_wifiApChecked) return;

            if (wifiOwnApConfigured())
            {
                _wifiApChecked = true;
                uint8_t channel = 0;
                bool band5 = false;
#ifdef ParamNETW_Channel
                channel = ParamNETW_Channel;
#endif
#ifdef ParamNETW_Band
                band5 = ParamNETW_Band == 1;
#endif
#ifndef OPENKNX_WIFI_5GHZ
                if (band5)
                {
                    logErrorP("WiFi: no 5 GHz on this hardware, own AP on 2.4 GHz");
                    band5 = false;
                    channel = 0; // the byte holds a 5 GHz channel: pick a 2.4 GHz one
                }
#endif
                _wifiApAuto5GHz = band5;
                const bool fixed = band5 ? (channel >= 36 && channel <= 48 && (channel - 36) % 4 == 0) : (channel >= 1 && channel <= 13);
                if (fixed)
                    startOwnAp(channel);
                else if (startWifiScan(false))
                    _wifiApAutoChannel = true; // loopWifiScan() starts the AP
                else
                    startOwnAp(band5 ? 36 : WIFI_AP_FALLBACK_CHANNEL);
                return;
            }

#ifdef ParamNETW_SetupAp
            if (knx.configured() && !ParamNETW_SetupAp)
            {
                _wifiApChecked = true;
                return;
            }
#endif
            if (_wifiSSID[0] == 0)
            {
                _wifiApChecked = true;
                startWifiAp();
                return;
            }
            if (established() || knx.configured())
            {
                _wifiApChecked = true;
                return;
            }
            if (millis() - _wifiApSince < WIFI_AP_FALLBACK_MS) return;
            _wifiApChecked = true;
            logInfoP("WiFi: no connection to \"%s\", falling back to the setup AP", _wifiSSID);
            startWifiAp();
        }

        /**
         * @brief Open setup AP named after the host, with captive-portal DNS.
         */
        void Module::startWifiAp()
        {
            wifiApMode();
#ifdef ARDUINO_ARCH_ESP32
            const bool ok = WiFi.softAP(_hostName);
#else
            const bool ok = WiFi.beginAP(_hostName) == WL_CONNECTED; // open: no passphrase
#endif
            if (!ok)
            {
                logErrorP("WiFi setup AP could not be started");
                return;
            }
            _wifiApActive = true;
            _wifiApIdleSince = millis();
            _wifiApClients = 0;
            memset(_wifiApStas, 0, sizeof(_wifiApStas));
#ifdef DEVICE_DISPLAY_MODULE
            _wifiWidgetShowSince = millis() | 1; // 0 means none
#endif
            strncpy(_wifiApSsid, _hostName, sizeof(_wifiApSsid) - 1);
            s_wifiApDns.start(53, "*", WiFi.softAPIP());
            // Open AP: no KNXnet/IP. controlKnxIp() leaves an interface (0x07B0) alone to keep live tunnels; an endpoint
            // from an earlier client link would answer tunnel requests on the AP, and there are no tunnels to keep.
            // networkChanged() opens it again with the next client link.
#if MASK_VERSION == 0x07B0 && defined(KNX_TUNNELING) // interface (Bau07B0IP); a plain TP device has no endpoint
            knx.bau().getPrimaryDataLinkLayer()->enabled(false);
#else
            controlKnxIp(false);
#endif
            logInfoP("WiFi setup AP \"%s\" at http://%s/wifi", _hostName, WiFi.softAPIP().toString().c_str());
        }

#ifdef ARDUINO_ARCH_RP2040
        /**
         * @brief Sets (set = true) or reads a per-BSS firmware variable of the AP (bsscfg index 1), sent on the STA
         *        interface like the driver does. The driver ignores the firmware status, hence the read-back in setApWpa2Only().
         * @return the value read (reply bytes for a set); 0xFFFFFFFF when the request failed.
         */
        static uint32_t apBssVar(const char *name, bool set, uint32_t value = 0)
        {
            uint8_t buf[32] = {}; // longest name "bsscfg:wpa_auth" + 8
            const size_t n = strlen(name) + 1;
            const uint32_t bss = 1; // little-endian, as the firmware expects
            memcpy(buf, name, n);
            memcpy(buf + n, &bss, 4);
            memcpy(buf + n + 4, &value, 4);
            if (cyw43_ioctl(&cyw43_state, set ? CYW43_IOCTL_SET_VAR : CYW43_IOCTL_GET_VAR, n + (set ? 8 : 4), buf, CYW43_ITF_STA) != 0)
                return 0xFFFFFFFF;
            memcpy(&value, buf, 4);
            return value;
        }

        /**
         * @brief Switches the running AP from WPA/WPA2 (what the driver sets for every WPA2 auth) to WPA2 only.
         * @return true when the AP reads back as up with WPA2 only.
         */
        static bool setApWpa2Only()
        {
            static constexpr uint32_t WPA2_AUTH_PSK = 0x0080; // CYW43_WPA2_AUTH_PSK, defined in cyw43_ll.c only
            apBssVar("bss", true, 0);
            apBssVar("bsscfg:wpa_auth", true, WPA2_AUTH_PSK);
            apBssVar("bss", true, 1);
            return apBssVar("bss", false) == 1 && apBssVar("bsscfg:wpa_auth", false) == WPA2_AUTH_PSK;
        }
#endif

        /**
         * @brief Own AP as operating mode: WPA2 with the transferred password; name from the ETS ("Netzname anpassen") or the host name.
         *        RP2040: the driver announces WPA and WPA2 (WPA1 IE in the beacon), which a Broadcom Windows client rejects;
         *        the AP is switched to WPA2 only, or restarted with WPA/WPA2 when that fails.
         */
        void Module::startOwnAp(uint8_t channel)
        {
            char psk[64] = {};
#ifdef ARDUINO_ARCH_RP2040
            File file = LittleFS.open("/WIFIAP.TXT", "r");
            if (file)
            {
                const String line = file.readStringUntil('\n');
                file.close();
                strncpy(psk, line.c_str(), sizeof(psk) - 1);
            }
#else
            Preferences preferences;
            preferences.begin("WIFI", true);
            preferences.getString("APPSK", psk, sizeof(psk));
            preferences.end();
#endif
            psk[strcspn(psk, "\r")] = 0;
            if (strlen(psk) < 8)
            {
                logErrorP("WiFi own AP not started: no password transferred");
                return;
            }

            char ssid[33] = {};
#if defined(ParamNETW_ApSsid) && defined(ParamNETW_CustomApSsid)
            if (ParamNETW_CustomApSsid) memcpy(ssid, ParamNETW_ApSsid, 32);
#endif
            if (ssid[0] == 0) strncpy(ssid, _hostName, sizeof(ssid) - 1);

            wifiApMode(); // a channel scan brought the client interface up
#ifdef ARDUINO_ARCH_ESP32
            const bool ok = WiFi.softAP(ssid, psk, channel);
#else
            bool ok = WiFi.beginAP(ssid, psk, channel) == WL_CONNECTED;
            _wifiApSecurity = 3; // WifiScanNet::security: WPA2
            if (ok && !setApWpa2Only())
            {
                logErrorP("WiFi own AP: WPA2 only not applied, restarting with WPA/WPA2");
                WiFi.disconnectAP();
                ok = WiFi.beginAP(ssid, psk, channel) == WL_CONNECTED;
                _wifiApSecurity = 4; // WPA/WPA2
            }
#endif
            memset(psk, 0, sizeof(psk));
            if (!ok)
            {
                logErrorP("WiFi own AP \"%s\" could not be started", ssid);
                return;
            }
            _wifiApActive = true;
            _wifiOwnAp = true;
            _wifiApIdleSince = millis();
            _wifiApClients = 0;
            memset(_wifiApStas, 0, sizeof(_wifiApStas));
            _wifiChannel = channel;
            strncpy(_wifiApSsid, ssid, sizeof(_wifiApSsid) - 1);
            joinApMulticast();
            logInfoP("WiFi own AP \"%s\" on channel %u at %s", ssid, (unsigned)channel, WiFi.softAPIP().toString().c_str());
        }

        /**
         * @brief Takes the AP down and re-arms loopWifiAp() (network reset).
         */
        void Module::stopWifiAp()
        {
            if (_wifiApActive)
            {
                if (!_wifiOwnAp) s_wifiApDns.stop();
#ifdef ARDUINO_ARCH_RP2040
                WiFi.disconnectAP(); // AP and its DHCP server, in any mode
#else
                // AP off before the flags below open the web paths again; STA keeps the driver initialised
                // (softAPdisconnect() would deinit it, or reconfigure the running AP as open with an empty name)
                WiFi.mode(WIFI_STA);
#endif
            }
            _wifiApActive = false;
            _wifiOwnAp = false;
            _wifiApAutoChannel = false;
            _wifiApChecked = false;
            _wifiApByProg = false;
            _wifiApClients = 0;
            _wifiApSince = millis();
#ifdef DEVICE_DISPLAY_MODULE
            _wifiWidgetShowSince = 0;
#endif
        }

        /**
         * @brief Setup AP -> client. The AP counted as the link and the client gets a new interface,
         *        so run the link-loss path: the next establish rejoins the multicast groups and mDNS.
         */
        void Module::leaveWifiAp()
        {
            stopWifiAp();
            onLinkLost();
            beginWifiClient();
        }

        /**
         * @brief (Re)joins the configured network. ESP32: returns once the driver runs (a few ms, at most 1 s). RP2040: starts
         *        startWifiJoinCycle() and returns at once.
         */
        void Module::beginWifiClient()
        {
#ifdef ARDUINO_ARCH_ESP32
            WiFi.mode(WIFI_STA);
            WiFi.config(_staticLocalIP, _staticGatewayIP, _staticSubnetMask, _staticNameServerIP);
            WiFi.begin(_wifiSSID, _wifiPassphrase[0] ? _wifiPassphrase : nullptr);
            WiFi.setSleep(false);
            _wifiRetryAt = millis();
#else
            startWifiJoinCycle();
#endif
        }

#ifdef ARDUINO_ARCH_RP2040
        /**
         * @brief Rejoin in two steps: leave, then loopWifiClient() calls finishWifiJoinCycle() once WIFI_LEAVE_SETTLE_MS
         *        passed and no scan runs. Leaving first starts the join from a clean driver state. Returns at once.
         */
        void Module::startWifiJoinCycle()
        {
            const struct netif *sta = __getCYW43Netif();
            if (sta != nullptr && netif_is_up(sta)) cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
            _wifiLeftAt = millis();
            _wifiRetryAt = _wifiLeftAt;
            _wifiFailedSince = 0;
            _wifiJoinPending = true;
        }

        /**
         * @brief Second step of startWifiJoinCycle(). Interface in place: driver join. Interface gone (failed join
         *        request, AP ran): begin() rebuilds it.
         */
        void Module::finishWifiJoinCycle()
        {
            _wifiJoinPending = false;
            if (_wifiApActive || wifiOwnApConfigured()) return; // an AP started meanwhile
            const struct netif *sta = __getCYW43Netif();
            if (sta != nullptr && netif_is_up(sta))
                rejoinWifiDriver();
            else
            {
                forgetOpenknxMdns(); // begin() removes and re-adds the interface; its old service slot is gone
                WiFi.config(_staticLocalIP, _staticNameServerIP, _staticGatewayIP, _staticSubnetMask);
                startWifiJoin();
            }
            _wifiRetryAt = millis();
        }
#endif

        /**
         * @brief Stores the AP of the client link; a change while the cache already held one is logged (roaming or rejoin elsewhere).
         */
        void Module::noteWifiBssid(const uint8_t *bssid)
        {
            const bool known = _wifiBssid[0] | _wifiBssid[1] | _wifiBssid[2] | _wifiBssid[3] | _wifiBssid[4] | _wifiBssid[5];
            const bool valid = bssid[0] | bssid[1] | bssid[2] | bssid[3] | bssid[4] | bssid[5];
            if (!valid || memcmp(bssid, _wifiBssid, sizeof(_wifiBssid)) == 0) return;
            if (known)
            {
                char from[18], to[18];
                formatBssid(_wifiBssid, from);
                formatBssid(bssid, to);
                logInfoP("WiFi: AP changed %s -> %s (%d dBm, channel %u)", from, to, (int)_wifiRssi, (unsigned)_wifiChannel);
            }
            memcpy(_wifiBssid, bssid, sizeof(_wifiBssid));
        }

        /**
         * @brief From the 500 ms link tick: prog mode switched on without a link, and 5 s later still on without one,
         *        opens the setup AP: the way back onto a device whose network is gone (not in own AP mode, not when
         *        the ETS switched it off). Only the switch-on arms it, so prog mode left on does not reopen it.
         */
        void Module::checkWifiProgAp(bool up)
        {
            const bool prog = knx.progMode();
            const bool switchedOn = prog && !_wifiProgWasOn;
            _wifiProgWasOn = prog;
            if (_wifiApActive || up || !prog || _wifiApAutoChannel || wifiOwnApConfigured()
#ifdef ParamNETW_SetupAp
                || (knx.configured() && !ParamNETW_SetupAp)
#endif
            )
            {
                _wifiProgPendingSince = 0;
                return;
            }
            if (_wifiProgPendingSince == 0)
            {
                if (!switchedOn) return;
                _wifiProgPendingSince = millis() | 1; // 0 means none
                logInfoP("WiFi: prog mode without a connection, setup AP in %us", (unsigned)(WIFI_PROG_AP_DELAY_MS / 1000));
                return;
            }
            if (millis() - _wifiProgPendingSince < WIFI_PROG_AP_DELAY_MS) return;
            _wifiProgPendingSince = 0;
            startWifiAp();
            _wifiApByProg = _wifiApActive;
        }

        /**
         * @brief Client mode, from the 500 ms link tick: caches RSSI/channel/security/AP, keeps the RSSI history and,
         *        rejoins while there is no link. RP2040: startWifiJoinCycle() while the interface exists, 30 s after the
         *        join request or the last link, 10 s once the driver state held a failure for 3 s; every 2 min while the
         *        interface is gone. ESP32: every 2 min on top of the driver's own reconnect.
         */
        void Module::loopWifiClient(bool up)
        {
            if (_wifiSSID[0] == 0 || wifiOwnApConfigured()) return;
            const uint32_t now = millis();
            if (_wifiApActive || _wifiApAutoChannel)
            {
                up = false; // the AP is the link: a gap in the client history, no rejoin
                _wifiRetryAt = now;
            }
            if (up && now - _wifiInfoAt >= WIFI_INFO_MS)
            {
                _wifiInfoAt = now;
#ifdef ARDUINO_ARCH_ESP32
                wifi_ap_record_t ap;
                if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
                {
                    _wifiRssi = ap.rssi < 0 ? ap.rssi : -1; // 0 marks a gap in the history
                    _wifiChannel = ap.primary;
                    _wifiSecurity = authSecurity(ap.authmode);
                    noteWifiBssid(ap.bssid);
                }
#else
                _wifiRssi = (int8_t)std::max<int32_t>(-127, std::min<int32_t>(-1, WiFi.RSSI()));
                _wifiChannel = (uint8_t)WiFi.channel();
                _wifiSecurity = _wifiPassphrase[0] ? 3 : 0; // the core joins WPA2-AES or open only
                uint8_t bssid[6];
                noteWifiBssid(WiFi.BSSID(bssid)); // all zero while not connected
#endif
            }
            if (now - _wifiRssiAt >= WIFI_RSSI_SAMPLE_MS)
            {
                _wifiRssiAt = now;
                _wifiRssiHist[_wifiRssiHead] = up ? _wifiRssi : 0;
                _wifiRssiHead = (_wifiRssiHead + 1) % WIFI_HIST_LEN;
                if (_wifiRssiCount < WIFI_HIST_LEN) _wifiRssiCount++;
            }
            if (up)
            {
                _wifiRetryAt = now;
                _wifiRejoinLogged = false;
#ifdef ARDUINO_ARCH_RP2040
                _wifiFailedSince = 0;
                if (_wifiJoinPending && now - _wifiLeftAt > WIFI_JOIN_DROP_MS)
                {
                    // the leave did not take the link down: drop the join instead of running it on a later loss
                    _wifiJoinPending = false;
                    logInfoP("WiFi: still connected, rejoin dropped");
                }
#endif
                return;
            }
#ifdef ARDUINO_ARCH_RP2040
            if (_wifiJoinPending)
            {
                if (!_wifiScanRunning && now - _wifiLeftAt >= WIFI_LEAVE_SETTLE_MS) finishWifiJoinCycle();
                return;
            }
            if (_wifiScanRunning || _wifiProgPendingSince != 0) return;
            const struct netif *sta = __getCYW43Netif();
            if (sta != nullptr && netif_is_up(sta))
            {
                // The interface outlived the link loss: rejoin cycle (leave, join), link and DHCP follow as events.
                // Associated without an address is DHCP's/AutoIP's job: a join would restart their wait.
                // The driver state decides, not the link flag alone (a failed join can leave it as it was); a failure
                // (NONET/FAIL/BADAUTH, or DOWN after a lost link or a failed internal rejoin) held for WIFI_FAILED_HOLD_MS
                // gets the shorter interval.
                const bool joining = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_JOIN;
                if (joining)
                    _wifiFailedSince = 0;
                else if (_wifiFailedSince == 0)
                    _wifiFailedSince = now | 1; // 0 means none
                if (joining && netif_is_link_up(sta)) return;
                const bool failed = _wifiFailedSince != 0 && now - _wifiFailedSince >= WIFI_FAILED_HOLD_MS;
                if (now - _wifiRetryAt < (failed ? WIFI_REJOIN_FAILED_MS : WIFI_REJOIN_MS)) return;
                if (!_wifiRejoinLogged) logInfoP("WiFi: no connection, rejoining \"%s\"", _wifiSSID);
                _wifiRejoinLogged = true;
                startWifiJoinCycle();
                return;
            }
            // Interface gone (failed join request, or an AP ran): only begin() rebuilds it, and it can block
            if (now - _wifiRetryAt < WIFI_RETRY_MS) return;
            logInfoP("WiFi: no connection, joining \"%s\" again", _wifiSSID);
            beginWifiClient();
#else
            // The driver retries only some disconnect reasons (not e.g. a password changed on the router)
            if (_wifiScanRunning || _wifiProgPendingSince != 0 || now - _wifiRetryAt < WIFI_RETRY_MS) return;
            _wifiRetryAt = now;
            if (!_wifiRejoinLogged) logInfoP("WiFi: no connection, joining \"%s\" every %us", _wifiSSID, (unsigned)(WIFI_RETRY_MS / 1000));
            _wifiRejoinLogged = true;
            WiFi.begin(_wifiSSID, _wifiPassphrase[0] ? _wifiPassphrase : nullptr); // returns at once on the ESP32
#endif
        }

#ifdef ARDUINO_ARCH_RP2040
        /**
         * @brief Join on the existing client interface; returns at once, link and DHCP follow as events.
         *        Same authentication choice as the core's begin(): WPA2-AES with a passphrase, else open.
         */
        void Module::rejoinWifiDriver()
        {
            clearWifiLinkFlag(); // left before (startWifiJoinCycle()): the next association comes from this join, a real link edge
            const size_t pskLen = strlen(_wifiPassphrase);
            cyw43_wifi_join(&cyw43_state, strlen(_wifiSSID), (const uint8_t *)_wifiSSID, pskLen, pskLen ? (const uint8_t *)_wifiPassphrase : nullptr,
                            pskLen ? CYW43_AUTH_WPA2_AES_PSK : CYW43_AUTH_OPEN, nullptr, CYW43_CHANNEL_NONE);
        }
#endif

        /**
         * @brief "WiFi-Status" LED function, from the 500 ms link tick; changes the LED only on a state change.
         *        Client: green / yellow / orange slow blink by signal level, red blink while rejoining, red without
         *        credentials. Setup AP: blue pulsing, blue fast blink in the 5 s before. Own AP: cyan.
         */
        void Module::updateWifiLed(bool up)
        {
            if (_wifiLedFunc == nullptr || !_wifiLedFunc->active()) return;

            uint8_t state;
            if (_wifiProgPendingSince != 0)
                state = 7;
            else if (_wifiApActive || _wifiApAutoChannel)
                state = (_wifiOwnAp || _wifiApAutoChannel) ? 8 : 6; // the own AP's channel scan counts as own AP
            else if (_wifiSSID[0] == 0 || wifiOwnApConfigured())
                state = 5; // nothing to join: no credentials, or the own AP did not start (no password)
            else if (!up)
                state = 4;
            else
                state = _wifiRssi >= -65 ? 1 : _wifiRssi >= -75 ? 2 : 3; // same levels as ETS, page and display
            if (state == _wifiLedState) return;
            _wifiLedState = state;

            using OpenKNX::Led::Capability;
            using OpenKNX::Led::Color;
            switch (state)
            {
                case 1: // connected, signal good or very good
                    _wifiLedFunc->color(Color::Green);
                    _wifiLedFunc->on();
                    break;
                case 2: // connected, signal fair
                    _wifiLedFunc->color(Color::Yellow);
                    _wifiLedFunc->on();
                    break;
                case 3: // connected, signal weak
                    _wifiLedFunc->color(Color::Orange);
                    _wifiLedFunc->blinking(1000);
                    break;
                case 4: // credentials set, no connection: rejoining
                    _wifiLedFunc->color(Color::Red);
                    _wifiLedFunc->blinking(500);
                    break;
                case 5: // no credentials
                    _wifiLedFunc->color(Color::Red);
                    _wifiLedFunc->on(Capability::COLOR);
                    _wifiLedFunc->off(Capability::MONOCHROME);
                    break;
                case 6: // setup AP open
                    _wifiLedFunc->color(Color::Blue);
                    _wifiLedFunc->pulsing(1000); // 2 s cycle; a non-dimmable LED stays on
                    break;
                case 7: // prog mode without a link: setup AP in 5 s
                    _wifiLedFunc->color(Color::Blue);
                    _wifiLedFunc->blinking(200);
                    break;
                default: // own AP
                    _wifiLedFunc->color(Color::Cyan);
                    _wifiLedFunc->on();
                    break;
            }
        }

#ifdef OPENKNX_WIFI_LED_PIN
        /**
         * @brief Onboard WiFi LED, from the 500 ms link tick: on with a link or an AP, blinking while joining or
         *        scanning, off with nothing to join. On a Pico W each write is a command to the WiFi chip, hence
         *        main loop only, writes on a change only, no dimming.
         */
        void Module::updateWifiBoardLed(bool up)
        {
            uint8_t on;
            if (_wifiScanRunning || _wifiApAutoChannel)
                on = !_wifiBoardLed;
            else if (_wifiApActive)
                on = 1;
            else if (_wifiSSID[0] == 0 || wifiOwnApConfigured())
                on = 0; // no credentials, or the own AP did not start (no password)
            else
                on = up ? 1 : !_wifiBoardLed;
            if (on == _wifiBoardLed) return;
            if (_wifiBoardLed == 0xFF) pinMode(OPENKNX_WIFI_LED_PIN, OUTPUT); // no-op at the WiFi chip, needed for a plain GPIO
            _wifiBoardLed = on;
            digitalWrite(OPENKNX_WIFI_LED_PIN, on ? HIGH : LOW);
        }
#endif

        /**
         * @brief Console 'net wifi': state from the cached values, no radio access.
         */
        void Module::showWifiStatus()
        {
            static const char *const levels[] = {"", "weak", "fair", "good", "very good"};
            const WifiInfo info = wifiInfo();
            auto showClients = [&]() {
                for (uint8_t i = 0; i < info.clients; i++)
                {
                    char mac[18];
                    formatBssid(_wifiApStas[i].mac, mac);
                    logInfoP("Client:    %s  %s", mac, _wifiApStas[i].ip ? IPAddress(_wifiApStas[i].ip).toString().c_str() : "address unknown");
                }
            };
            logBegin();
            logInfoP("WiFi");
            logIndentUp();
            switch (info.state)
            {
                case WifiState::SetupAp:
                    logInfoP("State:     setup AP \"%s\", %u client(s), page http://%s/wifi", info.ssid, (unsigned)info.clients, localIP().toString().c_str());
                    showClients();
                    break;
                case WifiState::OwnAp:
                    logInfoP("State:     own AP \"%s\", %s, channel %u, %u client(s), %s", info.ssid, wifiSecurityText(info.security), (unsigned)info.channel,
                             (unsigned)info.clients, localIP().toString().c_str());
                    showClients();
                    break;
                case WifiState::Offline:
                    if (info.ssid[0] && netDowntimeSec())
                        logInfoP("State:     not connected to \"%s\", retrying (down %s)", info.ssid, humanDuration(netDowntimeSec()).c_str());
                    else if (info.ssid[0])
                        logInfoP("State:     not connected to \"%s\", retrying", info.ssid);
                    else
                        logInfoP("State:     no credentials");
                    break;
                case WifiState::Client:
                {
                    const int8_t r = info.rssi;
                    const uint8_t level = r >= -55 ? 4 : r >= -65 ? 3 : r >= -75 ? 2 : 1; // same thresholds as ETS, page and display
                    logInfoP("State:     connected to \"%s\"", info.ssid);
                    logInfoP("Signal:    %d dBm (%s), channel %u, %s", (int)r, levels[level], (unsigned)info.channel, wifiSecurityText(info.security));
                    char bssid[18];
                    formatBssid(_wifiBssid, bssid);
                    logInfoP("AP:        %s", bssid);
                    logInfoP("Address:   %s, gateway %s", localIP().toString().c_str(), gatewayIP().toString().c_str());
                    if (netReconnects())
                        logInfoP("Connected: %s, reconnects %u, last outage %s", humanDuration(netUptimeSec()).c_str(), (unsigned)netReconnects(),
                                 humanDuration(netLastDowntimeSec()).c_str());
                    else
                        logInfoP("Connected: %s", humanDuration(netUptimeSec()).c_str());
                    break;
                }
            }
            logIndentDown();
            logEnd();
        }

        /**
         * @brief Console 'net wifi reconnect': joins the configured network now, closing an open setup AP first.
         *        RP2040: startWifiJoinCycle(); ESP32: driver reconnect.
         */
        void Module::reconnectWifi()
        {
            if (_wifiSSID[0] == 0)
            {
                logErrorP("WiFi: no credentials");
                return;
            }
            if (_wifiOwnAp || wifiOwnApConfigured())
            {
                logErrorP("WiFi: an AP runs instead of the client");
                return;
            }
            if (_wifiScanRunning)
            {
                logErrorP("WiFi: scan running, try again");
                return;
            }
            if (_wifiApActive)
            {
                if (knx.configured() && !ParamNET_HTTP)
                {
                    // as in loopWifiAp(): the AP started the web server, which cannot be stopped
                    logInfoP("WiFi: closing the setup AP, restart");
                    if (_restartTimer == 0) _restartTimer = delayTimerInit();
                    return;
                }
                logInfoP("WiFi: closing the setup AP, joining \"%s\"", _wifiSSID);
                leaveWifiAp();
                return;
            }
#ifdef ARDUINO_ARCH_RP2040
            logInfoP("WiFi: rejoining \"%s\"", _wifiSSID);
            startWifiJoinCycle();
#else
            _wifiRetryAt = millis();
            if (WiFi.reconnect())
                logInfoP("WiFi: reconnecting \"%s\"", _wifiSSID);
            else
                logErrorP("WiFi: reconnect refused by the driver (its own reconnect may be running)");
#endif
        }

        /**
         * @brief Console 'net wifi ap': opens the setup AP now, credentials kept. Closes like the prog button's
         *        after 10 min without a station, 'net wifi reconnect' earlier. Drops a live client link.
         */
        void Module::openWifiSetupAp()
        {
            if (_wifiApActive || _wifiApAutoChannel || wifiOwnApConfigured())
            {
                logErrorP("WiFi: an AP runs already");
                return;
            }
#ifdef ParamNETW_SetupAp
            if (knx.configured() && !ParamNETW_SetupAp)
            {
                logErrorP("WiFi: setup AP switched off in the configuration");
                return;
            }
#endif
            if (_wifiScanRunning)
            {
                logErrorP("WiFi: scan running, try again");
                return;
            }
            if (_currentLinkState) logInfoP("WiFi: leaving \"%s\" for the setup AP", _wifiSSID);
            _wifiProgPendingSince = 0;
            startWifiAp();
            if (!_wifiApActive) return;
            _wifiApByProg = true; // on demand: the 10 min idle rule of the prog button
            onLinkLost();         // the AP counts as link, so no down edge follows: the client's link loss runs here
        }

#ifdef OPENKNX_PING
        /**
         * @brief From the 500 ms link tick: one ping per second to the gateway while /wifi is open; the result
         *        goes to the RTT history.
         */
        void Module::loopWifiPing()
        {
            const uint32_t now = millis();
            if (_wifiPingBusy && now - _wifiPingAt >= 3 * WIFI_PING_MS) _wifiPingBusy = false; // callback lost (handler reset)
            if (_wifiPingBusy || _wifiPingPolledAt == 0 || now - _wifiPingPolledAt >= WIFI_PING_IDLE_MS || now - _wifiPingAt < WIFI_PING_MS) return;
            if (_wifiApActive || !established()) return;
            const IPAddress gw = gatewayIP();
            if (gw == IPAddress()) return;
            _wifiPingAt = now;
            _wifiPingBusy = true;
            ping(gw, [this](IPAddress, bool reachable, uint32_t rttMs) {
                _wifiPingHist[_wifiPingHead] = reachable ? (uint16_t)std::min<uint32_t>(rttMs, 0xFFFE) : 0xFFFF;
                _wifiPingHead = (_wifiPingHead + 1) % WIFI_HIST_LEN;
                if (_wifiPingCount < WIFI_HIST_LEN) _wifiPingCount++;
                _wifiPingBusy = false;
            }, WIFI_PING_MS);
        }
#endif

        /**
         * @brief State for the display widget; reads only cached values.
         */
        Module::WifiInfo Module::wifiInfo() const
        {
            WifiInfo info{WifiState::Offline, _wifiSSID, _wifiRssi, _wifiChannel, _wifiSecurity, _wifiApClients};
            if (_wifiApActive)
            {
                info.state = _wifiOwnAp ? WifiState::OwnAp : WifiState::SetupAp;
                info.ssid = _wifiApSsid;
                info.security = _wifiOwnAp ? _wifiApSecurity : 0;
                if (!_wifiOwnAp) info.channel = 0;
            }
            else if (_currentLinkState)
                info.state = WifiState::Client;
            return info;
        }

        /**
         * @brief Open setup AP: only the setup page, its assets and the captive probes; console, file manager,
         *        OTA page and websockets stay closed for anyone in radio range.
         */
        bool Module::wifiSetupPathAllowed(const std::string &path) const
        {
            if (!_wifiApActive || _wifiOwnAp) return true;
            if (path == "/wifi" || path.compare(0, 6, "/wifi/") == 0 || path.compare(0, 8, "/assets/") == 0) return true;
            for (const char *probe : kCaptiveProbes)
                if (path == probe) return true;
            return false;
        }

        /**
         * @brief AP state for "info" (one line) and "net" (connection line).
         */
        void Module::showWifiApInformation(bool console)
        {
            char line[96];
            snprintf(line, sizeof(line), "%s \"%s\" (%s, %u clients)", _wifiOwnAp ? "WiFi AP" : "WiFi setup AP", _wifiApSsid,
                     WiFi.softAPIP().toString().c_str(), (unsigned)_wifiApClients);
            if (console)
                logInfoP("Connection: %s", line);
            else
                openknx.logger.logWithPrefixAndValues("Network", "%s", line);
        }

        /**
         * @brief The KNX stack joined its routing/search groups before the AP existed; join them on the AP
         *        interface so search requests and routing telegrams of a client reach the device.
         */
        void Module::joinApMulticast()
        {
#ifdef USE_IP // no KNXnet/IP on this mask (e.g. TP-only 0x07B0), so no KNX multicast group to join
#ifdef ARDUINO_ARCH_RP2040
            LWIPMutex m;
            const uint32_t apIp = (uint32_t)WiFi.softAPIP();
            for (struct netif *n = netif_list; n != nullptr; n = n->next)
            {
                if (ip4_addr_get_u32(netif_ip4_addr(n)) != apIp) continue;
                if (!RP2040ArduinoPlatform::joinMulticastGroups(n)) logErrorP("WiFi AP: multicast join failed");
                return;
            }
            logErrorP("WiFi AP: interface not found for the multicast join");
#elif defined(ARDUINO_ARCH_ESP32)
            const int idx = esp_netif_get_netif_impl_index(WiFi.AP.netif());
            if (idx <= 0)
            {
                logErrorP("WiFi AP: interface not found for the multicast join");
                return;
            }
            LOCK_TCPIP_CORE();
            struct netif *n = netif_get_by_index((u8_t)idx);
            const bool ok = n != nullptr && Esp32Platform::joinMulticastGroups(n);
            UNLOCK_TCPIP_CORE();
            if (!ok) logErrorP("WiFi AP: multicast join failed");
#endif
#endif // USE_IP
        }

        /**
         * @brief Refreshes the station list of the running AP (loopWifiAp, 1 s); the address comes from the ARP table
         *        (RP2040, last known one kept while the station stays listed) or the DHCP server (ESP32), ip 0 = not known.
         */
        void Module::pollWifiApStations()
        {
            WifiApStation stas[WIFI_AP_STA_MAX] = {};
            uint8_t num = 0;
#ifdef ARDUINO_ARCH_RP2040
            uint8_t macs[WIFI_AP_STA_MAX * 6] = {};
            int count = WIFI_AP_STA_MAX; // in: buffer size, out: stations; left as is when the driver returns early
            cyw43_wifi_ap_get_stas(&cyw43_state, &count, macs);
            static const uint8_t none[6] = {};
            LWIPMutex m;
            // zero (driver returned early) or group bit set (echoed request after a firmware error): no station
            for (uint8_t i = 0; i < count && (macs[i * 6] & 1) == 0 && memcmp(macs + i * 6, none, 6) != 0; i++, num++)
            {
                memcpy(stas[i].mac, macs + i * 6, 6);
                for (size_t k = 0; k < ARP_TABLE_SIZE; k++)
                {
                    ip4_addr_t *ip;
                    struct netif *netif;
                    struct eth_addr *eth;
                    if (etharp_get_entry(k, &ip, &netif, &eth) && memcmp(eth->addr, stas[i].mac, 6) == 0)
                    {
                        stas[i].ip = ip4_addr_get_u32(ip);
                        break;
                    }
                }
                for (uint8_t o = 0; stas[i].ip == 0 && o < WIFI_AP_STA_MAX; o++)
                    if (memcmp(_wifiApStas[o].mac, stas[i].mac, 6) == 0) stas[i].ip = _wifiApStas[o].ip; // ARP entry aged out
            }
#else
            wifi_sta_list_t list;
            if (esp_wifi_ap_get_sta_list(&list) == ESP_OK && list.num > 0)
            {
                num = (uint8_t)std::min<int>(list.num, WIFI_AP_STA_MAX); // softAP() allows 4
                esp_netif_pair_mac_ip_t pairs[WIFI_AP_STA_MAX] = {};
                for (uint8_t i = 0; i < num; i++) memcpy(pairs[i].mac, list.sta[i].mac, 6);
                esp_netif_dhcps_get_clients_by_mac(WiFi.AP.netif(), num, pairs); // unknown MACs keep ip 0
                for (uint8_t i = 0; i < num; i++)
                {
                    memcpy(stas[i].mac, pairs[i].mac, 6);
                    stas[i].ip = pairs[i].ip.addr;
                }
            }
#endif
            if (num) memcpy(_wifiApStas, stas, sizeof(stas)); // RP2040: a failed poll keeps the known addresses
            _wifiApClients = num;
        }

        /**
         * @brief Stores the password of the own AP outside the parameter memory (function property 160/5 command 2).
         */
        bool Module::saveWifiApPassword(const char *psk)
        {
#ifdef ARDUINO_ARCH_RP2040
            File file = LittleFS.open("/WIFIAP.TXT", "w");
            if (!file) return false;
            file.println(psk);
            file.close();
#else
            Preferences preferences;
            preferences.begin("WIFI", false);
            preferences.putString("APPSK", psk);
            preferences.end();
#endif
            if (wifiOwnApConfigured()) _restartTimer = delayTimerInit(); // takes effect with the next start
            return true;
        }

        /**
         * @brief Setup page, scan and save routes plus the captive-portal probe URLs of the common OSes.
         *        Registers once: Module::loop() calls it on every pass until the web server runs.
         */
        void Module::setupWifiRoutes()
        {
#ifdef ARDUINO_ARCH_ESP32
            if (s_scanLock == nullptr) s_scanLock = xSemaphoreCreateMutex(); // before webserver.start(); retried while it fails
#endif
            if (_wifiRoutesAdded) return;
            _wifiRoutesAdded = true;
            webserver.addMenuItem("WiFi", "/wifi", 30);

            webserver.addRoute(WEB_GET, "/assets/wifi.css", Webserver::Asset(WebAssets::wifi_css_mime, WebAssets::wifi_css_gz, sizeof(WebAssets::wifi_css_gz)));
            webserver.addRoute(WEB_GET, "/assets/wifi.js", Webserver::Asset(WebAssets::wifi_js_mime, WebAssets::wifi_js_gz, sizeof(WebAssets::wifi_js_gz)));

            // The page is built by wifi.js from /wifi/status; the open setup AP gets it alone, without the menu
            webserver.addRoute(WEB_GET, "/wifi", [this](WebRequest &, WebResponse &res) {
                char v[16];
                snprintf(v, sizeof(v), "?v=%lu", (unsigned long)BUILD_TIMESTAMP);
                res.setContentType("text/html");
                std::string html;
                if (_wifiApActive && !_wifiOwnAp)
                {
                    res.setLayout(false);
                    html.append("<!DOCTYPE html><html lang='de'><head><meta charset='utf-8'>"
                                "<meta name='viewport' content='width=device-width,initial-scale=1'><title>WiFi einrichten</title>"
                                "<link rel='icon' type='image/svg+xml' href='/assets/favicon.svg")
                        .append(v)
                        .append("'><link rel='stylesheet' href='/assets/base.css")
                        .append(v)
                        .append("'>");
                }
                else
                {
                    res.setLayout(true);
                    res.setActiveMenu("/wifi");
                }
                html.append("<link rel='stylesheet' href='/assets/wifi.css").append(v).append("'>");
                if (!res.useLayout())
                    html.append("</head><body class='wf-band'><header class='wf-top'><img src='/assets/logo.svg")
                        .append(v)
                        .append("' alt='OpenKNX'><span>")
                        .append(openknx.info.firmwareName())
                        .append(" &middot; Einrichtung</span></header><main>");
                html.append("<div class='container' id='wf'></div><script src='/assets/wifi.js").append(v).append("'></script>");
                if (!res.useLayout()) html.append("</main></body></html>");
                res.send(html.c_str());
            });

            webserver.addRoute(WEB_GET, "/wifi/status", [this](WebRequest &, WebResponse &res) {
#ifdef OPENKNX_PING
                _wifiPingPolledAt = millis() | 1; // keeps the gateway ping running while the page is open
#endif
                static const char *const states[] = {"offline", "client", "setup", "own"};
                const WifiInfo info = wifiInfo();
                std::string j;
                j.reserve(1152);
                addf(j, "{\"mode\":\"%s\",\"prog\":%d,\"ssid\":\"", states[(uint8_t)info.state], knx.progMode() ? 1 : 0);
                jsonEscape(j, info.ssid);
                addf(j, "\",\"rssi\":%d,\"ch\":%u,\"sec\":%u,\"clients\":%u,\"ip\":\"%s\",\"gw\":\"%s\",\"host\":\"", (int)info.rssi,
                     (unsigned)info.channel, (unsigned)info.security, (unsigned)info.clients, localIP().toString().c_str(),
                     gatewayIP().toString().c_str());
                jsonEscape(j, _hostName);
#ifdef DEVICE_DISPLAY_MODULE
                const int display = 1;
#else
                const int display = 0;
#endif
                addf(j, "\",\"mdns\":%d,\"cfg\":%d,\"disp\":%d,\"up\":%lu,\"stas\":[", (!knx.configured() || ParamNET_mDNS) ? 1 : 0,
                     knx.configured() ? 1 : 0, display, (unsigned long)netUptimeSec());
                const uint8_t sn = info.state == WifiState::OwnAp ? info.clients : 0;
                for (uint8_t i = 0; i < sn; i++)
                {
                    const WifiApStation st = _wifiApStas[i]; // ESP32: a rewrite by the loop task can garble rows for one refresh
                    char mac[18];
                    formatBssid(st.mac, mac);
                    addf(j, "%s{\"mac\":\"%s\",\"ip\":\"%s\"}", i ? "," : "", mac, st.ip ? IPAddress(st.ip).toString().c_str() : "");
                }
                j += "],\"rh\":[";
                const uint8_t rc = _wifiRssiCount, rh = _wifiRssiHead; // the loop task may advance them meanwhile (ESP32)
                for (uint8_t i = 0; i < rc; i++)
                    addf(j, i ? ",%d" : "%d", (int)_wifiRssiHist[(rh + WIFI_HIST_LEN - rc + i) % WIFI_HIST_LEN]);
#ifdef OPENKNX_PING
                j += "],\"ph\":[";
                const uint8_t pc = _wifiPingCount, ph = _wifiPingHead;
                for (uint8_t i = 0; i < pc; i++)
                {
                    const uint16_t ms = _wifiPingHist[(ph + WIFI_HIST_LEN - pc + i) % WIFI_HIST_LEN];
                    addf(j, i ? ",%d" : "%d", ms == 0xFFFF ? -1 : (int)ms);
                }
#endif
                j += "]}";
                res.setContentType("application/json");
                res.send(j.c_str());
            });

#if __has_include("qrcodegen.h")
            // QR code of t (up to 80 bytes) as {"n": size, "b": modules row-major, 4 per hex digit}
            webserver.addRoute(WEB_GET, "/wifi/qr", [](WebRequest &req, WebResponse &res) {
                static std::string s_lastText, s_lastJson; // the page asks for the same URL on every visit
                const std::string text = req.getQueryParam("t");
                res.setContentType("application/json");
                if (!s_lastJson.empty() && text == s_lastText)
                {
                    res.send(s_lastJson.c_str());
                    return;
                }
                uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
                uint8_t temp[sizeof(qr)];
                if (text.empty() || text.size() > 80 ||
                    !qrcodegen_encodeText(text.c_str(), temp, qr, qrcodegen_Ecc_MEDIUM, 1, 6, OPENKNX_QR_MASK, true))
                {
                    res.setStatus(400);
                    res.sendStatic("{}");
                    return;
                }
                const int n = qrcodegen_getSize(qr);
                std::string j;
                j.reserve(16 + (n * n + 3) / 4);
                addf(j, "{\"n\":%d,\"b\":\"", n);
                uint8_t nibble = 0, bits = 0;
                for (int y = 0; y < n; y++)
                    for (int x = 0; x < n; x++)
                    {
                        nibble = (uint8_t)((nibble << 1) | (qrcodegen_getModule(qr, x, y) ? 1 : 0));
                        if (++bits == 4)
                        {
                            j += "0123456789abcdef"[nibble];
                            nibble = 0;
                            bits = 0;
                        }
                    }
                if (bits) j += "0123456789abcdef"[(nibble << (4 - bits)) & 0xF];
                j += "\"}";
                res.send(j.c_str());
                s_lastText = text;
                s_lastJson = std::move(j);
            });
#endif

            webserver.addRoute(WEB_GET, "/wifi/scan", [this](WebRequest &req, WebResponse &res) {
                res.setContentType("application/json");
                if (req.getQueryParam("start") == "1") _wifiScanRequested = true; // started by the link tick, in the loop task
                if (_wifiScanRunning || _wifiScanRequested)
                {
                    res.sendStatic("{\"running\":true}");
                    return;
                }
                if (_wifiScanFailed)
                {
                    res.sendStatic("{\"running\":false,\"failed\":true,\"networks\":[]}");
                    return;
                }
                std::string json = "{\"running\":false,\"networks\":[";
                ScanLock lock;
                for (size_t i = 0; i < _wifiScanNets.size(); i++)
                {
                    const WifiScanNet &w = _wifiScanNets[i];
                    json += i ? ",{\"ssid\":\"" : "{\"ssid\":\"";
                    jsonEscape(json, w.ssid.c_str());
                    addf(json, "\",\"rssi\":%d,\"ch\":%u,\"sec\":%u}", (int)w.rssi, (unsigned)w.channel, (unsigned)w.security);
                }
                json += "]}";
                res.send(json.c_str());
            });

            webserver.addRoute(WEB_POST, "/wifi/save", [this](WebRequest &req, WebResponse &res) {
                res.setContentType("text/plain; charset=utf-8");
                // Credentials change only from the setup AP or in prog mode (the web UI itself has no login yet)
                if ((!_wifiApActive || _wifiOwnAp) && !knx.progMode())
                {
                    res.setStatus(403);
                    res.sendStatic("Nur im Einrichtungsmodus oder im Programmiermodus m\xC3\xB6glich.");
                    return;
                }
                const std::string ssid = req.getQueryParam("ssid");
                const std::string psk = req.getQueryParam("psk");
                if (ssid.empty() || ssid.size() > 32 || (!psk.empty() && (psk.size() < 8 || psk.size() > 63)))
                {
                    res.setStatus(400);
                    res.sendStatic("Netzname 1-32 Zeichen, Passwort leer oder 8-63 Zeichen.");
                    return;
                }
                logInfoP("WiFi SSID: %s (web)", ssid.c_str());
                saveWifiSettings(ssid.c_str(), psk.c_str(), true); // restart timer: the answer goes out first
                res.sendStatic("Gespeichert. Das Ger\xC3\xA4t startet neu.");
            });

            // Captive-portal probes: open the setup page while the setup AP runs (302: never cached by the client)
            for (const char *probe : kCaptiveProbes)
                webserver.addRoute(WEB_GET, probe, [this](WebRequest &, WebResponse &res) {
                    if (_wifiApActive && !_wifiOwnAp)
                    {
                        std::string target;
                        addf(target, "http://%s/wifi", WiFi.softAPIP().toString().c_str());
                        res.setStatus(302);
                        res.setHeader("Location", target.c_str());
                        res.send("");
                    }
                    else
                        res.setStatus(404);
                });
        }
#endif // OPENKNX_WIFI_AP
    } // namespace Network
} // namespace OpenKNX
#endif // OPENKNX_WIFI_SCAN
#endif // KNX_IP_WIFI
