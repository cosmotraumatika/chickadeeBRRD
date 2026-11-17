#pragma once
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jamie Saker
// The stick's single APA102 RGB LED.

#include <stdint.h>

void led_init(void);                           // call after display_init; turns the LED off
void led_set(uint8_t r, uint8_t g, uint8_t b); // all zero is off; cheap enough to call repeatedly

// Send the current colour again. The display driver calls this after every
// screen update, because on this board screen traffic disturbs the LED.
void led_refresh(void);
