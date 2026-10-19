#ifdef DEVICE_DISPLAY_MODULE

#include "WidgetNetSpeed.h"
#include "OpenKNX.h"

// Total LAN packet counters, provided by NetworkModule (lwIP byte counters aren't available: prebuilt core).
// RP2040 takes them from the W5500 driver, ESP32 with wired LAN counts at the lwIP netif. A build without
// them (WiFi) shows "n/a".
#include "OpenKNX/Network/Module.h" // openknxLanTraffic(), OPENKNX_NET_HAVE_LAN_TRAFFIC
#ifdef OPENKNX_NET_HAVE_LAN_TRAFFIC
#define WIDGET_NETSPEED_HAVE_COUNTERS 1
#endif

WidgetNetSpeed::WidgetNetSpeed(uint32_t displayTime, WidgetFlags action)
    : _state(WidgetState::STOPPED), _displayTime(displayTime), _action(action), _display(nullptr)
{
}

void WidgetNetSpeed::setup()
{
    if (_display == nullptr) logErrorP("Display is NULL.");
}

void WidgetNetSpeed::start()
{
    if (_state == WidgetState::RUNNING) return;
    _state = WidgetState::RUNNING;
    _forceRedraw = true;
    _haveBase = false; // the first sample only takes the base; a rate over the time off would stand for the whole gap
    _haveRate = false;
}

void WidgetNetSpeed::stop()
{
    _state = WidgetState::STOPPED;
    if (_display)
    {
        _display->display->clearDisplay();
        _display->displayBuff();
    }
}

void WidgetNetSpeed::pause()
{
    if (_state == WidgetState::RUNNING) _state = WidgetState::PAUSED;
}

void WidgetNetSpeed::resume()
{
    if (_state == WidgetState::PAUSED)
    {
        _state = WidgetState::RUNNING;
        _forceRedraw = true;
        _haveBase = false;
        _haveRate = false;
    }
}

void WidgetNetSpeed::loop()
{
    if (_state != WidgetState::RUNNING || !_display) return;

    sampleRates(); // keep the history continuous regardless of redraw timing

    const uint32_t now = millis();
    if (_forceRedraw || now - _lastDraw >= REDRAW_INTERVAL_MS)
    {
        _lastDraw = now;
        _forceRedraw = false;
        draw();
    }
}

// Read the packet counters, turn the per-window delta into packets/s, push into the history ring.
void WidgetNetSpeed::sampleRates()
{
    const uint32_t now = millis();
    if (now - _lastSample < SAMPLE_INTERVAL_MS) return;
    const uint32_t dtMs = now - _lastSample;
    _lastSample = now;

    uint32_t rxCount = 0, txCount = 0;
#ifdef OPENKNX_NET_HAVE_LAN_TRAFFIC
    openknxLanTraffic(rxCount, txCount); // cumulative packet counters
#endif

    const bool haveBase = _haveBase;
    if (haveBase && dtMs > 0)
    {
        _rxRate = (uint32_t)((uint64_t)(rxCount - _lastRxPackets) * 1000u / dtMs); // uint32 delta wraps safely
        _txRate = (uint32_t)((uint64_t)(txCount - _lastTxPackets) * 1000u / dtMs);
        _haveRate = true;
    }
    _lastRxPackets = rxCount;
    _lastTxPackets = txCount;
    _haveBase = true;
    if (!haveBase) return; // first sample after start/resume only takes the base, it pushes no bar

    _rxHist[_histHead] = (uint16_t)(_rxRate > 0xFFFF ? 0xFFFF : _rxRate);
    _txHist[_histHead] = (uint16_t)(_txRate > 0xFFFF ? 0xFFFF : _txRate);
    _histHead = (_histHead + 1) % SPARK_LEN;
    if (_histCount < SPARK_LEN) _histCount++;
}

// Split a packet rate into value + unit: "42","P/s" or "1.2k","P/s". The magnitude suffix belongs to
// the VALUE (the widget draws value and unit in different sizes), only "P/s" goes into the unit field.
// Uses the shared humanCount() so every counter in this product scales the same way -- the local
// formatter had no M step and printed "1000.0k" past a million.
void WidgetNetSpeed::fmtParts(uint32_t pps, char *val, size_t vN, char *unit, size_t uN)
{
    snprintf(unit, uN, "P/s");
    snprintf(val, vN, "%s", humanCount(pps).c_str());
}

// Small 5x5 up/down triangle at (x,y).
void WidgetNetSpeed::drawArrow(int16_t x, int16_t y, bool up)
{
    if (up)
        _display->display->fillTriangle(x, y + 5, x + 2, y, x + 4, y + 5, WHITE);
    else
        _display->display->fillTriangle(x, y, x + 2, y + 5, x + 4, y, WHITE);
}

