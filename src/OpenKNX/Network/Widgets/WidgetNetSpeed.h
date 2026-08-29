#ifdef DEVICE_DISPLAY_MODULE

#pragma once
#include "Widget.h"
#include <cstdint>
#include <string>

/**
 * @brief LAN throughput widget (single page): live TX/RX rate + history sparklines.
 *
 * Total Ethernet traffic in bytes/s, from lwIP's netif byte counters (ifin/outoctets, needs
 * -D MIB2_STATS=1). "Big read-out": large TX (up) and RX (down) numbers, each with a sparkline.
 * Auto unit B/s -> KB/s -> MB/s. Read-only; the counters live in lwIP, this widget only samples them.
 */
class WidgetNetSpeed : public Widget
{
  public:
    const std::string logPrefix() { return "netspd"; }
    WidgetNetSpeed(uint32_t displayTime, WidgetFlags action);

    void start() override;
    void stop() override;
    void pause() override;
    void resume() override;
    void setup() override;
    void loop() override;

    inline const WidgetState getState() const override { return _state; }
    inline const std::string getName() const override { return _name; }
    inline void setName(const std::string &name) override { _name = name; }

    uint32_t getDisplayTime() const override { return _displayTime; }
    WidgetFlags getAction() const override { return _action; }

    inline void setDisplayTime(uint32_t displayTime) override { _displayTime = displayTime; }
    inline void setAction(uint8_t action) override { _action = static_cast<WidgetFlags>(action); }
    inline void addAction(uint8_t action) override { _action = static_cast<WidgetFlags>(_action | action); }
    inline void removeAction(uint8_t action) override { _action = static_cast<WidgetFlags>(_action & ~action); }

    void setDisplayModule(i2cDisplay *displayModule) override { _display = displayModule; }
    i2cDisplay *getDisplayModule() const override { return _display; }

    // Single page.
    size_t getPageCount() const override { return 1; }
    size_t getCurrentPage() const override { return 0; }
    void setPage(size_t) override {}
    void nextPage() override {}
    void prevPage() override {}

  private:
    WidgetState _state;
    uint32_t _displayTime;
    WidgetFlags _action;
    i2cDisplay *_display;
    std::string _name = "LAN-Speed";

    static constexpr uint32_t REDRAW_INTERVAL_MS = 500;
    static constexpr uint32_t SAMPLE_INTERVAL_MS = 1000; // rate window
    static constexpr uint8_t SPARK_LEN = 126;            // one per display column
    uint32_t _lastDraw = 0;
    bool _forceRedraw = true;

    // rate sampling from the lwIP byte counters (uint32 deltas wrap safely)
    uint32_t _lastSample = 0;
    uint32_t _lastRxBytes = 0, _lastTxBytes = 0;
    uint32_t _rxRate = 0, _txRate = 0; // bytes/s
    bool _haveBase = false;

    // history ring (bytes/s), auto-scaled at draw time
    uint16_t _rxHist[SPARK_LEN] = {0};
    uint16_t _txHist[SPARK_LEN] = {0};
    uint8_t _histHead = 0;
    uint8_t _histCount = 0;

    void sampleRates();
    void draw();
    void drawMirror(int16_t y, int16_t h, uint32_t scale);    // TX up / RX down from a centre baseline
    void drawArrow(int16_t x, int16_t y, bool up);            // small up/down triangle
    static void fmtParts(uint32_t rate, char *val, size_t vN, char *unit, size_t uN);
};

#endif // DEVICE_DISPLAY_MODULE
