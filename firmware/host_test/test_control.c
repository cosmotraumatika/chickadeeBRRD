// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jamie Saker

// Host tests for the part of the firmware that decides when the radio is on.
//
// This compiles the real command handler and control loop (main.c), the real
// session rules and the real message builder, against a clock the test owns
// and a stand-in radio that records what it was told. Two rules are checked
// on every command, every pass of the control loop and every line written:
//
//   - the radio is on the air only while a bt4 session is active;
//   - by the time the stick says it is idle, the radio is already off.
//
// It needs cJSON, which comes with ESP-IDF. From the repo root, in a terminal
// where ESP-IDF is active:
//   sh firmware/host_test/run.sh

#define _GNU_SOURCE
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "opendroneid.h"

#include "../main/main.c"

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            exit(1);                                               \
        }                                                          \
    } while (0)

int g_lock_depth;

static int64_t s_clock_ms;
int64_t esp_timer_get_time(void)
{
    return s_clock_ms * 1000;
}

// ---- the stand-in radio ----

static bool r_ready = true;
static bool r_on_air;
static bool r_refuse;
static int r_starts, r_updates;
static int r_power = 99;
static uint8_t r_last[RID_BT4_ADV_LEN];
static rid_msg_t r_sent[4096];
static uint8_t r_sent_counter[4096];
static int r_sent_n;

static void check_rule(const char *where)
{
    if (r_on_air && !session_is_bt4()) {
        printf("FAIL at %s: on the air without a bt4 session\n", where);
        exit(1);
    }
}

static void record(const uint8_t *adv)
{
    memcpy(r_last, adv, RID_BT4_ADV_LEN);
    // Bluetooth 4 Remote ID framing: service data, UUID 0xFFFA, app code 0x0D.
    CHECK(adv[0] == 0x1E && adv[1] == 0x16 && adv[2] == 0xFA && adv[3] == 0xFF && adv[4] == 0x0D);
    rid_msg_t which = RID_MSG_COUNT;
    switch (adv[6] >> 4) {
    case 0: which = RID_MSG_BASIC_ID; break;
    case 1: which = RID_MSG_LOCATION; break;
    case 3: which = RID_MSG_SELF_ID; break;
    case 4: which = RID_MSG_SYSTEM; break;
    }
    CHECK(which != RID_MSG_COUNT);
    CHECK(r_sent_n < 4096);
    r_sent_counter[r_sent_n] = adv[5];
    r_sent[r_sent_n++] = which;
}

esp_err_t bt4_init(void)
{
    return ESP_OK;
}

bool bt4_ready(void)
{
    return r_ready;
}

bool bt4_power_allowed(int dbm)
{
    return dbm >= BT4_POWER_MIN_DBM && dbm <= BT4_POWER_MAX_DBM && (dbm - BT4_POWER_MIN_DBM) % BT4_POWER_STEP_DB == 0;
}

esp_err_t bt4_start(int power_dbm, const uint8_t adv[BT4_ADV_LEN])
{
    CHECK(g_lock_depth == 1);
    CHECK(bt4_power_allowed(power_dbm));
    if (r_refuse) {
        return ESP_FAIL;
    }
    r_starts++;
    r_power = power_dbm;
    r_on_air = true;
    record(adv);
    check_rule("radio start");
    return ESP_OK;
}

esp_err_t bt4_update(const uint8_t adv[BT4_ADV_LEN])
{
    CHECK(g_lock_depth == 1);
    CHECK(r_on_air);
    r_updates++;
    record(adv);
    check_rule("radio update");
    return ESP_OK;
}

void bt4_stop(void)
{
    CHECK(g_lock_depth == 1);
    r_on_air = false;
}

// ---- the rest of the board ----

static char s_out[65536];

// Like the real driver, this refuses a write larger than its transmit buffer.
// Firmware 0.4.0 shipped a hello reply longer than the driver's default 256
// bytes, which the stick then silently never sent.
int usb_serial_jtag_write_bytes(const void *src, size_t size, int ticks)
{
    (void)ticks;
    if (size > USB_BUFFER_SIZE || size > MAX_LINE) {
        printf("FAIL: a %u-byte line is too long to send: %.*s", (unsigned)size, (int)size, (const char *)src);
        exit(1);
    }
    check_rule("a line being written");
    if (memmem(src, size, "\"state\":\"idle\"", 14) != NULL) {
        CHECK(!r_on_air);
    }
    strncat(s_out, src, size);
    return (int)size;
}

int usb_serial_jtag_read_bytes(void *buf, uint32_t length, int ticks)
{
    (void)buf;
    (void)length;
    (void)ticks;
    return 0;
}

