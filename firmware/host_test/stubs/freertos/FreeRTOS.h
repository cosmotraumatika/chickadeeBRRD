#pragma once
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jamie Saker
// Stand-in for an ESP-IDF header, just enough for host_test/test_control.c.
#include <stdint.h>
#define pdMS_TO_TICKS(ms) (ms)
#define portMAX_DELAY 0
typedef uint32_t TickType_t;
