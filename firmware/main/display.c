// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jamie Saker

// ST7735S driver for the T-Dongle-C5 screen: a full-frame buffer pushed over
// SPI. Init sequence and geometry follow LilyGo's code for this board.

#include "display.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "font8x8.h"
#include "led.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define W 160
#define H 80

// RGB565
#define C_BLACK     0x0000
#define C_WHITE     0xFFFF
#define C_GREY      0x8410
#define C_TURQUOISE 0x471A
#define C_GOLD      0xFE47

#define CMD_SWRESET 0x01
#define CMD_SLPOUT  0x11
#define CMD_NORON   0x13
#define CMD_INVON   0x21
#define CMD_DISPON  0x29
#define CMD_CASET   0x2A
#define CMD_RASET   0x2B
#define CMD_RAMWR   0x2C
#define CMD_MADCTL  0x36
#define CMD_COLMOD  0x3A

#define MADCTL_MY  0x80
#define MADCTL_MX  0x40
#define MADCTL_MV  0x20
#define MADCTL_BGR 0x08

static const char *TAG __attribute__((unused)) = "display";

static spi_device_handle_t s_spi;
// A second device on the same bus with no chip select, for bytes the panel
// must not act on.
static spi_device_handle_t s_spi_unselected;
static bool s_ready;
// Two bytes per pixel, high byte first, as the panel wants them.
static DMA_ATTR uint8_t s_fb[W * H * 2];

static esp_err_t tx(const uint8_t *data, size_t len, int dc)
{
    if (len == 0) {
        return ESP_OK;
    }
    gpio_set_level(BOARD_TFT_DC, dc);
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };
    return spi_device_polling_transmit(s_spi, &t);
}

static esp_err_t cmd(uint8_t c, const uint8_t *args, size_t n)
{
    ESP_RETURN_ON_ERROR(tx(&c, 1, 0), TAG, "command 0x%02x", c);
    return tx(args, n, 1);
}

static void delay_ms(int ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static esp_err_t panel_init(void)
{
    gpio_set_level(BOARD_TFT_RST, 0);
    delay_ms(20);
    gpio_set_level(BOARD_TFT_RST, 1);
    delay_ms(120);

    ESP_RETURN_ON_ERROR(cmd(CMD_SWRESET, NULL, 0), TAG, "reset");
    delay_ms(150);
    ESP_RETURN_ON_ERROR(cmd(CMD_SLPOUT, NULL, 0), TAG, "sleep out");
    delay_ms(255);

    // Frame rate, inversion and power settings for the ST7735S ("R" init).
    static const uint8_t frmctr[] = {0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D};
    cmd(0xB1, frmctr, 3);
    cmd(0xB2, frmctr, 3);
    cmd(0xB3, frmctr, 6);
    cmd(0xB4, (const uint8_t[]){0x07}, 1);
    cmd(0xC0, (const uint8_t[]){0xA2, 0x02, 0x84}, 3);
    cmd(0xC1, (const uint8_t[]){0xC5}, 1);
    cmd(0xC2, (const uint8_t[]){0x0A, 0x00}, 2);
    cmd(0xC3, (const uint8_t[]){0x8A, 0x2A}, 2);
    cmd(0xC4, (const uint8_t[]){0x8A, 0xEE}, 2);
    cmd(0xC5, (const uint8_t[]){0x0E}, 1);

    // This panel needs inversion on and BGR colour order.
    cmd(CMD_INVON, NULL, 0);
    uint8_t madctl = MADCTL_MV | MADCTL_BGR | (BOARD_TFT_FLIP ? MADCTL_MX : MADCTL_MY);
    cmd(CMD_MADCTL, &madctl, 1);
    cmd(CMD_COLMOD, (const uint8_t[]){0x05}, 1); // 16 bits per pixel

    static const uint8_t gamma_pos[] = {0x02, 0x1C, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2D,
                                        0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10};
    static const uint8_t gamma_neg[] = {0x03, 0x1D, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D,
                                        0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10};
    cmd(0xE0, gamma_pos, sizeof(gamma_pos));
    cmd(0xE1, gamma_neg, sizeof(gamma_neg));

    cmd(CMD_NORON, NULL, 0);
    delay_ms(10);
    ESP_RETURN_ON_ERROR(cmd(CMD_DISPON, NULL, 0), TAG, "display on");
    delay_ms(100);
    return ESP_OK;
}

// Push the whole frame buffer to the panel.
static void flush(void)
{
    // In landscape the long axis is the column address.
    const uint16_t x0 = BOARD_TFT_OFFSET_LONG, x1 = x0 + W - 1;
    const uint16_t y0 = BOARD_TFT_OFFSET_SHORT, y1 = y0 + H - 1;
    cmd(CMD_CASET, (const uint8_t[]){x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF}, 4);
    cmd(CMD_RASET, (const uint8_t[]){y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF}, 4);
    cmd(CMD_RAMWR, s_fb, sizeof(s_fb));
    // The LED has just been clocked full of pixel data; put its colour back.
    led_refresh();
}

void display_bus_write_unselected(const uint8_t *data, size_t len)
{
    if (!s_ready || len == 0) {
        return;
    }
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };
    spi_device_polling_transmit(s_spi_unselected, &t);
}

static void put_pixel(int x, int y, uint16_t colour)
{
    if (x < 0 || x >= W || y < 0 || y >= H) {
        return;
    }
    uint8_t *p = &s_fb[(y * W + x) * 2];
    p[0] = colour >> 8;
    p[1] = colour & 0xFF;
}

static void fill_rect(int x, int y, int w, int h, uint16_t colour)
{
    for (int j = y; j < y + h; j++) {
        for (int i = x; i < x + w; i++) {
            put_pixel(i, j, colour);
        }
    }
}

