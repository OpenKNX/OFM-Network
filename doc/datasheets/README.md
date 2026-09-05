# Network PHY datasheets

Reference copies of the vendor documents this module's drivers are written against. Each stays the
copyright of its manufacturer; the vendor source is listed so a newer revision can be pulled at any time.

| File | Document | Used by |
|---|---|---|
| `W5500-v1.1.0.pdf` | WIZnet W5500, Version 1.1.0, 67 pp. | RP2040 + REG1/REG2 ETH app board (`src/driver/W5500Phy.*`, the bring-up/self-heal in `src/OpenKNX/Network/Module.cpp`) |
| `LAN8720A.pdf` | Microchip LAN8720A/LAN8720Ai, DS00002165B (2016), 78 pp. | ESP32 native ETH on REG1-LAN-TP-Base (`ETH_PHY_TYPE ETH_PHY_LAN8720`) |

Source: [W5500](https://docs.wiznet.io/img/products/w5500/W5500_ds_v110e.pdf) ·
[LAN8720A](https://ww1.microchip.com/downloads/en/DeviceDoc/00002165B.pdf)

`pdftotext` is not installed here; extract text with PyMuPDF:

```bash
python3 -c "import fitz; d=fitz.open('doc/datasheets/W5500-v1.1.0.pdf'); print(d[41].get_text())"  # d[N] = page N+1
```

## W5500 facts this driver relies on

- **p.42 PHYCFGR** `[R/W] [0x002E] [0b10111XXX]` — OPMD/OPMDC and the PHY reset bit; `W5500Phy::applyOpmdc()`
  writes OPMD=1 + mode, then pulses bit 7. Link/speed/duplex status are read from the same register.
- **p.43 VERSIONR** `[R] [0x0039] [0x04]` — always 0x04 on a W5500. This is the presence probe in
  `chipVersion()` / `checkEthHealth()`; anything else means the chip is not answering on SPI.
- **§5.4 Power Dissipation p.60** (3.3 V, 25 °C, typ): 100M Link 128 mA, **10M Link 75 mA**, Un-Link
  (auto-neg) 65 mA, 100M Transmitting 132 mA, **10M Transmitting 79 mA**, Power Down 13 mA.
  Forcing 10 Mbit saves ~53 mA / ~175 mW in the chip.
- **§5.5.1 Reset Timing p.60** — `TRC` (reset cycle time) **min 500 us**, `TPL` (RSTn to internal PLL
  lock) **max 1 ms**. `hwResetPhy()` uses 2 ms low + 60 ms settle, so the pulse is well inside spec.
- **§5.5.4 SPI Timing p.61** — guaranteed SCLK **33.3 MHz** (80 MHz is design-theoretical only).
  `OPENKNX_NET_SPI_SPEED` defaults to 28 MHz.
- **§5.5.6** — the W5500 has **no auto-MDIX**.
