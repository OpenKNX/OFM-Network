#include "OpenKNX/Network/Webserver/Webserver.h"

#if defined(OPENKNX_WEBSERVER) && (defined(KNX_IP_WIFI) || defined(KNX_IP_LAN))

#include "OpenKNX.h"
#include "OpenKNX/Helper.h"
#include <cstring>

namespace OpenKNX
{
    namespace Network
    {

        bool WsTxQueue::setup(uint32_t size, uint32_t frameLimit)
        {
            if (_ring) return true;

            // Zweierpotenz erzwingen: die Positionen sind monoton wachsende uint32 und
            // laufen irgendwann über. Nur mit einer Maske bleibt die Abbildung auf den
            // Ring über diesen Überlauf hinweg stetig; ein "% size" würde dort springen.
            uint32_t pow2 = 1;
            while ((pow2 << 1) && (pow2 << 1) <= size)
                pow2 <<= 1;
            if (pow2 < 1024) pow2 = 1024;

            _ring = (uint8_t*)PSRAM_MALLOC(pow2);
            if (!_ring)
            {
                // Report once -- the caller retries, and a scarce heap gains nothing
                // from one message per second.
                if (!_allocFailed)
                {
                    _allocFailed = true;
                    logError("WsTxQueue", "%u bytes unavailable, send queue disabled", pow2);
                }
                return false;
            }
            _size = pow2;
            _mask = pow2 - 1;
            _maxRecord = (pow2 / 2) - HDR_SIZE;
            if (frameLimit && _maxRecord > frameLimit) _maxRecord = frameLimit;
            if (_maxRecord > 65535) _maxRecord = 65535;
            _wr = 0;
            _oldest = 0;
            return true;
        }

        // ── Recordkopf ────────────────────────────────────────────────────────
        // Byteweise, weil der Kopf über das Ringende laufen darf. Die Payload tut
        // das nie -- dafür sorgt der Füllrecord in append().

        uint16_t WsTxQueue::readLen(uint32_t pos) const
        {
            return (uint16_t)_ring[idx(pos)] | ((uint16_t)_ring[idx(pos + 1)] << 8);
        }

        int16_t WsTxQueue::readFd(uint32_t pos) const
        {
            return (int16_t)((uint16_t)_ring[idx(pos + 2)] | ((uint16_t)_ring[idx(pos + 3)] << 8));
        }

        uint8_t WsTxQueue::readRoute(uint32_t pos) const
        {
            return _ring[idx(pos + 4)];
        }

        void WsTxQueue::writeHeader(uint32_t pos, uint16_t len, int16_t fd, uint8_t route)
        {
            _ring[idx(pos)] = (uint8_t)(len & 0xFF);
            _ring[idx(pos + 1)] = (uint8_t)(len >> 8);
            _ring[idx(pos + 2)] = (uint8_t)((uint16_t)fd & 0xFF);
            _ring[idx(pos + 3)] = (uint8_t)((uint16_t)fd >> 8);
            _ring[idx(pos + 4)] = route;
        }

        // Gibt ganze Records frei, bis need Bytes passen. Verdrängen heisst nur
        // "_oldest weiterzählen": nichts wird verschoben, nichts kopiert. _oldest
        // steht dabei immer auf einem Kopf, weil es um genau HDR_SIZE + len springt.
        void WsTxQueue::makeRoom(uint32_t need)
        {
            while (_size - (_wr - _oldest) < need)
                _oldest += HDR_SIZE + readLen(_oldest);
        }

        bool WsTxQueue::append(int16_t fd, uint8_t route, const char* key, const char* data, uint32_t len)
        {
            if (!_ring) return false;

            uint32_t keyLen = key ? (uint32_t)strlen(key) : 0;
            uint32_t payload = keyLen ? keyLen + 1 + len : len;
            if (payload > maxRecord()) return false;

            // Payload nie über das Ringende hinweg: sonst müsste der Drain sie erst in
            // einen Zwischenpuffer kopieren, statt direkt in den Ring zu zeigen.
            uint32_t payloadStart = idx(_wr + HDR_SIZE);
            if (payloadStart + payload > _size)
            {
                uint32_t padLen = _size - payloadStart;
                makeRoom(HDR_SIZE + padLen);
                writeHeader(_wr, (uint16_t)padLen, FD_BROADCAST, ROUTE_PAD);
                _wr += HDR_SIZE + padLen; // steht danach genau auf Ringanfang
                payloadStart = idx(_wr + HDR_SIZE);
            }

            makeRoom(HDR_SIZE + payload);
            writeHeader(_wr, (uint16_t)payload, fd, route);

            uint8_t* dst = _ring + payloadStart;
            if (keyLen)
            {
                memcpy(dst, key, keyLen);
                dst[keyLen] = ':';
                dst += keyLen + 1;
            }
            if (len) memcpy(dst, data, len);

            _wr += HDR_SIZE + payload;
            return true;
        }

