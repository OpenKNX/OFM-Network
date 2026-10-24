#if defined(DEVICE_DISPLAY_MODULE) && defined(OPENKNX_WIFI_AP)

#include "WidgetWifi.h"
#include "OpenKNX.h"
#include "OpenKNX/Network/Module.h"
#include "Widgets/QRCode.h" // OFM-DeviceDisplay: drawQr()
#include "qrcodegen.h"

namespace
{
    using State = OpenKNX::Network::Module::WifiState;

    const char *securityText(uint8_t sec)
    {
        static const char *const names[] = {"open", "WEP", "WPA", "WPA2", "WPA/WPA2", "WPA3", "WPA2/WPA3", "Enterprise"};
        return sec < 8 ? names[sec] : "";
    }

    // Same thresholds as the ETS and the web page
    uint8_t signalLevel(int8_t rssi) { return rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : 1; }
} // namespace

WidgetWifi::WidgetWifi(uint32_t displayTime, WidgetFlags action)
    : _state(WidgetState::STOPPED), _displayTime(displayTime), _action(action), _display(nullptr)
{
}

void WidgetWifi::setup() {}
void WidgetWifi::start()
{
    _state = WidgetState::RUNNING;
    _lastDraw = 0;
}
void WidgetWifi::stop() { _state = WidgetState::STOPPED; }
void WidgetWifi::pause() { _state = WidgetState::PAUSED; }
void WidgetWifi::resume()
{
    _state = WidgetState::RUNNING;
    _lastDraw = 0;
}

size_t WidgetWifi::getPageCount() const
{
    return openknxNetwork.wifiInfo().state == State::SetupAp ? 2 : 1;
}

size_t WidgetWifi::getCurrentPage() const
{
    return _page < getPageCount() ? _page : 0;
}

void WidgetWifi::setPage(size_t page)
{
    _page = page % getPageCount();
    _lastDraw = 0;
}

void WidgetWifi::loop()
{
    if (_state != WidgetState::RUNNING || _display == nullptr) return;
    const uint32_t now = millis();
    if (_lastDraw != 0 && now - _lastDraw < REDRAW_INTERVAL_MS) return;
    _lastDraw = now | 1;
    draw();
}

void WidgetWifi::drawHeader(const char *title)
{
    const int16_t W = (int16_t)_display->GetDisplayWidth();
    _display->display->clearDisplay();
    _display->display->setTextColor(WHITE);
    _display->display->setTextSize(1);
    _display->display->setCursor(0, 0);
    _display->display->print(title);
    _display->display->drawLine(0, 10, W, 10, WHITE);

    const size_t pages = getPageCount();
    if (pages < 2) return;
    // One dot per page, the current one filled; clear of the manager's corner at x >= W-12 (as WidgetIPRouter).
    const int16_t startX = W - 12 - (int16_t)pages * 6;
    for (size_t i = 0; i < pages; i++)
    {
        const int16_t x = startX + (int16_t)i * 6 + 2;
        if (i == getCurrentPage())
            _display->display->fillCircle(x, 4, 2, WHITE);
        else
            _display->display->drawCircle(x, 4, 1, WHITE);
    }
}

void WidgetWifi::drawCentered(int16_t x0, int16_t x1, int16_t y, const char *text)
{
    int16_t bx, by;
    uint16_t bw, bh;
    _display->display->getTextBounds(text, 0, 0, &bx, &by, &bw, &bh);
    const int16_t x = x0 + ((x1 - x0) - (int16_t)bw) / 2;
    _display->display->setCursor(x < x0 ? x0 : x, y);
    _display->display->print(text);
}

void WidgetWifi::drawCentered(int16_t y, const char *text)
{
    drawCentered(0, (int16_t)_display->GetDisplayWidth(), y, text);
}

void WidgetWifi::draw()
{
    _display->display->setTextWrap(false); // a 24-char host name is wider than 21 columns: clip, do not wrap into the next row
    switch (openknxNetwork.wifiInfo().state)
    {
        case State::SetupAp:
            if (getCurrentPage() == 0)
                drawSetupQr();
            else
                drawSetupInfo();
            break;
        case State::OwnAp:
            drawOwnAp();
            break;
        default:
            drawClient();
            break;
    }
    _display->displayBuff();
}

