#if defined(RADIO_SX1262) && !defined(UNIT_TEST)

/*
 * ELRS driver for an SX1262 speaking to SX127x devices (ELRS 900 MHz LoRa).
 *
 * What interoperating with the SX127x driver (lib/SX127xDriver) requires, established on the bench (twatch-elrs,
 * 17.11.2026, SX1262 against SX1276 receivers and a reference TX on ELRS 4.1.0):
 *  - LoRa parameters unchanged from the SX127x rate table: implicit header, no hardware CRC, LDRO off, BW/SF/CR as given.
 *  - IQ always STANDARD. SX127xDriver::Config() ignores InvertIQ, so ELRS 900 never inverts IQ; this driver does the
 *    same (IQinverted stays false). Verified on air: IQ standard links, inverted does not.
 *  - Sync word: the SX127x byte 0x12 is 0x1424 on the SX126x (each nibble n -> n4).
 *  - Frequencies arrive as SX127x register values (FREQ_STEP = 32 MHz / 2^19). The SX126x step is 32 MHz / 2^25,
 *    so the SX126x value is exactly reg << 6.
 *  - Timing: the radio idles in STDBY_XOSC (fallback mode after TX/RX), so SetTx never waits for the TCXO. The TX path
 *    is WriteBuffer + ClearIrqStatus + SetTx; the RX path ClearIrqStatus + SetRx. DIO1 carries both TX_DONE and
 *    RX_DONE (set once in Begin), so no IRQ reconfiguration on the hot path - the SX127x driver does the same with
 *    its DIO0 mapping.
 *  - Errata (DS_SX1261-2 rev 2.1 chapter 15): 15.1 register 0x0889 bit 2 cleared for LoRa BW500, set otherwise;
 *    15.2 PA clamping 0x08D8 |= 0x1E at init; 15.4 register 0x0736 bit 2 set (standard IQ).
 */

#include "SX1262_Regs.h"
#include "SX1262_hal.h"
#include "SX1262.h"
#include "logging.h"
#include "RFAMP_hal.h"
#include <string.h>

SX1262Hal hal;
SX1262Driver *SX1262Driver::instance = NULL;

RFAMP_hal RFAMP;

static constexpr uint8_t SX1262_TX_BUFFER_BASE = 0x00;
static constexpr uint8_t SX1262_RX_BUFFER_BASE = 0x00;
static constexpr uint8_t SX1262_PA_RAMP = SX1262_RAMP_200U; // as on the bench (RadioLib's default)

SX1262Driver::SX1262Driver(): SX12xxDriverCommon()
{
    instance = this;
    currSyncWord = SX127X_SYNC_WORD;
    currOpmode = SX1262_MODE_SLEEP;
    currBW = SX1262_LORA_BW_500;
    currSF = 8;
    currCR = SX1262_LORA_CR_4_7;
    currPreambleLen = 0;
    PayloadLength = 8; // Dummy default value which is overwritten during setup.
    currFreq = 0;
    IQinverted = false;
    pwrCurrent = PWRPENDING_NONE;
    pwrPending = PWRPENDING_NONE;
    strongestReceivingRadio = SX12XX_Radio_1;
}

void SX1262Driver::End()
{
    SetMode(SX1262_MODE_SLEEP, SX12XX_Radio_All);
    hal.end();
    RFAMP.TXRXdisable();
    RemoveCallbacks();
}

