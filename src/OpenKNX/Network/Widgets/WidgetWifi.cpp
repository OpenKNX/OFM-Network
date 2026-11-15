#if defined(DEVICE_DISPLAY_MODULE) && defined(OPENKNX_WIFI_AP)

#include "WidgetWifi.h"
#include "DisplayUi.h"
#include "Icons/UiIcons.h"
#include "OpenKNX.h"
#include "OpenKNX/Network/Module.h"
#include "Widgets/QRCode.h" // OFM-DeviceDisplay: drawQr()
#include "qrcodegen.h"
#include <string.h>

namespace
{
    namespace Ui = OpenKNX::Ui;
    using State = OpenKNX::Network::Module::WifiState;
    using Info = OpenKNX::Network::Module::WifiInfo;

    const char *securityText(uint8_t sec)
    {
        static const char *const names[] = {"open", "WEP", "WPA", "WPA2", "WPA/WPA2", "WPA3", "WPA2/WPA3", "Enterprise"};
        return sec < 8 ? names[sec] : "-";
    }

    // Same thresholds as the ETS and the web page
    uint8_t signalLevel(int8_t rssi) { return rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : 1; }

    // Client: card, signal, details, network; offline: card, network; the APs: two pages each.
    uint8_t pageCount(State state) { return state == State::Client ? 4 : 2; }

    // Card symbol arcs, 35x21 at (47,3); the dot below them is a filled circle.
    const uint8_t kWifiArcs[] PROGMEM = {
        0x00, 0x0F, 0xFE, 0x00, 0x00, 0x00, 0x7F, 0xFF, 0xC0, 0x00, 0x01, 0xB8, 0x03, 0xB0, 0x00,
        0x07, 0xC0, 0x00, 0x7C, 0x00, 0x1F, 0x00, 0x00, 0x1F, 0x00, 0x38, 0x00, 0x00, 0x03, 0x80,
        0x50, 0x07, 0xFC, 0x01, 0x40, 0xE0, 0x1F, 0xFF, 0x00, 0xE0, 0x40, 0xFC, 0x07, 0xE0, 0x40,
        0x03, 0xC0, 0x00, 0x78, 0x00, 0x03, 0x80, 0x00, 0x38, 0x00, 0x0E, 0x00, 0x00, 0x0E, 0x00,
        0x04, 0x03, 0xF8, 0x04, 0x00, 0x00, 0x0F, 0xFE, 0x00, 0x00, 0x00, 0x3C, 0x07, 0x80, 0x00,
        0x00, 0x50, 0x01, 0x40, 0x00, 0x00, 0x60, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x01, 0xB0, 0x00, 0x00, 0x00, 0x02, 0x08, 0x00, 0x00, 0x00, 0x04, 0x04, 0x00, 0x00,
    };

    void drawWifiSymbol(Adafruit_SSD1306 &d)
    {
        d.drawBitmap(47, 3, kWifiArcs, 35, 21, WHITE);
        d.fillCircle(64, 26, 2, WHITE);
    }

    // AP card symbol: arc ends above the antenna, 12x4 at (64,0); the rest of the arcs lies above the screen.
    const uint8_t kApArcs[] PROGMEM = {0xC0, 0x10, 0x00, 0x00, 0x07, 0x00, 0x18, 0xC0};

    void drawApSymbol(Adafruit_SSD1306 &d)
    {
        d.drawRoundRect(50, 14, 28, 12, 2, WHITE); // router box with two LEDs
        d.fillRect(54, 18, 2, 2, WHITE);
        d.fillRect(58, 18, 2, 2, WHITE);
        d.drawFastVLine(70, 3, 11, WHITE); // antenna
        d.drawBitmap(64, 0, kApArcs, 12, 4, WHITE);
        d.fillCircle(70, 5, 1, WHITE);
    }

    // sin of 0..90 degrees in 2 degree steps, Q14
    const int16_t kSinQ14[46] PROGMEM = {
        0,     572,   1143,  1713,  2280,  2845,  3406,  3964,  4516,  5063,  5604,  6138,  6664,  7182,  7692,  8192,
        8682,  9162,  9630,  10087, 10531, 10963, 11381, 11786, 12176, 12551, 12911, 13255, 13583, 13894, 14189, 14466,
        14726, 14968, 15191, 15396, 15582, 15749, 15897, 16026, 16135, 16225, 16294, 16344, 16374, 16384,
    };

