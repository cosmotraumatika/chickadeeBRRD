#pragma once
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jamie Saker
// Stand-in for an ESP-IDF header, just enough for host_test/test_control.c.
void vTaskDelayUntil(TickType_t *wake, int ticks);
TickType_t xTaskGetTickCount(void);
int xTaskCreate(void (*fn)(void *), const char *name, int stack, void *arg, int prio, void *handle);
