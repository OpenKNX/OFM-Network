# **Applikationsbeschreibung Netzwerk**

<!-- DOC HelpContext="Dokumentation" -->

<!-- DOCCONTENT
Eine vollständige Applikationsbeschreibung ist unter folgendem Link verfügbar: https://github.com/openknx/OFM-Network/blob/v1/doc/Applikationsbeschreibung-Netzwerk.md
DOCCONTENT -->

Über die Netzwerkeinstellungen kann nicht nur die IP-Adresse des Geräts angepasst werden, sondern es können auch verschiedene Dienste ein- oder ausgeschaltet werden. Dazu gehören z. B. der NTP-Client zum Abrufen der aktuellen Zeit, mDNS für das automatisierte Auffinden von OpenKNX-Geräten sowie die Möglichkeit, die Geräte-Firmware per Netzwerk zu aktualisieren.

<!-- DOC Skip="12" -->
**Hinweis**: Nicht alle der hier beschriebenen Funktionen sind in jeder OpenKNX-Applikation verfügbar.

## Inhalte

<!-- no toc -->
* [Allgemein](#allgemein)
  * [IP-Adresse](#ip-adresse)
  * [Services](#services)
  * [Erweitere Einstellungen](#erweitere-einstellungen)
* [Webserver](#webserver-1)
* [WiFi-Assistent](#wifi-assistent)

<!-- DOC -->
## **Allgemein**

In diesem Abschnitt werden die Basiseinstellungen und verfügbaren Dienste festgelegt.

<!-- DOC -->
### **IP-Adresse**

In diesem Eingabeformular kannst du entscheiden, ob die IP-Adresse dynamisch durch einen DHCP-Server zugewiesen oder manuell festgelegt werden soll. Bei manueller Konfiguration sind neben der IP-Adresse auch die Netzmaske, ein Standardgateway (Router) und ein Nameserver (DNS) erforderlich.

**Hinweis**: Es wird empfohlen, die DHCP-Einstellungen beizubehalten und stattdessen eine feste IP-Adresse direkt im Router zuzuweisen. Dies liegt daran, dass die Netzwerkeinstellungen nur im programmierten Zustand gelten. Nach einem Update kann das Gerät beispielsweise wieder in den DHCP-Modus wechseln, bis es erneut programmiert wird.

### **Services**

Hier können die verschiendenen Dienste ein und ausgeschaltet werden.

<!-- DOC HelpContext="MDNS" -->
#### **mDNS**

Der mDNS Service ermöglicht das Auflösen von "Hostname.local" und kann auch später zum Auffinden von OpenKNX-Geräten im eigenen Netzwerk genutzt werden.

<!-- DOC HelpContext="NTP" -->
#### **NTP-Client**

Durch das Aktivieren des NTP-Clients kann das Gerät die aktuelle Zeit zyklisch von einem Zeitserver abrufen, anstatt sie vom Bus zu beziehen. Zudem kann das Gerät auf Wunsch die aktuelle Zeit auch auf den Bus senden.
Die bisherigen Einstellungen bzw. Kommunikationsobjekte zum Abrufen der Zeit vom Bus entfallen. Stattdessen stehen drei neue Kommunikationsobjekte zur Verfügung, mit denen Zeit, Datum und beides kombiniert auf dem Bus bereitgestellt werden können.

Außerdem kann der Zeitserver (NTP-Server) angepasst werden, von dem die aktuelle Zeit bezogen wird. In der Regel ist eine Änderung nicht erforderlich, da der voreingestellte Server (pool.ntp.org) zuverlässig arbeitet und weit verbreitet ist. Dieser Server fungiert als Alias für eine Vielzahl von Zeitservern.

<!-- DOC HelpContext="HTTP" -->
#### **Webserver**

Hier kann später ein Webserver aktiviert werden, der dann über den Browser aufgerufen werden kann. Diese Funktion ist derzeit noch nicht integriert und dient aktuell nur als Platzhalter.

<!-- DOC HelpContext="OTA" -->
#### **OTA-Update**

Ermöglicht eine direkte Firmwareaktualisierung, ohne den Einsatz von KNX oder einem USB-Anschluss.

* **Im Prog-Modus:** Für ein Update muss das Gerät zuvor in den Programmiermodus versetzt werden (z. B. durch Drücken der PROG-Taste).
* **Immer aktiv:** Aktiviert den dauerhaften Update-Modus für das Gerät. Wir raten von diesem Modus ab, da es schnell zu Verwechslungen zwischen Geräten kommen kann.
* **Ausgeschaltet** Deaktiviert die Möglichkeit, Updates über das Netzwerk durchzuführen.

<!-- DOC -->
### **Erweitere Einstellungen**

In diesem Abschnitt werden Einstellungen vorgenommen die vorwiegen von Netzwerkexperten benötigt werden.

<!-- DOC -->
#### **Hostname**

Der Hostname wird automatisch aus der Seriennummer generiert (OpenKNX-XXXXXXXX) und erfordert in der Regel keine Anpassung. Sollte jedoch eine individuelle Anpassung gewünscht sein, darf die Länge von 24 Zeichen nicht überschritten werden. Darüber hinaus sind nur Buchstaben, Zahlen und Bindestriche erlaubt. Der Hostname muss zudem mit einem Buchstaben beginnen und darf nicht mit einem Bindestrich enden.

<!-- DOC -->
#### **LAN-Modus**
(Diese Option wir bei WLAN Geräten ignoriert!)

Wähle den gewünschten Modus für die LAN-Schnittstelle aus. Die Auswahl des 10 MBit/s Modus kann genutzt werden, um den Stromverbrauch zu reduzieren.

**Hinweis**: In neueren Switches mit Geschwindigkeiten ab 2,5 GBit/s ist der 10 MBit/s Modus in der Regel nicht mehr vorgesehen. Es besteht daher die Möglichkeit, dass in solchen Fällen keine Verbindung hergestellt werden kann. Dennoch lohnt es sich, dies auszuprobieren, da einige Geräte diesen Modus unterstützen.

<!-- DOC -->
# **Webserver**

Hier kann später der Webserver konfiguriert werden, der dann über den Browser aufgerufen werden kann. Diese Funktion ist derzeit noch nicht integriert und dient aktuell nur als Platzhalter.

<!-- DOC -->
# **WiFi-Assistent**

Dieser WiFi-Assistent ermöglicht das Übertragen von WiFi-Zugangsdaten auf das Gerät. Voraussetzung ist, dass die verwendete Hardware einen WiFi-Adapter verwendet. Geräte, die per IP-Netzwerk angebunden werden, müssen bereits über eine WiFi-Verbindung und somit über gültige Zugangsdaten verfügen. Der Assistent kann daher nur die bestehenden Zugangsdaten ändern. TP-Geräte können hingegen immer per Bus angepasst werden.

IP-Geräte ohne WiFi-Zugangsdaten müssen initial auf anderem Wege eingerichtet werden. Dies hängt sowohl von der Gerätesoftware als auch der verwendeten Hardware ab. Die Einrichtung per Terminal (USB) sollte immer funktionieren. Dafür muss auf der Konsole nur `wifi SSID PSK` eingegeben werden.

Bei Geräten auf Arduino-Pico-Basis können die WiFi-Zugangsdaten in der Regel zusätzlich per USB übertragen werden. Dazu muss das Gerät am Rechner angeschlossen und der Transfermodus durch einen Doppelklick auf die Prog-Taste gestartet werden. Kopiere dann eine Datei namens "WIFI.TXT" auf das Wechsellaufwerk. Die erste Zeile muss die SSID und die zweite den PSK enthalten. Danach den Modus wieder mit einem Doppelklick beenden und das Gerät neu starten.

Der WiFi-Assistent kann auch WiFi-Daten, die zuvor auf dem Gerät hinterlegt wurden, löschen. 
Dazu müssen einfach SSID und PSK leer gelassen werden und 'Übertragen' betätigt werden.
