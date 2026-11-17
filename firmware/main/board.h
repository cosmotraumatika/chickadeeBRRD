#pragma once
// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Jamie Saker
// LilyGo T-Dongle-C5 pin map.
//
// Source: LilyGo's own code, read 2026-11-16.
//   - Xinyuan-LilyGO/T-Dongle-C5, include/pin_config.h and examples/LCD
//   - Xinyuan-LilyGO/LilyGo-display-library, src/LilyGo_T_Dongle_C5.h
// The go-spy header this replaced had every display pin wrong.
// Display pins, backlight polarity, colour order and orientation were
// confirmed on hardware 2026-11-16. The button is not yet confirmed; see the
// note on the LED below.

// Shared SPI bus (display and TF card)
#define BOARD_SPI_MOSI 2
#define BOARD_SPI_MISO 7
#define BOARD_SPI_SCLK 6

// 0.96" ST7735S TFT, 80x160, on the shared SPI bus
#define BOARD_TFT_CS   10
#define BOARD_TFT_DC   3
#define BOARD_TFT_RST  1
#define BOARD_TFT_BL   0          // backlight, ACTIVE LOW
#define BOARD_TFT_SPI_HZ (20 * 1000 * 1000)

// The 80x160 glass sits centred in the controller's 132x162 memory.
#define BOARD_TFT_OFFSET_SHORT 26 // along the 80-pixel axis
#define BOARD_TFT_OFFSET_LONG  1  // along the 160-pixel axis

// Landscape orientation. 0 matches LilyGo's LCD example; set to 1 if the
// picture is upside down for the way you hold the stick.
#define BOARD_TFT_FLIP 0

// TF card chip select (card shares the SPI bus). Unused so far.
#define BOARD_SD_CS    23

// APA102 RGB LED.
//
// LilyGo's schematic (V1.1) and code put its clock on GPIO 4 and data on
// GPIO 5. The unit this firmware was brought up on (2026-11-16) does not
// behave that way: its LED ignores those pins and instead takes its colour
// from whatever is clocked on the display's SPI lines. Every observation on
// that unit matches a simulation of an APA102 listening to the display bus.
// Whether other units do the same is unknown, so both routes are kept.
// Set to 0 to drive GPIO 4 and 5 as documented.
#define BOARD_LED_ON_DISPLAY_BUS 1
#define BOARD_LED_CLK  4
#define BOARD_LED_DATA 5

// BOOT button, active low. Unused so far.
#define BOARD_BUTTON   28
