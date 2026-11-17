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
    CHECK(session_start(&s, "led", 1000));
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
    session_start(&s, "led", 0);
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
    session_start(&s, "led", 0);
    // No tick ran in between; the renewal itself must notice the lapse.
    CHECK(!session_renew(&s, SESSION_LEASE_MS));
    CHECK(!s.active);
    CHECK(s.last_stop == SESSION_STOP_LEASE_EXPIRED);
}

static void limit_ends_a_renewed_session(void)
{
    session_t s;
    session_init(&s);
    session_start(&s, "led", 500);
    int64_t t = 500;
    while (t + 1000 < 500 + SESSION_MAX_MS) {
        t += 1000;
        CHECK(session_renew(&s, t));
    }
    CHECK(session_remaining_ms(&s, t) == 500 + SESSION_MAX_MS - t);
    CHECK(session_tick(&s, 500 + SESSION_MAX_MS - 1) == SESSION_STOP_NONE);
    CHECK(session_tick(&s, 500 + SESSION_MAX_MS) == SESSION_STOP_LIMIT);
    CHECK(s.last_stop == SESSION_STOP_LIMIT);
    CHECK(!session_renew(&s, 500 + SESSION_MAX_MS));
}

static void limit_wins_over_lease(void)
{
    session_t s;
    session_init(&s);
    session_start(&s, "led", 0);
    // Both the limit and the lease have passed; the limit is the reason given.
    CHECK(session_tick(&s, SESSION_MAX_MS + SESSION_LEASE_MS) == SESSION_STOP_LIMIT);
}

static void start_and_stop(void)
{
    session_t s;
    session_init(&s);
    CHECK(session_start(&s, "led", 0));
    CHECK(!session_start(&s, "led", 100));
    CHECK(session_stop(&s, 200));
    CHECK(!s.active && s.last_stop == SESSION_STOP_HOST);
    CHECK(!session_stop(&s, 300));
    // A new session can start after a stop, and after an expiry.
    CHECK(session_start(&s, "led", 400));
    CHECK(session_start(&s, "led", 400 + SESSION_LEASE_MS));
    CHECK(s.began_ms == 400 + SESSION_LEASE_MS);
}

static void long_signal_name_is_truncated(void)
{
    session_t s;
    session_init(&s);
    session_start(&s, "abcdefghijklmnop", 0);
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
    start_and_stop();
    long_signal_name_is_truncated();
    stop_names();
    printf(failures ? "%d FAILED\n" : "all session tests passed\n", failures);
    return failures ? 1 : 0;
}
