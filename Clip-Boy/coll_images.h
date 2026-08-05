#pragma once
// Auto-generated collectible image declarations + PSRAM loader
// 95 images, 200x200 A8, ~3.7MB PROGMEM → copied to PSRAM at boot
// Regenerate: py -3 scripts/batch_postprocess.py

#include <lvgl.h>
#include <esp_heap_caps.h>
#include <pgmspace.h>

extern "C" {
    LV_IMAGE_DECLARE(img_coll_1);
    LV_IMAGE_DECLARE(img_coll_2);
    LV_IMAGE_DECLARE(img_coll_3);
    LV_IMAGE_DECLARE(img_coll_4);
    LV_IMAGE_DECLARE(img_coll_5);
    LV_IMAGE_DECLARE(img_coll_6);
    LV_IMAGE_DECLARE(img_coll_7);
    LV_IMAGE_DECLARE(img_coll_8);
    LV_IMAGE_DECLARE(img_coll_9);
    LV_IMAGE_DECLARE(img_coll_10);
    LV_IMAGE_DECLARE(img_coll_12);
    LV_IMAGE_DECLARE(img_coll_13);
    LV_IMAGE_DECLARE(img_coll_14);
    LV_IMAGE_DECLARE(img_coll_15);
    LV_IMAGE_DECLARE(img_coll_16);
    LV_IMAGE_DECLARE(img_coll_17);
    LV_IMAGE_DECLARE(img_coll_18);
    LV_IMAGE_DECLARE(img_coll_19);
    LV_IMAGE_DECLARE(img_coll_20);
    LV_IMAGE_DECLARE(img_coll_21);
    LV_IMAGE_DECLARE(img_coll_22);
    LV_IMAGE_DECLARE(img_coll_23);
    LV_IMAGE_DECLARE(img_coll_124);
    LV_IMAGE_DECLARE(img_coll_127);
    LV_IMAGE_DECLARE(img_coll_26);
    LV_IMAGE_DECLARE(img_coll_27);
    LV_IMAGE_DECLARE(img_coll_28);
    LV_IMAGE_DECLARE(img_coll_29);
    LV_IMAGE_DECLARE(img_coll_30);
    LV_IMAGE_DECLARE(img_coll_31);
    LV_IMAGE_DECLARE(img_coll_32);
    LV_IMAGE_DECLARE(img_coll_33);
    LV_IMAGE_DECLARE(img_coll_34);
    LV_IMAGE_DECLARE(img_coll_35);
    LV_IMAGE_DECLARE(img_coll_36);
    LV_IMAGE_DECLARE(img_coll_37);
    LV_IMAGE_DECLARE(img_coll_38);
    LV_IMAGE_DECLARE(img_coll_39);
    LV_IMAGE_DECLARE(img_coll_108);
    LV_IMAGE_DECLARE(img_coll_112);
    LV_IMAGE_DECLARE(img_coll_42);
    LV_IMAGE_DECLARE(img_coll_118);
    LV_IMAGE_DECLARE(img_coll_44);
    LV_IMAGE_DECLARE(img_coll_45);
    LV_IMAGE_DECLARE(img_coll_46);
    LV_IMAGE_DECLARE(img_coll_47);
    LV_IMAGE_DECLARE(img_coll_48);
    LV_IMAGE_DECLARE(img_coll_49);
    LV_IMAGE_DECLARE(img_coll_50);
    LV_IMAGE_DECLARE(img_coll_51);
    LV_IMAGE_DECLARE(img_coll_52);
    LV_IMAGE_DECLARE(img_coll_53);
    LV_IMAGE_DECLARE(img_coll_54);
    LV_IMAGE_DECLARE(img_coll_55);
    LV_IMAGE_DECLARE(img_coll_56);
    LV_IMAGE_DECLARE(img_coll_57);
    LV_IMAGE_DECLARE(img_coll_58);
    LV_IMAGE_DECLARE(img_coll_60);
    LV_IMAGE_DECLARE(img_coll_61);
    LV_IMAGE_DECLARE(img_coll_62);
    LV_IMAGE_DECLARE(img_coll_63);
    LV_IMAGE_DECLARE(img_coll_64);
    LV_IMAGE_DECLARE(img_coll_65);
    LV_IMAGE_DECLARE(img_coll_66);
    LV_IMAGE_DECLARE(img_coll_67);
    LV_IMAGE_DECLARE(img_coll_68);
    LV_IMAGE_DECLARE(img_coll_69);
    LV_IMAGE_DECLARE(img_coll_70);
    LV_IMAGE_DECLARE(img_coll_71);
    LV_IMAGE_DECLARE(img_coll_72);
    LV_IMAGE_DECLARE(img_coll_73);
    LV_IMAGE_DECLARE(img_coll_74);
    LV_IMAGE_DECLARE(img_coll_75);
    LV_IMAGE_DECLARE(img_coll_76);
    LV_IMAGE_DECLARE(img_coll_77);
    LV_IMAGE_DECLARE(img_coll_78);
    LV_IMAGE_DECLARE(img_coll_79);
    LV_IMAGE_DECLARE(img_coll_80);
    LV_IMAGE_DECLARE(img_coll_81);
    LV_IMAGE_DECLARE(img_coll_83);
    LV_IMAGE_DECLARE(img_coll_84);
    LV_IMAGE_DECLARE(img_coll_85);
    LV_IMAGE_DECLARE(img_coll_86);
    LV_IMAGE_DECLARE(img_coll_87);
    LV_IMAGE_DECLARE(img_coll_88);
    LV_IMAGE_DECLARE(img_coll_89);
    LV_IMAGE_DECLARE(img_coll_90);
    LV_IMAGE_DECLARE(img_coll_92);
    LV_IMAGE_DECLARE(img_coll_93);
    LV_IMAGE_DECLARE(img_coll_95);
    LV_IMAGE_DECLARE(img_coll_96);
    LV_IMAGE_DECLARE(img_coll_97);
    LV_IMAGE_DECLARE(img_coll_98);
    LV_IMAGE_DECLARE(img_coll_99);
    LV_IMAGE_DECLARE(img_coll_100);
}

