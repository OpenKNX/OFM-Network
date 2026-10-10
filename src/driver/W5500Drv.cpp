/*
    Copyright (c) 2013, WIZnet Co., Ltd.
    Copyright (c) 2016, Nicholas Humfrey
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions
    are met:
    1. Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    2. Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    3. Neither the name of the copyright holder nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
    ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
    FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
    COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
    INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
    (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
    SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
    HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
    STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
    ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
    OF THE POSSIBILITY OF SUCH DAMAGE.

*/

// original sources: https://github.com/njh/W5500MacRaw
// Copy of arduino-pico 5.6.0 libraries/lwIP_w5500/src/utility/w5500.cpp, class renamed to W5500Drv.
// OpenKNX changes: Copyright (c) 2026, Erkan Çolak (erkan@colak.de), under the license above; listed in W5500Drv.md.

#include "W5500Drv.h"
#if defined(ARDUINO_ARCH_RP2040) && defined(OPENKNX_ETH_W5500)
#include <SPI.h>
#include <LwipEthernet.h>

// One SPI transfer for the header and one for the data.
uint8_t W5500Drv::wizchip_read(uint8_t block, uint16_t address) {
    uint8_t ret;
    wizchip_read_buf(block, address, &ret, 1);
    return ret;
}

uint16_t W5500Drv::wizchip_read_word(uint8_t block, uint16_t address) {
    uint8_t b[2];
    wizchip_read_buf(block, address, b, 2); // one transaction: the address auto-increments in variable length mode
    return ((uint16_t)b[0] << 8) | b[1];
}

void W5500Drv::wizchip_read_buf(uint8_t block, uint16_t address, uint8_t* pBuf, uint16_t len) {
    const uint8_t head[3] = {(uint8_t)(address >> 8), (uint8_t)address, (uint8_t)(block | AccessModeRead)};
    wizchip_cs_select();
    _spi.transfer(head, nullptr, sizeof(head));
    _spi.transfer(nullptr, pBuf, len);
    wizchip_cs_deselect();
}

void W5500Drv::wizchip_write(uint8_t block, uint16_t address, uint8_t wb) {
    wizchip_write_buf(block, address, &wb, 1);
}

void W5500Drv::wizchip_write_word(uint8_t block, uint16_t address, uint16_t word) {
    const uint8_t b[2] = {(uint8_t)(word >> 8), (uint8_t)word};
    wizchip_write_buf(block, address, b, 2);
}

void W5500Drv::wizchip_write_buf(uint8_t block, uint16_t address, const uint8_t* pBuf,
                                   uint16_t len) {
    const uint8_t head[3] = {(uint8_t)(address >> 8), (uint8_t)address, (uint8_t)(block | AccessModeWrite)};
    wizchip_cs_select();
    _spi.transfer(head, nullptr, sizeof(head));
    _spi.transfer(pBuf, nullptr, len);
    wizchip_cs_deselect();
}

bool W5500Drv::setSn_CR(uint8_t cr) {
    // Write the command to the Command Register
    wizchip_write(BlockSelectSReg, Sn_CR, cr);

    // Now wait for the chip to accept it (Sn_CR reads 0); the clock is read only when the first read is not 0.
    if (!wizchip_read(BlockSelectSReg, Sn_CR))
        return true;
    const uint32_t start = micros();
    while (wizchip_read(BlockSelectSReg, Sn_CR)) {
        if (micros() - start >= CmdAcceptTimeoutUs) {
            _cmdTimeouts++;
            return false;
        }
    }
    return true;
}

uint16_t W5500Drv::getSn_TX_FSR() {
    uint16_t val = 0, val1 = 0;
    do {
        val1 = wizchip_read_word(BlockSelectSReg, Sn_TX_FSR);
        if (val1 != 0) {
            val = wizchip_read_word(BlockSelectSReg, Sn_TX_FSR);
        }
    } while (val != val1);
    return val;
}

