# Hardware

Phase one runs on a LilyGo T-Dongle-C5. This page records the board's facts,
where each came from, and what was learned bringing it up.

## The board

| Item | Value |
|---|---|
| Chip | ESP32-C5HR8: single-core RISC-V, dual-band Wi-Fi 6, Bluetooth 5 LE, 802.15.4 |
| Flash | 16 MB |
| PSRAM | 8 MB, in the chip package (not enabled by this firmware) |
| USB | The chip's built-in USB Serial/JTAG. Id `303a:1001`. `/dev/ttyACM*` on Linux, `COM*` on Windows |
| Display | 0.96 inch ST7735S, 80 x 160, on SPI |
| LED | One APA102-2020 RGB LED |
| Storage | TF card slot, sharing the display's SPI bus |
| Button | BOOT |

## Pin map

From LilyGo's own repositories (`Xinyuan-LilyGO/T-Dongle-C5` and
`Xinyuan-LilyGO/LilyGo-display-library`) and the V1.1 schematic, read on
2026-11-16. The firmware's copy is `firmware/main/board.h`.

| Function | GPIO | Checked on hardware |
|---|---|---|
| SPI MOSI (display and TF card) | 2 | Yes |
| SPI clock | 6 | Yes |
| SPI MISO (TF card only) | 7 | Not used yet |
| Display chip select | 10 | Yes |
| Display data/command | 3 | Yes |
| Display reset | 1 | Yes |
| Display backlight, active low | 0 | Yes |
| TF card chip select | 23 | Not used yet |
| LED clock, as documented | 4 | Does not control the LED on the bring-up unit |
| LED data, as documented | 5 | Does not control the LED on the bring-up unit |
| BOOT button, active low | 28 | Not used yet |

An earlier pin map taken from community ports had every display pin wrong and
the backlight polarity reversed. The screen stayed dark until LilyGo's own
values were used.

## Display settings

Confirmed on hardware: 20 MHz SPI, mode 0, colour inversion on, BGR colour
order, landscape with `MADCTL = MY | MV | BGR`. The 80 x 160 glass sits
centred in the controller's 132 x 162 memory, so the window is offset by 26
along the short axis and 1 along the long axis.

## The LED on the bring-up unit

LilyGo's schematic and code put the LED's clock on GPIO 4 and its data on
GPIO 5. On the unit this firmware was brought up on, the LED ignores those
pins and takes its colour from whatever is clocked on the display's SPI
lines.

What was seen, before this was understood:

| Screen content | LED |
|---|---|
| White text | Bright white |
| The idle screen, with a grey line of text at the bottom | Dim green |
| A session screen redrawn once a second | A flash each second, then green |

A model of an APA102 listening to the display bus reproduces each of these
from the exact bytes the display driver sends. The grey text, for example,
is two pixels of `0x8410`, which an APA102 reads as brightness 4, blue 16,
green 132, red 16.

Why the unit behaves this way is not known. Either it differs from the
published schematic, or the LED's inputs pick up the display lines. Whether
other units do the same is also not known.

The firmware therefore sends LED frames on the display's SPI lines with the
display deselected, and re-sends the colour after every screen update. A
brief flicker during each redraw remains, because the pixel data still
passes the LED before the colour is restored.

`BOARD_LED_ON_DISPLAY_BUS` in `board.h` selects this. Setting it to 0 drives
GPIO 4 and 5 as LilyGo documents; that route is untested here.

## Bluetooth transmit power

For the radio phases: ESP-IDF v5.5 lists sixteen Bluetooth LE power levels
for the C5, from -24 dBm to +20 dBm, mostly in 3 dB steps.

## Towards a polished device

A device that is sold and transmits needs regulatory authorisation as an
intentional radiator. Building around a pre-certified C5 module with its
listed antenna keeps that manageable. The protocol's `hello` reports the
board and its capabilities so the same CLI and app can drive later hardware.