void SX1262Driver::CalibrateImage(uint32_t minimumFrequency, uint32_t maximumFrequency)
{
    // minimum/maximumFrequency are SX127x register values
    const uint32_t minMHz = (uint32_t)((double)minimumFrequency * FREQ_STEP / 1000000.0);
    const uint32_t maxMHz = (uint32_t)((double)maximumFrequency * FREQ_STEP / 1000000.0) + 1;

    // the datasheet's table 9-2 bands, else the generic formula (step 4 MHz)
    uint8_t buf[2];
    if (minMHz >= 902 && maxMHz <= 928)      { buf[0] = 0xE1; buf[1] = 0xE9; }
    else if (minMHz >= 863 && maxMHz <= 870) { buf[0] = 0xD7; buf[1] = 0xDB; }
    else if (minMHz >= 779 && maxMHz <= 787) { buf[0] = 0xC1; buf[1] = 0xC5; }
    else if (minMHz >= 470 && maxMHz <= 510) { buf[0] = 0x75; buf[1] = 0x81; }
    else if (minMHz >= 430 && maxMHz <= 440) { buf[0] = 0x6B; buf[1] = 0x6F; }
    else
    {
        buf[0] = (uint8_t)(minMHz / 4);
        buf[1] = (uint8_t)((maxMHz + 3) / 4);
    }
    DBGLN("SX1262 image calibration %u-%u MHz: %x %x", minMHz, maxMHz, buf[0], buf[1]);
    hal.WriteCommand(SX1262_CMD_CALIBRATE_IMAGE, buf, sizeof(buf), SX12XX_Radio_All);
    hal.WaitOnBusy(50000);
}

