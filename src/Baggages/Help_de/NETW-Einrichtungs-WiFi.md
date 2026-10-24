### Einrichtungs-WiFi

WiFi einrichten mit Handy oder Notebook, ohne Programmierung: Das Gerät öffnet ein offenes WiFi mit seinem Hostnamen als Namen. Darin ist nur die Einrichtungsseite erreichbar; die übrige Weboberfläche, Firmware-Updates über das Netz und KNXnet/IP bleiben gesperrt. Für die Inbetriebnahme ohne vorhandenes Netz dient die Betriebsart *Eigenes WiFi (Access Point)*.

Standard: ein.

**So geht's**

- **1.** Mit Handy oder Notebook in das WiFi des Geräts einwählen. An Geräten mit Display führt ein QR-Code direkt hinein.
- **2.** Die Einrichtungsseite öffnet sich von selbst, sonst http://10.42.0.1/wifi aufrufen.
- **3.** Netz wählen, Passwort eingeben, speichern. Das Gerät startet neu und verbindet sich.

**Wann es erscheint**

- **Keine Zugangsdaten:** Das Gerät kennt noch kein WiFi.
- **Verbindung gescheitert:** Ein noch nicht programmiertes Gerät konnte sich 30 Sekunden nach dem Start nicht verbinden, z. B. wegen eines falschen Passworts. Ist dann 5 Minuten niemand eingewählt, versucht es zwischendurch wieder das eingetragene Netz, etwa wenn der WiFi-Router nach einem Stromausfall später startet.
- **Prog-Taste:** Ist das Netz weg und wird der Prog-Modus eingeschaltet, öffnet das Gerät das Einrichtungs-WiFi nach etwa 5 Sekunden, auch wenn es programmiert ist. Wird der Prog-Modus vorher wieder ausgeschaltet, unterbleibt es. Ist 10 Minuten niemand eingewählt, schließt es wieder und das Gerät versucht erneut das eingetragene Netz.
- **Konsole:** *net wifi ap* öffnet das Einrichtungs-WiFi sofort, auch bei bestehender Verbindung; die Zugangsdaten bleiben erhalten. Es schließt wie bei der Prog-Taste nach 10 Minuten ohne Einwahl, *net wifi reconnect* schließt es sofort.

Eine Info-LED mit der Funktion *WiFi-Status* oder *Netzwerkstatus* pulsiert, solange das Einrichtungs-WiFi offen ist, und blinkt in den 5 Sekunden davor schnell; eine farbige LED leuchtet dabei blau, eine nicht dimmbare LED leuchtet statt zu pulsieren. Alle Zustände: siehe Hilfe *WiFi*, Abschnitt *LED-Anzeige*.

**Einrichtungs-WiFi aus** (Haken *Ein* nicht gesetzt): Ein programmiertes Gerät öffnet nie ein Einrichtungs-WiFi, auch nicht über die Prog-Taste oder *net wifi ap*. Fehlen dann die Zugangsdaten, hilft nur noch die Programmierung über TP (bei Geräten mit TP-Anschluss) oder die Konsole.