uint16_t W5500Drv::getSn_RX_RSR() {
    uint16_t val = 0, val1 = 0;
    do {
        val1 = wizchip_read_word(BlockSelectSReg, Sn_RX_RSR);
        if (val1 != 0) {
            val = wizchip_read_word(BlockSelectSReg, Sn_RX_RSR);
        }
    } while (val != val1);
    return val;
}

void W5500Drv::wizchip_recv_data(uint8_t* wizdata, uint16_t len) {
    uint16_t ptr;

    if (len == 0) {
        return;
    }
    ptr = getSn_RX_RD();
    wizchip_read_buf(BlockSelectRxBuf, ptr, wizdata, len);
    ptr += len;

    setSn_RX_RD(ptr);
}

void W5500Drv::wizchip_recv_ignore(uint16_t len) {
    uint16_t ptr;

    ptr = getSn_RX_RD();
    ptr += len;
    setSn_RX_RD(ptr);
}

void W5500Drv::wizchip_sw_reset() {
    setMR(MR_RST);
    getMR();  // for delay

    setSHAR(_mac_address);
}

int8_t W5500Drv::wizphy_getphylink() {
    int8_t tmp;
    if (getPHYCFGR() & PHYCFGR_LNK_ON) {
        tmp = PHY_LINK_ON;
    } else {
        tmp = PHY_LINK_OFF;
    }
    return tmp;
}

int8_t W5500Drv::wizphy_getphypmode() {
    int8_t tmp = 0;
    if (getPHYCFGR() & PHYCFGR_OPMDC_PDOWN) {
        tmp = PHY_POWER_DOWN;
    } else {
        tmp = PHY_POWER_NORM;
    }
    return tmp;
}

void W5500Drv::wizphy_reset() {
    uint8_t tmp = getPHYCFGR();
    tmp &= PHYCFGR_RST;
    setPHYCFGR(tmp);
    tmp = getPHYCFGR();
    tmp |= ~PHYCFGR_RST;
    setPHYCFGR(tmp);
}

int8_t W5500Drv::wizphy_setphypmode(uint8_t pmode) {
    uint8_t tmp = 0;
    tmp         = getPHYCFGR();
    if ((tmp & PHYCFGR_OPMD) == 0) {
        return -1;
    }
    tmp &= ~PHYCFGR_OPMDC_ALLA;
    if (pmode == PHY_POWER_DOWN) {
        tmp |= PHYCFGR_OPMDC_PDOWN;
    } else {
        tmp |= PHYCFGR_OPMDC_ALLA;
    }
    setPHYCFGR(tmp);
    wizphy_reset();
    tmp = getPHYCFGR();
    if (pmode == PHY_POWER_DOWN) {
        if (tmp & PHYCFGR_OPMDC_PDOWN) {
            return 0;
        }
    } else {
        if (tmp & PHYCFGR_OPMDC_ALLA) {
            return 0;
        }
    }
    return -1;
}

W5500Drv::W5500Drv(int8_t cs, SPIClass& spi, int8_t intr) : _spi(spi), _cs(cs), _intr(intr) {
}

bool W5500Drv::begin(const uint8_t* mac_address, netif *net) {
    _netif = net;
    memcpy(_mac_address, mac_address, 6);

    pinMode(_cs, OUTPUT);
    wizchip_cs_deselect();

    wizchip_sw_reset();
    _sendPending = false;

    // Use the full 16Kb of RAM for Socket 0
    setSn_RXBUF_SIZE(RxBufBytes / 1024);
    setSn_TXBUF_SIZE(16);

    // Set our local MAC address
    setSHAR(_mac_address);

    // Open Socket 0 in MACRaw mode
    setSn_MR(Sn_MR_MACRAW);
    if (!setSn_CR(Sn_CR_OPEN) || getSn_SR() != SOCK_MACRAW) {
        // Failed to put socket 0 into MACRaw mode
        return false;
    }

    if (_intr >= 0) {
        setSn_IR(0xff); // Clear everything
        setSIMR(1);
    }

    // Success
    return true;
}

