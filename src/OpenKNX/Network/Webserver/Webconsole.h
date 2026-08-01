#pragma once

#ifdef OPENKNX_WEBCONSOLE

#include <cstdint>
#include <map>
#include <string>
#include <vector>

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
            std::map<int, uint32_t> _consoleReadPos;
            char _pendingCmd[256] = {};
            // Separates Flag: ein leeres Enter (Befehl der Länge 0) lässt sich nicht
            // über _pendingCmd[0] != '\0' abbilden, soll aber serverseitig als Zeile
            // ausgegeben werden (Echo via Logger → Serial + alle Webconsole-Clients).
            volatile bool _hasPendingCmd = false;
        };

    } // namespace Network
} // namespace OpenKNX

#endif // OPENKNX_WEBCONSOLE