// One rate inside [x, xEnd): value in size 2 while value and unit fit, else size 1; "-" until the first rate.
void WidgetNetSpeed::drawRate(int16_t x, int16_t xEnd, uint32_t pps)
{
    auto *g = _display->display;
    char val[12], unit[8];
    fmtParts(pps, val, sizeof(val), unit, sizeof(unit));
    if (!_haveRate) snprintf(val, sizeof(val), "-");
    const int16_t len = (int16_t)strlen(val);
    const uint8_t size = (x + len * 12 + (int16_t)strlen(unit) * 6 <= xEnd) ? 2 : 1;
    g->setTextSize(size);
    g->setCursor(x, size == 2 ? 12 : 16); // 1 px clear of the divider at y=10; size 1 sits on the unit line
    g->print(val);
    g->setTextSize(1);
    g->setCursor(x + len * 6 * size, 16);
    g->print(unit);
}

// Mirror graph in [0,y,W,h]: centre baseline, TX bars up, RX bars down, shared scale, newest at the right.
void WidgetNetSpeed::drawMirror(int16_t y, int16_t h, uint32_t scale)
{
    auto *g = _display->display;
    const int16_t W = (int16_t)_display->GetDisplayWidth();
    const int16_t mid = y + h / 2;
    const int16_t half = h / 2 - 1;
    if (scale == 0) scale = 1;

    for (int16_t i = 0; i < W; i += 3) g->drawPixel(i, mid, WHITE); // faint baseline (dotted)

    for (int16_t i = 0; i < W && i < SPARK_LEN; i++)
    {
        int idx = (_histHead - 1 - i + SPARK_LEN) % SPARK_LEN;
        const int16_t x = W - 1 - i;
        int16_t tb = (int16_t)((uint64_t)_txHist[idx] * half / scale);
        int16_t rb = (int16_t)((uint64_t)_rxHist[idx] * half / scale);
        if (tb > half) tb = half;
        if (rb > half) rb = half;
        if (tb > 0) g->drawFastVLine(x, mid - tb, tb, WHITE);
        if (rb > 0) g->drawFastVLine(x, mid + 1, rb, WHITE);
    }
}

void WidgetNetSpeed::draw()
{
    auto *g = _display->display;
    const int16_t W = (int16_t)_display->GetDisplayWidth();
    g->clearDisplay();
    g->setTextColor(WHITE);

#ifndef WIDGET_NETSPEED_HAVE_COUNTERS
    // No packet counter in this build (WiFi): show a short note instead of a permanently-zero graph, and
    // log it once -- only while the widget is actually on screen.
    static bool warnedNoCounters = false;
    if (!warnedNoCounters)
    {
        warnedNoCounters = true;
        logInfoP("net-speed: this build has no packet counter");
    }
    g->setTextSize(1);
    g->setCursor(12, 0);
    g->print("NETWORK");
    g->drawLine(0, 10, W, 10, WHITE);
    g->setCursor(0, 16);
    g->print("Speed: n/a");
    _display->displayBuff();
    return;
#endif

    // shared scale = running max of either direction
    uint32_t scale = 1;
    for (uint8_t i = 0; i < _histCount; i++)
    {
        if (_rxHist[i] > scale) scale = _rxHist[i];
        if (_txHist[i] > scale) scale = _txHist[i];
    }

    // header: 3-bar icon + title + divider
    g->fillRect(1, 3, 2, 4, WHITE);
    g->fillRect(4, 1, 2, 6, WHITE);
    g->fillRect(7, 4, 2, 3, WHITE);
    g->setTextSize(1);
    g->setCursor(12, 0); // left, right after the 3-bar icon (x 1..8)
    g->print("NETWORK");
    g->drawLine(0, 10, W, 10, WHITE);

    // two big values: TX (up) left up to the RX arrow, RX (down) right up to the edge
    drawArrow(2, 14, true);
    drawRate(8, 66, _txRate);
    drawArrow(66, 14, false);
    drawRate(72, W, _rxRate);

    // mirror history + peak label
    drawMirror(32, 32, scale);
    g->setTextSize(1);
    char pk[12], pu[8];
    fmtParts(scale, pk, sizeof(pk), pu, sizeof(pu));
    // clear behind the peak label: it is drawn over the mirrored bars and was white on white
    g->fillRect(0, 31, (int16_t)strlen(pk) * 6 + 2, 9, BLACK);
    g->setCursor(1, 32);
    g->print(pk);

    _display->displayBuff();
}

#endif // DEVICE_DISPLAY_MODULE