int usb_serial_jtag_driver_install(usb_serial_jtag_driver_config_t *cfg)
{
    (void)cfg;
    return 0;
}

void usb_serial_jtag_vfs_use_driver(void) {}

const char *esp_err_to_name(esp_err_t code)
{
    (void)code;
    return "error";
}

TickType_t xTaskGetTickCount(void)
{
    return 0;
}

int xTaskCreate(void (*fn)(void *), const char *name, int stack, void *arg, int prio, void *handle)
{
    (void)fn;
    (void)name;
    (void)stack;
    (void)arg;
    (void)prio;
    (void)handle;
    return 1;
}

static char d_tx_note[64];
static char d_idle_note[64];

bool display_ready(void)
{
    return true;
}

esp_err_t display_init(void)
{
    return ESP_OK;
}

void display_show_idle(const char *fw_version, const char *note)
{
    (void)fw_version;
    snprintf(d_idle_note, sizeof(d_idle_note), "%s", note != NULL ? note : "");
}

void display_show_tx(const char *signal, int seconds_left, const char *note)
{
    (void)signal;
    (void)seconds_left;
    CHECK(strlen(note) <= 19); // what fits on the screen's bottom line
    snprintf(d_tx_note, sizeof(d_tx_note), "%s", note);
}

void display_show_text(const char *text)
{
    (void)text;
}

void led_init(void) {}

void led_set(uint8_t r, uint8_t g, uint8_t b)
{
    (void)r;
    (void)g;
    (void)b;
}

// ---- driving it ----

static void cmd(const char *line)
{
    s_out[0] = '\0';
    handle_line(line, strlen(line));
    CHECK(g_lock_depth == 0);
    check_rule("the end of a command");
}

#define SAID(needle)                                                                      \
    do {                                                                                  \
        if (strstr(s_out, needle) == NULL) {                                              \
            printf("FAIL %s:%d: wanted %s\n  got %s\n", __FILE__, __LINE__, needle, s_out); \
            exit(1);                                                                      \
        }                                                                                 \
    } while (0)

// Run the control loop for a number of 100 ms passes. The loop never returns
// on the stick; here its delay call jumps back out when the passes are used up.
static jmp_buf s_back;
static int s_passes_left;
static bool s_renewing;

void vTaskDelayUntil(TickType_t *wake, int ticks)
{
    (void)wake;
    CHECK(g_lock_depth == 0);
    check_rule("a pass of the control loop");
    if (s_passes_left-- <= 0) {
        longjmp(s_back, 1);
    }
    s_clock_ms += ticks;
    if (s_renewing && s_clock_ms % 1000 == 0) {
        // The host renewing once a second, as chickchirp does.
        char keep[sizeof(s_out)];
        strcpy(keep, s_out);
        const char *renew = "{\"id\":9,\"cmd\":\"lease.renew\"}";
        handle_line(renew, strlen(renew));
        strcpy(s_out, keep);
    }
}

static void run(int passes, bool renewing)
{
    s_passes_left = passes;
    s_renewing = renewing;
    s_out[0] = '\0';
    if (setjmp(s_back) == 0) {
        ui_task(NULL);
    }
}

#define BT4_AT_HOME "\"signal\":\"bt4\",\"lat\":40.9,\"lon\":-95.6,\"time\":1794000000"

static void hello_describes_the_limits(void)
{
    cmd("{\"id\":1,\"cmd\":\"hello\"}");
    SAID("\"signals\":[\"led\",\"bt4\"]");
    SAID("\"session_default_ms\":300000");
    SAID("\"session_max_ms\":3600000");
    SAID("\"power_default_dbm\":-24");
    SAID("\"power_max_dbm\":0");
    SAID("\"id\":\"CHICKADEEBRRD-TEST\"");
}

