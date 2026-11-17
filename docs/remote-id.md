# What the stick puts on the air

This page describes the `bt4` signal exactly: every message, every field, and
how a receiver can tell it is a test. The code is `firmware/main/rid.c`; the
encoder is the Open Drone ID reference library, unmodified, in
`firmware/components/opendroneid/`.

The stick transmits only during a `bt4` session, which needs a host renewing
a lease. See [safety.md](safety.md).

## The transport

Bluetooth 4 legacy advertising, as ASTM F3411 defines it for Remote ID.

| Property | Value |
|---|---|
| Advertising type | Non-connectable, non-scannable, undirected |
| Channels | The three advertising channels (37, 38, 39) |
| Advertising interval | 50 ms |
| Message change | A new message every 100 ms, so each goes out about twice per channel |
| Address | The chip's own public Bluetooth address |
| Power | -24 dBm unless the host asks for more; 0 dBm at most |

Each advertisement is 31 bytes:

| Bytes | Value | Meaning |
|---|---|---|
| 0 | `1E` | Length of what follows |
| 1 | `16` | Service data, 16-bit UUID |
| 2-3 | `FA FF` | UUID 0xFFFA, ASTM Remote ID |
| 4 | `0D` | Application code: Open Drone ID |
| 5 | counter | Counts up each time this message type is sent |
| 6-30 | message | One 25-byte Open Drone ID message |

## The rotation

Six slots of 100 ms, repeating:

| Slot | Message |
|---|---|
| 0, 2, 4 | Location |
| 1 | Basic ID |
| 3 | System |
| 5 | Self-ID |

Location goes out every 200 ms; each of the others every 600 ms.

## The messages

### Basic ID

| Field | Value |
|---|---|
| ID type | 4, Specific Session ID |
| UA type | 15, Other |
| ID, first byte | `0xE1`, in the range the encoder marks for private use |
| ID, remaining bytes | `CHICKADEEBRRD-TEST`, then zero padding |

The identity is a constant in the firmware. No host command can change it. It
is not a manufacturer serial number and not a registration, and it does not
claim to be either.

### Location

| Field | Value |
|---|---|
| Status | Ground |
| Latitude, longitude | As given by the host at `tx.start` |
| Direction | Unknown |
| Horizontal and vertical speed | 0 |
| Pressure and geodetic altitude | Unknown |
| Height | 0 m above take-off |
| All accuracies | Unknown |
| Timestamp | Tenths of a second since the top of the UTC hour, from the host's clock |

### System

| Field | Value |
|---|---|
| Operator location type | Fixed |
| Operator latitude, longitude | The same position as the Location message |
| Classification | Undeclared |
| Area count, radius | 1, 0 |
| Area ceiling and floor, operator altitude | Unknown |
| Timestamp | Seconds since 2019-01-01 UTC, from the host's clock |

### Self-ID

| Field | Value |
|---|---|
| Description type | 0, Text |
| Description | `chickadeeBRRD:TEST` |

## How a receiver can tell it is a test

Two markers are in every rotation, and a third is planned.

1. **The Basic ID.** A Specific Session ID whose first byte is in the private
   range, followed by text that ends in `-TEST`.
2. **The Self-ID.** Free text that a receiver app shows to a person:
   `chickadeeBRRD:TEST`.
3. **Planned: an Authentication message** with a private authentication type,
   carrying a marker that software can match without reading text. It will be
   added once the first signal has been received on the bench.

ASTM F3411 does not define a test flag, and no registry of test identities was
found, so these markers are this project's own convention. A receiver that
wants to filter test traffic has to be told about them.

## Things to check against your copy of the standard

- The private range for the first byte of a Specific Session ID. The encoder's
  header gives 225 to 255; `0xE1` is 225.
- Whether your receiver displays ID type 4. Some show only serial numbers and
  registrations. If yours does not, the Self-ID text is still there.

## The clock and the position

The stick has no clock and no GPS. The host sends its clock with `tx.start`
and with every lease renewal, and the stick counts forward from the most
recent one. The position is fixed for the whole session; to move it, stop and
start again.
