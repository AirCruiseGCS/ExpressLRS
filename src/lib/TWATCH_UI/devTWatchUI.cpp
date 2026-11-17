#if defined(TWATCH_UI) && defined(TARGET_TX) && defined(PLATFORM_ESP32)

/*
 * twatch-elrs: the watch's screens as an ELRS device (lib/DEVICE), on core 0 (the device task, priority 0).
 *
 * Timing: nothing here runs in the radio path. The packet timer ISR and the radio (SPI2 / SPIEx, DIO1 interrupt) are
 * on core 1; this device runs in the core-0 device task. The panel is on SPI3 (HSPI), polled, no interrupts; the
 * drawing reads fonts and icons from mapped flash (no cache-disabling flash calls); I2C (AXP2101, PCF8563) is used
 * only from this task (the radio's ALDO4 is switched once, in Radio.Begin, after devicesInit has finished).
 * From core 1 this device takes only plain loads of ELRS globals, and twatchUiOnTelemetry() (tx_main's loop, not an
 * ISR) copies a few bytes.
 *
 * Screen timeout: TWATCH_UI_SCREEN_TIMEOUT_MS after the last wake-up the backlight goes off, the panel is put to sleep
 * (ST7789 SLPIN) and redrawing stops; ALDO2 stays on so the panel keeps its configuration. Wake-up: SLPOUT, a full
 * redraw of the current screen, then the backlight. While dark the model is still sampled (the LQ history keeps
 * filling), nothing is sent to the panel.
 *
 * Build flags:
 *   TWATCH_UI                    this device (and the layout's screen_* pins)
 *   TWATCH_UI_START_SCREEN=n     the home screen, shown at boot and after a plain wake-up: 0 LINK, 1 RADIO,
 *                                2 WATCH (default, the owner's choice)
 *   TWATCH_UI_CYCLE_MS=ms        bench: switch to the next screen every ms while the screen is on (0/undefined: off)
 *   TWATCH_UI_WAKE_ON_EVENTS     bench: wake on bind started (-> 02 RADIO), link lost / regained (-> 01 LINK),
 *                                armed / disarmed (-> 03 WATCH, its TIMER card)
 */

#include "devTWatchUI.h"

#include <Arduino_GFX_Library.h>
#include <Wire.h>

#include "common.h"
#include "config.h"
#include "FHSS.h"
#include "logging.h"
#include "OTA.h"
#include "POWERMGNT.h"
#include "TXModuleEndpoint.h"
#include "crsf_protocol.h"

#if defined(SCREEN_POWER_AXP2101_ALDO2)
#include "AXP2101.h"
#endif

#if !defined(TWATCH_UI_START_SCREEN)
#define TWATCH_UI_START_SCREEN 2
#endif
#if !defined(TWATCH_UI_CYCLE_MS)
#define TWATCH_UI_CYCLE_MS 0
#endif

namespace
{
constexpr uint32_t TICK_AWAKE_MS = 250;      // model update + changed-region redraw
constexpr uint32_t TICK_ASLEEP_MS = 500;     // model update only
constexpr uint32_t LQ_SAMPLE_MS = 1500;      // 40 bars = 60 s
constexpr uint32_t BATTERY_MS = 5000;        // watch battery, TX temperature
constexpr uint32_t RTC_MS = 1000;
constexpr uint32_t RX_BAT_STALE_MS = 10000;  // RX battery telemetry older than this -> "—"
constexpr uint8_t RTC_ADDR = 0x51;           // PCF8563

Arduino_DataBus *bus;
Arduino_GFX *gfx;
bool panelOk = false;

class WatchPanel : public UiPanel
{
public:
    void fillScreen(uint16_t color) override { gfx->fillScreen(color); }
    void send(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t *pixels) override
    {
        gfx->draw16bitRGBBitmap(x, y, (uint16_t *)pixels, w, h);
    }
} panel;

UiModel model;
constexpr UiScreen HOME = (UiScreen)(TWATCH_UI_START_SCREEN % UI_SCREEN_COUNT);
UiScreen screen = HOME;
bool awake = false;
uint32_t wokeAt = 0;
uint32_t screenSince = 0;

volatile bool wakeRequest = true; // the first tick switches the screen on
volatile int8_t screenRequest = -1;

// CRSF battery telemetry, written by tx_main's loop (core 1)
volatile uint32_t rxBatCentiVolts = 0;
volatile uint32_t rxBatAt = 0;
volatile bool rxBatSeen = false;

// state tracking
bool lastLinked = false, lastArmed = false, lastBinding = false;
uint32_t linkSince = 0, armedSince = 0, bindSince = 0;
int32_t lastFlightSecs = -1;
uint32_t lastLqSample = 0, lastBattery = 0, lastRtc = 0;

uint8_t bcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }

