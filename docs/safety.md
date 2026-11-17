# Safety

chickadeeBRRD is a low-power test tool, not a drone. This page says what the
firmware enforces and what remains the user's responsibility.

## The principle

Limits live on the stick. The CLI and the app only ask; the firmware decides.
Changing the host software cannot remove a limit.

## What the firmware enforces

From firmware 0.4.0 the stick has a radio signal, `bt4`. These rules govern
it, and the `led` stand-in runs under the same session rules.

| Rule | Value | Where |
|---|---|---|
| A session needs a live host | The stick stops 3 seconds after the last `lease.renew` | `firmware/main/session.c` |
| A session has a time limit | 5 minutes from `tx.start` unless the host asks for longer | `firmware/main/session.c` |
| The time limit has a ceiling | 60 minutes; asking for more is refused | `firmware/main/session.c`, `main.c` |
| One session at a time | A second `tx.start` is refused and changes nothing | `firmware/main/main.c` |
| A lapsed lease cannot be revived | The host must start again | `firmware/main/session.c` |
| The radio is on only during a `bt4` session | Off before the stick reports it is idle | `firmware/main/main.c` |
| The identity is fixed | A test identity no host command can change | `firmware/main/rid.h` |
| Power is lowest by default | -24 dBm unless the host asks for more | `firmware/main/bt4.h` |
| Power has a ceiling | 0 dBm; asking for more is refused | `firmware/main/bt4.h` |
| Power is confirmed | Set and read back before transmitting; no confirmation, no transmission | `firmware/main/bt4.c` |
| A position is required | From the host with every start; 0, 0 is refused | `firmware/main/rid.c` |
| It cannot be connected to | Non-connectable advertising only; no scanning, no connections | `firmware/sdkconfig.defaults` |

The stick applies the lease and the limit ten times a second and again before
handling every command.

The chip itself can transmit at up to +20 dBm. The 0 dBm ceiling is one
constant, `BT4_POWER_MAX_DBM`, and raising it means building different
firmware.

## What has been tested, and how

| What | How | Result |
|---|---|---|
| Session rules: lease, default limit, asked-for limit, ceiling | Host tests, `firmware/host_test/test_session.c` | Pass |
| Message contents | Host tests that decode each message with the reference decoder, `test_rid.c` | Pass |
| The radio is never on without a `bt4` session, and is off before "idle" is reported | Host test of the real command handler and control loop against a stand-in radio, `test_control.c` | Pass |
| Refusals: power above the ceiling, off-step power, missing or 0, 0 position, missing clock, over-long session | The same host test | Pass |
| Lease and 5-minute limit with the `led` signal | On the stick, 2026-11-16 | Pass |
| `bt4` on the stick: received by a bench receiver, stops within 3 seconds of a killed host | On the stick | **Not yet done** |
| The power actually radiated at each setting | Needs a measurement | **Not yet done** |

The host tests prove the firmware's decisions. They cannot prove what the
radio chip does with them. Until the last two rows are filled in, treat the
power figures as what the firmware asks for, not as measured output.

## Your responsibility

- You are responsible for following the radio and aviation rules of the
  country you are in. This project does not tell you what they are.
- Whether, where and how you may transmit Remote ID test messages over the
  air is a legal question. Get an answer to it before you transmit.
- Use it on a bench, at the lowest power that reaches your receiver. A
  shielded box or enclosure keeps a test signal from reaching anyone else's
  receiver.
- The position is whatever the host gives. Give the stick's real position.
- Do not use it to imitate a real aircraft or to interfere with anyone
  else's receivers.

## Reporting a problem

If you find a way to make the stick transmit without a live host, past its
time limit, above its power ceiling or under another identity, please open an
issue. That is the kind of bug this project most
wants to hear about.
