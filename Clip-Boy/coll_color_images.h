#pragma once
// Full-color (RGB565) collectible images for the FULLSCREEN view only.
// The detail-pane thumbnail and the "you found" unlock modal stay A8 (mono,
// theme-tinted) -- the burst of real color is reserved for the fullscreen
// reveal, so it lands as a surprise. Only collectibles listed here have one.
//
// Stored in PROGMEM (flash is memory-mapped on the ESP32-S3, so LVGL renders
// directly from it -- no PSRAM copy needed for a single image shown briefly).
// Regenerate an entry: py -3 scripts/png_to_rgb565.py <png> img_coll_color_<id> 240

#include <lvgl.h>

extern "C" {
    LV_IMAGE_DECLARE(img_coll_color_75);   // SheetmetalCon ticket
}

// Return a full-color image for this collectible ID, or NULL if it has none.
// NOTE: keyed by collectible ID -- keep in sync if IDs are ever renumbered.
static inline const lv_image_dsc_t* coll_get_color_image(uint8_t id) {
    switch (id) {
        case 75: return &img_coll_color_75;   // SheetmetalCon
        default: return NULL;
    }
}
