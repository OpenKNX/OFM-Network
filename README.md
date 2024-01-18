# OFM-Network

This module provides the network functionality for the OpenKNX stack.

## Platforms

| Plattform | Ethernet | Stack      | Define         | Note                             |
| --------- | -------- | ---------- | -------------- | -------------------------------- |
| Header    | W5500    | Integrated | KNX_IP_W5500   | high latency + problems with knx |
| Header    | WIFI     | Integrated | KNX_IP_WIFI    | high latency + problems with knx |
| Header    | W5500    | Integrated | KNX_IP_GENERIC | low latency, limmited socket     |

## Platformio.ini

```ini
# Required when building firmware with Ethernet_Generic (typical for rp2040 with LAN).
lib_deps =
  ${Network_Generic.lib_deps}

# Required when network settings must also be available across an upgrade. This is essential for an IP-only device.
# Alternatively, the defines NETWORK_FLASH_OFFSET and NETWORK_FLASH_SIZE can be assigned manually, for instance, if the area is already allocated.
build_flags =
  ${Network_Flash.build_flags}

# when you use ESP with partition yo need to define NETWORK_FLASH
build_flags =
  -D NETWORK_FLASH

```
