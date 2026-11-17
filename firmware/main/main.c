// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jamie Saker

// chickadeeBRRD firmware, phase 3: protocol v0 over the built-in USB serial,
// with a leased session that either blinks the LED ("led") or broadcasts
// Remote ID test messages over Bluetooth 4 legacy advertising ("bt4").
//
// One JSON object per line in each direction. See docs/protocol.md.
// The session rules live in session.c and the message contents in rid.c;
// both are tested on the host. The radio is on the air only while a session
// with signal "bt4" is active, and this file is the only place that turns it
// on.

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bt4.h"
#include "cJSON.h"
#include "display.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led.h"
#include "rid.h"
#include "session.h"

#define FW_VERSION    "0.4.1"
#define PROTO_VERSION 0
#define DEVICE_NAME   "chickadeeBRRD"
#define BOARD_ID      "lilygo-t-dongle-c5"
#define MAX_LINE      1024
// The USB driver's buffers. A line is handed to the driver in one piece, and
// the driver drops a piece larger than its transmit buffer, so the buffers
// must hold the longest line the protocol allows, with room to spare.
#define USB_BUFFER_SIZE (2 * MAX_LINE)

// What a session can put out: the LED standing in for a radio, or Remote ID
// over Bluetooth 4 legacy advertising.
#define SIGNAL_LED "led"
#define SIGNAL_BT4 "bt4"

// Remote ID counts time from the start of 2019 (UTC). A host clock earlier
// than that is not a clock.
#define UNIX_2019 1546300800.0

static const char *TAG = "chickadee";

// Guards the session, the radio plan and the UI request below. Held only
// briefly; never across a USB write or a screen update. Radio commands are
// issued while holding it, so the radio can never be on without a session.
static SemaphoreHandle_t s_lock;
static session_t s_session;

// What a "bt4" session broadcasts. Set by tx.start, used by the control task.
typedef struct {
    rid_fix_t fix;
    int power_dbm;
    int64_t host_unix_ms; // the host's clock ...
    int64_t host_time_at; // ... as of this moment on ours
    uint32_t slot;
    uint8_t counters[RID_MSG_COUNT];
    bool on_air;
} radio_plan_t;
static radio_plan_t s_radio;

// What the idle screen should show. Written by the protocol task, drawn by
// the UI task, which is the only task that touches the screen and the LED.
static bool s_ui_text_set;
static char s_ui_text[DISPLAY_TEXT_MAX + 1];
static bool s_ui_dirty;

// LED self-test: the UI task steps the LED through named colours and puts the
// name on the screen, so a person can say what each one really looks like.
// Zero when not running.
#define SELFTEST_STEP_MS 2000
static int64_t s_selftest_began_ms;

typedef struct {
    const char *label;
    uint8_t r, g, b;
} led_step_t;

static const led_step_t SELFTEST_STEPS[] = {
    {"LED: red", 255, 0, 0},
    {"LED: green", 0, 255, 0},
    {"LED: blue", 0, 0, 255},
    {"LED: white", 255, 255, 255},
    {"LED: off", 0, 0, 0},
};
#define SELFTEST_STEP_COUNT ((int)(sizeof(SELFTEST_STEPS) / sizeof(SELFTEST_STEPS[0])))

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

// Send one protocol line. The object is consumed.
static void send_obj(cJSON *obj)
{
    char *text = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (text == NULL) {
        return;
    }
    size_t len = strlen(text);
    char *line = malloc(len + 1);
    if (line != NULL) {
        memcpy(line, text, len);
        line[len] = '\n';
        // A single write keeps the line intact among log output.
        if (usb_serial_jtag_write_bytes(line, len + 1, pdMS_TO_TICKS(200)) == 0) {
            ESP_LOGE(TAG, "a %u-byte line was not sent", (unsigned)(len + 1));
        }
        free(line);
    }
    cJSON_free(text);
}

static cJSON *reply_ok(int id)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "id", id);
    cJSON_AddBoolToObject(r, "ok", true);
    return r;
}

static cJSON *reply_err(int id, const char *err, const char *msg)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "id", id);
    cJSON_AddBoolToObject(r, "ok", false);
    cJSON_AddStringToObject(r, "err", err);
    if (msg != NULL) {
        cJSON_AddStringToObject(r, "msg", msg);
    }
    return r;
}

