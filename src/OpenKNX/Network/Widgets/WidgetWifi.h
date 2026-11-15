#if defined(DEVICE_DISPLAY_MODULE) && defined(OPENKNX_WIFI_AP)

#pragma once
#include "Widget.h"
#include <cstdint>
#include <string>

/**
 * @brief WiFi widget. Client: card, signal page with history, details page, network page (host, link, up time);
 *        offline: card + network page; setup AP: QR code to join + info page; own AP: card + values page (name,
 *        security, channel, address, stations). Page count follows the state.
 */
class WidgetWifi : public Widget
{
  public:
    const std::string logPrefix() { return "wifi"; }
    WidgetWifi(uint32_t displayTime, WidgetFlags action);

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

    size_t getPageCount() const override;
    size_t getCurrentPage() const override;
    void setPage(size_t page) override;
    void nextPage() override { setPage(getCurrentPage() + 1); }
    void prevPage() override { setPage(getCurrentPage() + getPageCount() - 1); }

  private:
    WidgetState _state;
    uint32_t _displayTime;
    WidgetFlags _action;
    i2cDisplay *_display;
    std::string _name = "WiFi";

    static constexpr uint32_t REDRAW_INTERVAL_MS = 500;
    static constexpr uint8_t QR_VERSION_MAX = 3; // 29 modules: 2 px each fit the 64 px height
    uint32_t _lastDraw = 0;
    size_t _page = 0;

    uint8_t _qr[((QR_VERSION_MAX * 4 + 17) * (QR_VERSION_MAX * 4 + 17) + 7) / 8 + 1] = {};
    std::string _qrFor; // SSID the cached QR code was built for
    bool _qrOk = false;
    uint8_t _shownState = 0xFF; // WifiState of the last draw or setPage(), 0xFF = none

    void draw();
    void drawSetupQr(Adafruit_SSD1306 &d, const char *ssid, uint8_t clients);
    bool buildQr(const char *ssid);
};

#endif
