// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jamie Saker

// APA102 driver for one LED.
//
// Two ways to reach the LED, chosen in board.h:
//
// BOARD_LED_ON_DISPLAY_BUS = 1: the LED frame is sent on the display's SPI
// clock and data lines with the display deselected. This is what the unit
// this firmware was brought up on needs: its LED follows the display bus and
// ignores GPIO 4 and 5 (see board.h).
//
// BOARD_LED_ON_DISPLAY_BUS = 0: clock and data are bit-banged on GPIO 4 and
// 5, as LilyGo's schematic and code describe. Untested here, because the
// bring-up unit does not answer to it.

#include "led.h"

#include "board.h"
#include "display.h"

// APA102 global brightness, 0..31. These LEDs are very bright; keep it low.
#define LED_BRIGHTNESS 5

static uint8_t s_r, s_g, s_b;

#if BOARD_LED_ON_DISPLAY_BUS

static void send(void)
{
    // 32 zero bits mark the start; the LED frame is brightness, blue, green, red.
    const uint8_t frame[8] = {0, 0, 0, 0, 0xE0 | LED_BRIGHTNESS, s_b, s_g, s_r};
    display_bus_write_unselected(frame, sizeof(frame));
}

void led_init(void)
{
    led_set(0, 0, 0);
}

#else

#include "driver/gpio.h"
#include "esp_rom_sys.h"

static void write_byte(uint8_t b)
{
    for (int bit = 7; bit >= 0; bit--) {
        gpio_set_level(BOARD_LED_DATA, (b >> bit) & 1);
        esp_rom_delay_us(1);
        gpio_set_level(BOARD_LED_CLK, 1);
        esp_rom_delay_us(1);
        gpio_set_level(BOARD_LED_CLK, 0);
    }
}

static void send(void)
{
    for (int i = 0; i < 4; i++) {
        write_byte(0x00);
    }
    write_byte(0xE0 | LED_BRIGHTNESS);
    write_byte(s_b);
    write_byte(s_g);
    write_byte(s_r);
    gpio_set_level(BOARD_LED_DATA, 0);
}

void led_init(void)
{
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << BOARD_LED_CLK) | (1ULL << BOARD_LED_DATA),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    gpio_set_level(BOARD_LED_CLK, 0);
    led_set(0, 0, 0);
}

#endif

void led_set(uint8_t r, uint8_t g, uint8_t b)
{
    s_r = r;
    s_g = g;
    s_b = b;
    send();
}

void led_refresh(void)
{
    send();
}
