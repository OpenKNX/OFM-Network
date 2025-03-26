#pragma once
#include "OpenKNX.h"
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
    #include <esp_http_server.h>
    #include <esp_err.h>
    #include "versions.h"
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

#if defined(ARDUINO_ARCH_ESP32)
    #ifndef WEBSERVER_BASE_URI
        #define WEBSERVER_BASE_URI "/openknx"
    #endif
    struct WebserverHandler {
        httpd_uri_t httpd;
        std::string uri;
        std::string name;
        bool isVisible = true;
    };
    struct WebserverPage {
        std::string uri;
        std::string name;
        bool isVisible = true;
        esp_err_t (*handler)(const char *uri, httpd_req_t *r, void *arg);
        void *arg;
    };
    static const char index_html[] = "<html><head><title>OpenKNX Webserver</title></head><body style='font-family:Arial, sans-serif;margin:20px;background-color:#f4f4f4;'><header style='position:relative;padding:10px 0;'><img src='https://raw.githubusercontent.com/OpenKNX/.github/main/profile/images/weiss.svg' alt='Logo' style='width:150px;height:auto;top:20px;right:20px;position:absolute;'><h1>Startseite</h1></header>";
    static const char webserver_base_uri[] = WEBSERVER_BASE_URI;
    static const uint8_t webserver_base_uri_len = strlen(webserver_base_uri);
#endif

typedef std::function<void(bool)> NetworkChangeCallback;

class NetworkModule : public OpenKNX::Module
{
  public:
    const std::string name() override;
    const std::string version() override;
    void init() override;
    void loop(bool configured) override;
    void setup(bool configured) override;

    void savePower() override;
    bool restorePower() override;
    void showInformations() override;
    bool processCommand(const std::string cmd, bool debugKo);
    void showHelp() override;
    void showNetworkInformations(bool console = false);

    bool processFunctionProperty(uint8_t objectIndex, uint8_t propertyId, uint8_t length, uint8_t *data, uint8_t *resultData, uint8_t &resultLength) override;

#ifdef KNX_IP_WIFI
    void saveWifiSettings(const char *ssid, const char *passphrase);
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

#ifdef ARDUINO_ARCH_ESP32
    void esp32NetworkEvent(arduino_event_id_t event);
#endif

#if defined(ARDUINO_ARCH_ESP32)
    const char * getWebserverBaseUri();
    void addWebserverHandler(WebserverHandler handler);
    void addWebserverPage(WebserverPage p);
    httpd_handle_t getWebserverHandler();
    std::vector<WebserverHandler> _webserverHandler;
    std::vector<WebserverPage> _webserverPages;
#endif

  private:
#if defined(ARDUINO_ARCH_ESP32)
    bool _firstLoop = true;
    httpd_handle_t _webserver = NULL;
    static esp_err_t webserver_base_handler(httpd_req_t *req);
    void handleWebserver();
#endif

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

    char *_mDNSHttpServiceName = nullptr;
    char *_mDNSDeviceServiceName = nullptr;
    char *_mDNSDeviceServiceNameTXT = nullptr;
    bool _currentLinkState = false;
    uint32_t _lastLinkCheck = false;
    uint32_t _restartTimer = 0;

    void initPhy();
    void initIp();
    void loadSettings();
    void checkLinkStatus();
    void checkIpStatus();
    void loadCallbacks(bool state);
    void handleMDNS();
    void handleOTA();

#ifdef KNX_IP_WIFI
    char _wifiSSID[33] = {};
    char _wifiPassphrase[64] = {};
#endif

    std::vector<NetworkChangeCallback> _callback;


};

extern NetworkModule openknxNetwork;