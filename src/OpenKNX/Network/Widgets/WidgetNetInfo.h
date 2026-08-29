#ifdef DEVICE_DISPLAY_MODULE

#pragma once
#include "Widget.h"
#include <cstdint>
#include <string>

/**
 * @brief Network identity widget (single page): host name, IP with its assignment method, link, link age.
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
    std::string _name = "Netzwerk";

    static constexpr uint32_t REDRAW_INTERVAL_MS = 500;
    uint32_t _lastDraw = 0;

    void draw();
    void drawKeyValueRow(uint8_t row, const char *key, const std::string &value);
};

#endif // DEVICE_DISPLAY_MODULE