static void bad_requests_are_refused_before_anything_starts(void)
{
    cmd("{\"id\":2,\"cmd\":\"tx.start\",\"signal\":\"bt4\"}");
    SAID("bad_args");
    cmd("{\"id\":2,\"cmd\":\"tx.start\",\"signal\":\"bt4\",\"lat\":0,\"lon\":0,\"time\":1794000000}");
    SAID("bad_args");
    cmd("{\"id\":2,\"cmd\":\"tx.start\",\"signal\":\"bt4\",\"lat\":91,\"lon\":10,\"time\":1794000000}");
    SAID("bad_args");
    cmd("{\"id\":2,\"cmd\":\"tx.start\",\"signal\":\"bt4\",\"lat\":40.9,\"lon\":-95.6}");
    SAID("bad_args");
    cmd("{\"id\":2,\"cmd\":\"tx.start\",\"signal\":\"bt4\",\"lat\":40.9,\"lon\":-95.6,\"time\":12}");
    SAID("bad_args");
    cmd("{\"id\":2,\"cmd\":\"tx.start\"," BT4_AT_HOME ",\"power_dbm\":3}");
    SAID("limit_exceeded");
    cmd("{\"id\":2,\"cmd\":\"tx.start\"," BT4_AT_HOME ",\"power_dbm\":20}");
    SAID("limit_exceeded");
    cmd("{\"id\":2,\"cmd\":\"tx.start\"," BT4_AT_HOME ",\"power_dbm\":-23}");
    SAID("bad_args");
    cmd("{\"id\":2,\"cmd\":\"tx.start\"," BT4_AT_HOME ",\"power_dbm\":-27}");
    SAID("bad_args");
    cmd("{\"id\":2,\"cmd\":\"tx.start\"," BT4_AT_HOME ",\"power_dbm\":-6.5}");
    SAID("bad_args");
    cmd("{\"id\":2,\"cmd\":\"tx.start\"," BT4_AT_HOME ",\"max_ms\":3600001}");
    SAID("limit_exceeded");
    cmd("{\"id\":2,\"cmd\":\"tx.start\"," BT4_AT_HOME ",\"max_ms\":0}");
    SAID("bad_args");
    cmd("{\"id\":2,\"cmd\":\"tx.start\",\"signal\":\"led\",\"max_ms\":9999999}");
    SAID("limit_exceeded");
    CHECK(!s_session.active);
    CHECK(r_starts == 0);
}

static void a_default_session_sends_the_rotation_at_lowest_power(void)
{
    cmd("{\"id\":3,\"cmd\":\"tx.start\"," BT4_AT_HOME "}");
    SAID("\"ok\":true");
    SAID("\"session_remaining_ms\":300000");
    SAID("\"power_dbm\":-24");
    SAID("\"state\":\"transmitting\"");

    run(12, true);
    CHECK(r_on_air);
    CHECK(r_starts == 1 && r_updates == 11);
    CHECK(r_power == -24);
    const rid_msg_t want[6] = {RID_MSG_LOCATION, RID_MSG_BASIC_ID, RID_MSG_LOCATION,
                               RID_MSG_SYSTEM,   RID_MSG_LOCATION, RID_MSG_SELF_ID};
    for (int i = 0; i < 12; i++) {
        CHECK(r_sent[i] == want[i % 6]);
    }
    // Each message type keeps its own counter.
    CHECK(r_sent_counter[0] == 0 && r_sent_counter[2] == 1 && r_sent_counter[10] == 5);
    CHECK(r_sent_counter[1] == 0 && r_sent_counter[7] == 1);
    CHECK(strcmp(d_tx_note, "-24 dBm  lease held") == 0);

    // The twelfth message is a Self-ID, and it says what this is.
    ODID_SelfID_data self_id;
    CHECK(decodeSelfIDMessage(&self_id, (ODID_SelfID_encoded *)(r_last + 6)) == ODID_SUCCESS);
    CHECK(strcmp(self_id.Desc, RID_SELF_ID_TEXT) == 0);

    cmd("{\"id\":4,\"cmd\":\"status\"}");
    SAID("\"signal\":\"bt4\"");
    SAID("\"power_dbm\":-24");
    SAID("\"lat\":40.9");
    SAID("\"lon\":-95.6");

    // A second start is refused and changes nothing about the first.
    cmd("{\"id\":5,\"cmd\":\"tx.start\",\"signal\":\"bt4\",\"lat\":1,\"lon\":1,\"time\":1794000000,\"power_dbm\":0}");
    SAID("busy");
    CHECK(s_radio.power_dbm == -24);
}

static void a_silent_host_takes_it_off_the_air(void)
{
    int64_t last_renewal = (s_clock_ms / 1000) * 1000;
    int passes = 0;
    while (r_on_air && passes < 100) {
        run(1, false);
        passes++;
    }
    CHECK(!r_on_air);
    CHECK(s_clock_ms - last_renewal <= SESSION_LEASE_MS + 100);
    SAID("lease_expired");

    int starts = r_starts, updates = r_updates;
    run(20, false);
    CHECK(!r_on_air && r_starts == starts && r_updates == updates);
}

