#pragma once

#if defined(TWATCH_UI)
#include "device.h"
#include "twatch_ui.h"

extern device_t TWatchUI_device;

// The screen goes dark (backlight off, panel asleep, no redraws) this long after the last wake-up.
constexpr uint32_t TWATCH_UI_SCREEN_TIMEOUT_MS = 30000;

/**
 * Hooks, safe to call from any task or core (they only set flags; the device task acts on them).
 * The input code (touch / side key, phase WE2.2) is to call these.
 */
void twatchUiWake();                     // screen on, on the home screen (03 WATCH), full redraw, timeout restarted
void twatchUiShowScreen(UiScreen s);     // switch screen (and wake)

// tx_main.cpp, for every CRSF telemetry frame from the receiver (core 1 loop): picks up the battery sensor frame
void twatchUiOnTelemetry(const uint8_t *crsfFrame);
#endif
