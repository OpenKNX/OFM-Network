#include "OpenKNX/Network/Webserver/WebResponse.h"
#include "OpenKNX/Network/Webserver/HeapAdmission.h"
#include "OpenKNX/Helper.h"
#include <cassert>
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

int main()
{
    // A fragmented heap cannot provide the old 4/8-KiB growing page string.
    // Real response assembly must still work with allocations limited to 1 KiB.
    blockLimit = 1024;
    const std::string body(12000, 'x');
    for (size_t step : {size_t(1), size_t(17), size_t(512), size_t(513)})
    {
        WebResponse response;
        HtmlWriter html(response);
        html += "prefix";
        html += body;
        html += "suffix";
        response.setLayout(true);
        response.setLayoutChrome("<header>", "<footer>");
        assert(response.statusCode() == 200 && response.isStreaming() && response.isHtmlStream());
        assert(response.streamTotal() == body.size() + 28);
        assert(readResponse(response, step) == "<header>prefix" + body + "suffix<footer>");
        response.cleanupStream();
        response.cleanupStream();
    }
    assert(largestAllocation <= 1024);

    // RP stops at Content-Length rather than asking for an extra EOF read.
    for (const std::string& text : {std::string(), std::string(512, 'a'), std::string(513, 'b')})
    {
        WebResponse response;
        HtmlWriter html(response);
        html += text;
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
        void* context = nullptr;
        auto cleanup = response.releaseStream(context);
        response.reset();
        cleanup(context);
    }

    // Fail context creation and each subsequent chunk allocation. Partial HTML
    // must be discarded, never returned with HTTP 200 or decorated with layout.
    for (size_t failure = 0; failure < 1 + (body.size() + 511) / 512; ++failure)
    {
        allocationCount = 0;
        failAt = failure;
        WebResponse response;
        HtmlWriter html(response);
        html += body;
        html += "still unavailable";
        response.setLayout(true);
        response.setStatus(404); // a handler finishing its page must not undo the 503
        response.setContentType("text/html");
        assert(response.statusCode() == 503 && !response.isStreaming() && !response.isHtmlStream() && !response.useLayout());
        assert(std::string(response.contentType()).rfind("text/plain", 0) == 0);
        response.finalizeSegments();
        assert(response.totalLength() > 0);
        assert(std::string(response.body()).find("insufficient memory") != std::string::npos);
        failAt = static_cast<size_t>(-1);
        response.reset();
        assert(response.statusCode() == 200);
        response.setStatus(404);
        assert(response.statusCode() == 404);
        response.setStatus(200);
        HtmlWriter retry(response);
        retry += "recovered";
        assert(readResponse(response, 17) == "recovered");
    }

    // A normal copied body must report an allocation failure as 503, too.
    WebResponse failure;
    failAt = 0; allocationCount = 0;
    failure.send("copy");
    assert(failure.statusCode() == 503 && failure.bodyLength() > 0);
    failAt = static_cast<size_t>(-1);

    // Keep RP2040's deferred file cleanup contract: reset must not close a file
    // from a network interrupt after its callback was parked for the loop task.
    unsigned cleaned = 0;
    WebResponse file;
    file.sendStream(0, [](void*, uint8_t*, size_t) { return size_t(0); },
        [](void* context) { ++*static_cast<unsigned*>(context); }, &cleaned);
    assert(file.isStreaming() && !file.isHtmlStream()); // RP paces file reads
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
        WebResponse aborted;
        HtmlWriter html(aborted);
        html += body;
        uint8_t buffer[17];
        assert(aborted.readStreamChunk(buffer, sizeof(buffer)) == sizeof(buffer));
        void* context = nullptr;
        auto cleanup = aborted.releaseStream(context);
        assert(!aborted.isHtmlStream());
        aborted.reset();
        cleanup(context);
        aborted.cleanupStream();
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