// The control loop is not the only thing that enforces the lease: a command
// that arrives after it lapsed, before the loop's next pass, does it too.
static void a_late_command_takes_it_off_the_air_itself(void)
{
    cmd("{\"id\":20,\"cmd\":\"tx.start\"," BT4_AT_HOME "}");
    run(3, true);
    CHECK(r_on_air);
    s_clock_ms += SESSION_LEASE_MS + 50; // the loop has not run in this time
    cmd("{\"id\":21,\"cmd\":\"lease.renew\"}");
    SAID("lease_expired");
    SAID("not_active");
    CHECK(!r_on_air);
    run(5, false);
    CHECK(!r_on_air);
}

static void asked_for_power_and_time_are_used_and_the_limit_ends_it(void)
{
    r_sent_n = 0;
    cmd("{\"id\":6,\"cmd\":\"tx.start\"," BT4_AT_HOME ",\"power_dbm\":-6,\"max_ms\":1500}");
    SAID("\"power_dbm\":-6");
    SAID("\"session_remaining_ms\":1500");
    run(30, true);
    SAID("\"reason\":\"limit\"");
    CHECK(!r_on_air);
    CHECK(r_power == -6);
    CHECK(r_sent_n >= 14 && r_sent_n <= 15);
    CHECK(strcmp(d_idle_note, "last: time limit") == 0);
}

static void stop_takes_it_off_the_air_before_the_reply(void)
{
    cmd("{\"id\":7,\"cmd\":\"tx.start\",\"signal\":\"bt4\",\"lat\":-33.9,\"lon\":151.2,"
        "\"time\":1794000200,\"max_ms\":3600000}");
    SAID("\"session_remaining_ms\":3600000");
    run(5, true);
    CHECK(r_on_air);

    // The position on the air is the one asked for, and its clock is the
    // host's clock plus the time since.
    ODID_Location_data loc;
    CHECK(r_sent[r_sent_n - 1] == RID_MSG_LOCATION);
    CHECK(decodeLocationMessage(&loc, (ODID_Location_encoded *)(r_last + 6)) == ODID_SUCCESS);
    CHECK(loc.Latitude > -33.901 && loc.Latitude < -33.899);
    CHECK(loc.Longitude > 151.199 && loc.Longitude < 151.201);
    // 1794000200 is 1400 s into its hour; 0.5 s have passed.
    CHECK(loc.TimeStamp > 1400.4f && loc.TimeStamp < 1400.6f);

    // The write stand-in checks the radio is off when the reply goes out.
    cmd("{\"id\":8,\"cmd\":\"tx.stop\"}");
    SAID("\"stopped\":true");
    CHECK(!r_on_air);
    run(10, false);
    CHECK(!r_on_air);
    cmd("{\"id\":8,\"cmd\":\"tx.stop\"}");
    SAID("\"stopped\":false");
}

static void an_led_session_never_touches_the_radio(void)
{
    int starts = r_starts, updates = r_updates;
    cmd("{\"id\":10,\"cmd\":\"tx.start\",\"signal\":\"led\",\"max_ms\":2000}");
    run(25, true);
    SAID("\"reason\":\"limit\"");
    CHECK(r_starts == starts && r_updates == updates);
    CHECK(strcmp(d_tx_note, "host lease held") == 0);
}

static void a_radio_that_refuses_ends_the_session(void)
{
    r_refuse = true;
    cmd("{\"id\":11,\"cmd\":\"tx.start\"," BT4_AT_HOME "}");
    SAID("\"ok\":true");
    run(2, true);
    SAID("\"reason\":\"error\"");
    CHECK(!s_session.active && !r_on_air);
    cmd("{\"id\":12,\"cmd\":\"status\"}");
    SAID("\"last_stop\":\"error\"");
    CHECK(strcmp(d_idle_note, "last: radio error") == 0);
    r_refuse = false;
}

static void without_bluetooth_the_signal_is_not_offered(void)
{
    r_ready = false;
    cmd("{\"id\":13,\"cmd\":\"hello\"}");
    SAID("\"signals\":[\"led\"]");
    cmd("{\"id\":14,\"cmd\":\"tx.start\"," BT4_AT_HOME "}");
    SAID("unsupported");
    r_ready = true;
}

int main(void)
{
    s_lock = 1;
    session_init(&s_session);
    s_clock_ms = 1000;

    hello_describes_the_limits();
    bad_requests_are_refused_before_anything_starts();
    a_default_session_sends_the_rotation_at_lowest_power();
    a_silent_host_takes_it_off_the_air();
    a_late_command_takes_it_off_the_air_itself();
    asked_for_power_and_time_are_used_and_the_limit_ends_it();
    stop_takes_it_off_the_air_before_the_reply();
    an_led_session_never_touches_the_radio();
    a_radio_that_refuses_ends_the_session();
    without_bluetooth_the_signal_is_not_offered();

    printf("all command and radio-control tests passed\n");
    return 0;
}
