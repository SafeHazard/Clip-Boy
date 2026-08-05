#pragma once
// ui_theme.h - Clip-Boy theme system for LVGL 9.2
// Header-only: all functions and variables are static.
//
// Supports switchable palettes:
//   Mojave     - amber-on-black (desert palette)
//   Ribbit City - green-on-black (Fallout 3)
//   Flashbang  - black-on-white (bring sunglasses)
//
// Call ui_theme_init() once in setup() after lcd_init().
// Call ui_theme_apply(idx) to switch themes at runtime.

#include <lvgl.h>

// ---------------------------------------------------------------------------
// External font declarations (defined in ui_font_pipboy_*.c)
// ---------------------------------------------------------------------------
LV_FONT_DECLARE(ui_font_pipboy_14);  // 14px - status bar, small labels, tabs
LV_FONT_DECLARE(ui_font_pipboy_16);  // 16px - list items, descriptions, body
LV_FONT_DECLARE(ui_font_pipboy_18);  // 18px - section headers, titles
LV_FONT_DECLARE(ui_font_pipboy_20);  // 20px - large labels, gauges

// ---------------------------------------------------------------------------
// Theme palette
// ---------------------------------------------------------------------------
struct ThemePalette {
    const char *name;
    uint32_t bg;        // background
    uint32_t primary;   // main text, borders
    uint32_t highlight; // selected/active
    uint32_t dim;       // inactive, secondary text
    uint32_t accent;    // warnings, running indicators
    uint32_t bg_dark;   // subtle contrast background
    uint32_t border;    // subtle borders
    uint32_t disabled;  // grayed-out items (fake SAOs)
    bool     dark;      // for LVGL default theme
};

// 3 base Pip-Boy themes are always available; the last 2 (Overseer, Space Badge)
// are compiled into every build but only listed in the Settings>Theme dropdown
// for the --rift variant (see the #ifdef BADGE_QUANTUM_RIFT gate where the
// dropdown is built). NUM_THEMES covers all of them so ui_theme_apply()'s clamp
// and NVS-persisted indices stay valid in both builds.
#define NUM_THEMES 6
#define THEME_OVERSEER     3   // --rift colorway indices (see theme_presets[])
#define THEME_SPACE_BADGE  4
#define THEME_QUANTA       5   // reward theme (unlocked by completing on-badge challenges)
// The completionist reward: a CUSTOM single-hue theme the player designs (pick a
// hue, accents auto-derive). Not in theme_presets[] -- its palette is generated
// from g_custom_hue at apply time. Index sits just past the static presets.
#define THEME_CUSTOM       NUM_THEMES   // 6
static uint16_t g_custom_hue = 190;     // active custom hue 0-359 (cyan default); set from cfg + hue picker

// The Overseer + Space Badge colorways are KS-backer-exclusive content. Their real
// palettes (and the matching LED chase presets in ui_nav.h) live in the local-only,
// gitignored rift_private.h and are pulled in ONLY for a --rift build on a machine
// that has that file (our KS build). Gating on the VARIANT (not merely the file's
// presence) keeps our own PUBLIC non-rift release binaries free of the exclusive
// palettes too. Public source/binaries + any build without the file use the generic
// placeholder colorways below. (ui_theme.h is included before ui_nav.h, so the
// RIFT_LED_* macros are defined by the time the LED presets reference them.)
#if defined(BADGE_QUANTUM_RIFT) && __has_include("rift_private.h")
#include "rift_private.h"
#endif
#ifndef RIFT_THEME_OVERSEER
#define RIFT_THEME_OVERSEER \
    "Rift Blue", 0x05080F, 0x4A90D8, 0x7AB4E8, 0x2A4870, 0x4A90D8, 0x03060C, 0x244468, 0x40506A, true
#define RIFT_THEME_SPACE_BADGE \
    "Rift Red", 0x0C0000, 0xD85448, 0xE87868, 0x603434, 0xD85448, 0x0C0606, 0x603434, 0x50403A, true
#define RIFT_LED_OV_R  40,120,200, 80,160,240,120,200
#define RIFT_LED_OV_G  80,140,200, 80,140,200, 80,140
#define RIFT_LED_OV_B 200,240,255,200,240,255,200,240
#define RIFT_LED_SB_R 200, 80,160,240,120,200, 80,160
#define RIFT_LED_SB_G  60,100,140, 60,100,140, 60,100
#define RIFT_LED_SB_B  80,120,180, 80,120,180, 80,120
#endif

