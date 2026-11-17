#pragma once
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jamie Saker
// Stand-in for an ESP-IDF header, just enough for host_test/test_control.c.
#include "esp_err.h"
#define ESP_LOGI(tag, ...) (void)(tag)
#define ESP_LOGW(tag, ...) (void)(tag)
#define ESP_LOGE(tag, ...) (void)(tag)
