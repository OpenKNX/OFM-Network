### WiFi-Assistent

Dieser WiFi-Assistent ermöglicht das Übertragen von WiFi-Zugangsdaten auf das Gerät. Voraussetzung ist, dass die verwendete Hardware einen WiFi-Adapter verwendet. Geräte, die per IP-Netzwerk angebunden werden, müssen bereits über eine WiFi-Verbindung und somit über gültige Zugangsdaten verfügen. Der Assistent kann daher nur die bestehenden Zugangsdaten ändern. TP-Geräte können hingegen immer per Bus angepasst werden.

IP-Geräte ohne WiFi-Zugangsdaten müssen initial auf anderem Wege eingerichtet werden. Dies hängt sowohl von der Gerätesoftware als auch der verwendeten Hardware ab. Die Einrichtung per Terminal (USB) sollte immer funktionieren. Dafür muss auf der Konsole nur "wifi SSID PSK" eingegeben werden.

Bei Geräten auf Arduino-Pico-Basis können die WiFi-Zugangsdaten in der Regel zusätzlich per USB übertragen werden. Dazu muss das Gerät am Rechner angeschlossen und der Transfermodus durch einen Doppelklick auf die Prog-Taste gestartet werden. Kopiere dann eine Datei namens "WIFI.TXT" auf das Wechsellaufwerk. Die erste Zeile muss die SSID und die zweite den PSK enthalten. Danach den Modus wieder mit einem Doppelklick beenden und das Gerät neu starten.