static const ThemePalette theme_presets[NUM_THEMES] = {
    // Mojave - desert amber
    { "Mojave",
      0x000000, 0xFF9000, 0xFFB040, 0x804800,
      0xFF6000, 0x0A0A00, 0x603600, 0x502D00, true },
    // Ribbit City - Fallout 3 green
    { "Ribbit City",
      0x000000, 0x20FF20, 0x60FF60, 0x208020,
      0x00FF00, 0x000A00, 0x106010, 0x105010, true },
    // Flashbang - black on white (you asked for it)
    { "Flashbang",
      0xF0F0F0, 0x202020, 0x000000, 0x909090,
      0xCC0000, 0xE0E0E0, 0xB0B0B0, 0xC0C0C0, false },
    // Overseer (--rift, index 3) - KS-exclusive colorway; real palette from
    // rift_private.h, generic placeholder in public/non-rift builds (see the gate
    // above). Kept as a full slot so NUM_THEMES + NVS theme indices stay stable.
    { RIFT_THEME_OVERSEER },
    // Space Badge (--rift, index 4) - KS-exclusive colorway; see Overseer note.
    { RIFT_THEME_SPACE_BADGE },
    // Quanta (REWARD) - MONOCHROME electric cyan on near-black, with near-WHITE
    // active/selected (NO yellow/gold, so it can't clash with Overseer's Vault-
    // Tec yellow). Unique vs all of the above (nobody owns cyan); high-contrast
    // for legibility. The active indicators PULSE (cyan glow) -- the one animated
    // theme, so it reads "reward" at a glance. (Named "Quanta" for legal distance.)
    //   bg, primary(cyan body), highlight(near-white active), dim(teal 2ndary),
    //   accent(bright-cyan running glow), bg_dark, border(teal divider), disabled.
    { "Quanta",
      0x00060E, 0x2EF2FF, 0xEAFEFF, 0x57C9C2,
      0x9CFBFF, 0x04121C, 0x1F8079, 0x2E5A57, true },
};

static uint8_t cur_theme_idx = 0;
static ThemePalette pal;  // mutable copy of active palette

// ---------------------------------------------------------------------------
// Color accessors (read from active palette)
// ---------------------------------------------------------------------------
static inline lv_color_t pip_bg(void)        { return lv_color_hex(pal.bg);        }
static inline lv_color_t pip_primary(void)   { return lv_color_hex(pal.primary);   }
static inline lv_color_t pip_highlight(void) { return lv_color_hex(pal.highlight); }
static inline lv_color_t pip_dim(void)       { return lv_color_hex(pal.dim);       }
static inline lv_color_t pip_accent(void)    { return lv_color_hex(pal.accent);    }
static inline lv_color_t pip_bg_dark(void)   { return lv_color_hex(pal.bg_dark);   }
static inline lv_color_t pip_border(void)    { return lv_color_hex(pal.border);    }
static inline lv_color_t pip_disabled(void)  { return lv_color_hex(pal.disabled);  }

// ---------------------------------------------------------------------------
// Style variables
// ---------------------------------------------------------------------------
static lv_style_t style_screen_bg;
static lv_style_t style_tab_bar;
static lv_style_t style_tab_btn;
static lv_style_t style_tab_btn_active;
static lv_style_t style_div_bar;
static lv_style_t style_div_btn;
static lv_style_t style_div_btn_active;
static lv_style_t style_list_bg;
static lv_style_t style_list_btn;
static lv_style_t style_list_btn_pressed;
static lv_style_t style_list_btn_selected;
static lv_style_t style_list_btn_disabled;
static lv_style_t style_detail_panel;
static lv_style_t style_action_btn;
static lv_style_t style_action_btn_pressed;
static lv_style_t style_status_bar;
static lv_style_t style_container;
static lv_style_t style_scrollbar;
static lv_style_t style_category_header;

