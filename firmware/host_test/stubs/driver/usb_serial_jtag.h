#pragma once
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jamie Saker
// Stand-in for an ESP-IDF header, just enough for host_test/test_control.c.
#include <stddef.h>
#include <stdint.h>
typedef struct {
    uint32_t tx_buffer_size;
    uint32_t rx_buffer_size;
} usb_serial_jtag_driver_config_t;
#define USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT() \
    (usb_serial_jtag_driver_config_t) { 0 }
int usb_serial_jtag_driver_install(usb_serial_jtag_driver_config_t *cfg);
int usb_serial_jtag_read_bytes(void *buf, uint32_t length, int ticks);
int usb_serial_jtag_write_bytes(const void *src, size_t size, int ticks);
