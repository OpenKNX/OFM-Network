### MQTT

Das Gerät meldet per MQTT, dass es läuft (*status*, *uptime*), und sendet auf Wunsch die Telegramme der TP-Linie. Es verbindet sich entweder mit einem vorhandenen Broker oder arbeitet selbst als kleiner Broker. MQTT startet, sobald das Gerät im Netzwerk ist. Ist MQTT in der Firmware des Geräts nicht enthalten, bleibt die Einstellung ohne Wirkung. Die Verbindung ist unverschlüsselt (kein TLS); Benutzer und Passwort gehen im Klartext über das Netzwerk.

**Verbindungsdaten**

- *Modus*: *Client* verbindet sich mit dem Broker unter *Server*. *Broker* startet einen eigenen Broker im Gerät; andere MQTT-Clients verbinden sich dann mit der IP-Adresse des Geräts und dem eingestellten *Port*. Der eigene Broker nimmt höchstens 3 Clients an (ESP32: 5), liefert mit QoS 0 aus und hält retained Nachrichten nur bis zum Neustart.
- *Server*: Hostname oder IP-Adresse des Brokers, höchstens 20 Zeichen. Nur im Modus *Client*; bleibt *Server* leer, ist MQTT aus.
- *Port*: im Modus *Client* der Port des Brokers, im Modus *Broker* der Port, auf dem das Gerät Verbindungen annimmt. Standard 1883.
- *Benutzer* und *Passwort*: je höchstens 20 Zeichen. Im Modus *Client* meldet sich das Gerät damit am Broker an. Im Modus *Broker* müssen sich Clients mit genau diesen Daten anmelden; bleibt *Benutzer* leer, nimmt der Broker jeden Client an. Ein Passwort ohne Benutzer ist nicht zulässig: im Modus *Client* kann der Broker die Verbindung dann ablehnen, im Modus *Broker* wird es ignoriert. Im Modus *Broker* kann sich bei gesetztem *Benutzer* und leerem *Passwort* kein Client anmelden.

**Erweiterte Funktionen**

- *Abweichender Gerätepräfix*: Alle Topics des Geräts beginnen mit *openknx/Präfix/*. Ohne eigenen Präfix sind das die letzten 8 Stellen der Seriennummer (ohne *00FA:*, z. B. *openknx/0001A2B3/*). Ein eigener Präfix hat höchstens 20 Zeichen aus A–Z, a–z, 0–9 und Bindestrich; leer bedeutet wieder die Seriennummer.
- *Sende KNX TP Rohdaten*: Jedes Telegramm auf der TP-Linie, auch selbst gesendete und Wiederholungen (im JSON unter *flags* gekennzeichnet), wird als JSON auf *openknx/Präfix/knx/tp/telegramm* veröffentlicht, in beiden Modi, nicht retained. Ohne MQTT-Verbindung werden Telegramme nicht zwischengespeichert; bei Dauerlast können Telegramme verloren gehen. Wirkt nur bei Geräten mit TP-Anschluss (z. B. IP-Interface).

**Topics**

- *openknx/Präfix/status*: *1* etwa alle 10 Sekunden, solange die Verbindung zum Broker steht bzw. der eigene Broker läuft (nicht retained). Im Modus *Client* setzt der Broker es auf *0* (retained), wenn die Verbindung abbricht, bei Stromausfall nach bis zu 90 Sekunden. Diese *0* bleibt am Broker gespeichert: ein Client, der *status* danach neu abonniert, erhält zuerst *0* und, sobald das Gerät wieder verbunden ist, nach etwa 10 Sekunden *1*. Im Modus *Broker* gibt es keine *0*; ein neuer Client erhält *1* nach etwa 10 Sekunden.
- *openknx/Präfix/uptime*: Sekunden seit dem Start, zusammen mit *status*.