// ---------------------------------------------------------------------------
// ui_theme_init_styles() - (re)initialize all styles from current palette
// ---------------------------------------------------------------------------
static void ui_theme_init_styles(void) {

    // -- Screen background --
    lv_style_reset(&style_screen_bg);
    lv_style_init(&style_screen_bg);
    lv_style_set_bg_color(&style_screen_bg, pip_bg());
    lv_style_set_bg_opa(&style_screen_bg, LV_OPA_COVER);
    lv_style_set_text_color(&style_screen_bg, pip_primary());
    lv_style_set_text_font(&style_screen_bg, &ui_font_pipboy_16);
    lv_style_set_pad_all(&style_screen_bg, 0);

    // -- Top tab bar --
    lv_style_reset(&style_tab_bar);
    lv_style_init(&style_tab_bar);
    lv_style_set_bg_color(&style_tab_bar, pip_bg());
    lv_style_set_bg_opa(&style_tab_bar, LV_OPA_COVER);
    lv_style_set_border_color(&style_tab_bar, pip_border());
    lv_style_set_border_width(&style_tab_bar, 1);
    lv_style_set_border_side(&style_tab_bar, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_pad_all(&style_tab_bar, 0);
    lv_style_set_pad_gap(&style_tab_bar, 0);

    // -- Inactive tab button --
    lv_style_reset(&style_tab_btn);
    lv_style_init(&style_tab_btn);
    lv_style_set_bg_color(&style_tab_btn, pip_bg());
    lv_style_set_bg_opa(&style_tab_btn, LV_OPA_COVER);
    lv_style_set_text_color(&style_tab_btn, pip_dim());
    lv_style_set_text_font(&style_tab_btn, &ui_font_pipboy_14);
    lv_style_set_border_width(&style_tab_btn, 0);
    lv_style_set_radius(&style_tab_btn, 0);
    lv_style_set_pad_hor(&style_tab_btn, 2);
    lv_style_set_pad_ver(&style_tab_btn, 4);

    // -- Active tab button --
    lv_style_reset(&style_tab_btn_active);
    lv_style_init(&style_tab_btn_active);
    lv_style_set_bg_opa(&style_tab_btn_active, LV_OPA_TRANSP);
    lv_style_set_text_color(&style_tab_btn_active, pip_primary());
    lv_style_set_text_font(&style_tab_btn_active, &ui_font_pipboy_14);
    lv_style_set_border_color(&style_tab_btn_active, pip_primary());
    lv_style_set_border_width(&style_tab_btn_active, 2);
    lv_style_set_border_side(&style_tab_btn_active, LV_BORDER_SIDE_BOTTOM);

    // -- Division bar (bottom) --
    lv_style_reset(&style_div_bar);
    lv_style_init(&style_div_bar);
    lv_style_set_bg_color(&style_div_bar, pip_bg());
    lv_style_set_bg_opa(&style_div_bar, LV_OPA_COVER);
    lv_style_set_border_color(&style_div_bar, pip_border());
    lv_style_set_border_width(&style_div_bar, 1);
    lv_style_set_border_side(&style_div_bar, LV_BORDER_SIDE_TOP);
    lv_style_set_pad_all(&style_div_bar, 0);
    lv_style_set_pad_gap(&style_div_bar, 0);

    // -- Inactive division button --
    lv_style_reset(&style_div_btn);
    lv_style_init(&style_div_btn);
    lv_style_set_bg_opa(&style_div_btn, LV_OPA_TRANSP);
    lv_style_set_text_color(&style_div_btn, pip_dim());
    lv_style_set_text_font(&style_div_btn, &ui_font_pipboy_16);
    lv_style_set_border_width(&style_div_btn, 0);
    lv_style_set_radius(&style_div_btn, 0);
    lv_style_set_pad_hor(&style_div_btn, 4);
    lv_style_set_pad_ver(&style_div_btn, 4);

    // -- Active division button --
    lv_style_reset(&style_div_btn_active);
    lv_style_init(&style_div_btn_active);
    lv_style_set_text_color(&style_div_btn_active, pip_highlight());

    // -- List background --
    lv_style_reset(&style_list_bg);
    lv_style_init(&style_list_bg);
    lv_style_set_bg_color(&style_list_bg, pip_bg());
    lv_style_set_bg_opa(&style_list_bg, LV_OPA_COVER);
    lv_style_set_border_width(&style_list_bg, 0);
    lv_style_set_pad_all(&style_list_bg, 0);
    lv_style_set_pad_gap(&style_list_bg, 0);
    lv_style_set_radius(&style_list_bg, 0);

    // -- List button (normal) --
    lv_style_reset(&style_list_btn);
    lv_style_init(&style_list_btn);
    lv_style_set_bg_opa(&style_list_btn, LV_OPA_TRANSP);
    lv_style_set_text_color(&style_list_btn, pip_primary());
    lv_style_set_text_font(&style_list_btn, &ui_font_pipboy_16);
    lv_style_set_border_color(&style_list_btn, pip_border());
    lv_style_set_border_width(&style_list_btn, 1);
    lv_style_set_border_side(&style_list_btn, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_min_height(&style_list_btn, 32);
    lv_style_set_pad_ver(&style_list_btn, 3);
    lv_style_set_pad_hor(&style_list_btn, 4);
    lv_style_set_radius(&style_list_btn, 0);

    // -- List button pressed --
    lv_style_reset(&style_list_btn_pressed);
    lv_style_init(&style_list_btn_pressed);
    lv_style_set_bg_color(&style_list_btn_pressed, pip_dim());
    lv_style_set_bg_opa(&style_list_btn_pressed, 80);
    lv_style_set_text_color(&style_list_btn_pressed, pip_highlight());

    // -- List button selected (current item) --
    lv_style_reset(&style_list_btn_selected);
    lv_style_init(&style_list_btn_selected);
    lv_style_set_text_color(&style_list_btn_selected, pip_highlight());
    lv_style_set_bg_color(&style_list_btn_selected, pip_bg_dark());
    lv_style_set_bg_opa(&style_list_btn_selected, LV_OPA_COVER);

    // -- List button disabled (fake/unavailable items) --
    lv_style_reset(&style_list_btn_disabled);
    lv_style_init(&style_list_btn_disabled);
    lv_style_set_text_color(&style_list_btn_disabled, pip_disabled());

    // -- Detail panel --
    lv_style_reset(&style_detail_panel);
    lv_style_init(&style_detail_panel);
    lv_style_set_bg_color(&style_detail_panel, pip_bg());
    lv_style_set_bg_opa(&style_detail_panel, LV_OPA_COVER);
    lv_style_set_border_color(&style_detail_panel, pip_border());
    lv_style_set_border_width(&style_detail_panel, 1);
    lv_style_set_border_side(&style_detail_panel, LV_BORDER_SIDE_LEFT);
    lv_style_set_pad_all(&style_detail_panel, 6);
    lv_style_set_pad_gap(&style_detail_panel, 4);
    lv_style_set_radius(&style_detail_panel, 0);

    // -- Action button --
    lv_style_reset(&style_action_btn);
    lv_style_init(&style_action_btn);
    lv_style_set_bg_color(&style_action_btn, pip_primary());
    lv_style_set_bg_opa(&style_action_btn, LV_OPA_COVER);
    lv_style_set_text_color(&style_action_btn, pip_bg());
    lv_style_set_text_font(&style_action_btn, &ui_font_pipboy_18);
    lv_style_set_border_width(&style_action_btn, 0);
    lv_style_set_radius(&style_action_btn, 4);
    lv_style_set_pad_ver(&style_action_btn, 6);
    lv_style_set_pad_hor(&style_action_btn, 16);

    // -- Action button pressed --
    lv_style_reset(&style_action_btn_pressed);
    lv_style_init(&style_action_btn_pressed);
    lv_style_set_bg_color(&style_action_btn_pressed, pip_highlight());
    lv_style_set_bg_opa(&style_action_btn_pressed, LV_OPA_COVER);
    lv_style_set_text_color(&style_action_btn_pressed, pip_bg());

    // -- Status bar --
    lv_style_reset(&style_status_bar);
    lv_style_init(&style_status_bar);
    lv_style_set_bg_color(&style_status_bar, pip_bg());
    lv_style_set_bg_opa(&style_status_bar, LV_OPA_COVER);
    lv_style_set_text_color(&style_status_bar, pip_dim());
    lv_style_set_text_font(&style_status_bar, &ui_font_pipboy_14);
    lv_style_set_border_width(&style_status_bar, 0);
    lv_style_set_pad_ver(&style_status_bar, 1);
    lv_style_set_pad_hor(&style_status_bar, 4);

    // -- Generic flex container --
    lv_style_reset(&style_container);
    lv_style_init(&style_container);
    lv_style_set_bg_opa(&style_container, LV_OPA_TRANSP);
    lv_style_set_border_width(&style_container, 0);
    lv_style_set_pad_all(&style_container, 0);
    lv_style_set_pad_gap(&style_container, 0);
    lv_style_set_radius(&style_container, 0);

    // -- Scrollbar --
    lv_style_reset(&style_scrollbar);
    lv_style_init(&style_scrollbar);
    lv_style_set_bg_color(&style_scrollbar, pip_border());
    lv_style_set_bg_opa(&style_scrollbar, 150);
    lv_style_set_width(&style_scrollbar, 3);
    lv_style_set_radius(&style_scrollbar, 1);
    lv_style_set_pad_right(&style_scrollbar, 1);

    // -- Category header (tool groups, etc.) --
    lv_style_reset(&style_category_header);
    lv_style_init(&style_category_header);
    lv_style_set_text_color(&style_category_header, pip_dim());
    lv_style_set_text_font(&style_category_header, &ui_font_pipboy_14);
    lv_style_set_pad_top(&style_category_header, 6);
    lv_style_set_pad_bottom(&style_category_header, 2);
    lv_style_set_pad_left(&style_category_header, 4);
    lv_style_set_border_color(&style_category_header, pip_border());
    lv_style_set_border_width(&style_category_header, 1);
    lv_style_set_border_side(&style_category_header, LV_BORDER_SIDE_TOP);
}

// ---------------------------------------------------------------------------
// ui_theme_init() - first-time init. Sets Mojave palette and inits styles.
// ---------------------------------------------------------------------------
static void ui_theme_init(void) {
    pal = theme_presets[0];
    cur_theme_idx = 0;
    ui_theme_init_styles();

    // Override LVGL default theme primary color to fix dropdown highlight
    // (replaces the default blue selection with our primary color)
    lv_theme_default_init(lv_display_get_default(),
                          pip_primary(), pip_highlight(),
                          pal.dark, &ui_font_pipboy_14);
}

// ---------------------------------------------------------------------------
// Custom (completionist) theme: derive a full single-hue dark palette from one
// hue. Roles mirror Mojave/Ribbit (bright body text, lighter active, dark
// dividers). Text colors are luminance-floored so ANY hue stays legible on
// black -- "pick within reason" -> we clamp the reason.
// ---------------------------------------------------------------------------
#include <math.h>
static uint32_t theme_hsv2rgb(float h, float s, float v) {
    h = fmodf(h, 360.0f); if (h < 0) h += 360.0f;
    float c = v * s, x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f)), m = v - c;
    float r, g, b;
    if      (h <  60) { r = c; g = x; b = 0; }
    else if (h < 120) { r = x; g = c; b = 0; }
    else if (h < 180) { r = 0; g = c; b = x; }
    else if (h < 240) { r = 0; g = x; b = c; }
    else if (h < 300) { r = x; g = 0; b = c; }
    else              { r = c; g = 0; b = x; }
    uint8_t R = (uint8_t)((r + m) * 255.0f + 0.5f);
    uint8_t G = (uint8_t)((g + m) * 255.0f + 0.5f);
    uint8_t B = (uint8_t)((b + m) * 255.0f + 0.5f);
    return ((uint32_t)R << 16) | ((uint32_t)G << 8) | B;
}
static float theme_luma(uint32_t c) {
    float r = ((c >> 16) & 0xFF) / 255.0f, g = ((c >> 8) & 0xFF) / 255.0f, b = (c & 0xFF) / 255.0f;
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;   // relative luminance
}
// Blend toward white until the color meets a minimum luminance (keeps dark hues
// like blue/purple readable on black).
static uint32_t theme_floor_luma(uint32_t c, float min_luma) {
    float r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    for (int i = 0; i < 24 && theme_luma(((uint32_t)(uint8_t)r << 16) |
                                         ((uint32_t)(uint8_t)g << 8) | (uint8_t)b) < min_luma; i++) {
        r += (255.0f - r) * 0.12f; g += (255.0f - g) * 0.12f; b += (255.0f - b) * 0.12f;
    }
    return ((uint32_t)(uint8_t)(r + 0.5f) << 16) | ((uint32_t)(uint8_t)(g + 0.5f) << 8) | (uint8_t)(b + 0.5f);
}
static void theme_derive_custom(uint16_t hue) {
    float h = (float)hue;
    pal.name      = "Custom";
    pal.bg        = 0x000000;
    pal.bg_dark   = theme_hsv2rgb(h, 1.0f, 0.05f);
    pal.primary   = theme_floor_luma(theme_hsv2rgb(h, 0.80f, 1.0f), 0.42f);  // body text
    pal.highlight = theme_floor_luma(theme_hsv2rgb(h, 0.50f, 1.0f), 0.62f);  // active/selected
    pal.dim       = theme_hsv2rgb(h, 1.0f, 0.50f);
    pal.accent    = theme_hsv2rgb(h, 1.0f, 1.0f);
    pal.border    = theme_hsv2rgb(h, 1.0f, 0.36f);
    pal.disabled  = theme_hsv2rgb(h, 0.45f, 0.30f);
    pal.dark      = true;
}

