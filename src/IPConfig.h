#include "OpenKNX.h"

#if defined(KNX_IP_W5500)
#include <W5500lwIP.h>
Wiznet5500lwIP KNX_NETIF(PIN_ETH_SS, ETH_SPI_INTERFACE);
#elif defined(KNX_IP_WIFI)
#include <WiFi.h>
#else
#error "no Ethernet stack specified, #define KNX_IP_WIFI or KNX_IP_W5500"
#endif


WiFiUDP Udp;


class IPConfigModule : public OpenKNX::Module
{
	public:
		const std::string name() override;
		const std::string version() override;
        void init() override;
        void loop() override;
        void showInformations() override;
        bool processCommand(const std::string cmd, bool debugKo);
        void showHelp() override;

	private:
        uint8_t *_data;
        IPAddress GetIpProperty(uint8_t PropertyId);
        void SetIpProperty(uint8_t PropertyId, IPAddress IPAddress);
        uint8_t GetByteProperty(uint8_t PropertyId);
        void SetByteProperty(uint8_t PropertyId, uint8_t value);
        uint8_t _linkstate = 0;
        IPAddress _localIP = 0;
        IPAddress _subnetMask = 0;
        IPAddress _gatewayIP = 0;
        uint8_t _mac[6] = {0x66, 0x4A, 0x7B, 0, 0, 0};
        uint8_t* _friendlyName;
        bool _useStaticIP;
        uint32_t _lastLinkCheck;
};

//Give your Module a name
//it will be displayed when you use the method log("Hello")
// -> Log     Hello
const std::string IPConfigModule::name()
{
    return "IPConfig";
}

//You can also give it a version
//will be displayed in Command Infos 
const std::string IPConfigModule::version()
{
    return "0.0dev";
}

void IPConfigModule::init()
{
    logInfoP("Init IP Stack");

    SetByteProperty(PID_IP_CAPABILITIES, 6);    // AutoIP + DHCP
    
    uint32_t serial = knx.platform().uniqueSerialNumber();
    uint8_t serialBytes[4];
    pushInt(knx.platform().uniqueSerialNumber(), serialBytes);
    _mac[3] = serialBytes[1];
    _mac[4] = serialBytes[2];
    _mac[5] = serialBytes[3];

    logInfoP("MAC: %02X:%02X:%02X:%02X:%02X:%02X", _mac[0], _mac[1], _mac[2], _mac[3], _mac[4], _mac[5]);

    randomSeed(millis());

#ifdef KNX_IP_W5500
    // Hardreset of W5500 ToDo
    //Ethernet.setRstPin(PIN_ETH_RES);
    //Ethernet.hardreset();

    ETH_SPI_INTERFACE.setRX(PIN_ETH_MISO);
    ETH_SPI_INTERFACE.setTX(PIN_ETH_MOSI);
    ETH_SPI_INTERFACE.setSCK(PIN_ETH_SCK);
    ETH_SPI_INTERFACE.setCS(PIN_ETH_SS);

    logInfoP("Ethernet SPI GPIO: RX/MISO: %d, TX/MOSI: %d, SCK/SCLK: %d, CSn/SS: %d", PIN_ETH_MISO, PIN_ETH_MOSI, PIN_ETH_SCK, PIN_ETH_SS);
#endif


    if(knx.configured())
    {
        uint8_t NoOfElem = 30;
        uint32_t length;
        knx.bau().propertyValueRead(OT_IP_PARAMETER, 0, PID_FRIENDLY_NAME, NoOfElem, 1, &_friendlyName, length);

        _gatewayIP = GetIpProperty(PID_DEFAULT_GATEWAY);
        _subnetMask = GetIpProperty(PID_SUBNET_MASK);
        _localIP = GetIpProperty(PID_IP_ADDRESS);
        _useStaticIP = GetByteProperty(PID_IP_ASSIGNMENT_METHOD) == 1; // see 2.5.6 of 03_08_03
    }
    else
    {
        _friendlyName = (uint8_t*)MAIN_OrderNumber;
    }

#if defined(KNX_IP_W5500)
    KNX_NETIF.hostname((const char *)_friendlyName);
    logInfoP("HostName: %s", KNX_NETIF.hostname().c_str());

    uint8_t EthernetState = 1;

    if(_useStaticIP)
    {
        logInfoP("Using Static IP");

        KNX_NETIF.config(_localIP, _gatewayIP, _subnetMask, IPAddress(8,8,8,8), IPAddress(4,4,4,4));
        SetByteProperty(PID_CURRENT_IP_ASSIGNMENT_METHOD, 1);
    }
    else
    {
        logInfoP("Using DHCP");
        SetByteProperty(PID_CURRENT_IP_ASSIGNMENT_METHOD, 2); // ToDo
    }

    if(!KNX_NETIF.begin())
    {
        openknx.hardware.fatalError(1, "Error communicating with W5500 Ethernet chip");
    }

    _linkstate = KNX_NETIF.isLinked();
    if(!_linkstate)
    {
        logInfoP("Lan Link missing");
    }
#elif defined(KNX_IP_WIFI)
    //ToDo for WiFi
    #pragma warn "Implementation for WiFi missing"
#endif


   
    
}

