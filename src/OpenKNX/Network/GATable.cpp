#if defined(KNX_IP_WIFI) || defined(KNX_IP_LAN)

#include "OpenKNX/Network/GATable.h"
#include <LittleFS.h>
#include <algorithm>

namespace OpenKNX
{
    namespace Network
    {

        void GATable::begin()
        {
            if (_loaded) return;
            _loaded = true;

            File file = LittleFS.open("/openknx_ga.tsv", "r");
            if (!file) return; // keine Tabelle → getDpt() liefert immer false

            // Zeilenweise on-the-fly parsen: nur die ersten drei Felder
            // (addr, dpt, subset) werden als Zahlen gelesen, der Rest der Zeile
            // (Texte beliebiger Länge) wird übersprungen – kein großer Zeilenpuffer.
            uint8_t field = 0;
            uint32_t num[3] = {0, 0, 0};
            bool digit[3] = {false, false, false};
            bool firstLine = true; // Header überspringen
            _index.reserve(file.size() / 16);              // grobe Schätzung: ~16 Byte/Zeile

            auto flushLine = [&]() {
                // Header (firstLine) und Zeilen ohne gültige Adresse ignorieren.
                if (!firstLine && digit[0] && num[0] > 0 && num[0] <= 0xFFFF)
                {
                    GAEntry e;
                    e.addr = (uint16_t)num[0];
                    e.dpt = digit[1] ? (uint8_t)num[1] : 0;
                    e.sub = digit[2] ? (uint8_t)num[2] : 0;
                    _index.push_back(e);
                }
                firstLine = false;
                field = 0;
                num[0] = num[1] = num[2] = 0;
                digit[0] = digit[1] = digit[2] = false;
            };

            uint8_t buf[128];
            int bufLen = 0;
            int bufPos = 0;

            while (true)
            {
                if (bufPos >= bufLen)
                {
                    bufLen = file.read(buf, sizeof(buf));
                    bufPos = 0;
                    if (bufLen <= 0) break;
                }
                uint8_t c = buf[bufPos++];

                if (c == '\n')
                {
                    flushLine();
                }
                else if (c == '\r')
                {
                    // ignorieren (CRLF)
                }
                else if (c == '\t')
                {
                    if (field < 255) field++;
                }
                else if (field < 3 && c >= '0' && c <= '9')
                {
                    num[field] = num[field] * 10 + (uint32_t)(c - '0');
                    digit[field] = true;
                }
                // alle anderen Zeichen (Texte in Feld >=3, Nicht-Ziffern) ignorieren
            }
            // letzte Zeile ohne abschließendes '\n'
            flushLine();

            file.close();

            // Für die Binärsuche in getDpt() nach Adresse sortieren.
            std::sort(_index.begin(), _index.end(),
                      [](const GAEntry& a, const GAEntry& b) { return a.addr < b.addr; });
        }

        bool GATable::getDpt(uint16_t addr, uint8_t& dpt, uint8_t& sub) const
        {
            if (_index.empty()) return false;

            // Binärsuche im sortierten Index.
            size_t lo = 0, hi = _index.size();
            while (lo < hi)
            {
                size_t mid = (lo + hi) / 2;
                uint16_t a = _index[mid].addr;
                if (a == addr)
                {
                    dpt = _index[mid].dpt;
                    sub = _index[mid].sub;
                    return true;
                }
                if (a < addr)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            return false;
        }

    } // namespace Network
} // namespace OpenKNX

#endif // KNX_IP_WIFI || KNX_IP_LAN