    /** @brief sin(deg) in Q14 for deg = 0, 2, ..., 358. */
    int32_t sinQ14(uint16_t deg)
    {
        const uint16_t b = deg % 90;
        const int32_t v = (int32_t)pgm_read_word(&kSinQ14[(deg / 90) % 2 ? (90 - b) / 2 : b / 2]);
        return deg >= 180 ? -v : v;
    }

    /** @brief r * sin(deg) rounded to the nearest pixel, halves away from zero. */
    int16_t polar(int16_t r, uint16_t deg)
    {
        const int32_t v = r * sinQ14(deg);
        return (int16_t)(v >= 0 ? (v + 8192) >> 14 : -((-v + 8192) >> 14));
    }

    /**
     * @brief Countdown ring at (cx,cy): from 12 o'clock clockwise 2 px thick for the part left, dotted every 12 degrees
     * for the part gone. left <= span, both in the same unit.
     */
    void drawRing(Adafruit_SSD1306 &d, int16_t cx, int16_t cy, int16_t r, uint32_t left, uint32_t span)
    {
        for (uint16_t a = 0; a < 360; a += 2)
        {
            const uint16_t c = (uint16_t)((a + 270) % 360); // y grows downwards: -r cos(a) = r sin(a + 270)
            if ((uint64_t)a * span <= (uint64_t)360 * left)
            {
                d.drawPixel(cx + polar(r, a), cy + polar(r, c), WHITE);
                d.drawPixel(cx + polar(r - 1, a), cy + polar(r - 1, c), WHITE);
            }
            else if (a % 12 == 0)
                d.drawPixel(cx + polar(r, a), cy + polar(r, c), WHITE);
        }
    }

    // Signal graph: -90..-40 dBm on 0..1000, values on y34..62, baseline y63, newest at x126.
    constexpr int16_t RssiFloor = -90, RssiRange = 50, RssiLimit = -75;
    constexpr int16_t GraphY = 34, GraphH = 29;

    uint16_t rssiPermille(int16_t rssi)
    {
        const int16_t v = (int16_t)((rssi - RssiFloor) * 1000 / RssiRange);
        return v < 0 ? 0 : v > 1000 ? 1000 : (uint16_t)v;
    }

    /** @brief Graph reader over the module's RSSI history; 0 dBm (no connection) is a gap. */
    uint16_t rssiAt(void *, uint8_t index)
    {
        const int8_t rssi = openknxNetwork.wifiRssiHistAt(index);
        return rssi == 0 ? Ui::GraphGap : rssiPermille(rssi);
    }

    /** @brief Dotted local IPv4 into b without the heap String of IPAddress::toString(). */
    const char *localIp(char (&b)[16])
    {
        const IPAddress a = openknxNetwork.localIP();
        snprintf(b, sizeof(b), "%u.%u.%u.%u", (unsigned)a[0], (unsigned)a[1], (unsigned)a[2], (unsigned)a[3]);
        return b;
    }

    const char *clientsText(char (&b)[12], uint8_t clients)
    {
        snprintf(b, sizeof(b), "%u %s", (unsigned)clients, clients == 1 ? "client" : "clients");
        return b;
    }

    /** @brief Client or offline card: word, badge and one detail line. */
    void drawCard(Adafruit_SSD1306 &d, const Info &info, uint8_t pages)
    {
        const bool online = info.state == State::Client;
        char b[24]; // the card shows 21 columns; printCentred cuts a longer text the same way
        const char *detail = b;
        if (online && info.rssi)
            snprintf(b, sizeof(b), "%s %d dBm", info.ssid, (int)info.rssi);
        else if (online)
            detail = info.ssid; // no RSSI read yet
        else if (info.ssid[0] && strlen(info.ssid) <= 12)
            snprintf(b, sizeof(b), "retrying %s", info.ssid); // 21 columns
        else if (info.ssid[0])
            detail = info.ssid; // 13+ characters: the SSID alone, from 22 on cut to 18 + "..." by printCentred
        else
            detail = "No credentials"; // size 2 holds 10 characters: the word stays "No WiFi"
        Ui::statusCard(d, drawWifiSymbol, online ? Ui::kBadgeOk : Ui::kBadgeError, online ? "Online" : "No WiFi", detail);
        Ui::pageDots(d, pages, 0);
    }

