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

