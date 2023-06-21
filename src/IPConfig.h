#include "OpenKNX.h"
#ifndef RP2040WIFI
#include <Ethernet_Generic.h>
#else
#include <WiFi.h>
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
};

//Stream* ethernet_logger = new OpenKNX::Log::VirtualSerial("Eth");

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

#ifndef RP2040WIFI
    uint32_t serial = knx.platform().uniqueSerialNumber();
    uint8_t serialBytes[4];
    pushInt(knx.platform().uniqueSerialNumber(), serialBytes);
    byte mac[] = {0x60, 0x4A, 0x7B, serialBytes[1], serialBytes[2], serialBytes[3]};

    logInfoP("MAC: ");
    logHexInfoP(mac, 6);

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

    pinMode(PIN_SS_, OUTPUT);
    digitalWrite(PIN_SS_, HIGH); // Todo check if this is neccessary
    spi->setRX(PIN_MISO_);
    spi->setTX(PIN_MOSI_);
    spi->setSCK(PIN_SCK_);
    spi->setCS(PIN_SS_);

    logInfoP("Ethernet SPI GPIO: RX/MISO: %d, TX/MOSI: %d, SCK/SCLK: %d, CSn/SS: %d", PIN_MISO_, PIN_MOSI_, PIN_SCK_, PIN_SS_);

    Ethernet.init(PIN_SS_);

    if(knx.configured())
    {
        uint8_t NoOfElem = 30;
        uint8_t *FriendlyName;
        uint32_t length;
        knx.bau().propertyValueRead(OT_IP_PARAMETER, 0, PID_FRIENDLY_NAME, NoOfElem, 1, &FriendlyName, length);
        Ethernet.setHostname((const char *)FriendlyName);
        delete[] FriendlyName;
    }
    else
    {
        Ethernet.setHostname(MAIN_OrderNumber);
    }

    logInfoP("HostName: %s", Ethernet.hostName());

    //uint8_t NoOfElem = 1;
    //uint8_t *data;
    //uint32_t length;
    //knx.bau().propertyValueRead(OT_IP_PARAMETER, 0, PID_IP_ASSIGNMENT_METHOD, NoOfElem, 1, &data, length);
    
    uint8_t EthernetState = 1;
    //switch(*data)
    switch(GetByteProperty(PID_IP_ASSIGNMENT_METHOD))
    {
        case 1: // manually see 2.5.6 of 03_08_03
        {
            logInfoP("Use Static IP");
            
            Ethernet.begin(mac, GetIpProperty(PID_IP_ADDRESS), IPAddress(8,8,8,8), GetIpProperty(PID_DEFAULT_GATEWAY), GetIpProperty(PID_SUBNET_MASK));
            //Ethernet.setDnsServerIP(IPAddress(8,8,8,8));    // use Google DNS unless we get a param for that
            // ToDo: set PID_CURRENT_IP_ASSIGNMENT_METHOD to 1
            break;
        }
        case 4: // DHCP see 2.5.6 of 03_08_03
        default:
        {
            logInfoP("Use DHCP");
            EthernetState = Ethernet.begin(mac);
            // ToDo: set PID_CURRENT_IP_ASSIGNMENT_METHOD to 4
            // set PID_DHCP_BOOTP_SERVER = 63,
            break;
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
#else
    if(knx.configured())
    {
        uint8_t NoOfElem = 30;
        uint8_t *FriendlyName;
        uint32_t length;
        knx.bau().propertyValueRead(OT_IP_PARAMETER, 0, PID_FRIENDLY_NAME, NoOfElem, 1, &FriendlyName, length);
        WiFi.setHostname((const char *)FriendlyName);
        delete[] FriendlyName;
    }
    else
    {
        WiFi.setHostname(MAIN_OrderNumber);
    }

    logTraceP("HostName: %s", WiFi.hostName());

    uint8_t EthernetState = 1;
    switch(GetByteProperty(PID_IP_ASSIGNMENT_METHOD))
    {
        case 1: // manually see 2.5.6 of 03_08_03
        {
            logInfoP("Use Static IP");
            
            WiFi.config(GetIpProperty(PID_IP_ADDRESS), IPAddress(8,8,8,8), GetIpProperty(PID_DEFAULT_GATEWAY), GetIpProperty(PID_SUBNET_MASK));
            EthernetState = WiFi.begin("dsnet", "huitza92gegO");
            // ToDo: set PID_CURRENT_IP_ASSIGNMENT_METHOD to 1
            break;
        }
        case 4: // DHCP see 2.5.6 of 03_08_03
        default:
        {
            logInfoP("Use DHCP");
            EthernetState = WiFi.begin("dsnet", "huitza92gegO");
            // ToDo: set PID_CURRENT_IP_ASSIGNMENT_METHOD to 4
            break;
        }
    }

    if(EthernetState)
    {
        logInfoP("Connected! IP address: %s", WiFi.localIP().toString().c_str());
    }
    else
    {

    }
#endif

}

void IPConfigModule::loop()
{

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
#ifndef RP2040WIFI
    openknx.logger.logWithPrefixAndValues("IP-Address", "%s", Ethernet.localIP().toString().c_str());
    openknx.logger.logWithPrefixAndValues("LAN-Port", "Speed: %S, Duplex: %s, Link state: %s", Ethernet.speedReport(), Ethernet.duplexReport(), Ethernet.linkReport());
#else
    openknx.logger.logWithPrefixAndValues("IP-Address:", WiFi.localIP().toString().c_str());
    //openknx.logger.logWithPrefixAndValues("SSID:", WiFi.SSID());
    //openknx.logger.logWithPrefixAndValues("SSID: dBm", WiFi.RSSI());
    //openknx.logger.logWithPrefixAndValues("SSID: dBm", WiFi.status());
#endif
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