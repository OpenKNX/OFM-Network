#pragma once

#ifdef OPENKNX_WEBSERVER

#include <cstddef>
#include <cstdint>

namespace OpenKNX
{
    namespace Network
    {

        // Sendequeue der WS-Transportschicht: ein geteilter Record-Ring für alle
        // Clients, je Client nur ein Cursor. Ein Broadcast liegt damit einmal im
        // Speicher statt einmal je Client, und der Bedarf hängt nicht an der
        // Clientzahl. Wer mitkommt, steht dicht an _wr und kann von der Verdrängung
        // (die am _oldest-Ende arbeitet) nicht eingeholt werden.
        class WsTxQueue
        {
          public:
            // Recordkopf im Ring, direkt vor der Payload: len(2) | fd(2) | route(1).
            // Bewusst 5 und nicht sizeof(struct) -- Padding machte daraus 6 und damit
            // einen stillen Off-by-one durch den ganzen Ring.
            static constexpr uint32_t HDR_SIZE = 5;
            static constexpr int16_t FD_BROADCAST = -1;
            static constexpr uint8_t ROUTE_PAD = 0xFF; // Füllrecord vor dem Ringende

            // frameLimit = largest payload the platform fits into one WS frame. Without
            // it the ring would accept records that can never be sent.
            bool setup(uint32_t size, uint32_t frameLimit);
            bool active() const { return _ring != nullptr; }
            // Half the ring so one message cannot evict everything, capped at the frame
            // limit and at UINT16_MAX -- the header carries the length in 16 bits.
            uint32_t maxRecord() const { return _maxRecord; }
            uint32_t capacity() const { return _size; }
            uint32_t freeBytes() const { return _size ? _size - (_wr - _oldest) : 0; }

            // key optional: ist er gesetzt, entsteht "key:data" ohne Zwischenpuffer.
            bool append(int16_t fd, uint8_t route, const char* key, const char* data, uint32_t len);

            // false = no ring or cursor table full. A client without a cursor would
            // receive nothing and loop reconnecting, so the caller must react.
            bool addClient(int fd, uint8_t route);
            // true, wenn die Verbindungsmeldung bereits zugestellt war. Sonst hat das
            // Modul von diesem Client nie erfahren und darf auch kein Disconnect sehen.
            bool removeClient(int fd);
            int clientCount() const { return _clientCount; }
            int clientFd(int i) const { return _clients[i].fd; }
            uint8_t clientRoute(int i) const { return _clients[i].route; }

            // Verbindungsmeldung wird bewusst verzögert zugestellt, siehe Webserver::loop().
            bool takePendingConnect(int i);
            bool overrun(int i) const;

            // Liefert den nächsten für diesen Client bestimmten Record. Fremde Records
            // werden dabei übersprungen und der Cursor rückt mit vor -- ein Client auf
            // einem anderen Endpoint bleibt so immer dicht an _wr und wird nie überholt.
            bool peek(int i, const uint8_t*& data, uint32_t& len);
            void advance(int i);

            // Anything left for this client? After a drain pass that means WouldBlock.
            bool hasPending(int i) const { return _clients[i].cursor != _wr; }
            // Accepted nothing for stallMs = really gone, not merely slower than a
            // producer that is currently writing a lot.
            bool stalled(int i, uint32_t now, uint32_t stallMs);

            // Frees whatever even the slowest client has read. Without it _oldest only
            // advances on eviction and freeBytes() would report "full" forever.
            void reclaim();

          private:
            struct Client
            {
                int fd;
                uint32_t cursor;
                uint8_t route;
                bool pendingConnect;
                uint32_t stallSince; // 0 = macht Fortschritt
            };

            uint32_t idx(uint32_t pos) const { return pos & _mask; }
            uint16_t readLen(uint32_t pos) const;
            int16_t readFd(uint32_t pos) const;
            uint8_t readRoute(uint32_t pos) const;
            void writeHeader(uint32_t pos, uint16_t len, int16_t fd, uint8_t route);
            void makeRoom(uint32_t need);
            int findClient(int fd) const;

            bool _allocFailed = false; // so the error is not logged on every attempt
            uint8_t* _ring = nullptr;
            uint32_t _size = 0;
            uint32_t _mask = 0; // _size ist immer eine Zweierpotenz, siehe setup()
            uint32_t _maxRecord = 0;
            uint32_t _wr = 0;
            uint32_t _oldest = 0;

            Client _clients[WS_MAX_CLIENTS] = {};
            int _clientCount = 0;
        };

    } // namespace Network
} // namespace OpenKNX

#endif // OPENKNX_WEBSERVER
