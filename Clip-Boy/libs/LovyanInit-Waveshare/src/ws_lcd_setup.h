/*******************************************************************************
 * ws_lcd_setup.h — Waveshare ESP32-S3-Touch-LCD-2.8 (SKU 27690)
 *
 * One-call init for LovyanGFX (ST7789) + CST328 touch + LVGL 9.2
 * with DMA double-buffering.
 *
 * Usage:
 *   #include "ws_lcd_setup.h"
 *   void setup() {
 *       Serial.begin(115200);
 *       lcd_init();            // display + touch + LVGL ready
 *       // ... create your LVGL UI ...
 *   }
 *   void loop() {
 *       lv_timer_handler();
 *       delay(1);
 *   }
 ******************************************************************************/
#pragma once

#include <Arduino.h>
#include <lvgl.h>

// Post-rotation logical screen size (landscape)
#define SCREEN_W  320
#define SCREEN_H  240

// Initializes LovyanGFX display, CST328 touch, and LVGL with double-buffered
// DMA rendering. Call once from setup() after Serial.begin().
// Returns true on success, false if buffer allocation failed fatally.
bool lcd_init(void);

// Backlight control (0-255)
void lcd_set_brightness(uint8_t level);
uint8_t lcd_get_brightness(void);

// Display sleep/wake (turns off backlight + LCD sleep mode)
void lcd_sleep(void);
void lcd_wakeup(void);