// ---------------------------------------------------------------------------
// ui_theme_apply(idx) - switch to theme at index. Re-inits styles.
//   Caller must rebuild the screen after calling this.
// ---------------------------------------------------------------------------
static void ui_theme_apply(uint8_t idx) {
    if (idx == THEME_CUSTOM) {
        theme_derive_custom(g_custom_hue);   // generate the palette from the chosen hue
        cur_theme_idx = idx;
    } else {
        if (idx >= NUM_THEMES) idx = 0;
        cur_theme_idx = idx;
        pal = theme_presets[idx];
    }
    ui_theme_init_styles();

    // Re-init LVGL default theme with new primary color
    lv_theme_default_init(lv_display_get_default(),
                          pip_primary(), pip_highlight(),
                          pal.dark, &ui_font_pipboy_14);
}

// ---------------------------------------------------------------------------
// Dropdown list styling callback - fixes popup highlight color
// Attach to every dropdown via LV_EVENT_FOCUSED or call after creation.
// ---------------------------------------------------------------------------
static void style_dropdown_list(lv_obj_t *dd) {
    // Style the dropdown header
    lv_obj_set_style_bg_color(dd, pip_bg_dark(), LV_PART_MAIN);
    lv_obj_set_style_text_color(dd, pip_primary(), LV_PART_MAIN);
    lv_obj_set_style_border_color(dd, pip_border(), LV_PART_MAIN);
    lv_obj_set_style_border_width(dd, 1, LV_PART_MAIN);
    lv_obj_set_style_text_font(dd, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_width(dd, lv_pct(95));
    lv_obj_set_style_max_height(dd, 28, LV_PART_MAIN);

    // Style the arrow indicator
    lv_obj_set_style_text_color(dd, pip_dim(), LV_PART_INDICATOR);
}

// Event callback to style the dropdown popup list when it opens
static void dd_open_cb(lv_event_t *e) {
    lv_obj_t *dd = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *list = lv_dropdown_get_list(dd);
    if (!list) return;

    lv_obj_set_style_bg_color(list, pip_bg(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(list, pip_primary(), LV_PART_MAIN);
    lv_obj_set_style_text_font(list, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_border_color(list, pip_border(), LV_PART_MAIN);

    // Selected/highlighted item in the popup
    lv_obj_set_style_bg_color(list, pip_primary(), LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SELECTED);
    lv_obj_set_style_text_color(list, pip_bg(), LV_PART_SELECTED);

    // Checked+selected (currently chosen value)
    // Space Badge's highlight is saturated red — black text reads muddy on it
    // (and on-theme blue would be ~1.1:1, invisible), so use white (~3.1:1).
    // Amber-highlight themes keep black text (high contrast on amber).
    lv_color_t chk_text = (cur_theme_idx == THEME_SPACE_BADGE) ? pip_primary() : pip_bg();
    lv_obj_set_style_bg_color(list, pip_highlight(), LV_PART_SELECTED | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(list, chk_text, LV_PART_SELECTED | LV_STATE_CHECKED);
}

// Helper: create a fully-themed dropdown
static lv_obj_t* make_dropdown(lv_obj_t *parent, const char *options) {
    lv_obj_t *dd = lv_dropdown_create(parent);
    lv_dropdown_set_options(dd, options);
    style_dropdown_list(dd);
    lv_obj_add_event_cb(dd, dd_open_cb, LV_EVENT_READY, NULL);
    return dd;
}