void backlight(bool on)
{
    if (GPIO_PIN_SCREEN_BL != UNDEF_PIN)
        digitalWrite(GPIO_PIN_SCREEN_BL, on ? HIGH : LOW); // active high on the watch
}

void readRtc()
{
    Wire.beginTransmission(RTC_ADDR);
    Wire.write(0x02);
    if (Wire.endTransmission(false) != 0 || Wire.requestFrom(RTC_ADDR, (uint8_t)7) != 7)
    {
        model.timeValid = false;
        return;
    }
    uint8_t r[7];
    for (auto &b : r)
        b = Wire.read();
    // VL (bit 7 of seconds): the clock integrity is not guaranteed - never set since power loss
    const bool vl = r[0] & 0x80;
    const uint8_t s = bcd(r[0] & 0x7F), mi = bcd(r[1] & 0x7F), h = bcd(r[2] & 0x3F);
    const uint8_t d = bcd(r[3] & 0x3F), mo = bcd(r[5] & 0x1F);
    const uint16_t y = 2000 + bcd(r[6]);
    if (vl || s > 59 || mi > 59 || h > 23 || d < 1 || d > 31 || mo < 1 || mo > 12)
    {
        model.timeValid = false;
        return;
    }
    model.timeValid = true;
    model.hour = h;
    model.minute = mi;
    model.second = s;
    model.day = d;
    model.month = mo;
    // weekday from the date (Sakamoto), not from the RTC's free-form weekday register
    static const uint8_t t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    const uint16_t yy = y - (mo < 3);
    model.weekday = (yy + yy / 4 - yy / 100 + yy / 400 + t[mo - 1] + d) % 7;
}

void readWatchBattery()
{
#if defined(HAS_AXP2101_PMU)
    const int mv = AXP2101::batteryMillivolts();
    int pct = AXP2101::batteryPercent();
    if (pct < 0 && mv > 0)
    {
        // no fuel-gauge reading: a LiPo resting-voltage curve, 3.30 V = 0 %, 4.15 V = 100 %
        static const uint16_t mvAt[] = {3300, 3600, 3700, 3750, 3800, 3870, 3950, 4050, 4150}; // 0, 12.5, ... 100 %
        pct = 100;
        for (uint8_t i = 1; i < sizeof(mvAt) / sizeof(mvAt[0]); ++i)
        {
            if (mv < mvAt[i])
            {
                pct = (i - 1) * 125 / 10 + (mv - mvAt[i - 1]) * 125 / 10 / (mvAt[i] - mvAt[i - 1]);
                break;
            }
        }
        if (mv < mvAt[0])
            pct = 0;
    }
    model.watchBatPct = mv > 0 ? (int8_t)pct : -1;
    model.watchCharging = AXP2101::charging();
#endif
}

uint16_t mwFor(PowerLevels_e p)
{
    static const uint16_t mw[] = {10, 25, 50, 100, 250, 500, 1000, 2000};
    return p < PWR_COUNT ? mw[p] : 0;
}

