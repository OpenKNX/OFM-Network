#include "Esp32EthLink.h"
#if defined(ARDUINO_ARCH_ESP32) && defined(KNX_IP_LAN)
#include <ETH.h>
#include <esp_eth.h>

namespace Esp32EthLink
{

    // Stop -> reconfigure -> start: the link-config ioctls are only accepted while the driver is STOPPED. We
    // always start again (even if a step failed) so the link is never left down, and return true only if every
    // reconfigure step reported ESP_OK.
    bool forceMode(bool speed100, bool full)
    {
        esp_eth_handle_t h = ETH.handle();
        if (h == nullptr) return false;

        esp_err_t err = esp_eth_stop(h);
        bool autoneg = false; // must be off before a fixed speed/duplex is accepted (S_SPEED checks this)
        if (err == ESP_OK) err = esp_eth_ioctl(h, ETH_CMD_S_AUTONEGO, &autoneg);
        eth_speed_t speed = speed100 ? ETH_SPEED_100M : ETH_SPEED_10M;
        if (err == ESP_OK) err = esp_eth_ioctl(h, ETH_CMD_S_SPEED, &speed);
        eth_duplex_t duplex = full ? ETH_DUPLEX_FULL : ETH_DUPLEX_HALF;
        if (err == ESP_OK) err = esp_eth_ioctl(h, ETH_CMD_S_DUPLEX_MODE, &duplex);
        esp_eth_start(h);
        return err == ESP_OK;
    }

    bool setAutoNeg()
    {
        esp_eth_handle_t h = ETH.handle();
        if (h == nullptr) return false;

        esp_err_t err = esp_eth_stop(h);
        bool autoneg = true;
        if (err == ESP_OK) err = esp_eth_ioctl(h, ETH_CMD_S_AUTONEGO, &autoneg);
        esp_eth_start(h);
        return err == ESP_OK;
    }

    bool advertise(uint16_t abilities)
    {
        static constexpr uint32_t REG_BMSR = 1, REG_ANAR = 4;
        static constexpr uint32_t ANAR_ABILITIES = ANAR_100_FULL | ANAR_100_HALF | ANAR_10_FULL | ANAR_10_HALF;
        static constexpr uint32_t ANAR_KEEP = 0x0C1F; // pause and selector; reserved bits are written as 0

        esp_eth_handle_t h = ETH.handle();
        if (h == nullptr) return false;

        // The link timer reads over the same MDIO bus without a lock, so it has to stop first; a running tick ends within 2 ms.
        esp_err_t err = esp_eth_stop(h);
        delay(2);
        uint32_t bmsr = 0, anar = 0;
        esp_eth_phy_reg_rw_data_t status = {REG_BMSR, &bmsr}, offer = {REG_ANAR, &anar};
        if (err == ESP_OK) err = esp_eth_ioctl(h, ETH_CMD_READ_PHY_REG, &status);
        if (err == ESP_OK) err = esp_eth_ioctl(h, ETH_CMD_READ_PHY_REG, &offer);
        const uint32_t usable = abilities & (bmsr >> 6) & ANAR_ABILITIES; // BMSR bits 14:11 = ANAR bits 8:5
        if (err == ESP_OK && usable == 0) err = ESP_FAIL;
        if (err == ESP_OK)
        {
            anar = (anar & ANAR_KEEP) | usable;
            err = esp_eth_ioctl(h, ETH_CMD_WRITE_PHY_REG, &offer);
        }
        const esp_err_t started = esp_eth_start(h); // restarts the negotiation with the new offer
        return err == ESP_OK && started == ESP_OK;
    }

} // namespace Esp32EthLink
#endif
