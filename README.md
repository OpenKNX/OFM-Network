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
```
