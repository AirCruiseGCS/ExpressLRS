#pragma once

// Semtech SX1261/SX1262 (DS_SX1261-2 rev 2.1) - the opcodes, registers and constants the ELRS SX1262 driver uses.
// The SX1262 here talks to SX127x devices (ELRS 900 MHz LoRa), see SX1262.cpp for what that requires.

#include <stdint.h>

// ---- commands (datasheet chapter 11/13) ----
enum SX1262_RadioCommands_t : uint8_t
{
    SX1262_CMD_SET_SLEEP                  = 0x84,
    SX1262_CMD_SET_STANDBY                = 0x80,
    SX1262_CMD_SET_FS                     = 0xC1,
    SX1262_CMD_SET_TX                     = 0x83,
    SX1262_CMD_SET_RX                     = 0x82,
    SX1262_CMD_STOP_TIMER_ON_PREAMBLE     = 0x9F,
    SX1262_CMD_SET_REGULATOR_MODE         = 0x96,
    SX1262_CMD_CALIBRATE                  = 0x89,
    SX1262_CMD_CALIBRATE_IMAGE            = 0x98,
    SX1262_CMD_SET_PA_CONFIG              = 0x95,
    SX1262_CMD_SET_RX_TX_FALLBACK_MODE    = 0x93,
    SX1262_CMD_WRITE_REGISTER             = 0x0D,
    SX1262_CMD_READ_REGISTER              = 0x1D,
    SX1262_CMD_WRITE_BUFFER               = 0x0E,
    SX1262_CMD_READ_BUFFER                = 0x1E,
    SX1262_CMD_SET_DIO_IRQ_PARAMS         = 0x08,
    SX1262_CMD_GET_IRQ_STATUS             = 0x12,
    SX1262_CMD_CLEAR_IRQ_STATUS           = 0x02,
    SX1262_CMD_SET_DIO2_AS_RF_SWITCH_CTRL = 0x9D,
    SX1262_CMD_SET_DIO3_AS_TCXO_CTRL      = 0x97,
    SX1262_CMD_SET_RF_FREQUENCY           = 0x86,
    SX1262_CMD_SET_PACKET_TYPE            = 0x8A,
    SX1262_CMD_GET_PACKET_TYPE            = 0x11,
    SX1262_CMD_SET_TX_PARAMS              = 0x8E,
    SX1262_CMD_SET_MODULATION_PARAMS      = 0x8B,
    SX1262_CMD_SET_PACKET_PARAMS          = 0x8C,
    SX1262_CMD_SET_BUFFER_BASE_ADDRESS    = 0x8F,
    SX1262_CMD_SET_LORA_SYMB_NUM_TIMEOUT  = 0xA0,
    SX1262_CMD_GET_STATUS                 = 0xC0,
    SX1262_CMD_GET_RSSI_INST              = 0x15,
    SX1262_CMD_GET_RX_BUFFER_STATUS       = 0x13,
    SX1262_CMD_GET_PACKET_STATUS          = 0x14,
    SX1262_CMD_GET_DEVICE_ERRORS          = 0x17,
    SX1262_CMD_CLEAR_DEVICE_ERRORS        = 0x07,
    SX1262_CMD_SET_TX_CONTINUOUS_WAVE     = 0xD1,
};

// ---- registers ----
#define SX1262_REG_VERSION_STRING      0x0320  // "SX1261 V2D 2D02" etc., 16 bytes
#define SX1262_REG_IQ_POLARITY         0x0736  // errata 15.4: bit 2 set for standard IQ, cleared for inverted
#define SX1262_REG_LORA_SYNC_WORD_MSB  0x0740
#define SX1262_REG_LORA_SYNC_WORD_LSB  0x0741
#define SX1262_REG_FREQ_ERROR          0x076B  // 20-bit, two's complement (undocumented, as used by RadioLib)
#define SX1262_REG_SENSITIVITY_CONFIG  0x0889  // errata 15.1: bit 2 cleared for LoRa BW500, set otherwise
#define SX1262_REG_RX_GAIN             0x08AC  // 0x94 power saving, 0x96 boosted
#define SX1262_REG_TX_CLAMP_CONFIG     0x08D8  // errata 15.2: |= 0x1E
#define SX1262_REG_OCP_CONFIGURATION   0x08E7

#define SX1262_RX_GAIN_BOOSTED         0x96

// ---- SetStandby ----
#define SX1262_STDBY_RC                0x00
#define SX1262_STDBY_XOSC              0x01

// ---- SetRxTxFallbackMode ----
#define SX1262_FALLBACK_FS             0x40
#define SX1262_FALLBACK_STDBY_XOSC     0x30
#define SX1262_FALLBACK_STDBY_RC       0x20

// ---- SetPacketType ----
#define SX1262_PACKET_TYPE_GFSK        0x00
#define SX1262_PACKET_TYPE_LORA        0x01

