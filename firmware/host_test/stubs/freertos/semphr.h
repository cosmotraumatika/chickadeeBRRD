#pragma once
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jamie Saker
// Stand-in for an ESP-IDF header, just enough for host_test/test_control.c.
// The "lock" counts how deep it is held, so the test can check that the radio
// is only ever commanded with it held.
typedef int SemaphoreHandle_t;
extern int g_lock_depth;
#define xSemaphoreCreateMutex() 1
#define xSemaphoreTake(lock, ticks) (g_lock_depth++)
#define xSemaphoreGive(lock) (g_lock_depth--)