bool SX1262Driver::Begin(uint32_t minimumFrequency, uint32_t maximumFrequency)
{
    hal.init();
    hal.IsrCallback_1 = &SX1262Driver::IsrCallback_1;
    hal.IsrCallback_2 = nullptr;

    RFAMP.init();

    // Detect the chip: the version string at 0x0320 reads "SX1261 ..." on SX1261/SX1262 alike.
    // The SX126x may ignore the first commands after a reset, so retry like the SX127x driver's DetectChip.
    bool found = false;
    for (int tries = 0; tries < 3 && !found; ++tries)
    {
        hal.reset();
        SetMode(SX1262_MODE_STDBY_RC, SX12XX_Radio_All);
        char version[17] = {0};
        hal.ReadRegister(SX1262_REG_VERSION_STRING, (uint8_t *)version, 16, SX12XX_Radio_1);
        DBGLN("SX1262 version string: %s", version);
        found = strncmp(version, "SX126", 5) == 0;
        if (!found)
            delay(100);
    }
    if (!found)
    {
        DBGLN("SX1262 not found");
        return false;
    }

    WORD_ALIGNED_ATTR uint8_t buf[4];

    buf[0] = 0; buf[1] = 0;
    hal.WriteCommand(SX1262_CMD_CLEAR_DEVICE_ERRORS, buf, 2, SX12XX_Radio_All);

#if SX1262_DIO3_TCXO_VOLTAGE >= 0
    // DIO3 powers the TCXO; the chip waits SX1262_TCXO_DELAY_STEPS * 15.625 us whenever it starts the oscillator
    buf[0] = SX1262_DIO3_TCXO_VOLTAGE;
    buf[1] = (uint8_t)((SX1262_TCXO_DELAY_STEPS >> 16) & 0xFF);
    buf[2] = (uint8_t)((SX1262_TCXO_DELAY_STEPS >> 8) & 0xFF);
    buf[3] = (uint8_t)(SX1262_TCXO_DELAY_STEPS & 0xFF);
    hal.WriteCommand(SX1262_CMD_SET_DIO3_AS_TCXO_CTRL, buf, 4, SX12XX_Radio_All);
#endif

    hal.WriteCommand(SX1262_CMD_SET_PACKET_TYPE, SX1262_PACKET_TYPE_LORA, SX12XX_Radio_All);
    buf[0] = SX1262_TX_BUFFER_BASE; buf[1] = SX1262_RX_BUFFER_BASE;
    hal.WriteCommand(SX1262_CMD_SET_BUFFER_BASE_ADDRESS, buf, 2, SX12XX_Radio_All);

    // Calibrate everything (with the TCXO running), then the image for the band in use
    hal.WriteCommand(SX1262_CMD_CALIBRATE, SX1262_CALIBRATE_ALL, SX12XX_Radio_All);
    delay(5);
    hal.WaitOnBusy(50000);
    CalibrateImage(minimumFrequency, maximumFrequency);

    uint16_t errors = 0;
    hal.ReadCommand(SX1262_CMD_GET_DEVICE_ERRORS, buf, 2, SX12XX_Radio_1);
    errors = ((uint16_t)buf[0] << 8) | buf[1];
    if (errors)
    {
        DBGLN("SX1262 device errors after calibration: 0x%x", errors);
        buf[0] = 0; buf[1] = 0;
        hal.WriteCommand(SX1262_CMD_CLEAR_DEVICE_ERRORS, buf, 2, SX12XX_Radio_All);
    }

    if (OPT_USE_HARDWARE_DCDC)
    {
        DBGLN("Enabling DCDC regulator");
        hal.WriteCommand(SX1262_CMD_SET_REGULATOR_MODE, SX1262_REGULATOR_DCDC, SX12XX_Radio_All);
    }

#if SX1262_DIO2_RF_SWITCH
    hal.WriteCommand(SX1262_CMD_SET_DIO2_AS_RF_SWITCH_CTRL, (uint8_t)0x01, SX12XX_Radio_All);
#endif

    // After TX/RX the radio falls back to STDBY_XOSC: the TCXO keeps running and the next SetTx starts at once
    hal.WriteCommand(SX1262_CMD_SET_RX_TX_FALLBACK_MODE, SX1262_FALLBACK_STDBY_XOSC, SX12XX_Radio_All);

    // Errata 15.2: PA clamping
    hal.WriteRegister(SX1262_REG_TX_CLAMP_CONFIG, hal.ReadRegister(SX1262_REG_TX_CLAMP_CONFIG, SX12XX_Radio_1) | 0x1E, SX12XX_Radio_All);
    // Boosted RX gain (telemetry)
    hal.WriteRegister(SX1262_REG_RX_GAIN, SX1262_RX_GAIN_BOOSTED, SX12XX_Radio_All);

    // DIO1 = TX_DONE | RX_DONE, for good
    WORD_ALIGNED_ATTR uint8_t irq[8] = {
        (uint8_t)((SX1262_IRQ_TX_DONE | SX1262_IRQ_RX_DONE) >> 8), (uint8_t)(SX1262_IRQ_TX_DONE | SX1262_IRQ_RX_DONE),
        (uint8_t)((SX1262_IRQ_TX_DONE | SX1262_IRQ_RX_DONE) >> 8), (uint8_t)(SX1262_IRQ_TX_DONE | SX1262_IRQ_RX_DONE),
        0, 0, 0, 0};
    hal.WriteCommand(SX1262_CMD_SET_DIO_IRQ_PARAMS, irq, sizeof(irq), SX12XX_Radio_All);
    ClearIrqStatus(SX1262_IRQ_ALL, SX12XX_Radio_All);

    // SX1262 high-power PA (+22 dBm max). SetPaConfig also resets OCP to 140 mA.
    buf[0] = SX1262_PA_DUTY_CYCLE;
    buf[1] = SX1262_PA_HP_MAX;
    buf[2] = SX1262_PA_DEVICE_SEL_SX1262;
    buf[3] = SX1262_PA_LUT;
    hal.WriteCommand(SX1262_CMD_SET_PA_CONFIG, buf, 4, SX12XX_Radio_All);

    SetMode(SX1262_MODE_STDBY_XOSC, SX12XX_Radio_All);
    hal.WaitOnBusy(20000); // TCXO start-up

    // Force the next power update, and the lowest power
    pwrCurrent = PWRPENDING_NONE;
    SetOutputPower(SX1262_POWER_MIN);
    CommitOutputPower();

    return true;
}

void SX1262Driver::startCWTest(uint32_t freq, SX12XX_Radio_Number_t radioNumber)
{
    SetMode(SX1262_MODE_STDBY_XOSC, SX12XX_Radio_All);
    const uint32_t regFreq = (uint32_t)((double)freq / (double)FREQ_STEP);
    SetFrequencyReg(regFreq, SX12XX_Radio_All);
    CommitOutputPower();
    RFAMP.TXenable(radioNumber);
    hal.WriteCommand(SX1262_CMD_SET_TX_CONTINUOUS_WAVE, radioNumber);
    currOpmode = SX1262_MODE_TX;
}

