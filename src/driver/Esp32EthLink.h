#pragma once
// Live ETH link-mode control on ESP32.
//
// The esp_eth link-config ioctls (ETH_CMD_S_AUTONEGO / S_SPEED / S_DUPLEX_MODE) are only accepted while the
// driver is STOPPED (esp_eth FSM guard); once ETH.begin() has started it they return ESP_ERR_INVALID_STATE.
// That is also WHY ETHClass forbids its public setLinkSpeed()/setAutoNegotiation()/setFullDuplex() after
// begin(), and why its private _setLinkSpeed() etc. run strictly BEFORE esp_eth_start() - not "at runtime".
// So to change the mode live we stop -> reconfigure -> start the driver (esp_eth_stop / ioctl / esp_eth_start),
// which re-links the W5500 at the forced speed like a brief cable re-plug (and emits the DOWN->UP the ladder
// needs). This is the ESP32 counterpart to W5500Phy (raw PHYCFGR) on RP2040 - NOT a full ETH.end()/begin()
// teardown, so it does not risk the driver-reinit brick seen on the RP2040 lwIP W5500.
//
// Own translation unit so it compiles to nothing off-target (ESP32 + KNX_IP_LAN only) and is easy to review.
// Returns false if the driver rejects a step (so the caller can surface it) instead of silently "succeeding".
#if defined(ARDUINO_ARCH_ESP32) && defined(KNX_IP_LAN)
namespace Esp32EthLink
{
    bool forceMode(bool speed100, bool full); // auto-negotiation off + fixed speed/duplex (stop/reconfigure/start)
    bool setAutoNeg();                        // auto-negotiation on (stop/reconfigure/start)
} // namespace Esp32EthLink
#endif
