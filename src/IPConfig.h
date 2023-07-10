#include "OpenKNX.h"

#if defined(KNX_ETH_GEN)
#include <Ethernet_Generic.h>
#elif defined(KNX_WIFI)
#include <WiFi.h>
#else
#error "no Ethernet stack specified, #define KNX_WIFI or KNX_ETH_GEN"
#endif


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
        uint8_t* _mac;
        uint8_t* _friendlyName;
        bool _useStaticIP;
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
    uint8_t mac[] = {0x60, 0x4A, 0x7B, serialBytes[1], serialBytes[2], serialBytes[3]};
    _mac = (uint8_t*)mac;

    logInfoP("MAC: ");
    logHexInfoP(_mac, 6);

    randomSeed(millis());

    SPIClassRP2040 *spi;
    if(USING_SPI2)
    {
        spi = &SPI1;
        logInfoP("Using SPI1 for Ethernet");
    }
    else
    {
        spi = &SPI;
        logInfoP("Using SPI for Ethernet");
    }

    // Hardreset of W5500
    Ethernet.setRstPin(PIN_ETH_RES);
    Ethernet.hardreset();

    spi->setRX(PIN_MISO_);
    spi->setTX(PIN_MOSI_);
    spi->setSCK(PIN_SCK_);
    spi->setCS(PIN_SS_);

    logInfoP("Ethernet SPI GPIO: RX/MISO: %d, TX/MOSI: %d, SCK/SCLK: %d, CSn/SS: %d", PIN_MISO_, PIN_MOSI_, PIN_SCK_, PIN_SS_);

    Ethernet.init(PIN_SS_);

    if(knx.configured())
    {
        uint8_t NoOfElem = 30;
        uint32_t length;
        knx.bau().propertyValueRead(OT_IP_PARAMETER, 0, PID_FRIENDLY_NAME, NoOfElem, 1, &_friendlyName, length);
        Ethernet.setHostname((const char *)_friendlyName);

        _gatewayIP = GetIpProperty(PID_DEFAULT_GATEWAY);
        _subnetMask = GetIpProperty(PID_SUBNET_MASK);
        _localIP = GetIpProperty(PID_IP_ADDRESS);
        _useStaticIP = GetByteProperty(PID_IP_ASSIGNMENT_METHOD) == 1; // see 2.5.6 of 03_08_03
    }
    else
    {
        _friendlyName = (uint8_t*)MAIN_OrderNumber;
    }

    Ethernet.setHostname((const char *)_friendlyName);
    logInfoP("HostName: %s", Ethernet.hostName());

    
    uint8_t EthernetState = 1;

    if(_useStaticIP)
    {
        logInfoP("Use Static IP");
        
        Ethernet.begin(_mac, _localIP, IPAddress(8,8,8,8), _gatewayIP, _subnetMask);
        SetByteProperty(PID_CURRENT_IP_ASSIGNMENT_METHOD, 1);
    }
    else
    {
        logInfoP("Use DHCP");
        EthernetState = Ethernet.begin(_mac, 10000); // 10s DHCP timeout
        if(EthernetState)
            SetByteProperty(PID_CURRENT_IP_ASSIGNMENT_METHOD, 4);
        else
        {
            // Assign AutoIP in 169.254. range based on serial number to avoid collisions
            uint8_t oct3 = _mac[2];
            uint8_t oct4 = _mac[3];
            if(oct3==0)
                oct3++;
            if(oct3==255)
                oct3--;
            if(oct4==0)
                oct3++;
            if(oct4==255)
                oct3--;
            Ethernet.setLocalIP(IPAddress(169,254,oct3,oct4));
            Ethernet.setSubnetMask(IPAddress(255,255,0,0));
            SetByteProperty(PID_CURRENT_IP_ASSIGNMENT_METHOD, 8);
        }
    }

    if(EthernetState)
    {
        logInfoP("Connected! IP address: %s", Ethernet.localIP().toString().c_str());
    }
    else
    {

    }

    if(Ethernet.getChip() == noChip)
    {
        logErrorP("Error communicating with Ethernet chip");
    }
    else
    {
        logInfoP("Speed: %S, Duplex: %s, Link state: %s", Ethernet.speedReport(), Ethernet.duplexReport(), Ethernet.linkReport());
    }

    _linkstate = Ethernet.link();
}

void IPConfigModule::loop()
{
    // ToDo: do this only every xxx ms
    uint8_t newLinkState = Ethernet.link();

    // got link
    if(newLinkState && !_linkstate)
    {
        Ethernet.maintain();
    }
    // lost link
    else if(!newLinkState && _linkstate)
    {

    }
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
    openknx.logger.logWithPrefixAndValues("IP-Address", "%s", Ethernet.localIP().toString().c_str());
    openknx.logger.logWithPrefixAndValues("LAN-Port", "Speed: %S, Duplex: %s, Link state: %s", Ethernet.speedReport(), Ethernet.duplexReport(), Ethernet.linkReport());

}

bool IPConfigModule::processCommand(const std::string cmd, bool debugKo)
{
    if(cmd == "xxx")
    {
        logInfoP("Test123");
        return true;
    }
    return false;
}

void IPConfigModule::showHelp()
{
    //openknx.logger.log("", ">  x  <  Foo [%s]", name().c_str());
}