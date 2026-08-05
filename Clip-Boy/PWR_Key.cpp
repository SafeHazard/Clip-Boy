#include "PWR_Key.h"
#include <ws_lcd_setup.h>   // provides lcd_set_brightness

static uint8_t  pwr_state     = 0;   // 0=init, 1=debounce, 2=running
static bool     pressing      = false;   // a press is in progress (state 2)
static uint32_t press_start   = 0;       // millis() when the press began

void PWR_Shutdown(void) {
    Serial.println("[PWR] Shutdown");
    lcd_set_brightness(0);
    digitalWrite(PWR_Control_PIN, LOW);
}

void PWR_Init(void) {
    pinMode(PWR_KEY_Input_PIN, INPUT);
    pinMode(PWR_Control_PIN, OUTPUT);
    digitalWrite(PWR_Control_PIN, LOW);
    vTaskDelay(pdMS_TO_TICKS(100));

    if (!digitalRead(PWR_KEY_Input_PIN)) {
        // Button held during power-on — latch power
        pwr_state = 1;
        digitalWrite(PWR_Control_PIN, HIGH);
        Serial.println("[PWR] Power latched");
    }
}

void PWR_Loop(void) {
    if (!pwr_state) return;

    if (!digitalRead(PWR_KEY_Input_PIN)) {
        // Button pressed (active low)
        if (pwr_state == 2) {
            if (!pressing) { pressing = true; press_start = millis(); }
            // Hard power-off fires WHILE held (no release needed).
            if (millis() - press_start >= PWR_SHUTDOWN_MS) {
                PWR_Shutdown();
            }
        }
    } else {
        // Button released
        if (pwr_state == 1) {
            pwr_state = 2;  // Debounce complete after first release (boot latch)
        } else if (pressing) {
            // Released before the shutdown threshold: a medium hold restarts.
            // (No sleep tier -- "dark while charging" is Dark Charge, not sleep.)
            uint32_t held = millis() - press_start;
            if (held >= PWR_RESTART_MS) {
                Serial.println("[PWR] Restart");
                ESP.restart();
            }
        }
        pressing = false;
    }
}
