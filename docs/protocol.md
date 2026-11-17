# chickadeeBRRD protocol, version 0

How a host (the `chickchirp` CLI, later the Android app) talks to the stick
over USB serial. Version 0 covers identifying the stick, reading its status,
showing text on its screen, and running a leased session. A session sends
one of two signals: `led`, where the stick's LED stands in for a radio, or
`bt4`, Remote ID test messages over Bluetooth 4.

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
| `tx.start` | `signal` (one of `signals`), and see "Signals" | `lease_ms`, `session_remaining_ms`, and `power_dbm` for `bt4` |
| `lease.renew` | `time` (optional) | `lease_ms`, `session_remaining_ms` |
| `tx.stop` | none | `state`, `stopped` |

- `caps` lists the optional features this firmware implements: `display.text`
  and `tx`. A host should check it before offering a feature.
- `signals` lists the values `tx.start` accepts. `bt4` is listed only if the
  Bluetooth radio started.
- `limits` holds the numbers in the table below.
- `rid` holds the identity the stick broadcasts: `id_type`, `id` and
  `self_id`. It is fixed in the firmware; no command changes it.
- `state` is `idle` or `transmitting`. While transmitting, `status` adds
  `signal`, `session_remaining_ms` and `lease_remaining_ms`, and for `bt4`
  also `power_dbm`, `lat` and `lon`. Once a session has ended, `status` adds
  `last_stop` with the reason.
- `tx.stop` always succeeds; `stopped` says whether a session was running.
- `display.text` is refused with `busy` while a session runs.
- Hosts must ignore fields they do not know, so fields can be added without a
  version change.

### Limits

| Field | Value in firmware 0.4.0 | Meaning |
|---|---|---|
| `lease_ms` | 3000 | A session ends if this long passes without `lease.renew` |
| `session_default_ms` | 300000 | A session's time limit when `tx.start` gives no `max_ms` |
| `session_max_ms` | 3600000 | The longest `max_ms` the stick accepts |
| `power_min_dbm` | -24 | The lowest transmit power |
| `power_default_dbm` | -24 | The power used when `tx.start` gives no `power_dbm` |
| `power_max_dbm` | 0 | The highest power the stick accepts |
| `power_step_db` | 3 | Power settings are this far apart, starting at the lowest |
| `text_max` | 64 | Longest `display.text`, in bytes |

Firmware before 0.4.0 reported only `lease_ms`, `session_max_ms` (then the
single 5-minute limit) and `text_max`.

## Signals

Arguments to `tx.start` beyond `signal`:

| Argument | `led` | `bt4` | Meaning |
|---|---|---|---|
| `max_ms` | optional | optional | Time limit for this session. More than `session_max_ms` is refused with `limit_exceeded` |
| `lat`, `lon` | ignored | required | The position to broadcast, in degrees. Out of range, or 0, 0, is refused with `bad_args` |
| `time` | ignored | required | The host's clock, in Unix seconds; fractions allowed. The stick has no clock of its own |
| `power_dbm` | ignored | optional | Transmit power, a whole number. Above `power_max_dbm` is refused with `limit_exceeded`; off the steps is `bad_args` |

```json
{"id":3,"cmd":"tx.start","signal":"bt4","lat":40.9,"lon":-95.6,"time":1794000000.25}
{"id":3,"ok":true,"lease_ms":3000,"session_remaining_ms":300000,"power_dbm":-24}
```

- Every argument is checked before the session begins. A refused `tx.start`
  starts nothing and changes nothing.
- A host should send `time` with every `lease.renew` during a `bt4` session,
  so the stick's copy of the clock does not drift.
- The stick sets the transmit power and reads it back before it transmits.
  If the radio does not confirm the power, or will not start, the session
  ends with reason `error`.
- What `bt4` broadcasts is described in [remote-id.md](remote-id.md).

## Sessions and the lease

The stick is active only while a host keeps asking it to be.

1. `tx.start` begins a session and grants a lease of `lease_ms`.
2. The host sends `lease.renew` before the lease runs out. Renewing three
   times per lease leaves room for a lost message.
3. The session ends when the host sends `tx.stop`, when the lease runs out,
   or when its time limit has passed since `tx.start`, whichever is first.
   The time limit is `max_ms` if `tx.start` gave one, else
   `session_default_ms`.

The stick applies these rules itself, ten times a second and before handling
every command, so a host that crashes, hangs or is unplugged cannot leave a
session running. A lease that has run out cannot be renewed; the host must
start a new session.

For `bt4`, the radio is taken off the air before the stick writes the reply
to `tx.stop` or the `idle` event, so a host that has been told the stick is
idle can rely on it.

## Diagnostics

`led.selftest` steps the LED through red, green, blue, white and off, two
seconds each, with the name of each on the screen. It exists for hardware
bring-up and may change.

## Events

| Event | Fields | When |
|---|---|---|
| `boot` | `fw`, `proto` | Once, when the firmware starts |
| `state` | `state: "transmitting"`, `signal`, and `power_dbm` for `bt4` | A session began |
| `state` | `state: "idle"`, `reason` | A session ended |

`reason` is `stopped` (the host sent `tx.stop`), `lease_expired`, `limit`
(the session's time limit) or `error` (the radio could not transmit).

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
| `limit_exceeded` | The request asks for more than the firmware allows: power or session length. |

## Reserved for later versions

- More radio signal types for `tx.start`: Bluetooth 5 long range and Wi-Fi.
- An Authentication message carrying a machine-readable test marker.
- The `display.*` namespace beyond `display.text`.

## Conformance

`testdata/vectors/v0-device-lines.jsonl` holds device lines with the kind each
must classify as. Every host implementation must pass them.
