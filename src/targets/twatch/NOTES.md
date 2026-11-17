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
