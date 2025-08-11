### NTP-Client

Durch das Aktivieren des NTP-Clients kann das Gerät die aktuelle Zeit zyklisch von einem Zeitserver abrufen, anstatt sie vom Bus zu beziehen. Zudem kann das Gerät auf Wunsch die aktuelle Zeit auch auf den Bus senden.
Die bisherigen Einstellungen bzw. Kommunikationsobjekte zum Abrufen der Zeit vom Bus entfallen. Stattdessen stehen drei neue Kommunikationsobjekte zur Verfügung, mit denen Zeit, Datum und beides kombiniert auf dem Bus bereitgestellt werden können.

Außerdem kann der Zeitserver (NTP-Server) angepasst werden, von dem die aktuelle Zeit bezogen wird. In der Regel ist eine Änderung nicht erforderlich, da der voreingestellte Server (openknx.pool.ntp.org) zuverlässig arbeitet und weit verbreitet ist. Dieser Server fungiert als Alias für eine Vielzahl von Zeitservern.

