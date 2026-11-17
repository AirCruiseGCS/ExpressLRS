# T-Watch S3 as an ELRS 4.1 900 MHz TX (SX1262) - build and flash

Env: `TWatch_S3_900_TX_via_UART` (`targets/twatch/twatch-s3-tx.ini`). FCC915, bind phrase "1991"
(UID 194,121,138,79,237,137), hardware layout `targets/twatch/twatch-s3-tx-layout.json`.

## Build

```sh
export PATH=$HOME/.local/bin:$PATH
cd ~/Projects/twatch-elrs/ExpressLRS/src
PYTHONPATH=$PWD/python/external/esptool pio run -e TWatch_S3_900_TX_via_UART
```

`PYTHONPATH` is needed on this machine only: the esptool in `~/.local/lib/python3.10/site-packages` (PlatformIO's)
shadows the copy ELRS bundles in `python/external/esptool`, and ELRS's build scripts then fail with
`ImportError: cannot import name 'make_image' from 'esptool.cmds'`. Nothing in the fork is changed for that.

Output, `.pio/build/TWatch_S3_900_TX_via_UART/`:
- `firmware.bin` - the app, with product name, options JSON (domain, UID) and the hardware layout appended
  (python/twatch_layout.py, same format as UnifiedConfiguration.py)
- `bootloader.bin`, `partitions.bin`, `boot_app0.bin`
- `firmware-merged.bin` - all four merged, flashable at 0x0

## Flash (NOT run yet - the watch's factory image is backed up in ~/Projects/twatch-backup/)

The flash still holds the factory firmware's data partitions; ELRS reads `/hardware.json` and `/options.json` from
LittleFS before the embedded ones, so erase first:

```sh
cd ~/Projects/twatch-elrs/ExpressLRS/src/.pio/build/TWatch_S3_900_TX_via_UART
esptool --chip esp32s3 --port /dev/ttyACM0 erase-flash
esptool --chip esp32s3 --port /dev/ttyACM0 --baud 460800 write-flash -z \
  --flash-mode dio --flash-freq 40m --flash-size detect \
  0x0 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.bin
```

or, the same in one file: `esptool --chip esp32s3 --port /dev/ttyACM0 --baud 460800 write-flash -z --flash-mode dio
--flash-freq 40m --flash-size detect 0x0 firmware-merged.bin`.
(esptool v4 spells them `erase_flash`, `write_flash`, `--flash_mode` ...; the addresses are those of
python/binary_flash.py for ESP32-S3.)

Back to the factory firmware: `esptool --port /dev/ttyACM0 write-flash 0 ~/Projects/twatch-backup/twatch-s3-factory-64e833790ac8.bin`.

## Starting without a handset

`-DDEBUG_TX_FREERUN` (an existing ELRS flag): devHandset.cpp calls the handset's "connected" callback at start-up, so
tx_main's UARTconnected() starts the packet timer; after the awaitingModelId delay the TX sends SYNC + RC packets.
The fork adds, under the same flag, centred ChannelData with AUX1 (arm) low instead of all zeros (tx_main.cpp setup()).
`-DDEBUG_TX_FREERUN_MODEL_MATCH` switches Model Match on (model id 0) and commits it at boot: the bench receiver holds
model id 0 (the reference TX has Model Match on), and without a handset there is no Lua to switch it on.
The default rate of a fresh config is 50 Hz (SF8) in an SX1262 build (config.cpp) - the rate verified on the bench.
Power levels: 10/25/50/100 mW = 10/14/17/20 dBm, default 10 mW.

## On the bench, 17.11.2026

- First boot panicked in `AutoDetect::Begin()` (RMT on an undefined CRSF pin): with no `serial_rx`/`serial_tx` in the
  layout both are UNDEF, `devHandset` takes the "RX == TX" half-duplex auto-detect path and configures RMT on pin -1.
  Fixed in the layout: `serial_rx` 41, `serial_tx` 42 - the watch's GPS-shield pins, unused on a plain T-Watch S3 - so
  the plain `CRSFHandset` (UART) is used and `DEBUG_TX_FREERUN` starts the link.
- Then **linked to the receiver** (the one bound to the BetaFPV Micro TX V2, 50 Hz, Model Match model 0): CRSF
  LINK_STATISTICS over USB - uplink LQ 100 %, RSSI -86 dBm, SNR +7; downlink (telemetry) LQ 100 % after ~18 s,
  RSSI -62 dBm, SNR +11; power index 1 (25 mW). The receiver's LED solid. No drops in 40 s.
- Diagnostics without a log port: the TX's USB carries CRSF to the (absent) handset - LINK_STATISTICS (0x14) every
  200 ms; RSSI bytes are int8.

## Packet rates, 17.11.2026 (bench flag `-DDEBUG_TX_FREERUN_RATE=<rate>`; the receiver power-cycled before each)

| Rate | LoRa | Result |
|---|---|---|
| 50 Hz | SF8 | linked: up LQ 100 %, down LQ 100 % |
| 100 Hz | SF7 | linked: up LQ 100 %, RSSI -80 dBm; telemetry rising as at 50 Hz |
| 200 Hz | SF6 | **no link** (60 s on a freshly started receiver) |

SF6 is not interoperable: the SX1261/2 datasheet says its SF5/SF6 were changed to work with an explicit header too, and
"SF6 will not be backward compatible with the SF6 used on SX1276". ELRS has an undocumented compatibility register for
the LR1121 (CorrectRegisterForSF6, from Semtech by email); none is known for the SX126x. So the watch has 25 / 50 / 100 Hz
against SX127x receivers; 200 Hz, 100 Hz Full and D50 (all SF6) do not link. To do (WE1.5): offer only those rates in an
SX1262 build. 25 Hz (SF9) not yet tried.

Also: an ELRS receiver left without a link for ~60 s starts its WiFi and stops listening - power-cycle it between tries.

## The screens (WE2.1, built, not yet flashed)

The owner's three designs (Design/extracted/01..03, 240x240 CSS px = the panel's pixels) as the watch's own UI,
lib/TWATCH_UI, build flag `TWATCH_UI` (ELRS's lib/SCREEN stays off: the layout has the `screen_*` pins but no
`screen_type`). Panel: ST7789 240x240 (a window of 240x320, rows offset 80 at rotation 2; `screen_reversed` -> 0),
IPS, 40 MHz on HSPI = SPI3 (the radio's SPIEx is SPI2); pins `screen_cs` 12, `screen_dc` 38, `screen_sck` 18,
`screen_mosi` 13, `screen_bl` 45 (active high), no RST / MISO. Supply: AXP2101 ALDO2 = 3.3 V
(`-DSCREEN_POWER_AXP2101_ALDO2`, lib/AXP2101 - also the radio's ALDO4 now), left on while the screen sleeps.

- 03 WATCH is the home screen (`-DTWATCH_UI_START_SCREEN=2`; 0 = 01 LINK, 1 = 02 RADIO).
- No input yet (WE2.2). Bench: `-DTWATCH_UI_CYCLE_MS=5000` shows the next screen every 5 s while the screen is on;
  `-DTWATCH_UI_WAKE_ON_EVENTS` switches it on for bind started (02), link lost / regained (01), arm change (03).
  Hooks for the input code: `twatchUiWake()` (home screen) and `twatchUiShowScreen(s)`, lib/TWATCH_UI/devTWatchUI.h.
- Screen timeout `TWATCH_UI_SCREEN_TIMEOUT_MS` = 30 s after the last wake-up: backlight off, ST7789 SLPIN, no
  redraws; wake-up = SLPOUT, full redraw, backlight on. The radio is untouched either way.
- Data: uplink LQ / RSSI / SNR and downlink RSSI from linkStats; RSSI 2 only if the receiver reports one; rate from
  the current air rate, the RATE card's corner = switch mode; POWER current / max mW, DYN; TLM ratio and bps (as the
  Lua's); RX BAT from CRSF battery telemetry (tx_main hands every telemetry frame to `twatchUiOnTelemetry`); MDL =
  the handset's model id; LINK = time since connected; TX = ESP32-S3 internal temperature; time and date from the
  PCF8563 (I2C 0x51; VL bit set or nonsense -> "--:--"); watch battery from the AXP2101 (gauge %, or a LiPo voltage
  curve), charging -> green. BLE does not exist yet: dim icon, "Off" / "—". Missing values show "—".
- Fonts: U8g2 bitmap fonts through Arduino_GFX (UTF-8, Cyrillic with the Ukrainian letters from 6x12 / 6x13).
- Drawing: every screen is a list of regions; each tick (0.25 s) all regions of the current screen are rendered into
  a 19 KB RAM buffer, hashed, and only changed ones are sent. A PC preview of the same drawing code (Arduino_GFX
  compiled for the host) produced the images used to check the layout against the designs.
- Cost at 40 MHz (0.4 us per pixel): full redraw 38-53 k px + a 57.6 k px clear = ~40-45 ms; a new second on 03 =
  2.3 k px ~ 1 ms; a LQ + RSSI change on 01 = 15.4 k px ~ 6 ms; the LQ history (every 1.5 s) 8.6 k px ~ 3.5 ms.
  All on core 0 (device task); the radio's timer ISR and SPI are on core 1 / SPI2.