void IPConfigModule::loop()
{
    if(!delayCheckMillis(_lastLinkCheck, 500))
        return;

#if defined(KNX_IP_W5500)
    uint8_t newLinkState = KNX_NETIF.isLinked();

    // got link
    if(newLinkState && !_linkstate)
    {

        logInfoP("LAN Link established.");
        netif_set_link_up(KNX_NETIF.getNetIf());
    }

    // lost link
    else if(!newLinkState && _linkstate)
    {
        netif_set_link_down(KNX_NETIF.getNetIf());
        logInfoP("LAN Link lost.");
    }

    _linkstate = newLinkState;
#elif defined(KNX_IP_WIFI)
    //ToDo for WiFi
    #pragma warn "Implementation for WiFi missing"
#endif
    
    _lastLinkCheck = millis();
}

IPAddress IPConfigModule::GetIpProperty(uint8_t PropertyId)
{
    uint8_t NoOfElem = 1;
    uint8_t *data;
    uint32_t length;
    knx.bau().propertyValueRead(OT_IP_PARAMETER, 0, PropertyId, NoOfElem, 1, &data, length);
    IPAddress ret = (data[3] << 24) + (data[2] << 16) + (data[1] << 8) + data[0];
    delete[] data;
    return ret;
}

void IPConfigModule::SetIpProperty(uint8_t PropertyId, IPAddress IPAddress)
{
    uint8_t NoOfElem = 1;
    uint8_t data[4];

    data[0] = IPAddress[0];
    data[1] = IPAddress[1];
    data[2] = IPAddress[2];
    data[3] = IPAddress[3];

    knx.bau().propertyValueWrite(OT_IP_PARAMETER, 0, PropertyId, NoOfElem, 1, data, 0);
}

uint8_t IPConfigModule::GetByteProperty(uint8_t PropertyId)
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

void IPConfigModule::SetByteProperty(uint8_t PropertyId, uint8_t value)
{
    uint8_t NoOfElem = 1;
    uint8_t data[1];

    data[0] = value;

    knx.bau().propertyValueWrite(OT_IP_PARAMETER, 0, PropertyId, NoOfElem, 1, data, 0);
}


void IPConfigModule::showInformations()
{
    openknx.logger.logWithPrefixAndValues("IP-Address", "%s", KNX_NETIF.localIP().toString().c_str());
    openknx.logger.logWithPrefixAndValues("MAC-Address", "%02X:%02X:%02X:%02X:%02X:%02X", _mac[0], _mac[1], _mac[2], _mac[3], _mac[4], _mac[5]);

#if defined(KNX_IP_W5500)
    openknx.logger.logWithPrefixAndValues("LAN-Port", "%s",_linkstate?"Linked":"No Link");
#elif defined(KNX_IP_WIFI)
    openknx.logger.logWithPrefixAndValues("SSID", "%s", KNX_NETIF.SSID());
#endif
}

bool IPConfigModule::processCommand(const std::string cmd, bool debugKo)
{
#if defined(KNX_IP_W5500)

#elif defined(KNX_IP_WIFI)

#endif
    
    //if(cmd == "xxx")
    //{
    //    logInfoP("Test123");
    //    return true;
    //}
    return false;
}

void IPConfigModule::showHelp()
{
    //openknx.logger.log("", ">  x  <  Foo [%s]", name().c_str());
}