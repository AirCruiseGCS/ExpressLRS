#if defined(TWATCH_UI) || defined(TWATCH_UI_HOST)

/*
 * twatch-elrs: drawing of the three watch screens (owner's designs: Design/extracted/01 LINK, 02 RADIO, 03 WATCH).
 *
 * The designs are 240x240 CSS px, so every coordinate below is the design's own (padding 8/9 px, flex gaps, card
 * sizes) worked out from its inline styles. Colours are the designs' hex values in RGB565. Fonts are U8g2 bitmap
 * fonts (no anti-aliasing on this path) picked to match the designs' cap heights: Inter -> Nokia Small / X11
 * Helvetica / haxrcorp4089, JetBrains Mono -> X11 fixed / t0 / Inconsolata Bold, Space Grotesk (clock) -> Logisoso,
 * Cyrillic (with the Ukrainian letters) -> X11 6x12 / 6x13 (U8g2 has no proportional Cyrillic sans closer to Inter).
 *
 * Partial updates: each screen is a list of regions. A region is rendered into a RAM buffer (always - it is cheap)
 * and hashed; it goes to the panel only if the hash differs from the one last sent for that region.
 */

#include "twatch_ui.h"

#include <Arduino_GFX_Library.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

// --- fonts (U8g2's own arrays, linked from the U8g2 library) ---------------------------------------------------------
extern "C" {
extern const uint8_t u8g2_font_NokiaSmallBold_tf[];
extern const uint8_t u8g2_font_NokiaSmallPlain_tf[];
extern const uint8_t u8g2_font_helvB10_tf[];
extern const uint8_t u8g2_font_helvB12_tf[];
extern const uint8_t u8g2_font_5x8_tf[];
extern const uint8_t u8g2_font_6x10_tf[];
extern const uint8_t u8g2_font_7x13B_tf[];
extern const uint8_t u8g2_font_8x13B_tf[];
extern const uint8_t u8g2_font_t0_17b_tf[];
extern const uint8_t u8g2_font_inb30_mn[];
extern const uint8_t u8g2_font_logisoso46_tn[];
extern const uint8_t u8g2_font_haxrcorp4089_tr[];
extern const uint8_t u8g2_font_6x12_t_cyrillic[];
extern const uint8_t u8g2_font_6x13_t_cyrillic[];
}

