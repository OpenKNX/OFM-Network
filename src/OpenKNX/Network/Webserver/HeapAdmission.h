#pragma once
#include <cstddef>

#ifndef OPENKNX_WEBSERVER_MIN_FREE_HEAP
#define OPENKNX_WEBSERVER_MIN_FREE_HEAP 8192
#endif
#ifndef OPENKNX_WEBSERVER_MIN_HEAP_BLOCK
#define OPENKNX_WEBSERVER_MIN_HEAP_BLOCK 2048
#endif

namespace OpenKNX
{
    namespace Network
    {
        inline bool webHeapReserveFits(size_t freeBytes, size_t bodyLength, size_t reserve)
        {
            return bodyLength <= freeBytes && freeBytes - bodyLength >= reserve;
        }

        inline bool webHeapFits(size_t freeBytes, size_t largestBlock, size_t bodyLength, size_t reserve, size_t minimumBlock)
        {
            return webHeapReserveFits(freeBytes, bodyLength, reserve) &&
                   largestBlock >= bodyLength && largestBlock >= minimumBlock;
        }
    }
}
