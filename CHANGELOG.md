# Changelog

Firmware versions, newest first. The CLI and protocol crate are at 0.1.0 and
have not had a release yet.

## Firmware 0.3.4 (2026-11-16)

- The LED is now driven over the display's SPI lines with the display
  deselected, and its colour is re-sent after every screen update. On the
  bring-up unit the LED follows the display bus and ignores GPIO 4 and 5; see
  `docs/hardware.md`.
- `led.selftest` is a plain colour cycle again: red, green, blue, white, off.

## Firmware 0.3.1 to 0.3.3 (2026-11-16)

Diagnostic builds while finding out why the LED showed green when told to be
off. They tried a slower clock, different framing, pin readback and a
five-way timing comparison. None fixed it, because the cause was not on the
LED's documented pins. Superseded by 0.3.4.

## Firmware 0.3.0 (2026-11-16)

- Sessions: `tx.start`, `lease.renew`, `tx.stop`, with a 3-second lease and a
  5-minute limit enforced on the stick.
- The session rules moved into `session.c` with host tests.
- Session screen with a countdown; idle screen shows why the last session ended.
- The LED blinks during a session. The only signal is `led`.
- CLI: `chickchirp start` and `chickchirp stop`.

## Firmware 0.2.0 (2026-11-16)

- Display driver for the ST7735S panel; `display.text` works.
- Pin map corrected from LilyGo's own code.

## Firmware 0.1.0 (2026-11-16)

- First firmware: protocol v0 over USB serial with `hello` and `status`.
