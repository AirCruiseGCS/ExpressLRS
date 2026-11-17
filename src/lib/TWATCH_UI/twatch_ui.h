#pragma once

/**
 * twatch-elrs: the T-Watch S3's own screens (owner's designs, Design/extracted/01..03) on its 240x240 ST7789.
 * Build flag TWATCH_UI. Replaces ELRS's lib/SCREEN for this build (the layout has no screen_type, so devScreen stays
 * off); the panel pins are the layout's screen_* keys.
 *
 * Split in two:
 *  - twatch_ui_draw.cpp: pure drawing. A UiModel (every value already measured, or flagged as missing) is drawn into
 *    rectangular regions; each region is rendered into a RAM buffer and sent to the panel only when its pixels differ
 *    from what was last sent (a hash per region). No ELRS dependencies (it also builds on a PC for previews).
 *  - devTWatchUI.cpp: the ELRS device (core 0 device task) that fills the UiModel, owns the panel, the 30 s screen
 *    timeout and the wake hooks.
 */

#include <stdint.h>
#include <stddef.h>

enum UiScreen : uint8_t
{
    UI_SCREEN_LINK = 0,  // 01 - ELRS link status
    UI_SCREEN_RADIO = 1, // 02 - ELRS TX, bind, BLE
    UI_SCREEN_WATCH = 2, // 03 - watch face with statuses
    UI_SCREEN_COUNT
};

enum UiBindState : uint8_t
{
    UI_BIND_OFF,     // the radio is not running
    UI_BIND_IDLE,    // running, not linked, not binding
    UI_BIND_BINDING, // ELRS bind mode active
    UI_BIND_BOUND    // linked to a receiver
};

constexpr uint8_t UI_LQ_HISTORY = 40; // bars on screen 01 (60 s -> one sample per 1.5 s)
constexpr uint8_t UI_NO_SAMPLE = 0xFF;

struct UiModel
{
    // --- the watch itself (header on every screen) ---
    int8_t watchBatPct = -1;   // -1 unknown
    bool watchCharging = false;
    bool bleAvailable = false; // no BLE yet: icon dimmed, "Off"/"—"

    // --- ELRS link ---
    bool linked = false;       // connectionState == connected
    bool armed = false;
    int16_t lq = -1;           // uplink LQ %, -1 = none
    int16_t rssi1 = 0, rssi2 = 0, snr = 0, rxRssi = 0; // dBm / dB, valid when linked
    bool hasRssi2 = false;     // the receiver reports a second antenna
    uint8_t lqHistory[UI_LQ_HISTORY]; // oldest first, UI_NO_SAMPLE = no link at that time
    int16_t lqMin = -1, lqAvg = -1;
    char band[8] = "";         // "915"
    uint16_t rateHz = 0;       // 0 = unknown
    char rateTag[12] = "";     // switch mode, e.g. "WIDE"
    uint16_t powerMw = 0, maxPowerMw = 0;
    bool dynamicPower = false;
    uint16_t tlmDenom = 0;     // 0 unknown, 1 = off
    int32_t tlmBps = -1;
    int32_t rxBatCentiVolts = -1; // CRSF battery telemetry, -1 = none
    uint8_t rxCells = 0;
    int16_t modelId = -1;
    int32_t linkSecs = -1;     // since the link came up, -1 = not linked
    int16_t txTempC = -128;    // ESP32-S3 internal sensor, -128 = unknown

    // --- screen 02 ---
    bool elrsOn = false;
    UiBindState bind = UI_BIND_OFF;
    uint32_t bindSecs = 0;     // seconds in bind mode (ELRS TX bind has no timeout to count down)
    uint8_t uid[6] = {0};

    // --- screen 03 ---
    bool timeValid = false;
    uint8_t hour = 0, minute = 0, second = 0;
    uint8_t weekday = 0;       // 0 = Sunday
    uint8_t day = 1, month = 1; // 1-based
    int32_t timerSecs = -1;    // armed time (running while armed, last flight's after), -1 = never armed

    UiModel()
    {
        for (uint8_t i = 0; i < UI_LQ_HISTORY; ++i)
            lqHistory[i] = UI_NO_SAMPLE;
    }
};

/**
 * Where finished regions go: the panel on the watch, an image on a PC. send() gets w*h RGB565 pixels, row-major.
 */
class UiPanel
{
public:
    virtual ~UiPanel() = default;
    virtual void fillScreen(uint16_t color) = 0;
    virtual void send(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t *pixels) = 0;
};

/**
 * Draws screen s. full = repaint everything (screen change, wake-up); otherwise only regions whose pixels changed
 * are sent. Returns the number of pixels sent (the SPI cost: 2 bytes each).
 */
uint32_t uiDraw(UiScreen s, const UiModel &m, UiPanel &panel, bool full);

// pure helpers shared with devTWatchUI.cpp
uint8_t uiCellsFor(int32_t centiVolts);