// ─── PROGMEM source table ─────────────────────────────────────────────────
// Maps collectible ID → PROGMEM image descriptor.
// Used at boot to bulk-copy pixel data into PSRAM.

#define COLL_BUILTIN_COUNT  95
#define COLL_IMG_DIM        200    // square A8 edge length
#define COLL_IMG_PIXELS     (COLL_IMG_DIM * COLL_IMG_DIM)   // 200 x 200 A8 = 40000

struct CollBuiltinEntry {
    uint8_t                  id;
    const lv_image_dsc_t    *progmem;  // PROGMEM source descriptor
};

static const CollBuiltinEntry coll_builtin_table[COLL_BUILTIN_COUNT] = {
    {  1, &img_coll_1  }, {  2, &img_coll_2  }, {  3, &img_coll_3  },
    {  4, &img_coll_4  }, {  5, &img_coll_5  }, {  6, &img_coll_6  },
    {  7, &img_coll_7  }, {  8, &img_coll_8  }, {  9, &img_coll_9  },
    { 10, &img_coll_10 }, { 12, &img_coll_12 }, { 13, &img_coll_13 },
    { 14, &img_coll_14 }, { 15, &img_coll_15 }, { 16, &img_coll_16 },
    { 17, &img_coll_17 }, { 18, &img_coll_18 }, { 19, &img_coll_19 },
    { 20, &img_coll_20 }, { 21, &img_coll_21 }, { 22, &img_coll_22 },
    { 23, &img_coll_23 }, { 124, &img_coll_124 }, { 127, &img_coll_127 },
    { 26, &img_coll_26 }, { 27, &img_coll_27 }, { 28, &img_coll_28 },
    { 29, &img_coll_29 }, { 30, &img_coll_30 }, { 31, &img_coll_31 },
    { 32, &img_coll_32 }, { 33, &img_coll_33 }, { 34, &img_coll_34 },
    { 35, &img_coll_35 }, { 36, &img_coll_36 }, { 37, &img_coll_37 },
    { 38, &img_coll_38 }, { 39, &img_coll_39 }, { 108, &img_coll_108 },
    { 112, &img_coll_112 }, { 42, &img_coll_42 }, { 118, &img_coll_118 },
    { 44, &img_coll_44 }, { 45, &img_coll_45 }, { 46, &img_coll_46 },
    { 47, &img_coll_47 }, { 48, &img_coll_48 }, { 49, &img_coll_49 },
    { 50, &img_coll_50 }, { 51, &img_coll_51 }, { 52, &img_coll_52 },
    { 53, &img_coll_53 }, { 54, &img_coll_54 }, { 55, &img_coll_55 },
    { 56, &img_coll_56 }, { 57, &img_coll_57 }, { 58, &img_coll_58 },
    { 60, &img_coll_60 }, { 61, &img_coll_61 }, { 62, &img_coll_62 },
    { 63, &img_coll_63 }, { 64, &img_coll_64 }, { 65, &img_coll_65 },
    { 66, &img_coll_66 }, { 67, &img_coll_67 }, { 68, &img_coll_68 },
    { 69, &img_coll_69 }, { 70, &img_coll_70 }, { 71, &img_coll_71 },
    { 72, &img_coll_72 }, { 73, &img_coll_73 }, { 74, &img_coll_74 },
    { 75, &img_coll_75 }, { 76, &img_coll_76 }, { 77, &img_coll_77 },
    { 78, &img_coll_78 }, { 79, &img_coll_79 }, { 80, &img_coll_80 },
    { 81, &img_coll_81 }, { 83, &img_coll_83 }, { 84, &img_coll_84 },
    { 85, &img_coll_85 }, { 86, &img_coll_86 }, { 87, &img_coll_87 },
    { 88, &img_coll_88 }, { 89, &img_coll_89 }, { 90, &img_coll_90 },
    { 92, &img_coll_92 }, { 93, &img_coll_93 }, { 95, &img_coll_95 },
    { 96, &img_coll_96 }, { 97, &img_coll_97 }, { 98, &img_coll_98 },
    { 99, &img_coll_99 }, {100, &img_coll_100},
};

