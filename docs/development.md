# Development

How to set up, build, flash and test. The project was brought up on Windows 11
with Debian 13 under WSL2; native Linux needs fewer steps, and the notes say
where they differ.

## What you need

| Tool | Version | Used for |
|---|---|---|
| Rust | 1.80 or newer | The CLI and the protocol crate |
| ESP-IDF | v5.5.2 or newer on the 5.5 line | The firmware. Older 5.5 releases have a PSRAM bug on the C5 |
| Python 3 | Any recent | The fake stick, and ESP-IDF's own tools |
| A C compiler | Any | The firmware's host tests |
| usbipd-win | 4 or newer | WSL only: passing the stick through from Windows |

The firmware was written against ESP-IDF v5.5.5 and has not been checked
against 6.x.

## Installing ESP-IDF

With Espressif's installer, pinning the version and the one chip we need:

```bash
sudo apt install eim-cli        # after adding Espressif's apt repository
eim install -i v5.5.5 -t esp32c5
```

The installer may report that it could not copy OpenOCD's udev rules, and
that pre-downloading components failed. Neither matters for building and
flashing. The rules are only needed for JTAG debugging.

ESP-IDF must be activated in every terminal that runs `idf.py`:

```bash
source ~/.espressif/tools/activate_idf_v5.5.5.sh
```

An activated terminal shows `(venv)` at the start of its prompt. A shortcut
saves retyping:

```bash
echo "alias get_idf='source ~/.espressif/tools/activate_idf_v5.5.5.sh'" >> ~/.bashrc
```

## Attaching the stick under WSL

The stick plugs into Windows and is passed through to WSL. From an
administrator PowerShell:

```powershell
usbipd list                                      # find the stick: 303a:1001
usbipd bind --busid <id>
usbipd attach --wsl --auto-attach --busid <id>
```

- Leave the `attach` window open. The stick re-enumerates on every flash and
  reset, and `--auto-attach` re-attaches it.
- If Wireshark's USBPcap is installed, `bind` needs `--force`. While bound
  that way, Windows itself cannot use the stick; `usbipd unbind --busid <id>`
  gives it back.

In WSL the stick appears as `/dev/ttyACM0`. Your user must be in the group
that owns it:

```bash
ls -l /dev/ttyACM0                         # shows the group
sudo usermod -aG <group> $USER             # then open a new terminal
```

On native Linux, skip usbipd; the port and the group step are the same.

## Where to keep the code and the build output

If the repository lives on the Windows side (for example `C:\Dev\chickadeeBRRD`,
seen from WSL as `/mnt/c/Dev/chickadeeBRRD`), keep build output inside WSL.
Building onto `/mnt/c` is slow.

```bash
export CARGO_TARGET_DIR=~/build/chickadee-target      # Rust
idf.py -B ~/build/chickadee-fw ...                    # firmware
```

On native Linux, plain `cargo` and `idf.py` with their default directories
are fine.

## A two-terminal routine

One terminal for Rust, one with ESP-IDF activated for firmware.

Rust terminal:

```bash
cd /mnt/c/Dev/chickadeeBRRD
export CARGO_TARGET_DIR=~/build/chickadee-target
cargo test
cargo install --path crates/chickchirp
```

Firmware terminal:

```bash
source ~/.espressif/tools/activate_idf_v5.5.5.sh
cd /mnt/c/Dev/chickadeeBRRD/firmware
idf.py -B ~/build/chickadee-fw set-target esp32c5      # first time only
idf.py -B ~/build/chickadee-fw -p /dev/ttyACM0 build flash
```

## Tests

```bash
cargo test                          # protocol crate and CLI
sh firmware/host_test/run.sh        # the firmware's rules, on the PC
```

The firmware's host tests come in three parts: the session rules, the Remote
ID messages (built, then decoded with the reference decoder), and the command
handler and control loop against a stand-in radio. The third needs cJSON,
which ships with ESP-IDF, so it runs only in a terminal where ESP-IDF is
activated and is skipped with a note elsewhere.

The shared file `testdata/vectors/v0-device-lines.jsonl` is the protocol's
conformance set; any new host implementation should classify every line in
it as labelled.

## Working without the stick

```bash
python3 tools/fakestick.py                 # prints a port such as /dev/pts/3
chickchirp --port /dev/pts/3 info
FAKESTICK_SESSION_DEFAULT_MS=2000 python3 tools/fakestick.py    # a short default limit, for testing
```

The fake stick offers `led` and `bt4` and applies the same checks as the
firmware. It transmits nothing; it prints what a real stick would do.

## After changing `sdkconfig.defaults`

ESP-IDF reads `sdkconfig.defaults` only when there is no `firmware/sdkconfig`
yet. When the defaults change, as they did in 0.4.0 to turn Bluetooth on,
delete the generated file and configure again:

```bash
cd /mnt/c/Dev/chickadeeBRRD/firmware
rm -f sdkconfig
idf.py -B ~/build/chickadee-fw set-target esp32c5
idf.py -B ~/build/chickadee-fw -p /dev/ttyACM0 build flash
```

## Checks on real hardware

After a firmware change, these confirm the things only hardware can show:

1. `chickchirp info` reports the new firmware version.
2. `chickchirp raw '{"cmd":"led.selftest"}'`: the LED shows red, green, blue,
   white, then goes dark, matching the labels on the screen.
3. `chickchirp start --signal led`, then `pkill -9 chickchirp` from another
   terminal: the LED goes dark and the screen reads "last: lease expired"
   within 3 seconds.
4. `chickchirp info` lists `bt4` among the signals. If it does not, the
   Bluetooth radio did not start; `chickchirp monitor` shows why after a
   reset.
5. `chickchirp start --signal bt4 --lat <lat> --lon <lon>`: the LED blinks
   blue, the screen shows "-24 dBm  lease held", and a receiver shows
   `CHICKADEEBRRD-TEST` at that position. Then `pkill -9 chickchirp`: the
   receiver stops hearing it within 3 seconds.

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `idf.py: command not found` | ESP-IDF is not activated in this terminal | Run the `source` line above |
| `chickchirp: no stick found` | The stick is not attached to WSL | Check the usbipd `attach` window |
| `cannot open /dev/ttyACM0: Permission denied` | Your user is not in the port's group | See "Attaching the stick" |
| `Unable to acquire exclusive lock` | Another program has the port, usually a running `chickchirp start` | Stop it, or wait for it to finish |
| `no response within 2s` | The stick is not running this firmware | Flash it |
| `info` shows no `bt4` after flashing 0.4.0 | The build used an old `sdkconfig` without Bluetooth | See "After changing `sdkconfig.defaults`" |
| `the stick ended the session: the radio could not transmit` | The radio refused to start or did not confirm the power | Run `chickchirp monitor`, reset the stick, start again and read the `bt4` log lines |
| The screen still shows the old firmware version after a flash | The flash ran in a terminal without ESP-IDF, so nothing was flashed | Check for `(venv)` in the prompt, then flash again |