static void send_idle_event(session_stop_t reason)
{
    cJSON *ev = cJSON_CreateObject();
    cJSON_AddStringToObject(ev, "ev", "state");
    cJSON_AddStringToObject(ev, "state", "idle");
    cJSON_AddStringToObject(ev, "reason", session_stop_name(reason));
    send_obj(ev);
}

// `power_dbm` is INT_MIN for a signal that has no power setting.
static void send_transmitting_event(const char *signal, int power_dbm)
{
    cJSON *ev = cJSON_CreateObject();
    cJSON_AddStringToObject(ev, "ev", "state");
    cJSON_AddStringToObject(ev, "state", "transmitting");
    cJSON_AddStringToObject(ev, "signal", signal);
    if (power_dbm != INT_MIN) {
        cJSON_AddNumberToObject(ev, "power_dbm", power_dbm);
    }
    send_obj(ev);
}

static bool session_is_bt4(void)
{
    return s_session.active && strcmp(s_session.signal, SIGNAL_BT4) == 0;
}

// Take the radio off the air. Call with s_lock held. Safe when it is not on.
static void radio_off(void)
{
    bt4_stop();
    s_radio.on_air = false;
}

// Put the next Remote ID message on the air. Call with s_lock held, only
// while a bt4 session is active. Returns false if the radio would not do it.
static bool radio_step(int64_t now)
{
    rid_msg_t which = rid_schedule(s_radio.slot);
    int64_t unix_ms = s_radio.host_unix_ms + (now - s_radio.host_time_at);
    uint8_t msg[RID_MESSAGE_LEN];
    uint8_t adv[RID_BT4_ADV_LEN];
    if (!rid_build(which, &s_radio.fix, unix_ms, msg)) {
        return false;
    }
    rid_bt4_wrap(msg, s_radio.counters[which], adv);

    esp_err_t err = s_radio.on_air ? bt4_update(adv) : bt4_start(s_radio.power_dbm, adv);
    if (err != ESP_OK) {
        return false;
    }
    s_radio.on_air = true;
    s_radio.counters[which]++;
    s_radio.slot++;
    return true;
}

static void add_lease_fields(cJSON *r, int64_t now)
{
    cJSON_AddNumberToObject(r, "lease_ms", SESSION_LEASE_MS);
    cJSON_AddNumberToObject(r, "session_remaining_ms", (double)session_remaining_ms(&s_session, now));
}

