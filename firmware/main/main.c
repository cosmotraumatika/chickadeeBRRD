// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jamie Saker

// chickadeeBRRD firmware, phase 2: protocol v0 over the built-in USB serial,
// with a leased session whose only output is the LED.
//
// One JSON object per line in each direction. See docs/protocol.md.
// No radio code exists in this firmware yet: "transmitting" means the LED
// blinks. The session rules live in session.c and are tested on the host.

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#include "session.h"

#define FW_VERSION    "0.3.4"
#define PROTO_VERSION 0
#define DEVICE_NAME   "chickadeeBRRD"
#define BOARD_ID      "lilygo-t-dongle-c5"
#define MAX_LINE      1024

// The only signal so far: the LED standing in for a radio.
#define SIGNAL_LED "led"

static const char *TAG = "chickadee";

// Guards the session and the UI request below. Held only briefly; never
// across a USB write or a screen update.
static SemaphoreHandle_t s_lock;
static session_t s_session;

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
        usb_serial_jtag_write_bytes(line, len + 1, pdMS_TO_TICKS(200));
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

static void send_transmitting_event(const char *signal)
{
    cJSON *ev = cJSON_CreateObject();
    cJSON_AddStringToObject(ev, "ev", "state");
    cJSON_AddStringToObject(ev, "state", "transmitting");
    cJSON_AddStringToObject(ev, "signal", signal);
    send_obj(ev);
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
    cJSON *limits = cJSON_AddObjectToObject(r, "limits");
    cJSON_AddNumberToObject(limits, "lease_ms", SESSION_LEASE_MS);
    cJSON_AddNumberToObject(limits, "session_max_ms", SESSION_MAX_MS);
    cJSON_AddNumberToObject(limits, "text_max", DISPLAY_TEXT_MAX);
    return r;
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
    bool started = false;     // send a "transmitting" event after the reply
    bool host_stopped = false; // send an "idle" event after the reply

    // Every command first applies the lease and the limit, so a command can
    // never act on a session that should already have ended.
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int64_t now = now_ms();
    session_stop_t expired = session_tick(&s_session, now);
    if (expired != SESSION_STOP_NONE) {
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
        const cJSON *jsignal = cJSON_GetObjectItemCaseSensitive(req, "signal");
        if (!cJSON_IsString(jsignal)) {
            reply = reply_err(id, "bad_args", "signal must be a string");
        } else if (strcmp(jsignal->valuestring, SIGNAL_LED) != 0) {
            reply = reply_err(id, "unsupported", "no such signal");
        } else if (!session_start(&s_session, SIGNAL_LED, now)) {
            reply = reply_err(id, "busy", "a session is already running");
        } else {
            s_selftest_began_ms = 0;
            reply = reply_ok(id);
            add_lease_fields(reply, now);
            started = true;
        }
    } else if (strcmp(cmd, "lease.renew") == 0) {
        if (!session_renew(&s_session, now)) {
            reply = reply_err(id, "not_active", NULL);
        } else {
            reply = reply_ok(id);
            add_lease_fields(reply, now);
        }
    } else if (strcmp(cmd, "tx.stop") == 0) {
        host_stopped = session_stop(&s_session, now);
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
        send_transmitting_event(SIGNAL_LED);
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
        return "last: 5 min limit";
    default:
        return NULL;
    }
}

// Enforces the lease and the limit ten times a second whether or not the
// host says anything, and keeps the LED and the screen in step.
static void ui_task(void *arg)
{
    (void)arg;
    bool was_active = false;
    int shown_secs = -1;
    int shown_step = -1;
    char text[DISPLAY_TEXT_MAX + 1];
    char signal[SESSION_SIGNAL_MAX];

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));

        xSemaphoreTake(s_lock, portMAX_DELAY);
        int64_t now = now_ms();
        session_stop_t expired = session_tick(&s_session, now);
        if (expired != SESSION_STOP_NONE) {
            s_ui_text_set = false;
            s_ui_dirty = true;
        }
        bool active = s_session.active;
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
        if (active) {
            if ((elapsed / 500) % 2 == 0) {
                led_set(255, 40, 0);
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
                display_show_tx(signal, secs);
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

    cJSON *boot = cJSON_CreateObject();
    cJSON_AddStringToObject(boot, "ev", "boot");
    cJSON_AddStringToObject(boot, "fw", FW_VERSION);
    cJSON_AddNumberToObject(boot, "proto", PROTO_VERSION);
    send_obj(boot);

    xTaskCreate(protocol_task, "protocol", 4096, NULL, 5, NULL);
    xTaskCreate(ui_task, "ui", 4096, NULL, 4, NULL);
}
