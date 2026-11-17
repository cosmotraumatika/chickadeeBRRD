// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jamie Saker

// Host tests for what the stick puts on the air. Each message is built, then
// decoded with the Open Drone ID library's own decoder and checked.
// From the repo root:
//   sh firmware/host_test/run.sh

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "opendroneid.h"
#include "rid.h"

static int failures;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            failures++;                                                \
        }                                                              \
    } while (0)

// 2026-11-16 18:30:15.300 UTC
#define T0_MS 1794853815300LL
static const rid_fix_t FIX = {.lat_deg = 41.2565, .lon_deg = -95.9345};

static void basic_id_is_a_private_session_id(void)
{
    uint8_t msg[RID_MESSAGE_LEN];
    CHECK(rid_build(RID_MSG_BASIC_ID, &FIX, T0_MS, msg));
    ODID_BasicID_data d;
    CHECK(decodeBasicIDMessage(&d, (ODID_BasicID_encoded *)msg) == ODID_SUCCESS);
    CHECK(d.IDType == ODID_IDTYPE_SPECIFIC_SESSION_ID);
    CHECK(d.UAType == ODID_UATYPE_OTHER);
    // First byte in the private experimental range, 225..255.
    CHECK((uint8_t)d.UASID[0] >= 225);
    CHECK((uint8_t)d.UASID[0] == RID_SESSION_ID_TYPE);
    CHECK(strcmp(&d.UASID[1], "CHICKADEEBRRD-TEST") == 0);
    // The identity never looks like a serial number or a registration.
    CHECK(d.IDType != ODID_IDTYPE_SERIAL_NUMBER && d.IDType != ODID_IDTYPE_CAA_REGISTRATION_ID);
}

static void location_is_stationary_at_the_fix(void)
{
    uint8_t msg[RID_MESSAGE_LEN];
    CHECK(rid_build(RID_MSG_LOCATION, &FIX, T0_MS, msg));
    ODID_Location_data d;
    CHECK(decodeLocationMessage(&d, (ODID_Location_encoded *)msg) == ODID_SUCCESS);
    CHECK(d.Status == ODID_STATUS_GROUND);
    CHECK(fabs(d.Latitude - FIX.lat_deg) < 1e-6);
    CHECK(fabs(d.Longitude - FIX.lon_deg) < 1e-6);
    CHECK(d.SpeedHorizontal == 0);
    CHECK(d.SpeedVertical == 0);
    CHECK(d.Height == 0);
    CHECK(d.AltitudeGeo == INV_ALT);
    CHECK(d.AltitudeBaro == INV_ALT);
    // 18:30:15.3 is 1815.3 seconds past the hour.
    CHECK(fabs(d.TimeStamp - 1815.3f) < 0.05f);
}

static void location_time_wraps_at_the_hour(void)
{
    uint8_t msg[RID_MESSAGE_LEN];
    ODID_Location_data d;
    int64_t top_of_hour = T0_MS - (T0_MS % 3600000LL) + 3600000LL;
    CHECK(rid_build(RID_MSG_LOCATION, &FIX, top_of_hour - 100, msg));
    decodeLocationMessage(&d, (ODID_Location_encoded *)msg);
    CHECK(fabs(d.TimeStamp - 3599.9f) < 0.05f);
    CHECK(rid_build(RID_MSG_LOCATION, &FIX, top_of_hour + 200, msg));
    decodeLocationMessage(&d, (ODID_Location_encoded *)msg);
    CHECK(fabs(d.TimeStamp - 0.2f) < 0.05f);
}

static void system_puts_the_operator_at_the_fix(void)
{
    uint8_t msg[RID_MESSAGE_LEN];
    CHECK(rid_build(RID_MSG_SYSTEM, &FIX, T0_MS, msg));
    ODID_System_data d;
    CHECK(decodeSystemMessage(&d, (ODID_System_encoded *)msg) == ODID_SUCCESS);
    CHECK(d.OperatorLocationType == ODID_OPERATOR_LOCATION_TYPE_FIXED);
    CHECK(fabs(d.OperatorLatitude - FIX.lat_deg) < 1e-6);
    CHECK(fabs(d.OperatorLongitude - FIX.lon_deg) < 1e-6);
    CHECK(d.AreaCount == 1);
    CHECK(d.Timestamp == (uint32_t)(T0_MS / 1000 - 1546300800LL));
}