void SX1262Driver::Config(uint8_t bw, uint8_t sf, uint8_t cr, uint32_t freq, uint8_t preambleLen, bool InvertIQ, uint8_t _PayloadLength)
{
    Config(bw, sf, cr, freq, preambleLen, currSyncWord, InvertIQ, _PayloadLength);
}

void SX1262Driver::Config(uint8_t bw, uint8_t sf, uint8_t cr, uint32_t freq, uint8_t preambleLen, uint8_t syncWord, bool InvertIQ, uint8_t _PayloadLength)
{
    // InvertIQ is ignored on purpose: SX127xDriver ignores it too, so ELRS 900 devices never invert IQ.
    (void)InvertIQ;
    IQinverted = false;
    PayloadLength = _PayloadLength;

    SetMode(SX1262_MODE_STDBY_XOSC, SX12XX_Radio_All);

    // SX127x register enums -> SX126x codes
    switch ((SX127x_Bandwidth)bw)
    {
    case SX127x_BW_125_00_KHZ: currBW = SX1262_LORA_BW_125; break;
    case SX127x_BW_250_00_KHZ: currBW = SX1262_LORA_BW_250; break;
    case SX127x_BW_500_00_KHZ:
    default:                   currBW = SX1262_LORA_BW_500; break;
    }
    currSF = (uint8_t)(sf >> 4); // SX127x_SF_n = n << 4
    switch ((SX127x_CodingRate)cr)
    {
    case SX127x_CR_4_5: currCR = SX1262_LORA_CR_4_5; break;
    case SX127x_CR_4_6: currCR = SX1262_LORA_CR_4_6; break;
    case SX127x_CR_4_8: currCR = SX1262_LORA_CR_4_8; break;
    case SX127x_CR_4_7:
    default:            currCR = SX1262_LORA_CR_4_7; break;
    }
    currPreambleLen = preambleLen;

    WORD_ALIGNED_ATTR uint8_t buf[6];
    buf[0] = currSF;
    buf[1] = currBW;
    buf[2] = currCR;
    buf[3] = SX1262_LORA_LDRO_OFF; // as SX127xDriver: LDRO always off
    hal.WriteCommand(SX1262_CMD_SET_MODULATION_PARAMS, buf, 4, SX12XX_Radio_All);

    buf[0] = 0;                     // preamble MSB
    buf[1] = currPreambleLen;       // preamble LSB
    buf[2] = SX1262_LORA_HEADER_IMPLICIT;
    buf[3] = PayloadLength;
    buf[4] = SX1262_LORA_CRC_OFF;
    buf[5] = SX1262_LORA_IQ_STANDARD;
    hal.WriteCommand(SX1262_CMD_SET_PACKET_PARAMS, buf, 6, SX12XX_Radio_All);

    // Errata 15.4: standard IQ -> bit 2 of 0x0736 set
    hal.WriteRegister(SX1262_REG_IQ_POLARITY, hal.ReadRegister(SX1262_REG_IQ_POLARITY, SX12XX_Radio_1) | 0x04, SX12XX_Radio_All);
    // Errata 15.1: LoRa BW500 -> bit 2 of 0x0889 cleared, any other BW -> set. Done here, once per rate change,
    // instead of before every transmission (the register keeps its value while the chip is not put to sleep).
    uint8_t sens = hal.ReadRegister(SX1262_REG_SENSITIVITY_CONFIG, SX12XX_Radio_1);
    sens = (currBW == SX1262_LORA_BW_500) ? (sens & 0xFB) : (sens | 0x04);
    hal.WriteRegister(SX1262_REG_SENSITIVITY_CONFIG, sens, SX12XX_Radio_All);

    SetSyncWord(syncWord);
    SetFrequencyReg(freq, SX12XX_Radio_All);
}

void SX1262Driver::SetSyncWord(uint8_t syncWord)
{
    // SX127x sync word 0xXY == SX126x 0xX4Y4 (0x12 -> 0x1424, 0x34 -> 0x3444)
    WORD_ALIGNED_ATTR uint8_t buf[2] = {
        (uint8_t)((syncWord & 0xF0) | 0x04),
        (uint8_t)(((syncWord & 0x0F) << 4) | 0x04),
    };
    hal.WriteRegister(SX1262_REG_LORA_SYNC_WORD_MSB, buf, 2, SX12XX_Radio_All);
    currSyncWord = syncWord;
}

