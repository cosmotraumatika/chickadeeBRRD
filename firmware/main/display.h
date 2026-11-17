#pragma once
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jamie Saker
// The stick's 160x80 colour screen, used in landscape.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// Longest message display_show_text accepts, in bytes.
#define DISPLAY_TEXT_MAX 64

// Bring up the SPI bus and the panel. Safe to call once at boot.
esp_err_t display_init(void);

// True once display_init has succeeded. The panel is write-only, so this
// means "the bus came up", not "a human can see it".
bool display_ready(void);

// Idle screen: product name and "ready". The bottom line shows `note` when
// given (for example why the last session ended), else the firmware version.
void display_show_idle(const char *fw_version, const char *note);

// Session screen: "ACTIVE", the signal type, and the time left as m:ss.
void display_show_tx(const char *signal, int seconds_left);

// Show a message under the product name. Bytes outside printable ASCII are
// drawn as '?'. Text longer than the screen holds is cut off.
void display_show_text(const char *text);

// Clock raw bytes onto the display's SPI lines with the display deselected,
// so the panel ignores them. Used to reach the LED, which shares those lines
// on this board. Does nothing until display_init has succeeded.
void display_bus_write_unselected(const uint8_t *data, size_t len);
