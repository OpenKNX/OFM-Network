#if defined(KNX_IP_WIFI) || defined(KNX_IP_LAN)
#pragma once

#include "OpenKNX/Helper.h" // PsramAllocator / OPENKNX_PSRAM
#include <cstdint>
#include <vector>

namespace OpenKNX
{
    namespace Network
    {

        // GA→DPT-Tabelle aus /openknx_ga.tsv (LittleFS). Nur Adresse + DPT/Subtyp,
        // Name/Gruppentexte bleiben auf dem Dateisystem und werden clientseitig gelesen.
        class GATable
        {
          public:
            void begin(); // idempotent, Datei fehlt → leer
            bool getDpt(uint16_t addr, uint8_t& dpt, uint8_t& sub) const;
            size_t count() const { return _index.size(); }

          private:
            struct GAEntry
            {
                uint16_t addr;
                uint8_t dpt;
                uint8_t sub;
            };

#ifdef OPENKNX_PSRAM
            std::vector<GAEntry, PsramAllocator<GAEntry>> _index;
#else
            std::vector<GAEntry> _index;
#endif
            bool _loaded = false;
        };

    } // namespace Network
} // namespace OpenKNX

#endif // KNX_IP_WIFI || KNX_IP_LAN