static void self_id_says_what_it_is(void)
{
    uint8_t msg[RID_MESSAGE_LEN];
    CHECK(rid_build(RID_MSG_SELF_ID, &FIX, T0_MS, msg));
    ODID_SelfID_data d;
    CHECK(decodeSelfIDMessage(&d, (ODID_SelfID_encoded *)msg) == ODID_SUCCESS);
    CHECK(d.DescType == ODID_DESC_TYPE_TEXT);
    CHECK(strcmp(d.Desc, "chickadeeBRRD:TEST") == 0);
    CHECK(strlen(RID_SELF_ID_TEXT) <= ODID_STR_SIZE);
    CHECK(strlen(RID_TEST_ID) <= ODID_ID_SIZE - 1);
}

static void bad_positions_and_times_are_refused(void)
{
    uint8_t msg[RID_MESSAGE_LEN];
    rid_fix_t f = FIX;
    f.lat_deg = 90.5;
    CHECK(!rid_fix_valid(&f) && !rid_build(RID_MSG_LOCATION, &f, T0_MS, msg));
    f = FIX;
    f.lon_deg = -180.5;
    CHECK(!rid_fix_valid(&f));
    f.lat_deg = 0;
    f.lon_deg = 0; // "unknown" in Remote ID
    CHECK(!rid_fix_valid(&f));
    f.lat_deg = NAN;
    f.lon_deg = 10;
    CHECK(!rid_fix_valid(&f));
    // A clock that was never set (near the Unix epoch) is refused.
    CHECK(!rid_build(RID_MSG_LOCATION, &FIX, 5000, msg));
    // The poles and the date line themselves are fine.
    f.lat_deg = -90;
    f.lon_deg = 180;
    CHECK(rid_fix_valid(&f));
}

static void bt4_wrapping_matches_the_reference_layout(void)
{
    uint8_t msg[RID_MESSAGE_LEN], adv[RID_BT4_ADV_LEN];
    rid_build(RID_MSG_BASIC_ID, &FIX, T0_MS, msg);
    rid_bt4_wrap(msg, 0x2A, adv);
    const uint8_t head[] = {0x1E, 0x16, 0xFA, 0xFF, 0x0D, 0x2A};
    CHECK(memcmp(adv, head, sizeof(head)) == 0);
    CHECK(memcmp(&adv[6], msg, RID_MESSAGE_LEN) == 0);
    CHECK(sizeof(adv) == 31);
    // Basic ID is message type 0, protocol version 2.
    CHECK(adv[6] == 0x02);
}

static void rotation_favours_location(void)
{
    int seen[RID_MSG_COUNT] = {0};
    for (uint32_t slot = 0; slot < 60; slot++) {
        seen[rid_schedule(slot)]++;
        // Location is never more than one slot away.
        CHECK(rid_schedule(slot) == RID_MSG_LOCATION || rid_schedule(slot + 1) == RID_MSG_LOCATION);
    }
    CHECK(seen[RID_MSG_LOCATION] == 30);
    CHECK(seen[RID_MSG_BASIC_ID] == 10 && seen[RID_MSG_SYSTEM] == 10 && seen[RID_MSG_SELF_ID] == 10);
}

int main(void)
{
    basic_id_is_a_private_session_id();
    location_is_stationary_at_the_fix();
    location_time_wraps_at_the_hour();
    system_puts_the_operator_at_the_fix();
    self_id_says_what_it_is();
    bad_positions_and_times_are_refused();
    bt4_wrapping_matches_the_reference_layout();
    rotation_favours_location();
    printf(failures ? "%d FAILED\n" : "all Remote ID message tests passed\n", failures);
    return failures ? 1 : 0;
}
