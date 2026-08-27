// =====================================================================
// DRONE WATCH — standalone UAV Remote ID detector
// Waveshare ESP32-S3-Touch-LCD-2.8 (repurposed Space Badge hardware)
//
// Passively receives ASTM F3411 / Open Drone ID broadcasts over WiFi
// (beacon + NAN) and Bluetooth LE, decodes them, and shows live drone
// contacts on the touchscreen. Receive-only; transmits nothing.
//
// Reuses the badge's display/touch/LVGL bring-up; everything else is new.
// =====================================================================
#include <Arduino.h>
#include <lvgl.h>

#include "Display_Driver.h"    // init_display(), LVGL glue (LovyanGFX)
#include "Touch_CST328.h"      // Touch_Init()
#include "rid_scan.h"
#include "rid_ui.h"

static unsigned long s_lastTick = 0;
static unsigned long s_lastUi   = 0;

void setup()
{
    Serial.begin(115200);
    delay(50);
    Serial.println("\n[DRONEWATCH] boot");

    // Bring up the BLE controller first, while internal RAM is wide open.
    rid_earlyInit();

    // PSRAM for LVGL object/style allocations.
    psramInit();

    // Display + touch (touch must init before the display registers its indev).
    Touch_Init();
    init_display();

    // Build the UI, then start the radios.
    rid_ui_init();
    lv_tick_inc(5);
    lv_timer_handler();

    rid_begin();
    Serial.println("[DRONEWATCH] scanning");
}

void loop()
{
    unsigned long now = millis();
    if (now - s_lastTick >= 5) { lv_tick_inc(now - s_lastTick); s_lastTick = now; }
    lv_timer_handler();

    rid_service();

    if (now - s_lastUi >= 250) {  // refresh UI ~4 Hz
        s_lastUi = now;
        rid_ui_tick();
        if (rid_takeNewDetection())
            Serial.println("[DRONEWATCH] NEW contact");
    }
}
