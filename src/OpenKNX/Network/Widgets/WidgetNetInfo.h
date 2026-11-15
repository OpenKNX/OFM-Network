#ifdef DEVICE_DISPLAY_MODULE

#pragma once
#include "Widget.h"
#include <cstdint>
#include <string>

/**
 * @brief Network identity widget. LAN: card with the link state on page 0, details on page 1.
 * WiFi: details only. With OPENKNX_WIFI_AP this widget is not registered; the WiFi widget carries card and values.
 *
 * Lives here rather than in the system-info widget of OGM-Common: the values belong to this module,
 * and Common must not reach into it.
 */
class WidgetNetInfo : public Widget
{
  public:
    const std::string logPrefix() { return "netinfo"; }
    WidgetNetInfo(uint32_t displayTime, WidgetFlags action);

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

    // LAN: page 0 card, page 1 details; the WidgetsManager flips them.
#ifdef KNX_IP_WIFI
    static constexpr uint8_t PAGES = 1;
#else
    static constexpr uint8_t PAGES = 2;
#endif
    size_t getPageCount() const override { return PAGES; }
    size_t getCurrentPage() const override { return _page; }
    void setPage(size_t page) override
    {
        page %= PAGES;
        if (page != _page) _lastDraw = millis() - REDRAW_INTERVAL_MS; // redraw at once
        _page = (uint8_t)page;
    }
    void nextPage() override { setPage(_page + 1); }
    void prevPage() override { setPage(_page + PAGES - 1); }

  private:
    WidgetState _state;
    uint32_t _displayTime;
    WidgetFlags _action;
    i2cDisplay *_display;
    std::string _name = "Network";

    static constexpr uint32_t REDRAW_INTERVAL_MS = 500;
    uint32_t _lastDraw = 0;
    uint8_t _page = 0;

    void draw();
#ifndef KNX_IP_WIFI
    void drawCard(Adafruit_SSD1306 &d, bool up);
#endif
    void drawDetails(Adafruit_SSD1306 &d, bool up);
};

#endif // DEVICE_DISPLAY_MODULE