void updateModel(uint32_t now)
{
    // --- link ---
    const bool linked = connectionState == connected;
    const bool armed = isArmed;
    const bool binding = InBindingMode;
    model.linked = linked;
    model.armed = armed;
    if (linked)
    {
        model.lq = linkStats.uplink_Link_quality;
        model.rssi1 = (int8_t)linkStats.uplink_RSSI_1;
        model.rssi2 = (int8_t)linkStats.uplink_RSSI_2;
        model.hasRssi2 = linkStats.uplink_RSSI_2 != 0; // a receiver without diversity leaves it 0
        model.snr = linkStats.uplink_SNR;
        model.rxRssi = (int8_t)linkStats.downlink_RSSI_1;
    }
    else
    {
        model.lq = -1;
    }

    // LQ history, one bar per 1.5 s, and min / avg over it
    if (now - lastLqSample >= LQ_SAMPLE_MS)
    {
        lastLqSample = now;
        memmove(model.lqHistory, model.lqHistory + 1, UI_LQ_HISTORY - 1);
        model.lqHistory[UI_LQ_HISTORY - 1] = linked ? (uint8_t)(model.lq > 100 ? 100 : model.lq) : UI_NO_SAMPLE;
        int n = 0, sum = 0, mn = 101;
        for (uint8_t v : model.lqHistory)
        {
            if (v == UI_NO_SAMPLE)
                continue;
            ++n;
            sum += v;
            if (v < mn)
                mn = v;
        }
        model.lqMin = n ? mn : -1;
        model.lqAvg = n ? (sum + n / 2) / n : -1;
    }

    // --- rate, power, telemetry ---
    const char *domain = FHSSconfig ? FHSSconfig->domain : "";
    const char *digits = domain;
    while (*digits && (*digits < '0' || *digits > '9'))
        ++digits;
    snprintf(model.band, sizeof(model.band), "%s", digits);
    const uint32_t interval = ExpressLRS_currAirRate_Modparams ? ExpressLRS_currAirRate_Modparams->interval : 0;
    model.rateHz = interval ? (uint16_t)(1000000UL / interval) : 0;
    static const char *const stdModes[] = {"WIDE", "HYBRID"};
    static const char *const fullModes[] = {"8CH", "16CH/2", "12CH MIX"};
    const uint8_t sm = config.GetSwitchMode();
    snprintf(model.rateTag, sizeof(model.rateTag), "%s",
             OtaIsFullRes ? (sm < 3 ? fullModes[sm] : "") : (sm < 2 ? stdModes[sm] : ""));
    model.powerMw = mwFor(POWERMGNT::currPower());
    model.maxPowerMw = mwFor(POWERMGNT::getMaxPower());
    model.dynamicPower = config.GetDynamicPower();
    model.tlmDenom = ExpressLRS_currTlmDenom;
    model.tlmBps = -1;
    if (model.rateHz && ExpressLRS_currTlmDenom > 1)
    {
        // as TXModuleParameters.cpp's bandwidth string
        const uint8_t burst = TLMBurstMaxForRateRatio(model.rateHz, ExpressLRS_currTlmDenom);
        const uint8_t bytesPerCall = OtaIsFullRes ? ELRS8_DATA_DL_BYTES_PER_CALL : ELRS4_DATA_DL_BYTES_PER_CALL;
        int32_t bps = (int32_t)bytesPerCall * 8 * burst * model.rateHz / ExpressLRS_currTlmDenom / (burst + 1);
        if (OtaIsFullRes)
            bps += 8 * (ELRS8_DATA_DL_BYTES_PER_CALL - sizeof(OTA_LinkStats_s));
        model.tlmBps = bps;
    }

    // --- RX battery (CRSF battery sensor telemetry) ---
    if (rxBatSeen && linked && now - rxBatAt < RX_BAT_STALE_MS)
    {
        model.rxBatCentiVolts = (int32_t)rxBatCentiVolts;
        model.rxCells = uiCellsFor(model.rxBatCentiVolts);
    }
    else
    {
        model.rxBatCentiVolts = -1;
        model.rxCells = 0;
    }

    model.modelId = crsfTransmitter.modelId;

    // --- timers ---
    if (linked && !lastLinked)
        linkSince = now;
    model.linkSecs = linked ? (int32_t)((now - linkSince) / 1000) : -1;
    if (armed && !lastArmed)
        armedSince = now;
    if (!armed && lastArmed)
        lastFlightSecs = (int32_t)((now - armedSince) / 1000);
    model.timerSecs = armed ? (int32_t)((now - armedSince) / 1000) : lastFlightSecs;

    // --- radio screen ---
    model.elrsOn = connectionState < MODE_STATES;
    if (binding && !lastBinding)
        bindSince = now;
    model.bindSecs = binding ? (now - bindSince) / 1000 : 0;
    model.bind = !model.elrsOn ? UI_BIND_OFF : binding ? UI_BIND_BINDING : linked ? UI_BIND_BOUND : UI_BIND_IDLE;
    memcpy(model.uid, UID, sizeof(model.uid));

#if defined(TWATCH_UI_WAKE_ON_EVENTS)
    // switch the screen on, on the screen the event belongs to
    if (binding && !lastBinding)
        screenRequest = UI_SCREEN_RADIO;
    else if (linked != lastLinked)
        screenRequest = UI_SCREEN_LINK;
    else if (armed != lastArmed)
        screenRequest = UI_SCREEN_WATCH;
#endif
    lastLinked = linked;
    lastArmed = armed;
    lastBinding = binding;

    // --- slow things: watch battery, TX temperature, RTC ---
    if (lastBattery == 0 || now - lastBattery >= BATTERY_MS)
    {
        lastBattery = now;
        readWatchBattery();
        const float t = temperatureRead();
        model.txTempC = isnan(t) ? -128 : (int16_t)lroundf(t);
    }
    if (awake && (lastRtc == 0 || now - lastRtc >= RTC_MS))
    {
        lastRtc = now;
        readRtc();
    }
}

void sleepPanel()
{
    backlight(false);
    gfx->displayOff(); // SLPIN (+120 ms, the controller's own requirement)
    awake = false;
    DBGLN("screen off");
}

