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

            static std::string ansiToHtml(const std::string& s);

          private:
            std::map<int, uint32_t> _consoleReadPos;
            char _pendingCmd[256] = {};
        };

    } // namespace Network
} // namespace OpenKNX

#endif // OPENKNX_WEBCONSOLE
