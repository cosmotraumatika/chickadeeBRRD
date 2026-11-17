#pragma once
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jamie Saker
//
// The Bluetooth radio, used only for non-connectable legacy advertising.
// It is brought up idle at boot and transmits nothing until bt4_start.

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// Transmit power, in dBm. The radio offers 3 dB steps. The default is the
// lowest it can do; BT4_POWER_MAX_DBM is this firmware's own ceiling, well
// under what the chip is capable of.
#define BT4_POWER_MIN_DBM (-24)
#define BT4_POWER_DEFAULT_DBM (-24)
#define BT4_POWER_MAX_DBM 0
#define BT4_POWER_STEP_DB 3

#define BT4_ADV_LEN 31

// Bring the Bluetooth stack up, idle. Safe to call once at boot.
esp_err_t bt4_init(void);

// True once the stack is up and has an address to advertise from.
bool bt4_ready(void);

// True for a power this firmware will transmit at: a 3 dB step from
// BT4_POWER_MIN_DBM up to BT4_POWER_MAX_DBM.
bool bt4_power_allowed(int dbm);

// Start advertising `adv` at `power_dbm`. Refuses a power that is not
// allowed, and refuses to transmit if the radio does not confirm the power.
esp_err_t bt4_start(int power_dbm, const uint8_t adv[BT4_ADV_LEN]);

// Replace the advertised bytes while advertising.
esp_err_t bt4_update(const uint8_t adv[BT4_ADV_LEN]);

// Stop advertising. Safe to call at any time, from any task.
void bt4_stop(void);
