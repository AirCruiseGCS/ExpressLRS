#pragma once

// SX1262 SPI/GPIO layer for ELRS, modelled on lib/SX1280Driver/SX1280_hal.* (the SX126x SPI framing is the same:
// a read command is opcode + status byte + data, a register read is opcode + 2-byte address + status + data, a buffer
// read is opcode + offset + status + data; every transfer waits for BUSY low first).
// Single radio only: the radio-number arguments are kept for the ELRS API, the second radio is never used.

#include "targets.h"
#include "SX1262_Regs.h"
#include "SX12xxDriverCommon.h"

class SX1262Hal
{
public:
    static SX1262Hal *instance;

    SX1262Hal();

    void init();
    void end();
    void reset();

    void ICACHE_RAM_ATTR WriteCommand(SX1262_RadioCommands_t opcode, uint8_t val, SX12XX_Radio_Number_t radioNumber);
    void ICACHE_RAM_ATTR WriteCommand(SX1262_RadioCommands_t opcode, const uint8_t *buffer, uint8_t size, SX12XX_Radio_Number_t radioNumber);
    void ICACHE_RAM_ATTR WriteCommand(SX1262_RadioCommands_t opcode, SX12XX_Radio_Number_t radioNumber); // no parameters

    // returns the status byte (the byte clocked out while the chip receives the NOP after the opcode)
    uint8_t ICACHE_RAM_ATTR ReadCommand(SX1262_RadioCommands_t opcode, uint8_t *buffer, uint8_t size, SX12XX_Radio_Number_t radioNumber);

    void ICACHE_RAM_ATTR WriteRegister(uint16_t address, const uint8_t *buffer, uint8_t size, SX12XX_Radio_Number_t radioNumber);
    void ICACHE_RAM_ATTR WriteRegister(uint16_t address, uint8_t value, SX12XX_Radio_Number_t radioNumber);
    void ICACHE_RAM_ATTR ReadRegister(uint16_t address, uint8_t *buffer, uint8_t size, SX12XX_Radio_Number_t radioNumber);
    uint8_t ICACHE_RAM_ATTR ReadRegister(uint16_t address, SX12XX_Radio_Number_t radioNumber);

    void ICACHE_RAM_ATTR WriteBuffer(uint8_t offset, const uint8_t *buffer, uint8_t size, SX12XX_Radio_Number_t radioNumber);
    void ICACHE_RAM_ATTR ReadBuffer(uint8_t offset, uint8_t *buffer, uint8_t size, SX12XX_Radio_Number_t radioNumber);

    // Waits for the previous SPI transfer to leave the bus, then for BUSY low. Calibration and reset need far longer than
    // the 1 ms the hot path allows, hence the parameter.
    bool ICACHE_RAM_ATTR WaitOnBusy(uint32_t timeoutUs = 1000U);

    static ICACHE_RAM_ATTR void dioISR_1();
    void (*IsrCallback_1)(); // function pointer for callback
    void (*IsrCallback_2)(); // never used (single radio), kept for symmetry with the other drivers

private:
    void powerUpRadio();
};