void W5500Drv::end() {
    _sendPending = false;
    setSn_CR(Sn_CR_CLOSE); // not accepted: the SOCK_CLOSED wait below runs into its bound

    // clear all interrupt of the socket
    setSn_IR(0xFF);

    // Wait for socket to change to closed; after SendDoneTimeoutUs go on as closed (counted).
    const uint32_t start = micros();
    while (getSn_SR() != SOCK_CLOSED) {
        if (micros() - start >= SendDoneTimeoutUs) {
            _closeTimeouts++;
            break;
        }
    }
}

uint16_t W5500Drv::readFrame(uint8_t* buffer, uint16_t bufsize) {
    uint16_t data_len = readFrameSize();

    if (data_len == 0) {
        return 0;
    }

    if (data_len > bufsize) {
        // Packet is bigger than buffer - drop the packet
        discardFrame(data_len);
        return 0;
    }

    return readFrameData(buffer, data_len);
}

uint16_t W5500Drv::readFrameSize() {
    setSn_IR(Sn_IR_RECV);

    uint16_t len = getSn_RX_RSR();

    if (len == 0) {
        return 0;
    }

    // Each frame starts with a 2-octet length that includes itself; it is checked before RX_RD moves.
    uint8_t        head[2];
    const uint16_t ptr = getSn_RX_RD();
    wizchip_read_buf(BlockSelectRxBuf, ptr, head, 2);
    const uint16_t size = ((uint16_t)head[0] << 8) | head[1];
    if (size < 2 || size > RxFrameMaxBytes) {
        // No frame boundary left to follow: drop what was received up to RX_WR, where the chip starts the next frame.
        setSn_RX_RD(ptr + len);
        setSn_CR(Sn_CR_RECV);
        return 0;
    }
    if (size > len)
        return 0; // the rest of the frame is not in the buffer yet

    setSn_RX_RD(ptr + 2);
    setSn_CR(Sn_CR_RECV); // RX_RD has moved, so the frame is returned either way to keep the read position in sync

    return size - 2;
}

void W5500Drv::discardFrame(uint16_t framesize) {
    wizchip_recv_ignore(framesize);
    setSn_CR(Sn_CR_RECV);
}

uint16_t W5500Drv::readFrameData(uint8_t* buffer, uint16_t framesize) {
    wizchip_recv_data(buffer, framesize);
    if (!setSn_CR(Sn_CR_RECV))
        return 0; // no frame: LwipIntfDev drops the buffer

    // let lwIP deal with mac address filtering
    return framesize;
}

uint16_t W5500Drv::sendFrame(struct pbuf *p) {
    ethernet_arch_lwip_gpio_mask(); // So we don't fire an IRQ and interrupt the send w/a receive!

    // Without INTn the previous frame's SENDOK is awaited here; with none pending one socket-state read.
    if (_sendPending ? !waitSendDone() : getSn_SR() != SOCK_MACRAW) {
        ethernet_arch_lwip_gpio_unmask();
        return -1;
    }

    // Wait for space in the transmit buffer; the socket state is read only when it is short. The buffer holds at most
    // the one frame sent last, so space that stays short for CmdAcceptTimeoutUs fails the send and reopens the socket.
    if (getSn_TX_FSR() < p->len) {
        const uint32_t start = micros();
        while (getSn_TX_FSR() < p->len) {
            if (getSn_SR() == SOCK_CLOSED) {
                ethernet_arch_lwip_gpio_unmask();
                return -1;
            }
            if (micros() - start >= CmdAcceptTimeoutUs) { // the previous frame is done here, so space short means a broken chip
                reopenSocket();
                ethernet_arch_lwip_gpio_unmask();
                return -1;
            }
        }
    }

    const uint16_t txWr = getSn_TX_WR();
    wizchip_write_buf(BlockSelectTxBuf, txWr, (const uint8_t*)p->payload, p->len);
    // Checked with the frame already in the chip, the last step before SEND: a ROUTING_BUSY that arrived meanwhile
    // stops this ROUTING_INDICATION. TX_WR stays where it was, so the chip discards the frame; the caller keeps it queued.
    if (isRoutingIndication(p) && routingBusyInRx()) {
        ethernet_arch_lwip_gpio_unmask();
        return 0;
    }
    setSn_TX_WR(txWr + p->len);
    if (!setSn_CR(Sn_CR_SEND)) {
        ethernet_arch_lwip_gpio_unmask();
        return -1;
    }
    _sendAtUs = micros();
    _sendPending = true;
    // With INTn in use a pending SENDOK holds the pin low after the unmask (Sn_IR != 0), so it is awaited here.
    if (_intr >= 0)
        waitSendDone();

    ethernet_arch_lwip_gpio_unmask();
    return p->len;
}

