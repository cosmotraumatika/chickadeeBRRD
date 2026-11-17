// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jamie Saker

#include "rid.h"

#include <string.h>

#include "opendroneid.h"

// Seconds between the Unix epoch and 2019-01-01, the System message's epoch.
#define ODID_EPOCH_OFFSET_S 1546300800LL

bool rid_fix_valid(const rid_fix_t *fix)
{
    if (!(fix->lat_deg >= -90.0 && fix->lat_deg <= 90.0)) {
        return false;
    }
    if (!(fix->lon_deg >= -180.0 && fix->lon_deg <= 180.0)) {
        return false;
    }
    return !(fix->lat_deg == 0.0 && fix->lon_deg == 0.0);
}

static bool build_basic_id(uint8_t *out)
{
    ODID_BasicID_data d;
    odid_initBasicIDData(&d);
    d.UAType = ODID_UATYPE_OTHER;
    d.IDType = ODID_IDTYPE_SPECIFIC_SESSION_ID;
    memset(d.UASID, 0, sizeof(d.UASID));
    d.UASID[0] = (char)RID_SESSION_ID_TYPE;
    memcpy(&d.UASID[1], RID_TEST_ID, strlen(RID_TEST_ID));
    return encodeBasicIDMessage((ODID_BasicID_encoded *)out, &d) == ODID_SUCCESS;
}

// A stationary emitter on the ground: position known, everything else
// declared unknown instead of invented.
static bool build_location(const rid_fix_t *fix, int64_t unix_ms, uint8_t *out)
{
    ODID_Location_data d;
    odid_initLocationData(&d);
    d.Status = ODID_STATUS_GROUND;
    d.Direction = INV_DIR;
    d.SpeedHorizontal = 0;
    d.SpeedVertical = 0;
    d.Latitude = fix->lat_deg;
    d.Longitude = fix->lon_deg;
    d.AltitudeBaro = INV_ALT;
    d.AltitudeGeo = INV_ALT;
    d.HeightType = ODID_HEIGHT_REF_OVER_TAKEOFF;
    d.Height = 0;
    d.HorizAccuracy = ODID_HOR_ACC_UNKNOWN;
    d.VertAccuracy = ODID_VER_ACC_UNKNOWN;
    d.BaroAccuracy = ODID_VER_ACC_UNKNOWN;
    d.SpeedAccuracy = ODID_SPEED_ACC_UNKNOWN;
    d.TSAccuracy = ODID_TIME_ACC_UNKNOWN;
    // Seconds since the top of the UTC hour, to a tenth.
    d.TimeStamp = (float)((unix_ms % 3600000LL) / 100) / 10.0f;
    return encodeLocationMessage((ODID_Location_encoded *)out, &d) == ODID_SUCCESS;
}

static bool build_system(const rid_fix_t *fix, int64_t unix_ms, uint8_t *out)
{
    ODID_System_data d;
    odid_initSystemData(&d);
    d.OperatorLocationType = ODID_OPERATOR_LOCATION_TYPE_FIXED;
    d.ClassificationType = ODID_CLASSIFICATION_TYPE_UNDECLARED;
    d.OperatorLatitude = fix->lat_deg;
    d.OperatorLongitude = fix->lon_deg;
    d.AreaCount = 1;
    d.AreaRadius = 0;
    d.AreaCeiling = INV_ALT;
    d.AreaFloor = INV_ALT;
    d.OperatorAltitudeGeo = INV_ALT;
    d.Timestamp = (uint32_t)(unix_ms / 1000 - ODID_EPOCH_OFFSET_S);
    return encodeSystemMessage((ODID_System_encoded *)out, &d) == ODID_SUCCESS;
}

static bool build_self_id(uint8_t *out)
{
    ODID_SelfID_data d;
    odid_initSelfIDData(&d);
    d.DescType = ODID_DESC_TYPE_TEXT;
    memset(d.Desc, 0, sizeof(d.Desc));
    memcpy(d.Desc, RID_SELF_ID_TEXT, strlen(RID_SELF_ID_TEXT));
    return encodeSelfIDMessage((ODID_SelfID_encoded *)out, &d) == ODID_SUCCESS;
}

bool rid_build(rid_msg_t which, const rid_fix_t *fix, int64_t unix_ms, uint8_t out[RID_MESSAGE_LEN])
{
    memset(out, 0, RID_MESSAGE_LEN);
    if (!rid_fix_valid(fix) || unix_ms < ODID_EPOCH_OFFSET_S * 1000) {
        return false;
    }
    switch (which) {
    case RID_MSG_BASIC_ID:
        return build_basic_id(out);
    case RID_MSG_LOCATION:
        return build_location(fix, unix_ms, out);
    case RID_MSG_SYSTEM:
        return build_system(fix, unix_ms, out);
    case RID_MSG_SELF_ID:
        return build_self_id(out);
    default:
        return false;
    }
}

void rid_bt4_wrap(const uint8_t msg[RID_MESSAGE_LEN], uint8_t counter, uint8_t out[RID_BT4_ADV_LEN])
{
    out[0] = 0x1E; // length of what follows
    out[1] = 0x16; // service data, 16-bit UUID
    out[2] = 0xFA; // UUID 0xFFFA: ASTM Remote ID
    out[3] = 0xFF;
    out[4] = 0x0D; // application code: Open Drone ID
    out[5] = counter;
    memcpy(&out[6], msg, RID_MESSAGE_LEN);
}

rid_msg_t rid_schedule(uint32_t slot)
{
    static const rid_msg_t order[] = {
        RID_MSG_LOCATION, RID_MSG_BASIC_ID, RID_MSG_LOCATION, RID_MSG_SYSTEM, RID_MSG_LOCATION, RID_MSG_SELF_ID,
    };
    return order[slot % (sizeof(order) / sizeof(order[0]))];
}
