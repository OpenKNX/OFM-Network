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

## Webserver (ESP32 only)

### Defines
|Define|Description|
|---|---|
|WEBSERVER_BASE_URI|Setzt die Hauptseite. Standard: /openknx
|WEBSERVER_BASE_URI_USED|Wenn gesetzt, wird kein Handler für "/" registriert. (Umleitung auf "/WEBSERVER_BASE_URI")|


### Webdateien
Um automatisch html/js/css zu komprimieren und als Headerdatei  
zur Verfügung zu stellen, muss folgendes in der .custom.ini eingetragen werden:
```ini
extra_scripts =
  ${env.extra_scripts}
  lib/OFM-Network/scripts/pio/minimize.py
```
Die Dateien müssen im Ordner /www sein und werden dann in den include Ordner als  
char array gespeichert.

### Automatische Ersetzung
Beim kompilieren kann in den Webdateien automatisch eine Ersetzung von verschiedenen Variablen erfolgen:
|Platzhalter|Ersetzt durch|Standardwert|
|---|---|---|
|#webserver#base-uri#|WEBSERVER_BASE_URI|/openknx