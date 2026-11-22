#include "OpenKNX/Network/Webserver/WebResponse.h"
#include "OpenKNX/Network/Webserver/HeapAdmission.h"
#include "OpenKNX/Helper.h"
#include <cassert>
#include <cstring>
#include <string_view>
#include <iostream>
#include <string>

using namespace OpenKNX::Network;

static std::string readResponse(WebResponse& response, size_t step)
{
    uint8_t buffer[513];
    std::string result;
    for (size_t count; (count = response.readStreamChunk(buffer, step));)
        result.append(reinterpret_cast<char*>(buffer), count);
    return result;
}

struct StreamState
{
    std::string_view remaining;
    unsigned cleaned = 0;
};

static size_t readStream(void* context, uint8_t* buffer, size_t length)
{
    auto& stream = *static_cast<StreamState*>(context);
    const size_t count = std::min(length, stream.remaining.size());
    if (count)
    {
        std::memcpy(buffer, stream.remaining.data(), count);
        stream.remaining.remove_prefix(count);
    }
    return count;
}

static void cleanupStream(void* context)
{
    ++static_cast<StreamState*>(context)->cleaned;
}

int main()
{
    // Stream large and empty bodies with layout under a small allocation limit.
    blockLimit = 1024;
    const std::string body(12000, 'x');
    for (size_t step : {size_t(1), size_t(17), size_t(512), size_t(513)})
    {
        StreamState stream{body};
        WebResponse response;
        response.setLayout(true);
        response.sendStream(body.size(), readStream, cleanupStream, &stream);
        response.setLayoutChrome("<header>", "<footer>");
        assert(response.statusCode() == 200 && response.isStreaming());
        assert(response.streamTotal() == body.size() + 16);
        assert(readResponse(response, step) == "<header>" + body + "<footer>");
        response.cleanupStream();
        response.cleanupStream();
        assert(stream.cleaned == 1);
    }
    assert(largestAllocation <= blockLimit);

    // RP stops at Content-Length.
    for (const std::string& text : {std::string(), std::string(512, 'a'), std::string(513, 'b')})
    {
        StreamState stream{text};
        WebResponse response;
        response.sendStream(text.size(), readStream, cleanupStream, &stream);
        response.setLayoutChrome("<h>", "<f>");
        const size_t total = response.streamTotal();
        std::string sent;
        uint8_t buffer[17];
        while (sent.size() < total)
        {
            size_t count = response.readStreamChunk(buffer, std::min(sizeof(buffer), total - sent.size()));
            assert(count > 0);
            sent.append(reinterpret_cast<char*>(buffer), count);
        }
        assert(sent == "<h>" + text + "<f>");
    }

    // Copied bodies report allocation failure without allocating a second body.
    WebResponse failure;
    failAt = 0;
    allocationCount = 0;
    failure.setLayout(true);
    failure.send("copy");
    failure.setLayout(true);
    failure.setStatus(404);
    failure.setContentType("text/html");
    assert(failure.statusCode() == 503 && failure.bodyLength() > 0 && !failure.useLayout());
    assert(std::string(failure.contentType()).rfind("text/plain", 0) == 0);
    failAt = static_cast<size_t>(-1);
    failure.reset();
    failure.send("recovered");
    assert(failure.statusCode() == 200 && std::string(failure.body()) == "recovered");

    // Keep RP2040's deferred file cleanup contract: reset must not close a file
    // from a network interrupt after its callback was parked for the loop task.
    unsigned cleaned = 0;
    WebResponse file;
    file.sendStream(0, [](void*, uint8_t*, size_t) { return size_t(0); },
        [](void* context) { ++*static_cast<unsigned*>(context); }, &cleaned);
    assert(file.isStreaming());
    void* context = nullptr;
    auto cleanup = file.releaseStream(context);
    file.reset();
    assert(cleaned == 0);
    cleanup(context);
    assert(cleaned == 1);

    // Reset/replacement/destruction own cleanup unless explicitly transferred.
    for (int mode = 0; mode < 3; ++mode)
    {
        WebResponse owned;
        owned.sendStream(0, [](void*, uint8_t*, size_t) { return size_t(0); },
            [](void* context) { ++*static_cast<unsigned*>(context); }, &cleaned);
        if (mode == 0) owned.reset();
        if (mode == 1) owned.sendStatic("replacement");
    }
    assert(cleaned == 4);

    // Disconnect after consuming only part of a chunk; ownership moves to the
    // deferred queue, so reset and later cleanup cannot free it twice.
    {
        StreamState stream{body};
        WebResponse aborted;
        aborted.sendStream(body.size(), readStream, cleanupStream, &stream);
        uint8_t buffer[17];
        assert(aborted.readStreamChunk(buffer, sizeof(buffer)) == sizeof(buffer));
        void* context = nullptr;
        auto cleanup = aborted.releaseStream(context);
        assert(!aborted.isStreaming());
        aborted.reset();
        cleanup(context);
        aborted.cleanupStream();
        assert(stream.cleaned == 1);
    }

    assert(webHeapReserveFits(8192, 0, 8192));
    assert(!webHeapReserveFits(8192, 1, 8192));
    assert(!webHeapReserveFits(8192, static_cast<size_t>(-1), 8192));
    assert(webHeapFits(20000, 4096, 1024, 16384, 4096));
    assert(!webHeapFits(17000, 4096, 1024, 16384, 4096));
    assert(!webHeapFits(20000, 2048, 0, 16384, 4096));
    assert(!webHeapFits(20000, 4096, static_cast<size_t>(-1), 16384, 4096));
    std::cout << "Web response fragmentation, allocation failure and stream cleanup checks passed\n";
}
