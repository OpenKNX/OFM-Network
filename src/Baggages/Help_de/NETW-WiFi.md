### WiFi

**WiFi-Schnittstelle** (Seite *Netzwerk > Allgemein*): Nur bei Geräten mit WiFi-Schnittstelle auf *Ja* stellen; dann erscheint die Seite *WiFi*. Bei Geräten ohne WiFi-Schnittstelle bleibt die Einstellung auf *Nein*, die WiFi-Einstellungen hätten dort keine Wirkung.

**WiFi-Betriebsart: zwei Einsatzfälle**

- *Mit vorhandenem WiFi verbinden* – **kein Netzwerkkabel, aber gutes WiFi.** Nebengebäude, Garage, Gartenhaus oder ein Altbau, in dem sich kein Kabel nachziehen lässt. Das Gerät meldet sich im bestehenden WiFi an; Geräte mit KNX IP kommunizieren darüber auch mit KNX. Ein IP-Router bindet so seine TP-Linie als weitere Linie an; über eine IP-Schnittstelle lässt sich die Linie programmieren und visualisieren.
- *Eigenes WiFi (Access Point)* – **noch gar kein Netz.** Baustelle, Erstinbetriebnahme, Service vor Ort. Das Gerät spannt ein eigenes, passwortgeschütztes WiFi auf (Adresse 10.42.0.1). Das Notebook wählt sich ein und erreicht das Gerät ohne Kabel und ohne Hausnetz. Bei Geräten mit KNX IP läuft die Programmierung darüber; über einen IP-Router oder eine IP-Schnittstelle auch die der angeschlossenen Linie.

Nur eine Betriebsart zur Zeit: Das Gerät arbeitet entweder als Teilnehmer in einem WiFi oder als eigenes WiFi.

**Funkqualität bestimmt die Übertragung**

Entfernung, Wände, Decken und andere Netze auf demselben Kanal kosten Signal; ein Verteiler aus Metall dämpft stark. Bei schwachem Signal kommen Telegramme später an oder gehen verloren. Deshalb am Einbauort auf gute WiFi-Abdeckung achten.

- Signal mindestens *gut* (−65 dBm oder besser). Anzeige unter *WiFi-Netze suchen* und auf der Seite *WiFi* der Weboberfläche.
- Standort prüfen: Die Seite *WiFi* der Weboberfläche zeigt Signalverlauf, Antwortzeit und Verluste zum WiFi-Router.
- KNX IP Routing nutzt Multicast: Darüber laufen die Gruppentelegramme von KNX-IP-Geräten und die Verbindung von Router zu Router. Diese Telegramme sendet das WiFi in der Regel ohne Wiederholung; ein schwaches Signal führt hier zuerst zu Verlusten. Tunneling-Verbindungen (Programmierung, Visualisierung) wiederholt das WiFi bei Bedarf.

**LED-Anzeige**

Auf der Seite *LEDs*, Abschnitt *Info LEDs*, lässt sich einer Info-LED die Funktion **WiFi-Status** zuweisen:

| LED | Bedeutung |
|---|---|
| grün | verbunden, Signal *gut* oder *sehr gut* |
| gelb | verbunden, Signal *mittel* |
| orange, blinkt langsam | verbunden, Signal *schwach* |
| rot, blinkt | kein WiFi, das Gerät versucht es weiter |
| rot | keine Zugangsdaten, oder das eigene WiFi startet nicht (kein Passwort) |
| blau, pulsiert | Einrichtungs-WiFi offen |
| blau, blinkt schnell | Prog-Modus ohne Netz: Einrichtungs-WiFi startet in 5 Sekunden |
| türkis | eigenes WiFi aktiv |

Eine einfarbige LED zeigt nur das Muster: an, blinkt oder pulsiert; „keine Zugangsdaten“ bleibt dunkel. Eine nicht dimmbare LED leuchtet statt zu pulsieren.

Geräte mit Raspberry Pi Pico W als Funkmodul: Die LED auf dem Modul zeigt fest den WiFi-Zustand, unabhängig von den Info-LEDs. Sie leuchtet bei bestehender Verbindung und bei aktivem eigenen WiFi oder Einrichtungs-WiFi, blinkt beim Verbindungsaufbau und während der Netzsuche und ist aus, wenn keine Zugangsdaten eingetragen sind und kein Access Point läuft oder wenn das eigene WiFi mangels Passwort nicht startet.

Die Funktion *Netzwerkstatus* zeigt weiter den Netzwerkzustand insgesamt: grün verbunden (flackert bei Verkehr), gelb ohne Adresse (einfarbig: blinkt), rot ohne Netz (einfarbig: dunkel); dazu das Einrichtungs-WiFi ebenfalls blau.

**Online-Funktionen**

*WiFi-Netze suchen*, *Zugangsdaten übertragen* und *Passwort übertragen* arbeiten online mit dem Gerät, bei Geräten mit TP-Anschluss auch über TP. Voraussetzung: Das Gerät ist programmiert (physikalische Adresse und Applikation geladen). Danach die Seite neu betreten.
