# Roadmap

Each phase is small on purpose and has a "done when" that can be checked on
the bench.

| Phase | Goal | Done when | State |
|---|---|---|---|
| 0. Bench | Stick visible to the build machine | The serial port survives a reset | Done 2026-11-16 |
| 1. Hello, stick | Protocol v0, `info`, `status`, `display` | The screen shows a message sent from the CLI | Done 2026-11-16 |
| 2. Lease | `start` and `stop`, with the LED standing in for the radio | Killing the CLI stops the session within 3 seconds | Done 2026-11-16 |
| 3. One signal | Bluetooth 4 Remote ID | A bench receiver sees it | Next |
| 4. Windows | The same CLI built natively | It works on a COM port | Not started |
| 5. Android | A Kotlin app on the same protocol, using the phone's GPS | Start and stop from the phone | Not started |
| 6. Polish and share | First-launch safety notice, clean screens | A friend can use it unaided | Not started |
| 7. Polished device | Own hardware, same protocol | The CLI and app work unchanged | Not started |

## What phase 2 proved on hardware

- Killing the host process stops the session within 3 seconds.
- A session left running stops itself at 5 minutes.
- The screen and the LED both follow the session state.

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

## Decided for phase 3, not yet built

- **Identity:** a fixed, self-declaring test identity that the host cannot change.
- **Location:** the host must supply latitude and longitude with every start.
- **Power:** the lowest setting by default; a higher setting only when asked
  for explicitly.
- **Duration:** 5 minutes by default; longer only when asked for explicitly,
  and never more than 60 minutes.

## Later

- More signal types: Wi-Fi Remote ID on 2.4 and 5 GHz.
- Saving a registered ID of your own.
- Graphics on the screen beyond text.
