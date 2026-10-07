#pragma once

#ifdef OPENKNX_WEBCONSOLE

#include <cstdint>

namespace OpenKNX
{
    namespace Network
    {

        class Webconsole
        {
          public:
            void setup();
            void loop();

          private:
            // Eine Leseposition für alle Clients: die Logzeile gilt ohnehin für jeden,
            // und die Sendequeue der Transportschicht hält sie jetzt vor. Ohne Clients
            // läuft sie mit dem Schreibzeiger mit, damit ein neuer Client kein Backlog
            // bekommt.
            uint32_t _readPos = 0;
            bool _initialized = false;
            // Summed up and reported at most once a second, else a blocked console
            // would emit one report per tick.
            uint32_t _lostBytes = 0;
            uint32_t _lostReported = 0;
            char _pendingCmd[256] = {};
            // Separates Flag: ein leeres Enter (Befehl der Länge 0) lässt sich nicht
            // über _pendingCmd[0] != '\0' abbilden, soll aber serverseitig als Zeile
            // ausgegeben werden (Echo via Logger → Serial + alle Webconsole-Clients).
            // Doubles as the buffer's occupancy marker: the writer in the network
            // callback only touches it while this is clear.
            volatile bool _hasPendingCmd = false;
        };

    } // namespace Network
} // namespace OpenKNX

#endif // OPENKNX_WEBCONSOLE
