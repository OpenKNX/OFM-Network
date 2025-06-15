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

Mit dem Netzwerkmodul wird eine OTA (Over the air) Update Funktion der Firmware ermöglicht.
Das OTA muss jedoch zuerst am Gerät erlaubt werden.
Dies kann durch drücken des PROG Tasters oder über die Konsole durch den Befehl `ota` erfolgen.

### ESP32 OTA

In platformio.custom.ini muss eine Section als OTA Target angelegt werden.
In dieser muss das upload_protocol OTA und die IP-Adresse oder der Hostname des Gerätes festgelegt werden.

```ini
upload_protocol = espota
upload_port = XXX.XXX.XXX.XXX # IP Address or Hostname
```

Hinweis: Der Hostname des Gerätes kann in der ETS im Abschnitt Netzwerk unter mDNS festgelegt werden.

### RP2040 OTA

Aktuelle ist der OTA Upload beim RP2040 noch nicht getestet.