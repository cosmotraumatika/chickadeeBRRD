// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jamie Saker

#include "session.h"

#include <string.h>

void session_init(session_t *s)
{
    memset(s, 0, sizeof(*s));
}

static void end(session_t *s, session_stop_t reason)
{
    s->active = false;
    s->last_stop = reason;
}

session_stop_t session_tick(session_t *s, int64_t now_ms)
{
    if (!s->active) {
        return SESSION_STOP_NONE;
    }
    if (now_ms - s->began_ms >= SESSION_MAX_MS) {
        end(s, SESSION_STOP_LIMIT);
        return SESSION_STOP_LIMIT;
    }
    if (now_ms >= s->lease_until_ms) {
        end(s, SESSION_STOP_LEASE_EXPIRED);
        return SESSION_STOP_LEASE_EXPIRED;
    }
    return SESSION_STOP_NONE;
}

bool session_start(session_t *s, const char *signal, int64_t now_ms)
{
    session_tick(s, now_ms);
    if (s->active) {
        return false;
    }
    s->active = true;
    strncpy(s->signal, signal, sizeof(s->signal) - 1);
    s->signal[sizeof(s->signal) - 1] = '\0';
    s->began_ms = now_ms;
    s->lease_until_ms = now_ms + SESSION_LEASE_MS;
    return true;
}

bool session_renew(session_t *s, int64_t now_ms)
{
    session_tick(s, now_ms);
    if (!s->active) {
        return false;
    }
    s->lease_until_ms = now_ms + SESSION_LEASE_MS;
    return true;
}

bool session_stop(session_t *s, int64_t now_ms)
{
    session_tick(s, now_ms);
    if (!s->active) {
        return false;
    }
    end(s, SESSION_STOP_HOST);
    return true;
}

int64_t session_remaining_ms(const session_t *s, int64_t now_ms)
{
    if (!s->active) {
        return 0;
    }
    int64_t left = SESSION_MAX_MS - (now_ms - s->began_ms);
    return left > 0 ? left : 0;
}

int64_t session_lease_remaining_ms(const session_t *s, int64_t now_ms)
{
    if (!s->active) {
        return 0;
    }
    int64_t left = s->lease_until_ms - now_ms;
    return left > 0 ? left : 0;
}

const char *session_stop_name(session_stop_t reason)
{
    switch (reason) {
    case SESSION_STOP_HOST:
        return "stopped";
    case SESSION_STOP_LEASE_EXPIRED:
        return "lease_expired";
    case SESSION_STOP_LIMIT:
        return "limit";
    default:
        return "none";
    }
}