// SENDOK or TIMEOUT of the frame sent last, both cleared (MACRAW has no peer to time out on). Without either flag
// SendDoneTimeoutUs after SEND the socket is reopened; false when the socket is not MACRAW.
bool W5500Drv::waitSendDone() {
    _sendPending = false;
    for (;;) {
        if (takeSendDone())
            return true;
        if (getSn_SR() != SOCK_MACRAW)
            return false;
        if (micros() - _sendAtUs >= SendDoneTimeoutUs)
            break;
    }
    if (takeSendDone()) // last read after the deadline
        return true;
    reopenSocket();
    return getSn_SR() == SOCK_MACRAW;
}

bool W5500Drv::takeSendDone() {
    const uint8_t ir = getSn_IR() & (Sn_IR_SENDOK | Sn_IR_TIMEOUT);
    if (ir)
        setSn_IR(ir);
    return ir != 0;
}

void W5500Drv::reopenSocket() {
    _sendReopens++;
    end();
    setSn_MR(Sn_MR_MACRAW);
    setSn_CR(Sn_CR_OPEN);
}

// KNXnet/IP over IPv4/UDP port 3671 with the given service type; frame = Ethernet II from the destination MAC.
static bool isKnxIpService(const uint8_t* f, uint16_t len, uint8_t serviceLow) {
    if (len < 14 + 20 + 8 + 4 || f[12] != 0x08 || f[13] != 0x00 || f[23] != 17)
        return false;
    const uint16_t udp = 14 + (uint16_t)(f[14] & 0x0F) * 4;
    return len >= udp + 8 + 4 && f[udp + 2] == 0x0E && f[udp + 3] == 0x57 && f[udp + 8 + 2] == 0x05
        && f[udp + 8 + 3] == serviceLow;
}

bool W5500Drv::isRoutingIndication(const struct pbuf* p) {
    return isKnxIpService((const uint8_t*)p->payload, p->len, 0x30);
}

// MACRAW receive buffer: each frame is a 2-octet length (itself included) and the Ethernet frame. Only the headers up
// to the KNXnet/IP service type are read; at most 16 frames, the rest is left to the next check.
bool W5500Drv::routingBusyInRx() {
    uint16_t pending = getSn_RX_RSR();
    uint16_t ptr = getSn_RX_RD();
    for (uint8_t n = 0; n < 16 && pending >= 2; n++) {
        uint8_t h[2 + 64];
        wizchip_read_buf(BlockSelectRxBuf, ptr, h, 2);
        const uint16_t size = ((uint16_t)h[0] << 8) | h[1];
        if (size < 2 || size > pending)
            return false;
        const uint16_t peek = (size - 2) < 64 ? (size - 2) : 64;
        wizchip_read_buf(BlockSelectRxBuf, ptr + 2, h + 2, peek);
        if (isKnxIpService(h + 2, peek, 0x32))
            return true;
        ptr += size;
        pending -= size;
    }
    return false;
}

#endif
