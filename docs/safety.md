# Safety

chickadeeBRRD is a low-power test tool, not a drone. This page says what the
firmware enforces and what remains the user's responsibility.

## The principle

Limits live on the stick. The CLI and the app only ask; the firmware decides.
Changing the host software cannot remove a limit.

## What the firmware enforces today

There is no radio code yet, so today these rules govern the LED that stands in
for the radio. They are the same rules a real signal will run under.

| Rule | Value | Where |
|---|---|---|
| A session needs a live host | The stick stops 3 seconds after the last `lease.renew` | `firmware/main/session.c` |
| A session has a time limit | 5 minutes from `tx.start` | `firmware/main/session.c` |
| One session at a time | A second `tx.start` is refused | `firmware/main/main.c` |
| A lapsed lease cannot be revived | The host must start again | `firmware/main/session.c` |

The stick applies the lease and the limit ten times a second and again before
handling every command. The rules have host tests in `firmware/host_test/`.

Both were checked on hardware on 2026-11-16: killing the host process stopped
a session within 3 seconds, and a session left alone stopped at 5 minutes.

## Planned for the first radio signal

These are decided but not yet built.

- A fixed, self-declaring test identity that a host cannot change.
- The lowest transmit power by default. A higher setting only when the host
  asks for it explicitly.
- 5 minutes by default. Longer only when asked for explicitly, with a hard
  ceiling of 60 minutes in the firmware.
- A latitude and longitude from the host with every start.

## Your responsibility

- You are responsible for following the radio and aviation rules of the
  country you are in. This project does not tell you what they are.
- Use it on a bench, at the lowest power that reaches your receiver.
- Do not use it to imitate a real aircraft or to interfere with anyone
  else's receivers.

## Reporting a problem

If you find a way to make the stick run a session without a live host, or past
its time limit, please open an issue. That is the kind of bug this project most
wants to hear about.
