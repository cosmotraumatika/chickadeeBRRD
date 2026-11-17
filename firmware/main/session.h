#pragma once
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jamie Saker
// The transmit session: the rule that the stick is active only while a host
// keeps renewing a lease, and never for longer than a fixed limit.
//
// Pure logic with the clock passed in, so it runs unchanged in host tests.
// Callers serialise access; nothing here is thread-safe on its own.

#include <stdbool.h>
#include <stdint.h>

// A session ends if this long passes without a renewal.
#define SESSION_LEASE_MS 3000
// A session ends after this long unless the host asked for longer at the start.
#define SESSION_DEFAULT_MS (5 * 60 * 1000)
// No session runs longer than this, whatever the host asks for.
#define SESSION_HARD_MAX_MS (60 * 60 * 1000)
#define SESSION_SIGNAL_MAX 8

typedef enum {
    SESSION_STOP_NONE = 0,
    SESSION_STOP_HOST,          // the host sent tx.stop
    SESSION_STOP_LEASE_EXPIRED, // the host went quiet
    SESSION_STOP_LIMIT,         // the session's time limit was reached
    SESSION_STOP_ERROR,         // the stick could not do what the session needs
} session_stop_t;

typedef struct {
    bool active;
    char signal[SESSION_SIGNAL_MAX];
    int64_t began_ms;
    int64_t lease_until_ms;
    int64_t max_ms; // this session's time limit
    session_stop_t last_stop;
} session_t;

void session_init(session_t *s);

// Apply the limit and the lease. Returns the reason if the session ended on
// this call, otherwise SESSION_STOP_NONE. Call it often.
session_stop_t session_tick(session_t *s, int64_t now_ms);

// Each of these applies session_tick first, so an expired session can never
// be renewed back to life.
// `max_ms` is the time limit asked for: zero or less means the default, and
// anything above SESSION_HARD_MAX_MS is cut down to it.
bool session_start(session_t *s, const char *signal, int64_t now_ms, int64_t max_ms); // false if one is running
bool session_renew(session_t *s, int64_t now_ms);                     // false if none is running
bool session_stop(session_t *s, int64_t now_ms);                      // false if none was running
bool session_fail(session_t *s, int64_t now_ms);                      // end with SESSION_STOP_ERROR

// Time until the limit, and until the lease lapses. Zero when idle.
int64_t session_remaining_ms(const session_t *s, int64_t now_ms);
int64_t session_lease_remaining_ms(const session_t *s, int64_t now_ms);

// Protocol name of a stop reason: "stopped", "lease_expired", "limit", "error".
const char *session_stop_name(session_stop_t reason);
