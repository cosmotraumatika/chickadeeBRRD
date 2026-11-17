// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jamie Saker

// Host tests for the session rules. No ESP-IDF needed; from the repo root:
//   sh firmware/host_test/run.sh

#include <stdio.h>
#include <string.h>

#include "session.h"

static int failures;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            failures++;                                                \
        }                                                              \
    } while (0)

static void idle_does_nothing(void)
{
    session_t s;
    session_init(&s);
    CHECK(session_tick(&s, 1000) == SESSION_STOP_NONE);
    CHECK(!session_renew(&s, 1000));
    CHECK(!session_stop(&s, 1000));
    CHECK(session_remaining_ms(&s, 1000) == 0);
    CHECK(s.last_stop == SESSION_STOP_NONE);
}

static void lease_expires_without_renewal(void)
{
    session_t s;
    session_init(&s);
    CHECK(session_start(&s, "led", 1000, 0));
    CHECK(s.active && strcmp(s.signal, "led") == 0);
    CHECK(session_tick(&s, 1000 + SESSION_LEASE_MS - 1) == SESSION_STOP_NONE);
    CHECK(session_tick(&s, 1000 + SESSION_LEASE_MS) == SESSION_STOP_LEASE_EXPIRED);
    CHECK(!s.active);
    CHECK(s.last_stop == SESSION_STOP_LEASE_EXPIRED);
    // The reason is reported once.
    CHECK(session_tick(&s, 1000 + SESSION_LEASE_MS + 1) == SESSION_STOP_NONE);
}

static void renewal_keeps_it_alive(void)
{
    session_t s;
    session_init(&s);
    session_start(&s, "led", 0, 0);
    for (int64_t t = 1000; t <= 20000; t += 1000) {
        CHECK(session_renew(&s, t));
        CHECK(session_tick(&s, t + SESSION_LEASE_MS - 1) == SESSION_STOP_NONE);
        CHECK(session_lease_remaining_ms(&s, t) == SESSION_LEASE_MS);
    }
    CHECK(s.active);
}

static void late_renewal_cannot_revive(void)
{
    session_t s;
    session_init(&s);
    session_start(&s, "led", 0, 0);
    // No tick ran in between; the renewal itself must notice the lapse.
    CHECK(!session_renew(&s, SESSION_LEASE_MS));
    CHECK(!s.active);
    CHECK(s.last_stop == SESSION_STOP_LEASE_EXPIRED);
}

static void limit_ends_a_renewed_session(void)
{
    session_t s;
    session_init(&s);
    session_start(&s, "led", 500, 0);
    int64_t t = 500;
    while (t + 1000 < 500 + SESSION_DEFAULT_MS) {
        t += 1000;
        CHECK(session_renew(&s, t));
    }
    CHECK(session_remaining_ms(&s, t) == 500 + SESSION_DEFAULT_MS - t);
    CHECK(session_tick(&s, 500 + SESSION_DEFAULT_MS - 1) == SESSION_STOP_NONE);
    CHECK(session_tick(&s, 500 + SESSION_DEFAULT_MS) == SESSION_STOP_LIMIT);
    CHECK(s.last_stop == SESSION_STOP_LIMIT);
    CHECK(!session_renew(&s, 500 + SESSION_DEFAULT_MS));
}

static void limit_wins_over_lease(void)
{
    session_t s;
    session_init(&s);
    session_start(&s, "led", 0, 0);
    // Both the limit and the lease have passed; the limit is the reason given.
    CHECK(session_tick(&s, SESSION_DEFAULT_MS + SESSION_LEASE_MS) == SESSION_STOP_LIMIT);
}

static void a_longer_limit_can_be_asked_for(void)
{
    session_t s;
    session_init(&s);
    const int64_t half_hour = 30 * 60 * 1000;
    CHECK(session_start(&s, "bt4", 0, half_hour));
    CHECK(session_remaining_ms(&s, 0) == half_hour);
    for (int64_t t = 1000; t < half_hour; t += 1000) {
        session_renew(&s, t);
    }
    // Still running past the default limit, stopped at the one asked for.
    CHECK(s.active);
    CHECK(session_tick(&s, half_hour - 1) == SESSION_STOP_NONE);
    CHECK(session_tick(&s, half_hour) == SESSION_STOP_LIMIT);
}

static void the_hard_limit_cannot_be_exceeded(void)
{
    session_t s;
    session_init(&s);
    // Ask for two hours; get one.
    CHECK(session_start(&s, "bt4", 0, 2 * 60 * 60 * 1000));
    CHECK(s.max_ms == SESSION_HARD_MAX_MS);
    for (int64_t t = 1000; t < SESSION_HARD_MAX_MS; t += 1000) {
        session_renew(&s, t);
    }
    CHECK(s.active);
    CHECK(session_tick(&s, SESSION_HARD_MAX_MS) == SESSION_STOP_LIMIT);
    // Zero and negative requests mean the default.
    session_start(&s, "led", SESSION_HARD_MAX_MS, 0);
    CHECK(s.max_ms == SESSION_DEFAULT_MS);
    session_stop(&s, SESSION_HARD_MAX_MS);
    session_start(&s, "led", SESSION_HARD_MAX_MS, -5);
    CHECK(s.max_ms == SESSION_DEFAULT_MS);
}

static void failure_ends_a_session(void)
{
    session_t s;
    session_init(&s);
    CHECK(!session_fail(&s, 0));
    session_start(&s, "bt4", 0, 0);
    CHECK(session_fail(&s, 100));
    CHECK(!s.active && s.last_stop == SESSION_STOP_ERROR);
    CHECK(strcmp(session_stop_name(SESSION_STOP_ERROR), "error") == 0);
}

static void start_and_stop(void)
{
    session_t s;
    session_init(&s);
    CHECK(session_start(&s, "led", 0, 0));
    CHECK(!session_start(&s, "led", 100, 0));
    CHECK(session_stop(&s, 200));
    CHECK(!s.active && s.last_stop == SESSION_STOP_HOST);
    CHECK(!session_stop(&s, 300));
    // A new session can start after a stop, and after an expiry.
    CHECK(session_start(&s, "led", 400, 0));
    CHECK(session_start(&s, "led", 400 + SESSION_LEASE_MS, 0));
    CHECK(s.began_ms == 400 + SESSION_LEASE_MS);
}

static void long_signal_name_is_truncated(void)
{
    session_t s;
    session_init(&s);
    session_start(&s, "abcdefghijklmnop", 0, 0);
    CHECK(strlen(s.signal) == SESSION_SIGNAL_MAX - 1);
}

static void stop_names(void)
{
    CHECK(strcmp(session_stop_name(SESSION_STOP_HOST), "stopped") == 0);
    CHECK(strcmp(session_stop_name(SESSION_STOP_LEASE_EXPIRED), "lease_expired") == 0);
    CHECK(strcmp(session_stop_name(SESSION_STOP_LIMIT), "limit") == 0);
}

int main(void)
{
    idle_does_nothing();
    lease_expires_without_renewal();
    renewal_keeps_it_alive();
    late_renewal_cannot_revive();
    limit_ends_a_renewed_session();
    limit_wins_over_lease();
    a_longer_limit_can_be_asked_for();
    the_hard_limit_cannot_be_exceeded();
    failure_ends_a_session();
    start_and_stop();
    long_signal_name_is_truncated();
    stop_names();
    printf(failures ? "%d FAILED\n" : "all session tests passed\n", failures);
    return failures ? 1 : 0;
}
