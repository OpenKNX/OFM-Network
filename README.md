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

### Verwendung in Modulen
Ander Module können eigene Pages oder Handler hinzufügen.  
Diese werden dann auch in der Übersichtsseite angezeigt.  

#### Pages (empfohlen)
Pages bieten die Möglichkeit ohne einen Hander zu verbrauchen, eigene Webseiten zur Verfügung zu stellen. Die URL dazu baut immer auf der WEBSERVER_BASE_URI auf. Eine Angabe von /dali wäre also zum Beispiel unter /openknx/dali erreichbar.  
Es werden auch alle anderen Anfragen weitergeleitet, die mit /openknx/dali beginnen (/openknx/dali.css oder /openknx/dali/monitor/lib.js)
```C++
WebserverPage _page = {
    .uri = "/dali", // URL (Relativ von /<WEBSERVER_BASE_URI>), Gleichzeitig auch Link auf der Übersichtsseite
    .name = "Dali Monitor", // Anzeigename auf der Übersichtsseite
    .isVisible = true, // Soll der Handler auf der Übersichtseite angezeigt werden
    .handler = page_handler, // Funktion welche die Anfragen bearbeitet
    .arg = (void*)this // (Optional) Argument um die Modulinstanz zu übergeben
};
openknxNetwork.webserver.addPage(_page);

[...]

int MeinModul::page_handler(const char *uri, WebRequest *req, void *arg)
{
  // uri wird ohne den prefix von WEBSERVER_BASE_URI angegeben
  // die volle uri ist unter req->uri abrufbar
  if(strcmp(uri, "/page") == 0)
  {
      std::string reponse = "Hier steht die Response drin";
      // NULL Terminierte char arrays können auch ohne längenangabe hinzugefügt werden
      req->setResponse("text/html", response.c_str());
      req->addResponseHeader("Content-Encoding", "gzip");
      return 0;
  }
  else if(strcmp(uri, "/dali.css") == 0)
  {
      req->setResponse("text/html", file_index_html, file_index_html_len);
      req->addResponseHeader("Content-Encoding", "gzip");
      return 0;
  }
  else if(strcmp(uri, "/dali.js") == 0)
  {
      [...]
  }

  req->setStatusCode(404);
  return -1;
}
```

#### Links
Mit Links kann man auf einen Wiki Eintrag oder auch das Repo verweisen.  
```C++
openknxNetwork.webserver.addLink("Wiki", "https://github.com/OpenKNX/OpenKNX.wiki");
```

#### Handler (ESP32 only)
Es kann unter Umständen hilfreich sein, einen eigenen Handler zu definieren.  
Dieser kann dann zum Beispiel für Websocket verbindungen verwendet werden.
```C++
httpd_uri_t ws = {
    .uri = "/", // URL der Anwendung (absoluter Pfad)
    .method = HTTP_GET, // Anzeigename auf der Übersichtsseite
    .handler = ws_handler, // Funktion welche die Anfragen bearbeitet
    .user_ctx = this, // Argument um die Modulinstanz zu übergeben
    .is_websocket = true // Angabe ob es sich um einen Websockethandler handelt
};
WebserverHandler _ws = {
    .httpd = ws, // Angabe des httpd_uri_t
    .uri = "/", // Angabe der URL für den Link auf der Übersichtsseite
    .name = "Dali Websocket", // Anzeigename auf der Übersichtsseite
    .isVisible = false // Soll der Handler auf der Übersichtseite angezeigt werden
};
openknxNetwork.addWebserverHandler(_ws);

[...]

static esp_err_t ws_handler(httpd_req_t *req)
{
    IotGateway *gw = (IotGateway *)req->user_ctx;

    if(req->method == HTTP_GET)
    {
      // Got new Websocket Connection
      return ESP_OK
    }

  // Handle websocket packages
  return ESP_OK;
}
```