### Eigenes WiFi

Inbetriebnahme ohne vorhandenes Netz: Das Gerät spannt ein eigenes, passwortgeschütztes WiFi auf und braucht dafür weder WiFi-Router noch Hausnetz. Es ist ein Inselnetz und führt nicht ins Internet und nicht zu anderen Geräten im Haus.

Nicht zu verwechseln mit dem Einrichtungs-WiFi: Das ist offen und dient nur zum Eintragen der WiFi-Zugangsdaten.

**Typische Einsätze**

- Erstinbetriebnahme, solange das Hausnetz noch nicht steht.
- Service vor Ort, ohne Zugang zum Netz des Kunden.

**So geht's**

- **1.** Mit dem Notebook in das WiFi des Geräts einwählen. Das Notebook erhält seine Adresse automatisch vom Gerät.
- **2.** Bei Geräten mit KNX IP lässt sich das Gerät über dieses WiFi programmieren. Ein IP-Router oder eine IP-Schnittstelle steht unter der Adresse 10.42.0.1 als Verbindung bereit und macht die angeschlossene Linie erreichbar.
- **3.** Ist der Webserver eingeschaltet (*Netzwerk*), ist die Weboberfläche unter http://10.42.0.1 erreichbar.

**Einstellungen**

- *Netzname anpassen*: *Nein* verwendet den Hostnamen des Geräts als WiFi-Namen. *Ja* blendet *Netzname (SSID)* ein, bis zu 32 Zeichen; leer gilt ebenfalls der Hostname.
- *Frequenzband*, *Kanal (5 GHz)*: nur bei Geräten mit 5-GHz-Funk. Angeboten werden die Kanäle 36 bis 48; sie kommen in der EU ohne Radarerkennung aus. *Automatisch* wählt unter ihnen den, der in der Umgebung am wenigsten belegt ist.
- *Kanal (2,4 GHz)*: *Automatisch* wählt beim Start unter den überlappungsfreien Kanälen 1, 6 und 11 den, der in der Umgebung am wenigsten belegt ist. Ein fester Kanal (1 bis 13) ist für Sonderfälle, etwa wenn ein bestimmter Kanal frei bleiben soll.
- *Passwort*: 8 bis 63 Zeichen, Pflicht. Ohne übertragenes Passwort startet das eigene WiFi nicht.
- *Passwort übertragen*: schickt das Passwort direkt ins Gerät. Nach erfolgreichem Übertragen wird das Feld im Projekt geleert; das Passwort liegt dann nur im Gerät. Geräte mit Raspberry-Pi-Pico-Chip legen es im Dateisystem ab. Dort ist es über den Dateimanager der Weboberfläche lesbar, sofern vorhanden und der Webserver eingeschaltet ist, und bei Geräten mit Dateiübertragung auch über den Bus.

**Wichtig:** Wer im WiFi des Geräts ist, erreicht das Gerät; bei Geräten mit KNX IP auch per KNX, bei einem IP-Router oder einer IP-Schnittstelle auch die angeschlossene Linie. Ein sicheres Passwort wählen und nur an Berechtigte weitergeben.