        // ── Clients ───────────────────────────────────────────────────────────

        int WsTxQueue::findClient(int fd) const
        {
            for (int i = 0; i < _clientCount; i++)
                if (_clients[i].fd == fd) return i;
            return -1;
        }

        bool WsTxQueue::addClient(int fd, uint8_t route)
        {
            if (!_ring) return false;

            int i = findClient(fd);
            if (i < 0)
            {
                if (_clientCount >= WS_MAX_CLIENTS) return false;
                i = _clientCount++;
            }
            // cursor = _wr: der Client sieht nur, was nach seinem Verbinden eingereiht
            // wurde. Auf RP2040 ist fd der Slotindex und wird wiederverwendet -- ohne
            // das bekäme ein neuer Client die offenen Unicasts seines Vorgängers.
            _clients[i] = {fd, _wr, route, true, 0};
            return true;
        }

        bool WsTxQueue::removeClient(int fd)
        {
            int i = findClient(fd);
            if (i < 0) return false;
            bool delivered = !_clients[i].pendingConnect;
            _clients[i] = _clients[--_clientCount];
            return delivered;
        }

        bool WsTxQueue::takePendingConnect(int i)
        {
            if (!_clients[i].pendingConnect) return false;
            _clients[i].pendingConnect = false;
            return true;
        }

        bool WsTxQueue::overrun(int i) const
        {
            return (int32_t)(_oldest - _clients[i].cursor) > 0;
        }

        bool WsTxQueue::peek(int i, const uint8_t*& data, uint32_t& len)
        {
            if (!_ring) return false;
            Client& c = _clients[i];

            while (c.cursor != _wr)
            {
                uint16_t recLen = readLen(c.cursor);
                uint8_t route = readRoute(c.cursor);
                int16_t recFd = readFd(c.cursor);

                bool mine = (route != ROUTE_PAD) &&
                            ((recFd >= 0) ? (recFd == c.fd) : (route == c.route));
                if (mine)
                {
                    data = _ring + idx(c.cursor + HDR_SIZE);
                    len = recLen;
                    return true;
                }
                // Überspringen rückt den Cursor mit vor. Nicht wegoptimieren: ohne das
                // würde ein Client auf einem anderen Endpoint von fremdem Verkehr
                // überholt und beim Überlauf getrennt, obwohl er nichts verpasst hat.
                c.cursor += HDR_SIZE + recLen;
            }
            return false;
        }

        void WsTxQueue::advance(int i)
        {
            Client& c = _clients[i];
            c.cursor += HDR_SIZE + readLen(c.cursor);
            c.stallSince = 0; // Fortschritt
        }

        void WsTxQueue::reclaim()
        {
            if (!_ring) return;
            if (!_clientCount)
            {
                _oldest = _wr; // niemand da, alles hinfällig
                return;
            }

            uint32_t slowest = _wr;
            for (int i = 0; i < _clientCount; i++)
                if ((int32_t)(_clients[i].cursor - slowest) < 0) slowest = _clients[i].cursor;

            // Never backwards: an overtaken client sits behind _oldest.
            if ((int32_t)(slowest - _oldest) > 0) _oldest = slowest;
        }

        // Evaluate *after* the peek loop: only there does hasPending() mean "did not
        // accept". Before it, records peek() merely skips would count too, and a client
        // on a foreign endpoint would be dropped after 10 s of unrelated traffic.
        bool WsTxQueue::stalled(int i, uint32_t now, uint32_t stallMs)
        {
            Client& c = _clients[i];
            if (!hasPending(i))
            {
                c.stallSince = 0;
                return false;
            }
            if (!c.stallSince)
            {
                c.stallSince = now ? now : 1; // 0 is the "no stall" value
                return false;
            }
            return (now - c.stallSince) > stallMs;
        }

    } // namespace Network
} // namespace OpenKNX

#endif // OPENKNX_WEBSERVER && (KNX_IP_WIFI || KNX_IP_LAN)
