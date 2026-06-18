#if defined(KNX_IP_WIFI) || defined(KNX_IP_LAN)
#pragma once

#include "OpenKNX/Helper.h" // PsramAllocator / OPENKNX_PSRAM
#include <cstdint>
#include <vector>

namespace OpenKNX
{
    namespace Network
    {

        // Kompakte GA→DPT-Tabelle, geladen aus /openknx_ga.tsv (LittleFS).
        // Hält pro GA nur Adresse + DPT/Subtyp (4 Bytes, reines POD, keine Strings) –
        // die variabel langen Texte (Name, Haupt-/Mittelgruppe) bleiben auf dem
        // Dateisystem und werden client-seitig (Browser) verarbeitet.
        // Genutzt vom Group Monitor zur Wert-Dekodierung (knx-Stack), später auch MQTT.
        class GATable
        {
          public:
            // Liest /openknx_ga.tsv einmalig ein und baut den sortierten Index.
            // Idempotent: ein erneuter Aufruf lädt nicht neu. Datei fehlt → leer.
            void begin();

            // O(log n)-Lookup ohne Datei-I/O. Liefert false, wenn GA unbekannt
            // oder keine Tabelle geladen ist.
            bool getDpt(uint16_t addr, uint8_t& dpt, uint8_t& sub) const;

            // Anzahl indizierter GAs (für Logging/Diagnose).
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