/***
 * @brief: Schedule an output power change after the next transmit (dBm, -9..+22)
 ***/
void SX1262Driver::SetOutputPower(int8_t power)
{
    const int16_t pwrNew = constrain(power, SX1262_POWER_MIN, SX1262_POWER_MAX);

    if ((pwrPending == PWRPENDING_NONE && pwrCurrent != pwrNew) || pwrPending != pwrNew)
    {
        pwrPending = pwrNew;
        DBGLN("SetPower: %d", pwrPending);
    }
}

void ICACHE_RAM_ATTR SX1262Driver::CommitOutputPower()
{
    if (pwrPending == PWRPENDING_NONE)
        return;

    pwrCurrent = pwrPending;
    pwrPending = PWRPENDING_NONE;
    WORD_ALIGNED_ATTR uint8_t buf[2] = {(uint8_t)(int8_t)pwrCurrent, SX1262_PA_RAMP};
    hal.WriteCommand(SX1262_CMD_SET_TX_PARAMS, buf, sizeof(buf), SX12XX_Radio_All);
}

void ICACHE_RAM_ATTR SX1262Driver::SetMode(SX1262_RadioOperatingModes_t mode, SX12XX_Radio_Number_t radioNumber)
{
    WORD_ALIGNED_ATTR uint8_t buf[3];

    switch (mode)
    {
    case SX1262_MODE_SLEEP:
        hal.WriteCommand(SX1262_CMD_SET_SLEEP, (uint8_t)0x00, radioNumber); // cold start: configuration is not kept
        break;

    case SX1262_MODE_STDBY_RC:
        hal.WriteCommand(SX1262_CMD_SET_STANDBY, SX1262_STDBY_RC, radioNumber);
        break;

    case SX1262_MODE_STDBY_XOSC:
        hal.WriteCommand(SX1262_CMD_SET_STANDBY, SX1262_STDBY_XOSC, radioNumber);
        break;

    case SX1262_MODE_FS:
        hal.WriteCommand(SX1262_CMD_SET_FS, radioNumber);
        break;

    case SX1262_MODE_TX:
        buf[0] = 0x00; // no timeout
        buf[1] = 0x00;
        buf[2] = 0x00;
        hal.WriteCommand(SX1262_CMD_SET_TX, buf, sizeof(buf), radioNumber);
        break;

    case SX1262_MODE_RX:
    case SX1262_MODE_RX_CONT:
        buf[0] = 0xFF; // continuous
        buf[1] = 0xFF;
        buf[2] = 0xFF;
        hal.WriteCommand(SX1262_CMD_SET_RX, buf, sizeof(buf), radioNumber);
        mode = SX1262_MODE_RX_CONT;
        break;

    default:
        break;
    }

    currOpmode = mode;
}

void ICACHE_RAM_ATTR SX1262Driver::SetFrequencyReg(uint32_t regfreq, SX12XX_Radio_Number_t radioNumber, bool doRx)
{
    currFreq = regfreq;

    // SetRfFrequency is a standby/FS command; the SX127x driver also forces standby here
    if (currOpmode != SX1262_MODE_STDBY_XOSC && currOpmode != SX1262_MODE_STDBY_RC)
    {
        SetMode(SX1262_MODE_STDBY_XOSC, radioNumber);
    }

    // SX127x step 32e6 / 2^19, SX126x step 32e6 / 2^25: exactly a factor of 64
    const uint32_t frf = regfreq << 6;
    WORD_ALIGNED_ATTR uint8_t buf[4] = {
        (uint8_t)((frf >> 24) & 0xFF),
        (uint8_t)((frf >> 16) & 0xFF),
        (uint8_t)((frf >> 8) & 0xFF),
        (uint8_t)(frf & 0xFF),
    };
    hal.WriteCommand(SX1262_CMD_SET_RF_FREQUENCY, buf, sizeof(buf), radioNumber);

    if (doRx)
    {
        RXnb();
    }
}

