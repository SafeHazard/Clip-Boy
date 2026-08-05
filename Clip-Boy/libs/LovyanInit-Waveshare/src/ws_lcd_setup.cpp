/*******************************************************************************
 * ws_lcd_setup.cpp — Waveshare ESP32-S3-Touch-LCD-2.8 hardware init
 ******************************************************************************/
#include "ws_lcd_setup.h"

#include <Wire.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// ─────────────────────────── PIN DEFINITIONS ────────────────────────────────
// LCD SPI
#define PIN_LCD_SCLK    40
#define PIN_LCD_MOSI    45
#define PIN_LCD_DC      41
#define PIN_LCD_CS      42
#define PIN_LCD_RST     39
#define PIN_LCD_BL      5

// Touch I2C (CST328)
#define PIN_TOUCH_SDA   1
#define PIN_TOUCH_SCL   3
#define PIN_TOUCH_INT   4
#define PIN_TOUCH_RST   2

// ─────────────────────────── LOVYANGFX ──────────────────────────────────────

class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ST7789   _panel;
    lgfx::Bus_SPI        _bus;
    lgfx::Light_PWM      _light;

public:
    LGFX(void) {
        // SPI Bus
        {
            auto cfg        = _bus.config();
            cfg.spi_host    = SPI2_HOST;
            cfg.spi_mode    = 0;
            cfg.freq_write  = 80000000;
            cfg.freq_read   = 16000000;
            cfg.pin_sclk    = PIN_LCD_SCLK;
            cfg.pin_mosi    = PIN_LCD_MOSI;
            cfg.pin_miso    = -1;
            cfg.pin_dc      = PIN_LCD_DC;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        // Panel
        {
            auto cfg          = _panel.config();
            cfg.pin_cs        = PIN_LCD_CS;
            cfg.pin_rst       = PIN_LCD_RST;
            cfg.pin_busy      = -1;
            cfg.memory_width  = 240;    // native (pre-rotation)
            cfg.memory_height = 320;
            cfg.panel_width   = 240;
            cfg.panel_height  = 320;
            cfg.offset_x      = 0;
            cfg.offset_y      = 0;
            cfg.offset_rotation = 0;
            cfg.readable      = false;
            cfg.invert        = true;   // ST7789 IPS
            cfg.rgb_order     = false;
            cfg.bus_shared    = false;
            _panel.config(cfg);
        }
        // Backlight
        {
            auto cfg        = _light.config();
            cfg.pin_bl      = PIN_LCD_BL;
            cfg.invert      = false;
            cfg.freq        = 12000;
            cfg.pwm_channel = 7;
            _light.config(cfg);
            _panel.setLight(&_light);
        }

        setPanel(&_panel);
    }
};

static LGFX tft;

// ─────────────────────────── CST328 TOUCH ───────────────────────────────────

#define CST328_ADDR              0x1A
#define CST328_READ_NUMBER_REG   0xD005
#define CST328_READ_XY_REG       0xD000
#define CST328_NORMAL_MODE_REG   0xD109

static volatile bool cst328_touched = false;

static void IRAM_ATTR cst328_isr(void) { cst328_touched = true; }

static bool cst328_i2c_read(uint16_t reg, uint8_t *buf, uint32_t len) {
    Wire.beginTransmission(CST328_ADDR);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)reg);
    if (Wire.endTransmission(true)) return false;
    Wire.requestFrom((uint8_t)CST328_ADDR, (uint8_t)len);
    for (uint32_t i = 0; i < len; i++) buf[i] = Wire.read();
    return true;
}

static bool cst328_i2c_write(uint16_t reg, const uint8_t *buf, uint32_t len) {
    Wire.beginTransmission(CST328_ADDR);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)reg);
    for (uint32_t i = 0; i < len; i++) Wire.write(buf[i]);
    return Wire.endTransmission(true) == 0;
}

static void cst328_init(void) {
    Wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL, 400000);
    pinMode(PIN_TOUCH_RST, OUTPUT);
    pinMode(PIN_TOUCH_INT, INPUT);

    // Reset sequence
    digitalWrite(PIN_TOUCH_RST, HIGH); delay(50);
    digitalWrite(PIN_TOUCH_RST, LOW);  delay(5);
    digitalWrite(PIN_TOUCH_RST, HIGH); delay(50);

    // Enter normal mode
    uint8_t dummy = 0;
    cst328_i2c_write(CST328_NORMAL_MODE_REG, &dummy, 0);

    attachInterrupt(PIN_TOUCH_INT, cst328_isr, RISING);
    Serial.println("[OK] CST328 touch initialized");
}