static cJSON *hello_reply(int id)
{
    cJSON *r = reply_ok(id);
    cJSON_AddStringToObject(r, "device", DEVICE_NAME);
    cJSON_AddStringToObject(r, "board", BOARD_ID);
    cJSON_AddStringToObject(r, "fw", FW_VERSION);
    cJSON_AddNumberToObject(r, "proto", PROTO_VERSION);
    cJSON *caps = cJSON_AddArrayToObject(r, "caps");
    if (display_ready()) {
        cJSON_AddItemToArray(caps, cJSON_CreateString("display.text"));
    }
    cJSON_AddItemToArray(caps, cJSON_CreateString("tx"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("led.selftest"));
    cJSON *signals = cJSON_AddArrayToObject(r, "signals");
    cJSON_AddItemToArray(signals, cJSON_CreateString(SIGNAL_LED));
    if (bt4_ready()) {
        cJSON_AddItemToArray(signals, cJSON_CreateString(SIGNAL_BT4));
    }
    cJSON *limits = cJSON_AddObjectToObject(r, "limits");
    cJSON_AddNumberToObject(limits, "lease_ms", SESSION_LEASE_MS);
    cJSON_AddNumberToObject(limits, "session_default_ms", SESSION_DEFAULT_MS);
    cJSON_AddNumberToObject(limits, "session_max_ms", SESSION_HARD_MAX_MS);
    cJSON_AddNumberToObject(limits, "power_min_dbm", BT4_POWER_MIN_DBM);
    cJSON_AddNumberToObject(limits, "power_default_dbm", BT4_POWER_DEFAULT_DBM);
    cJSON_AddNumberToObject(limits, "power_max_dbm", BT4_POWER_MAX_DBM);
    cJSON_AddNumberToObject(limits, "power_step_db", BT4_POWER_STEP_DB);
    cJSON_AddNumberToObject(limits, "text_max", DISPLAY_TEXT_MAX);
    // Who the stick says it is on the air. Fixed in the firmware.
    cJSON *rid = cJSON_AddObjectToObject(r, "rid");
    cJSON_AddStringToObject(rid, "id_type", "specific_session_id");
    cJSON_AddStringToObject(rid, "id", RID_TEST_ID);
    cJSON_AddStringToObject(rid, "self_id", RID_SELF_ID_TEXT);
    return r;
}

// Read the host's clock from a command, as Unix milliseconds. False if the
// field is missing or is not a time Remote ID can express.
static bool host_time_ms(const cJSON *req, int64_t *out)
{
    const cJSON *jtime = cJSON_GetObjectItemCaseSensitive(req, "time");
    if (!cJSON_IsNumber(jtime) || jtime->valuedouble < UNIX_2019 || jtime->valuedouble > 4e9) {
        return false;
    }
    *out = (int64_t)(jtime->valuedouble * 1000.0);
    return true;
}

// tx.start. Call with s_lock held. Every argument is checked before the
// session begins, so a session that starts is one the stick can carry out.
static cJSON *handle_tx_start(int id, const cJSON *req, int64_t now, bool *started)
{
    const cJSON *jsignal = cJSON_GetObjectItemCaseSensitive(req, "signal");
    if (!cJSON_IsString(jsignal)) {
        return reply_err(id, "bad_args", "signal must be a string");
    }
    bool is_led = strcmp(jsignal->valuestring, SIGNAL_LED) == 0;
    bool is_bt4 = strcmp(jsignal->valuestring, SIGNAL_BT4) == 0;
    if (!is_led && !is_bt4) {
        return reply_err(id, "unsupported", "no such signal");
    }
    if (is_bt4 && !bt4_ready()) {
        return reply_err(id, "unsupported", "bluetooth did not start");
    }

    int64_t max_ms = 0; // the default
    const cJSON *jmax = cJSON_GetObjectItemCaseSensitive(req, "max_ms");
    if (jmax != NULL) {
        if (!cJSON_IsNumber(jmax) || jmax->valuedouble < 1) {
            return reply_err(id, "bad_args", "max_ms must be a positive number");
        }
        if (jmax->valuedouble > SESSION_HARD_MAX_MS) {
            return reply_err(id, "limit_exceeded", "a session is limited to 60 minutes");
        }
        max_ms = (int64_t)jmax->valuedouble;
    }

    radio_plan_t plan = {.power_dbm = BT4_POWER_DEFAULT_DBM, .host_time_at = now};
    if (is_bt4) {
        const cJSON *jlat = cJSON_GetObjectItemCaseSensitive(req, "lat");
        const cJSON *jlon = cJSON_GetObjectItemCaseSensitive(req, "lon");
        if (!cJSON_IsNumber(jlat) || !cJSON_IsNumber(jlon)) {
            return reply_err(id, "bad_args", "bt4 needs lat and lon in degrees");
        }
        plan.fix.lat_deg = jlat->valuedouble;
        plan.fix.lon_deg = jlon->valuedouble;
        if (!rid_fix_valid(&plan.fix)) {
            return reply_err(id, "bad_args", "lat and lon must be a real position");
        }
        if (!host_time_ms(req, &plan.host_unix_ms)) {
            return reply_err(id, "bad_args", "bt4 needs time, the host clock in Unix seconds");
        }
        const cJSON *jpower = cJSON_GetObjectItemCaseSensitive(req, "power_dbm");
        if (jpower != NULL) {
            if (!cJSON_IsNumber(jpower) || jpower->valuedouble != (double)jpower->valueint) {
                return reply_err(id, "bad_args", "power_dbm must be a whole number");
            }
            if (jpower->valueint > BT4_POWER_MAX_DBM) {
                return reply_err(id, "limit_exceeded", "power is limited to 0 dBm");
            }
            if (!bt4_power_allowed(jpower->valueint)) {
                return reply_err(id, "bad_args", "power_dbm must be -24 to 0 in steps of 3");
            }
            plan.power_dbm = jpower->valueint;
        }
        // Prove every message can be built before anything is sent.
        uint8_t scratch[RID_MESSAGE_LEN];
        for (int which = 0; which < RID_MSG_COUNT; which++) {
            if (!rid_build((rid_msg_t)which, &plan.fix, plan.host_unix_ms, scratch)) {
                return reply_err(id, "bad_args", "those values cannot be encoded");
            }
        }
    }

    if (!session_start(&s_session, jsignal->valuestring, now, max_ms)) {
        return reply_err(id, "busy", "a session is already running");
    }
    s_selftest_began_ms = 0;
    s_radio = plan;
    cJSON *reply = reply_ok(id);
    add_lease_fields(reply, now);
    if (is_bt4) {
        cJSON_AddNumberToObject(reply, "power_dbm", plan.power_dbm);
    }
    *started = true;
    return reply;
}

static void handle_line(const char *line, size_t len)
{
    cJSON *req = cJSON_ParseWithLength(line, len);
    if (!cJSON_IsObject(req)) {
        cJSON_Delete(req);
        send_obj(reply_err(0, "bad_json", NULL));
        return;
    }
    const cJSON *jid = cJSON_GetObjectItemCaseSensitive(req, "id");
    const cJSON *jcmd = cJSON_GetObjectItemCaseSensitive(req, "cmd");
    if (!cJSON_IsNumber(jid) || !cJSON_IsString(jcmd)) {
        cJSON_Delete(req);
        send_obj(reply_err(0, "bad_args", "need numeric id and string cmd"));
        return;
    }
    int id = jid->valueint;
    const char *cmd = jcmd->valuestring;

    cJSON *reply = NULL;
    bool started = false;      // send a "transmitting" event after the reply
    bool host_stopped = false; // send an "idle" event after the reply
    char started_signal[SESSION_SIGNAL_MAX] = "";
    int started_power = INT_MIN;

    // Every command first applies the lease and the limit, so a command can
    // never act on a session that should already have ended.
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int64_t now = now_ms();
    session_stop_t expired = session_tick(&s_session, now);
    if (expired != SESSION_STOP_NONE) {
        radio_off();
        s_ui_text_set = false;
        s_ui_dirty = true;
    }

    if (strcmp(cmd, "hello") == 0) {
        reply = hello_reply(id);
    } else if (strcmp(cmd, "status") == 0) {
        reply = reply_ok(id);
        cJSON_AddStringToObject(reply, "state", s_session.active ? "transmitting" : "idle");
        cJSON_AddNumberToObject(reply, "uptime_ms", (double)now);
        if (s_session.active) {
            cJSON_AddStringToObject(reply, "signal", s_session.signal);
            cJSON_AddNumberToObject(reply, "session_remaining_ms", (double)session_remaining_ms(&s_session, now));
            cJSON_AddNumberToObject(reply, "lease_remaining_ms",
                                    (double)session_lease_remaining_ms(&s_session, now));
            if (session_is_bt4()) {
                cJSON_AddNumberToObject(reply, "power_dbm", s_radio.power_dbm);
                cJSON_AddNumberToObject(reply, "lat", s_radio.fix.lat_deg);
                cJSON_AddNumberToObject(reply, "lon", s_radio.fix.lon_deg);
            }
        }
        if (s_session.last_stop != SESSION_STOP_NONE) {
            cJSON_AddStringToObject(reply, "last_stop", session_stop_name(s_session.last_stop));
        }
    } else if (strcmp(cmd, "display.text") == 0) {
        const cJSON *jtext = cJSON_GetObjectItemCaseSensitive(req, "text");
        if (!display_ready()) {
            reply = reply_err(id, "not_implemented", "display did not start");
        } else if (!cJSON_IsString(jtext)) {
            reply = reply_err(id, "bad_args", "text must be a string");
        } else if (strlen(jtext->valuestring) > DISPLAY_TEXT_MAX) {
            reply = reply_err(id, "too_long", "text is limited to 64 bytes");
        } else if (s_session.active) {
            reply = reply_err(id, "busy", "a session is running");
        } else {
            strcpy(s_ui_text, jtext->valuestring);
            s_ui_text_set = true;
            s_ui_dirty = true;
            reply = reply_ok(id);
        }
    } else if (strcmp(cmd, "led.selftest") == 0) {
        if (s_session.active) {
            reply = reply_err(id, "busy", "a session is running");
        } else {
            s_selftest_began_ms = now;
            reply = reply_ok(id);
            cJSON_AddNumberToObject(reply, "duration_ms", SELFTEST_STEP_MS * SELFTEST_STEP_COUNT);
        }
    } else if (strcmp(cmd, "tx.start") == 0) {
        reply = handle_tx_start(id, req, now, &started);
        if (started) {
            strcpy(started_signal, s_session.signal);
            started_power = session_is_bt4() ? s_radio.power_dbm : INT_MIN;
        }
    } else if (strcmp(cmd, "lease.renew") == 0) {
        if (!session_renew(&s_session, now)) {
            reply = reply_err(id, "not_active", NULL);
        } else {
            // A renewal may carry the host's clock, to keep ours from drifting.
            int64_t host_ms;
            if (session_is_bt4() && host_time_ms(req, &host_ms)) {
                s_radio.host_unix_ms = host_ms;
                s_radio.host_time_at = now;
            }
            reply = reply_ok(id);
            add_lease_fields(reply, now);
        }
    } else if (strcmp(cmd, "tx.stop") == 0) {
        host_stopped = session_stop(&s_session, now);
        // Off the air before the reply is written, whether or not a session
        // was running.
        radio_off();
        if (host_stopped) {
            // Back to the plain idle screen, which shows why it stopped.
            s_ui_text_set = false;
            s_ui_dirty = true;
        }
        reply = reply_ok(id);
        cJSON_AddStringToObject(reply, "state", "idle");
        cJSON_AddBoolToObject(reply, "stopped", host_stopped);
    } else {
        reply = reply_err(id, "unknown_cmd", cmd);
    }
    xSemaphoreGive(s_lock);

    // The session ended before this command was handled: say so first.
    if (expired != SESSION_STOP_NONE) {
        send_idle_event(expired);
    }
    send_obj(reply);
    if (started) {
        send_transmitting_event(started_signal, started_power);
    }
    if (host_stopped) {
        send_idle_event(SESSION_STOP_HOST);
    }
    cJSON_Delete(req);
}

static void protocol_task(void *arg)
{
    (void)arg;
    static char line[MAX_LINE];
    size_t len = 0;
    bool overflow = false;
    uint8_t chunk[64];

    for (;;) {
        int n = usb_serial_jtag_read_bytes(chunk, sizeof(chunk), pdMS_TO_TICKS(100));
        for (int i = 0; i < n; i++) {
            char c = (char)chunk[i];
            if (c == '\n') {
                if (overflow) {
                    send_obj(reply_err(0, "too_long", "line exceeds 1024 bytes"));
                } else if (len > 0) {
                    handle_line(line, len);
                }
                len = 0;
                overflow = false;
            } else if (c == '\r') {
                // ignore
            } else if (len < sizeof(line)) {
                line[len++] = c;
            } else {
                overflow = true;
            }
        }
    }
}

static const char *stop_note(session_stop_t reason)
{
    switch (reason) {
    case SESSION_STOP_HOST:
        return "last: host stop";
    case SESSION_STOP_LEASE_EXPIRED:
        return "last: lease expired";
    case SESSION_STOP_LIMIT:
        return "last: time limit";
    case SESSION_STOP_ERROR:
        return "last: radio error";
    default:
        return NULL;
    }
}

// Enforces the lease and the limit ten times a second whether or not the
// host says anything, drives the radio, and keeps the LED and the screen in
// step. During a bt4 session each pass puts the next message on the air.
static void ui_task(void *arg)
{
    (void)arg;
    bool was_active = false;
    int shown_secs = -1;
    int shown_step = -1;
    char text[DISPLAY_TEXT_MAX + 1];
    char signal[SESSION_SIGNAL_MAX];
    char note[24];
    TickType_t wake = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(100));

        xSemaphoreTake(s_lock, portMAX_DELAY);
        int64_t now = now_ms();
        session_stop_t expired = session_tick(&s_session, now);
        if (session_is_bt4()) {
            if (!radio_step(now)) {
                // The radio would not do it: end the session and say so.
                session_fail(&s_session, now);
                expired = SESSION_STOP_ERROR;
            }
        }
        if (!session_is_bt4()) {
            radio_off();
        }
        if (expired != SESSION_STOP_NONE) {
            s_ui_text_set = false;
            s_ui_dirty = true;
        }
        bool active = s_session.active;
        bool bt4 = session_is_bt4();
        int power_dbm = s_radio.power_dbm;
        int64_t remaining = session_remaining_ms(&s_session, now);
        int64_t elapsed = now - s_session.began_ms;
        session_stop_t last = s_session.last_stop;
        strcpy(signal, s_session.signal);

        // Which self-test step is due, or -1 when the test is not running.
        int step = -1;
        if (s_selftest_began_ms != 0 && !active) {
            step = (int)((now - s_selftest_began_ms) / SELFTEST_STEP_MS);
            if (step >= SELFTEST_STEP_COUNT) {
                step = -1;
                s_selftest_began_ms = 0;
                s_ui_dirty = true;
            }
        }
        bool dirty = s_ui_dirty;
        s_ui_dirty = false;
        bool text_set = s_ui_text_set;
        strcpy(text, s_ui_text);
        xSemaphoreGive(s_lock);

        if (expired != SESSION_STOP_NONE) {
            send_idle_event(expired);
        }

        // LED, re-sent every pass. During a session: half a second on, half off.
        // Orange for the LED stand-in, blue when the radio is on the air.
        if (active) {
            if ((elapsed / 500) % 2 == 0) {
                if (bt4) {
                    led_set(0, 60, 255);
                } else {
                    led_set(255, 40, 0);
                }
            } else {
                led_set(0, 0, 0);
            }
        } else if (step >= 0) {
            led_set(SELFTEST_STEPS[step].r, SELFTEST_STEPS[step].g, SELFTEST_STEPS[step].b);
        } else {
            led_set(0, 0, 0);
        }

        if (active) {
            int secs = (int)((remaining + 999) / 1000);
            if (!was_active || secs != shown_secs) {
                if (bt4) {
                    snprintf(note, sizeof(note), "%d dBm  lease held", power_dbm);
                } else {
                    snprintf(note, sizeof(note), "host lease held");
                }
                display_show_tx(signal, secs, note);
                shown_secs = secs;
            }
        } else if (step >= 0) {
            if (step != shown_step) {
                display_show_text(SELFTEST_STEPS[step].label);
            }
        } else if (was_active || dirty) {
            if (text_set) {
                display_show_text(text);
            } else {
                display_show_idle(FW_VERSION, stop_note(last));
            }
        }
        shown_step = step;
        was_active = active;
    }
}