// ---- SetModulationParams (LoRa) ----
#define SX1262_LORA_BW_125             0x04
#define SX1262_LORA_BW_250             0x05
#define SX1262_LORA_BW_500             0x06
#define SX1262_LORA_CR_4_5             0x01
#define SX1262_LORA_CR_4_6             0x02
#define SX1262_LORA_CR_4_7             0x03
#define SX1262_LORA_CR_4_8             0x04
#define SX1262_LORA_LDRO_OFF           0x00

// ---- SetPacketParams (LoRa) ----
#define SX1262_LORA_HEADER_EXPLICIT    0x00
#define SX1262_LORA_HEADER_IMPLICIT    0x01
#define SX1262_LORA_CRC_OFF            0x00
#define SX1262_LORA_IQ_STANDARD        0x00
#define SX1262_LORA_IQ_INVERTED        0x01

// ---- IRQ (GetIrqStatus / SetDioIrqParams / ClearIrqStatus) ----
#define SX1262_IRQ_TX_DONE             0x0001
#define SX1262_IRQ_RX_DONE             0x0002
#define SX1262_IRQ_PREAMBLE_DETECTED   0x0004
#define SX1262_IRQ_SYNC_WORD_VALID     0x0008
#define SX1262_IRQ_HEADER_VALID        0x0010
#define SX1262_IRQ_HEADER_ERR          0x0020
#define SX1262_IRQ_CRC_ERR             0x0040
#define SX1262_IRQ_CAD_DONE            0x0080
#define SX1262_IRQ_CAD_DETECTED        0x0100
#define SX1262_IRQ_TIMEOUT             0x0200
#define SX1262_IRQ_NONE                0x0000
#define SX1262_IRQ_ALL                 0x03FF

// ---- Calibrate ----
#define SX1262_CALIBRATE_ALL           0x7F

// ---- SetRegulatorMode ----
#define SX1262_REGULATOR_LDO           0x00
#define SX1262_REGULATOR_DCDC          0x01

// ---- SetPaConfig, SX1262 high-power PA, up to +22 dBm (datasheet table 13-21) ----
#define SX1262_PA_DUTY_CYCLE           0x04
#define SX1262_PA_HP_MAX               0x07
#define SX1262_PA_DEVICE_SEL_SX1262    0x00
#define SX1262_PA_LUT                  0x01
#define SX1262_POWER_MIN               (-9)
#define SX1262_POWER_MAX               (22)

// ---- SetTxParams ramp time ----
#define SX1262_RAMP_10U                0x00
#define SX1262_RAMP_20U                0x01
#define SX1262_RAMP_40U                0x02
#define SX1262_RAMP_80U                0x03
#define SX1262_RAMP_200U               0x04

// ---- SetDIO3AsTCXOCtrl voltages ----
#define SX1262_TCXO_1_6V               0x00
#define SX1262_TCXO_1_7V               0x01
#define SX1262_TCXO_1_8V               0x02
#define SX1262_TCXO_2_2V               0x03
#define SX1262_TCXO_2_4V               0x04
#define SX1262_TCXO_2_7V               0x05
#define SX1262_TCXO_3_0V               0x06
#define SX1262_TCXO_3_3V               0x07

// ---- status byte (GetStatus and the 2nd byte of every read) ----
#define SX1262_STATUS_MODE_MASK        0x70
#define SX1262_STATUS_MODE_STDBY_RC    0x20
#define SX1262_STATUS_MODE_STDBY_XOSC  0x30
#define SX1262_STATUS_MODE_FS          0x40
#define SX1262_STATUS_MODE_RX          0x50
#define SX1262_STATUS_MODE_TX          0x60

// ---- board options (build flags; the defaults are the LilyGO T-Watch S3's) ----
// DIO3 drives the TCXO: voltage code above, or -1 for a plain crystal
#ifndef SX1262_DIO3_TCXO_VOLTAGE
#define SX1262_DIO3_TCXO_VOLTAGE       SX1262_TCXO_1_8V
#endif
// TCXO start-up time, in 15.625 us steps (320 = 5 ms, RadioLib's default)
#ifndef SX1262_TCXO_DELAY_STEPS
#define SX1262_TCXO_DELAY_STEPS        320
#endif
// DIO2 drives the RF switch (SetDIO2AsRfSwitchCtrl): 1 yes, 0 no
#ifndef SX1262_DIO2_RF_SWITCH
#define SX1262_DIO2_RF_SWITCH          1
#endif
// SPI clock; the SX126x allows up to 16 MHz
#ifndef SX1262_SPI_FREQUENCY
#define SX1262_SPI_FREQUENCY           10000000
#endif

typedef enum
{
    SX1262_MODE_SLEEP = 0x00,
    SX1262_MODE_STDBY_RC,
    SX1262_MODE_STDBY_XOSC,
    SX1262_MODE_FS,
    SX1262_MODE_TX,
    SX1262_MODE_RX,
    SX1262_MODE_RX_CONT,
    SX1262_MODE_CAD
} SX1262_RadioOperatingModes_t;
