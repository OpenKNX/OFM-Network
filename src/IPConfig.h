#include "OpenKNX.h"
#include <Ethernet_Generic.h>


class IPConfigModule : public OpenKNX::Module
{
	public:
		const std::string name() override;
		const std::string version() override;
        void setup() override;
        void loop() override;

	private:
        uint8_t *_data;
        IPAddress GetIpProperty(uint8_t PropertyId);
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

void IPConfigModule::setup()
{
    logInfoP("Test");

        // START ETHERNET stuff
    {
        byte mac[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0x01};
        logInfoP("Prepare Ethernet ....");

        randomSeed(millis());

        // Setup the GPIOs TODO: SPI / SPI1 selection.
        pinMode(PIN_SS_, OUTPUT);
        digitalWrite(PIN_SS_, HIGH);
        SPI1.setRX(PIN_MISO_);
        SPI1.setTX(PIN_MOSI_);
        SPI1.setSCK(PIN_SCK_);
        SPI1.setCS(PIN_SS_);
        //SPI.setRX(PIN_MISO_);
        //SPI.setTX(PIN_MOSI_);
        //SPI.setSCK(PIN_SCK_);
        //SPI.setCS(PIN_SS_);

        logInfoP("Setup Pins done ....");

        Ethernet.init(PIN_SS_);
        logInfoP("Initialized ");

        uint8_t NoOfElem = 1;
        uint8_t *data;
        uint32_t length;
        knx.bau().propertyValueRead(OT_IP_PARAMETER, 0, PID_IP_ASSIGNMENT_METHOD, NoOfElem, 1, &data, length);
        uint8_t EthernetState = 1;

        switch(*data)
        {
            case 1: // manually see 2.5.6 of 03_08_03
            {
                logInfoP("Use Static IP");
                Ethernet.begin(mac, GetIpProperty(PID_IP_ADDRESS), IPAddress(0), GetIpProperty(PID_DEFAULT_GATEWAY), GetIpProperty(PID_SUBNET_MASK));
                // ToDo: set PID_CURRENT_IP_ASSIGNMENT_METHOD to 1
                break;
            }
            case 4: // DHCP see 2.5.6 of 03_08_03
            default:
            {
                logInfoP("Use DHCP");
                EthernetState = Ethernet.begin(mac);
                // ToDo: set PID_CURRENT_IP_ASSIGNMENT_METHOD to 4
                break;
            }
        }

        if(EthernetState)
        {
            logInfoP("Connected! IP address: %s", Ethernet.localIP().toString());
        }
        else
        {

        }

        if ((Ethernet.getChip() == w5500) || (Ethernet.getChip() == w6100) || (Ethernet.getAltChip() == w5100s))
        {
            if (Ethernet.getChip() == w6100)
                logInfoP("W6100 => ");
            else if (Ethernet.getChip() == w5500)
                logInfoP("W5500 => ");
            else
                logInfoP("W5100S => ");

            logInfoP("Speed: %S, Duplex: %s, Link state: %s", Ethernet.speedReport(), Ethernet.duplexReport(), Ethernet.linkReport());

            // Serial.print(F("Speed: "));
            // Serial.print(Ethernet.speedReport());
            // Serial.print(F(", Duplex: "));
            // Serial.print(Ethernet.duplexReport());
            // Serial.print(F(", Link status: "));
            // Serial.println(Ethernet.linkReport());
        }

        // todo PID_IP_ASSIGNMENT_METHOD

        // Serial.println("KNX Properties:");
        // Serial.print(F("PID_IP_ADDRESS: "));
        // Serial.println(GetIpProperty(PID_IP_ADDRESS));
        // Serial.print(F("PID_SUBNET_MASK: "));
        // Serial.println(GetIpProperty(PID_SUBNET_MASK));
        // Serial.print(F("PID_DEFAULT_GATEWAY: "));
        // Serial.println(GetIpProperty(PID_DEFAULT_GATEWAY));
    }
    // end Ethernet stuff
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