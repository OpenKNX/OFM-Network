#ifdef DEVICE_DISPLAY_MODULE

#include "WidgetNetInfo.h"
#include "DisplayUi.h"
#include "Icons/UiIcons.h"
#include "OpenKNX.h"
#include "OpenKNX/Network/Module.h"

namespace Ui = OpenKNX::Ui;

namespace
{
#ifndef KNX_IP_WIFI
    /** @brief Card symbol: RJ45 socket seen from the front (body, 8 contacts, latch slot) in x46..77 y3..24. */
    void drawJack(Adafruit_SSD1306 &d)
    {
        d.drawRect(46, 3, 32, 17, WHITE);
        d.drawFastHLine(56, 19, 12, BLACK); // opening of the latch slot
        d.drawFastVLine(56, 19, 6, WHITE);
        d.drawFastVLine(67, 19, 6, WHITE);
        d.drawFastHLine(56, 24, 12, WHITE);
        for (int16_t x = 51; x <= 72; x += 3)
            d.drawFastVLine(x, 6, 5, WHITE);
    }
#endif

    /** @brief "DHCP", "FIX" or "AP": how the current address was assigned. */
    const char *method()
    {
        return openknxNetwork.wifiApActive() ? "AP" : (openknxNetwork.localUsesStaticIp() ? "FIX" : "DHCP");
    }

    void formatIp(char (&ip)[16])
    {
        const IPAddress a = openknxNetwork.localIP();
        snprintf(ip, sizeof(ip), "%u.%u.%u.%u", (unsigned)a[0], (unsigned)a[1], (unsigned)a[2], (unsigned)a[3]);
    }
} // namespace

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

void WidgetNetInfo::draw()
{
    Adafruit_SSD1306 &d = *_display->display;
    Ui::begin(d);
    d.clearDisplay();
    // phyMode() is a stub, so established() (connected AND a non-zero IP) is the link indicator.
    const bool up = openknxNetwork.established();
#ifdef KNX_IP_WIFI
    drawDetails(d, up);
#else
    if (_page == 0)
        drawCard(d, up);
    else
        drawDetails(d, up);
#endif
    _display->displayBuff();
}

#ifndef KNX_IP_WIFI
/** @brief Page 0: RJ45 socket, badge, "Online" with the address and how it was assigned, or the outage. */
void WidgetNetInfo::drawCard(Adafruit_SSD1306 &d, bool up)
{
    char b[24];
    if (up)
    {
        char ip[16];
        formatIp(ip);
        snprintf(b, sizeof(b), "%s %s", ip, method()); // 15 + 1 + 4 = 20 characters at most
        Ui::statusCard(d, drawJack, Ui::kBadgeOk, "Online", b);
    }
    else
        Ui::statusCard(d, drawJack, Ui::kBadgeError, "No LAN", "Check cable");
    Ui::pageDots(d, 2, 0);
}
#endif

/** @brief Page 1: host name, IP, link and link age as icon rows; the assignment method sits in the title. */
void WidgetNetInfo::drawDetails(Adafruit_SSD1306 &d, bool up)
{
    // A 15-character IP keeps its row because the method is in the title. Showing an IP while the
    // link is down would only ever be a stale one.
    const char *title = _name.c_str();
    char b[24];
    if (up)
    {
        snprintf(b, sizeof(b), "%s %s", title, method());
        title = b;
    }
    Ui::header(d, title, PAGES, PAGES - 1);

    // No "Host" label: from 15 characters on it would be cut to a stub; the icon stands for it.
    Ui::rowIcon(d, 0, Ui::kRowInfo, openknxNetwork.hostName());

    char ip[16];
    if (up)
        formatIp(ip);
    else
        snprintf(ip, sizeof(ip), "-"); // ASCII placeholder (SSD1306 CP437 has no em dash)
#ifdef KNX_IP_WIFI
    Ui::rowIcon(d, 1, Ui::kRowWifi, "IP", ip);
#else
    Ui::rowIcon(d, 1, Ui::kRowLan, "IP", ip);
#endif
    Ui::rowIcon(d, 2, up ? Ui::kRowOk : Ui::kRowErr, "Link", up ? "up" : "down");

    // Age of the current link, or of the ongoing outage. Built on uptime() seconds, so it survives
    // the 49.7-day millis() rollover. Short labels: "9999d 23:59:59" leaves 4 columns for the text.
    const uint32_t netUp = openknxNetwork.netUptimeSec();
    const uint32_t netDown = openknxNetwork.netDowntimeSec();
    if (netUp > 0)
        Ui::rowIcon(d, 3, Ui::kRowClock, "Up", humanDuration(netUp).c_str());
    else if (netDown > 0)
        Ui::rowIcon(d, 3, Ui::kRowClock, "Down", humanDuration(netDown).c_str());
    else
        Ui::rowIcon(d, 3, Ui::kRowClock, "Up", "-");
}

#endif // DEVICE_DISPLAY_MODULE
