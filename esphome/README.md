# Sendspin node (ESPHome)

[`sendspin-node.yaml`](sendspin-node.yaml) is a [Sendspin](https://esphome.io/components/sendspin/) player for
Music Assistant: a 38-pin ESP32-WROVER devkit on the ESP32 HA Proto carrier (v3, `~/src/esp32-ha-protoboard`),
with a PCM5102 I2S DAC module wired to it. The I2S pins are the ones the 2022 audio art boards used
([`../src/main.cpp`](../src/main.cpp)), three adjacent pins on the devkit's header.

## Wiring

| PCM5102 pin   | To                     | Why                                                               |
| ------------- | ---------------------- | ----------------------------------------------------------------- |
| VIN           | 5V                     | the module's own regulators make 3.3 V for the DAC                |
| GND           | GND                    |                                                                   |
| BCK           | GPIO27                 | bit clock                                                         |
| LRCK          | GPIO26                 | word select                                                       |
| DIN           | GPIO25                 | data                                                              |
| SCK           | GND                    | no master clock: the PCM5102 derives it from BCK                  |
| H1L (FLT)     | L                      | normal-latency filter                                             |
| H2L (DEMP)    | L                      | de-emphasis off                                                   |
| H3L (XSMT)    | **H**                  | soft mute off. Left open or low, the DAC stays silent             |
| H4L (FMT)     | L                      | I2S format                                                        |

The H1L–H4L solder jumpers are on the back of the module; on many of these modules they come open.

## Carrier pins

| Function              | GPIO           | On the HA Proto                     | In this firmware                     |
| --------------------- | -------------- | ----------------------------------- | ------------------------------------ |
| I2S to PCM5102        | 27, 26, 25     | devkit header / test pads           | speaker, 48 kHz 16-bit stereo        |
| PWM D                 | 33             | J8 bridged, MOSFET to HVCC          | `Red LED` light, lit while playing   |
| Button                | 0              | SW1 (also the devkit's BOOT)        | play/pause                           |
| PWM C / WS2812 out    | 5              | J7 / J11 (both bridged now)         | free                                 |
| I2C                   | SDA 21, SCL 22 | 3.3 V and 5 V headers, 4.7 kΩ pull-ups | free                              |
| SPI                   | CLK 18, MOSI 23, MISO 19, CS 5 | H6 via the 74HCT125, J1–J4 | free                                 |
| PWM A / PWM B         | 16 / 17        | J5 / J6                             | **unusable on a WROVER** (PSRAM)     |

Notes from the carrier's netlist:

- **GPIO16 and GPIO17 are the WROVER's PSRAM lines**, so PWM A and B can't use them. With J5 and J6 bridged the
  PSRAM still came up (8 MB, first boot on 2026-11-06), so this devkit probably leaves those header pins
  unconnected, as Espressif's WROVER devkits do. Either way PWM A and B do nothing useful. To use them, open J5/J6
  and wire the jumper's IN pad to a free GPIO (4, 13, 14 or 32).
- **The SPI header can't read.** The 74HCT125 drives all four lines toward the header, MISO included, so a
  device's MISO fights the buffer. For an SD card, wire MISO straight to GPIO19 and leave J4 open, or use the
  GPIO test pads directly.
- **GPIO5 feeds PWM C, the WS2812 output and SPI CS.** Bridge only one of J7, J11, J1.
- Spare outputs: 2, 4, 13, 14, 15, 32. Avoid 6–11 (flash), 1/3 (USB serial), 12 (flash-voltage strap) and
  34–39 (input only).

## Flashing

```bash
uv tool install esphome
esphome run sendspin-node.yaml --device /dev/cu.usbserial-XXXX
```

The first flash has to go over USB; after that, `--device sendspin-node-XXXXXX.local` (or its IP address, if the
Mac doesn't resolve the name) flashes it over Wi-Fi.
There are no secrets in the config. Give it Wi-Fi with Improv Serial at
[web.esphome.io](https://web.esphome.io) ("Connect to Wi-Fi"), or join its `Sendspin Node` hotspot and use the
captive portal. Home Assistant then finds it and sets its API key when you adopt it.

## Music Assistant

Sendspin players show up in Music Assistant on their own once they are on the network (Music Assistant 2.11 or
newer). The ESP32 needs TCP port 8928 reachable and mDNS not blocked.

**AirPlay** comes from Music Assistant, not the ESP32: add the **AirPlay Receiver** plugin (Settings → Plugins)
and add this player under "Connected players". The plugin advertises one AirPlay receiver per connected player,
so a player that isn't in that list doesn't show up; once added it appears on iPhones and Macs as "Sendspin Node
XXXXXX | Music Assistant". Music Assistant converts the AirPlay stream, and the node receives it as 48 kHz FLAC,
the same as anything else Music Assistant plays. Expect a few seconds' delay on play and pause.

**Several rooms:** group the speakers in Music Assistant (this player's companion speakers, or a group player)
and send AirPlay to this one receiver. Picking several "| Music Assistant" AirPlay targets on the Mac at once
plays, but out of sync: each receiver is its own stream in Music Assistant. A Music Assistant group plays in sync.

Tested 2026-11-07 with Home Assistant 2026.11.0 and Music Assistant 2.11.0b8: playback from Music Assistant and
AirPlay from a Mac, with track changes and grouping, for about 30 minutes without a crash or a dropped connection.

## Memory

**Use an ESP32 with a WROVER module (PSRAM).** The config sets `psram: ignore_not_found: false`, so on a WROOM
module (no PSRAM) the firmware stops at boot rather than playing badly. On the WROVER devkit (MAC
`4c:11:ae:df:ce:28`, ESP32-D0WD rev 1.0, 8 MB PSRAM) Sendspin gets its full 1.24 MB buffer and 200 KB of internal
heap stays free, 156 KB during playback.

**Keep Sendspin's task stacks in internal RAM** (`task_stack_in_psram: false`). This devkit's ESP32 is a
revision 1 chip, whose PSRAM cache bug can lose a write; ESP-IDF's workaround (`SPIRAM_CACHE_WORKAROUND`) only
covers code compiled with it. With the stacks in PSRAM, the board crashed three times in ten minutes of playback
(a null ring-buffer handle in the HTTP server task, then a null event group in the sync task), and Sendspin
dropped the connection about every 12 seconds on a failed decryption ("Noise AEAD failure in transport mode"),
17 times in all. With the stacks in internal RAM there were none (2026-11-07). The WROOM, without PSRAM, never
failed a decryption. A revision 3 ESP32 (ESP32-D0WD-V3, on WROVER-E modules) doesn't have the bug.

Below is what happened on a WROOM first.

The first ESP32 flashed with this (MAC `7c:9e:bd:06:72:cc`, the installation's controller node 6429) is an
ESP32-D0WDQ6 rev 1.0 on a WROOM module: 4 MB flash, no PSRAM. The ESP32 plugs into its carrier board, so the
chip ID names the ESP32, not the carrier.

**Sendspin plays on a WROOM, but it can't keep sync.** What it took to get that far, in the order the problems
showed up:

1. **Stream buffer.** Sendspin's default 1 MB buffer needs PSRAM. Without it, the library allocates its receive
   buffer as one block of about 2.3 × `buffer_size` + 8 KB. A 64 KB buffer needed 158 KB and failed, so the
   config sets `buffer_size: 32000`: one 80 KB block, allocated at boot.
2. **Network buffers.** The speaker and Sendspin components turn on ESPHome's streaming network settings (64 KB
   TCP window, 16 static and 64 dynamic Wi-Fi RX buffers). When the first stream opened, free heap fell from
   128 KB to 52 KB, and the I2S speaker and then the Opus decoder failed to allocate. The config turns off
   `network: enable_high_performance` and puts the Wi-Fi buffers back to the IDF defaults.
3. **Opus.** micro-opus takes a 60–120 KB scratch area per decoding thread. The largest free block was 48 KB,
   so the allocation failed and the decoder crashed the board (StoreProhibited in `celt/celt_decoder.c`). The
   config allows only FLAC and PCM.
4. **Sync.** FLAC plays, but 32 KB holds only about 0.2 s of FLAC. The log shows "Lost sync (96000us off)",
   one 96 ms FLAC block late, 189 times in a minute. Free heap during playback is 38 KB, with the lowest point at
   16 KB, so there is no room for a bigger buffer.

Those workarounds are gone from the config now that it targets PSRAM: Sendspin's 1 MB buffer, the Opus scratch
area, and ESPHome's streaming Wi-Fi and TCP buffers all go in PSRAM.

## Flashing notes

The board's CP2102 is unreliable at 921600 baud: esptool fails with "Invalid head of packet". At 230400 it
works, including the factory image:

```bash
esptool --port /dev/cu.usbserial-0001 --baud 230400 write-flash 0x0 \
  .esphome/build/sendspin-node/build/firmware.factory.bin
```

The firmware that was on the controller ESP32 before is saved, uncommitted, in
`backup/controller-6429-artnet.bin`. To restore it, write it at 0x0 the same way.
