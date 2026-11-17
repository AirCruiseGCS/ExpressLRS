#if defined(RADIO_SX1262) && !defined(UNIT_TEST)

#include "SX1262_hal.h"
#include "logging.h"
#include <SPIEx.h>
#include "AXP2101.h"

#if !defined(PLATFORM_ESP32)
#error "The SX1262 driver is only written for ESP32 targets"
#endif

#include <soc/spi_struct.h>

SX1262Hal *SX1262Hal::instance = NULL;

// The SX1262 is the only device on its bus: every transfer selects CS 0 only.
static constexpr uint8_t CS_MASK = SX12XX_Radio_1;

SX1262Hal::SX1262Hal()
{
    instance = this;
}

void SX1262Hal::end()
{
    detachInterrupt(GPIO_PIN_DIO1);
    SPIEx.end();
    IsrCallback_1 = nullptr;
    IsrCallback_2 = nullptr;
}

/**
 * The radio's supply. On the LilyGO T-Watch S3 the SX1262 is powered from the AXP2101 PMU's ALDO4, which has to be
 * set to 3.3 V and switched on before the radio is reset (as LilyGO's own firmware and the twatch-elrs probes do).
 * Build flag SX1262_POWER_AXP2101_ALDO4; the I2C pins are the layout's i2c_sda / i2c_scl (setupTargetCommon() has
 * already started Wire on them when they are defined).
 */
void SX1262Hal::powerUpRadio()
{
#if defined(SX1262_POWER_AXP2101_ALDO4)
    if (!AXP2101::begin() || !AXP2101::enableAldo(4, 3300))
    {
        ERRLN("SX1262: AXP2101 not answering, the radio may be unpowered");
        return;
    }
    delay(50); // the probes waited 50 ms between enabling ALDO4 and resetting the radio
#endif
}

void SX1262Hal::init()
{
    DBGLN("SX1262 Hal Init");

    powerUpRadio();

    pinMode(GPIO_PIN_BUSY, INPUT);
    pinMode(GPIO_PIN_DIO1, INPUT);
    pinMode(GPIO_PIN_NSS, OUTPUT);
    digitalWrite(GPIO_PIN_NSS, HIGH);

    SPIEx.begin(GPIO_PIN_SCK, GPIO_PIN_MISO, GPIO_PIN_MOSI, GPIO_PIN_NSS); // sck, miso, mosi, ss
    gpio_pullup_en((gpio_num_t)GPIO_PIN_MISO);
    SPIEx.setFrequency(SX1262_SPI_FREQUENCY);
    SPIEx.setHwCs(true);
    spiEnableSSPins(SPIEx.bus(), CS_MASK);

    attachInterrupt(digitalPinToInterrupt(GPIO_PIN_DIO1), this->dioISR_1, RISING);
}

void SX1262Hal::reset()
{
    DBGLN("SX1262 Reset");

    if (GPIO_PIN_RST != UNDEF_PIN)
    {
        pinMode(GPIO_PIN_RST, OUTPUT);
        digitalWrite(GPIO_PIN_RST, LOW);
        delay(2);   // datasheet: >= 100 us
        digitalWrite(GPIO_PIN_RST, HIGH);
        delay(10);  // BUSY stays high for ~3.5 ms (cold start) after reset
    }

    WaitOnBusy(20000);
    DBGLN("SX1262 Ready!");
}

void ICACHE_RAM_ATTR SX1262Hal::WriteCommand(SX1262_RadioCommands_t opcode, uint8_t val, SX12XX_Radio_Number_t radioNumber)
{
    WriteCommand(opcode, &val, 1, radioNumber);
}

void ICACHE_RAM_ATTR SX1262Hal::WriteCommand(SX1262_RadioCommands_t opcode, SX12XX_Radio_Number_t radioNumber)
{
    WriteCommand(opcode, nullptr, 0, radioNumber);
}

void ICACHE_RAM_ATTR SX1262Hal::WriteCommand(SX1262_RadioCommands_t opcode, const uint8_t *buffer, uint8_t size, SX12XX_Radio_Number_t radioNumber)
{
    WORD_ALIGNED_ATTR uint8_t OutBuffer[WORD_PADDED(size + 1)] = {
        (uint8_t)opcode,
    };

    if (size)
        memcpy(OutBuffer + 1, buffer, size);

    WaitOnBusy();
    SPIEx.write(CS_MASK, OutBuffer, size + 1);
}

