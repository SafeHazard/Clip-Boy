#pragma once
// clipboy_pins.h — Central pin definitions for all Clip-Boy hardware
//
// Waveshare ESP32-S3-Touch-LCD-2.8 connected to custom PCB with:
//   - 8x WS2812B NeoPixels (4 fuse + 4 front)
//   - VL53L5CX 8x8 ranging sensor (I2C)
//   - I2S stereo speakers
//   - SD card slot

// ─── I2S Audio ─────────────────────────────────────────────────────────────
#define CB_I2S_BCLK        48
#define CB_I2S_LRCK        38
#define CB_I2S_DOUT        47

// ─── NeoPixels ─────────────────────────────────────────────────────────────
#define CB_NEOPIXEL_PIN    44   // D0
#define CB_NEOPIXEL_COUNT   8   // 4 fuse (0-3) + 4 front (4-7)

// ─── VL53L5CX (I2C) ───────────────────────────────────────────────────────
#define CB_VL53_SDA        11
#define CB_VL53_SCL        10
#define CB_VL53_I2C_HZ     400000

// ─── SD Card (SPI) ─────────────────────────────────────────────────────────
#define CB_SD_CS           21
#define CB_SD_SCK          14
#define CB_SD_MOSI         17
#define CB_SD_MISO         16