namespace
{
// Inter 8.5-9 px 600, uppercase labels
const uint8_t *const F_LABEL = u8g2_font_NokiaSmallBold_tf;
// Inter 9 px 500
const uint8_t *const F_SANS9 = u8g2_font_NokiaSmallPlain_tf;
// Inter 10 px 500, hints (Latin; its Cyrillic variant lacks the Ukrainian \u0456)
const uint8_t *const F_HINT = u8g2_font_haxrcorp4089_tr;
// Inter 10 px 500, Cyrillic hints
const uint8_t *const F_HINT_CYR = u8g2_font_6x12_t_cyrillic;
// Inter 11-12 px 600/700
const uint8_t *const F_SANS12B = u8g2_font_helvB10_tf;
// Inter 13 px 700
const uint8_t *const F_SANS13B = u8g2_font_helvB12_tf;
// JetBrains Mono 9 px
const uint8_t *const F_MONO9 = u8g2_font_5x8_tf;
// JetBrains Mono 10 px
const uint8_t *const F_MONO10 = u8g2_font_6x10_tf;
// JetBrains Mono 12-13 px 600
const uint8_t *const F_MONO12B = u8g2_font_7x13B_tf;
// JetBrains Mono 14 px 700
const uint8_t *const F_MONO14B = u8g2_font_8x13B_tf;
// JetBrains Mono 17 px 700
const uint8_t *const F_MONO17B = u8g2_font_t0_17b_tf;
// JetBrains Mono 38 px 700 (digits only; cap height 28 -> Inconsolata Bold 30)
const uint8_t *const F_MONO38B = u8g2_font_inb30_mn;
// Space Grotesk 64 px 700 (digits and ':')
const uint8_t *const F_CLOCK = u8g2_font_logisoso46_tn;
// Inter 12 px 500, Cyrillic date
const uint8_t *const F_DATE = u8g2_font_6x13_t_cyrillic;

constexpr uint16_t rgb(uint32_t c)
{
    return (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
}

// the designs' palette
constexpr uint16_t C_BG = rgb(0x000000);
constexpr uint16_t C_TEXT = rgb(0xE9EEF2);
constexpr uint16_t C_TIME = rgb(0xF4F7F9);
constexpr uint16_t C_MUTED = rgb(0x8B97A2);
constexpr uint16_t C_DIM = rgb(0x5D6873);
constexpr uint16_t C_CARD = rgb(0x0E1114);
constexpr uint16_t C_LINE = rgb(0x1E242A);
constexpr uint16_t C_OFF = rgb(0x2A3138);
constexpr uint16_t C_ACCENT = rgb(0x3FD68A);
constexpr uint16_t C_ON_ACCENT = rgb(0x03140B);
constexpr uint16_t C_AMBER = rgb(0xF2B33D);
constexpr uint16_t C_RED = rgb(0xFF5A5F);
constexpr uint16_t C_BLUE = rgb(0x4DA3FF);
constexpr uint16_t C_ON_BLUE = rgb(0x04121F);
constexpr uint16_t C_ELRS_TILE = rgb(0x0F2A1D);
constexpr uint16_t C_BLE_TILE = rgb(0x0C1E33);
constexpr uint16_t C_BIND_IDLE_BG = rgb(0x071710);
constexpr uint16_t C_BINDING_BG = rgb(0x1D1506);
constexpr uint16_t C_BIND_OFF_BG = rgb(0x0A0C0E);
constexpr uint16_t C_CHIP_BORDER = rgb(0x1F4A33);

const char *const DASH = "\xE2\x80\x94"; // U+2014, drawn as a bar (not in the Latin fonts)

// ---------------------------------------------------------------------------------------------------------------------
// A region-sized canvas over one shared buffer.

constexpr int32_t BUF_PIXELS = 240 * 40; // the largest region: 02's 222x40 cards (buffer 19 KB)
uint16_t g_buf[BUF_PIXELS];

class Surface : public Arduino_GFX
{
public:
    Surface() : Arduino_GFX(240, 40) {}
    void resize(int16_t w, int16_t h)
    {
        WIDTH = w;
        HEIGHT = h;
        setRotation(0); // _width/_height, _max_x/_max_y and the text clip bounds
    }
    bool begin(int32_t) override { return true; }
    void writePixelPreclipped(int16_t x, int16_t y, uint16_t c) override { g_buf[(int32_t)y * _width + x] = c; }
    void writeFastHLine(int16_t x, int16_t y, int16_t w, uint16_t c) override
    {
        if (y < 0 || y > _max_y)
            return;
        if (x < 0) { w += x; x = 0; }
        if (x + w > _width) w = _width - x;
        uint16_t *p = g_buf + (int32_t)y * _width + x;
        while (w-- > 0) *p++ = c;
    }
    void writeFillRectPreclipped(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) override
    {
        for (int16_t i = 0; i < h; ++i)
            writeFastHLine(x, y + i, w, c);
    }
};
Surface g_surface;

// Drawing in screen coordinates into the current region.
struct Pen
{
    int16_t ox = 0, oy = 0;
    Surface &s = g_surface;

    void fill(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) { s.fillRect(x - ox, y - oy, w, h, c); }
    void rrect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t c)
    {
        if (r <= 0) fill(x, y, w, h, c);
        else s.fillRoundRect(x - ox, y - oy, w, h, r, c);
    }
    // a CSS box: border 1 px, background, radius r
    void card(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t bg, uint16_t border)
    {
        rrect(x, y, w, h, r, border);
        rrect(x + 1, y + 1, w - 2, h - 2, r - 1, bg);
    }
    void hline(int16_t x, int16_t y, int16_t w, uint16_t c) { fill(x, y, w, 1, c); }
    void vline(int16_t x, int16_t y, int16_t h, uint16_t c) { fill(x, y, 1, h, c); }
    void disc(int16_t cx, int16_t cy, int16_t r, uint16_t c) { s.fillCircle(cx - ox, cy - oy, r, c); }
    void line(float x0, float y0, float x1, float y1, float width, uint16_t c)
    {
        // a stroke: square dots along the line, width px wide
        const float len = sqrtf((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
        const int steps = (int)(len * 2) + 1;
        const int16_t d = width < 1.5f ? 1 : (int16_t)(width + 0.5f);
        for (int i = 0; i <= steps; ++i)
        {
            const float t = (float)i / steps;
            const int16_t px = (int16_t)floorf(x0 + (x1 - x0) * t - (d - 1) / 2.0f + 0.5f);
            const int16_t py = (int16_t)floorf(y0 + (y1 - y0) * t - (d - 1) / 2.0f + 0.5f);
            fill(px, py, d, d, c);
        }
    }
    // arc of a circle centred cx,cy, radius r, degrees (0 = east, 90 = south), stroke width
    void arc(float cx, float cy, float r, float a0, float a1, float width, uint16_t c)
    {
        const int steps = (int)(fabsf(a1 - a0) * r / 20) + 4;
        float px = cx + r * cosf(a0 * (float)M_PI / 180), py = cy + r * sinf(a0 * (float)M_PI / 180);
        for (int i = 1; i <= steps; ++i)
        {
            const float a = (a0 + (a1 - a0) * i / steps) * (float)M_PI / 180;
            const float nx = cx + r * cosf(a), ny = cy + r * sinf(a);
            line(px, py, nx, ny, width, c);
            px = nx;
            py = ny;
        }
    }

    // --- text (baseline coordinates); "—" is drawn as a bar, letter spacing in px ---
    int16_t width(const uint8_t *font, const char *str, int16_t spacing = 0) { return run(font, str, spacing, 0, 0, 0, false); }
    int16_t text(int16_t x, int16_t baseline, const uint8_t *font, uint16_t c, const char *str, int16_t spacing = 0)
    {
        return run(font, str, spacing, x, baseline, c, true);
    }
    int16_t textRight(int16_t right, int16_t baseline, const uint8_t *font, uint16_t c, const char *str, int16_t spacing = 0)
    {
        return text(right - width(font, str, spacing), baseline, font, c, str, spacing);
    }
    int16_t textCenter(int16_t cx, int16_t baseline, const uint8_t *font, uint16_t c, const char *str, int16_t spacing = 0)
    {
        return text(cx - width(font, str, spacing) / 2, baseline, font, c, str, spacing);
    }

private:
    bool hasGlyph(const char *p, int n)
    {
        s.setCursor(0, 10000);
        for (int i = 0; i < n; ++i)
            s.write((uint8_t)p[i]);
        return s.getCursorX() != 0;
    }
    int16_t run(const uint8_t *font, const char *str, int16_t spacing, int16_t x, int16_t baseline, uint16_t c, bool draw)
    {
        s.setFont(font);
        s.setUTF8Print(true);
        s.setTextWrap(false);
        s.setTextColor(c);
        // measuring draws far below the region (clipped, nothing written)
        const int16_t y = draw ? baseline - oy : 10000;
        int16_t cx = draw ? x - ox : 0;
        const int16_t start = cx;
        const char *p = str;
        bool first = true;
        while (*p)
        {
            // one UTF-8 sequence
            int n = 1;
            const uint8_t b = (uint8_t)*p;
            if (b >= 0xF0) n = 4;
            else if (b >= 0xE0) n = 3;
            else if (b >= 0xC0) n = 2;
            if (!first)
                cx += spacing;
            first = false;
            if (n == 3 && memcmp(p, DASH, 3) == 0)
            {
                // em dash: a bar at x-height middle, the width of two '-' cells
                s.setCursor(0, 10000);
                s.print("--");
                const int16_t w = s.getCursorX();
                if (draw)
                {
                    int16_t bx, by;
                    uint16_t bw, bh;
                    s.getTextBounds("x", 0, 0, &bx, &by, &bw, &bh);
                    const int16_t mid = y + by + bh / 2;
                    s.fillRect(cx + 1, mid, w - 2, (bh >= 9 ? 2 : 1), c);
                }
                cx += w;
            }
            else if (n == 2 && (uint8_t)p[0] == 0xC2 && (uint8_t)p[1] == 0xB7 && !hasGlyph(p, n))
            {
                // middle dot missing from the font (the _tr ones): a dot at x-height middle, one '-' cell wide
                s.setCursor(0, 10000);
                s.print("-");
                const int16_t w = s.getCursorX();
                if (draw)
                {
                    int16_t bx, by;
                    uint16_t bw, bh;
                    s.getTextBounds("x", 0, 0, &bx, &by, &bw, &bh);
                    s.fillRect(cx + w / 2 - 1, y + by + bh / 2 - 1, 2, 2, c);
                }
                cx += w;
            }
            else
            {
                s.setCursor(cx, y);
                for (int i = 0; i < n && p[i]; ++i)
                    s.write((uint8_t)p[i]);
                cx = s.getCursorX();
            }
            p += n;
        }
        return cx - start;
    }
};

// ---------------------------------------------------------------------------------------------------------------------
// Regions

struct Region
{
    int16_t x, y, w, h;
    void (*draw)(Pen &p, const UiModel &m);
};

uint16_t lqColor(int v)
{
    // the design's history-bar rule, used for the big LQ too
    return v >= 98 ? C_ACCENT : (v >= 95 ? C_AMBER : C_RED);
}

void fmt(char *buf, size_t n, const char *f, int v) { snprintf(buf, n, f, v); }

// --- shared header pieces (y 8..24, centre 16) ---

// battery icon 18x10 + "82%" right-aligned at x = right; returns the left edge
int16_t drawWatchBattery(Pen &p, const UiModel &m, int16_t right)
{
    char pct[8];
    if (m.watchBatPct >= 0)
        fmt(pct, sizeof(pct), "%d%%", m.watchBatPct);
    else
        snprintf(pct, sizeof(pct), "%s", DASH);
    const uint16_t c = m.watchCharging ? C_ACCENT : (m.watchBatPct >= 0 && m.watchBatPct < 15 ? C_RED : C_TEXT);
    const int16_t tw = p.width(F_MONO10, pct);
    p.text(right - tw, 20, F_MONO10, c, pct);
    const int16_t ix = right - tw - 3 - 18, iy = 11;
    // outline 16x10 r2, nub 2x4, fill up to 11 px
    p.s.drawRoundRect(ix - p.ox, iy - p.oy, 16, 10, 2, c);
    p.fill(ix + 16, iy + 3, 2, 4, c);
    if (m.watchBatPct >= 0)
    {
        const int16_t fw = (int16_t)((m.watchBatPct * 11 + 50) / 100);
        if (fw > 0)
            p.fill(ix + 2, iy + 2, fw, 6, c);
    }
    return ix;
}

// the Bluetooth rune, 11x11 (or 13x13) at x,y; the design's path M7 7l10 10-5 5V2l5 5L7 17 on a 24 grid
void drawBleIcon(Pen &p, int16_t x, int16_t y, int16_t size, uint16_t c)
{
    const float k = size / 24.0f;
    const float pts[][2] = {{7, 7}, {17, 17}, {12, 22}, {12, 2}, {17, 7}, {7, 17}};
    for (int i = 0; i < 5; ++i)
        p.line(x + pts[i][0] * k, y + pts[i][1] * k, x + pts[i + 1][0] * k, y + pts[i + 1][1] * k, 1.0f, c);
}

// --- 01 LINK ---------------------------------------------------------------------------------------------------------

void linkHeader(Pen &p, const UiModel &m)
{
    // "ELRS" chip: 9 px 700 mono, padding 3/5, radius 4; accent when linked
    const int16_t tw = p.width(F_MONO9, "ELRS", 1);
    const uint16_t bg = m.linked ? C_ACCENT : C_OFF;
    p.rrect(9, 9, tw + 11, 15, 4, bg);
    p.text(14, 20, F_MONO9, m.linked ? C_ON_ACCENT : C_MUTED, "ELRS", 1);
    p.text(15, 20, F_MONO9, m.linked ? C_ON_ACCENT : C_MUTED, "ELRS", 1); // faux bold: 1 px offset
    char band[24];
    snprintf(band, sizeof(band), "%s \xC2\xB7 TX", m.band[0] ? m.band : DASH);
    p.text(9 + tw + 11 + 5, 20, F_MONO10, C_MUTED, band);

    const int16_t left = drawWatchBattery(p, m, 231);
    drawBleIcon(p, left - 7 - 11, 11, 11, m.bleAvailable ? C_BLUE : C_OFF);
}

void linkLq(Pen &p, const UiModel &m)
{
    p.text(9, 36, F_LABEL, C_MUTED, "LINK QUALITY", 1);
    const bool has = m.linked && m.lq >= 0;
    const uint16_t c = has ? lqColor(m.lq) : C_DIM;
    int16_t x = 9;
    if (has)
    {
        char v[8];
        fmt(v, sizeof(v), "%d", m.lq);
        x += p.text(9, 75, F_MONO38B, c, v);
    }
    else
    {
        // a bar where the digits would be
        p.fill(11, 60, 40, 3, C_DIM);
        x = 55;
    }
    p.text(x + 2, 75, F_MONO14B, C_MUTED, "%");
    // bar 112x4 r2
    p.rrect(9, 85, 112, 4, 2, C_LINE);
    if (has && m.lq > 0)
        p.rrect(9, 85, (int16_t)(112 * (m.lq > 100 ? 100 : m.lq) / 100), 4, 2, c);
}

void rssiRow(Pen &p, int16_t baseline, const char *label, int dot, bool valid, int value, const char *unit, bool sign)
{
    int16_t lx = 141;
    if (dot == 1)
    {
        p.disc(143, baseline - 4, 2, C_ACCENT);
        lx = 150;
    }
    else if (dot == 2)
    {
        p.s.drawCircle(143 - p.ox, baseline - 4 - p.oy, 2, C_DIM);
        lx = 150;
    }
    p.text(lx, baseline, F_SANS9, C_MUTED, label);
    char u[8];
    snprintf(u, sizeof(u), " %s", unit);
    const int16_t uw = p.width(F_MONO9, u);
    p.text(231 - uw, baseline, F_MONO9, C_MUTED, u);
    char v[12];
    if (valid)
        fmt(v, sizeof(v), sign ? "%+d" : "%d", value);
    else
        snprintf(v, sizeof(v), "%s", DASH);
    p.textRight(231 - uw, baseline, F_MONO12B, valid ? C_TEXT : C_DIM, v);
}

void linkRssi(Pen &p, const UiModel &m)
{
    p.vline(131, 29, 60, C_LINE);
    rssiRow(p, 39, "RSSI 1", 1, m.linked, m.rssi1, "dBm", false);
    rssiRow(p, 55, "RSSI 2", 2, m.linked && m.hasRssi2, m.rssi2, "dBm", false);
    rssiRow(p, 71, "SNR", 0, m.linked, m.snr, "dB", true);
    rssiRow(p, 87, "RX RSSI", 0, m.linked, m.rxRssi, "dBm", false);
}

void linkHistory(Pen &p, const UiModel &m)
{
    p.text(9, 101, F_MONO9, C_MUTED, "LQ \xC2\xB7 60s");
    char mm[40];
    if (m.lqMin >= 0)
        snprintf(mm, sizeof(mm), "min %d%% \xC2\xB7 avg %d%%", m.lqMin, m.lqAvg);
    else
        snprintf(mm, sizeof(mm), "min %s \xC2\xB7 avg %s", DASH, DASH);
    p.textRight(231, 101, F_MONO9, C_MUTED, mm);
    // 40 bars, pitch 5.55, width 4.55, 20 px tall area at y 106..126
    for (int i = 0; i < UI_LQ_HISTORY; ++i)
    {
        const int16_t x0 = 9 + (int16_t)lroundf(i * 5.55f);
        const int16_t x1 = 9 + (int16_t)lroundf(i * 5.55f + 4.55f);
        const uint8_t v = m.lqHistory[i];
        if (v == UI_NO_SAMPLE)
        {
            p.fill(x0, 124, x1 - x0, 2, C_LINE);
            continue;
        }
        int h = (int)lroundf(((int)v - 85) / 15.0f * 20);
        if (h < 2) h = 2;
        if (h > 20) h = 20;
        p.rrect(x0, 126 - h, x1 - x0, h, h > 3 ? 1 : 0, lqColor(v));
    }
}

// a 2x2 grid card: title left/right (right in tagColor), value + small suffix
void statCard(Pen &p, int16_t x, int16_t y, int16_t w, const char *title, const char *tag, uint16_t tagColor,
              const char *value, const char *suffix, bool valid)
{
    p.card(x, y, w, 36, 6, C_CARD, C_LINE);
    p.text(x + 7, y + 12, F_LABEL, C_MUTED, title, 1);
    if (tag && tag[0])
        p.textRight(x + w - 7, y + 12, F_LABEL, tagColor, tag);
    const int16_t vx = p.text(x + 7, y + 29, F_MONO14B, valid ? C_TEXT : C_DIM, value);
    if (suffix && suffix[0])
        p.text(x + 7 + vx, y + 29, F_MONO9, C_MUTED, suffix);
}

void cardRate(Pen &p, const UiModel &m)
{
    char v[8];
    if (m.rateHz) fmt(v, sizeof(v), "%d", m.rateHz);
    else snprintf(v, sizeof(v), "%s", DASH);
    statCard(p, 9, 131, 109, "RATE", m.rateTag, C_MUTED, v, " Hz", m.rateHz != 0);
}

void cardPower(Pen &p, const UiModel &m)
{
    char v[8], s[20];
    if (m.powerMw) fmt(v, sizeof(v), "%d", m.powerMw);
    else snprintf(v, sizeof(v), "%s", DASH);
    if (m.maxPowerMw) snprintf(s, sizeof(s), " / %u mW", m.maxPowerMw);
    else snprintf(s, sizeof(s), " mW");
    statCard(p, 123, 131, 108, "POWER", m.dynamicPower ? "DYN" : "", C_AMBER, v, s, m.powerMw != 0);
}

void cardTlm(Pen &p, const UiModel &m)
{
    char v[12], tag[16] = "";
    if (m.tlmDenom == 1) snprintf(v, sizeof(v), "Off");
    else if (m.tlmDenom) fmt(v, sizeof(v), "1:%d", m.tlmDenom);
    else snprintf(v, sizeof(v), "%s", DASH);
    if (m.tlmBps >= 1000) snprintf(tag, sizeof(tag), "%d.%d kbps", (int)(m.tlmBps / 1000), (int)(m.tlmBps % 1000) / 100);
    else if (m.tlmBps > 0) snprintf(tag, sizeof(tag), "%d bps", (int)m.tlmBps);
    statCard(p, 9, 172, 109, "TLM", tag, C_MUTED, v, "", m.tlmDenom != 0);
}

void cardRxBat(Pen &p, const UiModel &m)
{
    char v[12], s[20] = "", tag[6] = "";
    const bool has = m.rxBatCentiVolts >= 0;
    if (has)
    {
        const int dv = (int)((m.rxBatCentiVolts + 5) / 10);
        snprintf(v, sizeof(v), "%d.%d", dv / 10, dv % 10);
        if (m.rxCells)
        {
            const int cell = (int)(m.rxBatCentiVolts / m.rxCells);
            snprintf(s, sizeof(s), " V \xC2\xB7 %d.%02d", cell / 100, cell % 100);
            fmt(tag, sizeof(tag), "%dS", m.rxCells);
        }
        else
        {
            snprintf(s, sizeof(s), " V");
        }
    }
    else
    {
        snprintf(v, sizeof(v), "%s", DASH);
    }
    statCard(p, 123, 172, 108, "RX BAT", tag, C_MUTED, v, s, has);
}

void linkFooter(Pen &p, const UiModel &m)
{
    char a[16], b[20], c[16];
    if (m.modelId >= 0) fmt(a, sizeof(a), "MDL %02d", m.modelId);
    else snprintf(a, sizeof(a), "MDL %s", DASH);
    if (m.linkSecs >= 0)
    {
        const int32_t mins = m.linkSecs / 60;
        if (mins >= 100)
            snprintf(b, sizeof(b), "LINK %dh%02d", (int)(mins / 60), (int)(mins % 60));
        else
            snprintf(b, sizeof(b), "LINK %02d:%02d", (int)mins, (int)(m.linkSecs % 60));
    }
    else
        snprintf(b, sizeof(b), "LINK %s", DASH);
    if (m.txTempC > -128) snprintf(c, sizeof(c), "TX %d\xC2\xB0" "C", m.txTempC);
    else snprintf(c, sizeof(c), "TX %s", DASH);
    // justify-content: space-between
    const int16_t wa = p.width(F_MONO9, a), wb = p.width(F_MONO9, b), wc = p.width(F_MONO9, c);
    const int16_t gap = (222 - wa - wb - wc) / 2;
    p.text(9, 221, F_MONO9, C_MUTED, a);
    p.text(9 + wa + gap, 221, F_MONO9, C_MUTED, b);
    p.text(231 - wc, 221, F_MONO9, C_MUTED, c);
}

constexpr Region LINK_REGIONS[] = {
    {0, 0, 240, 27, linkHeader},
    {0, 27, 128, 64, linkLq},
    {128, 27, 112, 64, linkRssi},
    {0, 92, 240, 36, linkHistory},
    {9, 131, 109, 36, cardRate},
    {123, 131, 108, 36, cardPower},
    {9, 172, 109, 36, cardTlm},
    {123, 172, 108, 36, cardRxBat},
    {0, 212, 240, 28, linkFooter},
};

// --- 02 RADIO --------------------------------------------------------------------------------------------------------

void toggle(Pen &p, int16_t x, int16_t y, bool on, uint16_t onColor, uint16_t knobOn)
{
    p.rrect(x, y, 34, 20, 10, on ? onColor : C_OFF);
    p.disc(on ? x + 24 : x + 10, y + 10, 8, on ? knobOn : C_MUTED);
}

void radioHeader(Pen &p, const UiModel &m)
{
    p.text(9, 20, F_SANS12B, C_TEXT, "RADIO", 1);
    drawWatchBattery(p, m, 231);
}

void radioElrs(Pen &p, const UiModel &m)
{
    p.card(9, 30, 222, 40, 8, C_CARD, C_LINE);
    p.rrect(16, 37, 26, 26, 6, m.elrsOn ? C_ELRS_TILE : C_LINE);
    // antenna: M12 12v10, arcs r5 and r9 about (12,12), dot r1.5; 14 px icon centred in the tile
    const float k = 14 / 24.0f, ix = 22, iy = 43;
    const uint16_t ic = m.elrsOn ? C_ACCENT : C_DIM;
    p.line(ix + 12 * k, iy + 12 * k, ix + 12 * k, iy + 22 * k, 1.3f, ic);
    p.arc(ix + 12 * k, iy + 12 * k, 4.95f * k, 225, 315, 1.0f, ic);
    p.arc(ix + 12 * k, iy + 12 * k, 9.2f * k, 225, 315, 1.0f, ic);
    p.disc((int16_t)(ix + 12 * k + 0.5f), (int16_t)(iy + 12 * k + 0.5f), 1, ic);

    p.text(49, 48, F_SANS12B, C_TEXT, "ELRS TX");
    char sub[48];
    if (m.elrsOn)
    {
        char rate[12], pwr[12];
        if (m.rateHz) fmt(rate, sizeof(rate), "%dHz", m.rateHz);
        else snprintf(rate, sizeof(rate), "%s", DASH);
        if (m.powerMw) fmt(pwr, sizeof(pwr), "%dmW", m.powerMw);
        else snprintf(pwr, sizeof(pwr), "%s", DASH);
        snprintf(sub, sizeof(sub), "%s MHz \xC2\xB7 %s \xC2\xB7 %s", m.band[0] ? m.band : DASH, rate, pwr);
    }
    else
        snprintf(sub, sizeof(sub), "Off \xC2\xB7 radio idle");
    p.text(49, 60, F_MONO9, C_MUTED, sub);
    toggle(p, 190, 40, m.elrsOn, C_ACCENT, C_ON_ACCENT);
}

// the bind button: a 78 px circle centred (120,115)
void radioBindCircle(Pen &p, const UiModel &m)
{
    const int16_t cx = 120, cy = 115;
    switch (m.bind)
    {
    case UI_BIND_OFF:
        p.disc(cx, cy, 39, C_OFF);
        p.disc(cx, cy, 37, C_BIND_OFF_BG);
        p.textCenter(cx, cy + 5, F_SANS13B, C_DIM, "BIND", 2);
        break;
    case UI_BIND_IDLE:
        p.disc(cx, cy, 39, C_ACCENT);
        p.disc(cx, cy, 37, C_BIND_IDLE_BG);
        p.textCenter(cx, cy + 5, F_SANS13B, C_ACCENT, "BIND", 2);
        break;
    case UI_BIND_BINDING:
    {
        p.disc(cx, cy, 36, C_BINDING_BG);
        // dashed 3 px border: 22 dashes, 60 % on
        for (int16_t y = -39; y <= 39; ++y)
            for (int16_t x = -39; x <= 39; ++x)
            {
                const float r = sqrtf((float)(x * x + y * y));
                if (r < 36.0f || r > 39.2f)
                    continue;
                float a = atan2f((float)y, (float)x) / (2 * (float)M_PI) + 0.5f; // 0..1
                const float ph = a * 22 - floorf(a * 22);
                if (ph < 0.6f)
                    p.fill(cx + x, cy + y, 1, 1, C_AMBER);
            }
        p.textCenter(cx, 108, F_LABEL, C_AMBER, "BINDING", 1);
        char t[12];
        snprintf(t, sizeof(t), "%u:%02u", (unsigned)(m.bindSecs / 60), (unsigned)(m.bindSecs % 60));
        p.textCenter(cx, 128, F_MONO17B, C_AMBER, t);
        break;
    }
    case UI_BIND_BOUND:
        p.disc(cx, cy, 39, C_ACCENT);
        // check: M5 12.5l4.5 4.5L19 7.5, 20 px, stroke 3, top at 98
        {
            const float k = 20 / 24.0f, ix = 110, iy = 98;
            p.line(ix + 5 * k, iy + 12.5f * k, ix + 9.5f * k, iy + 17 * k, 2.5f, C_ON_ACCENT);
            p.line(ix + 9.5f * k, iy + 17 * k, ix + 19 * k, iy + 7.5f * k, 2.5f, C_ON_ACCENT);
        }
        p.textCenter(cx, 130, F_LABEL, C_ON_ACCENT, "BOUND", 1);
        break;
    }
}

void radioBindText(Pen &p, const UiModel &m)
{
    char hint[48];
    uint16_t c = C_MUTED;
    switch (m.bind)
    {
    case UI_BIND_OFF:
        snprintf(hint, sizeof(hint), "\xD0\xA1\xD0\xBF\xD0\xB5\xD1\x80\xD1\x88\xD1\x83 \xD1\x83\xD0\xB2\xD1\x96\xD0\xBC\xD0\xBA\xD0\xBD\xD1\x96\xD1\x82\xD1\x8C ELRS"); // "Спершу увімкніть ELRS"
        break;
    case UI_BIND_IDLE:
        snprintf(hint, sizeof(hint), "No RX linked");
        break;
    case UI_BIND_BINDING:
        snprintf(hint, sizeof(hint), "Waiting for RX...");
        c = C_AMBER;
        break;
    case UI_BIND_BOUND:
        if (m.lq >= 0) snprintf(hint, sizeof(hint), "RX linked \xC2\xB7 LQ %d%%", m.lq);
        else snprintf(hint, sizeof(hint), "RX linked");
        c = C_ACCENT;
        break;
    }
    p.textCenter(120, 167, m.bind == UI_BIND_OFF ? F_HINT_CYR : F_HINT, c, hint);
    char uid[40];
    snprintf(uid, sizeof(uid), "UID %02X:%02X:%02X:%02X:%02X:%02X", m.uid[0], m.uid[1], m.uid[2], m.uid[3], m.uid[4], m.uid[5]);
    p.textCenter(120, 181, F_MONO9, C_MUTED, uid);
}

void radioBle(Pen &p, const UiModel &m)
{
    p.card(9, 190, 222, 40, 8, C_CARD, C_LINE);
    p.rrect(16, 197, 26, 26, 6, C_BLE_TILE);
    drawBleIcon(p, 23, 204, 13, m.bleAvailable ? C_BLUE : C_DIM);
    p.text(49, 208, F_SANS12B, C_TEXT, "Bluetooth LE");
    // no BLE in this build yet: off, nothing to report
    p.text(49, 220, F_MONO9, C_MUTED, m.bleAvailable ? "Off" : DASH);
    toggle(p, 190, 200, false, C_BLUE, C_ON_BLUE);
}

constexpr Region RADIO_REGIONS[] = {
    {0, 0, 240, 27, radioHeader},
    {9, 30, 222, 40, radioElrs},
    {80, 74, 80, 82, radioBindCircle},
    {0, 156, 240, 31, radioBindText},
    {9, 190, 222, 40, radioBle},
};

// --- 03 WATCH --------------------------------------------------------------------------------------------------------

const char *const WEEKDAYS[7] = {
    "\xD0\x9D\xD0\xB5\xD0\xB4\xD1\x96\xD0\xBB\xD1\x8F",                         // Неділя
    "\xD0\x9F\xD0\xBE\xD0\xBD\xD0\xB5\xD0\xB4\xD1\x96\xD0\xBB\xD0\xBE\xD0\xBA", // Понеділок
    "\xD0\x92\xD1\x96\xD0\xB2\xD1\x82\xD0\xBE\xD1\x80\xD0\xBE\xD0\xBA",         // Вівторок
    "\xD0\xA1\xD0\xB5\xD1\x80\xD0\xB5\xD0\xB4\xD0\xB0",                         // Середа
    "\xD0\xA7\xD0\xB5\xD1\x82\xD0\xB2\xD0\xB5\xD1\x80",                         // Четвер
    "\xD0\x9F'\xD1\x8F\xD1\x82\xD0\xBD\xD0\xB8\xD1\x86\xD1\x8F",                // П'ятниця
    "\xD0\xA1\xD1\x83\xD0\xB1\xD0\xBE\xD1\x82\xD0\xB0",                         // Субота
};
const char *const MONTHS[12] = {
    "\xD1\x81\xD1\x96\xD1\x87\xD0\xBD\xD1\x8F",                         // січня
    "\xD0\xBB\xD1\x8E\xD1\x82\xD0\xBE\xD0\xB3\xD0\xBE",                 // лютого
    "\xD0\xB1\xD0\xB5\xD1\x80\xD0\xB5\xD0\xB7\xD0\xBD\xD1\x8F",         // березня
    "\xD0\xBA\xD0\xB2\xD1\x96\xD1\x82\xD0\xBD\xD1\x8F",                 // квітня
    "\xD1\x82\xD1\x80\xD0\xB0\xD0\xB2\xD0\xBD\xD1\x8F",                 // травня
    "\xD1\x87\xD0\xB5\xD1\x80\xD0\xB2\xD0\xBD\xD1\x8F",                 // червня
    "\xD0\xBB\xD0\xB8\xD0\xBF\xD0\xBD\xD1\x8F",                         // липня
    "\xD1\x81\xD0\xB5\xD1\x80\xD0\xBF\xD0\xBD\xD1\x8F",                 // серпня
    "\xD0\xB2\xD0\xB5\xD1\x80\xD0\xB5\xD1\x81\xD0\xBD\xD1\x8F",         // вересня
    "\xD0\xB6\xD0\xBE\xD0\xB2\xD1\x82\xD0\xBD\xD1\x8F",                 // жовтня
    "\xD0\xBB\xD0\xB8\xD1\x81\xD1\x82\xD0\xBE\xD0\xBF\xD0\xB0\xD0\xB4\xD0\xB0", // листопада
    "\xD0\xB3\xD1\x80\xD1\x83\xD0\xB4\xD0\xBD\xD1\x8F",                 // грудня
};

void watchHeader(Pen &p, const UiModel &m)
{
    // outline chip: dot 5 px, gap 4, "ELRS <LQ>" 9 px 600 mono, padding 2/6/2/5, radius 8
    char t[16];
    const bool has = m.linked && m.lq >= 0;
    if (has) fmt(t, sizeof(t), "ELRS %d", m.lq);
    else snprintf(t, sizeof(t), "ELRS %s", DASH);
    const int16_t tw = p.width(F_MONO9, t);
    const int16_t w = 1 + 5 + 5 + 4 + tw + 6 + 1;
    p.card(9, 9, w, 15, 7, has ? C_BIND_IDLE_BG : C_BIND_OFF_BG, has ? C_CHIP_BORDER : C_OFF);
    p.disc(17, 16, 2, has ? C_ACCENT : C_DIM);
    p.text(24, 20, F_MONO9, has ? C_ACCENT : C_MUTED, t);
    drawBleIcon(p, 9 + w + 6, 11, 11, m.bleAvailable ? C_BLUE : C_OFF);
    drawWatchBattery(p, m, 231);
}

void watchClock(Pen &p, const UiModel &m)
{
    char hm[8], ss[4];
    if (m.timeValid)
    {
        snprintf(hm, sizeof(hm), "%02u:%02u", m.hour, m.minute);
        snprintf(ss, sizeof(ss), "%02u", m.second);
    }
    else
    {
        snprintf(hm, sizeof(hm), "--:--");
        ss[0] = 0;
    }
    const int16_t w = p.width(F_CLOCK, hm, -1);
    const int16_t sw = ss[0] ? p.width(F_MONO12B, ss) : 0;
    const int16_t x = 120 - (w + (sw ? 3 + sw : 0)) / 2;
    p.text(x, 86, F_CLOCK, m.timeValid ? C_TIME : C_DIM, hm, -1);
    if (sw)
        p.text(x + w + 3, 52, F_MONO12B, C_ACCENT, ss);
}

void watchDate(Pen &p, const UiModel &m)
{
    char d[64];
    if (m.timeValid && m.weekday < 7 && m.month >= 1 && m.month <= 12)
        snprintf(d, sizeof(d), "%s, %u %s", WEEKDAYS[m.weekday], m.day, MONTHS[m.month - 1]);
    else
        snprintf(d, sizeof(d), "%s", DASH);
    p.textCenter(120, 109, F_DATE, C_MUTED, d);
}

// a 2x2 card of screen 03: title, value (sans or mono), sub
void watchCard(Pen &p, int16_t x, int16_t y, int16_t w, const char *title, const char *value, bool mono, uint16_t vc,
               const char *sub)
{
    p.card(x, y, w, 50, 7, C_CARD, C_LINE);
    p.text(x + 8, y + 13, F_LABEL, C_MUTED, title, 1);
    p.text(x + 8, y + 29, mono ? F_MONO12B : F_SANS12B, vc, value);
    p.text(x + 8, y + 42, F_MONO9, C_MUTED, sub);
}

void watchElrs(Pen &p, const UiModel &m)
{
    char sub[32], rate[12], rssi[12];
    if (m.rateHz) fmt(rate, sizeof(rate), "%dHz", m.rateHz);
    else snprintf(rate, sizeof(rate), "%s", DASH);
    if (m.linked) fmt(rssi, sizeof(rssi), "%ddBm", m.rssi1);
    else snprintf(rssi, sizeof(rssi), "%s", DASH);
    snprintf(sub, sizeof(sub), "%s \xC2\xB7 %s", rate, rssi);
    watchCard(p, 9, 125, 109, "ELRS", m.linked ? "Linked" : "No link", false, m.linked ? C_ACCENT : C_MUTED, sub);
}

void watchRxBat(Pen &p, const UiModel &m)
{
    char v[16], sub[32];
    if (m.rxBatCentiVolts >= 0)
    {
        const int dv = (int)((m.rxBatCentiVolts + 5) / 10);
        snprintf(v, sizeof(v), "%d.%d V", dv / 10, dv % 10);
        if (m.rxCells)
        {
            const int cell = (int)(m.rxBatCentiVolts / m.rxCells);
            snprintf(sub, sizeof(sub), "%dS \xC2\xB7 %d.%02d/cell", m.rxCells, cell / 100, cell % 100);
        }
        else
            snprintf(sub, sizeof(sub), "%s", DASH);
    }
    else
    {
        snprintf(v, sizeof(v), "%s", DASH);
        snprintf(sub, sizeof(sub), "no telemetry");
    }
    watchCard(p, 123, 125, 108, "RX BAT", v, true, m.rxBatCentiVolts >= 0 ? C_TEXT : C_DIM, sub);
}

void watchBle(Pen &p, const UiModel &m)
{
    watchCard(p, 9, 180, 109, "BLE", "Off", false, C_MUTED, DASH);
}

void watchTimer(Pen &p, const UiModel &m)
{
    char v[16], sub[32], mdl[8];
    if (m.timerSecs >= 0)
        snprintf(v, sizeof(v), "%02d:%02d", (int)(m.timerSecs / 60), (int)(m.timerSecs % 60));
    else
        snprintf(v, sizeof(v), "%s", DASH);
    if (m.modelId >= 0) fmt(mdl, sizeof(mdl), "%02d", m.modelId);
    else snprintf(mdl, sizeof(mdl), "%s", DASH);
    snprintf(sub, sizeof(sub), "%s \xC2\xB7 MDL %s", m.armed ? "armed" : "disarmed", mdl);
    watchCard(p, 123, 180, 108, "TIMER", v, true, m.armed ? C_AMBER : (m.timerSecs >= 0 ? C_TEXT : C_DIM), sub);
}

constexpr Region WATCH_REGIONS[] = {
    {0, 0, 240, 27, watchHeader},
    // the clock in three parts, so a new second sends only the top-right 90x26 (the seconds sit there)
    {0, 56, 240, 36, watchClock},
    {0, 30, 150, 26, watchClock},
    {150, 30, 90, 26, watchClock},
    {0, 96, 240, 18, watchDate},
    {9, 125, 109, 50, watchElrs},
    {123, 125, 108, 50, watchRxBat},
    {9, 180, 109, 50, watchBle},
    {123, 180, 108, 50, watchTimer},
};

template <size_t N> constexpr bool regionsFit(const Region (&r)[N], size_t i = 0)
{
    return i == N || ((int32_t)r[i].w * r[i].h <= BUF_PIXELS && regionsFit(r, i + 1));
}
static_assert(regionsFit(LINK_REGIONS) && regionsFit(RADIO_REGIONS) && regionsFit(WATCH_REGIONS),
              "a region is larger than the region buffer");

struct ScreenDef
{
    const Region *regions;
    uint8_t count;
};
const ScreenDef SCREENS[UI_SCREEN_COUNT] = {
    {LINK_REGIONS, sizeof(LINK_REGIONS) / sizeof(LINK_REGIONS[0])},
    {RADIO_REGIONS, sizeof(RADIO_REGIONS) / sizeof(RADIO_REGIONS[0])},
    {WATCH_REGIONS, sizeof(WATCH_REGIONS) / sizeof(WATCH_REGIONS[0])},
};

constexpr uint8_t MAX_REGIONS = 12;
uint32_t g_sent[UI_SCREEN_COUNT][MAX_REGIONS];

uint32_t fnv1a(const uint16_t *px, int32_t n)
{
    uint32_t h = 2166136261u;
    const uint8_t *b = (const uint8_t *)px;
    for (int32_t i = 0; i < n * 2; ++i)
    {
        h ^= b[i];
        h *= 16777619u;
    }
    return h;
}
} // namespace

uint8_t uiCellsFor(int32_t centiVolts)
{
    // LiPo: the smallest cell count that keeps every cell at or under 4.35 V
    if (centiVolts <= 0)
        return 0;
    const int32_t n = (centiVolts + 434) / 435;
    return (uint8_t)(n > 14 ? 0 : n);
}

uint32_t uiDraw(UiScreen s, const UiModel &m, UiPanel &panel, bool full)
{
    if (s >= UI_SCREEN_COUNT)
        return 0;
    const ScreenDef &def = SCREENS[s];
    uint32_t sent = 0;
    if (full)
        panel.fillScreen(C_BG);
    for (uint8_t i = 0; i < def.count && i < MAX_REGIONS; ++i)
    {
        const Region &r = def.regions[i];
        if ((int32_t)r.w * r.h > BUF_PIXELS)
            continue; // cannot happen: checked at compile time below
        g_surface.resize(r.w, r.h);
        Pen p;
        p.ox = r.x;
        p.oy = r.y;
        p.fill(r.x, r.y, r.w, r.h, C_BG);
        r.draw(p, m);
        const uint32_t h = fnv1a(g_buf, (int32_t)r.w * r.h);
        if (full || h != g_sent[s][i])
        {
            g_sent[s][i] = h;
            panel.send(r.x, r.y, r.w, r.h, g_buf);
            sent += (uint32_t)r.w * r.h;
        }
    }
    return sent;
}

#endif