uint8_t ICACHE_RAM_ATTR SX1262Hal::ReadCommand(SX1262_RadioCommands_t opcode, uint8_t *buffer, uint8_t size, SX12XX_Radio_Number_t radioNumber)
{
    WORD_ALIGNED_ATTR uint8_t OutBuffer[WORD_PADDED(size + 2)] = {
        (uint8_t)opcode,
        0x00,
    };

    WaitOnBusy();
    SPIEx.read(CS_MASK, OutBuffer, size + 2); // opcode, status, data...
    memcpy(buffer, OutBuffer + 2, size);
    return OutBuffer[1];
}

void ICACHE_RAM_ATTR SX1262Hal::WriteRegister(uint16_t address, const uint8_t *buffer, uint8_t size, SX12XX_Radio_Number_t radioNumber)
{
    WORD_ALIGNED_ATTR uint8_t OutBuffer[WORD_PADDED(size + 3)] = {
        SX1262_CMD_WRITE_REGISTER,
        (uint8_t)((address & 0xFF00) >> 8),
        (uint8_t)(address & 0x00FF),
    };

    memcpy(OutBuffer + 3, buffer, size);

    WaitOnBusy();
    SPIEx.write(CS_MASK, OutBuffer, size + 3);
}

void ICACHE_RAM_ATTR SX1262Hal::WriteRegister(uint16_t address, uint8_t value, SX12XX_Radio_Number_t radioNumber)
{
    WriteRegister(address, &value, 1, radioNumber);
}

void ICACHE_RAM_ATTR SX1262Hal::ReadRegister(uint16_t address, uint8_t *buffer, uint8_t size, SX12XX_Radio_Number_t radioNumber)
{
    WORD_ALIGNED_ATTR uint8_t OutBuffer[WORD_PADDED(size + 4)] = {
        SX1262_CMD_READ_REGISTER,
        (uint8_t)((address & 0xFF00) >> 8),
        (uint8_t)(address & 0x00FF),
        0x00,
    };

    WaitOnBusy();
    SPIEx.read(CS_MASK, OutBuffer, size + 4);
    memcpy(buffer, OutBuffer + 4, size);
}

uint8_t ICACHE_RAM_ATTR SX1262Hal::ReadRegister(uint16_t address, SX12XX_Radio_Number_t radioNumber)
{
    uint8_t data;
    ReadRegister(address, &data, 1, radioNumber);
    return data;
}

void ICACHE_RAM_ATTR SX1262Hal::WriteBuffer(uint8_t offset, const uint8_t *buffer, uint8_t size, SX12XX_Radio_Number_t radioNumber)
{
    WORD_ALIGNED_ATTR uint8_t OutBuffer[WORD_PADDED(size + 2)] = {
        SX1262_CMD_WRITE_BUFFER,
        offset,
    };

    memcpy(OutBuffer + 2, buffer, size);

    WaitOnBusy();
    SPIEx.write(CS_MASK, OutBuffer, size + 2);
}

void ICACHE_RAM_ATTR SX1262Hal::ReadBuffer(uint8_t offset, uint8_t *buffer, uint8_t size, SX12XX_Radio_Number_t radioNumber)
{
    WORD_ALIGNED_ATTR uint8_t OutBuffer[WORD_PADDED(size + 3)] = {
        SX1262_CMD_READ_BUFFER,
        offset,
        0x00,
    };

    WaitOnBusy();
    SPIEx.read(CS_MASK, OutBuffer, size + 3);
    memcpy(buffer, OutBuffer + 3, size);
}

bool ICACHE_RAM_ATTR SX1262Hal::WaitOnBusy(uint32_t timeoutUs)
{
    // SPIEx.write() returns while the bytes are still being clocked out. BUSY only rises once NSS goes high again
    // (and up to 600 ns after it), so a BUSY check right behind a write could see the chip as free when it is not.
    // Wait for the SPI peripheral to finish, give BUSY time to rise, then poll it.
    spi_dev_t *spi = *(reinterpret_cast<spi_dev_t **>(SPIEx.bus()));
    while (spi->cmd.usr) {}
    delayMicroseconds(1);

    const uint32_t startTime = micros();
    while (digitalRead(GPIO_PIN_BUSY) == HIGH)
    {
        if ((micros() - startTime) > timeoutUs)
            return false;
    }
    return true;
}

void ICACHE_RAM_ATTR SX1262Hal::dioISR_1()
{
    if (instance->IsrCallback_1)
        instance->IsrCallback_1();
}

#endif // RADIO_SX1262 && !UNIT_TEST
