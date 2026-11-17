#include "AXP2101.h"

#if defined(HAS_AXP2101_PMU) && defined(PLATFORM_ESP32) && !defined(UNIT_TEST)

#include "targets.h"
#include "logging.h"
#include <Wire.h>

namespace
{
    constexpr uint8_t ADDR = 0x34;
    constexpr uint8_t REG_STATUS1 = 0x00;            // bit 5 VBUS good, bit 3 battery present
    constexpr uint8_t REG_STATUS2 = 0x01;            // bits 7..5 charge direction (1 = charging), bit 3 VBUS not in
    constexpr uint8_t REG_CHIP_ID = 0x03;            // 0x4A
    constexpr uint8_t REG_GAUGE_WDT_CTRL = 0x18;     // bit 3 fuel gauge enabled
    constexpr uint8_t REG_ADC_CHANNEL_CTRL = 0x30;   // bit 0 battery voltage
    constexpr uint8_t REG_ADC_VBAT_H = 0x34;         // bits 4..0 = mV bits 12..8
    constexpr uint8_t REG_ADC_VBAT_L = 0x35;
    constexpr uint8_t REG_LDO_ONOFF_CTRL0 = 0x90;    // bit 0 ALDO1 .. bit 3 ALDO4
    constexpr uint8_t REG_LDO_VOL0_CTRL = 0x92;      // 0x92..0x95 = ALDO1..4, bits 4..0: 500 mV + n * 100 mV
    constexpr uint8_t REG_BAT_PERCENT = 0xA4;

    bool adcOn = false;

    int readReg(uint8_t reg)
    {
        Wire.beginTransmission(ADDR);
        Wire.write(reg);
        if (Wire.endTransmission(false) != 0)
            return -1;
        if (Wire.requestFrom(ADDR, (uint8_t)1) != 1)
            return -1;
        return Wire.read();
    }

    bool writeReg(uint8_t reg, uint8_t val)
    {
        Wire.beginTransmission(ADDR);
        Wire.write(reg);
        Wire.write(val);
        return Wire.endTransmission() == 0;
    }
}

extern boolean i2c_enabled; // rxtx_common.cpp

bool AXP2101::begin()
{
    if (!i2c_enabled)
    {
        if (GPIO_PIN_SDA == UNDEF_PIN || GPIO_PIN_SCL == UNDEF_PIN)
        {
            ERRLN("AXP2101 needs i2c_sda/i2c_scl in the hardware layout");
            return false;
        }
        Wire.begin(GPIO_PIN_SDA, GPIO_PIN_SCL);
        i2c_enabled = true;
    }
    const int id = readReg(REG_CHIP_ID);
    DBGLN("AXP2101 chip id 0x%x", id);
    (void)id;
    return true;
}

bool AXP2101::enableAldo(uint8_t n, uint16_t millivolts)
{
    if (n < 1 || n > 4 || millivolts < 500 || millivolts > 3500)
        return false;
    const uint8_t volReg = REG_LDO_VOL0_CTRL + (n - 1);
    const int vol = readReg(volReg);
    const int onoff = readReg(REG_LDO_ONOFF_CTRL0);
    if (vol < 0 || onoff < 0)
        return false;
    writeReg(volReg, (uint8_t)((vol & 0xE0) | ((millivolts - 500) / 100)));
    writeReg(REG_LDO_ONOFF_CTRL0, (uint8_t)(onoff | (1 << (n - 1))));
    return true;
}

int AXP2101::batteryMillivolts()
{
    const int s1 = readReg(REG_STATUS1);
    if (s1 < 0)
        return -1;
    if (!(s1 & 0x08))
        return 0;
    if (!adcOn)
    {
        const int adc = readReg(REG_ADC_CHANNEL_CTRL);
        if (adc < 0)
            return -1;
        adcOn = (adc & 0x01) || writeReg(REG_ADC_CHANNEL_CTRL, (uint8_t)(adc | 0x01));
    }
    const int h = readReg(REG_ADC_VBAT_H);
    const int l = readReg(REG_ADC_VBAT_L);
    if (h < 0 || l < 0)
        return -1;
    return ((h & 0x1F) << 8) | l;
}

int AXP2101::batteryPercent()
{
    const int s1 = readReg(REG_STATUS1);
    const int gauge = readReg(REG_GAUGE_WDT_CTRL);
    if (s1 < 0 || gauge < 0 || !(s1 & 0x08) || !(gauge & 0x08))
        return -1;
    const int pct = readReg(REG_BAT_PERCENT);
    return (pct < 0 || pct > 100) ? -1 : pct;
}

bool AXP2101::vbusIn()
{
    const int s1 = readReg(REG_STATUS1);
    const int s2 = readReg(REG_STATUS2);
    return s1 >= 0 && s2 >= 0 && (s1 & 0x20) && !(s2 & 0x08);
}

bool AXP2101::charging()
{
    const int s2 = readReg(REG_STATUS2);
    return s2 >= 0 && (s2 >> 5) == 0x01;
}

#endif
