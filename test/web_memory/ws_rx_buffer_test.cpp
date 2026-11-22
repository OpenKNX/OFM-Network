#include "OpenKNX/Network/Webserver/WebSocketRxBuffer.h"

#include <cassert>

using OpenKNX::Network::WebSocketRxBuffer;

int main()
{
    WebSocketRxBuffer<2048> buffer;
    auto* inlineStart = buffer.data();
    const auto inlineSize = buffer.room();
    assert(inlineSize == 256 + 14);

    buffer.tail()[0] = 42;
    buffer.commit(1);
    buffer.consume(1);
    assert(buffer.room() == inlineSize && buffer.data() == inlineStart);

    std::memset(buffer.tail(), 0xa5, inlineSize);
    buffer.commit(inlineSize);
    assert(!buffer.growForFrame(2048 + 15));
    assert(buffer.data() == inlineStart && buffer.size() == inlineSize);
    assert(buffer.growForFrame(2048 + 14));
    assert(buffer.data() != inlineStart && buffer.size() == inlineSize);
    for (std::size_t i = 0; i < inlineSize; ++i)
        assert(buffer.data()[i] == 0xa5);

    const auto overflowRoom = buffer.room();
    std::memset(buffer.tail(), 0x6b, overflowRoom);
    buffer.commit(overflowRoom);
    buffer.consume(512);
    assert(buffer.data() != inlineStart && buffer.size() == 2048 + 14 - 512);

    // After a large frame, a partial next frame fits back in inline storage.
    buffer.consume(buffer.size() - 2);
    assert(buffer.data() == inlineStart && buffer.size() == 2);
    assert(buffer.data()[0] == 0x6b && buffer.data()[1] == 0x6b);
    buffer.consume(2);
    assert(buffer.room() == inlineSize);
    assert(buffer.growForFrame(800));
    buffer.commit(800);
    buffer.consume(800);
    assert(buffer.data() == inlineStart && buffer.room() == inlineSize);

    WebSocketRxBuffer<128> smallerLimit;
    assert(smallerLimit.room() == 128 + 14);
    assert(!smallerLimit.growForFrame(128 + 15));
}