    /** @brief Expert page: RSSI large, level bars, 10 min history with the -75 dBm limit. */
    void drawSignal(Adafruit_SSD1306 &d, const Info &info, uint8_t pages)
    {
        Ui::header(d, "WiFi", pages, 1);
        char b[6];
        if (info.rssi)
            snprintf(b, sizeof(b), "%d", (int)info.rssi);
        else
            snprintf(b, sizeof(b), "-");
        Ui::bigValue(d, 14, b, "dBm");

        const uint8_t level = info.rssi ? signalLevel(info.rssi) : 0;
        for (uint8_t i = 0; i < 4; i++)
        {
            const int16_t x = 104 + i * 6, h = 3 + i * 3;
            if (i < level)
                d.fillRect(x, 28 - h, 4, h, WHITE);
            else
                d.drawFastHLine(x, 27, 4, WHITE);
        }

        const int16_t W = d.width();
        for (int16_t y = GraphY; y < GraphY + GraphH; y += 3) d.drawPixel(0, y, WHITE); // grid
        const int16_t limitY = (int16_t)(GraphY + GraphH - 1 - rssiPermille(RssiLimit) * (GraphH - 1) / 1000);
        for (int16_t x = 0; x < W; x += 4) d.drawFastHLine(x, limitY, 2, WHITE); // limit, 2 on 2 off
        Ui::graph(d, 0, GraphY, W - 1, GraphH, 2, openknxNetwork.wifiRssiHistCount(), rssiAt, nullptr);

        // 60 samples of 10 s (WifiSetup.cpp WIFI_RSSI_SAMPLE_MS, Module.h WIFI_HIST_LEN)
        d.fillRect(W - 38, GraphY - 1, 38, 9, BLACK);
        d.setCursor(W - 37, GraphY);
        d.print("10 min");
    }

    /** @brief Expert page: network, level word, channel, security, address. */
    void drawDetails(Adafruit_SSD1306 &d, const Info &info, uint8_t pages)
    {
        static const char *const levels[] = {"-", "weak", "fair", "good", "very good"};
        Ui::header(d, openknxNetwork.localUsesStaticIp() ? "WiFi FIX" : "WiFi DHCP", pages, 2);
        Ui::rowIcon(d, 0, Ui::kRowWifi, info.ssid);
        Ui::rowIcon(d, 1, Ui::kRowInfo, "Signal", levels[info.rssi ? signalLevel(info.rssi) : 0]);
        char b[16];
        snprintf(b, sizeof(b), "%u", (unsigned)info.channel);
        Ui::rowIcon(d, 2, Ui::kRowInfo, "Channel", b);
        Ui::rowIcon(d, 3, Ui::kRowLock, "Security", securityText(info.security));
        Ui::rowIcon(d, 4, Ui::kRowLan, "IP", localIp(b));
    }

    /** @brief Last page in client and offline state: host name, link and how long it has been up or down. */
    void drawNetwork(Adafruit_SSD1306 &d, uint8_t pages, uint8_t page)
    {
        Ui::header(d, "Network", pages, page);
        // No "Host" label: from 15 characters on it would be cut to a stub; the icon stands for it.
        Ui::rowIcon(d, 0, Ui::kRowInfo, openknxNetwork.hostName());
        const bool up = openknxNetwork.established();
        Ui::rowIcon(d, 1, up ? Ui::kRowOk : Ui::kRowErr, "Link", up ? "up" : "down");
        // Built on uptime() seconds, so it survives the millis() rollover; "9999d 23:59:59" leaves 4 label columns.
        const uint32_t netUp = openknxNetwork.netUptimeSec();
        const uint32_t netDown = openknxNetwork.netDowntimeSec();
        if (netUp > 0)
            Ui::rowIcon(d, 2, Ui::kRowClock, "Up", humanDuration(netUp).c_str());
        else if (netDown > 0)
            Ui::rowIcon(d, 2, Ui::kRowClock, "Down", humanDuration(netDown).c_str());
        else
            Ui::rowIcon(d, 2, Ui::kRowClock, "Up", "-");
    }

    /** @brief Setup AP, page 2: where to open the setup page. */
    void drawSetupInfo(Adafruit_SSD1306 &d, const Info &info)
    {
        Ui::header(d, "WiFi Setup", 2, 1);
        Ui::printCentred(d, 14, info.ssid);
        Ui::printCentred(d, 24, "Open in browser");
        char ip[16], b[24];
        snprintf(b, sizeof(b), "%s/wifi", localIp(ip));
        Ui::printCentred(d, 34, b);
        char c[12];
        Ui::printCentred(d, 44, clientsText(c, info.clients));
        Ui::printCentred(d, 54, "open network");
    }

    /** @brief Own AP, page 1: card with the station count. */
    void drawOwnApCard(Adafruit_SSD1306 &d, const Info &info)
    {
        char c[12];
        Ui::statusCard(d, drawApSymbol, Ui::kBadgeOk, "AP on", clientsText(c, info.clients));
        Ui::pageDots(d, 2, 0);
    }

