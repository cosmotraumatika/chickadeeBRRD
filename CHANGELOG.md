# Changelog

Firmware versions, newest first. The CLI and protocol crate are at 0.1.0 and
have not had a release yet.

## Firmware 0.4.1 (2026-11-16)

- Fixed: `hello` got no reply, so `chickchirp info` and `chickchirp start`
  timed out. The reply had grown to 434 bytes in 0.4.0 and the USB driver
  silently drops a line larger than its transmit buffer, which was the
  default 256 bytes. The buffers are now 2048 bytes, twice the protocol's
  longest line, and a line that cannot be sent is logged.
- The host test's stand-in for the USB driver now refuses over-long lines the
  way the real one does.

## Firmware 0.4.0 (2026-11-16)

The first radio signal. Built and tested on a PC; not yet received by a bench
receiver.

- New signal `bt4`: Remote ID Basic ID, Location, System and Self-ID messages
  over Bluetooth 4 legacy advertising. See `docs/remote-id.md`.
- The identity is fixed in the firmware: a Specific Session ID in the private
  range, `CHICKADEEBRRD-TEST`, with the Self-ID text `chickadeeBRRD:TEST`.
- `tx.start` takes `lat`, `lon` and `time` (required for `bt4`), and optional
  `power_dbm` and `max_ms`. `lease.renew` takes an optional `time`.
- Power is -24 dBm unless asked for, 0 dBm at most. It is set and read back
  before each transmission.
- A session lasts 5 minutes unless `max_ms` asks for longer, 60 minutes at
  most. Asking for more than a limit is refused with `limit_exceeded`.
- `hello` reports `session_default_ms`, the power limits and the broadcast
  identity. `session_max_ms` now means the 60-minute ceiling.
- New stop reason `error`, for a radio that will not transmit.
- The LED blinks blue during a `bt4` session and orange during `led`.
- Bluetooth is built as a broadcaster only: no connections, no scanning.
- Host tests for the messages and for the command handler and control loop.
- Requires a fresh `sdkconfig`; see `docs/development.md`.

CLI, same date:

- `chickchirp start` takes `--lat`, `--lon` and `--power`; `--duration` is
  now passed to the stick as the session's time limit and accepts `m` and `h`.
- `chickchirp info` shows the limits, the power range and the identity.
- Mistakes in those options are reported before anything is sent to the stick.

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
