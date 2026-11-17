# Roadmap

Each phase is small on purpose and has a "done when" that can be checked on
the bench.

| Phase | Goal | Done when | State |
|---|---|---|---|
| 0. Bench | Stick visible to the build machine | The serial port survives a reset | Done 2026-11-16 |
| 1. Hello, stick | Protocol v0, `info`, `status`, `display` | The screen shows a message sent from the CLI | Done 2026-11-16 |
| 2. Lease | `start` and `stop`, with the LED standing in for the radio | Killing the CLI stops the session within 3 seconds | Done 2026-11-16 |
| 3. One signal | Bluetooth 4 Remote ID | A bench receiver sees it | Built 2026-11-16; not yet received on the bench |
| 4. Windows | The same CLI built natively | It works on a COM port | Not started |
| 5. Android | A Kotlin app on the same protocol, using the phone's GPS | Start and stop from the phone | Not started |
| 6. Polish and share | First-launch safety notice, clean screens | A friend can use it unaided | Not started |
| 7. Polished device | Own hardware, same protocol | The CLI and app work unchanged | Not started |

## What phase 2 proved on hardware

- Killing the host process stops the session within 3 seconds.
- A session left running stops itself at 5 minutes.
- The screen and the LED both follow the session state.

## What phase 3 still has to show on hardware

- A bench receiver decodes the four messages and shows the test identity.
- Killing the host process takes the signal off the air within 3 seconds.
- The received signal strength steps down with the power setting.

## Decisions so far

| Decision | Choice | Why |
|---|---|---|
| Order of work | CLI first, Android app after | The protocol gets designed and proven on the laptop |
| Wire format | One JSON object per line | Readable by hand in a serial terminal; easy on three platforms |
| CLI language | Rust | One static binary per OS; the protocol crate can be shared with Android later |
| Firmware | C on ESP-IDF | The best-supported route to the C5's radios |
| Where limits live | On the stick | They hold even if the host software is changed |
| Session model | A lease the host must keep renewing | A dead or unplugged host cannot leave a session running |
| Laptop location | Fixed latitude and longitude supplied by the host | The laptop has no GPS; gpsd can come later |

| Test identity | A Specific Session ID in the private range, `CHICKADEEBRRD-TEST`, plus the Self-ID text `chickadeeBRRD:TEST` | The standard defines no test flag; this claims to be neither a serial number nor a registration |
| Who sets the identity | The firmware, never the host | A tester should not be able to imitate an aircraft |
| Power | -24 dBm by default, 0 dBm at most, by request only | Enough for a bench; the ceiling is far below what the chip can do |
| Duration | 5 minutes by default, up to 60 minutes by request | Long tests are possible but never the default |
| Message encoder | The Open Drone ID reference library, unmodified | Receivers are tested against it; no encoder of our own to get wrong |

## Next within phase 3

- Receive the signal on the bench and record what the receiver shows.
- Add an Authentication message with a private type as a machine-readable
  test marker.

## Later

- More signal types: Wi-Fi Remote ID on 2.4 and 5 GHz.
- Saving a registered ID of your own.
- Graphics on the screen beyond text.