int32_t ICACHE_RAM_ATTR SX1262Driver::GetFrequencyError()
{
    // 20-bit two's complement at 0x076B (as RadioLib's getFrequencyError), Hz = raw * 1.55 * BW[kHz] / 1600
    WORD_ALIGNED_ATTR uint8_t reg[3] = {0};
    hal.ReadRegister(SX1262_REG_FREQ_ERROR, reg, sizeof(reg), SX12XX_Radio_1);
    int32_t efe = (((int32_t)reg[0] & 0x0F) << 16) | ((int32_t)reg[1] << 8) | reg[2];
    if (efe & 0x80000)
        efe -= 0x100000;
    const int32_t bwKHz = (currBW == SX1262_LORA_BW_500) ? 500 : (currBW == SX1262_LORA_BW_250) ? 250 : 125;
    return efe * 155 * bwKHz / 160000;
}

bool ICACHE_RAM_ATTR SX1262Driver::GetFrequencyErrorbool(SX12XX_Radio_Number_t radioNumber)
{
    // The sign bit of the estimate, the same bit the SX127x driver returns from its FEI register
    return (hal.ReadRegister(SX1262_REG_FREQ_ERROR, radioNumber) & 0x08) >> 3;
}

/////////////////////////////////////TX functions/////////////////////////////////////////

void ICACHE_RAM_ATTR SX1262Driver::TXnbISR()
{
    currOpmode = SX1262_MODE_STDBY_XOSC; // fallback mode after TX
    // The power level must be changed in standby, so this lags power changes by at most 1 packet
    CommitOutputPower();
    TXdoneCallback();
}

void ICACHE_RAM_ATTR SX1262Driver::TXnb(uint8_t * data, bool sendGeminiBuffer, uint8_t * dataGemini, SX12XX_Radio_Number_t radioNumber)
{
    transmittingRadio = radioNumber;

    // From RX (telemetry window) or an unfinished TX: back to standby first, as the SX127x driver always does.
    // After a TX done the radio is already in STDBY_XOSC and nothing is sent here.
    if (currOpmode != SX1262_MODE_STDBY_XOSC)
    {
        SetMode(SX1262_MODE_STDBY_XOSC, SX12XX_Radio_All);
    }

    if (radioNumber == SX12XX_Radio_NONE)
    {
        return;
    }

    RFAMP.TXenable(radioNumber);
    hal.WriteBuffer(SX1262_TX_BUFFER_BASE, data, PayloadLength, SX12XX_Radio_1);
    // A flag left set would keep DIO1 high and the TX_DONE edge would never come
    ClearIrqStatus(SX1262_IRQ_ALL, SX12XX_Radio_1);
    SetMode(SX1262_MODE_TX, SX12XX_Radio_1);
}

///////////////////////////////////RX Functions Non-Blocking///////////////////////////////////////////

bool ICACHE_RAM_ATTR SX1262Driver::RXnbISR(uint16_t irqStatus, SX12XX_Radio_Number_t radioNumber)
{
    // No hardware CRC and an implicit header (as SX127x ELRS): RX_DONE is a packet, ELRS checks its own CRC.
    WORD_ALIGNED_ATTR uint8_t status[2] = {0};
    hal.ReadCommand(SX1262_CMD_GET_RX_BUFFER_STATUS, status, 2, radioNumber); // payload length, start pointer
    hal.ReadBuffer(status[1], RXdataBuffer, PayloadLength, radioNumber);
    return RXdoneCallback(SX12XX_RX_OK);
}

void ICACHE_RAM_ATTR SX1262Driver::RXnb()
{
    RFAMP.RXenable();
    ClearIrqStatus(SX1262_IRQ_ALL, SX12XX_Radio_1);
    SetMode(SX1262_MODE_RX_CONT, SX12XX_Radio_1);
}

void ICACHE_RAM_ATTR SX1262Driver::CheckForSecondPacket()
{
    // single radio
    gotRadio[0] = true;
    gotRadio[1] = false;
    hasSecondRadioGotData = false;
}

