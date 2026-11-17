#pragma once

/**
 * twatch-elrs: the few AXP2101 PMU registers the LilyGO T-Watch S3 build needs - its LDO rails (ALDO4 = the SX1262,
 * ALDO2 = the ST7789 and its backlight) and the battery readings for the screen. Plain Wire register access on the
 * hardware layout's i2c_sda / i2c_scl, address 0x34; no XPowersLib. Register numbers as in XPowersLib's
 * AXP2101Constants.h / the AXP2101 datasheet.
 *
 * Only compiled in builds that use it (SX1262_POWER_AXP2101_ALDO4 or SCREEN_POWER_AXP2101_ALDO2).
 * Not thread safe (CONFIG_DISABLE_HAL_LOCKS=1 in ELRS's ESP32 builds): the radio's ALDO4 is switched once, from
 * Radio.Begin() on core 1 after devicesInit() has returned; everything else (the screen's ALDO2 at its init, the
 * battery reads) runs in the core-0 device task.
 */

#if defined(SX1262_POWER_AXP2101_ALDO4) || defined(SCREEN_POWER_AXP2101_ALDO2)
#define HAS_AXP2101_PMU 1

#include <stdint.h>

namespace AXP2101
{
    // Starts Wire on the layout's i2c pins if setupTargetCommon() has not; false only if the layout has none.
    bool begin();
    // ALDO1..4: set to millivolts (500..3500, 100 mV steps) and switch on. false if the PMU did not answer.
    bool enableAldo(uint8_t n, uint16_t millivolts);
    // Battery voltage in mV (the ADC channel is switched on by the first call); 0 = no battery, -1 = no answer.
    int batteryMillivolts();
    // The fuel gauge's state of charge, 0..100; -1 if there is no battery, the gauge is off, or no answer.
    int batteryPercent();
    // USB (VBUS) present and good.
    bool vbusIn();
    // Charger is charging.
    bool charging();
}
#endif