// ─── PSRAM image cache ────────────────────────────────────────────────────
// One contiguous PSRAM allocation holds all 95 images' pixel data.
// Descriptors are in a flat array; lookup uses id→index map (max ID 100).

static uint8_t         *coll_psram_pixels = NULL;     // contiguous pixel block
static lv_image_dsc_t  *coll_psram_dsc    = NULL;     // descriptor array
static int8_t           coll_psram_map[128];           // id → index (-1 = none); 0..127 SECDED ID space
static bool             coll_psram_ready  = false;

// Copy all PROGMEM image data to PSRAM. Call once at boot.
// Total PSRAM: 95 * 40000 = 3,800,000 bytes pixels + 95 * sizeof(lv_image_dsc_t) descriptors
static bool coll_images_init_psram(void) {
    if (coll_psram_ready) return true;

    // Init lookup map
    memset(coll_psram_map, -1, sizeof(coll_psram_map));

    // Allocate pixel block
    size_t total_pixels = (size_t)COLL_BUILTIN_COUNT * COLL_IMG_PIXELS;
    coll_psram_pixels = (uint8_t *)heap_caps_malloc(total_pixels, MALLOC_CAP_SPIRAM);
    if (!coll_psram_pixels) {
        Serial.printf("[IMG] PSRAM alloc failed for pixels (%u bytes)\n", (unsigned)total_pixels);
        return false;
    }

    // Allocate descriptor array
    coll_psram_dsc = (lv_image_dsc_t *)heap_caps_calloc(
        COLL_BUILTIN_COUNT, sizeof(lv_image_dsc_t), MALLOC_CAP_SPIRAM);
    if (!coll_psram_dsc) {
        Serial.println("[IMG] PSRAM alloc failed for descriptors");
        heap_caps_free(coll_psram_pixels);
        coll_psram_pixels = NULL;
        return false;
    }

    // Copy each image's pixel data from PROGMEM to PSRAM
    for (int i = 0; i < COLL_BUILTIN_COUNT; i++) {
        const CollBuiltinEntry &e = coll_builtin_table[i];
        uint8_t *dst = coll_psram_pixels + (size_t)i * COLL_IMG_PIXELS;

        // Copy pixel data from flash
        memcpy_P(dst, e.progmem->data, COLL_IMG_PIXELS);

        // Build PSRAM descriptor pointing to PSRAM copy
        coll_psram_dsc[i].header.magic  = LV_IMAGE_HEADER_MAGIC;
        coll_psram_dsc[i].header.cf     = LV_COLOR_FORMAT_A8;
        coll_psram_dsc[i].header.flags  = 0;
        coll_psram_dsc[i].header.w      = COLL_IMG_DIM;
        coll_psram_dsc[i].header.h      = COLL_IMG_DIM;
        coll_psram_dsc[i].header.stride = COLL_IMG_DIM;
        coll_psram_dsc[i].data_size     = COLL_IMG_PIXELS;
        coll_psram_dsc[i].data          = dst;

        // Register in lookup map
        if (e.id <= 127) coll_psram_map[e.id] = (int8_t)i;
    }

    coll_psram_ready = true;
    Serial.printf("[IMG] %d images copied to PSRAM (%u KB)\n",
                  COLL_BUILTIN_COUNT, (unsigned)(total_pixels / 1024));
    return true;
}

// Lookup: collectible ID -> PSRAM image (falls back to PROGMEM if init failed)
static const lv_image_dsc_t* coll_get_builtin_image(uint8_t id) {
    // Try PSRAM first
    if (coll_psram_ready && id <= 127 && coll_psram_map[id] >= 0) {
        return &coll_psram_dsc[coll_psram_map[id]];
    }
    // Fallback: scan PROGMEM table
    for (int i = 0; i < COLL_BUILTIN_COUNT; i++) {
        if (coll_builtin_table[i].id == id)
            return coll_builtin_table[i].progmem;
    }
    return NULL;
}
