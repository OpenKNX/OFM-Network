### WiFi-Netze suchen

*WiFi-Netze suchen* lässt das Gerät am Einbauort nach WiFi-Netzen in Reichweite suchen, also genau dort, wo später die Verbindung stehen muss. Die Suche dauert wenige Sekunden.

Die Tabelle zeigt bis zu 8 Netze, das stärkste zuerst:

- *Netzname (SSID)*: Name des WiFi-Netzes. Netze ohne Namen werden nicht angezeigt.
- *Signal* und *Pegel*: Stufe und Empfangsstärke in dBm.
  - *sehr gut*: −55 dBm oder besser
  - *gut*: −65 dBm oder besser, Empfehlung für den Betrieb
  - *mittel*: −75 dBm oder besser, Verluste möglich
  - *schwach*: darunter, nicht für den Betrieb geeignet
- *Kanal*: Funkkanal, im 2,4-GHz-Band 1 bis 13.
- *Sicherheit*: *offen* (ohne Passwort), WEP, WPA, WPA2, WPA3, Mischformen oder *Enterprise*. Geräte mit Raspberry-Pi-Pico-Funkchip unterscheiden WPA2 und WPA3 nicht und zeigen *WPA2/WPA3*; WEP zeigen sie als *unbekannt*. Enterprise-Netze (Anmeldung mit Benutzername) werden nicht unterstützt.
- *Übernehmen*: trägt den Netznamen in *Netzname (SSID)* ein und leert die Liste; eine neue Suche bringt sie zurück. Danach Passwort eingeben und *Zugangsdaten übertragen*.

**Wenn es nicht passt**

- Netz fehlt: außer Reichweite, versteckt oder nicht im 2,4-GHz-Band. Ein verstecktes Netz lässt sich trotzdem nutzen: Namen direkt in *Netzname (SSID)* eintragen.
- Signal nur *mittel* oder *schwach*: Ein Access Point näher am Verteiler oder ein Verteiler, der nicht aus Metall ist, verbessert den Empfang.

Der Knopf geht erst, wenn das Gerät programmiert ist; danach die Seite neu betreten.