void ICACHE_RAM_ATTR SX1262Driver::GetLastPacketStats()
{
    WORD_ALIGNED_ATTR uint8_t status[3] = {0};
    hal.ReadCommand(SX1262_CMD_GET_PACKET_STATUS, status, 3, SX12XX_Radio_1); // RssiPkt, SnrPkt, SignalRssiPkt

    int8_t rssi = -(int8_t)(status[0] / 2);
    const int8_t snr = (int8_t)status[1]; // 1/4 dB, as the SX127x's RegPktSnrValue (RADIO_SNR_SCALE 4)
    // Same correction the SX127x driver applies (SX1276 datasheet 3.5.5): below 0 dB SNR, add SNR to the RSSI
    if (snr < 0)
        rssi += snr / RADIO_SNR_SCALE;

    LastPacketRSSI = rssi;
    LastPacketSNRRaw = snr;
    gotRadio[0] = true;
    gotRadio[1] = false;
    strongestReceivingRadio = SX12XX_Radio_1;
}

int8_t ICACHE_RAM_ATTR SX1262Driver::GetLastPacketRSSI(SX12XX_Radio_Number_t radioNumber)
{
    WORD_ALIGNED_ATTR uint8_t status[3] = {0};
    hal.ReadCommand(SX1262_CMD_GET_PACKET_STATUS, status, 3, SX12XX_Radio_1);
    return -(int8_t)(status[0] / 2);
}

int8_t ICACHE_RAM_ATTR SX1262Driver::GetLastPacketSNRRaw(SX12XX_Radio_Number_t radioNumber)
{
    WORD_ALIGNED_ATTR uint8_t status[3] = {0};
    hal.ReadCommand(SX1262_CMD_GET_PACKET_STATUS, status, 3, SX12XX_Radio_1);
    return (int8_t)status[1];
}

int8_t ICACHE_RAM_ATTR SX1262Driver::GetCurrRSSI(SX12XX_Radio_Number_t radioNumber)
{
    WORD_ALIGNED_ATTR uint8_t rssi[1] = {0};
    hal.ReadCommand(SX1262_CMD_GET_RSSI_INST, rssi, 1, SX12XX_Radio_1);
    return -(int8_t)(rssi[0] / 2);
}

uint16_t ICACHE_RAM_ATTR SX1262Driver::GetIrqStatus(SX12XX_Radio_Number_t radioNumber)
{
    WORD_ALIGNED_ATTR uint8_t status[2] = {0};
    hal.ReadCommand(SX1262_CMD_GET_IRQ_STATUS, status, 2, SX12XX_Radio_1);
    return ((uint16_t)status[0] << 8) | status[1];
}

void ICACHE_RAM_ATTR SX1262Driver::ClearIrqStatus(uint16_t irqMask, SX12XX_Radio_Number_t radioNumber)
{
    WORD_ALIGNED_ATTR uint8_t buf[2] = {(uint8_t)(irqMask >> 8), (uint8_t)(irqMask & 0xFF)};
    hal.WriteCommand(SX1262_CMD_CLEAR_IRQ_STATUS, buf, sizeof(buf), SX12XX_Radio_1);
}

void ICACHE_RAM_ATTR SX1262Driver::IsrCallback_1()
{
    instance->IsrCallback(SX12XX_Radio_1);
}

void ICACHE_RAM_ATTR SX1262Driver::IsrCallback(SX12XX_Radio_Number_t radioNumber)
{
    instance->processingPacketRadio = radioNumber;

    const uint16_t irqStatus = instance->GetIrqStatus(radioNumber);
    if (irqStatus & SX1262_IRQ_TX_DONE)
    {
        RFAMP.TXRXdisable();
        instance->TXnbISR();
    }
    else if (irqStatus & SX1262_IRQ_RX_DONE)
    {
        instance->RXnbISR(irqStatus, radioNumber);
    }
    else if (irqStatus == SX1262_IRQ_NONE)
    {
        return;
    }

    instance->ClearIrqStatus(SX1262_IRQ_ALL, SX12XX_Radio_1);
}

#endif // RADIO_SX1262 && !UNIT_TEST
