#include "W5500Phy.h"
#if defined(ARDUINO_ARCH_RP2040) && defined(KNX_IP_LAN)

// The Wiznet5500lwIP driver serialises its SPI access with these (weak) core hooks. We take the same
// lock so our raw register access never interleaves with an in-flight driver transaction.
extern void ethernet_arch_lwip_begin() __attribute__((weak));
extern void ethernet_arch_lwip_end() __attribute__((weak));
static inline void lockBegin()
{
    if (ethernet_arch_lwip_begin) ethernet_arch_lwip_begin();
}
static inline void lockEnd()
{
    if (ethernet_arch_lwip_end) ethernet_arch_lwip_end();
}

// W5500 SPI frame (matches the driver): [addr_hi][addr_lo][control][data...]
// control = block | RW  (read: 0x00, write: 0x04), OM = VDM.
static constexpr uint8_t CTRL_READ = 0x00;
static constexpr uint8_t CTRL_WRITE = 0x04;
static constexpr uint8_t BLOCK_COMMON = 0x00; // common register block (BSB=0)
static constexpr uint16_t REG_PHYCFGR = 0x002E;
static constexpr uint16_t REG_VERSIONR = 0x0039;

// PHYCFGR bit layout (W5500 datasheet)
static constexpr uint8_t PHYCFGR_RST = 1 << 7;        // 0 = reset PHY (active low), 1 = normal
static constexpr uint8_t PHYCFGR_OPMD = 1 << 6;       // 1 = configure PHY from OPMDC (software)
static constexpr uint8_t PHYCFGR_OPMDC_ALLA = 7 << 3; // all-capable auto-negotiation
static constexpr uint8_t PHYCFGR_OPMDC_100F = 3 << 3;
static constexpr uint8_t PHYCFGR_OPMDC_100H = 2 << 3;
static constexpr uint8_t PHYCFGR_OPMDC_10F = 1 << 3;
static constexpr uint8_t PHYCFGR_OPMDC_10H = 0 << 3;
static constexpr uint8_t PHYCFGR_DPX_FULL = 1 << 2; // status: 1 = full duplex
static constexpr uint8_t PHYCFGR_SPD_100 = 1 << 1;  // status: 1 = 100 Mbit
static constexpr uint8_t PHYCFGR_LNK = 1 << 0;      // status: 1 = link up

uint8_t W5500Phy::readReg(uint8_t block, uint16_t addr)
{
    lockBegin();
    _spi->beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
    digitalWrite(_cs, LOW);
    _spi->transfer((addr & 0xFF00) >> 8);
    _spi->transfer(addr & 0x00FF);
    _spi->transfer(block | CTRL_READ);
    uint8_t v = _spi->transfer(0x00);
    digitalWrite(_cs, HIGH);
    _spi->endTransaction();
    lockEnd();
    return v;
}

void W5500Phy::writeReg(uint8_t block, uint16_t addr, uint8_t val)
{
    lockBegin();
    _spi->beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
    digitalWrite(_cs, LOW);
    _spi->transfer((addr & 0xFF00) >> 8);
    _spi->transfer(addr & 0x00FF);
    _spi->transfer(block | CTRL_WRITE);
    _spi->transfer(val);
    digitalWrite(_cs, HIGH);
    _spi->endTransaction();
    lockEnd();
}

uint8_t W5500Phy::chipVersion()
{
    return readReg(BLOCK_COMMON, REG_VERSIONR);
}

W5500Phy::Status W5500Phy::read()
{
    uint8_t r = readReg(BLOCK_COMMON, REG_PHYCFGR);
    Status s;
    s.linked = (r & PHYCFGR_LNK);
    s.speed100 = (r & PHYCFGR_SPD_100);
    s.fullDuplex = (r & PHYCFGR_DPX_FULL);
    return s;
}

void W5500Phy::applyOpmdc(uint8_t opmdc)
{
    const uint8_t base = PHYCFGR_OPMD | opmdc;                // OPMD=1 + chosen mode
    writeReg(BLOCK_COMMON, REG_PHYCFGR, base & ~PHYCFGR_RST); // RST low  -> PHY reset asserted
    delayMicroseconds(500);
    writeReg(BLOCK_COMMON, REG_PHYCFGR, base | PHYCFGR_RST); // RST high -> released -> config applied
}

void W5500Phy::forceMode(bool speed100, bool full)
{
    uint8_t opmdc = speed100 ? (full ? PHYCFGR_OPMDC_100F : PHYCFGR_OPMDC_100H)
                             : (full ? PHYCFGR_OPMDC_10F : PHYCFGR_OPMDC_10H);
    applyOpmdc(opmdc);
}

void W5500Phy::setAutoNeg()
{
    applyOpmdc(PHYCFGR_OPMDC_ALLA);
}

#endif // ARDUINO_ARCH_RP2040 && KNX_IP_LAN
