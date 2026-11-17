#pragma once

// ELRS driver for a Semtech SX1262 that has to talk to SX127x devices (ELRS 900 MHz LoRa). It stands in for
// SX127xDriver in a RADIO_SX127X build (lib/SX127xDriver/SX127xDriver.h aliases SX127xDriver to it when RADIO_SX1262
// is defined), so it takes the SX127x's arguments: bandwidth / SF / CR as SX127x register enums, frequencies as SX127x
// register values (FREQ_STEP 61.03515625 Hz), the SX127x sync word byte. Single radio only (no Gemini).

#include "targets.h"
#include "SX1262_Regs.h"
#include "SX127xRegs.h"          // the SX127x enums the rate tables in common.cpp are written in
#include "SX12xxDriverCommon.h"

#define RADIO_SNR_SCALE 4

class SX1262Driver : public SX12xxDriverCommon
{
public:
    static SX1262Driver *instance;

    ///////////Radio Variables////////
    uint8_t currSyncWord;   // the SX127x sync word byte ELRS uses (0x12); written to the SX1262 as 0x1424

    ////////////////Configuration Functions/////////////
    SX1262Driver();
    bool Begin(uint32_t minimumFrequency, uint32_t maximumFrequency);
    void End();
    void Config(uint8_t bw, uint8_t sf, uint8_t cr, uint32_t freq, uint8_t preambleLen, uint8_t syncWord, bool InvertIQ, uint8_t _PayloadLength);
    void Config(uint8_t bw, uint8_t sf, uint8_t cr, uint32_t freq, uint8_t preambleLen, bool InvertIQ, uint8_t _PayloadLength);
    void SetMode(SX1262_RadioOperatingModes_t mode, SX12XX_Radio_Number_t radioNumber);
    void SetTxIdleMode() { SetMode(SX1262_MODE_STDBY_XOSC, SX12XX_Radio_All); } // set Idle mode used when switching from RX to TX

    void startCWTest(uint32_t freq, SX12XX_Radio_Number_t radioNumber);
    void cwRepeat(SX12XX_Radio_Number_t radioNumber) {} // SetTxContinuousWave keeps going by itself

    void SetOutputPower(int8_t power); // dBm, -9..+22; applied after the next TX done (like the SX127x driver)

    #define FREQ_STEP 61.03515625      // the SX127x's grid - keeps FHSS identical to SX127x devices
    void SetFrequencyReg(uint32_t freq, SX12XX_Radio_Number_t radioNumber, bool doRx = false);
    // Frequency error: implemented for completeness, but not offered to ELRS (FrequencyErrorAvailable() is false),
    // the same as the LR1121 driver. The TX never uses it; an SX1262 *receiver* would run without FreqCorrection.
    bool FrequencyErrorAvailable() const { return false; }
    int32_t GetFrequencyError();
    bool GetFrequencyErrorbool(SX12XX_Radio_Number_t radioNumber);
    void SetPPMoffsetReg(int32_t offset, SX12XX_Radio_Number_t radioNumber) {} // the SX126x has no PPM offset register

    ////////////////////////////////////////////////////

    /////////////////Utility Functions//////////////////
    uint16_t GetIrqStatus(SX12XX_Radio_Number_t radioNumber);
    void ClearIrqStatus(uint16_t irqMask, SX12XX_Radio_Number_t radioNumber);

    //////////////RX related Functions/////////////////
    int8_t GetLastPacketRSSI(SX12XX_Radio_Number_t radioNumber);
    int8_t GetLastPacketSNRRaw(SX12XX_Radio_Number_t radioNumber);
    int8_t GetCurrRSSI(SX12XX_Radio_Number_t radioNumber);
    void StartRssiInst(SX12XX_Radio_Number_t radioNumber) {}
    int8_t GetRssiInst(SX12XX_Radio_Number_t radioNumber) { return GetCurrRSSI(radioNumber); }
    void GetLastPacketStats();
    void CheckForSecondPacket();

    ////////////Non-blocking TX related Functions/////////////////
    void TXnb(uint8_t * data, bool sendGeminiBuffer, uint8_t * dataGemini, SX12XX_Radio_Number_t radioNumber);
    /////////////Non-blocking RX related Functions///////////////
    void RXnb();

private:
    // constant used for no power change pending
    // must not be a valid power value
    static const int16_t PWRPENDING_NONE = -127;

    SX1262_RadioOperatingModes_t currOpmode;
    uint8_t currBW;          // SX126x code
    uint8_t currSF;
    uint8_t currCR;          // SX126x code
    uint8_t currPreambleLen;
    int16_t pwrCurrent;
    int16_t pwrPending;

    static void IsrCallback_1();
    static void IsrCallback(SX12XX_Radio_Number_t radioNumber);
    bool RXnbISR(uint16_t irqStatus, SX12XX_Radio_Number_t radioNumber); // ISR for non-blocking RX routine
    void TXnbISR(); // ISR for non-blocking TX routine
    void CommitOutputPower();
    void CalibrateImage(uint32_t minimumFrequency, uint32_t maximumFrequency);
    void SetSyncWord(uint8_t syncWord);
};
