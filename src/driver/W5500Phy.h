#pragma once
// Minimal, self-contained W5500 access layer via raw SPI - for things the Wiznet5500lwIP driver keeps
// private (PHY link mode/status, chip version, and any other register a future feature might need).
//
// Why: the driver's register helpers (wizchip_read/write, get/setPHYCFGR, wizphy_*) are PRIVATE, so we
// talk to the SAME SPI bus + CS pin ourselves, guarded by the driver's lwIP lock so we never race an
// in-flight driver transaction. Kept in its own translation unit so it compiles to nothing off-target
// (RP2040 + W5500 LAN only) and is easy to review / swap for a real lib later.
//
// Nice property vs. ESP32 (link speed must be set BEFORE ETH.begin()): forcing the W5500 PHY mode
// applies LIVE (OPMD + OPMDC + PHY reset), so an auto-fallback can escalate without a reboot.
//
// NEEDS HARDWARE VALIDATION (raw SPI to a live driver).
#if defined(ARDUINO_ARCH_RP2040) && defined(OPENKNX_ETH_W5500)
#include <Arduino.h>
#include <SPI.h>

class W5500Phy
{
  public:
    struct Status
    {
        bool linked;
        bool speed100;   // true = 100 Mbit, false = 10 Mbit
        bool fullDuplex; // true = full, false = half
    };

    W5500Phy(SPIClass &spi, int csPin) : _spi(&spi), _cs(csPin) {}

    // --- PHY link mode (PHYCFGR 0x002E) ---
    Status read();                            // link / speed / duplex status
    void forceMode(bool speed100, bool full); // OPMD + OPMDC(speed/duplex) + PHY reset (applies live)
    void setAutoNeg();                        // OPMD + all-capable auto-negotiation + PHY reset (live)

    // ESP32 ETHClass-style read accessors (keep higher-level code platform-symmetric):
    bool linkUp() { return read().linked; }
    uint16_t linkSpeed()
    {
        Status s = read();
        return s.speed100 ? 100 : 10;
    } // Mbit
    bool fullDuplex() { return read().fullDuplex; }

    // --- health / identification ---
    uint8_t chipVersion(); // VERSIONR (0x0039); a present W5500 returns 0x04
    bool present() { return chipVersion() == 0x04; }

    // --- generic register access (under the driver's lwIP lock) ---
    // Lets future features reach any W5500 register without extending this class.
    // block: 0x00 = common register block; socket blocks per datasheet. addr: register address.
    uint8_t readReg(uint8_t block, uint16_t addr);
    void writeReg(uint8_t block, uint16_t addr, uint8_t val);

  private:
    void applyOpmdc(uint8_t opmdc); // write OPMD|opmdc, then pulse the PHY reset bit
    SPIClass *_spi;
    int _cs;
};
#endif // ARDUINO_ARCH_RP2040 && OPENKNX_ETH_W5500
