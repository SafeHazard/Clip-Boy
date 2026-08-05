#pragma once
// PWR_Key.h — Power button handler for Clip-Boy badge
// Handles long-press restart/shutdown via the GPIO latch circuit.
//
// There is NO software-sleep path here on purpose: the old PWR_Sleep ->
// lcd_sleep() (= tft.sleep()) approach put the PANEL + TOUCH controller to
// sleep, so the badge could not be touch-woken and button-wake was flaky.
// "Screen dark while charging" is handled instead by Dark Charge (the
// screensaver dark state in ui_nav.h: backlight 0, touch alive, tap-and-hold
// to wake). See memory [[dark-charge]].
//
// Requires: ws_lcd_setup.h for lcd_set_brightness.

#include "Arduino.h"

#define PWR_KEY_Input_PIN   6
#define PWR_Control_PIN     7

// Long-press thresholds in REAL milliseconds (millis()-based).
#define PWR_RESTART_MS          1500  // release after ~1.5-3s -> restart
#define PWR_SHUTDOWN_MS         3000  // hold >=3s             -> hard power off (latch)

void PWR_Init(void);
void PWR_Loop(void);   // Call from main loop (non-blocking, checks button state)
void PWR_Shutdown(void);
