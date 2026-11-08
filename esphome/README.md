# Sendspin node (ESPHome)

[`sendspin-node.yaml`](sendspin-node.yaml) is a [Sendspin](https://esphome.io/components/sendspin/) player for
Music Assistant: a 38-pin ESP32-WROVER devkit on the ESP32 HA Proto carrier (v3, `~/src/esp32-ha-protoboard`),
with an I2S DAC module wired to it. The I2S pins are the ones the 2022 audio art boards used
([`../src/main.cpp`](../src/main.cpp)), three adjacent pins on the devkit's header.

Two nodes are built, with the same firmware:

| Node                   | ESP32                                                           | DAC                   |
| ---------------------- | --------------------------------------------------------------- | --------------------- |
| `sendspin-node-dfce28` | WROVER devkit, ESP32-D0WD rev 1.0, 8 MB PSRAM                   | PCM5102               |
| `sendspin-node-206ad8` | WROVER-IE devkit, ESP32-D0WD-V3 rev 3.0, 8 MB flash, 8 MB PSRAM | CJMCU-1334 (UDA1334A) |

## Wiring

Both DACs take the same five wires from the carrier: 5V, GND, and GPIO27 (bit clock), GPIO26 (word select) and
GPIO25 (data). Their pins are in a different order and their control pins work differently, so wire each by its
own table.

### PCM5102

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

### CJMCU-1334 (UDA1334A)

A clone of Adafruit's UDA1334A breakout, with a 3.5 mm line-out jack.

| CJMCU-1334 pin                      | To      | Why                                                          |
| ----------------------------------- | ------- | ------------------------------------------------------------ |
| VIN                                 | 5V      | the module's regulator makes 3.3 V                           |
| GND                                 | GND     |                                                              |
| BCLK                                | GPIO27  | bit clock                                                    |
| WSEL                                | GPIO26  | word select                                                  |
| DIN                                 | GPIO25  | data                                                         |
| 3VO                                 | nothing | the regulator's 3.3 V output                                 |
| SCLK, PLL, SF0, SF1, DEEM, **MUTE** | **GND** | audio mode, I2S format, no de-emphasis, not muted            |

On the long row the labels alternate above and below the pins. From the VIN end the order is VIN, 3VO, GND,
WSEL, DIN, BCLK, Lout, AGND, Rout. The short row, from the jack end, is DEEM, PLL, SF0, MUTE, SF1, SCLK.

What went wrong on the first build (2026-11-07):

- **SCLK is not the bit clock.** It is the system-clock input for the chip's video mode. The bit clock goes to
  BCLK on the long row. With word select on SCLK and WSEL left open, the DAC was silent.
- **MUTE high mutes**, the opposite of the PCM5102's XSMT.
- **Ground all six control pins.** Adafruit's board pulls them low with resistors. On this clone, with them left
  open, the DAC played white noise; with all six on GND it played cleanly.

## Carrier pins

| Function              | GPIO           | On the HA Proto                     | In this firmware                     |
| --------------------- | -------------- | ----------------------------------- | ------------------------------------ |
| I2S to the DAC        | 27, 26, 25     | devkit header / test pads           | speaker, 48 kHz 16-bit stereo        |
| PWM D                 | 33             | J8 bridged, MOSFET to HVCC          | `Red LED` light, lit while playing, PWM at 40 kHz |
| Button                | 0              | SW1 (also the devkit's BOOT)        | play/pause                           |
| PWM C / WS2812 out    | 5              | J7 / J11 (both bridged now)         | `LED Strip` on 206ad8 ([LED strip](#led-strip)); free on dfce28 |
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
[web.esphome.io](https://web.esphome.io) ("Connect to Wi-Fi"), or join its `sendspin-node-XXXXXX` hotspot (no
password; the suffix is the end of its MAC address) and use the captive portal. Home Assistant then finds it and sets its API key when you adopt it.

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

## LED strip

`sendspin-node-206ad8` drives a WS2812 board of 34 LEDs: data from GPIO5 through a separate 3.3 V to 5 V level
shifter, VCC on 5 V. With VCC on 3.3 V and the data wired straight to GPIO5 it stayed dark. The carrier's WS2812
output (CN9, GPIO5 through J11 and its own 3.3 V to 5 V buffer) would do the level shifter's job.
[`sendspin-node-led-strip.yaml`](sendspin-node-led-strip.yaml) is `sendspin-node.yaml` plus the strip:

```bash
esphome run sendspin-node-led-strip.yaml --device sendspin-node-206ad8.local
```

Open J7, or PWM C's MOSFET switches with the strip's data. The strip is the `LED Strip` light in Home Assistant,
with three effects that follow the music, and a rainbow:

| Effect         | Shows                                                                                   |
| -------------- | --------------------------------------------------------------------------------------- |
| Music Spectrum | one LED per band, 40 Hz to 16 kHz, red for the bass to violet for the treble            |
| Music Level    | a level meter from the middle outwards, green to red, with a falling peak dot           |
| Music Beat     | the whole strip flashes and steps to the next colour on each beat, brighter on downbeats |

The music data comes from Music Assistant, not from the node: Sendspin's visualizer role. Music Assistant analyses
the audio it streams and sends loudness, a spectrum and onsets (transients) about 30 times a second, and beats
once its audio analysis has the track, each stamped with the time it plays. The node shows each value at that
time, so the strip stays in step with the speakers and with the other players in a group. Music Beat flashes on
the stronger onsets until beats arrive. Each effect scales to the music's recent peak (each band to its own for
the spectrum), so it fills the strip at any volume; the node's volume doesn't change it.

[`components/sendspin_visualizer`](components/sendspin_visualizer) receives the data and draws the effects.
ESPHome 2026.10.2 can ask for the visualizer role but never adds it to the Sendspin client; the open pull request
[esphome/esphome#19276](https://github.com/esphome/esphome/pull/19276) does. Until that is in a release, the
component reaches into the hub to add the role itself (see the top of `sendspin_visualizer.cpp`); after it, the
component should call the hub's `set_visualizer()` instead.

**Brightness limit.** `Strip Brightness Limit` (a setting on the device in Home Assistant, 25% by default) caps
the strip, effects included: at 100% the strip shows the limit's brightness. ESPHome applies its gamma (2.8)
after the cap, so the current falls much faster than the number: full white draws about 2 A uncapped, 0.3 A at
50% and 45 mA at 25%; one colour alone draws a third of that.

**206ad8 hangs when the strip draws more than a little.** At full brightness (red, then with a 50% cap: about
0.1 A from the 5 V) the board hung several times on 2026-11-08: its log stopped, with no reboot, and only a power
cycle brought it back. Red at 34% brightness, a few mA, ran steadily, and so did full red and full white with the
25% limit, three minutes each. A WS2812 board this size should draw 0.1 A without trouble, so 206ad8's 5 V supply or its
wiring is weak, which may also be the cause of its stutters ([below](#open-206ad8-stutters), item 4). To check:
measure the 5 V at the board with the strip lit; wire the strip's 5 V and ground straight to the supply, not
through the devkit; and add a capacitor of 470 µF or more across the strip's 5 V and ground.

The WS2812's own PWM runs at a fixed rate in the audio band (about 0.4 to 2 kHz, depending on the version), and
can't be moved like the red LED's. If the strip is heard in the speakers, the same fixes apply: its own 5 V supply,
or at least its own ground wire back to the supply, and the capacitor at the strip.

Tested 2026-11-08 with Music Assistant 2.11.0b8 (aiosendspin 10.2.2), playing a podcast: the stream started with
loudness, onsets and a 34-band spectrum at 30 frames a second, and no beats, as expected for speech. Memory during
playback: 134 KB internal free, 1.8 MB PSRAM free. Not yet seen: beats on music, and how the effects look.

## Open: 206ad8 stutters

Installed with its LED and external antenna, `sendspin-node-206ad8` (the CJMCU-1334 node) plays with short
interruptions and stutters, while `sendspin-node-dfce28` sounds clean on the same stream. Not solved yet. What is
known (2026-11-07):

- **The audio leaves the ESP32 intact.** On the debug build ([Reading a node's log](#reading-a-nodes-log)) it
  played for 20 minutes, 12:00 to 12:20, and stuttered, but the node logged no "Lost sync", no underflow and no
  dropped data. The Sendspin client times itself against the I2S speaker's reports of when audio finished
  playing, and logs "Lost sync" when the output is more than 5 ms from where it should be, so the I2S output had
  no gaps. The fault is somewhere between the I2S pins and the speaker: the wiring, the DAC module or its supply.
- **Not Wi-Fi.** With the external antenna it is at −46 to −61 dBm (dfce28: −42). Before the antenna was fitted
  it was at −82 to −89 dBm, and Music Assistant logged stalled sends and late chunks to it; it has logged none
  since.
- **Not roaming.** ESPHome scans for a better access point every 5 minutes while the signal is below −49 dBm, so
  206ad8 scans and dfce28 doesn't. Sendspin holds the scans off during playback: the first one came as the stream
  ended.

To check next, in this order:

1. The three I2S wires (BCLK, WSEL, DIN), for a loose contact.
2. The six control-pin ground wires, MUTE and PLL above all. A floating MUTE mutes the DAC; a floating PLL takes
   away its clock.
3. The LED, whose PWM on GPIO33 runs at 50% during playback. Turn off `LED Shows Playback` and the light, and
   listen. Its PWM ran at ESPHome's default 1 kHz, and a dimmed LED was heard as a whine in the speakers; it runs
   at 40 kHz since 2026-11-08.
4. The 5 V supply.

## Reading a node's log

Home Assistant sets the API encryption key when it adopts a node, so `esphome logs` from another machine can't
connect to an adopted node. [`sendspin-node-syslog.yaml`](sendspin-node-syslog.yaml) is the same firmware with
its log also sent as UDP syslog to `log_host` (set it to the listening machine's address). Flash it over Wi-Fi,
listen on UDP port 5514, and flash `sendspin-node.yaml` back when done:

```bash
esphome run sendspin-node-syslog.yaml --device sendspin-node-XXXXXX.local
python3 -c '
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("", 5514))
while True:
    print(s.recv(4096).decode(errors="replace"), flush=True)
'
esphome run sendspin-node.yaml --device sendspin-node-XXXXXX.local
```

The node restarts on each flash. If it was leading a Music Assistant group, the group's stream stops; pause and
play the group to start it again. `nc -ul 5514` also receives the log, but runs the lines together.

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
failed a decryption. A revision 3 ESP32 (ESP32-D0WD-V3, on WROVER-E modules, like the second node's) doesn't have the bug.

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