void app_main(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.tx_buffer_size = USB_BUFFER_SIZE;
    cfg.rx_buffer_size = USB_BUFFER_SIZE;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    // Route printf and ESP_LOG through the driver so logs and protocol lines
    // are serialised by the same transmit buffer.
    usb_serial_jtag_vfs_use_driver();

    ESP_LOGI(TAG, "firmware %s, protocol %d", FW_VERSION, PROTO_VERSION);

    s_lock = xSemaphoreCreateMutex();
    session_init(&s_session);

    esp_err_t err = display_init();
    if (err == ESP_OK) {
        display_show_idle(FW_VERSION, NULL);
    } else {
        ESP_LOGE(TAG, "display did not start: %s", esp_err_to_name(err));
    }
    // After the display: on this board the LED is reached over the display bus.
    led_init();

    // The radio comes up idle. If it does not come up, the stick still works
    // and simply does not offer the bt4 signal.
    err = bt4_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bluetooth did not start: %s", esp_err_to_name(err));
    }

    cJSON *boot = cJSON_CreateObject();
    cJSON_AddStringToObject(boot, "ev", "boot");
    cJSON_AddStringToObject(boot, "fw", FW_VERSION);
    cJSON_AddNumberToObject(boot, "proto", PROTO_VERSION);
    send_obj(boot);

    xTaskCreate(protocol_task, "protocol", 6144, NULL, 5, NULL);
    xTaskCreate(ui_task, "ui", 6144, NULL, 4, NULL);
}