void wakePanel(uint32_t now)
{
    if (!awake)
    {
        gfx->displayOn(); // SLPOUT (+120 ms)
        lastRtc = 0;
        readRtc();
    }
    awake = true;
    wokeAt = now;
    screenSince = now;
    const uint32_t t0 = micros();
    const uint32_t px = uiDraw(screen, model, panel, true);
    DBGLN("screen on: full redraw %u px in %u us", px, micros() - t0);
    (void)t0;
    (void)px;
    backlight(true);
}

int tick()
{
    const uint32_t now = millis();
    updateModel(now);

    if (screenRequest >= 0)
    {
        screen = (UiScreen)(screenRequest % UI_SCREEN_COUNT);
        screenRequest = -1;
        wakeRequest = true;
    }
    if (wakeRequest)
    {
        wakeRequest = false;
        wakePanel(now);
        return TICK_AWAKE_MS;
    }
    if (!awake)
        return TICK_ASLEEP_MS;
    if (TWATCH_UI_SCREEN_TIMEOUT_MS != 0 && now - wokeAt >= TWATCH_UI_SCREEN_TIMEOUT_MS)
    {
        sleepPanel();
        return TICK_ASLEEP_MS;
    }
#if TWATCH_UI_CYCLE_MS > 0
    if (now - screenSince >= TWATCH_UI_CYCLE_MS)
    {
        screen = (UiScreen)((screen + 1) % UI_SCREEN_COUNT);
        screenSince = now;
        uiDraw(screen, model, panel, true);
        return TICK_AWAKE_MS;
    }
#endif
    uiDraw(screen, model, panel, false);
    return TICK_AWAKE_MS;
}

bool initialize()
{
    if (GPIO_PIN_SCREEN_CS == UNDEF_PIN || GPIO_PIN_SCREEN_DC == UNDEF_PIN || GPIO_PIN_SCREEN_SCK == UNDEF_PIN ||
        GPIO_PIN_SCREEN_MOSI == UNDEF_PIN)
    {
        ERRLN("twatch ui: no screen_* pins in the layout");
        return false;
    }
#if defined(SCREEN_POWER_AXP2101_ALDO2)
    // the panel's and the backlight's supply; stays on while the screen sleeps
    if (!AXP2101::begin() || !AXP2101::enableAldo(2, 3300))
        ERRLN("twatch ui: AXP2101 not answering, ALDO2 (the panel's supply) may be off");
    delay(10);
#endif
    if (GPIO_PIN_SCREEN_BL != UNDEF_PIN)
    {
        pinMode(GPIO_PIN_SCREEN_BL, OUTPUT);
        backlight(false);
    }
    // HSPI = SPI3: SPI2 (FSPI) is the radio's (SPIEx) on the ESP32-S3
    bus = new Arduino_ESP32SPI(GPIO_PIN_SCREEN_DC, GPIO_PIN_SCREEN_CS, GPIO_PIN_SCREEN_SCK, GPIO_PIN_SCREEN_MOSI,
                               GFX_NOT_DEFINED, HSPI);
    // a 240x240 window of a 240x320 ST7789, IPS; at rotation 2 the visible rows start at 80 (as tx-probe)
    gfx = new Arduino_ST7789(bus, GPIO_PIN_SCREEN_RST, OPT_SCREEN_REVERSED ? 0 : 2, true, 240, 240, 0, 80, 0, 80);
    panelOk = gfx->begin(40000000);
    if (!panelOk)
    {
        ERRLN("twatch ui: panel init failed");
        return false;
    }
    gfx->fillScreen(0);
    return true;
}

int start()
{
    return DURATION_IMMEDIATELY;
}

int event()
{
    // EVENT_CONNECTION_CHANGED / EVENT_ARM_FLAG_CHANGED: react now rather than at the next tick
    return tick();
}

int timeout()
{
    return tick();
}
} // namespace

void twatchUiWake()
{
    screenRequest = HOME; // acted on at the next tick: <= 0.5 s
}

void twatchUiShowScreen(UiScreen s)
{
    screenRequest = (int8_t)s;
}

void twatchUiOnTelemetry(const uint8_t *frame)
{
    const crsf_header_t *h = (const crsf_header_t *)frame;
    if (h->type != CRSF_FRAMETYPE_BATTERY_SENSOR || h->frame_size < 2 + sizeof(crsf_sensor_battery_t))
        return;
    // voltage: big-endian, 0.1 V units
    const uint16_t dv = ((uint16_t)h->payload[0] << 8) | h->payload[1];
    rxBatCentiVolts = (uint32_t)dv * 10;
    rxBatAt = millis();
    rxBatSeen = true;
}

device_t TWatchUI_device = {
    .initialize = initialize,
    .start = start,
    .event = event,
    .timeout = timeout,
    .subscribe = EVENT_CONNECTION_CHANGED | EVENT_ARM_FLAG_CHANGED,
};

#endif
