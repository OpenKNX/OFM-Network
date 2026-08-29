#ifdef DEVICE_DISPLAY_MODULE

#include "WidgetNetInfo.h"
#include "OpenKNX.h"
#include "OpenKNX/Network/Module.h"

WidgetNetInfo::WidgetNetInfo(uint32_t displayTime, WidgetFlags action)
    : _state(WidgetState::STOPPED), _displayTime(displayTime), _action(action), _display(nullptr)
{
}

void WidgetNetInfo::setup() {}
void WidgetNetInfo::start() { _state = WidgetState::RUNNING; }
void WidgetNetInfo::stop() { _state = WidgetState::STOPPED; }
void WidgetNetInfo::pause() { _state = WidgetState::PAUSED; }
void WidgetNetInfo::resume() { _state = WidgetState::RUNNING; }

void WidgetNetInfo::loop()
{
    if (_state != WidgetState::RUNNING || _display == nullptr)
        return;

    const uint32_t now = millis();
    if (now - _lastDraw < REDRAW_INTERVAL_MS)
        return;
    _lastDraw = now;
    draw();
}

void WidgetNetInfo::drawKeyValueRow(uint8_t row, const char *key, const std::string &value)
{
    _display->display->setCursor(0, (int16_t)(15 + row * 12));
    _display->display->print(key);
    _display->display->print(": ");
    _display->display->print(value.c_str());
}

void WidgetNetInfo::draw()
{
    const uint16_t W = _display->GetDisplayWidth();
    _display->display->clearDisplay();
    _display->display->setTextColor(WHITE);
    _display->display->setTextSize(1);
    _display->display->setCursor(0, 0);
    _display->display->print(_name.c_str());
    _display->display->drawLine(0, 10, W, 10, WHITE);

    const std::string dash = "-"; // ASCII placeholder (SSD1306 CP437 has no em dash)
    drawKeyValueRow(0, "Host", std::string(openknxNetwork.hostName()));

    // phyMode() is a stub, so established() (connected AND a non-zero IP) is the link indicator.
    const bool up = openknxNetwork.established();
    drawKeyValueRow(2, "Link", up ? std::string("up") : std::string("down"));

    // The label carries the assignment method, so it costs no extra row. Showing an IP while the
    // link is down would only ever be a stale one.
    if (up)
        drawKeyValueRow(1, openknxNetwork.localUsesStaticIp() ? "FIX" : "DHCP",
                        std::string(openknxNetwork.localIP().toString().c_str()));
    else
        drawKeyValueRow(1, "IP", dash);

    // Age of the current link, or of the ongoing outage. Built on uptime() seconds, so it survives
    // the 49.7-day millis() rollover.
    const uint32_t netUp = openknxNetwork.netUptimeSec();
    const uint32_t netDown = openknxNetwork.netDowntimeSec();
    if (netUp > 0)
        drawKeyValueRow(3, "Net-Up", humanDuration(netUp));
    else if (netDown > 0)
        drawKeyValueRow(3, "Net-Dn", humanDuration(netDown));
    else
        drawKeyValueRow(3, "Net-Up", dash);

    _display->displayBuff();
}

#endif // DEVICE_DISPLAY_MODULE
