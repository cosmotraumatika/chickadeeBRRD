#pragma once
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jamie Saker
// Stand-in for an ESP-IDF header, just enough for host_test/test_control.c.
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERROR_CHECK(x) (void)(x)
const char *esp_err_to_name(esp_err_t code);
