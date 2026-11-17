# chickadeeBRRD protocol, version 0

How a host (the `chickchirp` CLI, later the Android app) talks to the stick
over USB serial. Version 0 covers identifying the stick, reading its status,
showing text on its screen, and running a leased session. The only signal so
far is `led`, where the stick's LED stands in for a radio.

## Transport

- The stick's built-in USB serial port: `/dev/ttyACM*` on Linux, `COM*` on
  Windows, a CDC-ACM device on Android. USB id `303a:1001`.
- Baud rate is irrelevant; it is a native USB port.
- The stick's boot messages and logs share this stream with the protocol.

## Framing

- One JSON object per line, UTF-8, terminated by `\n`. A `\r` before the
  newline is ignored.
- A line is at most 1024 bytes including the newline.
- A received line that is not a protocol object is log text. Hosts must pass
  over it without error.

## Messages

**Request**, host to stick. `id` is a number chosen by the host; `cmd` names
the command; other fields are that command's arguments.

```json
{"id":7,"cmd":"status"}
```

**Response**, stick to host. Echoes the request `id`. Exactly one per request.

```json
{"id":7,"ok":true,"state":"idle","uptime_ms":12345}
{"id":7,"ok":false,"err":"unknown_cmd","msg":"no such command: fly"}
```

**Event**, stick to host, unsolicited. Has `ev` and no `id`.

```json
{"ev":"boot","fw":"0.1.0","proto":0}
```

A host tells the three apart this way: an object with a string `ev` is an
event; an object with a numeric `id` and a boolean `ok` is a response;
anything else is log text.

## Commands

| Command | Arguments | Success fields |
|---|---|---|
| `hello` | none | `device`, `board`, `fw`, `proto`, `caps`, `signals`, `limits` |
| `status` | none | `state`, `uptime_ms`, and see below |
| `display.text` | `text` (string, at most 64 bytes) | none |
| `tx.start` | `signal` (one of `signals`) | `lease_ms`, `session_remaining_ms` |
| `lease.renew` | none | `lease_ms`, `session_remaining_ms` |
| `tx.stop` | none | `state`, `stopped` |

- `caps` lists the optional features this firmware implements: `display.text`
  and `tx`. A host should check it before offering a feature.
- `signals` lists the values `tx.start` accepts.
- `limits` holds `lease_ms`, `session_max_ms` and `text_max`.
- `state` is `idle` or `transmitting`. While transmitting, `status` adds
  `signal`, `session_remaining_ms` and `lease_remaining_ms`. Once a session
  has ended, `status` adds `last_stop` with the reason.
- `tx.stop` always succeeds; `stopped` says whether a session was running.
- `display.text` is refused with `busy` while a session runs.
- Hosts must ignore fields they do not know, so fields can be added without a
  version change.

## Sessions and the lease

The stick is active only while a host keeps asking it to be.

1. `tx.start` begins a session and grants a lease of `lease_ms`.
2. The host sends `lease.renew` before the lease runs out. Renewing three
   times per lease leaves room for a lost message.
3. The session ends when the host sends `tx.stop`, when the lease runs out,
   or when `session_max_ms` has passed since `tx.start`, whichever is first.

The stick applies these rules itself, ten times a second and before handling
every command, so a host that crashes, hangs or is unplugged cannot leave a
session running. A lease that has run out cannot be renewed; the host must
start a new session.

## Diagnostics

`led.selftest` steps the LED through red, green, blue, white and off, two
seconds each, with the name of each on the screen. It exists for hardware
bring-up and may change.

## Events

| Event | Fields | When |
|---|---|---|
| `boot` | `fw`, `proto` | Once, when the firmware starts |
| `state` | `state: "transmitting"`, `signal` | A session began |
| `state` | `state: "idle"`, `reason` | A session ended |

`reason` is `stopped` (the host sent `tx.stop`), `lease_expired` or `limit`.

## Error codes

| `err` | Meaning |
|---|---|
| `bad_json` | The line was not a JSON object. Sent with `id` 0. |
| `bad_args` | A required field is missing or has the wrong type. |
| `too_long` | The line or an argument exceeds its limit. |
| `unknown_cmd` | No such command. |
| `not_implemented` | The command exists in the protocol but not in this firmware. |
| `busy` | A session is running and the command cannot be done during one. |
| `not_active` | `lease.renew` was sent with no session running. |
| `unsupported` | The requested signal is not one this stick offers. |

## Reserved for later versions

- Radio signal types for `tx.start`, with their identity and location
  arguments, and the power cap among `limits`.
- The `display.*` namespace beyond `display.text`.

## Conformance

`testdata/vectors/v0-device-lines.jsonl` holds device lines with the kind each
must classify as. Every host implementation must pass them.