static bool cst328_read(uint16_t *x, uint16_t *y) {
    uint8_t buf[28];
    uint8_t clear = 0;

    if (!cst328_i2c_read(CST328_READ_NUMBER_REG, buf, 1)) return false;
    uint8_t cnt = buf[0] & 0x0F;
    if (cnt == 0 || cnt > 5) {
        cst328_i2c_write(CST328_READ_NUMBER_REG, &clear, 1);
        return false;
    }
    if (!cst328_i2c_read(CST328_READ_XY_REG, buf, 27)) return false;
    cst328_i2c_write(CST328_READ_NUMBER_REG, &clear, 1);

    // 12-bit x/y from first touch point
    *x = ((uint16_t)buf[1] << 4) | ((buf[3] & 0xF0) >> 4);
    *y = ((uint16_t)buf[2] << 4) | ( buf[3] & 0x0F);
    return true;
}

// ─────────────────────────── LVGL CALLBACKS ─────────────────────────────────

static void disp_flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *px_map) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.writePixelsDMA((lgfx::rgb565_t *)px_map, w * h);
    tft.endWrite();

    lv_display_flush_ready(display);
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    if (!cst328_touched) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    cst328_touched = false;

    uint16_t raw_x, raw_y;
    if (cst328_read(&raw_x, &raw_y)) {
        // CST328 reports native 240x320 portrait coords.
        // Rotation 3 (landscape flipped) mapping:
        data->point.x = 319 - raw_y;
        data->point.y = raw_x;
        data->state   = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// ─────────────────────────── PUBLIC API ─────────────────────────────────────

bool lcd_init(void) {
    // Touch (must init I2C/reset before LovyanGFX claims pins)
    cst328_init();

    // Display
    tft.begin();
    tft.setRotation(3);
    tft.setBrightness(200);
    Serial.println("[OK] LovyanGFX initialized");

    // LVGL core
    lv_init();
    lv_tick_set_cb([](void) -> uint32_t { return millis(); });
    Serial.println("[OK] LVGL initialized");

    // Double-buffered framebuffers — try PSRAM first, fall back to internal RAM
    const uint32_t full_size = SCREEN_W * SCREEN_H * sizeof(lv_color_t);
    lv_color_t *buf1 = (lv_color_t *)heap_caps_malloc(full_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    lv_color_t *buf2 = (lv_color_t *)heap_caps_malloc(full_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    lv_display_t *disp;

    if (buf1 && buf2) {
        disp = lv_display_create(SCREEN_W, SCREEN_H);
        lv_display_set_buffers(disp, buf1, buf2, full_size, LV_DISPLAY_RENDER_MODE_FULL);
        Serial.printf("[OK] Full double-buffer: 2x %u bytes in PSRAM\n", full_size);
    } else {
        if (buf1) free(buf1);
        if (buf2) free(buf2);
        Serial.println("[WARN] PSRAM alloc failed, trying internal RAM partial buffers");
        const uint32_t part_size = SCREEN_W * 40 * sizeof(lv_color_t);
        buf1 = (lv_color_t *)heap_caps_malloc(part_size, MALLOC_CAP_DMA);
        buf2 = (lv_color_t *)heap_caps_malloc(part_size, MALLOC_CAP_DMA);
        if (!buf1 || !buf2) {
            Serial.println("[FATAL] Buffer allocation failed. Halting.");
            while (1) delay(1000);
            return false;
        }
        disp = lv_display_create(SCREEN_W, SCREEN_H);
        lv_display_set_buffers(disp, buf1, buf2, part_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
        Serial.printf("[WARN] Partial buffers: 2x %u bytes in internal RAM\n", part_size);
    }

    lv_display_set_flush_cb(disp, disp_flush_cb);

    // Touch input device
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
    Serial.println("[OK] Touch input registered");

    return true;
}

void lcd_set_brightness(uint8_t level) {
    tft.setBrightness(level);
}

uint8_t lcd_get_brightness(void) {
    return tft.getBrightness();
}

void lcd_sleep(void) {
    tft.sleep();
}

void lcd_wakeup(void) {
    tft.wakeup();
}
