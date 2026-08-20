#pragma once
// Dedicated drone-detector UI (LVGL). Sensor theme: amber/green on black.
void rid_ui_init(void);   // build + load the main screen
void rid_ui_tick(void);   // refresh from the engine (call ~4 Hz)
