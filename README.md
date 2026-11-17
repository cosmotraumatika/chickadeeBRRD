# chickadeeBRRD

A pocket Remote ID tester. A small USB stick (a LilyGo T-Dongle-C5) that
sends a short-range Remote ID test signal, controlled over USB serial from
Linux today, and from Windows 11 and Android later. The chickadee calls out its own name; Remote ID
is a drone calling out its identity.

It is for people who build drones, or the equipment that listens for them, and
want to test receivers on a desk instead of flying.

## Status

Firmware 0.4.0 adds the first radio signal: Remote ID test messages over
Bluetooth 4, under a fixed test identity. **It has been built and tested on a
PC against a stand-in radio. It has not yet been received by a bench
receiver**, and the power it radiates has not been measured.

| Phase | Goal | State |
|---|---|---|
| 0 | Bench: stick visible to the build machine | Done |
| 1 | Hello, stick: the CLI puts a message on the screen | Done |
| 2 | Lease: a session that stops when the host goes away | Done |
| 3 | One signal: Bluetooth 4 Remote ID seen by a bench receiver | Built; not yet received on the bench |
| 4 | Windows: the same CLI on a COM port | Not started |
| 5 | Android app | Not started |
| 6 | Polish and share | Not started |
| 7 | Polished device on its own hardware | Not started |

See [docs/roadmap.md](docs/roadmap.md) for what "done" means for each.

## How it stays safe

The limits live on the stick, not in the software that controls it, so they
hold even if someone changes the CLI or the app.

- A session runs only while a host renews a 3-second lease. If the host
  crashes, hangs or is unplugged, the stick stops by itself.
- A session ends after 5 minutes unless you ask for longer, and never runs
  past 60 minutes.
- It transmits at its lowest power, -24 dBm, unless you ask for more, and
  never above 0 dBm.
- It broadcasts one identity, `CHICKADEEBRRD-TEST`, with the Self-ID text
  `chickadeeBRRD:TEST`. No command changes it.
- The rules have tests that run on a PC.

Whether, where and how you may transmit Remote ID test messages over the air
is a legal question this project does not answer. More in
[docs/safety.md](docs/safety.md); what goes on the air is in
[docs/remote-id.md](docs/remote-id.md).

## Quick start

You need Rust, ESP-IDF v5.5.2 or newer on the 5.5 line, and the stick.
[docs/development.md](docs/development.md) has the full setup, including WSL.

```bash
# the CLI
cargo test
cargo install --path crates/chickchirp

# the firmware, in a shell with ESP-IDF activated
cd firmware
idf.py set-target esp32c5
idf.py -p /dev/ttyACM0 build flash

# try it
chickchirp info
chickchirp display "chick-a-dee-dee"
chickchirp start --signal led      # the LED blinks; Ctrl-C to stop
chickchirp start --signal bt4 --lat 40.9 --lon -95.6   # Remote ID over Bluetooth 4
```

## The CLI

```bash
chickchirp devices            # list attached sticks
chickchirp info               # firmware, protocol, board, capabilities, limits
chickchirp status             # what the stick is doing now
chickchirp display "hello"    # show a message on the screen
chickchirp start --signal led # run a session until Ctrl-C
chickchirp stop               # end a session left running by a host that died
chickchirp monitor            # everything the stick sends, until Ctrl-C
chickchirp raw '{"cmd":"status"}'
```

`start` takes these options:

| Option | Meaning | Default |
|---|---|---|
| `--signal` | `led` or `bt4`; `chickchirp info` lists what the stick offers | required |
| `--lat`, `--lon` | The position to broadcast, in degrees. Needed by `bt4`. Also read from `CHICKCHIRP_LAT` and `CHICKCHIRP_LON` | none |
| `--duration` | Time limit, such as `90s`, `10m` or `1h` | The stick's own: 5 minutes. Longest: 60 minutes |
| `--power` | Transmit power in dBm, from -24 to 0 in steps of 3 | The stick's lowest: -24 |

```bash
chickchirp start --signal bt4 --lat 40.9 --lon -95.6
chickchirp start --signal bt4 --lat 40.9 --lon -95.6 --power -12 --duration 20m
```

`start` holds the serial port until it ends, so a second `chickchirp` command
is refused meanwhile. The stick's screen shows the signal, the power and the
countdown.

- The port is found automatically when exactly one stick is attached.
  Otherwise use `--port` or set `CHICKCHIRP_PORT`.
- `--json` gives machine-readable output on stdout. Diagnostics go to stderr.
- `--verbose` shows the stick's log lines while waiting for a response.
- `chickchirp completions bash` (or zsh, fish, powershell) prints a completion
  script.

| Exit code | Meaning |
|---|---|
| 0 | Success |
| 1 | The stick refused the command |
| 2 | Usage error |
| 3 | No stick found, or more than one |
| 4 | No response in time |
| 5 | Port I/O error |

## Without hardware

`tools/fakestick.py` is a pretend stick on a pseudo-terminal that speaks the
same protocol, lease and both signals included. It transmits nothing.

```bash
python3 tools/fakestick.py          # prints a port such as /dev/pts/3
chickchirp --port /dev/pts/3 info
```

## Repository layout

| Path | What it is |
|---|---|
| `crates/chickadee-proto/` | Protocol message types and line codec, no I/O |
| `crates/chickchirp/` | The command-line tool |
| `firmware/` | ESP-IDF project for the stick (target `esp32c5`) |
| `firmware/components/opendroneid/` | The Open Drone ID reference encoder, unmodified |
| `firmware/host_test/` | Tests for the firmware's rules and messages that run on a PC |
| `tools/fakestick.py` | The pretend stick |
| `testdata/vectors/` | Protocol lines every implementation must handle |
| `docs/` | Protocol, development setup, hardware notes, safety, roadmap |

## Documentation

- [docs/protocol.md](docs/protocol.md): how a host talks to the stick
- [docs/development.md](docs/development.md): setting up, building, flashing, troubleshooting
- [docs/hardware.md](docs/hardware.md): the board, its pins, and what was learned bringing it up
- [docs/remote-id.md](docs/remote-id.md): exactly what the stick puts on the air
- [docs/safety.md](docs/safety.md): what the firmware enforces, what has been tested, and what is your responsibility
- [docs/roadmap.md](docs/roadmap.md): the phases and the decisions taken so far
- [CHANGELOG.md](CHANGELOG.md): what changed in each firmware version

## Licence and third-party material

Copyright (C) 2026 Jamie Saker <cosmotraumatika@gmail.com>

Licensed under the GNU Affero General Public License, version 3; see
[LICENSE](LICENSE).

- `firmware/main/font8x8.h` is from
  [font8x8](https://github.com/dhepper/font8x8) by Daniel Hepper, public domain.
- `firmware/components/opendroneid/` is from
  [opendroneid-core-c](https://github.com/opendroneid/opendroneid-core-c),
  Apache License 2.0, unmodified; its licence is in that directory.
- The firmware builds against [ESP-IDF](https://github.com/espressif/esp-idf)
  (Apache-2.0) and uses the cJSON it bundles (MIT).
- The board's pin map and display settings were taken from LilyGo's published
  code and schematic for the T-Dongle-C5; see [docs/hardware.md](docs/hardware.md).
