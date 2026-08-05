// ClipBoy Board Configuration
// Waveshare ESP32-S3 2.8 Touch (SKU 27690)
// 8MB PSRAM, 16MB Flash, 2.4GHz WiFi + BLE
//
// This file is auto-included by configs.h before any board target checks.
// All feature flags and pin definitions for ClipBoy live here.

#pragma once

#ifndef CLIPBOY_CONFIG_H
#define CLIPBOY_CONFIG_H

// ---- Board Identity ----
#define CLIPBOY_ESP32S3

// ---- Feature Flags ----
#define HAS_BT                // Bluetooth Low Energy support
#define HAS_SD                // SD card support
#define USE_SD                // Enable SD card operations
#define HAS_PSRAM             // 8MB OPI PSRAM
#define HAS_NIMBLE_2          // NimBLE 2.x API
#define HAS_IDF_3             // ESP-IDF 4.4+ APIs (esp_netif, esp_mac)

// Activates custom SPI pin path in SDInterface.cpp for our SD card pins.
// All CYD touch-screen code is gated by HAS_SCREEN (which we don't define).
#define HAS_CYD_TOUCH

// ---- SD Card Pins (SPI mode) ----
// Waveshare SDMMC slot mapped to SPI protocol:
//   Physical   -> SPI Function
//   CLK (14)   -> SCK
//   CMD (17)   -> MOSI
//   D0  (16)   -> MISO
//   D3  (21)   -> CS
#define SD_CS    21
#define SD_SCK   14
#define SD_MOSI  17
#define SD_MISO  16

// ---- Memory ----
#define MEM_LOWER_LIM 10000   // Heap low-water mark (bytes)

// ---- NOT Defined (excluded features) ----
// HAS_SCREEN       - No display in library mode
// HAS_GPS          - No GPS module
// HAS_BATTERY      - No battery monitoring IC
// HAS_BUTTONS      - No physical buttons
// HAS_NEOPIXEL_LED - No NeoPixel LED
// HAS_FLIPPER_LED  - Not a Flipper board
// HAS_DUAL_BAND    - ESP32-S3 is 2.4GHz only
// HAS_PWR_MGMT     - No power management IC
// HAS_TOUCH        - No touchscreen in library mode
// HAS_C5_SD        - Not a C5 board
// HAS_MINI_KB      - No mini keyboard
// HAS_TEMP_SENSOR  - No temperature sensor

#endif // CLIPBOY_CONFIG_H
