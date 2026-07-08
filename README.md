# OFM-Network

This module provides the network functionality for the OpenKNX stack.

## Platforms

* ARDUINO_ARCH_ESP32
* ARDUINO_ARCH_RP2040

## Defines

| Arch   | Type | Stack      | Define      | Note                                                          |
| ------ | ---- | ---------- | ----------- | ------------------------------------------------------------- |
| ESP32  | WiFi | Integrated | KNX_IP_WIFI |                                                               |
| ESP32  | LAN  | Integrated | KNX_IP_LAN  | You need to set right board and presumably CONFIG_ETH_ENABLED |
| RP2040 | WiFi | Integrated | KNX_IP_WIFI |                                                               |
| RP2040 | LAN  | Integrated | KNX_IP_LAN  |                                                               |


| Define         | Default  | Description           | Note                                    |
|----------------|----------|-----------------------|-----------------------------------------|
| OPENKNX_LED_IP |          | used LED for IP state | set to info2Led to use IP LED feature   |
| OPENKNX_ETH_FORCE_SPEED      | (off) | force fixed ETH link speed (10/100)  | KNX_IP_LAN (W5500), ESP32 & RP2040, autoneg off; opt-in — see "ETH link speed" |
| OPENKNX_ETH_FORCE_FULLDUPLEX | half  | full-duplex when forcing speed       | optional; only with OPENKNX_ETH_FORCE_SPEED                            |
| OPENKNX_ETH_AUTO_FALLBACK    | (off) | auto link-speed fallback ladder      | KNX_IP_LAN (W5500), ESP32 & RP2040; auto mode walks speeds until a real IP — see "ETH link speed" |

## ETH link speed (opt-in, W5500 LAN — ESP32 & RP2040)

By default the ETH PHY uses auto-negotiation. On both platforms the link speed can be forced
(auto-negotiation off) — a workaround for cheap/unmanaged switches that don't hold 100 Mbit
auto-negotiation with the W5500 (link flapping). Not needed on a proper switch. Half-duplex is
the safe default (an autoneg switch picks HALF via parallel detection, so forcing FULL can cause
a duplex mismatch). 10 Mbit is plenty for KNX-IP.

A fixed speed can be forced at build time:

```ini
build_flags =
  -D OPENKNX_ETH_FORCE_SPEED=10      ; 10 or 100 Mbit; enables the fixed mode (autoneg off)
  -D OPENKNX_ETH_FORCE_FULLDUPLEX    ; optional; without it = half-duplex
```

…or at runtime via the console. It is **applied live immediately (no reboot)** and persisted so it
survives the next boot (ESP32: NVS, RP2040: module flash). A runtime override takes precedence over
the build flag.

```
net eth                 # show current mode
net eth 10              # 10 Mbit half-duplex (autoneg off)
net eth 100 full        # 100 Mbit full-duplex
net eth auto            # back to auto-negotiation
```

### Auto-fallback ladder (`OPENKNX_ETH_AUTO_FALLBACK`)

Instead of forcing one fixed speed, this opt-in build flag lets the driver **find** a working speed
on its own. In auto mode it walks auto-neg → 100 Mbit full → 100 Mbit half → 10 Mbit full → 10 Mbit
half, judged by **real connectivity** (a link that actually gets an IP, not just carrier): it escalates
on link flapping or link-up-but-no-IP and locks onto the first stage that stays stable. It is *not*
persisted — the ladder re-searches from auto-negotiation on every boot (a flaky switch is inconsistent,
so a remembered speed could be wrong) — and it is disabled as soon as a fixed `net eth 10|100` mode is
set; `net eth auto` re-enables it.

## IP LED

If OPENKNX_LED_IP is defined, the LED is representing the state of the network. 
Possible values for OPENKNX_LED_IP: info1Led, info2Led, info3Led. Recommended value: info2Led

| State                         | LED           | RGB-LED            | Note                                   |
|-------------------------------|---------------|--------------------|----------------------------------------|
| WLAN configuration missing    | Fast flashing | Red fast flashing  | Only for HW with WLA                   |
| No connection to the network  | Off           | Red                |                                        |
| No IP adress                  | Slow flashing | Yellow             |                                        |
| IP adress assigned            | On            | Green              |                                        |

## OTA

The network module enables OTA (over-the-air) firmware updates. OTA must first be enabled on the
device — either by pressing the PROG button or via the console command `ota`.

### ESP32 OTA

Add an OTA target section to `platformio.custom.ini` with `upload_protocol = espota` and the
device's IP address or hostname:

```ini
upload_protocol = espota
upload_port = XXX.XXX.XXX.XXX # IP address or hostname
```

Note: the device hostname can be set in the ETS under Network → mDNS.

### RP2040 OTA

OTA upload on the RP2040 is not tested yet.