    /** @brief Own AP, page 2: the AP's values. */
    void drawOwnAp(Adafruit_SSD1306 &d, const Info &info)
    {
        Ui::header(d, "WiFi AP", 2, 1);
        Ui::rowIcon(d, 0, Ui::kRowWifi, info.ssid);
        Ui::rowIcon(d, 1, Ui::kRowLock, "Security", securityText(info.security));
        char b[16];
        snprintf(b, sizeof(b), "%u", (unsigned)info.channel);
        Ui::rowIcon(d, 2, Ui::kRowInfo, "Channel", b);
        Ui::rowIcon(d, 3, Ui::kRowLan, "IP", localIp(b));
        snprintf(b, sizeof(b), "%u", (unsigned)info.clients);
        Ui::rowIcon(d, 4, Ui::kRowInfo, "Clients", b);
    }
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
    return pageCount(openknxNetwork.wifiInfo().state);
}

size_t WidgetWifi::getCurrentPage() const
{
    const State state = openknxNetwork.wifiInfo().state;
    if ((uint8_t)state != _shownState) return 0; // the next draw() starts the new state on its first page
    return _page < pageCount(state) ? _page : 0;
}

void WidgetWifi::setPage(size_t page)
{
    const State state = openknxNetwork.wifiInfo().state;
    _shownState = (uint8_t)state; // a page chosen for this state is kept by draw()
    _page = page % pageCount(state);
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

void WidgetWifi::draw()
{
    Adafruit_SSD1306 &d = *_display->display;
    Ui::begin(d);
    d.clearDisplay();
    const auto info = openknxNetwork.wifiInfo();
    const uint8_t pages = pageCount(info.state);
    if ((uint8_t)info.state != _shownState)
    {
        _shownState = (uint8_t)info.state;
        _page = 0; // state changed since the last setPage() or draw: first page, unless auto paging picks one
    }
    const size_t page = _page < pages ? _page : 0; // the state may have dropped pages since setPage()
    switch (info.state)
    {
        case State::SetupAp:
            if (page == 0)
                drawSetupQr(d, info.ssid, info.clients);
            else
                drawSetupInfo(d, info);
            break;
        case State::OwnAp:
            if (page == 0)
                drawOwnApCard(d, info);
            else
                drawOwnAp(d, info);
            break;
        case State::Offline:
            if (page == 1)
                drawNetwork(d, pages, 1);
            else
                drawCard(d, info, pages);
            break;
        default:
            if (page == 1)
                drawSignal(d, info, pages);
            else if (page == 2)
                drawDetails(d, info, pages);
            else if (page == 3)
                drawNetwork(d, pages, 3);
            else
                drawCard(d, info, pages);
            break;
    }
    _display->displayBuff();
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

/**
 * @brief Setup AP, page 1: QR left, title, hint and station count in the right column.
 */
void WidgetWifi::drawSetupQr(Adafruit_SSD1306 &d, const char *ssid, uint8_t clients)
{
    if (!buildQr(ssid))
    {
        Ui::header(d, "WiFi Setup", 2, 0);
        Ui::printCentred(d, 24, "Join WiFi");
        Ui::printCentred(d, 34, ssid);
        return;
    }

    // QR as large as the height allows, left square: dark modules on a white field with a 3 px quiet zone
    const int16_t H = d.height();
    WidgetQRCode::drawQr(_display, _qr, 0, 0, H, H, 3, true);

    constexpr int16_t X = 68; // right column, clear of the 64 px QR field
    d.setCursor(X, 0);
    d.print("WiFi");
    d.drawFastHLine(X - 2, Ui::HeaderLine, d.width() - (X - 2), WHITE);
    Ui::pageDots(d, 2, 0);
    d.setCursor(X, 14);
    d.print("Scan to");
    d.setCursor(X, 23);
    d.print("set up");
    // Remaining time only while the AP has a deadline; a connected station keeps the AP open.
    uint32_t span = 0;
    const uint32_t left = openknxNetwork.wifiApTimeLeftMs(&span);
    if (clients == 0 && left > 0 && span >= left)
    {
        drawRing(d, 76, 46, 8, left, span);
        char t[12]; // "10:00" at most; sized for any uint32_t
        const uint32_t s = left / 1000;
        snprintf(t, sizeof(t), "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
        d.setCursor(88, 43);
        d.print(t);
    }
    char c[12];
    d.setCursor(X, 56);
    d.print(clientsText(c, clients));
}

#endif
