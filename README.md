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


| Define                   | Default | Description                          | Note                                         |
|--------------------------|---------|--------------------------------------|----------------------------------------------|
| OPENKNX_LED_IP           |         | LED used for IP state                | Set to info2Led to use IP LED feature        |
| OPENKNX_PING             |         | Enable ping functionality            | Requires inclusion of Header in setup        |
| OPENKNX_PING_TIMEOUT     | 1000    | Default ping timeout in milliseconds | Can be overridden per call                   |
| OPENKNX_PING_PARALLEL    | 5       | Max concurrent pings                 | Additional requests are queued automatically |
| OPENKNX_WEBCLIENT        |         | Enable HTTP(S) client                | See below for API                            |
| OPENKNX_WEBCLIENT_SLOTS  | 2       | Parallel request slots (RP2040) / queue depth (ESP32) | |
| OPENKNX_WEBCLIENT_TIMEOUT| 10000   | Request timeout in milliseconds      |                                              |
| OPENKNX_WEBCLIENT_TASK_STACK | 6144| ESP32 FreeRTOS task stack size (bytes) | SSL handshake requires ~4 KB             |
| OPENKNX_WEBCLIENT_MAX_BODY | 4096  | Max request body size (bytes)        |                                              |

## IP LED

If OPENKNX_LED_IP is defined, the LED is representing the state of the network. 
Possible values for OPENKNX_LED_IP: info1Led, info2Led, info3Led. Recommended value: info2Led

| State                         | LED           | RGB-LED            | Note                                   |
|-------------------------------|---------------|--------------------|----------------------------------------|
| WLAN configuration missing    | Fast flashing | Red fast flashing  | Only for HW with WLA                   |
| No connection to the network  | Off           | Red                |                                        |
| No IP adress                  | Slow flashing | Yellow             |                                        |
| IP adress assigned            | On            | Green              |                                        |

## Ping

The module provides a non-blocking ping API with an internal queue, parallel slot management, and optional retry logic.

Full documentation: [`README.Ping.md`](README.Ping.md)

## Webclient

`#define OPENKNX_WEBCLIENT` enables a non-blocking HTTP/HTTPS client accessible via `openknxNetwork.webclient`.

Full documentation: [`README.Webclient.md`](README.Webclient.md)

## OTA

The network module provides OTA (Over the Air) firmware update functionality.
OTA must first be enabled on the device.
This can be done by pressing the PROG button or via the console using the `ota` command.

A section must be added to `platformio.custom.ini` as an OTA target,
specifying the upload protocol and the IP address or hostname of the device:

```ini
upload_protocol = espota
upload_port = XXX.XXX.XXX.XXX # IP Address or Hostname
```

Note: The device hostname can be set in ETS under the Network → mDNS section.

## Webserver

The built-in HTTP/WebSocket server is documented in [`README.Webserver.md`](README.Webserver.md).

## MQTT

The built-in MQTT 3.1.1 client and broker is documented in [`README.MQTT.md`](README.MQTT.md).