void WidgetWifi::drawClient()
{
    const auto info = openknxNetwork.wifiInfo();
    drawHeader(_name.c_str());
    drawCentered(15, info.ssid);
    if (info.state != State::Client)
    {
        drawCentered(33, "not connected");
        drawCentered(45, info.ssid[0] ? "retrying" : "no credentials");
        return;
    }

    char line[24];
    static const char *const levels[] = {"", "weak", "fair", "good", "very good"};
    const uint8_t level = signalLevel(info.rssi);
    snprintf(line, sizeof(line), "%d dBm %s", (int)info.rssi, levels[level]);
    int16_t bx, by;
    uint16_t bw, bh;
    _display->display->getTextBounds(line, 0, 0, &bx, &by, &bw, &bh);
    const int16_t barsW = 4 * 3 - 1;
    const int16_t x = ((int16_t)_display->GetDisplayWidth() - barsW - 4 - (int16_t)bw) / 2;
    for (uint8_t i = 0; i < 4; i++)
    {
        const int16_t h = 2 + i * 2;
        if (i < level)
            _display->display->fillRect(x + i * 3, 34 - h, 2, h, WHITE);
        else
            _display->display->drawFastHLine(x + i * 3, 33, 2, WHITE);
    }
    _display->display->setCursor(x + barsW + 4, 27);
    _display->display->print(line);

    snprintf(line, sizeof(line), "Ch %u   %s", (unsigned)info.channel, securityText(info.security));
    drawCentered(39, line);
    snprintf(line, sizeof(line), "IP %s", openknxNetwork.localIP().toString().c_str());
    drawCentered(51, line);
}

/**
 * @brief QR code of the open setup AP ("WIFI:" scheme, no password), built once per SSID.
 */
bool WidgetWifi::buildQr(const char *ssid)
{
    if (_qrFor == ssid) return _qrOk;
    _qrFor = ssid;
    std::string text = "WIFI:T:nopass;S:";
    for (const char *p = ssid; *p; p++)
    {
        if (*p == '\\' || *p == ';' || *p == ',' || *p == ':' || *p == '"') text += '\\';
        text += *p;
    }
    text += ";;";
    uint8_t temp[sizeof(_qr)];
    _qrOk = qrcodegen_encodeText(text.c_str(), temp, _qr, qrcodegen_Ecc_LOW, 1, QR_VERSION_MAX, OPENKNX_QR_MASK, true);
    return _qrOk;
}

void WidgetWifi::drawSetupQr()
{
    const auto info = openknxNetwork.wifiInfo();
    const int16_t H = (int16_t)_display->GetDisplayHeight();
    _display->display->clearDisplay();
    if (!buildQr(info.ssid))
    {
        drawHeader("WiFi Setup");
        drawCentered(27, "Join WiFi");
        drawCentered(39, info.ssid);
        return;
    }

    // QR as large as the height allows, left square: dark modules on a white field with a 3 px quiet zone
    const int16_t box = WidgetQRCode::drawQr(_display, _qr, 0, 0, H, H, 3, true);

    const int16_t W = (int16_t)_display->GetDisplayWidth();
    _display->display->setTextColor(WHITE);
    _display->display->setTextSize(1);
    drawCentered(box, W, 22, "Scan to");
    drawCentered(box, W, 33, "config");

    // page dots, as in drawHeader()
    const int16_t startX = W - 12 - 2 * 6;
    _display->display->fillCircle(startX + 2, 4, 2, WHITE);
    _display->display->drawCircle(startX + 8, 4, 1, WHITE);
}

void WidgetWifi::drawSetupInfo()
{
    const auto info = openknxNetwork.wifiInfo();
    drawHeader("WiFi Setup");
    drawCentered(15, info.ssid);
    char line[32];
    snprintf(line, sizeof(line), "Open %s/wifi", openknxNetwork.localIP().toString().c_str());
    drawCentered(27, line);
    snprintf(line, sizeof(line), "Clients: %u", (unsigned)info.clients);
    drawCentered(39, line);
    drawCentered(51, "open network");
}

void WidgetWifi::drawOwnAp()
{
    const auto info = openknxNetwork.wifiInfo();
    drawHeader("WiFi AP");
    drawCentered(15, info.ssid);
    char line[24];
    snprintf(line, sizeof(line), "%s   Ch %u", securityText(info.security), (unsigned)info.channel);
    drawCentered(27, line);
    snprintf(line, sizeof(line), "IP %s", openknxNetwork.localIP().toString().c_str());
    drawCentered(39, line);
    snprintf(line, sizeof(line), "Clients: %u", (unsigned)info.clients);
    drawCentered(51, line);
}

#endif