static void draw_char(int x, int y, char c, int scale, uint16_t colour)
{
    unsigned char u = (unsigned char)c;
    if (u < FONT8X8_FIRST || u > FONT8X8_LAST) {
        u = '?';
    }
    const uint8_t *glyph = font8x8[u - FONT8X8_FIRST];
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            if (glyph[row] & (1 << col)) {
                fill_rect(x + col * scale, y + row * scale, scale, scale, colour);
            }
        }
    }
}

static void draw_string(int x, int y, const char *s, size_t n, int scale, uint16_t colour)
{
    for (size_t i = 0; i < n; i++) {
        draw_char(x + (int)i * 8 * scale, y, s[i], scale, colour);
    }
}

static void draw_header(void)
{
    fill_rect(0, 0, W, H, C_BLACK);
    draw_string(2, 2, "chickadeeBRRD", 13, 1, C_TURQUOISE);
    fill_rect(0, 12, W, 1, C_GOLD);
}

// Greedy word wrap of `text` into lines of at most `cols` characters, breaking
// at spaces and after hyphens. Lines are `pitch` pixels apart. Draws when
// `draw` is set. Returns the number of lines the text needs.
static int layout(const char *text, int cols, int scale, int y0, int pitch, int max_lines, bool draw)
{
    size_t len = strlen(text);
    size_t pos = 0;
    int line = 0;
    while (pos < len) {
        while (pos < len && text[pos] == ' ') {
            pos++;
        }
        if (pos >= len) {
            break;
        }
        size_t take = len - pos;
        if (take > (size_t)cols) {
            take = cols;
            // Break at the last space or hyphen in reach; a word longer than
            // the line is cut at the line's end.
            for (size_t k = cols; k > 0; k--) {
                if (text[pos + k] == ' ' || text[pos + k - 1] == '-') {
                    take = k;
                    break;
                }
            }
        }
        if (draw && line < max_lines) {
            draw_string(2, y0 + line * pitch, text + pos, take, scale, C_WHITE);
        }
        pos += take;
        line++;
    }
    return line;
}

esp_err_t display_init(void)
{
    // Backlight off (pin high) until the first frame is on the glass.
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << BOARD_TFT_BL) | (1ULL << BOARD_TFT_RST) | (1ULL << BOARD_TFT_DC),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&out), TAG, "gpio");
    gpio_set_level(BOARD_TFT_BL, 1);
    gpio_set_level(BOARD_TFT_RST, 1);

    // Keep the TF card deselected; it shares this bus.
    gpio_config_t sd = {.pin_bit_mask = 1ULL << BOARD_SD_CS, .mode = GPIO_MODE_OUTPUT};
    ESP_RETURN_ON_ERROR(gpio_config(&sd), TAG, "sd cs");
    gpio_set_level(BOARD_SD_CS, 1);

    spi_bus_config_t bus = {
        .mosi_io_num = BOARD_SPI_MOSI,
        .miso_io_num = -1, // write-only panel; MISO is only for the TF card
        .sclk_io_num = BOARD_SPI_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = sizeof(s_fb),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    spi_device_interface_config_t dev = {
        .clock_speed_hz = BOARD_TFT_SPI_HZ,
        .mode = 0,
        .spics_io_num = BOARD_TFT_CS,
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(SPI2_HOST, &dev, &s_spi), TAG, "spi device");

    spi_device_interface_config_t unselected = {
        .clock_speed_hz = BOARD_TFT_SPI_HZ,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(SPI2_HOST, &unselected, &s_spi_unselected), TAG, "spi unselected");

    ESP_RETURN_ON_ERROR(panel_init(), TAG, "panel");
    memset(s_fb, 0, sizeof(s_fb));
    s_ready = true;
    flush();
    gpio_set_level(BOARD_TFT_BL, 0); // backlight on
    return ESP_OK;
}

bool display_ready(void)
{
    return s_ready;
}

void display_show_idle(const char *fw_version, const char *note)
{
    if (!s_ready) {
        return;
    }
    draw_header();
    draw_string(2, 24, "ready", 5, 2, C_WHITE);
    if (note != NULL) {
        draw_string(2, 66, note, strlen(note), 1, C_GREY);
    } else {
        draw_string(2, 66, "fw ", 3, 1, C_GREY);
        draw_string(2 + 3 * 8, 66, fw_version, strlen(fw_version), 1, C_GREY);
    }
    flush();
}

void display_show_tx(const char *signal, int seconds_left, const char *note)
{
    if (!s_ready) {
        return;
    }
    if (seconds_left < 0) {
        seconds_left = 0;
    }
    char clock[16];
    snprintf(clock, sizeof(clock), "%d:%02d", seconds_left / 60, seconds_left % 60);

    draw_header();
    draw_string(2, 18, "ACTIVE", 6, 2, C_GOLD);
    draw_string(2 + 6 * 16 + 6, 26, signal, strlen(signal), 1, C_GREY);
    draw_string(2, 44, clock, strlen(clock), 2, C_WHITE);
    draw_string(2 + (int)strlen(clock) * 16 + 6, 52, "left", 4, 1, C_GREY);
    if (note != NULL) {
        size_t n = strlen(note);
        draw_string(2, 68, note, n > 19 ? 19 : n, 1, C_GREY);
    }
    flush();
}

void display_show_text(const char *text)
{
    if (!s_ready) {
        return;
    }
    draw_header();
    // Below the header there is room for 4 lines of 9 large characters,
    // or 6 lines of 19 small ones.
    const int y0 = 16;
    if (layout(text, 9, 2, y0, 16, 4, false) <= 4) {
        layout(text, 9, 2, y0, 16, 4, true);
    } else {
        layout(text, 19, 1, y0 + 2, 10, 6, true);
    }
    flush();
}
