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

} // namespace Esp32EthLink
#endif
