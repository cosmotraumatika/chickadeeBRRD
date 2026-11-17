#pragma once
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jamie Saker
//
// What the stick says on the air: four Remote ID messages describing a
// stationary test emitter, and their wrapping for Bluetooth 4 legacy
// advertising. No radio code here, so it runs unchanged in host tests.
//
// The identity is fixed in this file on purpose. A host chooses where and
// when; it cannot choose who.

#include <stdbool.h>
#include <stdint.h>

// Basic ID: a Specific Session ID whose first byte is in the range set aside
// for private experimental use, followed by this text. It claims to be
// neither a manufacturer serial number nor a registration.
#define RID_SESSION_ID_TYPE 0xE1
#define RID_TEST_ID "CHICKADEEBRRD-TEST"

// Self-ID: plain text any receiver app can show a person.
#define RID_SELF_ID_TEXT "chickadeeBRRD:TEST"

#define RID_MESSAGE_LEN 25
#define RID_BT4_ADV_LEN 31

typedef enum {
    RID_MSG_BASIC_ID,
    RID_MSG_LOCATION,
    RID_MSG_SYSTEM,
    RID_MSG_SELF_ID,
    RID_MSG_COUNT,
} rid_msg_t;

typedef struct {
    double lat_deg; // -90..90
    double lon_deg; // -180..180
} rid_fix_t;

// True when the fix is a usable position. 0,0 is refused: Remote ID uses it
// to mean "unknown".
bool rid_fix_valid(const rid_fix_t *fix);

// Encode one message as it stands at `unix_ms` (UTC). Returns false if the
// encoder rejects the values.
bool rid_build(rid_msg_t which, const rid_fix_t *fix, int64_t unix_ms, uint8_t out[RID_MESSAGE_LEN]);

// Wrap a message as the 31 bytes of a Bluetooth 4 legacy advertisement.
void rid_bt4_wrap(const uint8_t msg[RID_MESSAGE_LEN], uint8_t counter, uint8_t out[RID_BT4_ADV_LEN]);

// Which message goes out in rotation slot `slot`. Location comes round every
// second slot; each of the others every sixth.
rid_msg_t rid_schedule(uint32_t slot);
