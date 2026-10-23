### WiFi-Zugangsdaten

Zugangsdaten für die Betriebsart *Mit vorhandenem WiFi verbinden*.

- *Netzname (SSID)*: Name des WiFi-Netzes, 1 bis 32 Zeichen. Per *WiFi-Netze suchen* und *Übernehmen* oder von Hand.
- *Passwort*: 8 bis 63 Zeichen; bei einem offenen Netz leer lassen.

*Zugangsdaten übertragen* schickt beides direkt ins Gerät, bei Geräten mit TP-Anschluss auch über TP. Nach erfolgreichem Übertragen werden die Felder und die Liste der gefundenen Netze im Projekt geleert; die Zugangsdaten liegen dann nur im Gerät. Geräte mit Raspberry-Pi-Pico-Chip legen sie im Dateisystem ab. Dort sind sie über den Dateimanager der Weboberfläche lesbar, sofern vorhanden und der Webserver eingeschaltet ist, und bei Geräten mit Dateiübertragung auch über den Bus. Das Gerät startet neu und verbindet sich mit dem WiFi.

**Andere Wege**

Über das Einrichtungs-WiFi des Geräts oder die Konsole: `net wifi set SSID PSK`. Enthält der Netzname ein Leerzeichen, ein anderes Trennzeichen nehmen, das im Netznamen nicht vorkommt, z. B. `net wifi set;Mein Netz;Passwort`. `net wifi erase` löscht die Zugangsdaten.

Bei Geräten mit USB-Dateiaustausch auch per USB: Gerät am Rechner anschließen und die Prog-Taste doppelt drücken; das Gerät erscheint als Laufwerk. In der Datei *Wifi.txt* die erste Zeile durch den Netznamen und die zweite durch das Passwort ersetzen, speichern und die Prog-Taste erneut doppelt drücken. Das Gerät startet neu und verbindet sich. Offene Netze ohne Passwort gehen auf diesem Weg nicht.

**Im Betrieb**

Geht die Verbindung verloren, versucht das Gerät sie selbst wieder aufzubauen, bis das Netz wieder erreichbar ist. Die Seite *WiFi* der Weboberfläche zeigt Signal, Antwortzeit und Verluste zum WiFi-Router; auf der Konsole zeigt `net wifi` den Zustand, `net wifi reconnect` verbindet sofort neu.
