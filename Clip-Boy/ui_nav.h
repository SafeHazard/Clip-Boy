#pragma once
#include <lvgl.h>
#include <esp_heap_caps.h>
#include <Wire.h>
#include "drone_store.h"   // Remote ID (drone) detector store + decoder
#include <SparkFun_VL53L5CX_Library.h>
#include <HRScanner.h>
#include <HRScanIMU.h>

/*******************************************************************************
 * ui_nav.h - Clip-Boy Navigation + Content (V3)
 *
 * V3: Full hardware integration
 * - ClipBoyMarauder dispatch for all tool functions
 * - Live scan result polling (AP/Station/BT lists)
 * - NeoPixel hardware wiring from LED slider controls
 * - VL53L5CX + audio-tools theremin
 *
 * Included from ui_test.ino AFTER ui_theme.h, ui_config.h,
 * neopixel_driver.h, audio_driver.h.
 * Expects global: ClipBoyMarauder cb (from ui_test.ino)
 ******************************************************************************/

extern "C" {
    LV_IMAGE_DECLARE(ClipBoyGS153x192);
}

// Collectible images (95 x 80x80 A8, ~593KB PROGMEM → PSRAM at boot)
#include "coll_images.h"
#include "coll_color_images.h"

// SegFault-Tec FM station model (DC34-129/131). UI (show_radio) built below.
#include "radio.h"

// ─── Mascot image PSRAM copy ──────────────────────────────────────────────
// ClipBoyGS153x192 is 153x192 A8 = 29,376 bytes PROGMEM. Copy to PSRAM
// so LVGL doesn't need per-render flash reads.
#define MASCOT_PIXELS  (153 * 192)
static lv_image_dsc_t   mascot_psram_dsc = {};
static uint8_t          *mascot_psram_buf = NULL;

static const lv_image_dsc_t* mascot_image(void) {
    return mascot_psram_buf ? &mascot_psram_dsc : &ClipBoyGS153x192;
}

static void mascot_init_psram(void) {
    // Allocate ONCE. create_main_screen() re-calls this on every theme switch, but the
    // pixel data is theme-independent A8 (recolor is an LVGL tint), so re-allocating
    // just orphaned the prior ~29KB PSRAM buffer each switch (leak, review 2026-07-08).
    if (mascot_psram_buf) return;
    mascot_psram_buf = (uint8_t *)heap_caps_malloc(MASCOT_PIXELS, MALLOC_CAP_SPIRAM);
    if (!mascot_psram_buf) {
        CB_LOGLN("[IMG] Mascot PSRAM alloc failed, using flash");
        return;
    }
    memcpy_P(mascot_psram_buf, ClipBoyGS153x192.data, MASCOT_PIXELS);
    mascot_psram_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    mascot_psram_dsc.header.cf     = LV_COLOR_FORMAT_A8;
    mascot_psram_dsc.header.flags  = 0;
    mascot_psram_dsc.header.w      = 153;
    mascot_psram_dsc.header.h      = 192;
    mascot_psram_dsc.header.stride = 153;
    mascot_psram_dsc.data_size     = MASCOT_PIXELS;
    mascot_psram_dsc.data          = mascot_psram_buf;
    CB_LOGF("[IMG] Mascot copied to PSRAM (%u bytes)\n", (unsigned)MASCOT_PIXELS);
}

// ─────────────────────── CONSTANTS ─────────────────────────────────────────

#define NUM_DIVISIONS   3
#define NUM_TABS        3
#define SCREEN_W      320
#define SCREEN_H      240
#define STATUS_BAR_H   16
#define TAB_BAR_H      26
#define DIV_BAR_H      26
#define CONTENT_H     (SCREEN_H - STATUS_BAR_H - TAB_BAR_H - DIV_BAR_H)
#define CONTENT_Y     (STATUS_BAR_H + TAB_BAR_H)

#define LEFT_PANE_W   135
#define RIGHT_PANE_W  (SCREEN_W - LEFT_PANE_W)

// ─────────────────────── DIVISION & TAB DATA ──────────────────────────────

static const char *div_labels[NUM_DIVISIONS] = { "STATS", "ITEMS", "DATA" };

static const char *tab_labels[NUM_DIVISIONS][NUM_TABS] = {
    { "Status",  "L.E.E.T.",     "Radiation"    },
    { "Tools",   "Collectibles", "SAOs"         },
    { "LEDs",    "Settings",     "Theremin"     },
};

// ─────────────────────── SHARED GLOBALS ───────────────────────────────────

static uint8_t theremin_volume = 75;  // 0-100, shared between Theremin & Settings

// ─────────────────────── NAVIGATION STATE ─────────────────────────────────

static uint8_t cur_div = 0;
static uint8_t cur_tab = 0;
static int16_t cur_sel = -1;

// ─────────────────────── WIDGET REFERENCES ────────────────────────────────

static lv_obj_t *scr_main    = NULL;
static lv_obj_t *status_bar  = NULL;
static lv_obj_t *tab_bar_obj = NULL;
static lv_obj_t *content_obj = NULL;
static lv_obj_t *div_bar_obj = NULL;
static lv_obj_t *tab_btns[NUM_TABS]      = {};
static lv_obj_t *div_btns[NUM_DIVISIONS] = {};
static lv_obj_t *div_leds[NUM_DIVISIONS] = {};

static lv_obj_t *lbl_sbat   = NULL;
static lv_obj_t *lbl_swifi  = NULL;   // WiFi connected icon
static lv_obj_t *lbl_stask  = NULL;
static lv_obj_t *lbl_smem   = NULL;
static lv_obj_t *btn_sflash = NULL;   // Flashlight toggle button
static lv_obj_t *lbl_sflash = NULL;   // Flashlight label inside the button

// F5 -- repaint the status-bar "FL" indicator from the ACTUAL flashlight state.
// It used to be coloured in exactly two places: at build, and inside its own tap handler. So
// anything else that changed the flashlight left the indicator asserting the old state. Dark
// Charge does exactly that (neo_flashlight_set(false) on the way into the dark screensaver),
// and screensaver_dismiss() restores the strip but not the flag -- so you woke up with the
// light off and "FL" still lit, and your next tap turned it ON while the label didn't visibly
// change. Two taps to reach the state one tap should have given (owner-confirmed).
// Call this from EVERY path that changes the flashlight, not just the button.
static void flashlight_ui_sync(void) {
    if (!lbl_sflash || !lv_obj_is_valid(lbl_sflash)) return;
    lv_obj_set_style_text_color(lbl_sflash,
        neo_flashlight_is_on() ? pip_highlight() : pip_dim(), 0);
}
static lv_obj_t *btn_shelp  = NULL;   // '?' help button (visibility from cfg.help_btn)

static lv_obj_t *left_pane  = NULL;
static lv_obj_t *right_pane = NULL;

static lv_timer_t *status_timer = NULL;

// WiFi join state - declared early so all functions can access
static char wifi_join_ssid[65] = "";
static char wifi_join_pw[65]   = "";
static lv_obj_t *wifi_status_label   = NULL;
static lv_obj_t *wifi_scan_btn_label = NULL;

// Volume slider references for cross-update
static lv_obj_t *theremin_vol_slider = NULL;
static lv_obj_t *settings_vol_slider = NULL;
static lv_obj_t *theremin_vol_label  = NULL;
static lv_obj_t *settings_vol_label  = NULL;

// Screensaver-dependent row refs -- the Screensaver-timeout and Style dropdowns
// gate which of the related rows are meaningful. Captured at row creation so
// the dropdown callbacks can dim/undim them as the user adjusts.
static lv_obj_t *ss_style_row_label  = NULL;
static lv_obj_t *ss_style_dropdown   = NULL;
static lv_obj_t *ss_dim_row_label    = NULL;
static lv_obj_t *ss_dim_switch       = NULL;
static lv_obj_t *ss_bright_row_label = NULL;
static lv_obj_t *ss_bright_slider    = NULL;
static lv_obj_t *ss_bright_readout   = NULL;

// disp_off dropdown index 5 = "Never" -- screensaver won't fire at all, so
// every row that depends on the screensaver firing is moot.
#define DISP_OFF_NEVER 5

static void ss_settings_update_enables(void) {
    bool ss_will_fire   = (cfg.disp_off != DISP_OFF_NEVER);
    bool lit_style      = (cfg.ss_style != 1);  // idle brightness applies to lit savers (Clip-Boy + Flying Clippy)
    bool style_enabled  = ss_will_fire;
    bool dim_enabled    = ss_will_fire;
    bool bright_enabled = ss_will_fire && lit_style;

    if (ss_style_row_label)
        lv_obj_set_style_text_color(ss_style_row_label,
            style_enabled ? pip_primary() : pip_disabled(), 0);
    if (ss_style_dropdown) {
        if (style_enabled) lv_obj_add_flag(ss_style_dropdown, LV_OBJ_FLAG_CLICKABLE);
        else               lv_obj_remove_flag(ss_style_dropdown, LV_OBJ_FLAG_CLICKABLE);
    }

    if (ss_dim_row_label)
        lv_obj_set_style_text_color(ss_dim_row_label,
            dim_enabled ? pip_primary() : pip_disabled(), 0);
    if (ss_dim_switch) {
        if (dim_enabled) lv_obj_remove_state(ss_dim_switch, LV_STATE_DISABLED);
        else             lv_obj_add_state(ss_dim_switch, LV_STATE_DISABLED);
    }

    if (ss_bright_row_label)
        lv_obj_set_style_text_color(ss_bright_row_label,
            bright_enabled ? pip_primary() : pip_disabled(), 0);
    if (ss_bright_readout)
        lv_obj_set_style_text_color(ss_bright_readout,
            bright_enabled ? pip_dim() : pip_disabled(), 0);
    if (ss_bright_slider) {
        if (bright_enabled) {
            lv_obj_add_flag(ss_bright_slider, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_color(ss_bright_slider, pip_primary(),  LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(ss_bright_slider, pip_highlight(), LV_PART_KNOB);
        } else {
            lv_obj_remove_flag(ss_bright_slider, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_color(ss_bright_slider, pip_disabled(), LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(ss_bright_slider, pip_disabled(), LV_PART_KNOB);
        }
    }
}

// Debounced NVS save for ss_brightness -- matches the volume slider pattern.
// Per-drag flash writes were stalling the LED tick on core 0.
#define SS_BRIGHT_SAVE_DEBOUNCE_MS  2000
static lv_timer_t *ss_bright_save_timer = NULL;

static void ss_bright_save_timer_cb(lv_timer_t *t) {
    (void)t;
    cfg_save_ss_brightness();
    lv_timer_delete(ss_bright_save_timer);
    ss_bright_save_timer = NULL;
}

static void ss_bright_cfg_mark_dirty(void) {
    if (ss_bright_save_timer) {
        lv_timer_reset(ss_bright_save_timer);
    } else {
        ss_bright_save_timer = lv_timer_create(ss_bright_save_timer_cb,
                                               SS_BRIGHT_SAVE_DEBOUNCE_MS, NULL);
        lv_timer_set_repeat_count(ss_bright_save_timer, 1);
    }
}

static void ss_bright_slider_cb(lv_event_t *e) {
    lv_obj_t *sl = (lv_obj_t *)lv_event_get_target(e);
    int32_t val = lv_slider_get_value(sl);
    cfg.ss_brightness = (uint8_t)val;
    ss_bright_cfg_mark_dirty();
    lv_obj_t *lbl = (lv_obj_t *)lv_event_get_user_data(e);
    if (lbl) lv_label_set_text_fmt(lbl, "%ld%%", (long)val);
}

// ─── ClipBoy operation state ───────────────────────────────────────────────
static bool         cb_op_running    = false;    // Is a ClipBoy operation active?
static const char  *cb_op_name       = NULL;     // Name shown in status bar
static int32_t      cb_op_encoded    = -1;       // Which tool (encoded cat+item) is running, -1 = none
static lv_timer_t  *cb_scan_timer    = NULL;     // Poll timer for live scan results
static lv_obj_t    *cb_ap_list_area  = NULL;     // AP checkbox list container
static lv_obj_t    *cb_sta_list_area = NULL;     // Station checkbox list container
static int          cb_last_ap_count = 0;        // Last known AP count (for delta update)
static int          cb_last_sta_count = 0;

// ─── Collapsible tool categories ────────────────────────────────────────
static uint16_t tool_cats_expanded = 0x0000;  // bit per category, 0=collapsed, 1=expanded

// ─── Live output display ──────────────────────────────────────────────────
static lv_obj_t    *cb_output_scroll = NULL;  // Scrollable output container
static lv_obj_t    *cb_output_log    = NULL;  // Label inside for log text
static lv_timer_t  *cb_output_timer  = NULL;  // Poll timer for output updates
static int          cb_output_last_ap = 0;    // Incremental AP discovery tracking
static int          cb_output_last_sta = 0;   // Incremental station tracking

// ─── Screen saver state ───────────────────────────────────────────────────────
static bool         screensaver_active  = false;
static bool         screensaver_dimmed  = false;
// Dark Charge = a user-invoked screensaver where LEDs go off regardless of the
// "Dim LEDs when idle" setting, and stay off until the hold-to-unlock.
static bool         dark_charge_active  = false;
static lv_obj_t    *screensaver_overlay = NULL;
static lv_obj_t    *screensaver_bar     = NULL;
static lv_obj_t    *screensaver_lbl     = NULL;
static lv_timer_t  *screensaver_timer   = NULL;
static uint32_t     screensaver_hold_start = 0;

// ── Flying-Clippy screensaver sprites (ARG-complete reward, cfg.ss_style==2) ──
extern "C" {
  LV_IMAGE_DECLARE(clippy_up_l);   LV_IMAGE_DECLARE(clippy_down_l);
  LV_IMAGE_DECLARE(clippy_up_m);   LV_IMAGE_DECLARE(clippy_down_m);
  LV_IMAGE_DECLARE(clippy_up_s);   LV_IMAGE_DECLARE(clippy_down_s);
}
#define CLIPPY_N 7
static lv_obj_t   *clippy_spr[CLIPPY_N] = { NULL };
static lv_timer_t *clippy_timer = NULL;
static float       clippy_px[CLIPPY_N], clippy_py[CLIPPY_N];
static uint8_t     clippy_sz[CLIPPY_N];     // 0=large/near .. 2=small/far
static uint8_t     clippy_fr[CLIPPY_N];     // current wing frame (0=down,1=up)
static uint8_t     clippy_flap[CLIPPY_N];   // flap phase counter
static const lv_image_dsc_t *const clippy_up_img[3]   = { &clippy_up_l, &clippy_up_m, &clippy_up_s };
static const lv_image_dsc_t *const clippy_down_img[3] = { &clippy_down_l, &clippy_down_m, &clippy_down_s };
static const float clippy_speed[3] = { 4.2f, 2.9f, 1.9f };  // parallax: near flies faster
#define CLIPPY_COS 0.99580f   // cos 5.25 deg  (drift left)
#define CLIPPY_SIN 0.09150f   // sin 5.25 deg  (rise up)

#define SCREENSAVER_HOLD_MS  2000
// Unlock-hold progress tone: a soft sine that sweeps A3->E5 (log/perceptual) as
// the bar fills, then a short E6 chirp on completion. Gentle, mute+toggle gated.
#define SS_TONE_LO_HZ      220.0f   // A3 - hold start
#define SS_TONE_HI_HZ      660.0f   // E5 - hold complete
#define SS_TONE_CHIME_HZ   784.0f   // G5 - soft "unlocked!" confirm note (low + quiet so it won't overdrive the speakers)

static const uint32_t screensaver_timeouts[] = {
    15000, 30000, 60000, 120000, 300000, 0  // 0 = Never
};


// ─── CRT effects state ──────────────────────────────────────────────────────
static lv_obj_t    *crt_scanline_img   = NULL;   // Full-screen scanline overlay image
static uint8_t     *crt_scanline_buf   = NULL;   // 320x2 A8 pixel buffer (PSRAM)
static lv_image_dsc_t crt_scanline_dsc = {};     // Image descriptor for scanline tile
static lv_timer_t  *crt_flicker_timer  = NULL;   // Random-interval V-roll timer
static bool         crt_vroll_active   = false;  // V-roll animation in flight
static bool         crt_vroll_double   = false;  // This firing does two rolls
static int32_t      crt_vroll_tear_x   = 0;      // Current horizontal tear offset (px)
static int32_t      crt_vroll_anim_var = 0;      // Sentinel var for animations
static lv_draw_buf_t *crt_vroll_snap   = NULL;   // Snapshotted frame (RGB565, PSRAM)
static lv_obj_t    *crt_vroll_upper    = NULL;   // Upper snapshot copy
static lv_obj_t    *crt_vroll_lower    = NULL;   // Lower snapshot copy (wrap)
static lv_obj_t    *crt_vroll_sync_bar = NULL;   // Black sync bar at the seam
static lv_obj_t    *crt_vroll_dim      = NULL;   // Translucent brightness-dip overlay
static lv_obj_t    *crt_vroll_catcher  = NULL;   // Transparent touch catcher (abort on press)
static lv_timer_t  *crt_vroll_tear_timer = NULL; // Periodic tear-x randomizer

// ─── Theremin hardware state ───────────────────────────────────────────────
static TwoWire            vl53_wire(1);   // I2C bus 1 (bus 0 is touch controller)
static SparkFun_VL53L5CX  vl53_sensor;
static bool               vl53_initialized = false; // ranging active right now
static bool               vl53_begun       = false; // ~84KB firmware uploaded; persists across SLEEP/WAKE, cleared when an HR scan takes the sensor
static lv_timer_t        *theremin_poll_timer = NULL;
static bool               theremin_begun = false;  // tracks aud_theremin.begin() state

// Theremin UI state -- one entry per band (4 bands, L/CL/CR/R)
static lv_obj_t          *theremin_voice_bars[4] = {};   // vertical bar FILL objs (top-anchored)
static lv_obj_t          *theremin_voice_bar_wells[4] = {};  // backgrounds (so we can tint per band state)
static lv_obj_t          *theremin_voice_dd[4]   = {};   // voice-type dropdowns
static lv_obj_t          *theremin_dist_labels[4] = {};  // mm readout per band
static lv_obj_t          *theremin_freq_labels[4] = {};  // Hz readout per band
static lv_obj_t          *theremin_enable_btn   = NULL;  // moved to left pane
static lv_obj_t          *theremin_k_slider     = NULL;
static lv_obj_t          *theremin_k_label      = NULL;
static lv_obj_t          *theremin_agree_slider = NULL;
static lv_obj_t          *theremin_agree_label  = NULL;

// Spatial labels for the 4 bands; matches dropdown labels in left pane
static const char * const theremin_band_label[4] = { "L", "CL", "CR", "R" };

// Bar geometry -- vertical bar fills downward from the top to indicate
// "hand getting closer". Tall fill = close hand = high pitch.
#define THEREMIN_BAR_W 30
#define THEREMIN_BAR_H 80

// ─── HR Code scanner state ────────────────────────────────────────────────
static HRScan::Scanner    hr_scanner;
static HRScanIMU::QMI8658 hr_imu;
static bool               hr_imu_ok   = false;
static bool               hr_scanning = false;
// Sensor flat-field calibration: per-zone LiDAR baseline captured on a flat
// surface (cancels the ~8mm center-far/edges-near bowl that muddies bump reads).
static bool               hr_cal_capturing = false;
static uint32_t           hr_cal_start_ms  = 0;
static bool               hr_cal_aiming    = false;  // cal AIM phase (live preview before capture)
static lv_obj_t          *hr_cal_ready_btn = NULL;   // "Ready" btn during aim (child of hr_blackout)
static lv_obj_t          *hr_cal_readout   = NULL;   // live "N/64 zones - D mm" label during aim
static void hr_cal_aim_update(void);                 // fwd: per-loop live readout
static void hr_cal_do_capture(void);                 // fwd: Ready -> start the 5s capture
static int16_t            hr_zone_cal[64]  = {0};
static bool               hr_has_cal       = false;   // a good calibration is loaded
static int                hr_cal_zones     = 0;       // # zones covered by the loaded cal (modal)
static int                hr_cal_avg_mm    = 0;       // mean captured distance of the loaded cal (modal)
// Scan-screen level bubble (bottom-left of the scan strip; bigger + more visible
// than the overlay's old corner dot). Children of hr_blackout -> self-null on delete.
static lv_obj_t          *hr_level_ring = NULL;
static lv_obj_t          *hr_level_bub  = NULL;

// GRAVITY-ONLY level bubble (IMU roll/pitch). Purely a HUMAN aiming aid -- it
// does NOT gate locking (the lock FSM gates on tag-relative depth tilt, never on
// the IMU). Dot drifts with tilt; the dot AND the ring go WHITE in the "aimed
// right" band: ~0 roll and 0..20 deg nose-DOWN, because the tag holders are
// printed at ~20 deg pitch (so a level badge over a flat tag AND a 20-deg-down
// badge over a stand tag both read as good). Flip HR_PITCH_DOWN_SIGN if white
// lands on nose-UP instead of nose-down on hardware.
static constexpr float HR_PITCH_DOWN_SIGN = -1.0f;  // maps raw pitch so nose-down is +

static void hr_level_update(float roll, float pitch) {
    if (!hr_level_ring || !hr_level_bub) return;
    const int ring = 40, dot = 14;
    const int travel = (ring - dot) / 2 - 1;
    const int cc = (ring - dot) / 2 - 1;
    const float maxDeg = 45.0f;
    float px = roll  / maxDeg; if (px > 1) px = 1; if (px < -1) px = -1;
    float py = pitch / maxDeg; if (py > 1) py = 1; if (py < -1) py = -1;
    lv_obj_set_pos(hr_level_bub, cc + (int)(px * travel), cc + (int)(-py * travel));
    const float down = HR_PITCH_DOWN_SIGN * pitch;      // degrees nose-down
    const bool level = (fabsf(roll) <= 6.0f) && (down >= -6.0f) && (down <= 26.0f);
    const lv_color_t col = level ? lv_color_hex(0xFFFFFF) : pip_primary();
    lv_obj_set_style_bg_color(hr_level_bub, col, 0);     // dot
    lv_obj_set_style_border_color(hr_level_ring, col, 0); // + ring, both go white
}
static lv_obj_t          *hr_blackout = NULL;  // Full-screen dimming overlay
// The one-shot result-banner cleanup timer (timeout/sensor-fail). TRACKED so a new
// scan can cancel it -- else it fires 1.5s later and deletes the NEW scan's overlay
// (LVGL-timer-UAF: reboot on a scan started within 1.5s of a prior timeout).
static lv_timer_t        *hr_cleanup_timer = NULL;
static inline void hr_cancel_cleanup_timer(void) {
    if (hr_cleanup_timer) { lv_timer_delete(hr_cleanup_timer); hr_cleanup_timer = NULL; }
}

// ─────────────────────── FORWARD DECLARATIONS ─────────────────────────────

static void rebuild_content();
// Everything rebuild_content() does to DISMANTLE the outgoing screen, minus the rebuild.
// Any function that destroys the content pane must call this or it leaves live timers and
// animations pointing at freed widgets. See its definition for the full rationale.
static void content_teardown();
static void screensaver_dismiss(void);   // fwd — teardown must dismiss an active saver
static void show_help(lv_obj_t *cont);
static void show_clippy_intro();
static void show_clippy_tour_step(int n);
static lv_obj_t *find_coll_btn(int coll_idx);
static void rebuild_tabs();
static void update_div_indicators();
static void goto_div_tab(uint8_t d, uint8_t t);  // programmatic nav (status-bar jump)
static void show_new_station_modal(const char *station_name, const char *subtitle,
                                   void (*on_view)(void), bool allow_optout = false);  // radio popup
static void radio_check_unlocks(void);                      // fwd: announce collectible-gated station unlocks
static void radio_unlock_timer_cb(lv_timer_t *t);           // fwd: 1.5s-after-found-modal unlock check
static void radio_reminder_arm(void);                       // fwd: boot-time reminder nudge
static void radio_reminder_on_wake(void);                   // fwd: show a saver-deferred reminder on wake
static void custom_check_reveal(void);                      // fwd: 100%-collectibles custom-theme reveal
static void show_hue_picker(void);                          // fwd: custom-theme hue picker modal
static void coll_msg_modal(const char *title, const char *msg);  // fwd: simple title+message modal
static void show_radio(lv_obj_t *cont);   // SegFault-Tec FM full-screen radio (DC34-131)
static void radio_stop(void);             // radio teardown — stop audio/timers on nav-away
static void create_main_screen();
static void crt_apply();
static void crt_scanlines_raise();
static void crt_vroll_anim_cb(void *var, int32_t y);
static void crt_vroll_abort();
static void cb_stop_operation();
static void cb_inline_scan_refresh();   // F1: re-dim the inline Scan on any start/stop
// Clear a result store AND the widget showing it, synchronously (see their definitions).
static void cb_clear_ap_results();
static void cb_clear_sta_results();
// Redraw the CURRENT tool page. Handlers must use this, never rebuild_content(), which
// rebuilds from cur_sel and lands on an unrelated tool (see its definition ~:5188).
static void tool_page_reshow();
// Persist + apply an airplane-mode change. Every writer of cfg.airplane routes through this
// (Settings switch, the tool block dialog's "Turn Off", and the harness) so no path can skip
// the teardown -- see its definition below.
static void airplane_apply(bool on);
// The on-screen keyboard modal (defined ~:4584, with kb_done_cb_t typedef'd just above it).
// Declared with the raw function-pointer type so this needs no duplicate typedef -- a typedef
// is transparent, so this matches the definition exactly.
static void kb_open(const char *title, const char *initial, bool password,
                    void (*done_cb)(const char *text, void *user_data), void *ud);
static void cb_ensure_wifi();
static void cb_output_cleanup();
static void stop_lidar_activities();   // stop HR scan + theremin (share VL53L5CX)

// ─────────────────────── HELPERS ──────────────────────────────────────────

static void clear_children(lv_obj_t *obj) {
    if (!obj) return;
    uint32_t cnt = lv_obj_get_child_count(obj);
    for (int32_t i = cnt - 1; i >= 0; i--)
        lv_obj_delete(lv_obj_get_child(obj, i));
}

// Self-null a global lv_obj_t* when its object is deleted. Prevents the scan poll
// timer (cb_scan_poll_cb) from writing to freed content-pane widgets after the
// user navigates divisions/tabs (or switches theme) mid-scan -- the rebuild
// deletes those widgets but the C pointers were left dangling, and the poller's
// `if(!ptr)` guards passed on the non-NULL dangling pointer -> use-after-free
// crash. Same pattern cb_output_log already uses. Pass &the_global as user_data.
static void cb_selfnull_on_delete(lv_event_t *e) {
    lv_obj_t **pp = (lv_obj_t **)lv_event_get_user_data(e);
    // ONLY clear the global if it still points at the object being deleted.
    // Without this identity check the whole self-null pattern is UNSOUND on the
    // theme-switch path, because ui_theme_switch_live() builds the NEW screen BEFORE
    // freeing the old one (ui_nav.h:7899-7907):
    //     old_scr = scr_main; scr_main = NULL;
    //     create_main_screen();          // -> rebuild_content() -> the builders re-point
    //                                    //    these globals at the NEW widgets
    //     lv_obj_delete(old_scr);        // -> the OLD widgets' DELETE handlers run LAST
    // so an unconditional `*pp = NULL` wipes a pointer that now refers to a live, current
    // widget. Observed: a theme change on STATS > Radiation with the Geiger running froze
    // the needle and every stat label permanently (rad_poll_cb is all `if (ptr)`), while
    // the tick audio kept playing and the status bar still said "Geiger active".
    // Found by adversarial review 2026-07-25 as a regression from adding six rad_*
    // registrations -- but the hazard was already latent for the ~10 pre-existing ones
    // (cb_output_area, cb_output_chart, cb_ap_list_area, wifi_status_label, ...), so the
    // fix belongs here rather than in the call sites.
    if (pp && *pp == (lv_obj_t *)lv_event_get_target(e)) *pp = NULL;
}

// ── ARG "secret menu" (DC34): a hidden nav-tap sequence opens the P5 reward
// keypad, like a Konami code on the real UI chrome. Tap tokens are recorded from
// the division bar (STATS/ITEMS/DATA) + the status-bar flashlight button; when the
// last N taps match arg_secret[], the keypad is revealed.
// GATED on P1-P4 being complete: the HMAC code entry is still the real
// cryptographic gate, but revealing the finale keypad to a player who hasn't
// earned the endgame spoils its existence/shape — so the knock does nothing until
// the first four puzzles are solved.
// arg_reveal_keypad_fn is wired to show_p5_numpad by p5_register() (arg_p5_call.h).
enum { ARG_TOK_STATS = 1, ARG_TOK_ITEMS, ARG_TOK_DATA, ARG_TOK_FLASH };
#define ARG_PRE_P5  (ARG_P1_RADIO | ARG_P2_HACK | ARG_P3_DORK | ARG_P4_CAPTCHA)  // 0x0F
static const uint8_t arg_secret[] = { ARG_TOK_STATS, ARG_TOK_FLASH, ARG_TOK_FLASH, ARG_TOK_DATA, ARG_TOK_ITEMS };
#define ARG_SECRET_LEN (sizeof(arg_secret) / sizeof(arg_secret[0]))
static uint8_t   arg_secret_buf[ARG_SECRET_LEN] = {0};
static void    (*arg_reveal_keypad_fn)(void) = nullptr;

static void arg_secret_record(uint8_t tok) {
    // The knock only listens once the finale is earned — keeps the keypad hidden
    // from players still working P1-P4 (and from idle chrome-tapping).
    if (!arg_flag(ARG_PRE_P5)) return;
    for (uint8_t i = 1; i < ARG_SECRET_LEN; i++) arg_secret_buf[i - 1] = arg_secret_buf[i];
    arg_secret_buf[ARG_SECRET_LEN - 1] = tok;
    if (memcmp(arg_secret_buf, arg_secret, ARG_SECRET_LEN) == 0) {
        memset(arg_secret_buf, 0, sizeof(arg_secret_buf));   // consume, so it won't re-fire
        if (arg_reveal_keypad_fn) arg_reveal_keypad_fn();
    }
}

static lv_obj_t* make_label(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, lv_color_t color) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, color, 0);
    return lbl;
}

// Tool "terminal" / scan-log font, user-selectable (Settings > Terminal Text).
// 14/16/18 all carry the extended glyph range + emoji fallback so names render
// the same at any size. 0=Small 1=Medium 2=Large.
static inline const lv_font_t *term_font(void) {
    switch (cfg.term_font) {
        case 0:  return &ui_font_pipboy_14;
        case 2:  return &ui_font_pipboy_18;
        default: return &ui_font_pipboy_16;
    }
}

// ── Untrusted-name display sanitizer ──────────────────────────────────────
// SSIDs and BLE device names are adversary-controlled. Rendering them raw is
// both ugly (glyphs the font lacks show as tofu) and a spoofing surface
// (bidi-override / zero-width / control chars reorder or hide text). cb_safe()
// returns a render-safe copy that is LENGTH-PRESERVING and never collapses or
// silently drops -- each input codepoint maps to exactly one visible token, so
// two different names can't alias to the same string:
//   - a drawable codepoint (font incl. its emoji fallback chain can render it)
//     is copied verbatim. Font-driven via lv_font_get_glyph_dsc(.fallback), so
//     coverage tracks the actual fonts -- no hardcoded ranges to drift.
//   - a codepoint the font genuinely can't draw (unsupported emoji, CJK) -> the
//     WHITE square "tofu" U+25A1 ( box ). Benign-missing.
//   - a control / zero-width / bidi-override char (a *hostile* spoofing char) ->
//     the BLACK square U+25A0, a deliberately DISTINCT shape, NEVER dropped
//     (hiding it hides the tampering). With recolor=true it gets a hot color.
// A single inline marker still can't say WHICH glyph was missing (box vs box) --
// that disambiguation is the per-codepoint detail view's job; cb_safe keeps the
// list scannable and tamper-evident. Returns a pointer into one of a few
// rotating static buffers (LVGL is single-threaded on the UI core), so a couple
// of calls in one expression won't clobber each other.
//
// LVGL 9.2 has no per-glyph label recolor (span-only), so colour is applied at
// ROW granularity instead: cb_safe sets cb_safe_had_hostile when it emitted any
// hostile marker, and make_name_label() paints the whole label in a warning
// colour. The □-vs-■ shape distinction is the always-on signal; the row colour
// is the enhancement where an lv_label is used (checkboxes/log get shape only).
static uint32_t cb_utf8_next(const char *s, size_t *i) {
    const unsigned char *u = (const unsigned char *)s;
    unsigned char c = u[*i];
    if (c < 0x80) { (*i)++; return c; }
    int n; uint32_t cp;
    if      ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; }
    else { (*i)++; return 0xFFFD; }                      // invalid lead byte
    for (int k = 1; k <= n; k++) {
        unsigned char cc = u[*i + k];
        if ((cc & 0xC0) != 0x80) { (*i)++; return 0xFFFD; }  // truncated
        cp = (cp << 6) | (cc & 0x3F);
    }
    *i += n + 1;
    return cp;
}

static bool cb_is_unsafe_cp(uint32_t cp) {
    if (cp < 0x20 || cp == 0x7F) return true;            // C0 controls / DEL
    if (cp >= 0x80 && cp <= 0x9F) return true;           // C1 controls
    if (cp >= 0x200B && cp <= 0x200F) return true;       // zero-width / LRM/RLM
    if (cp >= 0x202A && cp <= 0x202E) return true;       // bidi embeddings/overrides
    if (cp >= 0x2066 && cp <= 0x2069) return true;       // bidi isolates
    if (cp == 0xFEFF) return true;                        // BOM / zero-width no-break
    return false;
}

#define CB_TOFU_MISSING "\xE2\x96\xA1"          // U+25A1 WHITE SQUARE  (missing glyph)
#define CB_TOFU_HOSTILE  "\xE2\x96\xA0"          // U+25A0 BLACK SQUARE  (control/bidi)
#define CB_WARN_RGB   0xFF5555                 // warning colour for a flagged row

static bool cb_safe_had_hostile = false;          // set by the last cb_safe() call

static const char *cb_safe(const char *in,
                           const lv_font_t *font = &ui_font_pipboy_14) {
    static char bufs[4][192];
    static uint8_t turn = 0;
    char *out = bufs[turn];
    turn = (turn + 1) & 3;
    cb_safe_had_hostile = false;

    if (!in) { out[0] = '\0'; return out; }

    size_t i = 0;
    char *o = out;
    char *end = out + sizeof(bufs[0]) - 4;   // room for a 3-byte marker + NUL
    lv_font_glyph_dsc_t g;
    while (in[i] && o < end) {
        size_t prev = i;
        uint32_t cp = cb_utf8_next(in, &i);
        if (cb_is_unsafe_cp(cp)) {
            // control / zero-width / bidi -> black-square marker. NEVER dropped
            // (hiding it hides the tampering); flag the row so it can be coloured.
            const char *m = CB_TOFU_HOSTILE;
            while (*m) *o++ = *m++;
            cb_safe_had_hostile = true;
            continue;
        }
        // Real glyph only -- lv_font_get_glyph_dsc can resolve a MISSING
        // codepoint to the placeholder (tofu) box and still return true, so
        // reject is_placeholder or the box would survive.
        bool drawable = (cp == ' ') ||
                        (lv_font_get_glyph_dsc(font, &g, cp, 0) &&
                         !g.is_placeholder);
        if (drawable) {
            for (size_t k = prev; k < i; k++) *o++ = in[k];  // copy raw bytes
        } else {
            const char *m = CB_TOFU_MISSING;                 // white-square tofu
            while (*m) *o++ = *m++;
        }
    }
    *o = '\0';
    return out;
}

// Name label for untrusted text: paints the WHOLE row in a warning colour when
// the name contained any control/bidi hostile char (LVGL 9 has no per-glyph
// recolor). Use for lv_label name displays.
static lv_obj_t *make_name_label(lv_obj_t *parent, const char *raw,
                                 const lv_font_t *font, lv_color_t color) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, cb_safe(raw, font));
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl,
        cb_safe_had_hostile ? lv_color_hex(CB_WARN_RGB) : color, 0);
    return lbl;
}

#ifdef TEST_HARNESS
// ── Glyph / untrusted-name test screen (serial cmd "glyphtest") ────────────
// Renders a fixed set of edge-case names through the REAL display path
// (make_name_label) next to their per-codepoint U+XXXX dump -- the dump is the
// disambiguation layer that tells '⚠' (U+26A0) from '🤖' (U+1F916) when both
// render as the same tofu box. Non-ASCII is written with \u/\U escapes so the
// source stays pure ASCII.
static const char *cb_codepoints(const char *in) {
    static char buf[220];
    char *o = buf, *end = buf + sizeof(buf) - 10;
    size_t i = 0;
    while (in[i] && o < end) {
        uint32_t cp = cb_utf8_next(in, &i);
        o += snprintf(o, (size_t)(end - o), "U+%04X ", (unsigned)cp);
    }
    if (o == buf) *o++ = '-';
    *o = '\0';
    return buf;
}

static lv_obj_t *g_glyphtest_modal = nullptr;

static void glyphtest_row(lv_obj_t *parent, const char *cap, const char *raw) {
    make_label(parent, cap, &ui_font_pipboy_14, pip_dim());
    make_name_label(parent, raw, &ui_font_pipboy_16, pip_primary());
    lv_obj_t *cps = make_label(parent, cb_codepoints(raw), &ui_font_pipboy_14, pip_dim());
    lv_label_set_long_mode(cps, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(cps, lv_pct(100));
}

static void show_glyph_test(void) {
    if (g_glyphtest_modal) { lv_obj_delete(g_glyphtest_modal); g_glyphtest_modal = nullptr; }
    lv_obj_t *m = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(m);
    lv_obj_set_size(m, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(m, pip_bg(), 0);
    lv_obj_set_style_bg_opa(m, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(m, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(m, 6, 0);
    lv_obj_set_style_pad_row(m, 3, 0);
    lv_obj_set_scroll_dir(m, LV_DIR_VER);
    lv_obj_add_style(m, &style_scrollbar, LV_PART_SCROLLBAR);
    g_glyphtest_modal = m;

    make_label(m, "GLYPH TEST  (render / U+codepoints)", &ui_font_pipboy_16, pip_highlight());

    glyphtest_row(m, "native Latin/Cyrillic/Greek:",
                  "café Привет Ωμέγα");
    glyphtest_row(m, "supported emoji (fallback):",
                  "sig \U0001F4F6 lock \U0001F512 skull \U0001F480");
    glyphtest_row(m, "collision A (unsupported emoji):", "\U0001F9A0toXXXic");
    glyphtest_row(m, "collision B (unsupported emoji):", "\U0001F9E8toXXXic");
    glyphtest_row(m, "CJK (unsupported):", "日本語ID");
    glyphtest_row(m, "bidi RLO spoof (row reds):", "bad\u202Eexe");
    glyphtest_row(m, "zero-width spoof (row reds):", "zero\u200Bwidth");
    glyphtest_row(m, "literal middle-dot survives:", "real·dot");

    make_label(m, "coverage sample (Latin/Greek/Cyrillic/arrows/blocks):",
               &ui_font_pipboy_14, pip_dim());
    lv_obj_t *cov = make_name_label(m,
        "Aa1 Àéñü ΑαΩ ДяЖ "
        "←↑→↓ ■□█ ·°€",
        &ui_font_pipboy_16, pip_primary());
    lv_label_set_long_mode(cov, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(cov, lv_pct(100));

    make_label(m, "emoji subset (24):", &ui_font_pipboy_14, pip_dim());
    lv_obj_t *emo = make_name_label(m,
        "\U0001F4F6\U0001F4E1\U0001F310\U0001F512\U0001F513\U0001F511\U0001F4BB"
        "\U0001F916\U0001F480☠\U0001F47B\U0001F47D\U0001F608\U0001F525⚡"
        "\U0001F680⭐❤✨\U0001F4A9\U0001F389\U0001F431\U0001F984\U0001F44D",
        &ui_font_pipboy_16, pip_primary());
    lv_label_set_long_mode(emo, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(emo, lv_pct(100));

    lv_obj_t *btn = lv_button_create(m);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_set_style_bg_color(btn, pip_highlight(), 0);
    lv_obj_add_event_cb(btn, [](lv_event_t *e) {
        (void)e;
        if (g_glyphtest_modal) { lv_obj_delete(g_glyphtest_modal); g_glyphtest_modal = nullptr; }
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "Close");
    lv_obj_set_style_text_color(bl, pip_bg(), 0);
    lv_obj_set_style_text_font(bl, &ui_font_pipboy_16, 0);
    lv_obj_center(bl);

    Serial.println("[TEST] glyph test screen shown");
}
#endif  // TEST_HARNESS

// Themed ClipBoy mascot image - A8 alpha mask, recolor provides theme tint
// Black pixels are transparent, bright pixels become the theme color
static lv_obj_t* make_clipboy_image(lv_obj_t *parent) {
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, mascot_image());
    lv_obj_set_style_image_recolor(img, pip_primary(), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    return img;
}

typedef void (*list_tap_fn)(lv_event_t *e);

static lv_obj_t* make_list_btn(lv_obj_t *list, const char *text,
                               list_tap_fn cb, void *user_data) {
    lv_obj_t *btn = lv_list_add_button(list, NULL, text);
    lv_obj_remove_style_all(btn);
    lv_obj_add_style(btn, &style_list_btn, 0);
    lv_obj_add_style(btn, &style_list_btn_pressed, LV_STATE_PRESSED);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_set_height(btn, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *label = lv_obj_get_child(btn, 0);
    if (label) {
        lv_obj_set_style_text_font(label, &ui_font_pipboy_16, 0);
        lv_obj_set_style_text_color(label, pip_primary(), 0);
        lv_obj_set_flex_grow(label, 1);
        // Long labels marquee-scroll horizontally rather than wrapping to
        // a second line (keeps every list row one line tall). Short names
        // (most tools) don't move; long collectible titles scroll.
        lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    }
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    return btn;
}

// Disabled-looking list button (grayed out, still tappable for descriptions)
static lv_obj_t* make_list_btn_dim(lv_obj_t *list, const char *text,
                                   list_tap_fn cb, void *user_data) {
    lv_obj_t *btn = make_list_btn(list, text, cb, user_data);
    lv_obj_add_style(btn, &style_list_btn_disabled, 0);
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    if (label) lv_obj_set_style_text_color(label, pip_disabled(), 0);
    return btn;
}

static void highlight_list_item(lv_obj_t *list, int8_t index) {
    uint32_t cnt = lv_obj_get_child_count(list);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *child = lv_obj_get_child(list, (int32_t)i);
        if ((int32_t)i == index)
            lv_obj_add_style(child, &style_list_btn_selected, 0);
        else
            lv_obj_remove_style(child, &style_list_btn_selected, 0);
    }
}

static lv_obj_t* create_left_list(lv_obj_t *parent) {
    lv_obj_t *list = lv_list_create(parent);
    lv_obj_remove_style_all(list);
    lv_obj_add_style(list, &style_list_bg, 0);
    lv_obj_add_style(list, &style_scrollbar, LV_PART_SCROLLBAR);
    lv_obj_set_size(list, LEFT_PANE_W, lv_pct(100));
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    return list;
}

static lv_obj_t* create_right_detail(lv_obj_t *parent) {
    lv_obj_t *pane = lv_obj_create(parent);
    lv_obj_remove_style_all(pane);
    lv_obj_add_style(pane, &style_detail_panel, 0);
    lv_obj_set_size(pane, RIGHT_PANE_W, lv_pct(100));
    lv_obj_set_flex_flow(pane, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(pane, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(pane, 6, 0);
    lv_obj_add_style(pane, &style_scrollbar, LV_PART_SCROLLBAR);
    lv_obj_set_scroll_dir(pane, LV_DIR_VER);  // vertical only, no horizontal scroll
    return pane;
}

// Helper: create a settings row container
static lv_obj_t* make_settings_row(lv_obj_t *parent, lv_flex_align_t main_align = LV_FLEX_ALIGN_START) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_add_style(row, &style_container, 0);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, main_align,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row, 6, 0);
    lv_obj_set_style_pad_ver(row, 2, 0);
    lv_obj_set_style_border_color(row, pip_border(), 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    return row;
}

// ── DATA > Settings page: uniform two-column row layout ───────────────────
// Row inner width = 312 (content pad_all=4 → 320-8). Three row shapes:
//   Slider:   Label(132) + 6 + Slider(118) + 4 + Readout(44) + RightPad(8)
//   Dropdown: Label(132) + 6 + Dropdown(166)                 + RightPad(8)
//   Switch:   Label(132) + 6 + Spacer(126) + Switch(40)      + RightPad(8)
// Controls share a right edge so the column reads cleanly.
#define SETTINGS_LABEL_W   132
#define SETTINGS_SLIDER_W  118
#define SETTINGS_READOUT_W 44
#define SETTINGS_CONTROL_W 166  // dropdown width / pre-switch spacer + switch

static lv_obj_t* make_uniform_settings_row(lv_obj_t *parent) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_add_style(row, &style_container, 0);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row, 0, 0);
    lv_obj_set_style_pad_ver(row, 2, 0);
    lv_obj_set_style_pad_right(row, 8, 0);
    lv_obj_set_style_border_color(row, pip_border(), 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    return row;
}

static lv_obj_t* settings_gap(lv_obj_t *row, int w) {
    lv_obj_t *g = lv_obj_create(row);
    lv_obj_remove_style_all(g);
    lv_obj_set_size(g, w, 1);
    return g;
}

static lv_obj_t* settings_label(lv_obj_t *row, const char *text) {
    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &ui_font_pipboy_14, 0);
    lv_obj_set_style_text_color(lbl, pip_primary(), 0);
    lv_obj_set_width(lbl, SETTINGS_LABEL_W);
    return lbl;
}

static void settings_size_slider(lv_obj_t *sl) {
    lv_obj_set_size(sl, SETTINGS_SLIDER_W, 8);
    lv_obj_set_style_bg_color(sl, pip_border(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, pip_primary(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, pip_highlight(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(sl, 2, LV_PART_KNOB);
}

static lv_obj_t* settings_add_readout(lv_obj_t *row, int initial_pct) {
    settings_gap(row, 4);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", initial_pct);
    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, buf);
    lv_obj_set_style_text_font(lbl, &ui_font_pipboy_14, 0);
    lv_obj_set_style_text_color(lbl, pip_dim(), 0);
    lv_obj_set_width(lbl, SETTINGS_READOUT_W);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_RIGHT, 0);
    return lbl;
}

// Tactile tap sound: a single indev-level hook plays a click on LV_EVENT_PRESSED
// for ANY lv_button-class widget (action/small/list buttons, tabs, divisions,
// dialog + status-bar buttons). Installed once from setup() via
// ui_install_global_click_sound(), so individual buttons never need their own
// callback and newly added buttons can't forget it. Sliders/switches/dropdowns
// aren't button-class, so they intentionally stay silent.
static void ui_global_click_sound_cb(lv_event_t *e) {
    (void)e;
    lv_obj_t *obj = lv_indev_get_active_obj();
    if (!obj) return;
    // has_class (walks base_class chain), NOT check_type (exact match): list rows
    // are lv_list_button_class, which inherits from lv_button_class. Buttons,
    // switches, checkboxes and dropdowns all give a tap click; sliders are
    // deliberately excluded (their drag fires a continuous event stream).
    if (lv_obj_has_class(obj, &lv_button_class)   ||
        lv_obj_has_class(obj, &lv_switch_class)   ||
        lv_obj_has_class(obj, &lv_checkbox_class) ||
        lv_obj_has_class(obj, &lv_dropdown_class))
        audio_play_click();
}
static void ui_install_global_click_sound(void) {
    for (lv_indev_t *id = lv_indev_get_next(NULL); id; id = lv_indev_get_next(id)) {
        if (lv_indev_get_type(id) == LV_INDEV_TYPE_POINTER)
            lv_indev_add_event_cb(id, ui_global_click_sound_cb, LV_EVENT_PRESSED, NULL);
    }
}

// Helper: create a themed action button

static lv_obj_t* make_action_btn(lv_obj_t *parent, const char *text,
                                 lv_event_cb_t cb = NULL, void *user_data = NULL) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_add_style(btn, &style_action_btn, 0);
    lv_obj_add_style(btn, &style_action_btn_pressed, LV_STATE_PRESSED);
    lv_obj_set_style_text_font(btn, &ui_font_pipboy_16, 0);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    // Center the label so the text sits cleanly in narrow flex-row btns
    // (e.g. < Back / Tour, Scan / All-or-Found) without depending on the
    // button's default flex layout (which lv_obj_remove_style_all wipes).
    lv_obj_center(lbl);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    return btn;
}

// Helper: create a small secondary button (for scan/deselect/add etc.)
static lv_obj_t* make_small_btn(lv_obj_t *parent, const char *text,
                                lv_event_cb_t cb = NULL, void *user_data = NULL) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_add_style(btn, &style_list_btn, 0);
    lv_obj_add_style(btn, &style_list_btn_pressed, LV_STATE_PRESSED);
    lv_obj_set_height(btn, LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, pip_border(), 0);
    lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_FULL, 0);
    lv_obj_set_style_pad_ver(btn, 2, 0);
    lv_obj_set_style_pad_hor(btn, 6, 0);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &ui_font_pipboy_14, 0);
    lv_obj_set_style_text_color(lbl, pip_primary(), 0);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    return btn;
}

// ─────────────────────── STATUS BAR ───────────────────────────────────────

static void status_bar_timer_cb(lv_timer_t *timer) {
    (void)timer;
    if (!lbl_sbat || !lbl_smem) return;
    float v = BAT_Get_Volts();
    int pct = BAT_Get_Percentage(v);
    lv_label_set_text_fmt(lbl_sbat, "BAT %d%%", pct);
    uint32_t free_dram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
    lv_label_set_text_fmt(lbl_smem, "%luK", (unsigned long)free_dram);

    // Update task/state display from ClipBoy
    if (lbl_stask && cb_op_running && cb_op_name) {
        // Check if ClipBoy is still actually scanning
        if (!cb.isScanning()) {
            cb_op_running = false;
            cb_op_name = NULL;
            cb_op_encoded = -1;
            lv_label_set_text(lbl_stask, "");
        }
    }

    // WiFi connected icon - show/hide via text content
    if (lbl_swifi) {
        if (WiFi.status() == WL_CONNECTED)
            lv_label_set_text(lbl_swifi, LV_SYMBOL_WIFI);
        else
            lv_label_set_text(lbl_swifi, "");
    }
}

static void create_status_bar(lv_obj_t *parent) {
    status_bar = lv_obj_create(parent);
    lv_obj_remove_style_all(status_bar);
    lv_obj_add_style(status_bar, &style_status_bar, 0);
    lv_obj_set_size(status_bar, SCREEN_W, STATUS_BAR_H);
    lv_obj_set_pos(status_bar, 0, 0);
    lv_obj_remove_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lbl_sbat = lv_label_create(status_bar);
    lv_label_set_text(lbl_sbat, "BAT --%");
    lv_obj_set_style_text_font(lbl_sbat, &ui_font_pipboy_14, 0);
    lv_obj_set_style_text_color(lbl_sbat, pip_dim(), 0);

    lbl_stask = lv_label_create(status_bar);
    lv_label_set_text(lbl_stask, "");
    lv_obj_set_style_text_font(lbl_stask, &ui_font_pipboy_14, 0);
    lv_obj_set_style_text_color(lbl_stask, pip_accent(), 0);
    lv_obj_set_flex_grow(lbl_stask, 1);
    lv_obj_set_style_text_align(lbl_stask, LV_TEXT_ALIGN_CENTER, 0);
    // Tapping the running-task indicator jumps back to the running tool
    // (ITEMS > Tools) from anywhere. Only meaningful while a Marauder op runs.
    lv_obj_add_flag(lbl_stask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(lbl_stask, 6);   // easier touch target in the 16px bar
    lv_obj_add_event_cb(lbl_stask, [](lv_event_t *e) {
        (void)e;
        // lbl_stask is a label, not button-class, so the global click-sound
        // hook (PRESSED on button/switch/checkbox/dropdown) skips it -- play
        // the tap sound here so the jump feels like the rest of the UI.
        if (cb_op_running) { audio_play_click(); goto_div_tab(1, 0); }  // ITEMS > Tools
    }, LV_EVENT_CLICKED, NULL);

    // WiFi icon - right side, before memory label
    lbl_swifi = lv_label_create(status_bar);
    lv_label_set_text(lbl_swifi, "");
    lv_obj_set_style_text_font(lbl_swifi, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_swifi, pip_highlight(), 0);
    lv_obj_set_style_pad_right(lbl_swifi, 4, 0);  // small gap before help/flashlight

    // Help button (?) - tappable shortcut into the Help system from any screen.
    // Visibility is gated by cfg.help_btn (Settings > Help Button toggle).
    btn_shelp = lv_button_create(status_bar);
    lv_obj_remove_style_all(btn_shelp);
    lv_obj_set_height(btn_shelp, STATUS_BAR_H);
    lv_obj_set_width(btn_shelp, 18);
    lv_obj_set_style_pad_all(btn_shelp, 0, 0);
    lv_obj_set_style_pad_right(btn_shelp, 6, 0);
    lv_obj_set_style_bg_opa(btn_shelp, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(btn_shelp, LV_OPA_TRANSP, LV_STATE_PRESSED);
    lv_obj_t *lbl_shelp = lv_label_create(btn_shelp);
    lv_label_set_text(lbl_shelp, "?");
    lv_obj_set_style_text_font(lbl_shelp, &ui_font_pipboy_14, 0);
    lv_obj_set_style_text_color(lbl_shelp, pip_dim(), 0);
    lv_obj_center(lbl_shelp);
    if (!cfg.help_btn) lv_obj_add_flag(btn_shelp, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(btn_shelp, [](lv_event_t *e){
        (void)e;
        if (content_obj) show_help(content_obj);
    }, LV_EVENT_CLICKED, NULL);

    // Flashlight toggle - persistent, tappable from any screen
    btn_sflash = lv_button_create(status_bar);
    lv_obj_remove_style_all(btn_sflash);
    lv_obj_set_height(btn_sflash, STATUS_BAR_H);
    lv_obj_set_width(btn_sflash, 26);
    lv_obj_set_style_pad_all(btn_sflash, 0, 0);
    lv_obj_set_style_pad_right(btn_sflash, 6, 0);
    lv_obj_set_style_bg_opa(btn_sflash, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(btn_sflash, LV_OPA_TRANSP, LV_STATE_PRESSED);
    lbl_sflash = lv_label_create(btn_sflash);
    lv_label_set_text(lbl_sflash, "FL");
    lv_obj_set_style_text_font(lbl_sflash, &ui_font_pipboy_14, 0);
    lv_obj_center(lbl_sflash);
    // Paint from the ACTUAL flashlight state (see flashlight_ui_sync below), not from an
    // assumption about it -- this also keeps a theme rebuild correct.
    flashlight_ui_sync();
    lv_obj_add_event_cb(btn_sflash, [](lv_event_t *e){
        (void)e;
        neo_flashlight_toggle();
        flashlight_ui_sync();
        arg_secret_record(ARG_TOK_FLASH);   // secret-menu input
    }, LV_EVENT_CLICKED, NULL);

    lbl_smem = lv_label_create(status_bar);
    lv_label_set_text(lbl_smem, "--K");
    lv_obj_set_style_text_font(lbl_smem, &ui_font_pipboy_14, 0);
    lv_obj_set_style_text_color(lbl_smem, pip_dim(), 0);

    status_bar_timer_cb(NULL);
}

// ─────────────────────── TOOL DATA STRUCTURES ───────────────────────────

enum ToolActionType {
    TAT_SIMPLE,      // No config needed, just START/STOP
    TAT_AP,          // Needs AP scan + checkbox selection
    TAT_STA,         // Needs Station scan + checkbox selection
    TAT_SSID,        // Needs SSID list management
    TAT_TEXT,        // Needs text input
    TAT_FILE,        // Needs file selection
    TAT_IMMEDIATE,   // Execute immediately (no START, just EXECUTE)
    TAT_LIST_VIEW,   // Read-only list display
    TAT_CHANNEL,     // Channel dropdown (1-14 + Auto)
};

struct ToolItem {
    const char *name;
    const char *desc;
    ToolActionType type;
};

struct ToolCategory {
    uint8_t id;            // stable id (== array position in the Res34rch build).
                           // Survives the Sn34k array compaction when ACTIVE
                           // RESEARCH cats are gated out, so dispatch and the
                           // tool_info table (both keyed by id) stay correct.
    const char *name;
    const ToolItem *items;
    uint8_t count;
};

// ─────────────────────── DETECT-LED TAXONOMY (Jun 2026) ─────────────────────
// PASSIVE categories (ids 0-5) ship in BOTH SKUs. ACTIVE categories (ids 6-11)
// are a CONTIGUOUS TAIL gated behind CLIPBOY_RES34RCH so the Sn34k-Boy build never
// links a transmit primitive. Display order == array order; stable id ==
// array position in the Res34rch build (survives the Sn34k compaction).

// --- id0 Detect (passive recon) ---
static const ToolItem cat_detect[] = {
    { "AirTag",           "Detect Apple AirTags tracking nearby.",                           TAT_SIMPLE },
    { "Skimmer Check",    "Heuristic check for known BLE skimmer module names.",             TAT_SIMPLE },
    { "Flipper Zero",     "Detect Flipper Zero devices in range.",                           TAT_SIMPLE },
    // Renamed from "Flock Safety" 2026-07-27. The matcher keys on the Xuntong manufacturer ID
    // and the Penguin name shapes -- that is the BATTERY PACK some units carry, not the camera.
    // Solar units are typically BLE-silent, so the old name promised the camera and delivered an
    // accessory: a user could stand under a camera, read "0 Flock", and conclude they were not
    // being watched. The name is the first thing they see, so it carries the correction.
    { "Flock Batteries",  "SOME Flock camera systems' batteries are detectable here. Cameras "
                          "themselves usually are not -- see More Info.",                     TAT_SIMPLE },
    { "Rogue AP",         "Flag APs that answer probes for SSIDs they shouldn't know.",      TAT_SIMPLE },
    { "Evil Twin",        "Flag duplicate SSIDs across multiple BSSIDs (evil-twin signal).", TAT_SIMPLE },
    { "Remote ID",        "Detect drone Remote ID (ASTM F3411) broadcasts over Bluetooth.",  TAT_SIMPLE },
};

// --- id1 Scan ---
static const ToolItem cat_scan[] = {
    { "APs (full)",       "Full AP scan. Passive recon of all nearby access points.",        TAT_SIMPLE },
    { "APs + Stations",   "Scan APs and connected stations at the same time.",               TAT_SIMPLE },
    { "Stations",         "Scan for connected stations on nearby networks.",                 TAT_SIMPLE },
    { "BT Devices",       "Full Bluetooth plus BLE device scan.",                            TAT_SIMPLE },
    { "BLE Adverts",      "BLE advertisement counter. Packets only, no device details.",     TAT_SIMPLE },
};

// --- id2 Monitor ---
static const ToolItem cat_monitor[] = {
    { "Packets",          "Live packet count monitor across all channels.",                  TAT_SIMPLE },
    { "Packet Rate",      "Monitor packets/sec with rolling graph.",                         TAT_SIMPLE },
    { "RSSI",             "Monitor signal strength of selected AP over time.",               TAT_AP },
    { "Channel Stats",    "Histogram of channel utilization (1-14).",                        TAT_SIMPLE },
    { "MAC Tracker",      "Top talker MACs ranked by frame count.",                          TAT_SIMPLE },
};

// --- id3 Analyze (passive capture) ---
static const ToolItem cat_analyze[] = {
    { "Beacons",          "Capture beacon frames from nearby APs.",                          TAT_SIMPLE },
    { "Probes",           "Capture probe request frames from stations.",                     TAT_SIMPLE },
    { "Deauth",           "Sniff deauth packets in the area.",                               TAT_SIMPLE },
    { "Raw/PCAP",         "Raw WiFi packet capture to SD card.",                             TAT_SIMPLE },
    { "Pwnagotchi",       "Detect and log nearby Pwnagotchi devices.",                       TAT_SIMPLE },
    { "Espressif",        "Sniff for Espressif (ESP8266/ESP32) devices.",                    TAT_SIMPLE },
    { "SAE Commit",       "Sniff WPA3 SAE commit frames.",                                   TAT_SIMPLE },
    { "EAPOL/PMKID",      "Capture WPA handshakes for offline analysis. Select target AP.",  TAT_AP },
};

// --- id4 Utilities/Lists ---
static const ToolItem cat_utilities[] = {
    { "List APs",             "View scanned access points.",                                 TAT_LIST_VIEW },
    { "List SSIDs",           "View current SSID list.",                                     TAT_LIST_VIEW },
    { "List Stations",        "View scanned stations.",                                      TAT_LIST_VIEW },
    { "List BT Devices",      "View scanned Bluetooth devices.",                             TAT_LIST_VIEW },
    { "List AirTags",         "View detected AirTags.",                                      TAT_LIST_VIEW },
    { "List Flippers",        "View detected Flipper Zero devices.",                         TAT_LIST_VIEW },
    { "Saved Networks",       "View or manage saved WiFi networks (SSID and password).",     TAT_LIST_VIEW },
    { "Add SSID",             "Add a custom SSID to the SSID list.",                         TAT_TEXT },
    { "Gen Rnd SSIDs",        "Generate random SSIDs and add them to the list.",             TAT_IMMEDIATE },
    { "Select AP",            "Select an AP from scan results for targeting.",               TAT_AP },
    { "Clear All",            "Clear all scanned data (APs, stations, SSIDs).",              TAT_IMMEDIATE },
    { "Set Channel",          "Set WiFi monitor channel (1 to 14, or Auto).",                TAT_CHANNEL },
};

// --- id5 Network ---
static const ToolItem cat_network[] = {
    { "Join WiFi",            "Connect to a WiFi network (SSID and password).",              TAT_TEXT },
    { "Rnd AP MAC",           "Randomize the AP MAC address.",                               TAT_IMMEDIATE },
    { "Rnd STA MAC",          "Randomize the station MAC address.",                          TAT_IMMEDIATE },
};

#ifdef CLIPBOY_RES34RCH  // ─── ACTIVE RESEARCH tail (Res34rch-Boy only), ids 6-11 ───
// --- id6 Deauth --- (all entries active/transmitting)
static const ToolItem cat_deauth[] = {
    { "! Discovered",     "Deauth all clients from discovered access points.",               TAT_AP },
    { "! Manual",         "Manual deauth with AP selection.",                                TAT_AP },
    { "! Stations",       "Deauth specific stations from their networks.",                   TAT_STA },
};

// --- id7 Flood --- (all entries active/transmitting)
static const ToolItem cat_flood[] = {
    { "! Auth",           "Flood selected AP with authentication frames.",                   TAT_AP },
    { "! Bad Msg",        "Send malformed management frames (broadcast).",                    TAT_SIMPLE },
    { "! Bad Msg Target", "Send malformed mgmt frames to specific stations.",                TAT_STA },
    { "! Sleep",          "Broadcast power-save poll frames to all stations.",               TAT_SIMPLE },
    { "! Sleep Target",   "Send sleep frames to specific stations.",                         TAT_STA },
};

// --- id8 Beacon Spam --- (spectrum noise; un-prefixed = harmless beacon noise)
static const ToolItem cat_beacon_spam[] = {
    { "Random",           "Flood with random fake SSIDs.",                                   TAT_SIMPLE },
    { "List",             "Spam beacons from the SSID list.",                                TAT_SSID },
    { "AP Clone",         "Clone selected AP beacons with fake BSSIDs.",                     TAT_AP },
    { "Rick Roll",        "Never gonna give you up. Never gonna let you down.",              TAT_SIMPLE },
    { "Funny",            "Broadcast humorous SSIDs to nearby devices.",                     TAT_SIMPLE },
};

// --- id9 BLE Spam --- (all entries active/transmitting)
static const ToolItem cat_bt_spam[] = {
    { "! Sour Apple",     "Send spoofed Apple BLE notification packets.",                    TAT_SIMPLE },
    { "! Swiftpair",      "Spam Windows Swiftpair Bluetooth popups.",                        TAT_SIMPLE },
    { "! Samsung",        "Spam Samsung BLE pairing popups.",                                TAT_SIMPLE },
    { "! Google",         "Spam Google Fast Pair BLE popups.",                               TAT_SIMPLE },
    { "! Flipper",        "Spam Flipper Zero style BLE notifications.",                      TAT_SIMPLE },
    { "! All",            "Spam all BT types at once. May be slow to start - if it hangs, use a single type.", TAT_SIMPLE },
};

// --- id10 SAE ---
static const ToolItem cat_sae[] = {
    { "! Commit Flood",   "WPA3 SAE commit flood against selected AP.",                      TAT_AP },
};

// --- id11 Evil Portal --- ('!' marks the active phishing actions)
static const ToolItem cat_evil_portal[] = {
    { "! Start Default",      "Launch default captive portal for credential harvesting.",    TAT_SIMPLE },
    { "! Start Custom",       "Launch custom HTML portal from SD card.",                     TAT_FILE },
    { "Stop",                 "Stop the running evil portal.",                               TAT_IMMEDIATE },
};
#endif // CLIPBOY_RES34RCH

// --- Master category array ---
// `id` is the stable category identity (== position in the Res34rch build). In the
// Sn34k-Boy build the ACTIVE RESEARCH cats (the contiguous tail, ids 6-11) are
// compiled out so positions compact, but ids do not - dispatch_clipboy_action()
// and tool_info (both keyed by id) stay correct. NUM_TOOL_CATS is derived so it
// tracks the active SKU automatically.
static const ToolCategory tool_categories[] = {
    {  0, "Detect",          cat_detect,    sizeof(cat_detect)    / sizeof(ToolItem) },
    {  1, "Scan",            cat_scan,      sizeof(cat_scan)      / sizeof(ToolItem) },
    {  2, "Monitor",         cat_monitor,   sizeof(cat_monitor)   / sizeof(ToolItem) },
    {  3, "Analyze",         cat_analyze,   sizeof(cat_analyze)   / sizeof(ToolItem) },
    {  4, "Utilities/Lists", cat_utilities, sizeof(cat_utilities) / sizeof(ToolItem) },
    {  5, "Network",         cat_network,   sizeof(cat_network)   / sizeof(ToolItem) },
#ifdef CLIPBOY_RES34RCH  // ACTIVE RESEARCH tail (ids 6-11)
    {  6, "Deauth",          cat_deauth,      sizeof(cat_deauth)      / sizeof(ToolItem) },
    {  7, "Flood",           cat_flood,       sizeof(cat_flood)       / sizeof(ToolItem) },
    {  8, "Beacon Spam",     cat_beacon_spam, sizeof(cat_beacon_spam) / sizeof(ToolItem) },
    {  9, "BLE Spam",        cat_bt_spam,     sizeof(cat_bt_spam)     / sizeof(ToolItem) },
    { 10, "SAE",             cat_sae,         sizeof(cat_sae)         / sizeof(ToolItem) },
    { 11, "Evil Portal",     cat_evil_portal, sizeof(cat_evil_portal) / sizeof(ToolItem) },
#endif
};
#define NUM_TOOL_CATS ((uint8_t)(sizeof(tool_categories) / sizeof(tool_categories[0])))

// Returns true if this tool transmits or receives on WiFi or Bluetooth.
// List-view utilities just display already-collected scan data, and a few
// utilities only manipulate state (channel, SSID list) -- those don't need
// the radio so airplane mode shouldn't block them.
static inline bool tool_needs_radio(const ToolItem *wi) {
    if (wi->type == TAT_LIST_VIEW) return false;
    static const char * const non_radio[] = {
        "Add SSID", "Gen Rnd SSIDs", "Clear All", "Select AP", "Set Channel",
    };
    for (size_t i = 0; i < sizeof(non_radio) / sizeof(non_radio[0]); i++) {
        if (strcmp(wi->name, non_radio[i]) == 0) return false;
    }
    return true;
}

// Which radio does this tool use? Under the DETECT-LED taxonomy BT tools are no
// longer confined to whole categories - they're scattered across Detect, Scan,
// Utilities (list views), and BLE Spam. So we decide per-TOOL by name rather
// than per-category.
static inline bool tool_is_bluetooth(const ToolItem *wi) {
    if (!wi || !wi->name) return false;
    // ⚠ THIS TABLE IS KEYED BY THE TOOL'S DISPLAY NAME, so ANY rename in cat_*[] must be mirrored
    // here or the tool silently drops out of the Bluetooth set. That happened: the 2026-07-27
    // "Flock Safety" -> "Flock Batteries" rename missed this line, so tool_is_bluetooth() returned
    // false for Flock and the airplane-mode dialog told the user "WiFi is disabled" for a
    // Bluetooth-only tool -- on every shipped badge. Rename a tool, grep its old name.
    static const char * const bt_tools[] = {
        "AirTag", "Skimmer Check", "Flipper Zero", "Flock Batteries",
        "Remote ID",
        "BT Devices", "BLE Adverts",
        "List BT Devices", "List AirTags", "List Flippers",
#ifdef CLIPBOY_RES34RCH  // BLE Spam tools exist only in Res34rch; their names
        "! Sour Apple", "! Swiftpair", "! Samsung", "! Google", "! Flipper", "! All",
#endif                   // must not ship as strings in the Sn34k binary
    };
    for (size_t i = 0; i < sizeof(bt_tools) / sizeof(bt_tools[0]); i++) {
        if (strcmp(wi->name, bt_tools[i]) == 0) return true;
    }
    return false;
}
static inline const char* tool_radio_name(const ToolItem *wi) {
    return tool_is_bluetooth(wi) ? "Bluetooth" : "WiFi";
}

// Encode category index + item index into a single int for user_data
// Upper 8 bits = category, lower 8 bits = item
static inline int32_t tool_encode(uint8_t cat, uint8_t item) {
    return ((int32_t)cat << 8) | item;
}
static inline uint8_t tool_cat(int32_t encoded)  { return (uint8_t)(encoded >> 8); }
static inline uint8_t tool_item(int32_t encoded) { return (uint8_t)(encoded & 0xFF); }

// Which of the three live result stores a tool actually fills.
// ⚠ NEVER infer this from "whichever list is non-empty" -- the AP/STA/BT lists PERSIST
// across tool switches by design (dispatch_clipboy_action deliberately leaves AP/STA/SSID
// alone; only Utilities > Clear All wipes them). An AP-first "has data?" guess therefore
// shows the PREVIOUS scan's APs during a BT scan. Bug (Jul 2026): scan APs -> stop ->
// scan BT -> Live Devices table listed WiFi APs. Decide from the TOOL, not the data.
enum MonType { MON_AP, MON_STA, MON_BT };

static inline MonType tool_result_kind(int32_t encoded) {
    if (encoded < 0) return MON_AP;
    uint8_t pos = tool_cat(encoded), item = tool_item(encoded);
    if (pos >= NUM_TOOL_CATS) return MON_AP;
    uint8_t id = tool_categories[pos].id;
    const ToolItem *wi = (item < tool_categories[pos].count)
                         ? &tool_categories[pos].items[item] : NULL;
    // BT-radio tools (Scan > BT Devices, Detect > Skimmer/AirTag/Flipper/Flock, BLE spam)
    if (tool_is_bluetooth(wi)) return MON_BT;
    if (id == 1 && item == 2)  return MON_STA;   // Scan > Stations
    // Everything else that fills a list is an AP scan: Scan > APs (full) / APs + Stations,
    // Analyze > Beacons + Espressif (both run WIFI_SCAN_TARGET_AP under the hood).
    return MON_AP;
}

// The inline "Scan" buttons on the AP and station pickers start a scan with NO tool behind
// it (they leave cb_op_encoded = -1), so tool_result_kind() alone can't tell them apart and
// would report the AP default during a station scan. Remember what the button started.
// -1 = no manual scan in progress.
static int cb_manual_scan_kind = -1;

static inline MonType cb_active_result_kind(void) {
    if (cb_op_encoded >= 0)       return tool_result_kind(cb_op_encoded);
    if (cb_manual_scan_kind >= 0) return (MonType)cb_manual_scan_kind;
    return MON_AP;
}

// ─────────────────────── LIST ITEM DATA (non-tool) ──────────────────────

struct ListItem {
    const char *name;
    const char *desc;
};

// Collectible data is now loaded dynamically from ui_collectibles.h
// (coll_items[], coll_count)

static const ListItem sao_items[] = {
    { "RTX 7090Ti",      "16,535 CUDA cores in a 2x3. Requires 2.3 teratons of cooling to run Crysis above 15 FPS. Use may decrease battery life." },
    // NOT a troll SAO -- this one is real. Tapping it opens the radio (show_radio),
    // not the troll detail. Rendered ENABLED (the rest are dimmed). Sits near the
    // top (index 1) so players find it. Never tagged "secret": discovery IS the find.
    { "Whether Radio",   "Whether it's a radio or not, you'll never know. Drags 'broadcasts' out of the RF noise floor. Allegedly." },
    { "Quantum SAO",     "Collapses your badge state just by observing it. Schrodinger approved." },
    { "Coffee Maker",    "Brews espresso via I2C. Requires external water and 240V SAO header." },
    { "AI Girlfriend",   "She's very understanding. Runs on 4 bits of RAM. Still ghosted you." },
    { "Flux Capacitor",  "Requires 1.21 gigawatts. SAO header rated for 3.3V. Do the math." },
    { "Bitcoin Miner",   "Estimated earnings: $0.0000001/year. GPU not included." },
    { "Floppy Drive",    "1.44MB of pure nostalgia. Takes 47 disks to install this badge firmware." },
    { "Clippy SAO",      "It looks like you're installing an SAO! To install the Clippy SAO, first install the Clippy SAO. See also: the Clippy SAO." },
};
#define NUM_SAOS  (sizeof(sao_items) / sizeof(sao_items[0]))
#define RADIO_SAO_IDX  1   // the "Whether Radio" entry (slot #2) → show_radio

// Indexed by PHYSICAL LED index (matches cfg.leds[] / the WS2812B chain). The
// labels are intentionally NOT in index order: the UI names each LED by its
// physical position, but the WS2812B chain order differs.
//   Fuses: physical layout 0=BL 1=BR 2=TR 3=TL (see neopixel_driver.h). Labeled by
//   corner. So LED0="Fuse BL"  LED1="Fuse BR"  LED2="Fuse TR"  LED3="Fuse TL".
//   Fronts (idx 4-7): chain order is reversed vs physical placement, so the labels
//   run Front 4,3,2,1 over LEDs 4,5,6,7 -> they read Front 1->4 left to right.
// led_order below lists the fuses TL,BL,TR,BR (left column then right) and the
// fronts Front 1->4.
static const char *led_names[] = {
    "Fuse BL", "Fuse BR", "Fuse TR", "Fuse TL",    // fuse: idx0=BL 1=BR 2=TR 3=TL
    "Front 4", "Front 3", "Front 2", "Front 1",    // front reversed: idx4..7 = Front 4..1
    "Front 5",    // idx 8 - easter egg (no config slot)
    "All LEDs",   // idx 9 - presets only (no config slot)
};
#define NUM_LEDS  (sizeof(led_names) / sizeof(led_names[0]))

// ─────────────────────── CONTENT BUILDERS: STATS ──────────────────────────

// --- STATS > Status ---
#ifdef RADIO_PCM_TEST
// Radio PCM spike (DC34-129): double-tap "CLIP-BOY 3000" on the Status page to
// stream the test mu-law clip; double-tap again to stop. Validates the radio
// streaming-audio path + 11 kHz/8-bit fidelity. Asset is in radio_test_pcm.c.
extern "C" {
    extern const uint8_t  radio_test_pcm_ulaw[];
    extern const uint32_t radio_test_pcm_len;
    extern const uint32_t radio_test_pcm_rate;
}
static void radio_pcm_test_cb(lv_event_t *e) {
    (void)e;
    static uint32_t last = 0;
    uint32_t now = millis();
    if (now - last < 400) {                 // double-tap
        last = 0;
        if (aud_pcm_active || aud_lfs_active) audio_pcm_stop();
        else radio_pcm_test_lfs();           // seed-if-needed + stream from littlefs
    } else {
        last = now;
    }
}
#endif

static void build_stats_status(lv_obj_t *cont) {
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(cont, 8, 0);
    lv_obj_set_style_pad_gap(cont, 4, 0);
    lv_obj_add_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(cont, &style_scrollbar, LV_PART_SCROLLBAR);

    lv_obj_t *cb_title = make_label(cont, "CLIP-BOY 3000", &ui_font_pipboy_20, pip_highlight());
    (void)cb_title;
#ifdef RADIO_PCM_TEST
    lv_obj_add_flag(cb_title, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cb_title, radio_pcm_test_cb, LV_EVENT_CLICKED, NULL);
#endif

    // Count collected
    int found = 0;
    for (uint16_t i = 0; i < coll_count; i++)
        if (coll_items[i].collected) found++;

    char coll_buf[48];
    snprintf(coll_buf, sizeof(coll_buf), "Collectibles: %d / %d", found, coll_count);
    make_label(cont, coll_buf, &ui_font_pipboy_16, pip_primary());

    // Compute stat rollup from all collected items
    coll_compute_rollup();

    if (coll_rollup_count > 0) {
        // Thin separator
        lv_obj_t *sep = lv_obj_create(cont);
        lv_obj_remove_style_all(sep);
        lv_obj_set_size(sep, lv_pct(100), 1);
        lv_obj_set_style_bg_color(sep, pip_border(), 0);
        lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);

        make_label(cont, "STAT BONUSES", &ui_font_pipboy_16, pip_highlight());

        for (uint16_t s = 0; s < coll_rollup_count; s++) {
            char stat_buf[96];
            int16_t v = coll_rollup[s].total;
            if (v == 99 || v == -99 || v > 99 || v < -99) {
                snprintf(stat_buf, sizeof(stat_buf), "%sINF %s",
                         v > 0 ? "+" : "-", coll_rollup[s].stat);
            } else {
                snprintf(stat_buf, sizeof(stat_buf), "%+d %s",
                         v, coll_rollup[s].stat);
            }
            // Space Badge: semantic +blue / -red stat values. Other themes keep
            // the bright(highlight) / muted(dim) treatment.
            lv_color_t stat_col = (cur_theme_idx == THEME_SPACE_BADGE)
                ? (v > 0 ? pip_border() : pip_accent())
                : (v > 0 ? pip_highlight() : pip_dim());
            lv_obj_t *sl = make_label(cont, stat_buf, &ui_font_pipboy_14, stat_col);
            lv_label_set_long_mode(sl, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(sl, lv_pct(100));
        }
    } else {
        // One-line spacer
        lv_obj_t *sp = lv_obj_create(cont);
        lv_obj_remove_style_all(sp);
        lv_obj_set_size(sp, 1, 6);

        // No stats yet - mascot left, hint right
        lv_obj_t *row = lv_obj_create(cont);
        lv_obj_remove_style_all(row);
        lv_obj_add_style(row, &style_container, 0);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_flex_grow(row, 1);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(row, 0, 0);

        lv_obj_t *mascot = make_clipboy_image(row);
        lv_image_set_scale(mascot, 102);  // ~40%
        lv_obj_set_style_translate_x(mascot, -27, 0);  // nudge left into transparent padding

        lv_obj_t *msg = make_label(row,
            "It looks like you're\nsurviving the digital\napocalypse.\n\nWould you like help?",
            &ui_font_pipboy_14, pip_primary());
        lv_obj_set_flex_grow(msg, 1);
        lv_obj_set_style_translate_x(msg, -40, 0);  // reclaim the space
        lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    }
}

// --- STATS > L.E.E.T. ---
static void build_stats_leet(lv_obj_t *cont) {
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, 6, 0);
    lv_obj_set_style_pad_gap(cont, 2, 0);
    lv_obj_add_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(cont, &style_scrollbar, LV_PART_SCROLLBAR);

    // Tab header already says "L.E.E.T." -- the four rows below spell it out
    // letter by letter so a page title is redundant and pushes 'T' off-screen.

    static const char *letters[] = { "L", "E", "E", "T" };
    static const char *names[]   = { "cpu Load", "mEmory", "storagE", "upTime" };

    for (int i = 0; i < 4; i++) {
        lv_obj_t *row = lv_obj_create(cont);
        lv_obj_remove_style_all(row);
        lv_obj_add_style(row, &style_container, 0);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(row, 8, 0);
        lv_obj_set_style_pad_ver(row, 4, 0);
        lv_obj_set_style_border_color(row, pip_border(), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);

        make_label(row, letters[i], &ui_font_pipboy_20, pip_highlight());

        lv_obj_t *col = lv_obj_create(row);
        lv_obj_remove_style_all(col);
        lv_obj_add_style(col, &style_container, 0);
        lv_obj_set_flex_grow(col, 1);
        lv_obj_set_height(col, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);

        make_label(col, names[i], &ui_font_pipboy_16, pip_primary());

        char val[48];
        switch (i) {
            case 0: snprintf(val, sizeof(val), "~%d%%", rand() % 30 + 10); break;
            case 1: {
                uint32_t fd = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
                uint32_t fp = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024;
                snprintf(val, sizeof(val), "DRAM %luK  PSRAM %luK",
                         (unsigned long)fd, (unsigned long)fp);
            } break;
            case 2: snprintf(val, sizeof(val), "16384K flash"); break;
            case 3: {
                unsigned long up = millis() / 1000;
                snprintf(val, sizeof(val), "%lum %lus", up / 60, up % 60);
            } break;
        }
        make_label(col, val, &ui_font_pipboy_14, pip_dim());
    }

    lv_obj_t *brow = lv_obj_create(cont);
    lv_obj_remove_style_all(brow);
    lv_obj_add_style(brow, &style_container, 0);
    lv_obj_set_size(brow, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(brow, 4, 0);

    float v = BAT_Get_Volts();
    int pct = BAT_Get_Percentage(v);
    char bbuf[48];
    snprintf(bbuf, sizeof(bbuf), "Battery: %d%% (%.2fV)", pct, v);
    make_label(brow, bbuf, &ui_font_pipboy_14, pip_dim());

    // Per-badge "issued callsign" = the P3 passphrase, hidden in plain sight as a
    // flavor stat. Shows on EVERY badge so it never outs the ARG; it's where Queue
    // sends a stuck player ("stamped on its L.E.E.T. sheet").
    lv_obj_t *crow = lv_obj_create(cont);
    lv_obj_remove_style_all(crow);
    lv_obj_add_style(crow, &style_container, 0);
    lv_obj_set_size(crow, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(crow, 4, 0);
    char csw[8]; p3_passphrase(csw);
    char csbuf[40]; snprintf(csbuf, sizeof(csbuf), "Issued callsign: %s", csw);
    make_label(crow, csbuf, &ui_font_pipboy_14, pip_dim());
}

// --- STATS > Radiation ---
static bool rad_geiger_active       = false;
static bool rad_geiger_audio_on     = false;  // tracks whether synth is running

// Drive the synthesized tick generator directly from the measured deauth rate  - 
// no MP3 segments, no decoder artifacts, clicks/sec == deauths/sec exactly.
static void rad_geiger_audio_update(int rate) {
    if (!rad_geiger_active || !cfg.sound) {
        if (rad_geiger_audio_on) {
            audio_geiger_stop();
            rad_geiger_audio_on = false;
        }
        return;
    }
    if (!rad_geiger_audio_on) {
        audio_geiger_start();
        rad_geiger_audio_on = true;
    }
    audio_geiger_set_rate(rate);
}

// ─── Geiger counter state (persists across navigation) ──────────────────
extern "C" { LV_IMAGE_DECLARE(nuclear_60x60); }

static lv_obj_t   *rad_scale      = NULL;  // lv_scale widget
static lv_obj_t   *rad_needle     = NULL;  // current value needle (lv_line)
static lv_obj_t   *rad_max_needle = NULL;  // max value needle (lv_line)
static lv_obj_t   *rad_val_lbl    = NULL;  // "Deauths/Second" label + mode line
// Declared up here because rad_poll_cb (below) sets the off-scale marker, but the helpers
// that render it live with the rest of the channel-policy code further down.
static bool        rad_off_scale  = false; // true while rate > 100, i.e. the needle is pegged
static void rad_refresh_mode_line(void);
static void rad_reset_stats(void);
static void rad_update_status_task(void);   // status-bar mode + rate; called from rad_poll_cb
static lv_obj_t   *rad_max_lbl    = NULL;
static lv_obj_t   *rad_topch_lbl  = NULL;
static lv_obj_t   *rad_topbss_lbl = NULL;
static lv_obj_t   *rad_timer_lbl  = NULL;
static lv_obj_t   *rad_toggle_btn = NULL;
static lv_timer_t *rad_timer      = NULL;
static uint32_t    rad_last_deauth = 0;
static int         rad_display_val = 0;  // smoothed gauge value
static int         rad_current_rate = 0; // actual rate this tick

// Persistent stats - survive navigation
static int         rad_max_rate    = 0;
static uint8_t     rad_top_channel = 0;
static char        rad_top_bssid[18] = "";
static uint32_t    rad_start_time  = 0;
static uint32_t    rad_total_deauths = 0;
static uint8_t     rad_ch_counts[15] = {};

// Red style for >100 needle
static lv_style_t rad_style_needle_red;
static bool rad_style_needle_red_init = false;

static void rad_poll_cb(lv_timer_t *t) {
    (void)t;
    if (!rad_geiger_active) return;

    CBPacketCounters pc = cb.getPacketCounters();
    uint32_t now_deauths = pc.deauthFrames;
    int rate = (int)(now_deauths - rad_last_deauth);
    rad_last_deauth = now_deauths;
    if (rate < 0) rate = 0;
    rad_current_rate = rate;
    rad_update_status_task();   // mode + rate, readable from every screen

    // Update persistent stats
    rad_total_deauths += rate;
    if (rate > rad_max_rate) rad_max_rate = rate;

    // Channel stats = distribution of the events CURRENTLY in the ring, recomputed each
    // tick. This used to ACCUMULATE: every second it re-added all <=32 retained events to
    // the running totals, so a single deauth was counted once per second for as long as it
    // stayed in the ring -- inflating counts ~1 ring/sec until this uint8_t wrapped at 256
    // and "Top ch" became noise. Recomputing keeps it honest ("busiest channel among the
    // last 32 deauths") and self-heals as the ring turns over.
    int ring_count = cb.getDeauthEventCount();
    memset(rad_ch_counts, 0, sizeof(rad_ch_counts));
    for (int i = 0; i < ring_count; i++) {
        CBDeauthEvent ev;
        if (cb.getDeauthEvent(i, ev)) {
            if (ev.channel >= 1 && ev.channel <= 14 && rad_ch_counts[ev.channel] < 255)
                rad_ch_counts[ev.channel]++;
        }
    }
    uint8_t best_ch = 0; int best_ch_cnt = 0;
    for (int c = 1; c <= 14; c++) {
        if (rad_ch_counts[c] > best_ch_cnt) {
            best_ch_cnt = rad_ch_counts[c];
            best_ch = c;
        }
    }
    if (best_ch > 0) rad_top_channel = best_ch;

    if (ring_count > 0) {
        CBDeauthEvent latest;
        if (cb.getDeauthEvent(ring_count - 1, latest))
            strncpy(rad_top_bssid, latest.src, 17);
    }

    // Animate gauge - jump up, decay down 30%/sec
    int capped = (rate > 100) ? 100 : rate;
    // Surface the saturation the needle hides. Only repaint on a CHANGE: this runs at 1Hz and
    // lv_label_set_text_fmt reallocates, so an unconditional rewrite would churn the heap for
    // a string that is identical 99% of ticks.
    if ((rate > 100) != rad_off_scale) { rad_off_scale = (rate > 100); rad_refresh_mode_line(); }
    if (capped >= rad_display_val) {
        rad_display_val = capped;
    } else {
        rad_display_val = rad_display_val * 7 / 10;
        if (rad_display_val < capped) rad_display_val = capped;
    }

    // Drive tick generator from the smoothed gauge value - instant-on when
    // a deauth burst arrives, 30%/sec fade when activity stops. Audio feel
    // matches the needle so bursts don't vanish between 1s poll boundaries.
    rad_geiger_audio_update(rad_display_val);

    // Update arcs
    if (rad_needle) {
        lv_arc_set_value(rad_needle, rad_display_val);
        // Red arc when rate > 100
        if (rate > 100)
            lv_obj_set_style_arc_color(rad_needle, lv_color_hex(0xFF0000), LV_PART_INDICATOR);
        else
            lv_obj_set_style_arc_color(rad_needle, pip_primary(), LV_PART_INDICATOR);
    }
    int max_capped = (rad_max_rate > 100) ? 100 : rad_max_rate;
    if (rad_max_needle)
        lv_arc_set_value(rad_max_needle, max_capped);

    // Update stat labels if visible
    if (rad_max_lbl)
        lv_label_set_text_fmt(rad_max_lbl, "Max: %d/s", rad_max_rate);
    if (rad_topch_lbl)
        lv_label_set_text_fmt(rad_topch_lbl, "Top ch: %d", rad_top_channel);
    if (rad_topbss_lbl) {
        if (rad_top_bssid[0])
            lv_label_set_text(rad_topbss_lbl, rad_top_bssid);
        else
            lv_label_set_text(rad_topbss_lbl, "---");
    }
    if (rad_timer_lbl && rad_start_time > 0) {
        uint32_t elapsed = (millis() - rad_start_time) / 1000;
        uint32_t h = elapsed / 3600, m = (elapsed % 3600) / 60, s = elapsed % 60;
        lv_label_set_text_fmt(rad_timer_lbl, "%02lu:%02lu:%02lu",
            (unsigned long)h, (unsigned long)m, (unsigned long)s);
    }
}

// ── Deauth channel policy: shared between the Radiation gauge and Tools > Analyze > Deauth ──
//
// ONE setting, not one per page: both surfaces (and the CLI `sniffdeauth`) run the same
// WIFI_SCAN_DEAUTH scan and only one can run at a time, so per-page settings would let the UI
// disagree with the radio -- the stale-result-store shape this project has shipped five times.
//
// The dropdown INDEX is never stored. cfg.deauth_chan holds the semantic value (0 / 200 /
// 1-14); these two helpers map it to and from the list position, so a future reorder of the
// options cannot silently repoint everyone's saved channel.
// ORDER MATTERS FOR DISCOVERABILITY: the default must be the FIRST entry. This list is
// taller than the screen, and LVGL opens a dropdown scrolled to the selected item -- so with
// "1-14" above the default, that option sat off-screen above the fold and a user would never
// know it existed unless they went hunting. Default first means everything else is below it.
// (Reordering is free precisely because cfg stores the SEMANTIC value, not the list index --
// which is why that choice was made. Both mapping helpers must move together.)
#define CB_DEAUTH_DD_OPTS "1/6/11\n1-14\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14"

static_assert(CB_DEAUTH_HOP_TRI == 200,
              "CFG_DEF_DEAUTH_CHAN/ui_config.h hardcodes 200; keep it equal to CB_DEAUTH_HOP_TRI");

static uint8_t cb_deauth_idx_to_mode(uint16_t idx) {
    if (idx == 0) return CB_DEAUTH_HOP_TRI;   // list order: default first (see CB_DEAUTH_DD_OPTS)
    if (idx == 1) return CB_DEAUTH_HOP_ALL;
    return (uint8_t)(idx - 1);            // idx 2 -> ch 1 ... idx 15 -> ch 14
}
static uint16_t cb_deauth_mode_to_idx(uint8_t mode) {
    if (mode == CB_DEAUTH_HOP_TRI) return 0;
    if (mode == CB_DEAUTH_HOP_ALL) return 1;
    if (mode >= 1 && mode <= 14)   return (uint16_t)(mode + 1);
    return 0;                             // unknown -> show the default rather than lie
}

// The mode as the user needs to read it. "Sampling" is doing the work here: it tells a
// first-time reader the number is a SAMPLE, not a total, without a word of RF theory.
static const char *cb_deauth_mode_text(uint8_t mode) {
    if (mode == CB_DEAUTH_HOP_ALL) return "sampling all 14 channels";
    if (mode == CB_DEAUTH_HOP_TRI) return "sampling ch 1, 6, 11";
    static char buf[16];
    snprintf(buf, sizeof(buf), "ch %u only", (unsigned)mode);
    return buf;
}

// Applies + persists. Shared by BOTH selectors -- STATS > Radiation and Tools > Analyze >
// Deauth -- so the two can never diverge. (This comment previously asserted two call sites
// while only one existed: the Analyze selector was specified but not built, which also left
// cb_deauth_mode_text() dead and the word "sampling" nowhere on the device.)
static void cb_deauth_dd_cb(lv_event_t *e) {
    lv_obj_t *dd = (lv_obj_t *)lv_event_get_target(e);
    // Optional long-form caption (Analyze > Deauth passes one; the gauge page does not).
    // Passed as user_data rather than held in a file-scope pointer: it is a sibling of the
    // dropdown and dies with it, so this callback can never outlive it.
    lv_obj_t *cap = (lv_obj_t *)lv_event_get_user_data(e);
    uint8_t mode = cb_deauth_idx_to_mode(lv_dropdown_get_selected(dd));
    if (mode == cfg.deauth_chan) return;
    cfg.deauth_chan = mode;
    cfg_save_deauth_chan();
    cb.setDeauthChannel(mode);     // live re-tune if a deauth scan is running
    // Owner decision: changing mode mid-run resets the per-channel-set stats, because carrying
    // Max/Top across a channel change mixes two different populations. The Analyze page's event
    // LOG is deliberately kept -- it is a scrolling history, not a live aggregate.
    //
    // Mirror the RESET BUTTON, not just rad_reset_stats(). Two gaps otherwise:
    //  - with the Geiger STOPPED no poll runs, so the internals zero but the LABELS keep
    //    showing the previous policy's "Max: 52/s / Top ch: 7 / <bssid>";
    //  - without clearDeauthEvents() the poller re-derives "Top ch" from the retained 32-event
    //    ring, so after switching to ch11 the page keeps asserting "Top ch: 6" from the channel
    //    set we just left. rad_geiger_start() clears the ring for exactly this reason.
    // ⚠ rad_reset_stats() also restarts the elapsed timer when one is running -- intended
    //   (the timer measures THIS channel set), but worth stating since it is not obvious.
    cb.clearDeauthEvents();
    rad_reset_stats();
    if (rad_needle)     lv_arc_set_value(rad_needle, 0);
    if (rad_max_needle) lv_arc_set_value(rad_max_needle, 0);
    if (rad_max_lbl)    lv_label_set_text(rad_max_lbl, "Max: 0/s");
    if (rad_topch_lbl)  lv_label_set_text(rad_topch_lbl, "Top ch: --");
    if (rad_topbss_lbl) lv_label_set_text(rad_topbss_lbl, "---");
    if (cap) lv_label_set_text(cap, cb_deauth_mode_text(mode));
    rad_refresh_mode_line();
    rad_update_status_task();
}

// `off_scale` appends "100+" because the NEEDLE saturates at 100 (rad_poll_cb caps it) while
// the real rate keeps climbing -- without it a pegged needle and a 400/s flood look identical.
// Deliberately NOT trying to signal the ~250/s processing ceiling: that number is measured,
// moves with PCAP state, and would be a lie baked into the UI. It goes in Help instead.
// Short mode token for the STATUS BAR, which is readable from all 9 screens.
// This is the surface that actually carries the always-visible requirement the NVS decision
// rests on: the Radiation page has the selector, but a user who starts the Geiger and then
// wanders the badge sees only this. Without the mode here, a locked badge reads a plausible
// low number everywhere with nothing saying it is listening to one channel.
static const char *cb_deauth_mode_short(uint8_t mode) {
    if (mode == CB_DEAUTH_HOP_ALL) return "1-14";
    if (mode == CB_DEAUTH_HOP_TRI) return "1/6/11";
    static char b[8];
    snprintf(b, sizeof(b), "ch%u", (unsigned)mode);
    return b;
}

// Status-bar text for a running Geiger: mode + live rate. Rate included because the mode
// alone cannot tell you whether a quiet reading means "quiet" or "wrong channel" -- seeing
// both together is what makes the number interpretable away from the gauge.
static void rad_update_status_task(void) {
    if (!lbl_stask || !rad_geiger_active) return;
    lv_label_set_text_fmt(lbl_stask, "Geiger %s %d/s",
                          cb_deauth_mode_short(cfg.deauth_chan), rad_current_rate);
}

static void rad_refresh_mode_line(void) {
    if (!rad_val_lbl) return;
    // SINGLE line. The mode used to be a second line here and it collided with the 0/100 tick
    // labels and the division bar -- the one band of this column that is already full. The
    // selector sits permanently on this page reading "1/6/11" / "ch 6", so a mode line
    // underneath only duplicated it. Where the mode is genuinely NOT visible is the status bar
    // (readable from all 9 screens) and the Analyze page; those are handled there, not here.
    lv_label_set_text_fmt(rad_val_lbl, "Deauths/sec%s", rad_off_scale ? " 100+" : "");
    rad_update_status_task();
}

static void rad_reset_stats(void) {
    rad_max_rate = 0;
    rad_top_channel = 0;
    rad_top_bssid[0] = '\0';
    rad_total_deauths = 0;
    rad_display_val = 0;
    rad_off_scale = false;   // else '100+' latches on forever
    rad_current_rate = 0;
    memset(rad_ch_counts, 0, sizeof(rad_ch_counts));
    if (rad_start_time > 0) rad_start_time = millis();
}

static void show_airplane_block_dialog(const char *radio_name);   // defined ~:5270

// ── rad_geiger_start: the ONE way to START the Geiger ───────────────────────
// Mirrors rad_geiger_force_stop()'s single-helper design, and for the same reason. This body
// used to exist TWICE -- here and hand-copied into th_cmd_geiger_start() -- and the copies had
// already drifted once (the harness copy carries a "match rad_toggle_cb" comment from when
// cb.clearDeauthEvents() had to be back-ported into it). A duplicated start path means every
// future change to Geiger startup silently applies to the UI but not to the tests, so the
// tests stop exercising what ships. theremin_enable() already solved this the same way: one
// function, both callers.
//
// Returns false if the start was REFUSED, so the caller can surface it however suits it
// (the UI shows a dialog, the harness returns an error).
static bool rad_geiger_start(void) {
    // F7 -- airplane gate. tool_tap_cb has gated radio TOOLS since airplane mode shipped
    // (:5327), but this gauge bypassed it entirely: cb_ensure_wifi() below self-suppresses on
    // airplane, which made it LOOK considered, and then cb.sniffDeauth() runs anyway and
    // reaches esp_wifi_start() + esp_wifi_set_promiscuous(true) in WiFiScan.cpp -- IDF calls
    // that know nothing about cfg.airplane. So the badge sat in promiscuous RX while the
    // Airplane switch read engaged: the switch itself was the thing lying.
    // Checked FIRST, before any teardown below, so a refused start cannot still have killed
    // the user's theremin or running tool on its way out.
    if (cfg.airplane) {
        CB_LOGLN("[RAD] Geiger blocked by airplane mode");
        return false;
    }
    if (cb_op_running) cb_stop_operation();
    stop_lidar_activities();   // release HR scan + theremin (share VL53L5CX)
    if (WiFi.status() == WL_CONNECTED) {
        WiFi.disconnect(false);
        wifi_join_ssid[0] = '\0';
    }
    cb_ensure_wifi();
    // Wipe the shared deauth ring. It's written ONLY by the WIFI_SCAN_DEAUTH sniffer --
    // shared by this gauge and Tools > Analyze > Deauth -- and cleared ONLY here and in
    // cb_tool_start_stop_cb; neither resetDisplayAccumulators() nor StopScan() touch it.
    // Without this, starting the Geiger right after an Analyze > Deauth run showed that
    // run's channel + source BSSID as if they'd just been detected.
    cb.clearDeauthEvents();
    cb.sniffDeauth();
    rad_geiger_active = true;
    rad_last_deauth = cb.getPacketCounters().deauthFrames;
    rad_start_time = millis();
    rad_reset_stats();
    rad_geiger_audio_update(0);  // Start at low level
    if (lbl_stask) lv_label_set_text_fmt(lbl_stask, "Geiger %s 0/s",
                                     cb_deauth_mode_short(cfg.deauth_chan));
    if (rad_timer) lv_timer_delete(rad_timer);
    rad_timer = lv_timer_create(rad_poll_cb, 1000, NULL);
    return true;
}

static void rad_toggle_cb(lv_event_t *e) {
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    const char *cur = lv_label_get_text(lbl);
    if (cur[0] == 'S' && cur[1] == 't' && cur[2] == 'a') {
        if (!rad_geiger_start()) {
            show_airplane_block_dialog("WiFi");
            return;                      // label stays "Start" -- nothing was started
        }
        lv_label_set_text(lbl, "Stop");
    } else {
        cb.stopScan();
        rad_geiger_active = false;
        rad_geiger_audio_update(0);  // Stops audio (rad_geiger_active=false)
        rad_start_time = 0;
        if (rad_timer) { lv_timer_delete(rad_timer); rad_timer = NULL; }
        rad_display_val = 0;
        rad_off_scale = false;   // else '100+' latches on forever
        if (rad_needle) lv_arc_set_value(rad_needle, 0);
        lv_label_set_text(lbl, "Start");
        if (lbl_stask) lv_label_set_text(lbl_stask, "");
    }
}

// ── rad_geiger_force_stop: the ONE way to stop the Geiger ────────────────────
// Audit FB11+FB12. `rad_geiger_active = false` appeared at NINE sites and only the
// Radiation Stop button (above) did the full teardown. The others each missed something:
//
//  FB11 -- six sites cleared the flag WITHOUT calling rad_geiger_audio_update(0), which is
//  the only route to audio_geiger_stop(). Its periodic caller bails on !rad_geiger_active,
//  so `aud_geiger_active` latched TRUE for the session. That branch sits ABOVE
//  `aud_tone_active` in the core-0 fixed-priority chain (audio_driver.h:704 vs :712), so the
//  screensaver tap-and-hold rising tone AND the unlock chime were dead -- while the hold
//  still unlocked, silently. Clicking also continued if the frozen rate was nonzero: silent
//  on a quiet bench (which is why it shipped), audible at DEFCON.
//
//  FB12 -- cb_stop_operation()'s geiger branch never called cb.stopScan(), because
//  cb.stopScan() lives inside its `if (cb_op_running)` block and rad_toggle_cb never sets
//  cb_op_running. So the WIFI_SCAN_DEAUTH sniffer kept running with NO consumer, reachable
//  from theremin_enable / hr_scan_start / airplane_cb. That callback is not idle: it does
//  delay(random(0,10)) plus blocking Serial.print PER DEAUTH FRAME on the WiFi task -- the
//  documented CDC-block -> WDT-reboot hazard, from a path nobody associates with the radio.
//  (theremin_enable's own comment claims it "stops tool + geiger" -- it did not.)
//
// All three panels insisted on ONE helper rather than nine patches, because the class had
// already regenerated once: Dark Charge called audio_geiger_stop() raw, leaving
// rad_geiger_audio_on true, so the NEXT Radiation Start was silent.
static void rad_geiger_force_stop(void) {
    if (!rad_geiger_active && !rad_timer && !rad_geiger_audio_on) return;   // already down
    bool was_active = rad_geiger_active;
    rad_geiger_active = false;
    rad_geiger_audio_update(0);      // sees !rad_geiger_active -> stops audio, clears the latch
    rad_start_time = 0;
    rad_display_val = 0;
    rad_off_scale = false;   // else '100+' latches on forever
    if (rad_timer) { lv_timer_delete(rad_timer); rad_timer = NULL; }
    if (rad_needle) lv_arc_set_value(rad_needle, 0);
    if (rad_toggle_btn) {
        lv_obj_t *bl = lv_obj_get_child(rad_toggle_btn, 0);
        if (bl) lv_label_set_text(bl, "Start");
    }
    // Release the sniffer ONLY if the Geiger was what owned it. A running tool sets
    // cb_op_running and owns the radio itself, so stopping the scan here would kill it.
    if (was_active && !cb_op_running) cb.stopScan();
    // F3 -- clear the status-bar label too. This helper resets the needle, the button, the
    // timer and the audio, but never the text, so "Geiger active" (set at :1745) stuck on
    // EVERY screen, permanently, via three routes: Airplane ON, Dark Charge -> wake, and the
    // PCAP-gated early return in the Raw/PCAP handler. cb_stop_operation() does clear the
    // label, but at a site INSIDE its `if (cb_op_running)` block while it calls this helper
    // from OUTSIDE it -- and rad_toggle_cb never sets cb_op_running, so that clear (and the
    // self-heal in status_bar_timer_cb, gated on the same flag) could never fire for the
    // Geiger. Same ownership condition as the sniffer release above: only wipe the label if
    // the Geiger is what put text there, otherwise a tool-stop path would erase a live tool's
    // status on its way past.
    if (was_active && !cb_op_running && lbl_stask) lv_label_set_text(lbl_stask, "");
}

static void build_stats_radiation(lv_obj_t *cont) {
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(cont, 2, 0);
    lv_obj_set_style_pad_gap(cont, 0, 0);

    // ── Left: Scale meter with nuclear logo ──
    // Use a plain container with no clipping so scale labels can overflow
    lv_obj_t *gauge_col = lv_obj_create(cont);
    lv_obj_remove_style_all(gauge_col);
    lv_obj_set_size(gauge_col, 193, CONTENT_H - 4);   // 175->193: room for the
    // channel selector without shrinking the gauge. Ceiling is set by the BSSID in
    // the right pane: 17 chars * 7px mono + padding, and it CLIPS rather than wraps.
    lv_obj_remove_flag(gauge_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_clip_corner(gauge_col, false, 0);
    lv_obj_add_flag(gauge_col, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    // Channel selector. Sits in the ~36px above the scale (rad_scale is 115px centred at
    // +10, so it occupies roughly y 36..152 of this 168px column). NO file-scope pointer for
    // it: the selection is read from cfg at build time, exactly as the Raw/PCAP selector does
    // (ui_nav.h:4482), so nothing periodic can hold a dangling reference to it.
    {
        lv_obj_t *rad_dd = make_dropdown(gauge_col, CB_DEAUTH_DD_OPTS);
        lv_obj_set_width(rad_dd, 70);
        lv_obj_align(rad_dd, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_dropdown_set_selected(rad_dd, cb_deauth_mode_to_idx(cfg.deauth_chan));
        lv_obj_add_event_cb(rad_dd, cb_deauth_dd_cb, LV_EVENT_VALUE_CHANGED, NULL);

        // Say what the control IS. "1/6/11" alone is meaningless to anyone who does not
        // already know WiFi channels exist -- which is most of the people this badge goes to.
        // Dim + 14px so it reads as a caption rather than competing with the gauge.
        lv_obj_t *dd_cap = make_label(gauge_col, "Ch #", &ui_font_pipboy_14, pip_dim());
        lv_obj_align_to(dd_cap, rad_dd, LV_ALIGN_OUT_BOTTOM_LEFT, 2, 1);
    }

    // Scale meter - smaller to fit labels within content area
    rad_scale = lv_scale_create(gauge_col);
    lv_obj_set_size(rad_scale, 103, 103);
    lv_obj_align(rad_scale, LV_ALIGN_CENTER, 22, 10);
    lv_scale_set_mode(rad_scale, LV_SCALE_MODE_ROUND_OUTER);
    lv_scale_set_range(rad_scale, 0, 100);
    lv_scale_set_total_tick_count(rad_scale, 51);
    lv_scale_set_major_tick_every(rad_scale, 5);
    lv_scale_set_label_show(rad_scale, true);

    // Scale styling
    lv_obj_set_style_arc_color(rad_scale, pip_border(), LV_PART_MAIN);
    lv_obj_set_style_arc_width(rad_scale, 2, LV_PART_MAIN);
    lv_obj_set_style_line_color(rad_scale, pip_dim(), LV_PART_ITEMS);
    lv_obj_set_style_line_width(rad_scale, 1, LV_PART_ITEMS);
    lv_obj_set_style_length(rad_scale, 4, LV_PART_ITEMS);
    lv_obj_set_style_line_color(rad_scale, pip_primary(), LV_PART_INDICATOR);
    lv_obj_set_style_line_width(rad_scale, 2, LV_PART_INDICATOR);
    lv_obj_set_style_length(rad_scale, 7, LV_PART_INDICATOR);
    lv_obj_set_style_text_font(rad_scale, &ui_font_pipboy_14, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(rad_scale, pip_primary(), LV_PART_INDICATOR);

    // Max arc (behind current arc) - highlight color, stays at peak
    // Size just inside the scale tick marks
    rad_max_needle = lv_arc_create(rad_scale);
    lv_obj_add_event_cb(rad_max_needle, cb_selfnull_on_delete, LV_EVENT_DELETE, &rad_max_needle);
    lv_obj_set_size(rad_max_needle, 98, 98);
    lv_obj_center(rad_max_needle);
    lv_arc_set_rotation(rad_max_needle, 135);
    lv_arc_set_bg_angles(rad_max_needle, 0, 270);
    lv_arc_set_range(rad_max_needle, 0, 100);
    int max_capped = (rad_max_rate > 100) ? 100 : rad_max_rate;
    lv_arc_set_value(rad_max_needle, max_capped);
    lv_obj_remove_flag(rad_max_needle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(rad_max_needle, pip_border(), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(rad_max_needle, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_color(rad_max_needle, pip_highlight(), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(rad_max_needle, LV_OPA_50, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(rad_max_needle, 6, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(rad_max_needle, LV_OPA_TRANSP, LV_PART_KNOB);

    // Current value arc - primary color, on top
    rad_needle = lv_arc_create(rad_scale);
    lv_obj_add_event_cb(rad_needle, cb_selfnull_on_delete, LV_EVENT_DELETE, &rad_needle);
    lv_obj_set_size(rad_needle, 98, 98);
    lv_obj_center(rad_needle);
    lv_arc_set_rotation(rad_needle, 135);
    lv_arc_set_bg_angles(rad_needle, 0, 270);
    lv_arc_set_range(rad_needle, 0, 100);
    lv_arc_set_value(rad_needle, rad_geiger_active ? rad_display_val : 0);
    lv_obj_remove_flag(rad_needle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(rad_needle, pip_border(), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(rad_needle, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_color(rad_needle, pip_primary(), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(rad_needle, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(rad_needle, true, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(rad_needle, LV_OPA_TRANSP, LV_PART_KNOB);

    // Nuclear logo in center - scale up from 60x60
    lv_obj_t *logo = lv_image_create(rad_scale);
    lv_image_set_src(logo, &nuclear_60x60);
    lv_image_set_scale(logo, 220);  // 60x60 → ~82x82 rendered, fills arc center
    lv_obj_set_style_image_recolor(logo, pip_primary(), 0);
    lv_obj_set_style_image_recolor_opa(logo, LV_OPA_COVER, 0);
    lv_obj_center(logo);

    // "Deauths/Second" at bottom of scale between 0 and 100
    rad_val_lbl = lv_label_create(gauge_col);   // gauge_col, NOT rad_scale: as a child of
    // the scale the 2nd line collided with the 0/100 ticks, which draw OUTSIDE the arc.
    lv_label_set_text(rad_val_lbl, "Deauths/sec");
    lv_obj_set_style_text_font(rad_val_lbl, &ui_font_pipboy_14, 0);
    lv_obj_set_style_text_color(rad_val_lbl, pip_dim(), 0);
    lv_obj_set_style_text_align(rad_val_lbl, LV_TEXT_ALIGN_CENTER, 0);
    // x offset must MATCH the scale's (22), not the column's centre. This label is a child of
    // gauge_col but reads as the meter's caption, so centring it on the column left it visibly
    // off-centre under the shifted arc.
    lv_obj_align(rad_val_lbl, LV_ALIGN_BOTTOM_MID, 22, 2);
    // ⚠ rad_val_lbl is now written by rad_poll_cb (the "100+" off-scale marker), which it was
    // NOT before. Every other global that periodic callback touches already self-nulls; this
    // one was safe only because nothing periodic used it. content_teardown() is not sufficient
    // coverage -- ui_theme_switch_live() keeps a hand-rolled teardown that omits exactly this
    // pointer -- so the identity-checked self-null is what actually closes the UAF.
    lv_obj_add_event_cb(rad_val_lbl, cb_selfnull_on_delete, LV_EVENT_DELETE, &rad_val_lbl);
    rad_refresh_mode_line();

    // Bring gauge column to front so tick labels overlay the stats
    lv_obj_move_foreground(gauge_col);

    // ── Right: Stats + controls ──
    lv_obj_t *stats_col = lv_obj_create(cont);
    lv_obj_remove_style_all(stats_col);
    lv_obj_set_flex_grow(stats_col, 1);
    lv_obj_set_height(stats_col, CONTENT_H - 4);
    lv_obj_set_flex_flow(stats_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(stats_col, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(stats_col, 3, 0);
    lv_obj_set_style_pad_left(stats_col, 8, 0);

    // Reset button. pad_ver 6->3 LOCALLY (-6px): style_action_btn is shared by 17 buttons,
    // so shrinking it there would resize every action button on the badge. Measured overflow
    // of this column was only 4px, so the two overrides clear it with room to spare and every
    // label stays (owner decision -- "Top BSSID:" header keeps its heading).
    lv_obj_t *rad_reset_btn = make_action_btn(stats_col, "Reset", [](lv_event_t *e) {
        (void)e;
        rad_reset_stats();
        if (rad_needle) lv_arc_set_value(rad_needle, 0);
        if (rad_max_needle) lv_arc_set_value(rad_max_needle, 0);
        if (rad_max_lbl) lv_label_set_text(rad_max_lbl, "Max: 0/s");
        if (rad_topch_lbl) lv_label_set_text(rad_topch_lbl, "Top ch: --");
        if (rad_topbss_lbl) lv_label_set_text(rad_topbss_lbl, "---");
        if (rad_timer_lbl) lv_label_set_text(rad_timer_lbl, "00:00:00");
    }, NULL);
    lv_obj_set_style_pad_ver(rad_reset_btn, 3, 0);

    // Timer
    rad_timer_lbl = make_label(stats_col, "00:00:00", &ui_font_pipboy_14, pip_dim());
    lv_obj_add_event_cb(rad_timer_lbl, cb_selfnull_on_delete, LV_EVENT_DELETE, &rad_timer_lbl);
    if (rad_geiger_active && rad_start_time > 0) {
        uint32_t elapsed = (millis() - rad_start_time) / 1000;
        uint32_t h = elapsed / 3600, m = (elapsed % 3600) / 60, s = elapsed % 60;
        lv_label_set_text_fmt(rad_timer_lbl, "%02lu:%02lu:%02lu",
            (unsigned long)h, (unsigned long)m, (unsigned long)s);
    }

    // Stats
    rad_max_lbl = make_label(stats_col, "Max: 0/s", &ui_font_pipboy_14, pip_primary());
    lv_obj_add_event_cb(rad_max_lbl, cb_selfnull_on_delete, LV_EVENT_DELETE, &rad_max_lbl);
    if (rad_max_rate > 0) lv_label_set_text_fmt(rad_max_lbl, "Max: %d/s", rad_max_rate);

    rad_topch_lbl = make_label(stats_col, "Top ch: --", &ui_font_pipboy_14, pip_primary());
    lv_obj_add_event_cb(rad_topch_lbl, cb_selfnull_on_delete, LV_EVENT_DELETE, &rad_topch_lbl);
    if (rad_top_channel > 0) lv_label_set_text_fmt(rad_topch_lbl, "Top ch: %d", rad_top_channel);

    make_label(stats_col, "Top BSSID:", &ui_font_pipboy_14, pip_dim());
    rad_topbss_lbl = make_label(stats_col, "---", &ui_font_pipboy_14, pip_primary());
    lv_obj_add_event_cb(rad_topbss_lbl, cb_selfnull_on_delete, LV_EVENT_DELETE, &rad_topbss_lbl);
    if (rad_top_bssid[0]) lv_label_set_text(rad_topbss_lbl, rad_top_bssid);
    lv_label_set_long_mode(rad_topbss_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(rad_topbss_lbl, lv_pct(100));
    lv_obj_set_style_text_align(rad_topbss_lbl, LV_TEXT_ALIGN_CENTER, 0);

    // Start/Stop button
    rad_toggle_btn = make_action_btn(stats_col,
        rad_geiger_active ? "Stop" : "Start",
        rad_toggle_cb, NULL);
    lv_obj_set_style_pad_ver(rad_toggle_btn, 3, 0);   // local, same reason as Reset above
}

// ─────────────────────── CLIPBOY DISPATCH ──────────────────────────────────

// Map (category id, item_index) to ClipBoyMarauder API calls. Callers pass
// the stable ToolCategory.id (NOT the array position), so this stays correct
// in the Sn34k build where ACTIVE RESEARCH cats are compiled out and the
// remaining array positions compact.
// A capture tool whose ONLY product is the .pcap file -- Analyze > Raw/PCAP (item 3) and
// Analyze > EAPOL/PMKID (item 7) -- is pointless with PCAP Saving off: it would run and
// write nothing, misleading the user. Gate those two on cfg.allow_pcap. Other Analyze
// tools (Beacons/Probes/Deauth/...) show useful live results, so they're not gated.
static inline bool pcap_file_tool_gated(uint8_t cat, uint8_t item) {
    return cat == 3 && (item == 3 || item == 7) && !cfg.allow_pcap;
}

// Raw/PCAP channel selector: 0 = Hop (all, default), 1-14 = lock to that channel.
static uint8_t raw_capture_channel = 0;

static void dispatch_clipboy_action(uint8_t cat, uint8_t item) {
    if (pcap_file_tool_gated(cat, item)) {   // CLI / harness / any dispatch caller
        Serial.println("[CB] Blocked: enable 'Allow PCAP Saving' (DATA > Settings) first.");
        return;
    }
    // Clean slate on every tool start -- the single "standard event" that stops any tool from
    // displaying a prior tool's leftovers. Zeroes both packet-counter families (StopScan resets
    // *_frames but NOT num_* -> Raw/PCAP once showed a stale num_probe) AND the pure-output
    // lists that persist across tool switches: BT/Flock/Pwnagotchi devices + the MAC-tracker
    // table + channel-activity. Does NOT touch AP/STA/SSID lists (select-then-run inputs).
    cb.resetDisplayAccumulators();
    cb_manual_scan_kind = -1;   // a real tool owns the readout now, not an inline Scan button
#ifdef CLIPBOY_RES34RCH
    // Active-transmit tools that raw-TX via esp_wifi_80211_tx(WIFI_IF_AP,...) need
    // the AP interface up -- the badge boots STA-only (listen-only / no idle AP), so
    // switch to APSTA on demand here (stable cat ids: 6 Deauth, 7 Flood, 8 Beacon
    // Spam, 11 Evil Portal). SAE (10) transmits via WIFI_IF_STA and BLE Spam (9) is
    // Bluetooth, so neither needs it. cb_stop_operation restores STA-only.
    if (cat == 6 || cat == 7 || cat == 8 || cat == 11)
        cb.setRawTxMode(true);
#endif
    switch (cat) {
        case 0: // Detect
            switch (item) {
                case 0: cb.btScanAirtags();   break;  // AirTag
                case 1: cb.btScanSkimmers();  break;  // Skimmer Check
                case 2: cb.btScanFlippers();  break;  // Flipper Zero
                case 3: cb.btScanFlock();     break;  // Flock Batteries
                case 4: cb.sniffPinescan();   break;  // Rogue AP
                case 5: cb.sniffMultiSSID();  break;  // Evil Twin
                case 6: cb.btScanRemoteID();  break;  // Remote ID (drone)
            } break;
        case 1: // Scan
            switch (item) {
                case 0: cb.scanAPs();             break;  // APs (full)
                case 1: cb.scanAPsAndStations();  break;  // APs + Stations
                case 2: cb.scanStations();        break;  // Stations
                case 3: cb.btScanAll();           break;  // BT Devices
                case 4: cb.btScanSimple();        break;  // BLE Adverts
            } break;
        case 2: // Monitor
            switch (item) {
                case 0: cb.packetMonitor();    break;
                case 1: cb.packetMonitor();    break;  // Rate derives from the monitor's num* totals
                case 2: cb.signalMonitor();    break;
                case 3: cb.channelActivity();  break;
                case 4: cb.macTracker();       break;
            } break;
        case 3: // Analyze (passive capture)
            switch (item) {
                case 0: cb.sniffBeacons();     break;  // Beacons
                case 1: cb.sniffProbes();      break;  // Probes
                case 2: cb.sniffDeauth();      break;  // Deauth
                case 3: cb.sniffRaw(); cb.setRawCaptureChannel(raw_capture_channel); break;  // Raw/PCAP
                case 4: cb.sniffPwnagotchi();  break;  // Pwnagotchi
                case 5: cb.sniffEspressif();   break;  // Espressif
                case 6: cb.sniffSAE();         break;  // SAE Commit
                case 7: cb.sniffEAPOL();       break;  // EAPOL/PMKID
            } break;
        case 4: // Utilities
            switch (item) {
                case 0: /* List APs - handled in UI */          break;
                case 1: /* List SSIDs - handled in UI */        break;
                case 2: /* List Stations - handled in UI */     break;
                case 3: /* List BT Devices - handled in UI */   break;
                case 4: /* List AirTags - handled in UI */      break;
                case 5: /* List Flippers - handled in UI */     break;
                case 6: /* Saved Networks - handled in UI */    break;
                case 7: /* Add SSID - handled in show_tool_text */ break;
                case 8: cb.generateSSIDs(20);                   break;
                case 9: /* Select AP - handled in UI */         break;
                case 10:
                    // Clear All: wipe the VIEWS too, not just the stores. Leaving the widgets
                    // populated after an explicit "clear" is the most literal version of this
                    // whole defect class.
                    cb_clear_ap_results();
                    cb_clear_sta_results();
                    cb.clearSSIDs();
                    break;
                case 11: /* Set Channel - handled via dropdown */ break;
            } break;
        case 5: // Network
            switch (item) {
                case 0: /* Join WiFi - handled in show_tool_text */ break;
                case 1: cb.randomizeAPMac();                    break;
                case 2: cb.randomizeSTAMac();                   break;
            } break;
#ifdef CLIPBOY_RES34RCH  // ─── ACTIVE RESEARCH dispatch tail (Res34rch-Boy only), ids 6-11 ───
        case 6: // Deauth
            switch (item) {
                case 0: cb.deauthAPs();        break;  // ! Discovered
                case 1: cb.deauthManual();     break;  // ! Manual
                case 2: cb.deauthStations();   break;  // ! Stations
            } break;
        case 7: // Flood
            switch (item) {
                case 0: cb.authFlood();        break;  // ! Auth
                case 1: cb.badMsgFlood();      break;  // ! Bad Msg
                case 2: cb.badMsgStations();   break;  // ! Bad Msg Target
                case 3: cb.sleepFlood();       break;  // ! Sleep
                case 4: cb.sleepStations();    break;  // ! Sleep Target
            } break;
        case 8: // Beacon Spam
            switch (item) {
                case 0: cb.beaconSpamRandom(); break;  // Random
                case 1: cb.beaconSpamList();   break;  // List
                case 2: cb.beaconSpamClone();  break;  // AP Clone
                case 3: cb.beaconRickRoll();   break;  // Rick Roll
                case 4: cb.beaconFunny();      break;  // Funny
            } break;
        case 9: // BLE Spam
            switch (item) {
                case 0: cb.btSpamApple();     break;
                case 1: cb.btSpamWindows();   break;
                case 2: cb.btSpamSamsung();   break;
                case 3: cb.btSpamGoogle();    break;
                case 4: cb.btSpamFlipper();   break;
                case 5: cb.btSpamAll();       break;
            } break;
        case 10: // SAE
            cb.saeCommitFlood(); break;
        case 11: // Evil Portal
            // Apply the staged AP MAC (Rnd/Set AP MAC) BEFORE the portal's softAP(). Unlike the
            // raw-TX tools (Deauth/Flood/Beacon Spam), EvilPortal::startAP() never calls setMac(),
            // so without this the portal beacons on the chip default and ignores Rnd AP MAC (a fork
            // regression vs upstream, which documents Evil Portal as a Set-MACs consumer). Placed
            // here because setRawTxMode(true) above already brought up APSTA -- the AP analog of
            // joinWiFi's proven mode->setMac->begin sequence; softAP() does not reset the MAC. Not
            // on Stop (item 2): no portal is coming up, and setMac mid-teardown is pointless.
            if (item != 2) cb.applyStagedMacs();
            switch (item) {
                case 0: cb.startEvilPortal();                   break;
                case 1: cb.startEvilPortal("custom.html");      break;
                case 2: cb.stopEvilPortal();                    break;
            } break;
#endif // CLIPBOY_RES34RCH
        default:
            CB_LOGF("[WARN] Unknown tool cat=%d item=%d\n", cat, item);
            break;
    }
}

// Stop any running ClipBoy operation and clean up scan timer
static void cb_stop_operation(void) {
    if (cb_op_running) {
        cb.stopScan();
        // Clip-Boy (PCAP perf): finalize any open capture -- drain the last buffer and
        // close the pcap file handle we now hold open across the whole capture (O(1)
        // drains instead of a per-150ms open+append+close FAT re-scan). Idempotent, so
        // it's a no-op for non-capture tools. Runs on this (main/LVGL) task, the same
        // task as ClipBoy loop()'s buffer_obj.save(), so the file state is single-owner.
        cb.finishCapture();
        // FB1 (audit 2026-07-24): tear down promiscuous mode OURSELVES. cb.stopScan() clears
        // the library's scan MODE, but the ESP-IDF promiscuous filter and its RX callback stay
        // installed: shutdownWiFi()'s esp_wifi_set_promiscuous(false) sits behind
        // `if (!this->wifi_connected)` (WiFiScan.cpp:2476), and that flag latches TRUE on the
        // first cb.loop() -- it ORs in `WiFi.softAPIP() != 0.0.0.0`, and WiFi.mode(WIFI_STA)
        // unconditionally creates the AP netif whose default IP is the static 192.168.4.1. The
        // library also contains ZERO esp_wifi_set_promiscuous_rx_cb(NULL) calls against 12
        // registrations. So after every tool stop the badge kept RECEIVING and PROCESSING every
        // frame in the air with the UI idle: ~80 mA the user thinks is off, unbounded
        // access_points growth (a `new LinkedList<uint16_t>` per BSSID plus a blocking
        // Serial.print per new AP, on the WiFi task), continued buffer_obj.append into a pcap
        // the user believes closed, and a much wider window for the FB9 LinkedList race.
        // The tell that this was an oversight rather than intent: the three Join-WiFi handlers
        // hand-rolled exactly these two calls themselves.
        //
        // Deliberately OUR layer only -- do NOT try to untangle wifi_connected or call
        // esp_wifi_stop(). This project has already been bitten by a WiFi-state "cleanup" here:
        // a WiFi.mode(WIFI_OFF) in this function broke Raw/PCAP's promiscuous re-init until a
        // power cycle, because a cold boot never bounces the radio. Order matters: clear the
        // CALLBACK first, then the filter, so no frame can arrive between the two and land in a
        // handler whose consumer we have just torn down.
        esp_wifi_set_promiscuous_rx_cb(NULL);
        esp_wifi_set_promiscuous(false);
        cb_op_running = false;
        cb_op_name = NULL;
        cb_op_encoded = -1;
        cb_manual_scan_kind = -1;
        if (lbl_stask) lv_label_set_text(lbl_stask, "");
#ifdef CLIPBOY_RES34RCH
        // Drop the on-demand AP interface an active-transmit tool may have brought
        // up (dispatch_clipboy_action) so the badge returns to STA-only and stops
        // beaconing a SoftAP while idle. Harmless when already STA.
        cb.setRawTxMode(false);
#endif
        // NOTE (DC34): we used to WiFi.mode(WIFI_OFF) here to save ~80mA, but the OFF->STA
        // bounce on the NEXT tool (via cb_ensure_wifi) left Raw/PCAP's promiscuous re-init
        // broken -- 0 frames + empty pcap until a power-cycle (a cold boot works because it
        // never bounces WiFi off). Keeping WiFi in STA between tools matches that known-good
        // state. Airplane mode still forces WiFi off. (Power tradeoff: WiFi stays ~on between
        // tools; revisit with a screensaver/idle power-down if battery life needs it.)
    }
    if (cb_scan_timer) {
        lv_timer_delete(cb_scan_timer);
        cb_scan_timer = NULL;
    }
    // Stop Geiger if active (FB11/FB12: this branch used to skip both the tick-audio
    // teardown and cb.stopScan(), leaving the sniffer running with no consumer)
    rad_geiger_force_stop();
    // Full output cleanup - stop timer, clear buffer, reset counters
    cb_output_cleanup();
    cb_last_ap_count = 0;
    cb_last_sta_count = 0;
    wifi_status_label = NULL;
    // Do NOT null wifi_scan_btn_label here. Every set-site registers cb_selfnull_on_delete, so
    // the global already self-clears when the scan button is deleted (nav/theme rebuild). Nulling
    // it mid-stop made the scan handlers' OWN "Scan"/"Stop" set_text -- which run right after
    // cb_stop_operation() -- a no-op, so the button never flipped Scan<->Stop (all TAT_AP/STA tools).
    cb_inline_scan_refresh();   // F1: the radio is free again -> un-dim the inline Scan
}

// Ensure WiFi is on before a ClipBoy scan operation.
// Disconnects any active WiFi connection - scans use promiscuous mode.
static void cb_ensure_wifi(void) {
    if (cfg.airplane) return;  // respect airplane mode
    // Disconnect station connection if active - promiscuous mode is incompatible
    if (WiFi.status() == WL_CONNECTED) {
        WiFi.disconnect(false);
        wifi_join_ssid[0] = '\0';
        CB_LOGLN("[CB] Disconnected WiFi for scan");
    }
    if (WiFi.getMode() == WIFI_OFF) {
        WiFi.mode(WIFI_STA);
        delay(50);
        CB_LOGLN("[CB] WiFi enabled for scan");
    }
}

// ─── Live scan result polling ──────────────────────────────────────────────

// Populate AP checkbox list from ClipBoy scan results
// RSSI monitors exactly ONE AP, so its inline list is single-select (tapping one AP
// clears the others). Other TAT_AP tools (EAPOL, active-research targeting) keep
// multi-select. show_tool_ap sets this per-tool before (re)populating.
static bool cb_ap_single_select = false;
static lv_obj_t *cb_rssi_start_btn = NULL;   // RSSI START button -- disabled until an AP is picked

static bool cb_any_ap_selected(void) {
    int n = cb.getAPCount();
    for (int i = 0; i < n; i++) if (cb.isAPSelected(i)) return true;
    return false;
}
// Enable (bright) / disable (dim + non-clickable) the RSSI START button. When it's showing
// STOP (a monitor is running) it stays enabled; otherwise it's live only once an AP is picked.
// F1 -- the inline "Scan" button on the AP/station pickers shares the radio with whatever
// tool is running. Tapping it used to silently stop that tool and start scanning, while the
// tool's own "> STOP <" button (whose label is written ONCE at build time) and its lv_chart
// kept asserting it was still running. Owner-confirmed: tapping "> STOP <" afterwards STARTS
// a scan, because cb_op_encoded no longer matches.
//
// Rule: the inline Scan is disabled while THIS PAGE's own tool is running -- see
// cb_inline_scan_blocked_for() below for why it is scoped that way rather than to any running
// tool. (An earlier revision of this comment described the broader rule; it was superseded when
// the broad version proved to create a dead end, and the stale text contradicted the
// implementation for one commit.)
// Note this partly reverses FB6's "one tap scans" convenience for the same-page case, on
// purpose: the button is visibly dimmed rather than lying about what a tap will do.
// Which tool's page the live inline Scan button belongs to (only one picker is ever built).
static int32_t cb_inline_scan_page_enc = -1;

// ── Clear a result STORE and the VIEW of it in one operation ────────────────
// Owner-reported: after tapping Scan there was a ~500 ms window where the status bar already
// read "0 APs found" but the list still showed the previous scan's rows, one of them still
// highlighted, and "> START <" still live -- so you could launch a tool against a selection
// that no longer existed. The clear was happening LAZILY, on the next 500 ms poller tick.
//
// The rule this encodes: whoever destroys the data destroys the view of it, synchronously, in
// the same call. Eventually-consistent is not good enough when the stale frame is clickable.
// Every widget that renders a cb.* result list, and every control gated on that list, must be
// reset here rather than left for a poller to notice.
static void cb_rssi_refresh_start(void);   // defined below
static void cb_clear_ap_results(void) {
    cb.clearAPs();
    cb_last_ap_count = 0;
    // ALSO reset the output-log high-water marks. cb_poll_aps/cb_poll_stations are index-based
    // incremental readers exactly like cb_populate_ap_list, and the F14 shrink guard was added
    // to only one of the three. Leaving these high meant: start Analyze > Beacons, then run
    // Utilities > Clear All (EXECUTE does NOT stop the running tool), and the store restarts at
    // 0 while cb_output_last_ap still holds ~30 -- so `while (cb_output_last_ap < count)` never
    // fires again and the AP log lists NOTHING for the rest of that run.
    // cb.clearAPs() -> RunClearAPs() clears stations too, so reset both marks here.
    cb_output_last_ap = 0;
    cb_output_last_sta = 0;
    if (cb_ap_list_area) clear_children(cb_ap_list_area);
    cb_rssi_refresh_start();   // selection is gone with the store -> START dims immediately
}
static void cb_clear_sta_results(void) {
    cb.clearStations();
    cb_last_sta_count = 0;
    cb_output_last_sta = 0;    // same index-based-reader reset as above
    if (cb_sta_list_area) clear_children(cb_sta_list_area);
}

static bool cb_inline_scan_blocked_for(int32_t page_enc) {
    // AIRPLANE MODE FIRST, before any of the ownership reasoning below.
    // The inline Scan buttons started a scan with NO airplane check at all: `cb_ensure_wifi()`
    // self-suppresses under airplane, but that is not a gate -- `WiFiScan::StartScan` reaches
    // `esp_wifi_start()` + `esp_wifi_set_promiscuous(true)` on its own. Traced 2026-07-26.
    // LIVE, TAP-ONLY REPRO (the reason this is a fix and not a doc note): Airplane ON ->
    // ITEMS > Tools > Utilities/Lists > "Select AP" -> tap "Scan". That page is reachable under
    // airplane because `tool_needs_radio()` ALLOWLISTS "Select AP" as a non-radio tool, so
    // `tool_tap_cb`'s gate never fires -- and then the page offers a button that starts the radio.
    // The STA picker and the Join WiFi picker have the identical missing check but are NOT
    // reachable under airplane (every tool that builds those pages is radio-needing and blocked
    // at entry). Guarding HERE covers the live path and both latent ones in one place, instead of
    // three call-site edits that would each be a chance to miss the fourth.
    // Bonus: because this predicate also drives `cb_btn_set_enabled`, the button DIMS under
    // airplane rather than looking available and refusing -- same treatment F14 gave the big
    // START button.
    if (cfg.airplane) return true;
    if (!cb_op_running) return false;
    // The inline scan's own operation is named "Scanning APs"/"Scanning Stations" -- if THAT
    // is what is running, this button is a legitimate Stop toggle, not a thief. (Its label is
    // built as "Stop" from the same test, so a scan started on one picker page can be stopped
    // from another. The AP scan is tool-agnostic shared state; that is correct.)
    if (cb_op_name && strstr(cb_op_name, "Scan")) return false;
    // Refuse ONLY when the running tool's own START/STOP button and output live on THIS page.
    // That is the whole harm: the owner-confirmed bug is a tap here silently stopping the tool
    // whose "> STOP <" is right there, which then reads STOP for a dead tool and freezes its
    // chart. Once you have navigated away those widgets are destroyed, so nothing is left to
    // lie -- and refusing there produced a DEAD END (a tool running on another page made this
    // button permanently dim with no way to stop that tool from here). Allowing it cross-page
    // also matches how tapping any tool's own START behaves: cb_tool_start_stop_cb begins with
    // `if (cb_op_running) cb_stop_operation()`, a full teardown. Same radio, same rule.
    return cb_op_encoded == page_enc;
}

static void cb_inline_scan_refresh(void);   // defined below cb_btn_set_enabled

// The live inline Scan button, so its dimming can be REFRESHED when the running tool changes.
// Owner-reported: the mutex refused correctly but the button stayed bright, because its state
// was only ever set at build time and the tool is usually started AFTER the page is built.
// A button that silently does nothing is its own kind of lie.
static lv_obj_t *cb_inline_scan_btn = NULL;

// Paint a small button as unavailable AND make it ignore taps. Both halves matter: a dimmed
// button that still fires is a lie in the other direction.
static void cb_btn_set_enabled(lv_obj_t *btn, bool on) {
    if (!btn || !lv_obj_is_valid(btn)) return;
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    if (on) {
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        if (lbl) lv_obj_set_style_text_color(lbl, pip_primary(), 0);
    } else {
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        if (lbl) lv_obj_set_style_text_color(lbl, pip_disabled(), 0);
    }
}

// Re-apply the inline Scan button's dimming to the CURRENT running-tool state. Called from
// every path that starts or stops an operation: the button is built once, but the tool it
// competes with is usually started AFTERWARDS, so its build-time state goes stale immediately.
// (Owner-reported: the mutex refused correctly while the button still looked available.)
static void cb_inline_scan_refresh(void) {
    if (!cb_inline_scan_btn || !lv_obj_is_valid(cb_inline_scan_btn)) return;
    cb_btn_set_enabled(cb_inline_scan_btn, !cb_inline_scan_blocked_for(cb_inline_scan_page_enc));
}

static void cb_rssi_refresh_start(void) {
    if (!cb_rssi_start_btn || !lv_obj_is_valid(cb_rssi_start_btn)) return;
    lv_obj_t *lbl = lv_obj_get_child(cb_rssi_start_btn, 0);
    const char *txt = lbl ? lv_label_get_text(lbl) : "";
    bool ok = (txt && strstr(txt, "STOP")) || cb_any_ap_selected();
    if (ok) {
        lv_obj_add_flag(cb_rssi_start_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(cb_rssi_start_btn, pip_highlight(), 0);
        if (lbl) lv_obj_set_style_text_color(lbl, pip_bg(), 0);
    } else {
        lv_obj_remove_flag(cb_rssi_start_btn, LV_OBJ_FLAG_CLICKABLE);   // taps ignored
        lv_obj_set_style_bg_color(cb_rssi_start_btn, pip_border(), 0);  // dim = disabled
        if (lbl) lv_obj_set_style_text_color(lbl, pip_dim(), 0);
    }
}
// Paint an RSSI list row as selected (filled highlight) or not (transparent) -- readable both.
static void cb_rssi_row_paint(lv_obj_t *row, bool sel) {
    if (!row) return;
    lv_obj_t *nm = lv_obj_get_child(row, 0);
    lv_obj_t *rs = lv_obj_get_child(row, 1);
    if (sel) {
        lv_obj_set_style_bg_color(row, pip_highlight(), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        if (nm) lv_obj_set_style_text_color(nm, pip_bg(), 0);
        if (rs) lv_obj_set_style_text_color(rs, pip_bg(), 0);
    } else {
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        if (nm) lv_obj_set_style_text_color(nm, pip_primary(), 0);
        if (rs) lv_obj_set_style_text_color(rs, pip_dim(), 0);
    }
}

static void cb_populate_ap_list(void) {
    if (!cb_ap_list_area) return;
    int count = cb.getAPCount();
    if (count == cb_last_ap_count) return;  // no change

    // F14: the list SHRANK (a new scan just called cb.clearAPs(), so count drops to 0). The
    // incremental append below only ever ADDS from cb_last_ap_count, and the clear was gated on
    // `cb_last_ap_count == 0` -- which is false here -- so the widget kept showing the previous
    // scan's rows WITH the old selection highlighted, while the status bar already read
    // "0 APs found" and the library's selection was gone. Owner screenshot rssi-06 caught
    // exactly that. Clear on shrink so the widget never asserts rows the store no longer has.
    if (count < cb_last_ap_count) {
        clear_children(cb_ap_list_area);
        cb_last_ap_count = 0;
    }

    // Only add NEW entries (incremental) - avoids full teardown/rebuild churn
    // On first populate (last_count=0), clear any placeholder text
    if (cb_last_ap_count == 0) clear_children(cb_ap_list_area);

    for (int i = cb_last_ap_count; i < count; i++) {
        CBAccessPointInfo ap;
        if (!cb.getAP(i, ap)) continue;
        ap.essid[sizeof(ap.essid) - 1] = '\0';  // null-term guard

        if (cb_ap_single_select) {
            // Single-select highlight row (RSSI et al.): tap ONE AP, no checkboxes; keeps the
            // per-AP dBm so you can pick by signal. Selecting clears the others + lights START.
            lv_obj_t *row = lv_button_create(cb_ap_list_area);
            lv_obj_remove_style_all(row);
            lv_obj_set_width(row, lv_pct(100));
            lv_obj_set_height(row, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_ver(row, 2, 0);
            lv_obj_set_style_pad_hor(row, 3, 0);
            lv_obj_set_style_radius(row, 0, 0);
            // SSID: grow to fill, and ELLIPSIZE rather than wrap. make_label leaves LVGL's default
            // LONG_WRAP on, so a long SSID grew the row to two lines -- pre-existing, and adding the
            // channel below would have made it routine. LONG_DOT needs a bounded width, which
            // flex_grow(1) supplies.
            lv_obj_t *ap_lbl = make_label(row, cb_safe(ap.essid), &ui_font_pipboy_14, pip_primary());
            lv_obj_set_flex_grow(ap_lbl, 1);
            lv_label_set_long_mode(ap_lbl, LV_LABEL_LONG_DOT);
            // Channel folded INTO the signal label rather than added as a third flex child: same
            // information, no extra gap, and it sits next to the dBm where you look when comparing
            // APs. Channel matters here because Monitor > RSSI tunes to the selected AP's channel --
            // if a row's channel is stale (the AP moved since the scan) the tool reports "no packets
            // for Ns", and seeing the channel is what makes that diagnosable rather than mysterious.
            char rb[24]; snprintf(rb, sizeof(rb), "c%d %ddBm", ap.channel, ap.rssi);
            lv_obj_t *sig_lbl = make_label(row, rb, &ui_font_pipboy_14, pip_dim());
            lv_label_set_long_mode(sig_lbl, LV_LABEL_LONG_CLIP);
            cb_rssi_row_paint(row, cb.isAPSelected(i));
            lv_obj_add_event_cb(row, [](lv_event_t *e) {
                int idx = (int)(intptr_t)lv_event_get_user_data(e);
                cb.deselectAPs();
                cb.selectAP(idx);
                lv_obj_t *self = (lv_obj_t *)lv_event_get_current_target(e);
                if (cb_ap_list_area) {
                    uint32_t rows = lv_obj_get_child_count(cb_ap_list_area);
                    for (uint32_t r = 0; r < rows; r++) {
                        lv_obj_t *ro = lv_obj_get_child(cb_ap_list_area, r);
                        cb_rssi_row_paint(ro, ro == self);
                    }
                }
                cb_rssi_refresh_start();
            }, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            continue;
        }

        lv_obj_t *cb_row = lv_obj_create(cb_ap_list_area);
        lv_obj_remove_style_all(cb_row);
        lv_obj_add_style(cb_row, &style_container, 0);
        lv_obj_set_size(cb_row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(cb_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(cb_row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(cb_row, 4, 0);
        lv_obj_set_style_pad_ver(cb_row, 1, 0);

        lv_obj_t *chk = lv_checkbox_create(cb_row);
        lv_checkbox_set_text(chk, cb_safe(ap.essid));
        lv_obj_set_style_text_font(chk, &ui_font_pipboy_14, 0);
        lv_obj_set_style_text_color(chk, pip_primary(), 0);
        lv_obj_set_style_text_color(chk, pip_highlight(), LV_PART_INDICATOR | LV_STATE_CHECKED);

        if (cb.isAPSelected(i)) lv_obj_add_state(chk, LV_STATE_CHECKED);

        // Toggle selection on tap
        lv_obj_add_event_cb(chk, [](lv_event_t *e) {
            int idx = (int)(intptr_t)lv_event_get_user_data(e);
            lv_obj_t *checkbox = (lv_obj_t *)lv_event_get_target(e);
            if (lv_obj_has_state(checkbox, LV_STATE_CHECKED)) {
                if (cb_ap_single_select) {
                    // Radio behaviour: clear every other selection + uncheck sibling boxes
                    // (programmatic remove_state does NOT re-fire VALUE_CHANGED, so no recursion).
                    cb.deselectAPs();
                    if (cb_ap_list_area) {
                        uint32_t rows = lv_obj_get_child_count(cb_ap_list_area);
                        for (uint32_t r = 0; r < rows; r++) {
                            lv_obj_t *row = lv_obj_get_child(cb_ap_list_area, r);
                            lv_obj_t *other = row ? lv_obj_get_child(row, 0) : NULL;
                            if (other && other != checkbox) lv_obj_remove_state(other, LV_STATE_CHECKED);
                        }
                    }
                }
                cb.selectAP(idx);
            } else {
                cb.deselectAPs();  // Toggle off - deselect for simplicity
            }
        }, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);

        // RSSI info
        char rssi_buf[16];
        snprintf(rssi_buf, sizeof(rssi_buf), "%ddBm", ap.rssi);
        make_label(cb_row, rssi_buf, &ui_font_pipboy_14, pip_dim());
    }
    cb_last_ap_count = count;
    // F14: re-evaluate the single-select START button against the CURRENT store. It was
    // refreshed only from the row-tap handler, so once a selection lit it, a later rescan --
    // which calls cb.clearAPs() and takes the selection with it -- left it lit with nothing
    // selected. Owner screenshot rssi-07: fresh rows, none highlighted, START still enabled.
    // The row painting above was always correct; it was the BUTTON that never re-asked.
    cb_rssi_refresh_start();
}

// Populate Station checkbox list
static void cb_populate_sta_list(void) {
    if (!cb_sta_list_area) return;
    int count = cb.getStationCount();
    if (count == cb_last_sta_count) return;

    if (cb_last_sta_count == 0) clear_children(cb_sta_list_area);
    for (int i = cb_last_sta_count; i < count; i++) {
        CBStationInfo sta;
        if (!cb.getStation(i, sta)) continue;

        lv_obj_t *chk = lv_checkbox_create(cb_sta_list_area);
        lv_checkbox_set_text(chk, cb.macToString(sta.mac).c_str());
        lv_obj_set_style_text_font(chk, &ui_font_pipboy_14, 0);
        lv_obj_set_style_text_color(chk, pip_primary(), 0);

        if (cb.isStationSelected(i)) lv_obj_add_state(chk, LV_STATE_CHECKED);

        lv_obj_add_event_cb(chk, [](lv_event_t *e) {
            int idx = (int)(intptr_t)lv_event_get_user_data(e);
            cb.selectStation(idx);
        }, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
    }
    cb_last_sta_count = count;
}

// WiFi join UI refs (main state vars declared at top of file)
static lv_obj_t *wifi_ssid_label     = NULL;
static lv_obj_t *wifi_pw_label       = NULL;
static lv_timer_t *wifi_connect_timer = NULL;
static uint8_t     wifi_connect_tries = 0;
#define WIFI_CONNECT_MAX_TRIES 20   // 20 x 500ms = 10 sec max

// Timer callback: poll scan results every 500ms
static bool is_espressif_oui(const uint8_t *b);  // fwd (defined with the Espressif poller)

static void cb_scan_poll_cb(lv_timer_t *t) {
    (void)t;

    // Update status bar with live counts - type-aware
    if (cb_op_running && lbl_stask) {
        char sbuf[40];
        // Translate the running tool's position index to its stable category id
        // and grab the ToolItem (BT tools are now scattered across categories,
        // so radio type is decided per-tool, not per-category).
        uint8_t spos = (cb_op_encoded >= 0) ? tool_cat(cb_op_encoded) : 255;
        uint8_t sii  = (cb_op_encoded >= 0) ? tool_item(cb_op_encoded) : 0;
        uint8_t sc   = (spos < NUM_TOOL_CATS) ? tool_categories[spos].id : 255;
        const ToolItem *swi = (spos < NUM_TOOL_CATS && sii < tool_categories[spos].count)
                              ? &tool_categories[spos].items[sii] : NULL;
        // Tool-SPECIFIC live counts first -- each reads the list/counter its scan actually
        // fills. These MUST precede the broad bluetooth / Analyze / WiFi-default branches
        // below, which otherwise show a fixed 0 or the wrong metric (audit 2026-07-08).
        if (sc == 0 && sii == 2) {          // Detect > Flipper Zero
            snprintf(sbuf, sizeof(sbuf), "%d Flippers", cb.getFlipperCount());
        } else if (sc == 0 && sii == 3) {   // Detect > Flock Batteries
            // Show HOW LONG AGO the freshest camera was last heard, not just how many are known.
            // Dedup (correctly) stops the list growing on a re-sighting, so a plain count sits at
            // "1 Flock" forever and looks identical to a hung scan -- in the one tool where
            // "is it still there?" is the whole question. An age answers that and is
            // SELF-VERIFYING: it ticks up with the wall clock (so the readout is visibly alive)
            // and drops to ~0 on each sighting (so the user can see the badge still hearing it).
            // A rising advert COUNT was the other candidate and was rejected on measurement: the
            // pre-fix control logged 13 adverts in 45 s (~0.29/s), and WiFiScan::main() cycles the
            // BLE scan off and on about every second, so such a counter would itself sit frozen
            // for several refreshes at a time -- reproducing the symptom it was meant to cure.
            int fcount = cb.getFlockDeviceCount();
            uint32_t newest = 0;
            bool have_age = false;
            for (int i = 0; i < fcount; i++) {
                CBFlockInfo fi;
                if (!cb.getFlockDevice(i, fi)) continue;
                // A torn LinkedList node-cache read makes get() hand back a value-initialised
                // entry while STILL returning true, so last_seen can be 0. At i>0 the signed
                // compare rejects it; at i==0 it would latch have_age on newest=0 and render
                // millis() as the age for one frame. No producer ever stores 0, so skipping it
                // costs nothing real.
                if (fi.last_seen == 0) continue;
                if (!have_age || (int32_t)(fi.last_seen - newest) > 0) {
                    newest = fi.last_seen;
                    have_age = true;
                }
            }
            if (have_age) {
                // Unsigned subtraction, so this stays correct across the millis() rollover.
                unsigned long age_s = (unsigned long)((millis() - newest) / 1000UL);
                // Switch to minutes rather than clamping seconds. A clamp SATURATES -- past the
                // ceiling the number stops moving, which is precisely the frozen-display symptom
                // this feature exists to remove. Both forms stay inside lbl_stask's ~21-char
                // budget: "50 Flock, 599s ago" and "50 Flock, 99m ago" are 18 and 17 chars.
                if (age_s <= 599UL) {
                    snprintf(sbuf, sizeof(sbuf), "%d Flock, %lus ago", fcount, age_s);
                } else {
                    unsigned long age_m = age_s / 60UL;
                    if (age_m > 99UL) age_m = 99UL;   // 99m+ is "ages ago"; keeps the width bound
                    snprintf(sbuf, sizeof(sbuf), "%d Flock, %lum ago", fcount, age_m);
                }
            } else {
                snprintf(sbuf, sizeof(sbuf), "%d Flock", fcount);
            }
        } else if (sc == 0 && sii == 4) {   // Detect > Rogue AP (pinescan)
            snprintf(sbuf, sizeof(sbuf), "%d rogue", cb.getPinescanCount());
        } else if (sc == 0 && sii == 5) {   // Detect > Evil Twin (multi-SSID)
            snprintf(sbuf, sizeof(sbuf), "%d multi-SSID", cb.getMultiSSIDCount());
        } else if (sc == 0 && sii == 6) {   // Detect > Remote ID (drones)
            snprintf(sbuf, sizeof(sbuf), "%d drones", drone_count());
        } else if (sc == 1 && sii == 4) {   // Scan > BLE Adverts (frame counter)
            snprintf(sbuf, sizeof(sbuf), "%d BLE pkts", cb.getBTFrames());
        } else if (sc == 2 && sii == 1) {   // Monitor > Packet Rate (total frames)
            CBPacketCounters pc = cb.getPacketCounters();
            snprintf(sbuf, sizeof(sbuf), "%lu frames",
                     (unsigned long)(pc.mgmtFrames + pc.dataFrames));
        } else if (sc == 2 && sii == 2) {   // Monitor > Signal (selected AP RSSI)
            const char *r = "no target";
            char rb[16];
            for (int i = 0; i < cb.getAPCount(); i++) {
                if (!cb.isAPSelected(i)) continue;
                CBAccessPointInfo ap;
                if (cb.getAP(i, ap)) { snprintf(rb, sizeof(rb), "%d dBm", ap.rssi); r = rb; }
                break;
            }
            snprintf(sbuf, sizeof(sbuf), "%s", r);
        } else if (sc == 2 && sii == 3) {   // Monitor > Channel Stats
            CBChannelActivity ca = cb.getChannelActivity();
            unsigned long tot = 0; int ch = 0;
            for (int i = 0; i < 14; i++) { tot += ca.counts[i]; if (ca.counts[i]) ch++; }
            snprintf(sbuf, sizeof(sbuf), "%lu fr / %d ch", tot, ch);
        } else if (sc == 2 && sii == 4) {   // Monitor > MAC Tracker
            // getMACTrackerTop10 does a full-table scan; this status poller runs every
            // ~500ms, so cache the count and refresh it only every ~4th tick (~2s).
            static uint8_t mac_sb_skip = 0, mac_sb_cnt = 0;
            if (mac_sb_skip++ % 4 == 0) {
                MacEntry tmp[10];
                mac_sb_cnt = cb.getMACTrackerTop10(tmp, MacSortMode::MOST_FRAMES);
            }
            snprintf(sbuf, sizeof(sbuf), "%u MACs", mac_sb_cnt);
        } else if (sc == 3 && sii == 1) {   // Analyze > Probes
            snprintf(sbuf, sizeof(sbuf), "%d SSIDs", cb.getProbeSSIDCount());
        } else if (sc == 3 && sii == 4) {   // Analyze > Pwnagotchi
            snprintf(sbuf, sizeof(sbuf), "%d pwn", cb.getPwnagotchiCount());
        } else if (sc == 3 && sii == 6) {   // Analyze > SAE Commit
            snprintf(sbuf, sizeof(sbuf), "SAE:%lu",
                     (unsigned long)cb.getPacketCounters().saeFrames);
        } else if (sc == 3 && sii == 7) {   // Analyze > EAPOL/PMKID
            CBPacketCounters pc = cb.getPacketCounters();
            snprintf(sbuf, sizeof(sbuf), "EAP:%lu", (unsigned long)pc.eapolFrames);
        } else if (swi && tool_is_bluetooth(swi)) {
            // BT scan/detect/spam (AirTag, Skimmer, generic BT)
            int bt = cb.getBTDeviceCount();
            int at = cb.getAirTagCount();
            snprintf(sbuf, sizeof(sbuf), "%d BT, %d tags", bt, at);
        } else if (sc == 3 && sii == 5) {   // Espressif -- count ONLY ESP-OUI APs (match log)
            int esp = 0;
            for (int i = 0; i < cb.getAPCount(); i++) {
                CBAccessPointInfo ap;
                if (cb.getAP(i, ap) && is_espressif_oui(ap.bssid)) esp++;
            }
            snprintf(sbuf, sizeof(sbuf), "%d ESP", esp);
        } else if (sc == 3 && sii == 0) {   // Beacons -- runs an AP scan; show AP count.
            snprintf(sbuf, sizeof(sbuf), "%d APs", cb.getAPCount());
        } else if (sc == 3 && sii == 3) {   // Raw/PCAP -- total frames captured (mgmt+data),
            CBPacketCounters pc = cb.getPacketCounters();  // not DEA/BCN which are often 0
            snprintf(sbuf, sizeof(sbuf), "Cap:%lu",
                     (unsigned long)(pc.mgmtFrames + pc.dataFrames));
        } else if (sc == 3 || sc == 2) {
            // Remaining Analyze/Monitor (Deauth, Raw/PCAP, SAE, Packets) - frame counts.
            CBPacketCounters pc = cb.getPacketCounters();
            // Deauth carries its channel mode. This string is the status bar, readable from all
            // 9 screens, so with Analyze > Deauth running the user can wander the badge reading
            // "DEA:0" with nothing saying the badge is listening to a single channel. Appended
            // ONLY when not hop-all, so the common case is byte-identical to before.
            if (sc == 3 && sii == 2 && cfg.deauth_chan != CB_DEAUTH_HOP_ALL) {
                snprintf(sbuf, sizeof(sbuf), "DEA:%lu %s",
                         (unsigned long)pc.deauthFrames,
                         cb_deauth_mode_short(cfg.deauth_chan));
            } else {
                snprintf(sbuf, sizeof(sbuf), "DEA:%lu BCN:%lu",
                         (unsigned long)pc.deauthFrames, (unsigned long)pc.beaconFrames);
            }
        } else if (sc >= 6 && sc <= 11) {
            // Active research tail - show frames sent
            int sent = cb.getPacketsSent();
            snprintf(sbuf, sizeof(sbuf), "Sent: %d pkts", sent);
        } else {
            // WiFi scan / detect / default. Which count to show comes from the TOOL (or the
            // inline Scan button), not from which list is non-empty: the AP/STA lists persist
            // across tool switches, so a plain AP scan run after a Stations scan used to
            // report both, and the station picker's inline Scan (cb_op_encoded = -1) fell
            // through to the AP phrasing and showed a leftover AP count for its whole run.
            int ap = cb.getAPCount();
            int sta = cb.getStationCount();
            if (cb_active_result_kind() == MON_STA)   // Scan > Stations, or the STA picker
                snprintf(sbuf, sizeof(sbuf), "%d STAs found", sta);
            else if (sc == 1 && sii == 1)   // Scan > APs + Stations (the one both-lists tool)
                snprintf(sbuf, sizeof(sbuf), "%d APs, %d STAs", ap, sta);
            else
                snprintf(sbuf, sizeof(sbuf), "%d APs found", ap);
        }
        lv_label_set_text(lbl_stask, sbuf);
        if (wifi_status_label) lv_label_set_text(wifi_status_label, sbuf);
    }

    if (!cb.isScanning()) {
        // Scan finished - do a final populate and stop polling
        cb_populate_ap_list();
        cb_populate_sta_list();
        int ap_count  = cb.getAPCount();
        int sta_count = cb.getStationCount();
        int bt_count  = cb.getBTDeviceCount();
        int at_count  = cb.getAirTagCount();
        // Report the store the FINISHED tool actually filled -- read the kind BEFORE
        // clearing cb_op_encoded. The old "bt_count > 0 ? BT : ..." guess had the same
        // stale-list bug as the Live Devices table, mirrored: run a BT scan, then an AP
        // scan, and the AP scan's "Done:" reported the leftover BT devices.
        MonType done_kind = cb_active_result_kind();
        // Scan > APs + Stations is the one tool that fills both lists, so it's the only
        // one allowed to report both (otherwise a stale STA list follows an AP scan).
        uint8_t dpos = (cb_op_encoded >= 0) ? tool_cat(cb_op_encoded) : 255;
        bool done_ap_sta = (dpos < NUM_TOOL_CATS) && tool_categories[dpos].id == 1
                           && tool_item(cb_op_encoded) == 1;
        cb_op_running = false;
        cb_op_encoded = -1;
        cb_manual_scan_kind = -1;
        char dbuf[40];
        if (done_kind == MON_BT)
            snprintf(dbuf, sizeof(dbuf), "Done: %d BT, %d tags", bt_count, at_count);
        else if (done_kind == MON_STA)
            snprintf(dbuf, sizeof(dbuf), "Done: %d STAs", sta_count);
        else if (done_ap_sta)
            snprintf(dbuf, sizeof(dbuf), "Done: %d APs, %d STAs", ap_count, sta_count);
        else
            snprintf(dbuf, sizeof(dbuf), "Done: %d APs", ap_count);
        if (lbl_stask) lv_label_set_text(lbl_stask, dbuf);
        if (wifi_status_label) lv_label_set_text(wifi_status_label, dbuf);
        // Update scan button label back to "Scan" if it exists
        if (wifi_scan_btn_label) lv_label_set_text(wifi_scan_btn_label, "Scan");
        if (cb_scan_timer) {
            lv_timer_delete(cb_scan_timer);
            cb_scan_timer = NULL;
        }
        return;
    }
    cb_populate_ap_list();
    cb_populate_sta_list();
}

// Start a scan poll timer
static void cb_start_scan_polling(void) {
    if (cb_scan_timer) lv_timer_delete(cb_scan_timer);
    cb_last_ap_count = 0;
    cb_last_sta_count = 0;
    cb_scan_timer = lv_timer_create(cb_scan_poll_cb, 500, NULL);
}

// ─── Tool action callbacks (real hardware) ───────────────────────────────

// Scan APs button callback
static void cb_scan_aps_cb(lv_event_t *e) {
    (void)e;
    cb_stop_operation();
    cb_ensure_wifi();
    cb.clearAPs();
    cb.scanAPs();
    cb_op_running = true;
    cb_op_name = "Scanning APs";
    cb_manual_scan_kind = MON_AP;
    if (lbl_stask) lv_label_set_text(lbl_stask, cb_op_name);
    cb_start_scan_polling();
}

// Scan Stations button callback
static void cb_scan_stas_cb(lv_event_t *e) {
    (void)e;
    cb_stop_operation();
    cb_ensure_wifi();
    cb.clearStations();
    cb.scanStations();
    cb_op_running = true;
    cb_op_name = "Scanning Stations";
    cb_manual_scan_kind = MON_STA;
    if (lbl_stask) lv_label_set_text(lbl_stask, cb_op_name);
    cb_start_scan_polling();
}

// Deselect all APs
static void cb_deselect_aps_cb(lv_event_t *e) {
    (void)e;
    cb.deselectAPs();
    cb_last_ap_count = 0;  // Force repopulate
    cb_populate_ap_list();
}

// Deselect all stations
static void cb_deselect_stas_cb(lv_event_t *e) {
    (void)e;
    cb.deselectStations();
    cb_last_sta_count = 0;
    cb_populate_sta_list();
}

// Add SSIDs from scanned APs
// A1 -- the SSID pane's list widget, so the two buttons that fill the store can redraw it.
// It was built ONCE at nav time and no timer ever touched it, so "From APs" and "Random" added
// SSIDs to the store while the pane kept reading "SSID list empty" -- and then "> START <"
// beaconed every one of them. That is the AP-list bug INVERTED: the view UNDERSTATES the store,
// and the control the user taps next acts on data the view denies exists. On an active-TX SKU
// that is the material direction. Owner-confirmed on COM5 with screenshots.
static lv_obj_t *cb_ssid_list_area = NULL;

static void cb_populate_ssid_list(void) {
    if (!cb_ssid_list_area || !lv_obj_is_valid(cb_ssid_list_area)) return;
    clear_children(cb_ssid_list_area);
    int n = cb.getSSIDCount();
    if (n <= 0) {
        make_label(cb_ssid_list_area, "SSID list empty", &ui_font_pipboy_14, pip_dim());
        return;
    }
    for (int i = 0; i < n; i++) {
        CBSSIDInfo info;
        if (cb.getSSID(i, info)) {
            info.essid[sizeof(info.essid) - 1] = '\0';
            // make_name_label: these strings are untrusted (harvested ESSIDs), so
            // they go through the same cb_safe() sanitising the scan lists use.
            make_name_label(cb_ssid_list_area, info.essid, &ui_font_pipboy_14, pip_primary());
        }
    }
}

static void cb_ssids_from_aps_cb(lv_event_t *e) {
    (void)e;
    int count = cb.getAPCount();
    int added = 0;
    for (int i = 0; i < count; i++) {
        CBAccessPointInfo ap;
        if (cb.getAP(i, ap)) {
            ap.essid[sizeof(ap.essid) - 1] = '\0';
            if (strlen(ap.essid) > 0) {
                cb.addSSID(String(ap.essid));
                added++;
            }
        }
    }
    // Report what was ADDED, not how many APs were walked -- the old message counted APs
    // including the un-named ones it skipped, so the log overstated the result.
    CB_LOGF("[CB] Added %d SSIDs from %d APs\n", added, count);
    cb_populate_ssid_list();   // store changed -> redraw the view in the same call
}

// Generate random SSIDs
static void cb_random_ssids_cb(lv_event_t *e) {
    (void)e;
    cb.generateSSIDs(20);
    CB_LOGLN("[CB] Generated 20 random SSIDs");
    cb_populate_ssid_list();   // store changed -> redraw the view in the same call
}

// Channel dropdown callback
static void cb_channel_dd_cb(lv_event_t *e) {
    lv_obj_t *dd = (lv_obj_t *)lv_event_get_target(e);
    uint16_t sel = lv_dropdown_get_selected(dd);
    int channel = (sel == 0) ? 0 : (int)sel;  // 0 = Auto
    cb.setChannel(channel);
    CB_LOGF("[CB] Channel set to %d\n", channel);
}

// ─── Live output display helpers ──────────────────────────────────────────

#define CB_LOG_MAX_LEN  4096  // Cap scan output buffer size

// Persistent log buffer - survives navigation. Allocated in PSRAM.
static char  *cb_log_buf     = NULL;
static size_t cb_log_len     = 0;
static size_t cb_log_ui_len  = 0;     // length already pushed to the label
static bool   cb_log_ui_stale = false; // true when buf was trimmed/replaced
static bool   cb_log_pin_top   = false; // MAC Tracker replaces the whole list -> pin to TOP, not bottom

static void cb_log_buf_init(void) {
    if (!cb_log_buf) {
        cb_log_buf = (char *)heap_caps_malloc(CB_LOG_MAX_LEN, MALLOC_CAP_SPIRAM);
        if (cb_log_buf) cb_log_buf[0] = '\0';
        cb_log_len = 0;
    }
    cb_log_ui_len = 0;
    cb_log_ui_stale = false;
}

static void cb_log_buf_clear(void) {
    if (cb_log_buf) { cb_log_buf[0] = '\0'; cb_log_len = 0; }
    cb_log_ui_len = 0;
    cb_log_ui_stale = true;  // whatever's on the label must be wiped
}

// Append to persistent buffer. Does NOT touch the LVGL label - call
// cb_log_flush_to_ui() once per poll to push accumulated text incrementally.
static void cb_log_append(const char *text) {
    if (!cb_log_buf) return;
    size_t tlen = strlen(text);
    if (cb_log_len + tlen >= CB_LOG_MAX_LEN) {
        // Trim: keep most recent half. Invalidates the label - next flush
        // will do a full set_text to resync.
        size_t skip = cb_log_len / 2;
        const char *trimmed = cb_log_buf + skip;
        const char *nl = strchr(trimmed, '\n');
        if (nl) trimmed = nl + 1;
        size_t keep = cb_log_len - (trimmed - cb_log_buf);
        memmove(cb_log_buf, trimmed, keep);
        cb_log_len = keep;
        cb_log_buf[cb_log_len] = '\0';
        cb_log_ui_stale = true;
    }
    memcpy(cb_log_buf + cb_log_len, text, tlen);
    cb_log_len += tlen;
    cb_log_buf[cb_log_len] = '\0';
}

// Replace the whole buffer (used by counter-style displays). Always forces
// a full label set_text on next flush.
static void cb_log_replace(const char *text) {
    if (cb_log_buf) {
        // Keep the "capture couldn't be saved" warning pinned above the live stats.
        // The poller calls cb_log_replace every tick, so without this the notice
        // logged once at tool start would vanish within one poll interval -- a
        // warning the user must act on has to persist, not flash for 800ms.
        if (cb_pcap_write_failed) {
            snprintf(cb_log_buf, CB_LOG_MAX_LEN,
                     "PCAP NOT SAVED (SD full / too many files). Clear /pcaps.\n%s", text);
        } else if (cb_pcap_seq >= 5000) {
            // Pile-up advisory pinned above the live stats (else the 800ms poller wipes
            // the start-time note). O(1): cb_pcap_seq is the NVS name-index, not a scan.
            snprintf(cb_log_buf, CB_LOG_MAX_LEN,
                     "5000+ pcaps: captures lag. Clear /pcaps. See Help.\n%s", text);
        } else if (cb_pcap_seq >= 500) {
            snprintf(cb_log_buf, CB_LOG_MAX_LEN,
                     "500+ pcaps: saves slowing. Clear /pcaps. See Help.\n%s", text);
        } else {
            strncpy(cb_log_buf, text, CB_LOG_MAX_LEN - 1);
            cb_log_buf[CB_LOG_MAX_LEN - 1] = '\0';
        }
        cb_log_len = strlen(cb_log_buf);
    }
    cb_log_ui_stale = true;
}

// Push any pending buffer content to the label. Uses lv_label_ins_text for
// the common append case (O(new_bytes), no full relayout) and falls back to
// full set_text only after a trim/replace. Call this once per poll tick.
static void cb_log_flush_to_ui(void) {
    if (!cb_output_log) return;
    bool changed = false;
    if (cb_log_ui_stale) {
        lv_label_set_text(cb_output_log, cb_log_buf ? cb_log_buf : "");
        cb_log_ui_len = cb_log_len;
        cb_log_ui_stale = false;
        changed = true;
    } else if (cb_log_ui_len < cb_log_len) {
        lv_label_ins_text(cb_output_log, LV_LABEL_POS_LAST,
                          cb_log_buf + cb_log_ui_len);
        cb_log_ui_len = cb_log_len;
        changed = true;
    }
    if (changed && cb_output_scroll) {
        // ANIM_OFF: animation was a CPU cost we couldn't afford during heavy scans.
        // pin_top tools (MAC Tracker) replace the whole list each tick and want the top
        // rows visible; append-style tools want the newest line at the bottom.
        lv_obj_scroll_to_y(cb_output_scroll, cb_log_pin_top ? 0 : LV_COORD_MAX, LV_ANIM_OFF);
    }
}

// ═══════════════ Wireshark-style result monitor popup ═══════════════════════
// Full-screen modal table over the LIVE scan results, read straight from cb's
// result store (no poller rewire). A selection steps through rows via big
// prev/next buttons + a detail strip; rows append live while capture runs.
// Pause-follow: new rows do NOT move your selection unless you're already on the
// latest row (or hit "Latest"). Scope: AP / STA / BT scans (the common ones).
// MonType + tool_result_kind() live up with the tool tables (the status bar needs them too).
static lv_obj_t   *mon_modal  = NULL;
static lv_obj_t   *mon_list   = NULL;   // scrollable rows container
static lv_obj_t   *mon_detail = NULL;   // detail strip label
static lv_obj_t   *mon_title  = NULL;   // title + running counts
static lv_timer_t *mon_timer  = NULL;   // live-refresh poll
static MonType     mon_type   = MON_AP;
static int         mon_sel    = -1;     // selected row index (-1 = none)
static int         mon_shown  = 0;      // rows already built into mon_list
static bool        mon_follow = true;   // auto-track the newest row
// Data index of child 0. Rows are trimmed from the HEAD once MON_MAX_ROWS is exceeded
// (FB14), and mon_sel is a DATA index (mon_get/mon_update_detail use it) while the rows are
// addressed by CHILD index -- so without this offset a trimmed table would paint and detail
// the wrong device. child_index = data_index - mon_row_first.
static int         mon_row_first = 0;

#define MON_COL_TYPE 30
#define MON_COL_SIG  36
#define MON_COL_CH   26

struct MonView { char type[4]; char sig[8]; char ch[4]; char name[48]; char detail[176]; bool warn; };

static const char *mon_sec_str(uint8_t s) {
    switch (s) { case 0: return "Open"; case 1: return "WEP";  case 2: return "WPA";
                 case 3: return "WPA2"; case 4: return "WPA3"; case 5: return "WPA2/3"; }
    return "sec?";
}
static lv_color_t mon_type_color(void) {
    switch (mon_type) { case MON_STA: return pip_dim(); case MON_BT: return pip_accent();
                        default: return pip_primary(); }
}
static const char *mon_type_plural(void) {
    switch (mon_type) { case MON_STA: return "STA"; case MON_BT: return "BT"; default: return "APs"; }
}

static int mon_count(void) {
    switch (mon_type) { case MON_STA: return cb.getStationCount();
                        case MON_BT:  return cb.getBTDeviceCount();
                        default:      return cb.getAPCount(); }
}

static bool mon_get(int i, MonView &v) {
    v.type[0] = v.sig[0] = v.ch[0] = v.name[0] = v.detail[0] = '\0';
    v.warn = false;
    if (mon_type == MON_AP) {
        CBAccessPointInfo ap;
        if (!cb.getAP(i, ap)) return false;
        ap.essid[sizeof(ap.essid) - 1] = '\0';
        strcpy(v.type, "AP");
        snprintf(v.sig, sizeof(v.sig), "%d", ap.rssi);
        snprintf(v.ch, sizeof(v.ch), "%d", ap.channel);
        const char *nm = ap.essid[0] ? cb_safe(ap.essid) : "<hidden>";
        v.warn = cb_safe_had_hostile;
        strncpy(v.name, nm, sizeof(v.name) - 1); v.name[sizeof(v.name) - 1] = '\0';
        // The WPS manufacturer string is raw bytes from a received probe response
        // (WiFiScan.cpp:6243 memcpy's it straight out of the frame with no validation), and it
        // was interpolated UNSANITISED two lines below an essid that IS sanitised. So a hostile
        // AP could put bidi overrides or zero-width characters into the detail strip -- which
        // is never warn-coloured -- and reorder or hide the BSSID/channel/RSSI text around it,
        // defeating the very □/■ scheme cb_safe() exists to provide.
        // Sanitised into a LOCAL first, and the hostile flag OR'd separately: cb_safe_had_hostile
        // is a single global overwritten by each call, and snprintf argument evaluation order is
        // unspecified, so calling cb_safe() inside the argument list would sample it unreliably.
        char mfg[64] = "";
        if (ap.manufacturer[0]) {
            strncpy(mfg, cb_safe(ap.manufacturer), sizeof(mfg) - 1);
            mfg[sizeof(mfg) - 1] = '\0';
            v.warn |= cb_safe_had_hostile;
        }
        snprintf(v.detail, sizeof(v.detail),
            "BSSID %02X:%02X:%02X:%02X:%02X:%02X\nch %d   %d dBm   %s%s\n%s",
            ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4], ap.bssid[5],
            ap.channel, ap.rssi, mon_sec_str(ap.security), ap.wps ? "  WPS" : "",
            mfg);
        return true;
    } else if (mon_type == MON_STA) {
        CBStationInfo st;
        if (!cb.getStation(i, st)) return false;
        strcpy(v.type, "STA");
        snprintf(v.name, sizeof(v.name), "%02X:%02X:%02X:%02X:%02X:%02X",
            st.mac[0], st.mac[1], st.mac[2], st.mac[3], st.mac[4], st.mac[5]);
        snprintf(v.detail, sizeof(v.detail), "%s\n%u packets%s",
            v.name, st.packets, st.apIndex >= 0 ? "   (associated)" : "");
        return true;
    } else { // MON_BT
        CBBTDeviceInfo bt;
        if (!cb.getBTDevice(i, bt)) return false;
        bt.name[sizeof(bt.name) - 1] = '\0';
        strcpy(v.type, "BT");
        snprintf(v.sig, sizeof(v.sig), "%d", bt.rssi);
        const char *nm = bt.name[0] ? cb_safe(bt.name) : "<no name>";
        v.warn = cb_safe_had_hostile;
        strncpy(v.name, nm, sizeof(v.name) - 1); v.name[sizeof(v.name) - 1] = '\0';
        snprintf(v.detail, sizeof(v.detail), "%s\n%s   %d dBm", v.name, bt.mac, bt.rssi);
        return true;
    }
}

static void mon_update_detail(void) {
    if (!mon_detail) return;
    MonView v;
    if (mon_sel >= 0 && mon_get(mon_sel, v))
        lv_label_set_text(mon_detail, v.detail);
    else
        lv_label_set_text(mon_detail, "");
}

static void mon_row_paint(lv_obj_t *row, bool selected) {
    if (!row) return;
    lv_obj_set_style_bg_color(row, pip_highlight(), 0);
    lv_obj_set_style_bg_opa(row, selected ? LV_OPA_30 : LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(row, pip_highlight(), 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(row, selected ? 3 : 0, 0);
}

// Select row i. user=true means a manual prev/next/tap (updates follow state).
static void mon_select(int i, bool user) {
    int n = mon_count();
    if (n <= 0) { mon_sel = -1; mon_update_detail(); return; }
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    int built = (int)lv_obj_get_child_count(mon_list);
    int prev_ci = mon_sel - mon_row_first;          // data index -> child index (FB14)
    if (prev_ci >= 0 && prev_ci < built)
        mon_row_paint(lv_obj_get_child(mon_list, prev_ci), false);
    mon_sel = i;
    int ci = i - mon_row_first;
    if (ci >= 0 && ci < built) {
        lv_obj_t *row = lv_obj_get_child(mon_list, ci);
        mon_row_paint(row, true);
        if (row) lv_obj_scroll_to_view(row, LV_ANIM_ON);
    }
    if (user) mon_follow = (i == n - 1);   // manually landing on the last row re-arms follow
    mon_update_detail();
}

static void mon_make_row(int i) {
    MonView v;
    if (!mon_get(i, v)) return;
    lv_obj_t *row = lv_obj_create(mon_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_hor(row, 2, 0);
    lv_obj_set_style_pad_ver(row, 1, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(row, (void *)(intptr_t)i);
    lv_obj_add_event_cb(row, [](lv_event_t *e) {
        // Rows are plain lv_obj (not button-class), so the global click-sound
        // hook skips them -- play the tap here to match the buttons.
        audio_play_click();
        int idx = (int)(intptr_t)lv_obj_get_user_data((lv_obj_t *)lv_event_get_target(e));
        mon_select(idx, true);
    }, LV_EVENT_CLICKED, NULL);

    auto col = [&](const char *t, int w, bool grow, lv_color_t c) {
        lv_obj_t *l = lv_label_create(row);
        lv_label_set_text(l, t);
        lv_obj_set_style_text_font(l, &ui_font_pipboy_14, 0);
        lv_obj_set_style_text_color(l, c, 0);
        if (grow) {
            lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
            lv_obj_set_flex_grow(l, 1);
            lv_obj_set_width(l, lv_pct(100));
        } else {
            lv_obj_set_width(l, w);
        }
    };
    col(v.type, MON_COL_TYPE, false, mon_type_color());
    col(v.sig,  MON_COL_SIG,  false, pip_dim());
    col(v.ch,   MON_COL_CH,   false, pip_dim());
    col(v.name, 0, true, v.warn ? lv_color_hex(CB_WARN_RGB) : pip_primary());
    mon_row_paint(row, false);
}

static void mon_update_title(void) {
    if (!mon_title) return;
    char buf[40];
    snprintf(buf, sizeof(buf), "MONITOR   %d %s", mon_count(), mon_type_plural());
    lv_label_set_text(mon_title, buf);
}

// Append any rows that arrived since last build; follow the newest if armed.
// Newest-N window for the live table (audit FB14). One row is ~1.0-1.4 KB of PSRAM (5 LVGL
// objects), and mon_select()'s lv_obj_scroll_to_view() forces lv_obj_update_layout() over the
// whole tree TWICE A SECOND. A dense con floor puts several hundred devices in the AP/BT lists,
// so the table grew to thousands of PSRAM objects being flex-laid-out at 2 Hz: progressive jank,
// then a WDT reboot around ~6000 rows (LV_USE_ASSERT_MALLOC + LV_ASSERT_HANDLER while(1)),
// sooner if Collectibles had already claimed its 3.7 MB.
#define MON_MAX_ROWS 200

static void mon_append_rows(void) {
    if (!mon_list) return;
    int n = mon_count();
    for (; mon_shown < n; mon_shown++) mon_make_row(mon_shown);
    // Trim from the HEAD so the newest rows survive -- this is a live monitor, and the row a
    // user cares about is the one that just appeared.
    uint32_t built = lv_obj_get_child_count(mon_list);
    while (built > MON_MAX_ROWS) {
        lv_obj_t *oldest = lv_obj_get_child(mon_list, 0);
        if (!oldest) break;
        lv_obj_delete(oldest);
        built--;
        mon_row_first++;        // keep child_index = data_index - mon_row_first true
    }
    // If the selected DEVICE scrolled out of the retained window, drop the selection rather
    // than silently detailing whichever device now sits at that child index.
    if (mon_sel >= 0 && mon_sel < mon_row_first) mon_sel = -1;
    mon_update_title();
    if (mon_follow && n > 0) mon_select(n - 1, false);
}

static void mon_refresh_cb(lv_timer_t *t) { (void)t; mon_append_rows(); }

static void mon_popup_close(void) {
    if (mon_timer) { lv_timer_delete(mon_timer); mon_timer = NULL; }
    if (mon_modal) { lv_obj_delete(mon_modal); mon_modal = NULL; }
    mon_list = mon_detail = mon_title = NULL;
    mon_sel = -1; mon_shown = 0; mon_row_first = 0;
}

static void mon_nav_cb(lv_event_t *e) {
    int dir = (int)(intptr_t)lv_event_get_user_data(e);  // -1 prev, +1 next, 0 latest
    if (dir == 0) { mon_follow = true; mon_select(mon_count() - 1, false); }
    else          { mon_select((mon_sel < 0 ? 0 : mon_sel) + dir, true); }
}

// `type` is decided by the TOOL that owns the button (bound at button-creation time),
// never by which result list happens to be non-empty -- see tool_result_kind().
static void mon_popup_open(MonType type) {
    mon_type = type;

    mon_popup_close();  // defensive: never stack two
    mon_modal = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(mon_modal);
    lv_obj_set_size(mon_modal, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(mon_modal, pip_bg(), 0);
    lv_obj_set_style_bg_opa(mon_modal, LV_OPA_COVER, 0);
    lv_obj_remove_flag(mon_modal, LV_OBJ_FLAG_SCROLLABLE);

    // Title bar + close
    lv_obj_t *bar = lv_obj_create(mon_modal);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, SCREEN_W, 22);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(bar, 4, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    mon_title = lv_label_create(bar);
    lv_label_set_text(mon_title, "");   // same "Text" default as mon_detail; set before paint
                                        // today, but not by construction -- so pin it here
    lv_obj_set_style_text_font(mon_title, &ui_font_pipboy_16, 0);
    lv_obj_set_style_text_color(mon_title, pip_highlight(), 0);
    lv_obj_t *xbtn = lv_button_create(bar);
    lv_obj_set_size(xbtn, 36, 20);
    lv_obj_set_style_bg_color(xbtn, pip_highlight(), 0);
    lv_obj_add_event_cb(xbtn, [](lv_event_t *e){ (void)e; mon_popup_close(); }, LV_EVENT_CLICKED, NULL);
    lv_obj_t *xl = lv_label_create(xbtn);
    lv_label_set_text(xl, "X");
    lv_obj_set_style_text_color(xl, pip_bg(), 0);
    lv_obj_center(xl);

    // Column header (fixed, dim) -- the "instrument readout" tell
    lv_obj_t *hdr = lv_obj_create(mon_modal);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, SCREEN_W, 16);
    lv_obj_set_pos(hdr, 0, 22);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_hor(hdr, 2, 0);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    auto hcol = [&](const char *t, int w, bool grow) {
        lv_obj_t *l = lv_label_create(hdr);
        lv_label_set_text(l, t);
        lv_obj_set_style_text_font(l, &ui_font_pipboy_14, 0);
        lv_obj_set_style_text_color(l, pip_dim(), 0);
        if (grow) { lv_obj_set_flex_grow(l, 1); lv_obj_set_width(l, lv_pct(100)); }
        else lv_obj_set_width(l, w);
    };
    hcol("T", MON_COL_TYPE, false); hcol("SIG", MON_COL_SIG, false);
    hcol("CH", MON_COL_CH, false);  hcol("NAME / ADDRESS", 0, true);

    // Scrollable rows
    mon_list = lv_obj_create(mon_modal);
    lv_obj_remove_style_all(mon_list);
    lv_obj_set_size(mon_list, SCREEN_W, 124);
    lv_obj_set_pos(mon_list, 0, 38);
    lv_obj_set_flex_flow(mon_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(mon_list, 0, 0);
    lv_obj_set_style_pad_gap(mon_list, 0, 0);
    lv_obj_set_scroll_dir(mon_list, LV_DIR_VER);
    lv_obj_add_style(mon_list, &style_scrollbar, LV_PART_SCROLLBAR);

    // Detail strip
    mon_detail = lv_label_create(mon_modal);
    // LVGL's default label content is the literal "Text", and this strip is only written when a
    // ROW IS SELECTED -- so on an empty/just-opened table the badge displayed the word "Text" in
    // the lower left until the first device arrived and was tapped. Owner caught it on hardware.
    // Set it empty at creation: an explicit initial value costs nothing and cannot be forgotten
    // the way "the refresh will fill it in" can.
    lv_label_set_text(mon_detail, "");
    lv_label_set_long_mode(mon_detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_size(mon_detail, SCREEN_W - 8, 44);
    lv_obj_set_pos(mon_detail, 4, 164);
    lv_obj_set_style_text_font(mon_detail, &ui_font_pipboy_14, 0);
    lv_obj_set_style_text_color(mon_detail, pip_primary(), 0);

    // Prev / Latest / Next buttons (big touch targets)
    lv_obj_t *brow = lv_obj_create(mon_modal);
    lv_obj_remove_style_all(brow);
    lv_obj_set_size(brow, SCREEN_W, 28);
    lv_obj_set_pos(brow, 0, SCREEN_H - 28);
    lv_obj_set_flex_flow(brow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(brow, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(brow, LV_OBJ_FLAG_SCROLLABLE);
    auto navbtn = [&](const char *t, int dir) {
        lv_obj_t *b = lv_button_create(brow);
        lv_obj_set_size(b, 98, 26);
        lv_obj_set_style_bg_color(b, pip_primary(), 0);
        lv_obj_add_event_cb(b, mon_nav_cb, LV_EVENT_CLICKED, (void *)(intptr_t)dir);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, t);
        lv_obj_set_style_text_font(l, &ui_font_pipboy_16, 0);
        lv_obj_set_style_text_color(l, pip_bg(), 0);
        lv_obj_center(l);
    };
    navbtn("< PREV", -1); navbtn("LATEST", 0); navbtn("NEXT >", 1);

    // Build current rows, select latest, then poll for live updates.
    mon_shown = 0; mon_follow = true; mon_sel = -1;
    mon_append_rows();
    mon_timer = lv_timer_create(mon_refresh_cb, 500, NULL);
}

// ─── Monitor charts (adapted from menutest/pip_output.h) ────────────────────
// Monitor > Packets / Packet Rate / RSSI / Channel Stats render an lv_chart instead of the
// text log. Rate/RSSI are scrolling LINE time-series; Packets/Channel are BAR charts.
#define CB_TS_POINTS 30
static lv_obj_t *cb_output_area  = NULL;   // wraps the whole output (chart/log + table btn)
static lv_obj_t *cb_tool_desc    = NULL;   // the tool's description label (hidden while running for output room)
static lv_obj_t *cb_output_chart = NULL;
static lv_chart_series_t *cb_chart_series = NULL;
static lv_obj_t *cb_chart_ymax_lbl = NULL;
static lv_obj_t *cb_chart_ymid_lbl = NULL;
static uint32_t cb_chart_ymax = 100;
static uint32_t cb_rate_hist[CB_TS_POINTS] = {};
static int16_t  cb_rssi_hist[CB_TS_POINTS];
static uint8_t  cb_smooth_chan[14] = {};
static uint32_t cb_chart_last_total = 0;

// ─── Monitor > RSSI loss-of-signal indicator ────────────────────────────────
// A flat RSSI trace means EITHER a steady signal OR a target that stopped transmitting, and
// nothing on screen distinguished them: `ap.rssi` is written only when a frame from that BSSID
// arrives, and nothing ages it. Proven on the rig 2026-07-26 -- the readout sat at -44 dBm,
// pixel-identical, 25 s after the beacon was killed and the injector interface downed.
// `AccessPoint` has no last-seen timestamp, but the wrapper DOES expose `packets`, which only
// advances when a frame from that BSSID is received. So the freshness signal is available
// without touching the vendored struct: watch the counter, not the clock.
// Behaviour (owner's spec): show "no packets for Ns" and HALT chart updates until a new packet
// arrives, then reset the counter and resume. Holding the trace still is deliberate -- appending
// a stale value every tick is what made a dead target look like a live one.
static lv_obj_t *cb_rssi_state_lbl   = NULL;   // "receiving" / "no packets for Ns"
static uint32_t  cb_rssi_last_pkts   = 0;      // last cb.getSigStrenLastRxMs() sample
static uint32_t  cb_rssi_last_rx_ms  = 0;
static bool      cb_rssi_pkts_valid  = false;  // have we sampled the counter at least once?
// Monotonic count of points APPENDED to the trace. Exposed in tool_state purely so a test can
// prove the HALT directly instead of inferring it from the label: while stalled this must not
// advance, and it must resume when traffic returns. Reading the label would only prove the
// label. (Instrument the state the fix changes, not a downstream consequence of it.)
static uint32_t  cb_rssi_appends     = 0;
#define CB_RSSI_STALL_MS 2000U                 // silence before the trace is declared stalled

// Monitor items that render a chart: 0 Packets, 1 Rate, 2 RSSI, 3 Channel Stats.
// (item 4 MAC Tracker stays a text list.)
static bool cb_tool_is_chart(uint8_t cat_id, uint8_t item) {
    return cat_id == 2 && item <= 3;
}

static uint32_t cb_nice_ceil(uint32_t v) {
    const uint32_t steps[] = {10,20,50,100,200,500,1000,2000,5000};
    for (unsigned i = 0; i < sizeof(steps)/sizeof(steps[0]); i++) if (v <= steps[i]) return steps[i];
    return ((v / 1000) + 1) * 1000;
}
static void cb_chart_set_ymax(uint32_t m) {
    if (m == cb_chart_ymax || !cb_output_chart) return;
    cb_chart_ymax = m;
    lv_chart_set_range(cb_output_chart, LV_CHART_AXIS_PRIMARY_Y, 0, (int32_t)m);
    char b[8];
    if (cb_chart_ymax_lbl) { snprintf(b, sizeof(b), m >= 1000 ? "%luk" : "%lu", (unsigned long)(m >= 1000 ? m/1000 : m)); lv_label_set_text(cb_chart_ymax_lbl, b); }
    if (cb_chart_ymid_lbl) { uint32_t d = m/2; snprintf(b, sizeof(b), d >= 1000 ? "%luk" : "%lu", (unsigned long)(d >= 1000 ? d/1000 : d)); lv_label_set_text(cb_chart_ymid_lbl, b); }
}

// Build the chart into `parent` for a Monitor tool (item 0-3).
static void cb_create_chart(lv_obj_t *parent, uint8_t item) {
    bool ts = (item == 1 || item == 2);                       // Rate/RSSI = time-series LINE
    int points = (item == 0) ? 3 : (item == 3) ? 14 : CB_TS_POINTS;
    memset(cb_rate_hist, 0, sizeof(cb_rate_hist));
    for (int i = 0; i < CB_TS_POINTS; i++) cb_rssi_hist[i] = -100;
    memset(cb_smooth_chan, 0, sizeof(cb_smooth_chan));
    cb_chart_last_total = 0;
    cb_chart_ymax = 100;
    cb_rssi_last_pkts = 0;
    cb_rssi_pkts_valid = false;         // no sample yet -- the first poll seeds it
    cb_rssi_last_rx_ms = millis();      // start the clock now, not at 0, or the first tick
                                        // would read as "no packets for <uptime>s"

    // Freshness line for RSSI, above the graph (owner's placement). Self-nulls on delete like
    // every other global a periodic timer writes to -- cb_chart_update() runs every 800 ms and
    // both teardown paths free this label.
    if (item == 2) {
        cb_rssi_state_lbl = make_label(parent, "receiving", &ui_font_pipboy_14, pip_dim());
        lv_obj_add_event_cb(cb_rssi_state_lbl, cb_selfnull_on_delete, LV_EVENT_DELETE,
                            &cb_rssi_state_lbl);
    }

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), 130);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_gap(row, 2, 0);

    lv_obj_t *ycol = lv_obj_create(row);
    lv_obj_remove_style_all(ycol);
    lv_obj_set_size(ycol, 28, lv_pct(100));
    lv_obj_remove_flag(ycol, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(ycol, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ycol, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    cb_chart_ymax_lbl = make_label(ycol, (item == 2) ? "0" : "100", &ui_font_pipboy_14, pip_dim());
    lv_obj_add_event_cb(cb_chart_ymax_lbl, cb_selfnull_on_delete, LV_EVENT_DELETE, &cb_chart_ymax_lbl);
    cb_chart_ymid_lbl = make_label(ycol, (item == 2) ? "-50" : "50", &ui_font_pipboy_14, pip_dim());
    lv_obj_add_event_cb(cb_chart_ymid_lbl, cb_selfnull_on_delete, LV_EVENT_DELETE, &cb_chart_ymid_lbl);
    make_label(ycol, (item == 2) ? "-100" : "0", &ui_font_pipboy_14, pip_dim());

    cb_output_chart = lv_chart_create(row);
    lv_obj_add_event_cb(cb_output_chart, cb_selfnull_on_delete, LV_EVENT_DELETE, &cb_output_chart);
    lv_obj_set_flex_grow(cb_output_chart, 1);
    lv_obj_set_height(cb_output_chart, lv_pct(100));
    lv_obj_set_style_bg_color(cb_output_chart, pip_bg_dark(), 0);
    lv_obj_set_style_bg_opa(cb_output_chart, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cb_output_chart, 1, 0);
    lv_obj_set_style_border_color(cb_output_chart, pip_border(), 0);
    lv_obj_set_style_radius(cb_output_chart, 0, 0);
    lv_chart_set_type(cb_output_chart, ts ? LV_CHART_TYPE_LINE : LV_CHART_TYPE_BAR);
    lv_chart_set_point_count(cb_output_chart, points);
    if (item == 2) lv_chart_set_range(cb_output_chart, LV_CHART_AXIS_PRIMARY_Y, -100, 0);
    else           lv_chart_set_range(cb_output_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    cb_chart_series = lv_chart_add_series(cb_output_chart, pip_primary(), LV_CHART_AXIS_PRIMARY_Y);
    if (ts) lv_obj_set_style_line_width(cb_output_chart, 2, LV_PART_ITEMS);
    int32_t init = (item == 2) ? -100 : 0;
    for (int i = 0; i < points; i++) lv_chart_set_value_by_id(cb_output_chart, cb_chart_series, i, init);

    if (item == 0) {
        // Per-bar labels aligned UNDER the 3 bars. A left spacer equal to the y-axis
        // column width pushes the labels over the plot area, and SPACE_AROUND centers
        // each one under its bar (BCN / DEA / PRB).
        lv_obj_t *lrow = lv_obj_create(parent);
        lv_obj_remove_style_all(lrow);
        lv_obj_set_size(lrow, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(lrow, LV_FLEX_FLOW_ROW);
        lv_obj_remove_flag(lrow, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *spc = lv_obj_create(lrow);
        lv_obj_remove_style_all(spc);
        lv_obj_set_size(spc, 30, 1);            // 28px ycol + 2px gap
        lv_obj_t *labs = lv_obj_create(lrow);
        lv_obj_remove_style_all(labs);
        lv_obj_set_flex_grow(labs, 1);
        lv_obj_set_height(labs, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(labs, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(labs, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(labs, LV_OBJ_FLAG_SCROLLABLE);
        make_label(labs, "BCN", &ui_font_pipboy_14, pip_dim());
        make_label(labs, "DEA", &ui_font_pipboy_14, pip_dim());
        make_label(labs, "PRB", &ui_font_pipboy_14, pip_dim());
    } else {
        const char *cap = (item == 1) ? "frames/sec, ch 1-14" :
                          (item == 2) ? "RSSI dBm - selected AP" : "channels 1-14";
        lv_obj_t *c = make_label(parent, cap, &ui_font_pipboy_14, pip_dim());
        lv_label_set_long_mode(c, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(c, lv_pct(100));
        lv_obj_set_style_text_align(c, LV_TEXT_ALIGN_RIGHT, 0);
    }
}

// Refresh the Monitor chart from live data (called each poll tick for chart tools).
static void cb_chart_update(uint8_t item) {
    if (!cb_output_chart || !cb_chart_series) return;
    if (item == 3) {                                          // Channel Stats: peak-hold histogram
        // The vendored channel-activity hop only covers 7 channels per "page" (page 1 =
        // ch 1-7, page 2 = ch 8-14) and defaults to page 1, so ch 8-14 never sampled.
        // Flip the page each tick (~800ms, faster than the 5s snapshot reset) so all 14
        // channels populate. Picked up on the next 100ms hop, no re-init.
        static uint8_t chan_pg = 1;
        chan_pg = (chan_pg == 1) ? 2 : 1;
        cb.setChannelActivityPage(chan_pg);
        CBChannelActivity act = cb.getChannelActivity();
        uint8_t maxv = 1;
        for (int i = 0; i < 14; i++) {
            if (act.counts[i] > cb_smooth_chan[i]) cb_smooth_chan[i] = act.counts[i];
            else if (act.counts[i] > 0)            cb_smooth_chan[i] = act.counts[i];
            else if (cb_smooth_chan[i] > 0)        cb_smooth_chan[i]--;
            if (cb_smooth_chan[i] > maxv) maxv = cb_smooth_chan[i];
            lv_chart_set_value_by_id(cb_output_chart, cb_chart_series, i, cb_smooth_chan[i]);
        }
        cb_chart_set_ymax(cb_nice_ceil(maxv));
    } else if (item == 0) {                                   // Packets: 3-bar type breakdown
        // WIFI_PACKET_MONITOR feeds the num* family (numBeacon/numDeauth/numProbe), NOT the
        // *Frames family (only the Analyze sniff callbacks touch those) -- reading *Frames
        // here left the chart stuck at 0. See CBPacketCounters in ClipBoyMarauder.h.
        CBPacketCounters pc = cb.getPacketCounters();
        uint32_t v[3] = { (uint32_t)pc.numBeacon, (uint32_t)pc.numDeauth, (uint32_t)pc.numProbe };
        uint32_t maxv = 1; for (int i = 0; i < 3; i++) if (v[i] > maxv) maxv = v[i];
        cb_chart_set_ymax(cb_nice_ceil(maxv));
        for (int i = 0; i < 3; i++) lv_chart_set_value_by_id(cb_output_chart, cb_chart_series, i, (int32_t)v[i]);
    } else if (item == 1) {                                   // Packet Rate: scrolling frames/sec
        // Runs the same WIFI_PACKET_MONITOR sniffer (dispatch maps item 1 -> packetMonitor);
        // the old WIFI_SCAN_PACKET_RATE mode produced no global frame total. Rate = delta of
        // the num* running totals per tick.
        CBPacketCounters pc = cb.getPacketCounters();
        uint32_t total = (uint32_t)(pc.numBeacon + pc.numDeauth + pc.numProbe);
        uint32_t delta = (total >= cb_chart_last_total) ? total - cb_chart_last_total : 0;
        cb_chart_last_total = total;
        memmove(&cb_rate_hist[0], &cb_rate_hist[1], (CB_TS_POINTS - 1) * sizeof(uint32_t));
        cb_rate_hist[CB_TS_POINTS - 1] = delta;
        uint32_t maxv = 1; for (int i = 0; i < CB_TS_POINTS; i++) if (cb_rate_hist[i] > maxv) maxv = cb_rate_hist[i];
        cb_chart_set_ymax(cb_nice_ceil(maxv));
        for (int i = 0; i < CB_TS_POINTS; i++) lv_chart_set_value_by_id(cb_output_chart, cb_chart_series, i, (int32_t)cb_rate_hist[i]);
    } else if (item == 2) {                                   // RSSI: scrolling selected-AP rssi
        int16_t  rssi = -100;
        bool     have_sel = false;
        for (int i = 0; i < cb.getAPCount(); i++) {
            CBAccessPointInfo ap;
            if (cb.getAP(i, ap) && cb.isAPSelected(i)) {
                rssi = ap.rssi; have_sel = true; break;
            }
        }
        // ⚠ NOT ap.packets. That was the first attempt and it was WRONG: `packets` is incremented
        // only by checkMatchAP() on the AP-SCAN path and is never touched by the SIG_STREN handler,
        // so during RSSI monitoring it NEVER advances -- which made this indicator report "no
        // packets" permanently, i.e. confidently wrong where the old behaviour was merely
        // ambiguous. Measured on hardware: appends froze at 3 while the target was demonstrably
        // beaconing.
        // ⚠ RETRACTED: an earlier version of this comment added "and pinning the radio to the
        // target's channel changed nothing (so it was not a channel-binding problem either)". IT
        // WAS a channel problem -- that measurement ran on top of a failed ap_scan, so nothing was
        // selected and the pin had nothing to act on. The real cause was that Monitor > RSSI never
        // tuned to the selected AP: beacon_frames stayed at 0 for 12 s while a 5-way fanout ran on
        // ch6, and pinning after the tool started took it to 138 in five seconds. Fixed in
        // ClipBoyMarauder::signalMonitor().
        // `ap.rssi` is no better as a signal: the live handler rewrites it only when the value MOVES
        // (its filter is +/-1 dBm), so a steady target updates nothing at all.
        // The signal is a timestamp stamped in the LIVE SIG_STREN handler on every matched frame,
        // before that filter. ⚠ There are three near-identical SIG_STREN handlers in
        // WiFiScan.cpp; two are unreachable in this mode. The stamp is in the live one -- an
        // adversarial trace was needed to establish which that is, so do not relocate it casually.
        uint32_t pkts = cb.getSigStrenLastRxMs();
        uint32_t now = millis();
        // A CHANGE in the stamp is the evidence a frame arrived. Comparing changes rather than
        // reading the stamp as an absolute time means no reset is needed when the tool starts.
        if (have_sel && (!cb_rssi_pkts_valid || pkts != cb_rssi_last_pkts)) {
            cb_rssi_last_pkts  = pkts;
            cb_rssi_last_rx_ms = now;
            cb_rssi_pkts_valid = true;
        }
        bool stalled = have_sel && cb_rssi_pkts_valid &&
                       (now - cb_rssi_last_rx_ms) >= CB_RSSI_STALL_MS;
        // HALT the trace while stalled. Appending the last-known rssi every tick is precisely
        // what made a dead target indistinguishable from a steady one.
        if (!stalled) {
            memmove(&cb_rssi_hist[0], &cb_rssi_hist[1], (CB_TS_POINTS - 1) * sizeof(int16_t));
            cb_rssi_hist[CB_TS_POINTS - 1] = rssi;
            for (int i = 0; i < CB_TS_POINTS; i++)
                lv_chart_set_value_by_id(cb_output_chart, cb_chart_series, i, cb_rssi_hist[i]);
            cb_rssi_appends++;
        }
        if (cb_rssi_state_lbl) {
            char sb[40];
            if (!have_sel) {
                snprintf(sb, sizeof(sb), "no AP selected");
            } else if (stalled) {
                snprintf(sb, sizeof(sb), "no packets for %lus",
                         (unsigned long)((now - cb_rssi_last_rx_ms) / 1000U));
            } else {
                snprintf(sb, sizeof(sb), "receiving");
            }
            lv_label_set_text(cb_rssi_state_lbl, sb);
            lv_obj_set_style_text_color(cb_rssi_state_lbl,
                stalled ? lv_color_hex(CB_WARN_RGB) : pip_dim(), 0);
        }
    }
    lv_chart_refresh(cb_output_chart);
}

static void cb_create_output_area(lv_obj_t *parent) {
    // Tear down any prior output before building a new one. Each START used to APPEND a
    // fresh chart/log to the right pane (STOP never deleted it), so repeated START/STOP
    // cycles stacked N graphs on top of each other. Wrapping everything in one container
    // and deleting it first makes re-create idempotent for chart AND log tools.
    if (cb_output_area && lv_obj_is_valid(cb_output_area)) lv_obj_delete(cb_output_area);
    cb_output_area = lv_obj_create(parent);
    lv_obj_remove_style_all(cb_output_area);
    lv_obj_set_size(cb_output_area, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cb_output_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(cb_output_area, 3, 0);
    lv_obj_remove_flag(cb_output_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(cb_output_area, cb_selfnull_on_delete, LV_EVENT_DELETE, &cb_output_area);
    parent = cb_output_area;   // everything below builds into the container

    // Reclaim the description's vertical space while a tool is running so the live output
    // (esp. the MAC-tracker top-10 list) gets the full pane. Hidden objects drop out of the
    // flex layout in LVGL 9. Restored on the next nav rebuild.
    if (cb_tool_desc && lv_obj_is_valid(cb_tool_desc)) lv_obj_add_flag(cb_tool_desc, LV_OBJ_FLAG_HIDDEN);
    cb_log_pin_top = false;   // default: newest-at-bottom; MAC Tracker's poller re-pins to top

    // Pop-out: full-screen Wireshark-style table of detected APs / stations / BT devices.
    // It reads ONLY those three lists, so show its button ONLY for the tools that fill one
    // of them: Scan (APs / APs+STA / STA / BT Devices), Analyze Beacons/Espressif (AP
    // scans), Detect Skimmer (BT). It'd be a blank table on every other tool (Monitor
    // counters, Probes/Deauth/etc., AirTag/Flipper/Flock/Rogue/Evil-Twin whose results
    // live in separate lists), so we omit it there. cb_op_encoded is set before this runs.
    uint8_t _mc  = (cb_op_encoded >= 0) ? tool_cat(cb_op_encoded) : 255;
    uint8_t _mi  = (cb_op_encoded >= 0) ? tool_item(cb_op_encoded) : 0;
    uint8_t _mid = (_mc < NUM_TOOL_CATS) ? tool_categories[_mc].id : 255;
    bool has_table = (_mid == 1 && _mi <= 3) ||
                     (_mid == 3 && (_mi == 0 || _mi == 5)) ||
                     (_mid == 0 && _mi == 1);
    if (has_table) {
        lv_obj_t *monbtn = lv_button_create(parent);
        lv_obj_set_width(monbtn, lv_pct(100));
        lv_obj_set_height(monbtn, 22);
        lv_obj_set_style_bg_color(monbtn, pip_highlight(), 0);
        // Bind the table's result type to THIS tool now, at creation. Reading it at tap
        // time would be wrong: cb_op_encoded is reset to -1 when the tool stops (or a scan
        // self-completes), and the button outlives that -- a BT scan that finished would
        // fall back to the AP default and list the previous WiFi scan's APs.
        lv_obj_add_event_cb(monbtn, [](lv_event_t *e){
                                mon_popup_open((MonType)(intptr_t)lv_event_get_user_data(e));
                            },
                            LV_EVENT_CLICKED,
                            (void *)(intptr_t)tool_result_kind(cb_op_encoded));
        lv_obj_t *monl = lv_label_create(monbtn);
        lv_label_set_text(monl, "Live Devices (table)");
        lv_obj_set_style_text_font(monl, &ui_font_pipboy_14, 0);
        lv_obj_set_style_text_color(monl, pip_bg(), 0);
        lv_obj_center(monl);
    }

    // Monitor Packets/Rate/RSSI/Channel render an lv_chart instead of the text log.
    if (cb_tool_is_chart(_mid, _mi)) {
        cb_create_chart(parent, _mi);
        return;
    }

    // Scrollable bordered container for log output
    cb_output_scroll = lv_obj_create(parent);
    lv_obj_remove_style_all(cb_output_scroll);
    lv_obj_set_size(cb_output_scroll, lv_pct(100), 150);   // taller now the desc is hidden while running
    lv_obj_set_style_border_color(cb_output_scroll, pip_border(), 0);
    lv_obj_set_style_border_width(cb_output_scroll, 1, 0);
    lv_obj_set_style_border_side(cb_output_scroll, LV_BORDER_SIDE_FULL, 0);
    lv_obj_set_style_bg_color(cb_output_scroll, pip_bg_dark(), 0);
    lv_obj_set_style_bg_opa(cb_output_scroll, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(cb_output_scroll, 3, 0);
    lv_obj_add_flag(cb_output_scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(cb_output_scroll, LV_DIR_VER);
    lv_obj_add_style(cb_output_scroll, &style_scrollbar, LV_PART_SCROLLBAR);

    cb_output_log = lv_label_create(cb_output_scroll);
    // Self-null the global when this label is destroyed. The content rebuild on
    // nav (e.g. Tools -> Settings) deletes it; without this the pointer dangles
    // and any later touch -- a settings poke, or cb_log_flush_to_ui from a scan
    // still running in the background -- writes to freed heap ("free() target
    // outside heap areas" assert on the next alloc/free).
    lv_obj_add_event_cb(cb_output_log, [](lv_event_t *e) {
        (void)e; cb_output_log = NULL;
    }, LV_EVENT_DELETE, NULL);
    // Restore persistent log buffer contents if resuming
    lv_label_set_text(cb_output_log, (cb_log_buf && cb_log_len > 0) ? cb_log_buf : "");
    lv_label_set_long_mode(cb_output_log, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(cb_output_log, lv_pct(100));
    lv_obj_set_style_text_font(cb_output_log, term_font(), 0);
    lv_obj_set_style_text_color(cb_output_log, pip_primary(), 0);
    // Tighten line spacing so more rows fit (e.g. MAC Tracker's top-10) in the fixed box.
    lv_obj_set_style_text_line_space(cb_output_log, -4, 0);
    // Sync flush-tracker with what's on the label so subsequent appends
    // can use the cheap incremental path.
    cb_log_ui_len = cb_log_len;
    cb_log_ui_stale = false;
    // Scroll to bottom to show latest entries
    if (cb_output_scroll)
        lv_obj_scroll_to_y(cb_output_scroll, LV_COORD_MAX, LV_ANIM_OFF);
}

static int cb_output_last_bt     = 0;
static int cb_output_last_airtag = 0;
static int cb_output_last_flipper = 0;
static int cb_output_last_flock  = 0;
static int cb_output_last_pwna   = 0;
static int cb_output_last_pine   = 0;
static int cb_output_last_multi  = 0;
static int cb_output_last_probe  = 0;
static int cb_output_last_deauth = 0;  // for delta tracking
static int cb_output_last_deauth_ring = 0;  // tracks ring events read
static int cb_output_last_pkts   = 0;
static bool cb_ep_last_has_creds = false;

// Null UI refs when navigating away - timer + buffer keep running in background
static void cb_output_detach_ui(void) {
    cb_output_scroll = NULL;
    cb_output_log = NULL;
}

// Full cleanup - stop timer, clear buffer, reset counters (called on operation stop)
static void cb_output_cleanup(void) {
    if (cb_output_timer) { lv_timer_delete(cb_output_timer); cb_output_timer = NULL; }
    // F1: blank the chart. Deleting the updater timer above left the lv_chart on screen
    // holding its last plotted trace with nothing driving it -- a frozen instrument readout
    // that looks exactly like a live one that has gone quiet. Same "froze at its last values"
    // signature as SB3's theremin bars. The widget itself is UAF-safe (it self-nulls on
    // LV_EVENT_DELETE at its creation site); this is purely about it asserting stale data.
    if (cb_output_chart && lv_obj_is_valid(cb_output_chart) && cb_chart_series) {
        uint32_t pts = lv_chart_get_point_count(cb_output_chart);
        for (uint32_t i = 0; i < pts; i++)
            lv_chart_set_value_by_id(cb_output_chart, cb_chart_series, i, LV_CHART_POINT_NONE);
        lv_chart_refresh(cb_output_chart);
    }
    cb_output_detach_ui();
    cb_log_buf_clear();
    cb_output_last_ap = 0;
    cb_output_last_sta = 0;
    cb_output_last_bt = 0;
    cb_output_last_airtag = 0;
    cb_output_last_flipper = 0;
    cb_output_last_flock = 0;
    cb_output_last_pwna = 0;
    cb_output_last_pine = 0;
    cb_output_last_multi = 0;
    cb_output_last_probe = 0;
    cb_output_last_deauth = 0;
    cb_output_last_deauth_ring = 0;
    cb_output_last_pkts = 0;
    cb_ep_last_has_creds = false;
}

// ── Incremental pollers per result type ─────────────────────────────────

static void cb_poll_aps(void) {
    int count = cb.getAPCount();
    while (cb_output_last_ap < count) {
        CBAccessPointInfo ap;
        if (cb.getAP(cb_output_last_ap, ap)) {
            ap.essid[sizeof(ap.essid) - 1] = '\0';
            char line[80];
            snprintf(line, sizeof(line), "AP: %s [ch:%d] %ddBm\n",
                     cb_safe(ap.essid), ap.channel, ap.rssi);
            cb_log_append(line);
        }
        cb_output_last_ap++;
    }
}

static void cb_poll_stations(void) {
    int count = cb.getStationCount();
    while (cb_output_last_sta < count) {
        CBStationInfo sta;
        if (cb.getStation(cb_output_last_sta, sta)) {
            char line[64];
            snprintf(line, sizeof(line), "STA: %02X:%02X:%02X:%02X:%02X:%02X (%d pkts)\n",
                     sta.mac[0], sta.mac[1], sta.mac[2],
                     sta.mac[3], sta.mac[4], sta.mac[5], sta.packets);
            cb_log_append(line);
        }
        cb_output_last_sta++;
    }
}

static void cb_poll_bt_devices(void) {
    int count = cb.getBTDeviceCount();
    while (cb_output_last_bt < count) {
        CBBTDeviceInfo dev;
        if (cb.getBTDevice(cb_output_last_bt, dev)) {
            dev.name[sizeof(dev.name) - 1] = '\0';
            char line[80];
            snprintf(line, sizeof(line), "BT: %s [%s] %ddBm\n",
                     dev.name[0] ? cb_safe(dev.name) : "??", dev.mac, dev.rssi);
            cb_log_append(line);
        }
        cb_output_last_bt++;
    }
}

static void cb_poll_airtags(void) {
    int count = cb.getAirTagCount();
    while (cb_output_last_airtag < count) {
        CBAirTagInfo tag;
        if (cb.getAirTag(cb_output_last_airtag, tag)) {
            char line[64];
            snprintf(line, sizeof(line), "TAG: %s %ddBm\n", tag.mac, tag.rssi);
            cb_log_append(line);
        }
        cb_output_last_airtag++;
    }
}

static void cb_poll_flippers(void) {
    int count = cb.getFlipperCount();
    while (cb_output_last_flipper < count) {
        CBFlipperInfo flip;
        if (cb.getFlipper(cb_output_last_flipper, flip)) {
            flip.name[sizeof(flip.name) - 1] = '\0';
            char line[80];
            snprintf(line, sizeof(line), "FLIP: %s [%s]\n",
                     flip.name[0] ? cb_safe(flip.name) : "??", flip.mac);
            cb_log_append(line);
        }
        cb_output_last_flipper++;
    }
}

static void cb_poll_flock(void) {
    int count = cb.getFlockDeviceCount();
    while (cb_output_last_flock < count) {
        CBFlockInfo fl;
        if (cb.getFlockDevice(cb_output_last_flock, fl)) {
            char line[96];
            snprintf(line, sizeof(line), "FLOCK: %s [%s] %ddBm\n",
                     fl.name[0] ? cb_safe(fl.name) : "??", fl.mac, fl.rssi);
            cb_log_append(line);
        }
        cb_output_last_flock++;
    }
}

// Remote ID (drone) poller. The drone store recycles/updates slots in place
// (keyed by UAS serial), so we log each slot's contact once when it first
// appears and reset the flag when the slot frees. Status bar shows live count.
static uint8_t cb_drone_logged[DRONE_MAX] = {0};
static void cb_poll_drone(void) {
    for (int i = 0; i < DRONE_MAX; i++) {
        const DroneRec *d = drone_get(i);
        if (!d) { cb_drone_logged[i] = 0; continue; }
        if (cb_drone_logged[i]) continue;
        cb_drone_logged[i] = 1;
        char alt[16];
        if (d->altGeo > INV_ALT)       snprintf(alt, sizeof(alt), "%.0fm", d->altGeo);
        else if (d->height > INV_ALT)  snprintf(alt, sizeof(alt), "%.0fm", d->height);
        else                           snprintf(alt, sizeof(alt), "alt?");
        char line[96];
        snprintf(line, sizeof(line), "DRONE: %s %s %ddBm\n", d->uasId, alt, d->rssi);
        cb_log_append(line);
    }
}

static void cb_poll_pwnagotchi(void) {
    int count = cb.getPwnagotchiCount();
    while (cb_output_last_pwna < count) {
        CBPwnagotchiInfo pwn;
        if (cb.getPwnagotchi(cb_output_last_pwna, pwn)) {
            char line[80];
            // cb_safe: name/version are JSON lifted out of a received beacon, so they are
            // untrusted. cb_safe has 4 rotating static buffers, so two calls in one
            // expression is safe.
            snprintf(line, sizeof(line), "PWNA: %s v%s (%d pwnd)\n",
                     cb_safe(pwn.name), cb_safe(pwn.version), pwn.pwnd_tot);
            cb_log_append(line);
        }
        cb_output_last_pwna++;
    }
}

static void cb_poll_pinescan(void) {
    int count = cb.getPinescanCount();
    while (cb_output_last_pine < count) {
        CBPinescanInfo pi;
        if (cb.getPinescan(cb_output_last_pine, pi)) {
            pi.essid[sizeof(pi.essid) - 1] = '\0';
            char line[96];
            // cb_safe on essid only: detection_type comes from a fixed literal set in
            // WiFiScan.cpp (ending `detection = "OTHER";`), so it does not need sanitising.
            snprintf(line, sizeof(line), "PINE: %s %s [ch:%d] %ddBm\n",
                     pi.detection_type, cb_safe(pi.essid), pi.channel, pi.rssi);
            cb_log_append(line);
        }
        cb_output_last_pine++;
    }
}

static void cb_poll_multissid(void) {
    int count = cb.getMultiSSIDCount();
    while (cb_output_last_multi < count) {
        CBMultiSSIDInfo mi;
        if (cb.getMultiSSIDResult(cb_output_last_multi, mi)) {
            mi.essid[sizeof(mi.essid) - 1] = '\0';
            char line[80];
            snprintf(line, sizeof(line), "MULTI: %s (%d SSIDs) [ch:%d]\n",
                     cb_safe(mi.essid), mi.ssid_count, mi.channel);   // air-sourced ESSID
            cb_log_append(line);
        }
        cb_output_last_multi++;
    }
}

static void cb_poll_probes(void) {
    int count = cb.getProbeSSIDCount();
    while (cb_output_last_probe < count) {
        CBProbeSSIDInfo pr;
        if (cb.getProbeSSID(cb_output_last_probe, pr)) {
            pr.essid[sizeof(pr.essid) - 1] = '\0';
            char line[64];
            snprintf(line, sizeof(line), "PRB: %s (%d reqs)\n",
                     cb_safe(pr.essid), pr.requests);   // probe-request SSID: arbitrary client text
            cb_log_append(line);
        }
        cb_output_last_probe++;
    }
}

static void cb_poll_deauth_sniff(void) {
    // FB13: this used to compare cb.getDeauthEventCount() against a high-watermark. That
    // count SATURATES at CB_DEAUTH_RING_SIZE (32), so once the ring filled, `32 > 32` was
    // false FOREVER: Analyze > Deauth logged exactly 32 events and then froze for the rest of
    // the session while the sniffer kept recording and the status bar kept a correct, climbing
    // DEA: counter. Internally inconsistent, and it silently lost the tool's actual product.
    // Only a Stop+Start re-armed it (which reset the watermark) -- for another 32.
    //
    // Now driven by the monotonic cb.getDeauthTotal(). The ring still only holds the newest
    // 32, so if MORE than a ringful arrived since the last poll (very possible during a burst
    // at an 800 ms poll interval) the older ones are genuinely gone -- say so instead of
    // pretending, which is also how the old code silently lost events even before the freeze.
    uint32_t total = cb.getDeauthTotal();
    if (total == (uint32_t)cb_output_last_deauth_ring) return;
    if (total < (uint32_t)cb_output_last_deauth_ring) {   // ring was cleared (tool restart)
        cb_output_last_deauth_ring = 0;
    }
    uint32_t arrived = total - (uint32_t)cb_output_last_deauth_ring;
    int held = cb.getDeauthEventCount();                  // newest <= 32 still in the ring
    if (arrived > (uint32_t)held) {
        char drop[48];
        snprintf(drop, sizeof(drop), "... %lu dropped\n", (unsigned long)(arrived - held));
        cb_log_append(drop);
        arrived = (uint32_t)held;
    }
    // getDeauthEvent() indexes oldest-first over what is currently held, so the newest
    // `arrived` entries start at held - arrived.
    for (int i = held - (int)arrived; i < held; i++) {
        CBDeauthEvent ev;
        if (cb.getDeauthEvent(i, ev)) {
            char line[64];
            snprintf(line, sizeof(line), "%ddBm ch%d %s>%s\n",
                     ev.rssi, ev.channel, ev.src, ev.dst);
            cb_log_append(line);
        }
    }
    cb_output_last_deauth_ring = (int)total;
}

static void cb_poll_packet_counters(void) {
    CBPacketCounters pc = cb.getPacketCounters();
    // "Captured" = all frames the raw callback logged (mgmt + data). The type row uses the
    // *Frames family the RAW callback feeds -- NOT numProbe (that's the WIFI_PACKET_MONITOR
    // counter, which is left STALE by a prior Monitor tool and showed e.g. PRB:2 while the
    // real capture had 100 probe requests). Probe = req+resp frames (newly exposed).
    int captured = (int)pc.mgmtFrames + (int)pc.dataFrames;
    if (captured != cb_output_last_pkts) {
        cb_output_last_pkts = captured;
        char line[80];
        snprintf(line, sizeof(line), "Captured: %d\nBCN:%lu PRB:%lu DEA:%lu EAP:%lu\n",
                 captured,
                 (unsigned long)pc.beaconFrames,
                 (unsigned long)(pc.reqFrames + pc.respFrames),
                 (unsigned long)pc.deauthFrames,
                 (unsigned long)pc.eapolFrames);
        cb_log_replace(line);
    }
}

// Monitor > Packet Rate: overall frames/sec (delta over the ~800ms poll interval). The
// WIFI_SCAN_PACKET_RATE mode only counts a *selected* AP's packets (and this tool has no
// selection UI), so the raw counters read 0 -- show the air-wide rate instead.
static void cb_poll_packet_rate(void) {
    CBPacketCounters pc = cb.getPacketCounters();
    int total = pc.numBeacon + pc.numDeauth + pc.numProbe;  // num* = what packetMonitor feeds
    int delta = total - cb_output_last_pkts;
    if (delta < 0 || cb_output_last_pkts == 0) delta = 0;  // ignore the first-tick baseline
    cb_output_last_pkts = total;
    char line[48];
    snprintf(line, sizeof(line), "Rate: ~%d pkts/s\n", (delta * 1000) / 800);
    cb_log_replace(line);
}

// Analyze > SAE Commit: SAE (WPA3) commit frames seen.
static void cb_poll_sae(void) {
    CBPacketCounters pc = cb.getPacketCounters();
    if ((int)pc.saeFrames != cb_output_last_pkts) {
        cb_output_last_pkts = (int)pc.saeFrames;
        char line[48];
        snprintf(line, sizeof(line), "SAE commits: %lu\n", (unsigned long)pc.saeFrames);
        cb_log_replace(line);
    }
}

#ifdef CLIPBOY_RES34RCH  // ACTIVE RESEARCH output counters (Res34rch-Boy only)
static void cb_poll_deauth_counter(void) {
    int sent = cb.getPacketsSent();
    if (sent != cb_output_last_pkts) {
        cb_output_last_pkts = sent;
        char line[48];
        snprintf(line, sizeof(line), "Sent: %d deauth frames\n", sent);
        cb_log_replace(line);
    }
}
#endif // CLIPBOY_RES34RCH

static void cb_poll_bt_frames(void) {
    int frames = cb.getBTFrames();
    if (frames != cb_output_last_pkts) {
        cb_output_last_pkts = frames;
        char line[48];
        snprintf(line, sizeof(line), "BLE packets: %d", frames);
        cb_log_replace(line);
    }
}

// Monitor > Signal (RSSI): the SELECTED AP's live RSSI (SIG_STREN updates ap.rssi; the
// analyzer fields are never written in this mode). Prompts if no AP is selected.
static void cb_poll_signal(void) {
    int n = cb.getAPCount();
    for (int i = 0; i < n; i++) {
        if (!cb.isAPSelected(i)) continue;
        CBAccessPointInfo ap;
        if (cb.getAP(i, ap)) {
            ap.essid[sizeof(ap.essid) - 1] = '\0';
            char line[80];
            snprintf(line, sizeof(line), "Signal: %d dBm  Ch %d\n%s\n",
                     ap.rssi, ap.channel, ap.essid[0] ? cb_safe(ap.essid) : "(hidden)");
            cb_log_replace(line);
            return;
        }
    }
    cb_log_replace("Scan, then tap Select above\nto pick an AP to monitor.\n");
}

// Monitor > Channel Stats: per-channel frame counts (1-14), non-zero only.
static void cb_poll_channel_activity(void) {
    CBChannelActivity ca = cb.getChannelActivity();
    int total = 0;
    for (int i = 0; i < 14; i++) total += ca.counts[i];
    if (total != cb_output_last_pkts) {
        cb_output_last_pkts = total;
        char line[128];
        int n = snprintf(line, sizeof(line), "Channel activity:\n");
        for (int c = 0; c < 14 && n < (int)sizeof(line) - 12; c++)
            if (ca.counts[c])
                n += snprintf(line + n, sizeof(line) - n, "Ch%d:%u  ", c + 1, ca.counts[c]);
        snprintf(line + n, sizeof(line) - n, "\n");
        cb_log_replace(line);
    }
}

// Monitor > MAC Tracker: top talkers by frame count. getMACTrackerTop10 scans the WHOLE
// tracked-MAC table (can be large), so run it every ~3rd poll tick (~2.4s) -- doing it
// every 800ms on the UI thread made the whole UI sluggish.
static void cb_poll_mac_tracker(void) {
    static uint8_t mt_skip = 0;
    if (mt_skip++ % 5 != 0) return;   // ~4s: the whole-table top-10 sort on the UI thread is heavy
    MacEntry top[10];
    uint8_t k = cb.getMACTrackerTop10(top, MacSortMode::MOST_FRAMES);
    char buf[320];
    // No "Top talkers:" header -- the tool title already says MAC Tracker, and dropping it
    // frees a line so all 10 fit without scrolling.
    int p = 0;
    buf[0] = '\0';
    for (int i = 0; i < k && p < (int)sizeof(buf) - 32; i++)
        p += snprintf(buf + p, sizeof(buf) - p,
                      "%02X:%02X:%02X:%02X:%02X:%02X %u\n",
                      top[i].mac[0], top[i].mac[1], top[i].mac[2],
                      top[i].mac[3], top[i].mac[4], top[i].mac[5], top[i].frame_count);
    if (k == 0) snprintf(buf + p, sizeof(buf) - p, "(listening...)\n");
    cb_log_pin_top = true;   // the flush pins this to the top so the top talkers stay visible
    cb_log_replace(buf);
}

// Analyze > Espressif: filter the scanned AP list to Espressif (ESP32/ESP8266) OUIs.
static const uint8_t kEspressifOUIs[][3] = {
    {0x24,0x0A,0xC4},{0x24,0x6F,0x28},{0x30,0xAE,0xA4},{0x3C,0x71,0xBF},{0x54,0x5A,0xA6},
    {0x7C,0x9E,0xBD},{0x84,0x0D,0x8E},{0x84,0xCC,0xA8},{0x8C,0xAA,0xB5},{0x90,0x38,0x0C},
    {0x98,0xF4,0xAB},{0xA0,0x20,0xA6},{0xA4,0xCF,0x12},{0xAC,0x0B,0xFB},{0xB4,0xE6,0x2D},
    {0xBC,0xDD,0xC2},{0xC4,0x4F,0x33},{0xCC,0x50,0xE3},{0xD8,0xA0,0x1D},{0xDC,0x4F,0x22},
    {0xE8,0xDB,0x84},{0xEC,0xFA,0xBC},{0xF4,0xCF,0xA2},
};
static bool is_espressif_oui(const uint8_t *b) {
    for (unsigned i = 0; i < sizeof(kEspressifOUIs) / sizeof(kEspressifOUIs[0]); i++)
        if (b[0] == kEspressifOUIs[i][0] && b[1] == kEspressifOUIs[i][1] && b[2] == kEspressifOUIs[i][2])
            return true;
    return false;
}
static void cb_poll_espressif(void) {
    int n = cb.getAPCount();
    char buf[256];
    int p = snprintf(buf, sizeof(buf), "ESP devices:\n");
    int found = 0;
    for (int i = 0; i < n && p < (int)sizeof(buf) - 48; i++) {
        CBAccessPointInfo ap;
        if (!cb.getAP(i, ap)) continue;
        ap.essid[sizeof(ap.essid) - 1] = '\0';
        if (!is_espressif_oui(ap.bssid)) continue;
        found++;
        p += snprintf(buf + p, sizeof(buf) - p, "%s [%02X:%02X:%02X] ch%d\n",
                      ap.essid[0] ? cb_safe(ap.essid) : "(hidden)",
                      ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.channel);
    }
    if (!found) snprintf(buf + p, sizeof(buf) - p, "(none yet)\n");
    cb_log_replace(buf);
}

#ifdef CLIPBOY_RES34RCH  // ACTIVE RESEARCH output counters (Res34rch-Boy only)
static void cb_poll_active_counter(void) {
    int sent = cb.getPacketsSent();
    if (sent != cb_output_last_pkts) {
        cb_output_last_pkts = sent;
        char line[48];
        snprintf(line, sizeof(line), "Sent: %d packets\n", sent);
        cb_log_replace(line);
    }
}
#endif // CLIPBOY_RES34RCH

#ifdef CLIPBOY_RES34RCH  // ACTIVE RESEARCH output (Res34rch-Boy only)
static void cb_poll_evil_portal(void) {
    CBEvilPortalStatus ep = cb.getEvilPortalStatus();
    if (ep.hasCredentials && !cb_ep_last_has_creds) {
        cb_ep_last_has_creds = true;
        char line[256];
        // cb_safe: these are POST-body fields from the captive portal, i.e. entirely
        // untrusted text rendered straight to the operator's screen.
        snprintf(line, sizeof(line), "CRED: %s / %s\n",
                 cb_safe(ep.userName), cb_safe(ep.password));
        cb_log_append(line);
    } else if (!ep.hasCredentials) {
        cb_ep_last_has_creds = false;  // reset for next credential
    }
}
#endif // CLIPBOY_RES34RCH

// ── Main output poll dispatcher ─────────────────────────────────────────

static void cb_output_poll_cb(lv_timer_t *timer) {
    (void)timer;

    if (!cb_op_running) {
        cb_log_append("--- Complete ---\n");
        cb_log_flush_to_ui();
        if (cb_output_timer) { lv_timer_delete(cb_output_timer); cb_output_timer = NULL; }
        return;
    }

    if (cb_op_encoded < 0) return;
    uint8_t ci = tool_cat(cb_op_encoded);
    uint8_t ii = tool_item(cb_op_encoded);
    uint8_t sc = ci < NUM_TOOL_CATS ? tool_categories[ci].id : 255;

    // Monitor Packets/Rate/RSSI/Channel drive an lv_chart, not the text pollers.
    if (cb_tool_is_chart(sc, ii)) { cb_chart_update(ii); return; }

    switch (sc) {
        case 0:  // Detect: 0=AirTag,1=Skimmer,2=Flipper,3=Flock,4=Rogue AP,5=Evil Twin
            switch (ii) {
                case 0: cb_poll_airtags(); break;
                case 1: cb_poll_bt_devices(); break;        // Skimmer Check
                case 2: cb_poll_flippers(); break;
                case 3: cb_poll_flock(); break;
                case 4: cb_poll_pinescan(); break;          // Rogue AP (Pineapple)
                case 5: cb_poll_multissid(); break;         // Evil Twin (Multi-SSID)
                case 6: cb_poll_drone(); break;             // Remote ID (drone)
                default: cb_poll_aps(); break;
            }
            break;
        case 1:  // Scan: 0=APs,1=APs+STA,2=STA,3=BT Devices,4=BLE Adverts
            switch (ii) {
                case 3: cb_poll_bt_devices(); break;        // BT Devices - full list
                case 4: cb_poll_bt_frames(); break;         // BLE Adverts - packet counter
                default:                                    // WiFi AP/STA scans
                    cb_poll_aps();
                    cb_poll_stations();
                    break;
            }
            break;
        case 2:  // Monitor: 0=Packets,1=Rate,2=Signal,3=Channel Stats,4=MAC Tracker
            switch (ii) {
                case 1: cb_poll_packet_rate();      break;  // Packet Rate
                case 2: cb_poll_signal();           break;  // Signal/RSSI
                case 3: cb_poll_channel_activity(); break;  // Channel Stats
                case 4: cb_poll_mac_tracker();      break;  // MAC Tracker
                default: cb_poll_packet_counters(); break;  // Packets
            }
            break;
        case 3:  // Analyze (passive capture)
            switch (ii) {
                case 0: cb_poll_aps(); break;               // Beacons
                case 1: cb_poll_probes(); break;            // Probes
                case 2: cb_poll_deauth_sniff(); break;      // Deauth - per-event BSSID log
                case 3: cb_poll_packet_counters(); break;   // Raw/PCAP
                case 4: cb_poll_pwnagotchi(); break;        // Pwnagotchi
                case 5: cb_poll_espressif(); break;         // Espressif (OUI-filtered)
                case 6: cb_poll_sae(); break;               // SAE Commit
                case 7:                                     // EAPOL/PMKID
                    cb_poll_aps();  // for handshake status
                    cb_poll_packet_counters();
                    break;
                default: cb_poll_packet_counters(); break;
            }
            break;
#ifdef CLIPBOY_RES34RCH  // ─── ACTIVE RESEARCH output polling tail (Res34rch-Boy only), ids 6-11 ───
        case 6:  // Deauth
            cb_poll_deauth_counter();
            break;
        case 7:  // Flood
            cb_poll_active_counter();
            break;
        case 8:  // Beacon Spam
            cb_poll_active_counter();
            break;
        case 9:  // BLE Spam
            cb_poll_active_counter();
            break;
        case 10: // SAE
            cb_poll_active_counter();
            break;
        case 11: // Evil Portal
            cb_poll_evil_portal();
            break;
#endif // CLIPBOY_RES34RCH
        default:
            break;
    }
    // Push all accumulated buffer changes to the label in one go. Keeps
    // full-buffer relayouts out of the hot per-line path.
    cb_log_flush_to_ui();
}

// START/STOP action button callback
static void cb_tool_start_stop_cb(lv_event_t *e) {
    int32_t encoded = (int32_t)(intptr_t)lv_event_get_user_data(e);
    uint8_t ci = tool_cat(encoded);
    uint8_t ii = tool_item(encoded);

    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);

    if (cb_op_running && cb_op_encoded == encoded) {
        // STOP - this tool is running, stop it
        cb_stop_operation();
        cb_log_append("--- Stopped ---\n");
        cb_log_flush_to_ui();
        if (cb_output_timer) { lv_timer_delete(cb_output_timer); cb_output_timer = NULL; }
        if (lbl) lv_label_set_text(lbl, "> START <");
        // TAT_AP tools share the Scan/Stop toggle with cb_op_running; stopping via the
        // main button must also revert the Scan label (else it stays stuck on "Stop").
        if (wifi_scan_btn_label) lv_label_set_text(wifi_scan_btn_label, "Scan");
        CB_LOGF("[CB] Stopped: %s > %s\n",
                      tool_categories[ci].name,
                      tool_categories[ci].items[ii].name);
        return;  // STOP only - don't fall through to START
    }
    if (cb_op_running) {
        // A different tool is running - stop it first, then start this one
        cb_stop_operation();
        if (cb_output_timer) { lv_timer_delete(cb_output_timer); cb_output_timer = NULL; }
    }
    if (!cb_op_running) {
        // Stop Geiger if active - kill its sniffer + timer. This was the one site that DID
        // call cb.stopScan(); it still missed the tick-audio teardown, so the unlock tone
        // stayed dead. rad_geiger_force_stop() does both (FB11/FB12).
        rad_geiger_force_stop();
        // Release the LiDAR (HR scan + theremin) before claiming the radio.
        stop_lidar_activities();
        if (ci < NUM_TOOL_CATS && ii < tool_categories[ci].count) {
            const char *name = tool_categories[ci].items[ii].name;
            // File-only capture with PCAP Saving off: don't start -- tell the user instead
            // of running a capture that records nothing.
            if (pcap_file_tool_gated(tool_categories[ci].id, ii)) {
                if (right_pane) {
                    cb_create_output_area(right_pane);
                    cb_log_append("Can't capture: PCAP Saving is\n");
                    cb_log_append("off. Enable 'Allow PCAP Saving'\n");
                    cb_log_append("in DATA > Settings, then retry.\n");
                    cb_log_flush_to_ui();
                }
                return;
            }
            cb_ensure_wifi();  // disconnects any active WiFi connection
            cb_pcap_write_blocked = false;   // openFile (during dispatch) sets it if a pcap
            cb_pcap_write_failed  = false;   // ...or if pcap file creation failed (SD/dir full)
            cb_pcap_seq           = 0;        // createFile (during dispatch) sets it for a pcap
                                              // tool; stays 0 for non-pcap tools so the O(1)
                                              // pile-up warning below never fires on them
            dispatch_clipboy_action(tool_categories[ci].id, ii);  // write was skipped (saving off)
            cb_op_running = true;
            cb_op_name = name;
            cb_op_encoded = encoded;
            // Starting a tool stops any AP scan -> the Scan/Stop toggle should read "Scan".
            if (wifi_scan_btn_label) lv_label_set_text(wifi_scan_btn_label, "Scan");
            if (lbl_stask) lv_label_set_text(lbl_stask, name);
            if (lbl) lv_label_set_text(lbl, "> STOP <");
            CB_LOGF("[CB] Started: %s > %s\n",
                          tool_categories[ci].name, name);

            // Create live output area in right pane
            if (right_pane) {
                cb_create_output_area(right_pane);
                char start_msg[64];
                snprintf(start_msg, sizeof(start_msg), "Starting: %s...\n", name);
                cb_log_append(start_msg);
                // This tool tried to write a pcap but 'Allow PCAP Saving' is off (the
                // openFile call during dispatch set the flag). It still runs live -- one
                // compact line says so + points at More Info. (Raw/PCAP + EAPOL are
                // hard-gated and never reach here.)
                if (cb_pcap_write_failed) {
                    // Distinct from saving-off: the file could not be created (SD full,
                    // /pcaps full, or too many files). Tell the user what + how to fix.
                    cb_log_append("PCAP NOT SAVED: SD full or too many files.\n"
                                  "Free space or clear /pcaps, then retry.\n");
                } else if (cb_pcap_write_blocked) {
                    cb_log_append("Not Saving / See Help\n");
                } else if (cb_pcap_seq >= 5000) {
                    // O(1) pile-up warning (proxy = last claimed name-index; NOT a dir
                    // enumeration). Fires once per tool session, only when the capture is
                    // actually saving. Uses coarse tiers, not the raw seq (over-counts
                    // after external pruning). Heavy tier: captures start to lag.
                    cb_log_append("Whoa: 5000+ pcaps on this card.\n"
                                  "Captures will lag. Move them to a\n"
                                  "PC and clear /pcaps. See Help.\n");
                } else if (cb_pcap_seq >= 500) {
                    cb_log_append("Heads up: 500+ pcaps on this card.\n"
                                  "Saves get slower as they pile up.\n"
                                  "Clean out /pcaps on a PC. See Help.\n");
                }
                cb_log_flush_to_ui();
            }
            // Reset all incremental tracking counters.
            // M1: AP and STA are seeded to ZERO, not to the current count, unlike every other
            // store below. The rest are emptied at tool start by resetDisplayAccumulators(),
            // so their counts ARE zero here and seeding from them is the same thing. But the
            // AP/STA/SSID lists deliberately PERSIST across tool switches (they are
            // select-then-run inputs), so seeding from their count told cb_poll_aps() to skip
            // every AP already in the store -- and cb_output_cleanup() had just wiped the log
            // buffer. Net effect on the SECOND run of any AP-listing tool: an empty log while
            // the status bar cheerfully reported "12 APs found". For Scan > APs (full) the log
            // is the tool's only output, so the pane read as "found nothing".
            // Seeding at 0 re-lists what is actually in the store, which is what the user is
            // looking at the log to learn.
            cb_output_last_ap      = 0;
            cb_output_last_sta     = 0;
            // ...but SAY SO. Seeding at 0 means the first poll re-emits whatever is already in
            // the persisting AP/STA store, and those entries were found by an EARLIER tool --
            // e.g. Scan > APs, stop, then start Scan > Stations, and the station log would open
            // with N "AP:" lines a station scan never produced. Presenting a previous tool's
            // results as this tool's findings is the stale-result-store class that
            // tool_result_kind()/cb_active_result_kind() exist to prevent, so label the batch
            // instead of silently attributing it. (Empty-log was the other wrong answer.)
            {
                int _pre_ap  = cb.getAPCount();
                int _pre_sta = cb.getStationCount();
                if (_pre_ap > 0 || _pre_sta > 0) {
                    char _pb[72];
                    snprintf(_pb, sizeof(_pb),
                             "-- %d AP / %d STA already in list --\n", _pre_ap, _pre_sta);
                    cb_log_append(_pb);
                }
            }
            // ⚠ bt / flock / pwnagotchi are seeded at ZERO, not at their count, because
            // resetDisplayAccumulators() (called from dispatch_clipboy_action a few lines above)
            // has just EMPTIED exactly those three lists -- clearBTDevices / clearFlockDevices /
            // clearPwnagotchis. So the count here is 0 in the normal case, and a NON-zero value
            // can only mean a device arrived from the BLE callback in the window between that
            // clear and this line. Seeding at the count then sets the high-watermark past it and
            // cb_poll_* NEVER EMITS THAT DEVICE: the status bar reads "1 Flock" while the log
            // pane shows nothing but "Starting: ...", i.e. the tool says it found a camera and
            // never says which.
            // REPRODUCED 2026-07-27 with a fast advertiser (kalipi2b at sub-second intervals,
            // flock=1 / flock_age~1s with an empty log, twice); the original ~0.29/s emitter
            // almost never landed inside the window, which is why this survived until the
            // emulation got faster. Seeding at 0 is correct for a list that was just cleared and
            // is race-free by construction.
            // ⚠ NOT changed for airtag/flipper/pine/multi/probe: those are NOT cleared by
            // resetDisplayAccumulators, so they persist, and seeding them at 0 would re-emit a
            // previous tool's finds as this tool's -- the stale-result-store class. (They do
            // carry the empty-log-on-second-run problem the AP/STA comment above describes;
            // that is pre-existing and needs the same batch-labelling treatment. Logged.)
            cb_output_last_bt      = 0;
            cb_output_last_airtag  = cb.getAirTagCount();
            cb_output_last_flipper = cb.getFlipperCount();
            cb_output_last_flock   = 0;
            drone_clear();
            for (int _di = 0; _di < DRONE_MAX; _di++) cb_drone_logged[_di] = 0;
            cb_output_last_pwna    = 0;
            cb_output_last_pine    = cb.getPinescanCount();
            cb_output_last_multi   = cb.getMultiSSIDCount();
            cb_output_last_probe   = cb.getProbeSSIDCount();
            cb_output_last_deauth  = 0;
            cb_output_last_deauth_ring = 0;
            cb_output_last_pkts    = 0;
            cb.clearDeauthEvents();  // clear ring buffer for fresh start
            if (cb_output_timer) lv_timer_delete(cb_output_timer);
            cb_output_timer = lv_timer_create(cb_output_poll_cb, 800, NULL);

            // Start scan polling for scan-type operations. Under the DETECT-LED
            // taxonomy the passive scan/sniff/detect categories are Detect(0),
            // Scan(1) and Analyze(3) (Monitor(2) uses the output poller only).
            {
                uint8_t _cid = tool_categories[ci].id;
                if (_cid == 0 || _cid == 1 || _cid == 3)
                    cb_start_scan_polling();
            }
        }
    }
    // F1: this tool now owns the radio (or just released it) -- re-dim the inline Scan to
    // match. Covers BOTH branches of this callback, which is why it sits at the very end.
    cb_inline_scan_refresh();
}

// EXECUTE (immediate) callback
static void cb_tool_execute_cb(lv_event_t *e) {
    int32_t encoded = (int32_t)(intptr_t)lv_event_get_user_data(e);
    uint8_t ci = tool_cat(encoded);
    uint8_t ii = tool_item(encoded);
    if (ci < NUM_TOOL_CATS && ii < tool_categories[ci].count) {
        // R1. `dispatch_clipboy_action()` clears cb_manual_scan_kind unconditionally, because for
        // a real START that IS correct -- the new tool takes over the status readout. But an
        // EXECUTE is a one-shot: this handler never sets cb_op_running or cb_op_encoded, so it
        // never becomes the readout's owner. Clearing the kind here therefore ORPHANS a live
        // manual station scan: cb_active_result_kind() falls back to MON_AP and the status bar
        // reverts to a stale AP count while stations are still being enumerated. Measured on
        // hardware: '0 STAs found' -> '5 APs found', manual_kind 1 -> -1, with the station scan
        // still running.
        // Save and restore around the dispatch, and only restore if a scan is still live -- if
        // the one-shot did end the scan, the readout legitimately belongs to nobody.
        // ⚠ LOAD-BEARING PREMISE: no EXECUTE/ENTER tool STARTS a scan of its own. The full set
        // is Gen Rnd SSIDs, Clear All, Rnd AP MAC, Rnd STA MAC and Evil Portal > Stop -- all
        // one-shots that leave the radio's scan state alone. That is why `cb.isScanning()` being
        // true here means the PRE-EXISTING manual scan is still the one running, so restoring its
        // kind is right. If you ever add a TAT_IMMEDIATE/TAT_ENTER tool that starts a scan, this
        // guard would resurrect the OLD kind over the new owner's -- bind the kind in that tool
        // instead of extending this.
        // ⚠ WHICH HALF THIS FIXES: only the EXECUTE path. The root cause is that
        // dispatch_clipboy_action() conflates "start a tool that owns the readout" with "fire a
        // one-shot", and the clean fix is to make ownership an explicit parameter and clear the
        // kind at each producer that sets cb_op_running. That means enumerating every producer
        // (3 live sites plus the harness) and is a wider change than is justified this close to
        // DEF CON. This guard is narrow, local, and cannot affect any other caller of dispatch.
        int saved_kind = cb_manual_scan_kind;
        dispatch_clipboy_action(tool_categories[ci].id, ii);
        if (saved_kind >= 0 && cb_manual_scan_kind < 0 && cb.isScanning())
            cb_manual_scan_kind = saved_kind;
        CB_LOGF("[CB] Executed: %s > %s\n",
                      tool_categories[ci].name,
                      tool_categories[ci].items[ii].name);
    }
}

// ─────────────────────── TOOLS: RIGHT PANE BUILDERS ─────────────────────

// Build right pane for TAT_SIMPLE: Description + [> START <]
static void show_tool_simple(const ToolItem *wi, int32_t encoded) {
    lv_obj_t *desc = make_label(right_pane, wi->desc,
                                &ui_font_pipboy_16, pip_primary());
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(desc, lv_pct(100));
    cb_tool_desc = desc;   // cb_create_output_area hides this while running to free output space
    lv_obj_add_event_cb(desc, cb_selfnull_on_delete, LV_EVENT_DELETE, &cb_tool_desc);

    // Analyze > Raw/PCAP: channel selector. "Hop (all)" (default) sweeps the band so a general
    // capture sees every channel's beacons; locking a channel targets one network without hopping.
    {
        uint8_t _stp  = tool_cat(encoded);
        uint8_t _stid = (_stp < NUM_TOOL_CATS) ? tool_categories[_stp].id : 255;
        if (_stid == 3 && tool_item(encoded) == 3) {
            lv_obj_t *crow = lv_obj_create(right_pane);
            lv_obj_remove_style_all(crow);
            lv_obj_set_size(crow, lv_pct(100), LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(crow, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(crow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_gap(crow, 6, 0);
            lv_obj_remove_flag(crow, LV_OBJ_FLAG_SCROLLABLE);
            make_label(crow, "Channel", &ui_font_pipboy_16, pip_primary());
            lv_obj_t *dd = make_dropdown(crow, "Hop (all)\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14");
            lv_dropdown_set_selected(dd, raw_capture_channel);
            lv_obj_set_flex_grow(dd, 1);
            lv_obj_add_event_cb(dd, [](lv_event_t *e){
                lv_obj_t *d = (lv_obj_t *)lv_event_get_target(e);
                raw_capture_channel = (uint8_t)lv_dropdown_get_selected(d);   // 0 = hop, 1-14 = lock
                if (cb_op_running) cb.setRawCaptureChannel(raw_capture_channel);  // apply live if capturing
            }, LV_EVENT_VALUE_CHANGED, NULL);
        }
        // Analyze > Deauth: the SAME channel policy the Radiation gauge uses -- one setting,
        // because both pages drive the identical WIFI_SCAN_DEAUTH scan and only one can run at
        // a time. Two settings would let the UI disagree with the radio.
        // This page carries the LONG-FORM mode text ("sampling ch 1, 6, 11"). The gauge page
        // has no room for it (it collided with the 0/100 ticks) and shows the terse token in
        // the status bar instead -- so without this caption the word "sampling" appears
        // NOWHERE on the device, and the plain-language mitigation the channel lock was
        // approved on would exist only in the plan.
        if (_stid == 3 && tool_item(encoded) == 2) {
            lv_obj_t *crow = lv_obj_create(right_pane);
            lv_obj_remove_style_all(crow);
            lv_obj_set_size(crow, lv_pct(100), LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(crow, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(crow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_gap(crow, 6, 0);
            lv_obj_remove_flag(crow, LV_OBJ_FLAG_SCROLLABLE);
            make_label(crow, "Channel", &ui_font_pipboy_16, pip_primary());
            lv_obj_t *dd = make_dropdown(crow, CB_DEAUTH_DD_OPTS);
            lv_dropdown_set_selected(dd, cb_deauth_mode_to_idx(cfg.deauth_chan));
            lv_obj_set_flex_grow(dd, 1);
            // Plain-language caption, created BEFORE the handler is attached so it can be
            // passed as user_data. Built-once-from-cfg was not enough: changing the dropdown
            // on this page left the caption reading the OLD mode until the page was rebuilt,
            // which is the "UI disagrees with the setting" failure this feature exists to stop.
            lv_obj_t *cap = make_label(right_pane, cb_deauth_mode_text(cfg.deauth_chan),
                                       &ui_font_pipboy_14, pip_dim());
            lv_label_set_long_mode(cap, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(cap, lv_pct(100));
            lv_obj_add_event_cb(dd, cb_deauth_dd_cb, LV_EVENT_VALUE_CHANGED, cap);
        }
    }

    lv_obj_t *sp = lv_obj_create(right_pane);
    lv_obj_remove_style_all(sp);
    lv_obj_set_size(sp, 1, 4);

    make_action_btn(right_pane,
                    (cb_op_running && cb_op_encoded == encoded) ? "> STOP <" : "> START <",
                    cb_tool_start_stop_cb, (void *)(intptr_t)encoded);
}

// Build right pane for TAT_AP: Description + [Scan/Stop] [Select] [Deselect] + list + [> START <]
// AP-scan tools that target exactly ONE AP get the single-select highlight list + a START
// disabled until an AP is picked; the rest keep multi-select (Select AP utility, Deauth-all,
// Manual deauth). Keyed by stable category id + item; each is "selected AP" (singular) in copy.
static bool tool_wants_single_ap(uint8_t cat_id, uint8_t item) {
    return (cat_id == 2  && item == 2)   // Monitor     > RSSI
        || (cat_id == 3  && item == 7)   // Analyze     > EAPOL/PMKID
        || (cat_id == 7  && item == 0)   // Flood       > ! Auth
        || (cat_id == 8  && item == 2)   // Beacon Spam > AP Clone
        || (cat_id == 10 && item == 0);  // SAE         > ! Commit Flood
}

static void show_tool_ap(const ToolItem *wi, int32_t encoded) {
    uint8_t _atp  = tool_cat(encoded);
    uint8_t _atid = (_atp < NUM_TOOL_CATS) ? tool_categories[_atp].id : 255;
    bool single   = tool_wants_single_ap(_atid, tool_item(encoded));   // RSSI/EAPOL/Auth/Clone/CommitFlood
    cb_ap_single_select = single;

    lv_obj_t *desc = make_label(right_pane, wi->desc,
                                &ui_font_pipboy_16, pip_primary());
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(desc, lv_pct(100));

    // Button row: Scan/Stop + Select + Deselect
    lv_obj_t *brow = lv_obj_create(right_pane);
    lv_obj_remove_style_all(brow);
    lv_obj_add_style(brow, &style_container, 0);
    lv_obj_set_size(brow, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(brow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(brow, 4, 0);

    // Scan/Stop toggle
    {
        lv_obj_t *scan_btn = make_small_btn(brow,
            (cb_op_running && cb_op_name && strstr(cb_op_name, "Scan")) ? "Stop" : "Scan",
            [](lv_event_t *e) {
                (void)e;
                // F1 hard refusal. The visual disable above can go stale if a tool starts
                // while this pane is already built, and a tap here would then silently stop
                // that tool -- leaving its "> STOP <" button and lv_chart asserting it still
                // runs. Refuse at the point of action, where the state is current.
                if (cb_inline_scan_blocked_for(cb_inline_scan_page_enc)) return;
                // Only treat a tap as "stop scan" if an AP SCAN is actually running. If a
                // DIFFERENT tool was left running (cb_op_running true, but not a scan), the
                // first tap used to hit this stop-path and just showed "0 APs" -- so it took
                // two taps to start scanning. Now one tap scans.
                bool ap_scanning = cb_op_running && cb_op_name && strstr(cb_op_name, "Scan");
                if (ap_scanning) {
                    cb_stop_operation();
                    int cnt = cb.getAPCount();
                    char dbuf[32];
                    snprintf(dbuf, sizeof(dbuf), "%d APs", cnt);
                    if (lbl_stask) lv_label_set_text(lbl_stask, dbuf);
                    if (wifi_scan_btn_label) lv_label_set_text(wifi_scan_btn_label, "Scan");
                } else {
                    if (cb_op_running) cb_stop_operation();  // stop whatever else was running first
                    rad_geiger_force_stop();   // FB11: bare flag clear latched the tick audio on
                    cb_ensure_wifi();
                    cb_clear_ap_results();   // store AND view, synchronously
                    cb.scanAPs();
                    cb_op_running = true;
                    cb_op_name = "Scanning APs";
                    cb_op_encoded = -1;
                    cb_manual_scan_kind = MON_AP;    // no tool behind this scan; say so
                    if (lbl_stask) lv_label_set_text(lbl_stask, "0 APs found");
                    if (wifi_scan_btn_label) lv_label_set_text(wifi_scan_btn_label, "Stop");
                    cb_start_scan_polling();
                }
            }, NULL);
        wifi_scan_btn_label = lv_obj_get_child(scan_btn, 0);
        lv_obj_add_event_cb(wifi_scan_btn_label, cb_selfnull_on_delete, LV_EVENT_DELETE, &wifi_scan_btn_label);
        // F1: dim + un-clickable while another operation owns the radio. The handler also
        // hard-refuses (below), so a stale-enabled button still cannot steal a running tool.
        cb_inline_scan_btn = scan_btn;
        lv_obj_add_event_cb(scan_btn, cb_selfnull_on_delete, LV_EVENT_DELETE, &cb_inline_scan_btn);
        cb_inline_scan_page_enc = encoded;
        cb_btn_set_enabled(scan_btn, !cb_inline_scan_blocked_for(encoded));
    }

    // Single-AP-target tools (RSSI/EAPOL/Auth/AP-Clone/Commit-Flood) pick ONE AP via the inline
    // list below, so the Select modal + Deselect button are redundant there. Multi-select tools keep them.
    if (!single) {
    // Select from scanned APs (checkbox modal)
    make_small_btn(brow, "Select", [](lv_event_t *e) {
        (void)e;
        int count = cb.getAPCount();
        if (count == 0) return;

        lv_obj_t *modal = lv_obj_create(lv_screen_active());
        lv_obj_remove_style_all(modal);
        lv_obj_set_size(modal, SCREEN_W, SCREEN_H);
        lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(modal, LV_OPA_80, 0);
        lv_obj_remove_flag(modal, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *box = lv_obj_create(modal);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, 280, 200);
        lv_obj_center(box);
        lv_obj_set_style_bg_color(box, pip_bg(), 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(box, pip_highlight(), 0);
        lv_obj_set_style_border_width(box, 2, 0);
        lv_obj_set_style_pad_all(box, 6, 0);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_gap(box, 3, 0);
        lv_obj_add_style(box, &style_scrollbar, LV_PART_SCROLLBAR);

        make_label(box, "Select APs", &ui_font_pipboy_16, pip_highlight());

        for (int i = 0; i < count && i < 20; i++) {
            CBAccessPointInfo ap;
            if (!cb.getAP(i, ap)) continue;
            ap.essid[sizeof(ap.essid) - 1] = '\0';
            lv_obj_t *cb_row = lv_obj_create(box);
            lv_obj_remove_style_all(cb_row);
            lv_obj_set_size(cb_row, lv_pct(100), LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(cb_row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(cb_row, LV_FLEX_ALIGN_START,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_gap(cb_row, 4, 0);

            lv_obj_t *chk = lv_checkbox_create(cb_row);
            lv_checkbox_set_text(chk, cb_safe(ap.essid));
            lv_obj_set_style_text_font(chk, &ui_font_pipboy_14, 0);
            lv_obj_set_style_text_color(chk, pip_primary(), 0);
            if (ap.selected) lv_obj_add_state(chk, LV_STATE_CHECKED);
            // Carry the REAL AP index on the widget. The Done handler used to recover it by
            // counting child positions, but this loop `continue`s past a failed getAP() WITHOUT
            // creating a row -- so one skip shifted every later mapping by one and you selected
            // a DIFFERENT AP than you ticked. On Res34rch that means deauthing the wrong BSSID,
            // which is an authorized-target problem, not just a UI glitch. Same pattern already
            // used by the single-select row handler.
            lv_obj_set_user_data(chk, (void *)(intptr_t)i);
        }

        // Done button - batch-apply checkbox selections
        lv_obj_t *btn_done = lv_button_create(box);
        lv_obj_set_size(btn_done, lv_pct(100), 26);
        lv_obj_set_style_bg_color(btn_done, pip_highlight(), 0);
        lv_obj_t *ld = lv_label_create(btn_done);
        lv_label_set_text(ld, "Done");
        lv_obj_set_style_text_font(ld, &ui_font_pipboy_14, 0);
        lv_obj_set_style_text_color(ld, pip_bg(), 0);
        lv_obj_center(ld);
        lv_obj_add_event_cb(btn_done, [](lv_event_t *e2) {
            lv_obj_t *b = (lv_obj_t *)lv_event_get_target(e2);
            lv_obj_t *bx = lv_obj_get_parent(b);
            // Walk checkbox rows (skip title label at child 0, button at end)
            cb.deselectAPs();
            uint32_t child_cnt = lv_obj_get_child_count(bx);
            for (uint32_t ci = 1; ci < child_cnt - 1; ci++) {
                lv_obj_t *row = lv_obj_get_child(bx, ci);
                lv_obj_t *chk = lv_obj_get_child(row, 0);
                if (chk && lv_obj_has_state(chk, LV_STATE_CHECKED)) {
                    // Read the AP index off the widget instead of counting positions. The old
                    // running `ap_idx++` assumed row N == AP N, which the builder's
                    // `if (!cb.getAP(i, ap)) continue;` breaks: a single skipped AP shifted every
                    // subsequent selection onto its neighbour. Ticking one SSID could select --
                    // and on Res34rch, deauth -- a different one.
                    cb.selectAP((int)(intptr_t)lv_obj_get_user_data(chk));
                }
            }
            cb_last_ap_count = 0;
            cb_populate_ap_list();
            lv_obj_delete(lv_obj_get_parent(bx));
            crt_scanlines_raise();
        }, LV_EVENT_CLICKED, NULL);

        crt_scanlines_raise();
    }, NULL);

    make_small_btn(brow, "Deselect", cb_deselect_aps_cb, NULL);
    }  // end if(!single): RSSI hides Select/Deselect

    // Live AP list. RSSI has no Select/Deselect buttons, so give its single-select list the
    // reclaimed height.
    lv_obj_t *list_area = lv_obj_create(right_pane);
    lv_obj_remove_style_all(list_area);
    lv_obj_set_size(list_area, lv_pct(100), single ? 104 : 60);
    lv_obj_set_style_border_color(list_area, pip_border(), 0);
    lv_obj_set_style_border_width(list_area, 1, 0);
    lv_obj_set_style_border_side(list_area, LV_BORDER_SIDE_FULL, 0);
    lv_obj_set_style_pad_all(list_area, 4, 0);
    lv_obj_set_flex_flow(list_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_style(list_area, &style_scrollbar, LV_PART_SCROLLBAR);
    cb_ap_list_area = list_area;
    lv_obj_add_event_cb(cb_ap_list_area, cb_selfnull_on_delete, LV_EVENT_DELETE, &cb_ap_list_area);

    if (cb.getAPCount() > 0) {
        cb_last_ap_count = 0;
        cb_populate_ap_list();
    } else {
        make_label(list_area, "Tap Scan to find APs", &ui_font_pipboy_14, pip_dim());
    }

    // Utilities/Lists tools (e.g. Select AP, stable cat id 4) are UI-only -- Scan/Select/
    // Deselect + the checkbox list IS the whole tool. Their dispatch is a no-op, so a
    // START button would just freeze at "Starting..." (audit 2026-07-08). Omit it there.
    {
        uint8_t _tp = tool_cat(encoded);
        uint8_t _tid = (_tp < NUM_TOOL_CATS) ? tool_categories[_tp].id : 255;
        if (_tid != 4) {
            lv_obj_t *_sb = make_action_btn(right_pane,
                            (cb_op_running && cb_op_encoded == encoded) ? "> STOP <" : "> START <",
                            cb_tool_start_stop_cb, (void *)(intptr_t)encoded);
            if (single) {
                // Disable START until an AP is picked (single-select). Re-lit by the row tap.
                cb_rssi_start_btn = _sb;
                lv_obj_add_event_cb(_sb, cb_selfnull_on_delete, LV_EVENT_DELETE, &cb_rssi_start_btn);
                cb_rssi_refresh_start();
            }
        }
    }
}

// Build right pane for TAT_STA: Description + [Scan/Stop] [Select] [Deselect] + list + [> START <]
static void show_tool_sta(const ToolItem *wi, int32_t encoded) {
    lv_obj_t *desc = make_label(right_pane, wi->desc,
                                &ui_font_pipboy_16, pip_primary());
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(desc, lv_pct(100));

    // Button row: Scan/Stop + Select + Deselect
    lv_obj_t *brow = lv_obj_create(right_pane);
    lv_obj_remove_style_all(brow);
    lv_obj_add_style(brow, &style_container, 0);
    lv_obj_set_size(brow, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(brow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(brow, 4, 0);

    // Scan/Stop toggle for stations
    {
        lv_obj_t *scan_btn = make_small_btn(brow,
            (cb_op_running && cb_op_name && strstr(cb_op_name, "Scan")) ? "Stop" : "Scan",
            [](lv_event_t *e) {
                (void)e;
                if (cb_inline_scan_blocked_for(cb_inline_scan_page_enc)) return;   // F1, see the AP picker's note
                // Mirror of the AP picker's guard: only treat a tap as "stop" if a STATION
                // SCAN is actually running. If some OTHER tool was left running, the first
                // tap used to just stop it and print a stale "%d STAs", so it took two taps
                // to start scanning.
                bool sta_scanning = cb_op_running && cb_op_name && strstr(cb_op_name, "Scan");
                if (sta_scanning) {
                    cb_stop_operation();
                    int cnt = cb.getStationCount();
                    char dbuf[32];
                    snprintf(dbuf, sizeof(dbuf), "%d STAs", cnt);
                    if (lbl_stask) lv_label_set_text(lbl_stask, dbuf);
                    if (wifi_scan_btn_label) lv_label_set_text(wifi_scan_btn_label, "Scan");
                } else {
                    if (cb_op_running) cb_stop_operation();  // stop whatever else was running
                    rad_geiger_force_stop();   // FB11: bare flag clear latched the tick audio on
                    cb_ensure_wifi();
                    cb_clear_sta_results();  // store AND view, synchronously
                    cb.scanStations();
                    cb_op_running = true;
                    cb_op_name = "Scanning STAs";
                    cb_op_encoded = -1;
                    cb_manual_scan_kind = MON_STA;   // no tool behind this scan; say so
                    if (lbl_stask) lv_label_set_text(lbl_stask, "Scanning STAs");
                    if (wifi_scan_btn_label) lv_label_set_text(wifi_scan_btn_label, "Stop");
                    cb_start_scan_polling();
                }
            }, NULL);
        wifi_scan_btn_label = lv_obj_get_child(scan_btn, 0);
        lv_obj_add_event_cb(wifi_scan_btn_label, cb_selfnull_on_delete, LV_EVENT_DELETE, &wifi_scan_btn_label);
        // F1: dim + un-clickable while another operation owns the radio. The handler also
        // hard-refuses (below), so a stale-enabled button still cannot steal a running tool.
        cb_inline_scan_btn = scan_btn;
        lv_obj_add_event_cb(scan_btn, cb_selfnull_on_delete, LV_EVENT_DELETE, &cb_inline_scan_btn);
        cb_inline_scan_page_enc = encoded;
        cb_btn_set_enabled(scan_btn, !cb_inline_scan_blocked_for(encoded));
    }

    make_small_btn(brow, "Deselect", cb_deselect_stas_cb, NULL);

    // Live station list
    lv_obj_t *list_area = lv_obj_create(right_pane);
    lv_obj_remove_style_all(list_area);
    lv_obj_set_size(list_area, lv_pct(100), 60);
    lv_obj_set_style_border_color(list_area, pip_border(), 0);
    lv_obj_set_style_border_width(list_area, 1, 0);
    lv_obj_set_style_border_side(list_area, LV_BORDER_SIDE_FULL, 0);
    lv_obj_set_style_pad_all(list_area, 4, 0);
    lv_obj_set_flex_flow(list_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_style(list_area, &style_scrollbar, LV_PART_SCROLLBAR);
    cb_sta_list_area = list_area;
    lv_obj_add_event_cb(cb_sta_list_area, cb_selfnull_on_delete, LV_EVENT_DELETE, &cb_sta_list_area);

    if (cb.getStationCount() > 0) {
        cb_last_sta_count = 0;
        cb_populate_sta_list();
    } else {
        make_label(list_area, "Tap Scan to find stations", &ui_font_pipboy_14, pip_dim());
    }

    make_action_btn(right_pane,
                    (cb_op_running && cb_op_encoded == encoded) ? "> STOP <" : "> START <",
                    cb_tool_start_stop_cb, (void *)(intptr_t)encoded);
}

// Build right pane for TAT_SSID: Description + [From APs] [Add] [Random] + SSID list + [> START <]
static void show_tool_ssid(const ToolItem *wi, int32_t encoded) {
    lv_obj_t *desc = make_label(right_pane, wi->desc,
                                &ui_font_pipboy_16, pip_primary());
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(desc, lv_pct(100));

    lv_obj_t *brow = lv_obj_create(right_pane);
    lv_obj_remove_style_all(brow);
    lv_obj_add_style(brow, &style_container, 0);
    lv_obj_set_size(brow, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(brow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(brow, 4, 0);
    make_small_btn(brow, "From APs", cb_ssids_from_aps_cb, NULL);
    // "Add" was a fully-lit button wired to a NULL callback -- it did nothing, forever, with no
    // hint that it was unimplemented (owner-confirmed). Now it opens the keyboard and adds what
    // you type, reusing the same kb_open flow as Saved Networks.
    make_small_btn(brow, "Add", [](lv_event_t *e) {
        (void)e;
        kb_open("SSID to spam", "", false, [](const char *txt, void *ud) {
            (void)ud;
            if (!txt || !txt[0]) return;
            cb.addSSID(String(txt));
            cb_populate_ssid_list();
        }, NULL);
    }, NULL);
    make_small_btn(brow, "Random", cb_random_ssids_cb, NULL);

    lv_obj_t *list_area = lv_obj_create(right_pane);
    lv_obj_remove_style_all(list_area);
    lv_obj_set_size(list_area, lv_pct(100), 50);
    lv_obj_set_style_border_color(list_area, pip_border(), 0);
    lv_obj_set_style_border_width(list_area, 1, 0);
    lv_obj_set_style_border_side(list_area, LV_BORDER_SIDE_FULL, 0);
    lv_obj_set_style_pad_all(list_area, 4, 0);
    lv_obj_set_flex_flow(list_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_style(list_area, &style_scrollbar, LV_PART_SCROLLBAR);

    // Claim the list area so the From APs / Add / Random handlers can redraw it, and self-null
    // on delete so a handler firing after a nav/theme rebuild can't write freed memory.
    cb_ssid_list_area = list_area;
    lv_obj_add_event_cb(list_area, cb_selfnull_on_delete, LV_EVENT_DELETE, &cb_ssid_list_area);
    cb_populate_ssid_list();   // single source of truth for this pane's contents

    make_action_btn(right_pane,
                    (cb_op_running && cb_op_encoded == encoded) ? "> STOP <" : "> START <",
                    cb_tool_start_stop_cb, (void *)(intptr_t)encoded);
}

// ─── Full-screen keyboard modal ──────────────────────────────────────────
// Opens a modal with a text area + LVGL keyboard.
// On confirm, calls the provided callback with the entered text.
// user_data is passed through to the callback.

typedef void (*kb_done_cb_t)(const char *text, void *user_data);

static kb_done_cb_t   kb_on_done = NULL;
static void          *kb_user_data = NULL;
static lv_obj_t      *kb_modal = NULL;
// Carries the SSID between the two prompts of "+ Add Network" (SSID -> password) without
// borrowing wifi_join_ssid, which would retarget the Join WiFi page as a side effect.
static char           kb_pending_ssid[33] = "";

// When the current keyboard appeared. Two prompts chained back-to-back (Add Network:
// SSID -> password) are IDENTICAL full-screen layouts, so the new OK button lands on the
// exact pixel the just-deleted one occupied. A quick double-tap therefore committed the
// second prompt blind -- saving a network with an empty password as "open" without the user
// ever seeing the password step. Ignore OK for a moment after the modal opens.
static uint32_t       kb_opened_ms = 0;
#define KB_COMMIT_GUARD_MS 250

static void kb_close(void) {
    if (kb_modal) { lv_obj_delete(kb_modal); kb_modal = NULL; }
    kb_on_done = NULL;
    kb_user_data = NULL;
    kb_pending_ssid[0] = '\0';   // abandon any half-finished Add Network chain
    crt_scanlines_raise();
}

static void kb_open(const char *title, const char *initial,
                    bool password, kb_done_cb_t done_cb, void *ud)
{
    kb_on_done = done_cb;
    kb_user_data = ud;
    kb_opened_ms = millis();

    kb_modal = lv_obj_create(lv_screen_active());
    // Any path that frees the screen (ui_theme_switch_live deletes scr_main) must not leave
    // this global dangling -- kb_modal is a SCREEN child, so content_teardown() never sees it,
    // and a dangling non-NULL defeats the `if (!kb_modal)` guards. NULL is the safe state.
    lv_obj_add_event_cb(kb_modal, cb_selfnull_on_delete, LV_EVENT_DELETE, &kb_modal);
    lv_obj_remove_style_all(kb_modal);
    lv_obj_set_size(kb_modal, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(kb_modal, pip_bg(), 0);
    lv_obj_set_style_bg_opa(kb_modal, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(kb_modal, 4, 0);
    lv_obj_set_flex_flow(kb_modal, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(kb_modal, 4, 0);
    lv_obj_remove_flag(kb_modal, LV_OBJ_FLAG_SCROLLABLE);

    // Title
    make_label(kb_modal, title, &ui_font_pipboy_16, pip_highlight());

    // Text area
    lv_obj_t *ta = lv_textarea_create(kb_modal);
    lv_obj_set_width(ta, lv_pct(100));
    lv_obj_set_height(ta, 30);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, 64);
    if (initial && initial[0]) lv_textarea_set_text(ta, initial);
    if (password) lv_textarea_set_password_mode(ta, true);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ta, pip_primary(), 0);
    lv_obj_set_style_bg_color(ta, pip_bg_dark(), 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ta, pip_border(), 0);
    lv_obj_set_style_border_width(ta, 1, 0);

    // Button row: Cancel + OK
    lv_obj_t *btn_row = lv_obj_create(kb_modal);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_size(btn_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(btn_row, 8, 0);

    lv_obj_t *btn_cancel = lv_button_create(btn_row);
    lv_obj_set_size(btn_cancel, 70, 24);
    lv_obj_set_style_bg_color(btn_cancel, pip_border(), 0);
    lv_obj_t *lc = lv_label_create(btn_cancel);
    lv_label_set_text(lc, "Cancel");
    lv_obj_set_style_text_font(lc, &ui_font_pipboy_14, 0);
    lv_obj_set_style_text_color(lc, pip_primary(), 0);
    lv_obj_center(lc);
    lv_obj_add_event_cb(btn_cancel, [](lv_event_t *e) {
        (void)e; kb_close();
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_ok = lv_button_create(btn_row);
    lv_obj_set_size(btn_ok, 70, 24);
    lv_obj_set_style_bg_color(btn_ok, pip_highlight(), 0);
    lv_obj_t *lo = lv_label_create(btn_ok);
    lv_label_set_text(lo, "OK");
    lv_obj_set_style_text_font(lo, &ui_font_pipboy_14, 0);
    lv_obj_set_style_text_color(lo, pip_bg(), 0);
    lv_obj_center(lo);
    lv_obj_add_event_cb(btn_ok, [](lv_event_t *e) {
        (void)e;
        // SB1 (audit 2026-07-24): DETACH the globals BEFORE running the callback.
        // This used to be `if (kb_on_done) kb_on_done(...); kb_close();`. A callback is
        // allowed to open another keyboard -- Utilities > Saved Networks > "+ Add Network"
        // chains SSID -> password -- and kb_open() overwrites kb_modal unconditionally, so
        // the trailing kb_close() deleted the BRAND-NEW modal and orphaned the old one with
        // kb_modal == NULL. Net effect: nothing was added, `Cancel` became a no-op (it is
        // `if (kb_modal)`), the opaque full-screen modal blocked the whole UI, and the next
        // OK tap ran lv_obj_get_child(NULL, 1) -> LoadProhibited panic. Four taps, both
        // SKUs, release build.
        //
        // Order matters and this order is deliberate: clear the globals -> run the callback
        // (which may bind a fresh keyboard) -> delete OUR modal last. The textarea is still
        // alive while the callback reads `text`, so no copy/truncation is needed.
        if (!kb_modal) return;                  // nothing bound: ignore rather than deref
        // Swallow an OK that lands within a few hundred ms of this modal appearing: chained
        // prompts reuse the same geometry, so a double-tap would commit the second one blind.
        if (millis() - kb_opened_ms < KB_COMMIT_GUARD_MS) return;
        lv_obj_t *modal = kb_modal;
        kb_done_cb_t cb = kb_on_done;
        void *ud = kb_user_data;
        kb_modal = NULL; kb_on_done = NULL; kb_user_data = NULL;

        // Find the textarea (child 1 of modal: 0=title, 1=ta, 2=btn_row, 3=kb)
        lv_obj_t *ta = lv_obj_get_child(modal, 1);
        const char *text = ta ? lv_textarea_get_text(ta) : "";
        if (cb) cb(text ? text : "", ud);

        // Never delete a NEWER modal the callback may have opened -- that is exactly the bug
        // above. `modal == kb_modal` is impossible here (the old modal is still allocated when
        // kb_open() allocates the new one), so this reduces to "delete our own".
        // The lv_obj_is_valid() call is belt-and-braces for a future callback that frees the
        // screen: today none can. kb_modal is a SIBLING of content_obj (both are children of
        // scr_main, ui_nav.h:4226 / :11276), so the password step's rebuild_content() --
        // which only clears content_obj's children -- cannot invalidate it. An earlier version
        // of this comment claimed it could; that was wrong (adversarial review, 2026-07-25).
        // NOTE lv_obj_is_valid() is a live-object registry walk, not a dereference
        // (libs/lvgl/src/core/lv_obj.c:413), so passing a stale pointer is safe -- but it is
        // pointer-identity only, so it cannot detect a recycled heap block.
        if (modal != kb_modal && lv_obj_is_valid(modal)) lv_obj_delete(modal);
        crt_scanlines_raise();
    }, LV_EVENT_CLICKED, NULL);

    // Keyboard
    lv_obj_t *kb = lv_keyboard_create(kb_modal);
    lv_obj_set_flex_grow(kb, 1);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(kb, pip_bg_dark(), 0);
    lv_obj_set_style_bg_color(kb, pip_border(), LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, pip_primary(), LV_PART_ITEMS);
    // Special keys (Shift, Backspace, Enter) - same style as normal keys
    lv_obj_set_style_bg_color(kb, pip_border(), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(kb, pip_primary(), LV_PART_ITEMS | LV_STATE_CHECKED);

    crt_scanlines_raise();
}

// ─── Join WiFi state ────────────────────────────────────────────────────────
// (wifi join state moved to forward declarations above cb_scan_poll_cb)

static void wifi_join_update_labels(void) {
    // cb_safe: wifi_join_ssid is air-sourced whenever it was filled from the AP picker
    // (`strncpy(wifi_join_ssid, ap2.essid, ...)`) or from a saved credential that was itself
    // seeded that way -- only the keyboard path is user-typed. The scan LIST sanitises essids
    // and this label did not, so the one screen that shows a single chosen SSID at size 14 was
    // the soft spot. Recolour to match make_name_label: LVGL 9 cannot tint a single glyph, so
    // a hostile codepoint anywhere paints the whole label.
    if (wifi_ssid_label) {
        lv_label_set_text(wifi_ssid_label,
                          cb_safe(wifi_join_ssid[0] ? wifi_join_ssid : "(none)"));
        lv_obj_set_style_text_color(wifi_ssid_label,
            cb_safe_had_hostile ? lv_color_hex(CB_WARN_RGB) : pip_primary(), 0);
    }
    if (wifi_pw_label)
        lv_label_set_text(wifi_pw_label, wifi_join_pw[0] ? "********" : "(none)");
}

// Build right pane for TAT_TEXT
static void show_tool_text(const ToolItem *wi, int32_t encoded) {
    uint8_t ci = tool_cat(encoded);
    uint8_t ii = tool_item(encoded);

    lv_obj_t *desc = make_label(right_pane, wi->desc,
                                &ui_font_pipboy_16, pip_primary());
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(desc, lv_pct(100));

    lv_obj_t *sp = lv_obj_create(right_pane);
    lv_obj_remove_style_all(sp);
    lv_obj_set_size(sp, 1, 4);

    if (tool_categories[ci].id == 5 && ii == 0) {
        // ─── Join WiFi (Network id5, item0): SSID + Password + Connect ────

        // Show current connection status if connected
        if (WiFi.status() == WL_CONNECTED) {
            char cbuf[80];
            snprintf(cbuf, sizeof(cbuf), "Connected to:\n%s", WiFi.SSID().c_str());
            lv_obj_t *clbl = make_label(right_pane, cbuf, &ui_font_pipboy_14, pip_highlight());
            lv_label_set_long_mode(clbl, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(clbl, lv_pct(100));

            // Button row: Disconnect + Save
            lv_obj_t *conn_row = lv_obj_create(right_pane);
            lv_obj_remove_style_all(conn_row);
            lv_obj_add_style(conn_row, &style_container, 0);
            lv_obj_set_size(conn_row, lv_pct(100), LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(conn_row, LV_FLEX_FLOW_ROW);
            lv_obj_set_style_pad_gap(conn_row, 4, 0);

            make_small_btn(conn_row, "Disconnect", [](lv_event_t *e) {
                (void)e;
                WiFi.disconnect();
                CB_LOGLN("[CB] WiFi disconnected by user");
                if (wifi_status_label)
                    lv_label_set_text(wifi_status_label, "Disconnected");
                tool_page_reshow();   // stay on Join WiFi, not cur_sel's tool
            }, NULL);

            // Offer to save if not already saved
            if (wifi_creds_find(wifi_join_ssid) < 0 && wifi_join_ssid[0]) {
                make_small_btn(conn_row, "Save", [](lv_event_t *e) {
                    (void)e;
                    if (wifi_creds_add(wifi_join_ssid, wifi_join_pw)) {
                        CB_LOGF("[WIFI] Saved: %s\n", wifi_join_ssid);
                        tool_page_reshow();   // stay on Join WiFi, not cur_sel's tool
                    }
                }, NULL);
            } else if (wifi_join_ssid[0]) {
                make_label(conn_row, "Saved", &ui_font_pipboy_14, pip_dim());
            }

            lv_obj_t *sep = lv_obj_create(right_pane);
            lv_obj_remove_style_all(sep);
            lv_obj_set_size(sep, lv_pct(100), 1);
            lv_obj_set_style_bg_color(sep, pip_border(), 0);
            lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
        }

        // SSID row: label + [Edit] + [From APs]
        make_label(right_pane, "SSID", &ui_font_pipboy_14, pip_dim());
        lv_obj_t *ssid_row = lv_obj_create(right_pane);
        lv_obj_remove_style_all(ssid_row);
        lv_obj_add_style(ssid_row, &style_container, 0);
        lv_obj_set_size(ssid_row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(ssid_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(ssid_row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(ssid_row, 4, 0);

        wifi_ssid_label = make_name_label(ssid_row,   // air-sourced; see wifi_join_update_labels
            wifi_join_ssid[0] ? wifi_join_ssid : "(none)",
            &ui_font_pipboy_14, pip_primary());
        lv_obj_set_flex_grow(wifi_ssid_label, 1);
        lv_label_set_long_mode(wifi_ssid_label, LV_LABEL_LONG_CLIP);

        // Button row: Scan, Select, Edit
        lv_obj_t *btn_row = lv_obj_create(right_pane);
        lv_obj_remove_style_all(btn_row);
        lv_obj_add_style(btn_row, &style_container, 0);
        lv_obj_set_size(btn_row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_gap(btn_row, 4, 0);

        // Scan/Stop toggle button
        {
            lv_obj_t *scan_btn = make_small_btn(btn_row,
                // FB6: the LABEL and the ACTION used to disagree. The label asked
                // `strstr(cb_op_name, "Scan")` while the action below branched on a bare
                // `cb_op_running`. No ToolItem name contains the case-sensitive substring
                // "Scan" (verified across all of cat_detect..cat_network), so with ANY tool
                // running this rendered "Scan" and the tap STOPPED it -- two taps to scan,
                // and the first tap silently killed someone's capture. Both sides now ask the
                // same question, matching the AP picker (3910) and STA picker (4089).
                (cb_op_running && cb_op_name && strstr(cb_op_name, "Scan")) ? "Stop" : "Scan",
                [](lv_event_t *e) {
                    (void)e;
                    // F1: Join WiFi has NO START/STOP button of its own, so under the
                    // narrowed same-page rule nothing here can ever be lied about and this
                    // guard never fires -- cb_op_encoded can't equal this page's tool because
                    // Join WiFi is not startable. Kept anyway so the three inline Scan
                    // handlers stay identical: the next person to make this page startable
                    // gets the protection for free instead of rediscovering the bug.
                    if (cb_inline_scan_blocked_for(cb_inline_scan_page_enc)) return;
                    bool ap_scanning = cb_op_running && cb_op_name && strstr(cb_op_name, "Scan");
                    if (ap_scanning) {
                        // FB5: this hand-rolled teardown cleared the UI's idea of "running"
                        // but never called cb.stopScan(), which is the ONLY thing that clears
                        // WiFiScan's currentScanMode. cb.loop() keeps driving
                        // wifi_scan_obj.main() every pass, where all active TX lives -- so on
                        // Res34rch the badge kept RADIATING deauth/beacon-spam with the status
                        // bar reading "Stopped" and nothing on screen. It also skipped
                        // cb.finishCapture() (leaving an open Raw/PCAP unfinalised, so a user
                        // told "Stopped" powers off and loses the tail) and
                        // setRawTxMode(false) (leaving APSTA up). Because cb_op_running was
                        // already false, the NEXT tool start skipped its own
                        // `if (cb_op_running) cb_stop_operation();` too.
                        // This was the THIRD instance of the pattern fixed at 3910 and 4089.
                        cb_stop_operation();
                        int cnt = cb.getAPCount();
                        char dbuf[32];
                        snprintf(dbuf, sizeof(dbuf), "Stopped: %d APs", cnt);
                        if (lbl_stask) lv_label_set_text(lbl_stask, dbuf);
                        if (wifi_status_label) lv_label_set_text(wifi_status_label, dbuf);
                        if (wifi_scan_btn_label) lv_label_set_text(wifi_scan_btn_label, "Scan");
                    } else {
                        if (cb_op_running) cb_stop_operation();  // stop whatever else was running
                        // Start scanning
                        rad_geiger_force_stop();   // FB11: bare flag clear latched the tick audio on
                        cb_ensure_wifi();
                        cb_clear_ap_results();   // store AND view, synchronously
                        cb.scanAPs();
                        cb_op_running = true;
                        cb_op_name = "Scanning APs";
                        cb_op_encoded = -1;
                        cb_manual_scan_kind = MON_AP;   // else a prior STA scan's kind sticks
                        if (lbl_stask) lv_label_set_text(lbl_stask, "0 APs found");
                        if (wifi_status_label) lv_label_set_text(wifi_status_label, "Scanning...");
                        if (wifi_scan_btn_label) lv_label_set_text(wifi_scan_btn_label, "Stop");
                        cb_start_scan_polling();
                    }
                }, NULL);
            // Save reference to button's label for toggling text
            wifi_scan_btn_label = lv_obj_get_child(scan_btn, 0);
        lv_obj_add_event_cb(wifi_scan_btn_label, cb_selfnull_on_delete, LV_EVENT_DELETE, &wifi_scan_btn_label);
            // Claim the inline-Scan globals for THIS page. Without this, page_enc would still
            // hold the last AP/station picker's tool, so the handler above would compare
            // cb_op_encoded against a tool that is not on screen -- a stale global doing
            // exactly what this whole sweep is about.
            cb_inline_scan_btn = scan_btn;
            cb_inline_scan_page_enc = encoded;
            lv_obj_add_event_cb(scan_btn, cb_selfnull_on_delete, LV_EVENT_DELETE, &cb_inline_scan_btn);
            cb_btn_set_enabled(scan_btn, !cb_inline_scan_blocked_for(encoded));
        }

        // Select button - pick from scanned APs
        make_small_btn(btn_row, "Select", [](lv_event_t *e) {
            (void)e;
            int count = cb.getAPCount();
            if (count == 0) {
                if (wifi_status_label)
                    lv_label_set_text(wifi_status_label, "No APs - run Scan first");
                return;
            }

            lv_obj_t *modal = lv_obj_create(lv_screen_active());
            lv_obj_remove_style_all(modal);
            lv_obj_set_size(modal, SCREEN_W, SCREEN_H);
            lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(modal, LV_OPA_80, 0);
            lv_obj_remove_flag(modal, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *box = lv_obj_create(modal);
            lv_obj_remove_style_all(box);
            lv_obj_set_size(box, 260, 180);
            lv_obj_center(box);
            lv_obj_set_style_bg_color(box, pip_bg(), 0);
            lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(box, pip_highlight(), 0);
            lv_obj_set_style_border_width(box, 2, 0);
            lv_obj_set_style_pad_all(box, 8, 0);
            lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_gap(box, 4, 0);
            lv_obj_add_style(box, &style_scrollbar, LV_PART_SCROLLBAR);

            make_label(box, "Select AP", &ui_font_pipboy_16, pip_highlight());

            for (int i = 0; i < count && i < 20; i++) {
                CBAccessPointInfo ap;
                if (!cb.getAP(i, ap)) continue;
                ap.essid[sizeof(ap.essid) - 1] = '\0';
                lv_obj_t *btn = lv_button_create(box);
                lv_obj_remove_style_all(btn);
                lv_obj_add_style(btn, &style_list_btn, 0);
                lv_obj_add_style(btn, &style_list_btn_pressed, LV_STATE_PRESSED);
                lv_obj_set_width(btn, lv_pct(100));
                lv_obj_set_height(btn, LV_SIZE_CONTENT);
                lv_obj_t *lbl = lv_label_create(btn);
                lv_label_set_text(lbl, cb_safe(ap.essid));
                lv_obj_set_style_text_font(lbl, &ui_font_pipboy_14, 0);
                lv_obj_set_style_text_color(lbl,
                    cb_safe_had_hostile ? lv_color_hex(CB_WARN_RGB) : pip_primary(), 0);
                // Stash the AP index, NOT the label text -- the label is the
                // SANITIZED display string (boxes/markup); we must join the REAL
                // essid, re-fetched by index on tap.
                lv_obj_set_user_data(btn, (void *)(intptr_t)i);
                lv_obj_add_event_cb(btn, [](lv_event_t *e2) {
                    lv_obj_t *b = (lv_obj_t *)lv_event_get_target(e2);
                    int idx = (int)(intptr_t)lv_obj_get_user_data(b);
                    CBAccessPointInfo ap2;
                    if (cb.getAP(idx, ap2)) {
                        ap2.essid[sizeof(ap2.essid) - 1] = '\0';
                        strncpy(wifi_join_ssid, ap2.essid, sizeof(wifi_join_ssid) - 1);
                        wifi_join_ssid[sizeof(wifi_join_ssid) - 1] = '\0';
                        wifi_join_update_labels();
                        // Target chosen -- stop the AP scan (no reason to keep sniffing).
                        // FB5: was another hand-rolled teardown that never called
                        // cb.stopScan(), so currentScanMode stayed set and cb.loop() kept
                        // driving the scan (and, on Res34rch, any active TX) with the UI
                        // showing it stopped. cb_stop_operation() does the whole job:
                        // stopScan + finishCapture + setRawTxMode(false) + the timers.
                        if (cb_op_running) {
                            cb_stop_operation();
                            if (wifi_scan_btn_label) lv_label_set_text(wifi_scan_btn_label, "Scan");
                        }
                    }
                    lv_obj_delete(lv_obj_get_parent(lv_obj_get_parent(b)));
                    crt_scanlines_raise();
                }, LV_EVENT_CLICKED, NULL);
            }

            lv_obj_t *btn_c = lv_button_create(box);
            lv_obj_set_size(btn_c, lv_pct(100), 24);
            lv_obj_set_style_bg_color(btn_c, pip_border(), 0);
            lv_obj_t *lc = lv_label_create(btn_c);
            lv_label_set_text(lc, "Cancel");
            lv_obj_set_style_text_font(lc, &ui_font_pipboy_14, 0);
            lv_obj_set_style_text_color(lc, pip_primary(), 0);
            lv_obj_center(lc);
            lv_obj_add_event_cb(btn_c, [](lv_event_t *e2) {
                lv_obj_t *b = (lv_obj_t *)lv_event_get_target(e2);
                lv_obj_delete(lv_obj_get_parent(lv_obj_get_parent(b)));
                crt_scanlines_raise();
            }, LV_EVENT_CLICKED, NULL);

            crt_scanlines_raise();
        }, NULL);

        // Edit button - manual SSID entry via keyboard
        make_small_btn(btn_row, "Edit", [](lv_event_t *e) {
            (void)e;
            kb_open("Enter SSID", wifi_join_ssid, false, [](const char *text, void *ud) {
                (void)ud;
                strncpy(wifi_join_ssid, text, sizeof(wifi_join_ssid) - 1);
                wifi_join_ssid[sizeof(wifi_join_ssid) - 1] = '\0';
                wifi_join_update_labels();
            }, NULL);
        }, NULL);

        // Password row
        make_label(right_pane, "Password", &ui_font_pipboy_14, pip_dim());
        lv_obj_t *pw_row = lv_obj_create(right_pane);
        lv_obj_remove_style_all(pw_row);
        lv_obj_add_style(pw_row, &style_container, 0);
        lv_obj_set_size(pw_row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(pw_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(pw_row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(pw_row, 4, 0);

        wifi_pw_label = make_label(pw_row,
            wifi_join_pw[0] ? "********" : "(none)",
            &ui_font_pipboy_14, pip_primary());
        lv_obj_set_flex_grow(wifi_pw_label, 1);

        make_small_btn(pw_row, "Edit", [](lv_event_t *e) {
            (void)e;
            kb_open("Enter Password", "", false, [](const char *text, void *ud) {
                (void)ud;
                strncpy(wifi_join_pw, text, sizeof(wifi_join_pw) - 1);
                wifi_join_pw[sizeof(wifi_join_pw) - 1] = '\0';
                wifi_join_update_labels();
            }, NULL);
        }, NULL);

        // Save this network -- ALWAYS visible (the connected-state Save is easy to scroll
        // past). Persistence is opt-in: nothing is saved unless you tap this, so creds
        // aren't stored by default. Shows "Saved" once this SSID is in the store.
        {
            bool already = wifi_join_ssid[0] && wifi_creds_find(wifi_join_ssid) >= 0;
            if (already) {
                make_label(right_pane, "This network is saved.", &ui_font_pipboy_14, pip_highlight());
            } else {
                make_small_btn(right_pane, "Save Network", [](lv_event_t *e) {
                    (void)e;
                    if (!wifi_join_ssid[0]) {
                        if (wifi_status_label) lv_label_set_text(wifi_status_label, "Enter an SSID first");
                        return;
                    }
                    if (wifi_creds_add(wifi_join_ssid, wifi_join_pw)) {
                        CB_LOGF("[WIFI] Saved: %s\n", wifi_join_ssid);
                        if (wifi_status_label) lv_label_set_text(wifi_status_label, "Saved");
                        rebuild_content();
                    }
                }, NULL);
                lv_obj_t *note = make_label(right_pane,
                    "Not saved unless you tap Save. Saved networks persist across reboots.",
                    &ui_font_pipboy_14, pip_dim());
                lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
                lv_obj_set_width(note, lv_pct(100));
            }
        }

        // Saved networks - quick-fill SSID + password
        if (wifi_cred_count > 0) {
            make_label(right_pane, "Saved", &ui_font_pipboy_14, pip_dim());
            for (int i = 0; i < wifi_cred_count; i++) {
                lv_obj_t *btn = lv_button_create(right_pane);
                lv_obj_remove_style_all(btn);
                lv_obj_add_style(btn, &style_list_btn, 0);
                lv_obj_add_style(btn, &style_list_btn_pressed, LV_STATE_PRESSED);
                lv_obj_set_width(btn, lv_pct(100));
                lv_obj_set_height(btn, LV_SIZE_CONTENT);
                lv_obj_t *lbl = lv_label_create(btn);
                // Air-sourced: a saved credential's SSID is normally whatever the AP picker
                // copied out of a beacon, so it carries the same tofu/bidi exposure as the
                // scan list it came from.
                lv_label_set_text(lbl, cb_safe(wifi_creds[i].ssid));
                lv_obj_set_style_text_font(lbl, &ui_font_pipboy_14, 0);
                lv_obj_set_style_text_color(lbl,
                    cb_safe_had_hostile ? lv_color_hex(CB_WARN_RGB) : pip_primary(), 0);
                lv_obj_add_event_cb(btn, [](lv_event_t *e) {
                    int ci = (int)(intptr_t)lv_event_get_user_data(e);
                    strncpy(wifi_join_ssid, wifi_creds[ci].ssid, sizeof(wifi_join_ssid) - 1);
                    wifi_join_ssid[sizeof(wifi_join_ssid) - 1] = '\0';
                    strncpy(wifi_join_pw, wifi_creds[ci].pw, sizeof(wifi_join_pw) - 1);
                    wifi_join_pw[sizeof(wifi_join_pw) - 1] = '\0';
                    wifi_join_update_labels();
                    if (wifi_status_label)
                        lv_label_set_text(wifi_status_label, "Loaded - tap Connect");
                }, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            }
        }

        // Status label
        wifi_status_label = make_label(right_pane, "", &ui_font_pipboy_14, pip_dim());
        lv_obj_add_event_cb(wifi_status_label, cb_selfnull_on_delete, LV_EVENT_DELETE, &wifi_status_label);

        // Connect button
        make_action_btn(right_pane, "> CONNECT <", [](lv_event_t *e) {
            (void)e;
            if (!wifi_join_ssid[0]) {
                if (wifi_status_label)
                    lv_label_set_text(wifi_status_label, "Enter an SSID first");
                return;
            }
            // FB5: third hand-rolled teardown. This one is the least dangerous of the three
            // (joinWiFi itself later sets currentScanMode to WIFI_CONNECTED/OFF), but it still
            // skipped cb.stopScan()/finishCapture(), so an open Raw/PCAP was left unfinalised
            // and any active-TX mode kept running until the join completed. Use the one path.
            if (cb_op_running) cb_stop_operation();
            // F4 (owner-confirmed) -- the Geiger is NOT a "tool": rad_toggle_cb never sets
            // cb_op_running, so the gated teardown above steps straight past it while the
            // UNCONDITIONAL promiscuous kill below rips out the very RX callback the Geiger
            // counts deauths through. WiFiScan::main() only channel-hops for
            // WIFI_SCAN_DEAUTH and never re-registers that callback, and every deauth_frames
            // writer is downstream of it -- so the gauge read a confident 0/s forever while
            // rad_geiger_active stayed true: "Stop" on the button, elapsed timer still
            // counting, needle at zero. A plausible "the air is clean here" on a DEFCON floor,
            // which is the worst possible way for this particular tool to fail.
            // joinWiFi also overwrites currentScanMode to WIFI_CONNECTED without a StopScan,
            // and isScanning() is `currentScanMode != WIFI_SCAN_OFF`, so WIFI_CONNECTED counts
            // as scanning and the status_bar_timer_cb self-heal could not fire either.
            // Stop it HONESTLY through the one shared helper (which resets the needle, the
            // button label, the tick audio and the status text) instead of silently deafening
            // it. Same fix shape as F3/F7: route the Geiger through rad_geiger_force_stop()
            // rather than adding another cb_op_running-keyed guard that skips it.
            rad_geiger_force_stop();
            if (wifi_scan_btn_label) lv_label_set_text(wifi_scan_btn_label, "Scan");
            // Keep these explicit: the documented precondition for cb.joinWiFi() is that
            // promiscuous mode is OFF and its RX callback is cleared. cb_stop_operation() now
            // does this too (FB1), but joinWiFi is reachable without a tool having run.
            esp_wifi_set_promiscuous_rx_cb(NULL);
            esp_wifi_set_promiscuous(false);

            audio_suspend();

            if (wifi_status_label)
                lv_label_set_text(wifi_status_label, "Connecting...");
            lv_refr_now(NULL);

            CB_LOGF("[CB] joinWiFi(%s) BLOCKING on Core 1\n", wifi_join_ssid);
            bool ok = cb.joinWiFi(String(wifi_join_ssid), String(wifi_join_pw));
            CB_LOGF("[CB] joinWiFi -> %s\n", ok ? "OK" : "FAIL");

            audio_resume();

            if (wifi_status_label)
                lv_label_set_text(wifi_status_label, ok ? "Connected!" : "Failed");
            if (lbl_stask)
                lv_label_set_text(lbl_stask, ok ? "WiFi connected" : "");
        }, NULL);

    } else if (tool_categories[ci].id == 4 && ii == 7) {
        // ─── Add SSID (Utilities id4, item7): text input + add button ────
        make_action_btn(right_pane, "> ENTER SSID <", [](lv_event_t *e) {
            (void)e;
            kb_open("Enter SSID to add", "", false, [](const char *text, void *ud) {
                (void)ud;
                if (text && text[0]) {
                    cb.addSSID(String(text));
                    CB_LOGF("[CB] Added SSID: %s\n", text);
                }
            }, NULL);
        }, NULL);

    } else {
        // Generic text - fallback
        make_action_btn(right_pane, "> ENTER <", cb_tool_execute_cb, (void *)(intptr_t)encoded);
    }
}

// Build right pane for TAT_FILE: Description + [> START <]
static void show_tool_file(const ToolItem *wi, int32_t encoded) {
    lv_obj_t *desc = make_label(right_pane, wi->desc,
                                &ui_font_pipboy_16, pip_primary());
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(desc, lv_pct(100));

    make_label(right_pane, "(SD card file selection\nnot yet implemented)",
               &ui_font_pipboy_14, pip_dim());

    make_action_btn(right_pane,
                    (cb_op_running && cb_op_encoded == encoded) ? "> STOP <" : "> START <",
                    cb_tool_start_stop_cb, (void *)(intptr_t)encoded);
}

// Build right pane for TAT_IMMEDIATE: Description + [> EXECUTE <]
static void show_tool_immediate(const ToolItem *wi, int32_t encoded) {
    lv_obj_t *desc = make_label(right_pane, wi->desc,
                                &ui_font_pipboy_16, pip_primary());
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(desc, lv_pct(100));

    lv_obj_t *sp = lv_obj_create(right_pane);
    lv_obj_remove_style_all(sp);
    lv_obj_set_size(sp, 1, 4);

    make_action_btn(right_pane, "> EXECUTE <", cb_tool_execute_cb, (void *)(intptr_t)encoded);
}

static void show_tool_detail(uint8_t cat_idx, uint8_t item_idx);   // defined below

// ── Which tool page to redraw after a handler mutates something ─────────────
// Tool-page handlers used to call rebuild_content(), which rebuilds from cur_sel and therefore
// dumped the user onto an UNRELATED tool (owner-reported twice: once from Saved Networks
// "+ Add Network", once from Join WiFi "Save" -- both landed on Utilities > List AirTags).
// I fixed the first instance with a bespoke `saved_net_return_enc` and missed its siblings,
// which is precisely the whack-a-mole the class rule warns about. So this is set ONCE in
// show_tool_detail() -- every tool page gets it for free, including ones added later -- and
// tool_page_reshow() is what a handler calls instead of rebuild_content().
static int32_t tool_page_return_enc = -1;

static void tool_page_reshow(void) {
    if (tool_page_return_enc >= 0)
        show_tool_detail(tool_cat(tool_page_return_enc), tool_item(tool_page_return_enc));
    else
        rebuild_content();   // not on a tool page -> the old behaviour is correct
}

// Build right pane for TAT_LIST_VIEW: Description + scrollable read-only list
static void show_tool_list_view(const ToolItem *wi, uint8_t cat_idx, uint8_t item_idx) {
    lv_obj_t *desc = make_label(right_pane, wi->desc,
                                &ui_font_pipboy_16, pip_primary());
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(desc, lv_pct(100));

    // Saved Networks: "+ Add Network" lives OUTSIDE the scroll box, ABOVE the list.
    // It used to be the last child INSIDE list_area, so every saved network pushed it further
    // down and eventually below the 80px fold -- at which point its reported coordinates land
    // on whatever is underneath and the button is simply unreachable without scrolling. That
    // is a real usability bug (owner-reported) and it is also the most likely explanation for
    // the intermittent SB1 failures, which alternated between "could not tap + Add Network"
    // and "no keyboard modal appeared" as the row count drifted across the fold.
    // Created BEFORE list_area so flex order puts it above, with no move_to_index juggling.
    if (item_idx == 6) {
        make_small_btn(right_pane, "+ Add Network", [](lv_event_t *e) {
            (void)e;
            kb_open("Network SSID", "", false, [](const char *ssid, void *ud) {
                (void)ud;
                if (!ssid || !ssid[0]) return;
                // Stash the SSID across the two-step prompt in a DEDICATED buffer. This
                // used to borrow wifi_join_ssid, so adding a saved network silently
                // retargeted the Join WiFi page as a side effect (audit SB1 note).
                strncpy(kb_pending_ssid, ssid, sizeof(kb_pending_ssid) - 1);
                kb_pending_ssid[sizeof(kb_pending_ssid) - 1] = '\0';
                kb_open("Password (blank=open)", "", false, [](const char *pw, void *ud2) {
                    (void)ud2;
                    if (!kb_pending_ssid[0]) return;
                    wifi_creds_add(kb_pending_ssid, pw ? pw : "");
                    kb_pending_ssid[0] = '\0';
                    // Return to Saved Networks instead of rebuild_content(), which rebuilt from
                    // cur_sel and dumped the user back on an unrelated tool (owner-reported:
                    // it landed on Utilities > List AirTags). show_tool_detail() rebuilds only
                    // the right pane and nulls the timer-written refs on the way in.
                    tool_page_reshow();
                }, NULL);
            }, NULL);
        }, NULL);
    }

    lv_obj_t *list_area = lv_obj_create(right_pane);
    lv_obj_remove_style_all(list_area);
    lv_obj_set_size(list_area, lv_pct(100), 80);
    lv_obj_set_style_border_color(list_area, pip_border(), 0);
    lv_obj_set_style_border_width(list_area, 1, 0);
    lv_obj_set_style_border_side(list_area, LV_BORDER_SIDE_FULL, 0);
    lv_obj_set_style_pad_all(list_area, 4, 0);
    lv_obj_set_flex_flow(list_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_style(list_area, &style_scrollbar, LV_PART_SCROLLBAR);

    // Populate from ClipBoy data based on which list view
    bool has_data = false;
    if (item_idx == 0) {  // List APs
        int count = cb.getAPCount();
        for (int i = 0; i < count; i++) {
            CBAccessPointInfo ap;
            if (cb.getAP(i, ap)) {
                ap.essid[sizeof(ap.essid) - 1] = '\0';
                char buf[64];
                snprintf(buf, sizeof(buf), "%s (%ddBm) ch%d", cb_safe(ap.essid), ap.rssi, ap.channel);
                make_label(list_area, buf, &ui_font_pipboy_14, pip_primary());
                has_data = true;
            }
        }
    } else if (item_idx == 1) {  // List SSIDs
        int count = cb.getSSIDCount();
        for (int i = 0; i < count; i++) {
            CBSSIDInfo info;
            if (cb.getSSID(i, info)) {
                info.essid[sizeof(info.essid) - 1] = '\0';
                make_name_label(list_area, info.essid, &ui_font_pipboy_14, pip_primary());
                has_data = true;
            }
        }
    } else if (item_idx == 2) {  // List Stations
        int count = cb.getStationCount();
        for (int i = 0; i < count; i++) {
            CBStationInfo sta;
            if (cb.getStation(i, sta)) {
                make_label(list_area, cb.macToString(sta.mac).c_str(),
                           &ui_font_pipboy_14, pip_primary());
                has_data = true;
            }
        }
    } else if (item_idx == 3) {  // List BT Devices
        int count = cb.getBTDeviceCount();
        for (int i = 0; i < count; i++) {
            CBBTDeviceInfo dev;
            if (cb.getBTDevice(i, dev)) {
                dev.name[sizeof(dev.name) - 1] = '\0';
                char buf[64];
                snprintf(buf, sizeof(buf), "%s [%s] %ddBm",
                         dev.name[0] ? cb_safe(dev.name) : "??", dev.mac, dev.rssi);
                make_label(list_area, buf, &ui_font_pipboy_14, pip_primary());
                has_data = true;
            }
        }
    } else if (item_idx == 4) {  // List AirTags
        int count = cb.getAirTagCount();
        for (int i = 0; i < count; i++) {
            CBAirTagInfo tag;
            if (cb.getAirTag(i, tag)) {
                char buf[48];
                snprintf(buf, sizeof(buf), "%s %ddBm", tag.mac, tag.rssi);
                make_label(list_area, buf, &ui_font_pipboy_14, pip_primary());
                has_data = true;
            }
        }
    } else if (item_idx == 5) {  // List Flippers
        int count = cb.getFlipperCount();
        for (int i = 0; i < count; i++) {
            CBFlipperInfo flip;
            if (cb.getFlipper(i, flip)) {
                flip.name[sizeof(flip.name) - 1] = '\0';
                char buf[64];
                snprintf(buf, sizeof(buf), "%s [%s]",
                         flip.name[0] ? cb_safe(flip.name) : "??", flip.mac);
                make_label(list_area, buf, &ui_font_pipboy_14, pip_primary());
                has_data = true;
            }
        }
    } else if (item_idx == 6) {  // Saved Networks
        for (int i = 0; i < wifi_cred_count; i++) {
            lv_obj_t *row = lv_obj_create(list_area);
            lv_obj_remove_style_all(row);
            lv_obj_add_style(row, &style_container, 0);
            lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_ver(row, 2, 0);

            // Sanitise into a LOCAL first, exactly as the WPS manufacturer strip has to:
            // cb_safe() hands back one of four rotating static buffers and sets the warn flag
            // as a side effect, so interpolating the call directly into snprintf's argument
            // list would sample both unreliably (argument evaluation order is unspecified).
            // 104: an SSID is char[33], and every byte can expand to a 3-byte marker
            // (32 * 3 + NUL = 97), so this never truncates mid-UTF-8-sequence.
            char sname[104];
            strncpy(sname, cb_safe(wifi_creds[i].ssid), sizeof(sname) - 1);
            sname[sizeof(sname) - 1] = '\0';
            bool sname_warn = cb_safe_had_hostile;
            char buf[128];
            snprintf(buf, sizeof(buf), "%s %s", sname,
                     wifi_creds[i].pw[0] ? "(pw)" : "(open)");
            lv_obj_t *lbl = make_label(row, buf, &ui_font_pipboy_14,
                                       sname_warn ? lv_color_hex(CB_WARN_RGB) : pip_primary());
            lv_obj_set_flex_grow(lbl, 1);

            // Delete button
            make_small_btn(row, "X", [](lv_event_t *e) {
                int idx = (int)(intptr_t)lv_event_get_user_data(e);
                wifi_creds_remove(idx);
                // DEFERRED. rebuild_content() tears down the whole content pane -- including
                // THIS button and the row it sits in -- while LVGL is still dispatching this
                // button's own event. That is the project's documented "rebuild inside a touch
                // event" trap, and led_theme_refresh_async() already had to be deferred for the
                // same reason. Symptom here was not a clean crash but intermittent misbehaviour
                // straight afterwards: the SB1 regression test, whose cleanup loop taps this
                // button, failed ~1 run in 3 with "no keyboard modal appeared", while the same
                // Add Network tap in isolation succeeded 8/8.
                // tool_page_reshow(), NOT rebuild_content(). An adversarial review caught that
                // 25de7da8 claimed "one return path for every tool page" while leaving THIS
                // handler -- in the same function as the "+ Add Network" one it did fix --
                // calling rebuild_content(), which with nothing running falls through to
                // show_tool_detail(0, 0) = Detect > AirTag. So deleting a saved network dumped
                // you off Saved Networks and you could not delete a second one without
                // navigating back. Third sibling of the same handler shape, missed twice.
                lv_async_call([](void *) { tool_page_reshow(); }, NULL);
            }, (void *)(intptr_t)i);
            has_data = true;
        }
        // The "+ Add Network" button is built ABOVE list_area (see the top of this function),
        // so it can never be pushed below the fold by a growing list.
        has_data = true;  // the Add button above is always available
    }
    if (!has_data)
        make_label(list_area, "No data - run a scan first", &ui_font_pipboy_14, pip_dim());
}

// Build right pane for TAT_CHANNEL: Description + channel dropdown
static void show_tool_channel(const ToolItem *wi) {
    lv_obj_t *desc = make_label(right_pane, wi->desc,
                                &ui_font_pipboy_16, pip_primary());
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(desc, lv_pct(100));

    make_label(right_pane, "Channel", &ui_font_pipboy_14, pip_dim());
    lv_obj_t *dd = make_dropdown(right_pane, "Auto\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14");

    // Set current channel
    int ch = cb.getChannel();
    lv_dropdown_set_selected(dd, (ch <= 0) ? 0 : (uint16_t)ch);
    lv_obj_add_event_cb(dd, cb_channel_dd_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

// Dispatch to the correct right pane builder based on ToolActionType
// State for the expand/collapse: the toggle button is pair-referenced by the
// bottom "close" button so tapping either one does the same thing.
struct ToolInfoRefs {
    lv_obj_t *scroll_container;  // right_pane
    lv_obj_t *top_button;         // top "? More Info" / "? Less Info"
    lv_obj_t *panel;              // the bordered container
};

// Shared toggle handler - works for both the top button and the close button
// inside the panel. user_data is a ToolInfoRefs* bound to the pair.
static void tool_info_toggle_cb(lv_event_t *e) {
    ToolInfoRefs *refs = (ToolInfoRefs *)lv_event_get_user_data(e);
    if (!refs || !refs->panel || !refs->top_button) return;
    bool hidden = lv_obj_has_flag(refs->panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *top_lbl = lv_obj_get_child(refs->top_button, 0);
    if (hidden) {
        lv_obj_remove_flag(refs->panel, LV_OBJ_FLAG_HIDDEN);
        if (top_lbl) lv_label_set_text(top_lbl, "? Less Info");
        // Scroll so the top button sits at the very top of the visible area,
        // with the info panel flowing below it. Minimal scroll_to_view lands
        // us in the middle; scroll-to-y with the button's own Y puts it flush.
        lv_obj_update_layout(refs->scroll_container);
        int32_t btn_y = lv_obj_get_y(refs->top_button);
        lv_obj_scroll_to_y(refs->scroll_container, btn_y, LV_ANIM_ON);
    } else {
        lv_obj_add_flag(refs->panel, LV_OBJ_FLAG_HIDDEN);
        if (top_lbl) lv_label_set_text(top_lbl, "? More Info");
        // Scroll back up so the user lands somewhere sensible instead of
        // staring at an empty bottom of the pane.
        lv_obj_scroll_to_y(refs->scroll_container, 0, LV_ANIM_ON);
    }
}

// Append a collapsible "More Info" section to a tool's detail pane.
// Renders a top button, a hidden bordered panel with subsections, and a
// second close button inside the panel so the user doesn't have to scroll
// all the way back up to collapse.
static void append_tool_info_section(lv_obj_t *parent, uint8_t cat, uint8_t item) {
    if (!parent) return;
    const ToolInfo *info = tool_info_lookup(
        cat < NUM_TOOL_CATS ? tool_categories[cat].id : cat, item);

    // Allocate refs - LVGL owns it for the lifetime of the objects; freed
    // via the delete event on the top button (one owner).
    ToolInfoRefs *refs = (ToolInfoRefs *)lv_malloc(sizeof(ToolInfoRefs));
    if (!refs) return;
    refs->scroll_container = parent;

    // Top button - flex order index N
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_add_style(btn, &style_list_btn, 0);
    lv_obj_add_style(btn, &style_list_btn_pressed, LV_STATE_PRESSED);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_set_height(btn, LV_SIZE_CONTENT);
    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "? More Info");
    lv_obj_set_style_text_font(btn_lbl, &ui_font_pipboy_14, 0);
    lv_obj_set_style_text_color(btn_lbl, pip_primary(), 0);
    refs->top_button = btn;

    // Free the refs struct when the button is destroyed
    lv_obj_add_event_cb(btn, [](lv_event_t *e){
        ToolInfoRefs *r = (ToolInfoRefs *)lv_event_get_user_data(e);
        if (r) lv_free(r);
    }, LV_EVENT_DELETE, refs);
    lv_obj_add_event_cb(btn, tool_info_toggle_cb, LV_EVENT_CLICKED, refs);

    // Panel - flex order index N+1
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_set_width(panel, lv_pct(100));
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(panel, 4, 0);
    lv_obj_set_style_pad_gap(panel, 4, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, pip_border(), 0);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
    refs->panel = panel;

    // Splits `body` on '\n' and renders each segment as its own label so
    // paragraph breaks always land where the author intended, independent
    // of LVGL's wrap-mode whitespace handling.
    auto add_section = [&](const char *heading, const char *body) {
        if (!body) return;
        lv_obj_t *h = make_label(panel, heading, &ui_font_pipboy_14, pip_highlight());
        lv_obj_set_width(h, lv_pct(100));
        const char *seg = body;
        while (seg && *seg) {
            const char *nl = strchr(seg, '\n');
            size_t len = nl ? (size_t)(nl - seg) : strlen(seg);
            if (len > 0) {
                char *chunk = (char *)lv_malloc(len + 1);
                if (chunk) {
                    memcpy(chunk, seg, len);
                    chunk[len] = '\0';
                    lv_obj_t *b = make_label(panel, chunk, &ui_font_pipboy_14, pip_primary());
                    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
                    lv_obj_set_width(b, lv_pct(100));
                    lv_free(chunk);
                }
            }
            if (!nl) break;
            seg = nl + 1;
        }
    };

    if (info) {
        // USE RESPONSIBLY first: warnings belong BEFORE the spell, not after.
        // Tools with no avoid guidance (NULL) just skip this section.
        add_section("USE RESPONSIBLY",   info->avoid);
        add_section("WHAT IT DOES",      info->what);
        add_section("REQUIRES",           info->requires);
        add_section("WHAT YOU'LL SEE",    info->effects);
        // KNOWN BEHAVIOUR, rendered by RULE rather than copied into each entry's prose.
        // Every tool that shows or targets from the shared AP/station lists can display a
        // momentarily incomplete row, or skip a selected device for one pass, while a scan is
        // still adding results. Emitting it from the action type means it cannot drift out of
        // sync across ~15 entries and it automatically covers tools added later.
        // Facts only: what the user may observe, no mechanism, no attribution.
        // ⚠ Coverage was wrong on first attempt (caught by adversarial review): the TAT_LIST_VIEW
        // arm was dead TWICE OVER -- the caller skips this whole function for that type
        // (`if (wi->type != TAT_LIST_VIEW)`), and tool_info.h has no entries for the Utilities
        // list items so the enclosing `if (info)` would not fire either. Meanwhile the plain
        // Scan tools are TAT_SIMPLE and were excluded, yet they are exactly what the README and
        // Help text are ABOUT ("reading the list closely while a scan runs"). Net effect: the
        // note appeared on 3 tools, not the ~15 it applies to, and none of them were the ones
        // the other two layers describe.
        // Keyed on the CATEGORY as well now: every tool in Detect/Scan/Monitor/Analyze reads or
        // fills the shared AP/STA store, plus any AP/STA-targeted tool anywhere.
        if (cat < NUM_TOOL_CATS && item < tool_categories[cat].count) {
            ToolActionType _t  = tool_categories[cat].items[item].type;
            uint8_t        _id = tool_categories[cat].id;
            bool reads_shared_lists = (_t == TAT_AP || _t == TAT_STA) ||
                                      (_id == 0 || _id == 1 || _id == 2 || _id == 3);
            if (reads_shared_lists) {
                add_section("KNOWN BEHAVIOUR", TOOL_KNOWN_LIST_NOTE);
                // Same rule, second fact: the lists are cumulative ("seen since START"), not a
                // live census. Correct for a survey tool, but a list that only grows does not
                // announce that about itself -- so the tool says it.
                add_section("ABOUT THESE LISTS", TOOL_CUMULATIVE_LIST_NOTE);
            }
        }
    } else {
        lv_obj_t *tbd = make_label(panel,
            "Detailed description coming before final release. "
            "In the meantime, see the short label above.",
            &ui_font_pipboy_14, pip_dim());
        lv_label_set_long_mode(tbd, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(tbd, lv_pct(100));
    }

    // Close button at the bottom of the panel so the user can collapse
    // without scrolling back to the top.
    lv_obj_t *close_btn = lv_button_create(panel);
    lv_obj_remove_style_all(close_btn);
    lv_obj_add_style(close_btn, &style_list_btn, 0);
    lv_obj_add_style(close_btn, &style_list_btn_pressed, LV_STATE_PRESSED);
    lv_obj_set_width(close_btn, lv_pct(100));
    lv_obj_set_height(close_btn, LV_SIZE_CONTENT);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, "^ Close Info");
    lv_obj_set_style_text_font(close_lbl, &ui_font_pipboy_14, 0);
    lv_obj_set_style_text_color(close_lbl, pip_primary(), 0);
    lv_obj_add_event_cb(close_btn, tool_info_toggle_cb, LV_EVENT_CLICKED, refs);
}

static void show_tool_detail(uint8_t cat_idx, uint8_t item_idx) {
    if (!right_pane) return;
    if (cat_idx >= NUM_TOOL_CATS) return;
    const ToolCategory *cat = &tool_categories[cat_idx];
    if (item_idx >= cat->count) return;
    const ToolItem *wi = &cat->items[item_idx];
    int32_t encoded = tool_encode(cat_idx, item_idx);
    // Remember the page we are drawing, so any handler on it can redraw THIS page rather than
    // calling rebuild_content() and landing on whatever cur_sel happens to point at. Set here
    // (one place) instead of per-page, so tool pages added later inherit it.
    tool_page_return_enc = encoded;

    // Clear all UI refs that timers might write to - they become dangling
    // after clear_children destroys the right pane's widgets
    cb_ap_list_area = NULL;
    cb_sta_list_area = NULL;
    cb_output_scroll = NULL;
    cb_output_log = NULL;
    wifi_status_label = NULL;
    wifi_scan_btn_label = NULL;
    wifi_ssid_label = NULL;
    wifi_pw_label = NULL;

    clear_children(right_pane);
    lv_obj_scroll_to_y(right_pane, 0, LV_ANIM_OFF);
    make_label(right_pane, wi->name, &ui_font_pipboy_18, pip_highlight());

    switch (wi->type) {
        case TAT_SIMPLE:    show_tool_simple(wi, encoded);              break;
        case TAT_AP:        show_tool_ap(wi, encoded);                  break;
        case TAT_STA:       show_tool_sta(wi, encoded);                 break;
        case TAT_SSID:      show_tool_ssid(wi, encoded);                break;
        case TAT_TEXT:       show_tool_text(wi, encoded);                break;
        case TAT_FILE:       show_tool_file(wi, encoded);                break;
        case TAT_IMMEDIATE: show_tool_immediate(wi, encoded);           break;
        case TAT_LIST_VIEW: show_tool_list_view(wi, cat_idx, item_idx); break;
        case TAT_CHANNEL:   show_tool_channel(wi);                      break;
    }

    // Resume: if this tool is currently running, recreate output area
    // and show persisted log buffer contents
    if (cb_op_running && cb_op_encoded == encoded) {
        if (right_pane && !cb_output_scroll) {
            cb_create_output_area(right_pane);
        }
    }

    // Append the collapsible "More Info" section at the end of the right
    // pane for every tool type. List-view utilities skip it - they don't
    // run anything, so a detailed description would be noise.
    if (wi->type != TAT_LIST_VIEW) {
        append_tool_info_section(right_pane, cat_idx, item_idx);
    }
}

// ─────────────────────── TOOLS: GROUPED LIST + TAP ──────────────────────

// Modal: airplane mode is on and the user tapped a radio-using tool.
// "Turn Off" disables airplane mode and dismisses; user re-taps the tool.
// radio_name = "WiFi" or "Bluetooth" -- the body text names the actual radio
// the tool would have engaged so the user knows what's about to come back on.
static void show_airplane_block_dialog(const char *radio_name) {
    lv_obj_t *modal = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(modal);
    lv_obj_set_size(modal, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_80, 0);
    lv_obj_remove_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(modal, 0, 0);

    lv_obj_t *box = lv_obj_create(modal);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 280, 150);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, pip_bg(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, pip_highlight(), 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_pad_all(box, 12, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(box, 8, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    make_label(box, "AIRPLANE MODE", &ui_font_pipboy_18, pip_highlight());

    char body_buf[96];
    snprintf(body_buf, sizeof(body_buf),
             "This tool needs %s.\nTurn off airplane mode?",
             radio_name ? radio_name : "WiFi or Bluetooth");
    lv_obj_t *body = make_label(box, body_buf, &ui_font_pipboy_14, pip_primary());
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *row = lv_obj_create(box);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *btn_cancel = lv_button_create(row);
    lv_obj_set_size(btn_cancel, 110, 30);
    lv_obj_set_style_bg_color(btn_cancel, pip_border(), 0);
    lv_obj_t *lc = lv_label_create(btn_cancel);
    lv_label_set_text(lc, "Cancel");
    lv_obj_set_style_text_font(lc, &ui_font_pipboy_14, 0);
    lv_obj_set_style_text_color(lc, pip_primary(), 0);
    lv_obj_center(lc);
    lv_obj_add_event_cb(btn_cancel, [](lv_event_t *e2) {
        lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e2);
        lv_obj_t *m = lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(btn)));
        lv_obj_delete(m);
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_off = lv_button_create(row);
    lv_obj_set_size(btn_off, 110, 30);
    lv_obj_set_style_bg_color(btn_off, pip_highlight(), 0);
    lv_obj_t *lo = lv_label_create(btn_off);
    lv_label_set_text(lo, "Turn Off");
    lv_obj_set_style_text_font(lo, &ui_font_pipboy_14, 0);
    lv_obj_set_style_text_color(lo, pip_bg(), 0);
    lv_obj_center(lo);
    lv_obj_add_event_cb(btn_off, [](lv_event_t *e2) {
        lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e2);
        lv_obj_t *m = lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(btn)));
        lv_obj_delete(m);
        // Route through the shared applier, like the Settings switch and the harness. This was
        // a THIRD raw writer of cfg.airplane; airplane_apply(false) currently only logs, so the
        // behaviour is identical today -- but the next time turning airplane OFF needs to do
        // something (re-arm a radio, refresh an indicator), this path would have silently
        // skipped it. Same reason th_cmd_cfg_set was routed through it.
        airplane_apply(false);
        CB_LOGLN("[CFG] Airplane OFF (via tool dialog)");
    }, LV_EVENT_CLICKED, NULL);
}

// ── Is this tool allowed to launch right now? ───────────────────────────────
// Hoisted out of tool_tap_cb so the TEST HARNESS asks the same question the UI does.
// tool_start / tool_open dispatch directly and therefore bypassed this gate entirely: a
// scripted `cfg_set airplane true` followed by `tool_start` would start a radio tool and reach
// esp_wifi_start() / esp_wifi_set_promiscuous(true) with the Airplane switch reading engaged.
// That is F7 one layer up -- and it meant the tool-side airplane block had ZERO automated
// coverage, so a regression in it could not be caught, and a test written against tool_start
// would report a failure the product does not have.
// Returns false when the launch must be refused; the caller decides how to say so (the UI shows
// a dialog, the harness returns an error a test can assert on).
static bool tool_launch_allowed(uint8_t ci, uint8_t ii) {
    if (ci >= NUM_TOOL_CATS || ii >= tool_categories[ci].count) return false;
    const ToolItem *wi = &tool_categories[ci].items[ii];
    if (cfg.airplane && tool_needs_radio(wi)) {
        CB_LOGF("[TOOL] Blocked by airplane mode: %s > %s\n",
                tool_categories[ci].name, wi->name);
        return false;
    }
    return true;
}

static void tool_tap_cb(lv_event_t *e) {
    int32_t encoded = (int32_t)(intptr_t)lv_event_get_user_data(e);
    uint8_t ci = tool_cat(encoded);
    uint8_t ii = tool_item(encoded);
    const ToolItem *wi = &tool_categories[ci].items[ii];

    // Block radio-using tools while airplane mode is on. User confirms in the
    // dialog whether to turn airplane off; on Yes they re-tap to actually launch.
    // The predicate lives in tool_launch_allowed() so the harness asks the same question.
    if (!tool_launch_allowed(ci, ii)) {
        show_airplane_block_dialog(tool_radio_name(wi));
        return;
    }

    // We can't easily highlight across categories with the simple index scheme,
    // so just show the detail
    show_tool_detail(ci, ii);

    CB_LOGF("[TOOL] Selected: %s > %s\n",
                  tool_categories[ci].name, wi->name);
}

// ─── Collapsible category toggle callback ─────────────────────────────────
static void tool_cat_toggle_cb(lv_event_t *e) {
    uint8_t c = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    tool_cats_expanded ^= (1 << c);
    bool expanded = tool_cats_expanded & (1 << c);

    // Update header prefix
    lv_obj_t *hdr = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *hdr_lbl = lv_obj_get_child(hdr, 0);
    if (hdr_lbl) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%s %s", expanded ? "v" : ">", tool_categories[c].name);
        lv_label_set_text(hdr_lbl, buf);
    }

    // Toggle visibility of the items container (next sibling after header)
    int32_t hdr_idx = lv_obj_get_index(hdr);
    lv_obj_t *parent = lv_obj_get_parent(hdr);
    lv_obj_t *items_cont = lv_obj_get_child(parent, hdr_idx + 1);
    if (items_cont) {
        if (expanded)
            lv_obj_remove_flag(items_cont, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(items_cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_update_layout(parent);
        if (expanded) {
            // Bring the EXPANDED HEADER to the top of the pane, so the category you just opened
            // and its items are what you see. Without this, expanding the third visible category
            // leaves the two collapsed ones above it occupying the pane and the items you asked
            // for pushed to the bottom edge -- it works, it just reads as unfinished. Owner
            // request (2026-07-26).
            // Clamped by LVGL: scrolling beyond the content bottom is not possible, so the last
            // category simply lands as high as it can rather than leaving blank space.
            int32_t target = lv_obj_get_y(hdr) - lv_obj_get_style_pad_top(parent, 0);
            if (target < 0) target = 0;
            lv_obj_scroll_to_y(parent, target, LV_ANIM_ON);
        } else {
            // Remove gap at bottom after collapse: clamp scroll position
            int32_t scroll_y = lv_obj_get_scroll_y(parent);
            int32_t max_y    = lv_obj_get_scroll_bottom(parent);
            if (scroll_y > 0 && max_y < 0) {
                // Scrolled past content - snap back
                lv_obj_scroll_to_y(parent, scroll_y + max_y, LV_ANIM_ON);
            }
        }
    }
}

static void build_items_tools(lv_obj_t *cont) {
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_set_style_pad_gap(cont, 0, 0);

    tool_cats_expanded = 0x0000;  // Reset on rebuild
    left_pane = create_left_list(cont);

    // Layer 2: Passive security notice at top of tools list. First clause in
    // primary color so it doesn't read like skippable EULA fine print; the
    // longer-form responsibility clause stays dim.
    // One compact wrapped line (was two stacked labels - merged to drop the
    // inter-label gap + short-first-line empty space that looked oversized on
    // the 320x240 panel). Kept readable (not dim fine print) - it's the notice.
    lv_obj_t *notice = make_label(left_pane,
#ifdef CLIPBOY_RES34RCH
        "Active TX tools - authorized targets ONLY. You are responsible for lawful use.",
#else
        "Authorized use only - you are responsible for lawful use in your jurisdiction.",
#endif
        &ui_font_pipboy_14, pip_primary());
    lv_label_set_long_mode(notice, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(notice, lv_pct(100));
    lv_obj_set_style_pad_hor(notice, 4, 0);
    lv_obj_set_style_pad_ver(notice, 4, 0);

    // Build grouped list with collapsible category headers
    for (uint8_t c = 0; c < NUM_TOOL_CATS; c++) {
        const ToolCategory *cat = &tool_categories[c];

        // Category header - clickable, with > prefix
        lv_obj_t *hdr = lv_obj_create(left_pane);
        lv_obj_remove_style_all(hdr);
        lv_obj_add_style(hdr, &style_category_header, 0);
        lv_obj_add_style(hdr, &style_list_btn_pressed, LV_STATE_PRESSED);
        lv_obj_set_size(hdr, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_add_flag(hdr, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *hdr_lbl = lv_label_create(hdr);
        char hdr_buf[48];
        snprintf(hdr_buf, sizeof(hdr_buf), "> %s", cat->name);
        lv_label_set_text(hdr_lbl, hdr_buf);
        lv_obj_add_event_cb(hdr, tool_cat_toggle_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)c);

        // Items container - lv_list so make_list_btn works, hidden by default
        lv_obj_t *items_cont = lv_list_create(left_pane);
        lv_obj_remove_style_all(items_cont);
        lv_obj_set_size(items_cont, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(items_cont, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(items_cont, 0, 0);
        lv_obj_set_style_pad_gap(items_cont, 0, 0);
        lv_obj_remove_flag(items_cont, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(items_cont, LV_OBJ_FLAG_HIDDEN);

        for (uint8_t i = 0; i < cat->count; i++) {
            int32_t encoded = tool_encode(c, i);
            lv_obj_t *btn = make_list_btn(items_cont, cat->items[i].name,
                                          tool_tap_cb, (void *)(intptr_t)encoded);
            lv_obj_set_style_pad_left(btn, 8, 0);
        }
    }

    right_pane = create_right_detail(cont);

    // If a tool is running, auto-select it and expand its category
    if (cb_op_running && cb_op_encoded >= 0) {
        uint8_t rc = tool_cat(cb_op_encoded);
        uint8_t ri = tool_item(cb_op_encoded);
        // Expand the running tool's category
        if (rc < NUM_TOOL_CATS) {
            tool_cats_expanded |= (1 << rc);
            // Update header text and show items container. left_pane layout is:
            // [0]=ONE merged notice label, then per category [hdr, items_cont].
            // (The notice was two stacked labels once; this index was stale at
            // "2 + rc*2" and landed on the items_cont -> lv_label_set_text on a
            // non-label freed a garbage pointer = the heap-assert crash when you
            // navigated back to a running scan.)
            int hdr_idx = 1 + rc * 2;  // skip the single notice label
            int items_idx = hdr_idx + 1;
            if (items_idx < (int)lv_obj_get_child_count(left_pane)) {
                lv_obj_t *hdr = lv_obj_get_child(left_pane, hdr_idx);
                lv_obj_t *hdr_lbl = hdr ? lv_obj_get_child(hdr, 0) : NULL;
                // Type-guard: never set_text on something that isn't a label
                // (a wrong index would otherwise free a non-label's bytes).
                if (hdr_lbl && lv_obj_check_type(hdr_lbl, &lv_label_class)) {
                    char buf[48];
                    snprintf(buf, sizeof(buf), "v %s", tool_categories[rc].name);
                    lv_label_set_text(hdr_lbl, buf);
                }
                lv_obj_t *items_c = lv_obj_get_child(left_pane, items_idx);
                if (items_c) lv_obj_remove_flag(items_c, LV_OBJ_FLAG_HIDDEN);
            }
        }
        show_tool_detail(rc, ri);
    } else if (cfg.airplane) {
        // Auto-showing a radio tool while airplane is on would let the user hit
        // Start with no feedback (cb_ensure_wifi silently no-ops). Show a notice
        // instead so the gate is visible, not silent.
        make_label(right_pane, "Airplane Mode On",
                   &ui_font_pipboy_18, pip_highlight());
        lv_obj_t *desc = make_label(right_pane,
            "Tap a tool to use it. Tools that need WiFi or Bluetooth "
            "will ask before turning airplane mode off.",
            &ui_font_pipboy_14, pip_primary());
        lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(desc, lv_pct(100));
    } else {
        show_tool_detail(0, 0);
    }
}

// ─────────────────────── SPLIT-PANE (non-tool lists) ────────────────────

static void show_item_detail(const ListItem *items, int count, int idx) {
    if (!right_pane || idx < 0 || idx >= count) return;
    clear_children(right_pane);
    lv_obj_scroll_to_y(right_pane, 0, LV_ANIM_OFF);
    make_label(right_pane, items[idx].name,
               &ui_font_pipboy_18, pip_highlight());

    lv_obj_t *desc = make_label(right_pane, items[idx].desc,
                                &ui_font_pipboy_16, pip_primary());
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(desc, lv_pct(100));
}

// Show collectible detail with image, tier, description and stat modifiers
// ─── Collectible card rotate-by-drag ─────────────────────────────────────
// Static state. show_collectible_detail rebuilds these slots on each
// card; only one card is active at a time, so static is fine.
static lv_obj_t                *coll_rot_card = nullptr;
static lv_obj_t                *coll_rot_img  = nullptr;
static const lv_image_dsc_t    *coll_rot_front_src = nullptr;
static const lv_image_dsc_t    *coll_rot_back_src  = nullptr;
static uint8_t                  coll_rot_id        = 0;   // active collectible (for color-fullscreen lookup)
static int    coll_rot_press_x        = 0;
static int    coll_rot_press_y        = 0;
static float  coll_rot_angle          = 0.0f;
static bool   coll_rot_press_active   = false;
static bool   coll_rot_showing_back   = false;
static bool   coll_rot_gesture_owned  = false;

// Image scales (LVGL 256 = 1x). Collectible art is now 200x200 (was 80x80),
// so the on-screen size is driven by the source's own header rather than a
// fixed constant -- this keeps the layout stable regardless of source res
// (built-in 200px, or an SD-card override of any square size).
//   coll_fit_scale(src, target_px) -> LVGL scale that renders src at target_px.
// Front art renders ~80 px in the 100 px card; the mascot back is 153x192.
static inline int coll_fit_scale(const lv_image_dsc_t *src, int target_px) {
    int w = (src && src->header.w) ? (int)src->header.w : 80;
    int s = (target_px * 256) / w;
    return s < 1 ? 1 : s;
}
static constexpr int kCollRotFrontPx    = 80;      // on-screen front-art size
static constexpr int kCollRotBackScale  = 107;     // mascot 80/192*256 ≈ 107
static constexpr int kCollRotMinScale   = 4;       // never fully zero -- keeps the seam visible

// Apply a rotation angle to the active card: swap front/back at ±90°,
// modulate scale_x by |cos(angle)|. Scaling the WHOLE CARD (frame +
// image) via transform_scale_x sells the 3D illusion -- if only the
// image scaled, the frame would stay 100x100 and the rotation would
// look like a window-blind effect instead of a turning card.
static void coll_rot_apply(float angle_deg) {
    if (!coll_rot_card || !coll_rot_img) return;
    while (angle_deg > 180.0f)  angle_deg -= 360.0f;
    while (angle_deg < -180.0f) angle_deg += 360.0f;
    coll_rot_angle = angle_deg;

    float angle_rad = angle_deg * 0.0174533f;
    float cos_a = cosf(angle_rad);
    bool show_back = (fabsf(angle_deg) > 90.0f);

    if (show_back != coll_rot_showing_back) {
        const lv_image_dsc_t *src = show_back ? coll_rot_back_src : coll_rot_front_src;
        if (src) lv_image_set_src(coll_rot_img, src);
        // Set the BASE intrinsic scale of the image so it fits the card.
        // The card's transform_scale_x then modulates this for rotation.
        lv_image_set_scale(coll_rot_img,
                           show_back ? kCollRotBackScale
                                     : coll_fit_scale(src, kCollRotFrontPx));
        coll_rot_showing_back = show_back;
    }
    // 256 = unity (1.0x) for transform_scale_x.
    int sx = (int)(256.0f * fabsf(cos_a));
    if (sx < kCollRotMinScale) sx = kCollRotMinScale;
    lv_obj_set_style_transform_scale_x(coll_rot_card, sx, 0);
}

static void coll_rot_anim_exec(void *var, int32_t v) {
    (void)var;
    coll_rot_apply((float)v / 100.0f);
}

static void coll_rot_press_cb(lv_event_t *e) {
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    coll_rot_press_x = p.x;
    coll_rot_press_y = p.y;
    coll_rot_press_active  = true;
    coll_rot_gesture_owned = false;  // not yet committed to rotating vs scrolling
    // Cancel any in-flight snap-back animation on the card
    lv_anim_delete(NULL, coll_rot_anim_exec);
}

static void coll_rot_pressing_cb(lv_event_t *e) {
    (void)e;
    if (!coll_rot_press_active) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int dx = p.x - coll_rot_press_x;
    int dy = p.y - coll_rot_press_y;

    // Only "claim" the gesture once the user has clearly committed to a
    // horizontal drag. Below the threshold, ignore so the right-pane's
    // vertical scroll still wins for downward swipes.
    if (!coll_rot_gesture_owned) {
        if (abs(dx) < 8) return;            // not enough movement yet
        if (abs(dy) > abs(dx)) {            // vertical drag -- yield to scroll
            coll_rot_press_active = false;
            return;
        }
        coll_rot_gesture_owned = true;
    }
    // 1 deg per px gives a comfortable feel: ~90 px drag = full edge-on.
    coll_rot_apply((float)dx);
}

// Fullscreen view of the collectible image. Any touch dismisses.
// Target on-screen height in px; coll_fit_scale converts to an LVGL scale
// based on the source resolution. 232 nearly fills the 240 px screen with
// a small margin. With 200 px source art this is a ~1.16x upscale (was 3x
// off the old 80 px art) -- far crisper.
#define COLL_FULLSCREEN_PX 232

static lv_obj_t *coll_fullscreen_overlay = nullptr;
static bool      coll_fs_audio_on = false;   // "Summon the Data" playing under the id-75 fullscreen

static void coll_fullscreen_dismiss_cb(lv_event_t *e) {
    (void)e;
    if (coll_fs_audio_on) { audio_mp3_stream_stop(); coll_fs_audio_on = false; }
    if (coll_fullscreen_overlay) {
        lv_obj_delete(coll_fullscreen_overlay);
        coll_fullscreen_overlay = nullptr;
    }
}

// Fires when the overlay is deleted by ANY path -- touch dismiss OR scr_main being
// rebuilt on a theme switch (e.g. the ARG P5 THEME_QUANTA flip). Stops the looping
// Summon stream + nulls the global so the fullscreen feature can't soft-lock and
// the audio can't leak across screens. (reboot-UAF review finding #3)
static void coll_fs_delete_cb(lv_event_t *e) {
    (void)e;
    if (coll_fs_audio_on) { audio_mp3_stream_stop(); coll_fs_audio_on = false; }
    coll_fullscreen_overlay = nullptr;
}

static void coll_show_fullscreen_image() {
    if (coll_fullscreen_overlay) return;            // already up
    if (!coll_rot_front_src) return;                // nothing to show

    coll_fullscreen_overlay = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(coll_fullscreen_overlay);
    lv_obj_set_size(coll_fullscreen_overlay, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(coll_fullscreen_overlay, 0, 0);
    lv_obj_set_style_bg_color(coll_fullscreen_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(coll_fullscreen_overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(coll_fullscreen_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(coll_fullscreen_overlay, coll_fullscreen_dismiss_cb,
                        LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(coll_fullscreen_overlay, coll_fs_delete_cb,
                        LV_EVENT_DELETE, NULL);   // stop Summon + null on any teardown

    // If this collectible has a full-color (RGB565) image, show it here in
    // its true colors -- no theme recolor. The thumbnail + unlock modal still
    // used the A8 version, so the color only appears on the fullscreen reveal.
    const lv_image_dsc_t *color = coll_get_color_image(coll_rot_id);
    const lv_image_dsc_t *src   = color ? color : coll_rot_front_src;

    lv_obj_t *img = lv_image_create(coll_fullscreen_overlay);
    lv_image_set_src(img, src);
    if (!color) {
        lv_obj_set_style_image_recolor(img, pip_primary(), 0);
        lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    }
    lv_image_set_scale(img, coll_fit_scale(src, COLL_FULLSCREEN_PX));
    lv_obj_center(img);

    // Collectible 75 (SheetmetalCon ticket): score the reveal with "Summon the
    // Data" on a loop until the image is dismissed.
    if (coll_rot_id == 75) {
        for (int i = 0; i < g_radio_clip_count; i++)
            if (!strcmp(g_radio_clips[i].name, "Summon_the_Data")) {
                const RadioClip *c = &g_radio_clips[i];
                // Summon now rides littlefs (data==NULL) to shrink the app; fall back
                // to PROGMEM if it's still embedded. Missing littlefs clip = silent
                // reveal (never a crash), same graceful policy as the radio player.
                if (c->data) {
                    audio_mp3_stream_play_progmem(c->data, c->len, true);
                    coll_fs_audio_on = true;
                } else if (c->path && LittleFS.exists(c->path)) {
                    audio_mp3_stream_play_file(LittleFS, c->path, true);
                    coll_fs_audio_on = true;
                }
                break;
            }
    }
}

static void coll_rot_release_cb(lv_event_t *e) {
    (void)e;
    if (!coll_rot_press_active) return;
    bool was_tap = !coll_rot_gesture_owned;
    coll_rot_press_active  = false;
    coll_rot_gesture_owned = false;

    if (was_tap) {
        audio_play_click();  // genuine tap (not a swipe) -> tactile feedback
        // Touch with no significant drag -> show fullscreen image.
        // Snap any in-flight rotation back to 0 first.
        coll_rot_apply(0.0f);
        coll_show_fullscreen_image();
        return;
    }

    // Snap back to angle=0 over 250 ms with an ease-out path.
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, NULL);
    lv_anim_set_values(&a, (int32_t)(coll_rot_angle * 100.0f), 0);
    lv_anim_set_duration(&a, 250);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, coll_rot_anim_exec);
    lv_anim_start(&a);
}

// Cancel any in-flight snap-back anim and null the card refs. The anim is started
// with var=NULL, so LVGL's on-delete cancel (var==deleted-obj) never catches it --
// both screen teardown paths (rebuild_content + ui_theme_switch_live) must call this
// or the 250ms anim keeps firing coll_rot_apply() on a freed card. (reboot-UAF review)
static void coll_rot_reset(void) {
    lv_anim_delete(NULL, coll_rot_anim_exec);
    coll_rot_card = nullptr;
    coll_rot_img  = nullptr;
}

static void show_collectible_detail(int idx) {
    if (!right_pane || idx < 0 || idx >= (int)coll_count) return;
    clear_children(right_pane);
    lv_obj_scroll_to_y(right_pane, 0, LV_ANIM_OFF);

    // Wipe any rotate-card refs before rebuild so callbacks can't touch
    // a freed object if a snap-back anim was mid-flight.
    coll_rot_reset();
    coll_rot_press_active  = false;
    coll_rot_gesture_owned = false;
    coll_rot_showing_back  = false;

    const Collectible &c = coll_items[idx];

    if (!c.collected) {
        // ── LOCKED ──
        lv_obj_t *lt = make_label(right_pane, "??? Locked", &ui_font_pipboy_18, pip_dim());
        lv_obj_set_width(lt, lv_pct(100));
        lv_obj_set_style_text_align(lt, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_t *ls = make_label(right_pane, "[LOCKED]", &ui_font_pipboy_16, pip_dim());
        lv_obj_set_width(ls, lv_pct(100));
        lv_obj_set_style_text_align(ls, LV_TEXT_ALIGN_CENTER, 0);

        // Tier only (no source), centered
        char rtier[64];
        snprintf(rtier, sizeof(rtier), "Rarity: %s", coll_tier_name(c.tier));
        lv_obj_t *rt = make_label(right_pane, rtier, &ui_font_pipboy_14,
                   c.tier >= 2 ? pip_highlight() : pip_dim());
        lv_obj_set_width(rt, lv_pct(100));
        lv_obj_set_style_text_align(rt, LV_TEXT_ALIGN_CENTER, 0);

        lv_obj_t *hint = make_label(right_pane,
            "Scan an HR Code to unlock this collectible.",
            &ui_font_pipboy_14, pip_dim());
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(hint, lv_pct(100));
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }

    // ── UNLOCKED ──
    // Title: insert \n before parenthetical if present
    char title_buf[COLL_MAX_TITLE + 2];
    const char *paren = strchr(c.title, '(');
    if (paren && paren > c.title) {
        size_t pre = paren - c.title;
        // Trim trailing space before '('
        while (pre > 0 && c.title[pre - 1] == ' ') pre--;
        snprintf(title_buf, sizeof(title_buf), "%.*s\n%s", (int)pre, c.title, paren);
    } else {
        strncpy(title_buf, c.title, sizeof(title_buf));
        title_buf[sizeof(title_buf) - 1] = '\0';
    }
    // Title (highlight color, full size). Explicit SIZE_CONTENT height so
    // long titles ('Dark Tangent's Fabled Conference Badge') flex vertically
    // instead of getting clipped at an implicit default height.
    lv_obj_t *title = lv_label_create(right_pane);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_size(title, lv_pct(100), LV_SIZE_CONTENT);
    lv_label_set_text(title, title_buf);
    lv_obj_set_style_text_font(title, &ui_font_pipboy_18, 0);
    lv_obj_set_style_text_color(title, pip_highlight(), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    // IP / source line directly under the title -- same highlight color
    // but dimmer (70% text opa) so it reads as a subtitle.
    lv_obj_t *src_lbl = lv_label_create(right_pane);
    lv_label_set_long_mode(src_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_size(src_lbl, lv_pct(100), LV_SIZE_CONTENT);
    lv_label_set_text(src_lbl, c.source);
    lv_obj_set_style_text_font(src_lbl, &ui_font_pipboy_14, 0);
    lv_obj_set_style_text_color(src_lbl, pip_highlight(), 0);
    lv_obj_set_style_text_opa(src_lbl, LV_OPA_70, 0);
    lv_obj_set_style_text_align(src_lbl, LV_TEXT_ALIGN_CENTER, 0);

    // Themed image (A8 alpha mask) - SD > LittleFS > compiled-in
    const lv_image_dsc_t *img_src = coll_load_image(c.id);
    if (!img_src) img_src = coll_get_builtin_image(c.id);

    // Card-style wrapper with a Pip-Boy themed border. Drag horizontally
    // to "rotate" the card -- scale_x animates with cos(angle) so the
    // image foreshortens through zero, then the source swaps to the
    // Clip-Boy mascot for a "back of the card" peek. Releases snap back.
    lv_obj_t *img_wrap = lv_obj_create(right_pane);
    lv_obj_remove_style_all(img_wrap);
    lv_obj_set_size(img_wrap, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(img_wrap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(img_wrap, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *card = lv_obj_create(img_wrap);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 100, 100);  // 80 image + 10 frame each side
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, pip_primary(), 0);
    lv_obj_set_style_border_opa(card, LV_OPA_80, 0);
    lv_obj_set_style_radius(card, 4, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    // Transform pivot at the card's horizontal center so scale_x
    // shrinks/grows symmetrically around the middle (not the left edge).
    lv_obj_set_style_transform_pivot_x(card, 50, 0);
    lv_obj_set_style_transform_pivot_y(card, 50, 0);

    lv_obj_t *img = lv_image_create(card);
    if (img_src) lv_image_set_src(img, img_src);
    // Render the (200 px) source down to ~80 px so it fits the 100 px card.
    // coll_rot_apply skips the scale-set on its first call (front/back state
    // unchanged), so the base scale must be established here.
    if (img_src) lv_image_set_scale(img, coll_fit_scale(img_src, kCollRotFrontPx));
    lv_obj_set_style_image_recolor(img, pip_primary(), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_center(img);

    // Stash refs + sources in module-static slots used by the gesture
    // callbacks. Detail rebuild is single-active-card so static state
    // is fine.
    coll_rot_card = card;
    coll_rot_img = img;
    coll_rot_front_src = img_src;
    coll_rot_id = c.id;
    coll_rot_back_src = &ClipBoyGS153x192;
    coll_rot_angle = 0.0f;
    coll_rot_showing_back = false;
    coll_rot_press_active = false;

    lv_obj_add_event_cb(card, coll_rot_press_cb,    LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(card, coll_rot_pressing_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(card, coll_rot_release_cb,  LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(card, coll_rot_release_cb,  LV_EVENT_PRESS_LOST, NULL);

    // Collectible 75 (SheetmetalCon ticket): pulse a highlight outline around the
    // card so it reads as "special / tap me". Anim auto-frees when the card is
    // deleted on nav/theme rebuild (LVGL clears animations of a deleted var).
    if (c.id == 75) {
        lv_obj_set_style_outline_color(card, pip_highlight(), 0);
        lv_obj_set_style_outline_width(card, 3, 0);
        lv_obj_set_style_outline_pad(card, 2, 0);
        lv_anim_t oa;
        lv_anim_init(&oa);
        lv_anim_set_var(&oa, card);
        lv_anim_set_values(&oa, LV_OPA_20, LV_OPA_COVER);
        lv_anim_set_duration(&oa, 700);
        lv_anim_set_playback_duration(&oa, 700);
        lv_anim_set_repeat_count(&oa, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_exec_cb(&oa, [](void *o, int32_t v) {
            lv_obj_set_style_outline_opa((lv_obj_t *)o, (lv_opa_t)v, 0);
        });
        lv_anim_start(&oa);
    }

    // Rarity only (source is now its own label under the title).
    char tier_buf[80];
    snprintf(tier_buf, sizeof(tier_buf), "Rarity: %s", coll_tier_name(c.tier));
    lv_obj_t *tier_lbl = make_label(right_pane, tier_buf, &ui_font_pipboy_14,
               c.tier >= 2 ? pip_highlight() : pip_dim());
    lv_obj_set_width(tier_lbl, lv_pct(100));
    lv_obj_set_style_text_align(tier_lbl, LV_TEXT_ALIGN_CENTER, 0);

    // Description (word-wrapped). Set width FIRST so the wrap engine
    // has the constraint when set_text fires; explicit SIZE_CONTENT
    // height makes the label flex vertically with wrapped content
    // instead of inheriting any parent flex height constraint.
    lv_obj_t *desc = lv_label_create(right_pane);
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_size(desc, lv_pct(100), LV_SIZE_CONTENT);
    lv_label_set_text(desc, c.desc);
    lv_obj_set_style_text_font(desc, &ui_font_pipboy_14, 0);
    lv_obj_set_style_text_color(desc, pip_primary(), 0);

    // Stat modifiers
    if (c.mod_count > 0) {
        // Thin separator
        lv_obj_t *sep = lv_obj_create(right_pane);
        lv_obj_remove_style_all(sep);
        lv_obj_set_size(sep, lv_pct(100), 1);
        lv_obj_set_style_bg_color(sep, pip_border(), 0);
        lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);

        for (int m = 0; m < c.mod_count; m++) {
            char mod_buf[272];
            if (c.mods[m].value == 99 || c.mods[m].value == -99) {
                snprintf(mod_buf, sizeof(mod_buf), "%sINF %s",
                         c.mods[m].value > 0 ? "+" : "-",
                         c.mods[m].stat);
            } else {
                snprintf(mod_buf, sizeof(mod_buf), "%+d %s",
                         c.mods[m].value, c.mods[m].stat);
            }
            lv_obj_t *ml = make_label(right_pane, mod_buf, &ui_font_pipboy_14,
                       c.mods[m].value > 0 ? pip_highlight() : pip_dim());
            lv_label_set_long_mode(ml, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(ml, lv_pct(100));
        }
    }

    make_label(right_pane, "[COLLECTED]", &ui_font_pipboy_14, pip_highlight());

    // Shareable code: the tag's raised-bump pattern, so a friend who doesn't have this
    // one yet can hand-enter it (Collectibles > Scan > MANUAL ENTRY, or hold Scan) to
    // unlock it from yours. Only shown once you own it. (Bryce: "do it".)
    make_label(right_pane, "Share this code:", &ui_font_pipboy_14, pip_dim());
    {
        bool ug[4][4]; HRScan::Engine::encodeUserGrid((int)coll_items[idx].id, ug);
        lv_obj_t *sg = lv_obj_create(right_pane);
        lv_obj_remove_style_all(sg);
        lv_obj_set_size(sg, 98, 98);
        lv_obj_remove_flag(sg, LV_OBJ_FLAG_SCROLLABLE);
        for (int r = 0; r < 4; r++)
            for (int cc = 0; cc < 4; cc++) {
                lv_obj_t *cell = lv_obj_create(sg);
                lv_obj_remove_style_all(cell);
                lv_obj_set_size(cell, 20, 20);
                lv_obj_set_pos(cell, cc * 25, r * 25);
                lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_set_style_radius(cell, ug[r][cc] ? 8 : 2, 0);
                if (ug[r][cc]) {
                    lv_obj_set_style_bg_color(cell, pip_highlight(), 0);
                    lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
                } else {
                    lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
                    lv_obj_set_style_border_width(cell, 1, 0);
                    lv_obj_set_style_border_color(cell, pip_border(), 0);
                }
            }
    }

    // Collection-wide parody disclaimer, dim, under the per-item Source mark.
    // Generic "respective owners" wording so it's accurate for every franchise
    // without naming (or misnaming) any corporate rights holder. The
    // legally-load-bearing version lives on the Legal screen (legal_serious).
    lv_obj_t *foot = make_label(right_pane,
        "Fan-made parody. Properties (C) their respective owners.",
        &ui_font_pipboy_14, pip_dim());
    lv_label_set_long_mode(foot, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(foot, lv_pct(100));
    lv_obj_set_style_text_align(foot, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(foot, 6, 0);
}

static void hr_scan_stop(void);                // forward declaration
static void hr_scan_cleanup(void);             // forward declaration
static void hr_scan_delayed_cleanup_cb(lv_timer_t *t);  // forward declaration
static void hr_scan_btn_cb(lv_event_t *e);  // forward declaration
static void show_manual_grid(const bool *prefill16, bool from_scan);  // manual-entry grid (fwd)
static void show_found_reveal(int id);           // shared "Found: X" reveal (fwd)
static void hr_found_notthis_cb(lv_event_t *e);  // reveal "Not this? Fix" (fwd)

static void list_item_tap_cb(lv_event_t *e) {
    int32_t idx = (int32_t)(intptr_t)lv_event_get_user_data(e);
    cur_sel = (int16_t)idx;

    // Stop HR scanner if running (any tap dismisses scan UI)
    if (hr_scanning) hr_scan_stop();

    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    // Highlight the actual button position in its parent. Filter-safe --
    // works whether the Collectibles tab is in 'All' or 'Found' view.
    int16_t hi = (int16_t)lv_obj_get_index(btn);
    highlight_list_item(lv_obj_get_parent(btn), hi);

    if (cur_div == 1 && cur_tab == 1)
        show_collectible_detail(idx);
    else if (cur_div == 1 && cur_tab == 2) {
        if (idx == RADIO_SAO_IDX)
            show_radio(content_obj);          // the secret-real SAO opens the radio
        else
            show_item_detail(sao_items, NUM_SAOS, idx);
    }
}

static void build_split_pane(lv_obj_t *cont, const ListItem *items, int count,
                             bool dim_items, int enabled_idx = -1) {
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_set_style_pad_gap(cont, 0, 0);

    left_pane = create_left_list(cont);

    for (int i = 0; i < count; i++) {
        // enabled_idx (e.g. the real "Whether Radio" SAO) renders NOT-dim even in
        // an otherwise-dimmed list, so the one real entry reads as tappable.
        if (dim_items && i != enabled_idx)
            make_list_btn_dim(left_pane, items[i].name,
                              list_item_tap_cb, (void *)(intptr_t)i);
        else
            make_list_btn(left_pane, items[i].name,
                          list_item_tap_cb, (void *)(intptr_t)i);
    }

    right_pane = create_right_detail(cont);

    if (count > 0) {
        cur_sel = 0;
        highlight_list_item(left_pane, 0);
        show_item_detail(items, count, 0);
    } else {
        make_label(right_pane, "No items", &ui_font_pipboy_14, pip_dim());
    }
}

// ─────────────────────── ITEMS > Collectibles ─────────────────────────────

// Hand the VL53L5CX off to another consumer (HR scanner or radio activity).
// The HR scanner uses its OWN TwoWire + SparkFun driver on the SAME I2C bus 1,
// so a clean handoff requires: (1) wake the sensor — the scanner's begin()
// can't initialize a SLEEPing sensor; and (2) fully release bus 1 — leaving the
// theremin's TwoWire begun puts TWO instances on one I2C peripheral, which
// wedges the scanner's begin(). Without this, ANY theremin use before an HR
// scan hung the board (pre-existing bug; the scanner is the heavily-used path).
static void theremin_release_sensor(void) {
    if (theremin_poll_timer) {
        lv_timer_delete(theremin_poll_timer);
        theremin_poll_timer = NULL;
    }
    if (vl53_initialized) {
        vl53_sensor.stopRanging();
        audio_theremin_stop();
        vl53_initialized = false;
    }
    if (vl53_begun) {
        vl53_sensor.setPowerMode(SF_VL53L5CX_POWER_MODE::WAKEUP);
        vl53_wire.end();      // release I2C bus 1 for the next consumer
        vl53_begun = false;   // sensor handed off; theremin re-begins next time
    }
    CB_LOGLN("[HR] Released VL53L5CX from theremin (bus freed)");
}

// Check if an HR code ID corresponds to a valid collectible
static bool hr_id_is_valid(int id) {
    if (id < 0 || id > 255) return false;
    return coll_find_by_id((uint8_t)id) >= 0;
}

// Non-negative when the last scan ended on an ID that isn't in our
// collectibles CSV. The main loop reads this to display a specific
// "unknown code" banner rather than falling into the success path.
static int hr_scan_last_invalid_id = -1;

// Whitelist filter: noise can produce CRC-valid wrong IDs (e.g. 99 → 227).
// On a single invalid lock we ClearLock and keep scanning -- on the next
// frame window the engine has a fresh shot at the real ID. After a few
// total invalid locks (any IDs, not necessarily the same) we give up
// and surface the "unknown code" banner with the most recent ID. This
// matters because dense bit patterns can flip into different invalid
// IDs each cycle, which would otherwise stall until full timeout.
static constexpr int HR_INVALID_LOCK_THRESHOLD = 3;
static int hr_invalid_lock_total = 0;
static int hr_invalid_lock_last  = -1;

// Lock callback - fires when scanner confirms an ID
// Was the reveal's id NOT already owned before this scan/entry? Gates the "Not this?
// Fix" undo so it can only remove an unlock THIS action created -- never erase a
// collectible the user earned earlier (F1, review 2026-07-08).
static bool hr_found_newly = false;
static HRScan::LockAction hr_lock_cb(const HRScan::Result &r, void *user) {
    (void)user;
    CB_LOGF("[HR] Locked on ID %d (run=%d/%d)\n",
                  r.lockedId, r.run, r.runRequired);

    if (!hr_id_is_valid(r.lockedId)) {
        hr_invalid_lock_total++;
        hr_invalid_lock_last = r.lockedId;
        CB_LOGF("[HR] Reject ID %d (total=%d/%d)\n",
                      r.lockedId, hr_invalid_lock_total,
                      HR_INVALID_LOCK_THRESHOLD);

        if (hr_invalid_lock_total >= HR_INVALID_LOCK_THRESHOLD) {
            // Repeated invalid locks across different IDs strongly suggests
            // either a non-collectible code or persistent noise. Bail to
            // the unknown-code UX with the most recent ID.
            hr_scan_last_invalid_id = hr_invalid_lock_last;
            return HRScan::LockAction::StopScanner;
        }
        // Single/double invalid lock: probably a CRC-collision on noise.
        // Drop the lock and let the engine try again on subsequent frames.
        return HRScan::LockAction::ClearLock;
    }
    hr_scan_last_invalid_id = -1;
    hr_invalid_lock_total = 0;
    hr_invalid_lock_last  = -1;

    // Stop the scan bed FIRST so it doesn't play under/after scan_ok.
    audio_mp3_stop();
    if (cfg.sound) audio_mp3_play(scan_ok_mp3, scan_ok_mp3_len, false);

    // Snapshot ownership BEFORE marking so "Not this? Fix" can't erase a pre-owned id.
    hr_found_newly = !coll_is_found((uint8_t)r.lockedId);
    // Mark collectible as found in NVS -- UNLESS "Manually confirm scans" is on, in
    // which case the unlock is deferred until the user confirms the pattern in the
    // grid (hr_scan_success_cleanup routes to verify-first below; the grid's Confirm
    // does the coll_mark_found). The success modal + scroll-to already tell the user
    // what they unlocked; the cramped status bar would truncate titles, so leave it.
    if (!cfg.manual_confirm) coll_mark_found((uint8_t)r.lockedId);

    return HRScan::LockAction::StopScanner;
}

// Phase 2: actually init the scanner (called from one-shot timer)
static void hr_scan_begin_cb(lv_timer_t *t) {
    lv_timer_delete(t);
    // The scan may have been torn down during this deferred-begin's 50ms window (a
    // serial theme_set -> ui_theme_switch_live -> hr_scan_stop deletes hr_blackout +
    // sets it NULL; or a COLL_DEBUG coll add -> rebuild_content). Abort cleanly --
    // else the success branch below does lv_obj_clean(NULL) (crash). Guard on
    // hr_blackout ONLY: hr_scanning is NOT set true until the END of this callback's
    // success path (line ~5054), so checking !hr_scanning here aborts EVERY scan.
    if (!hr_blackout) {
        // F2 + F6 sibling. Found by enumerating EVERY early return that sits between the
        // caller arming the LED sweep and hr_scanning becoming true -- not by noticing it.
        // The overlay is gone (nav change, theme rebuild, or a tap within the 50 ms defer),
        // so this aborts, and without these three lines the sweep stays on and the
        // calibration flags stay set with hr_scanning still false -- which makes every
        // teardown path dead-end on `if (!hr_scanning) return;`. Identical consequence to the
        // sensor-failure return below; two exits, one leak.
        neo_scan_sweep_set(false);
        hr_cal_aiming = false;
        hr_cal_capturing = false;
        return;
    }

    HRScan::Config scan_cfg;
    scan_cfg.i2cSda          = CB_VL53_SDA;
    scan_cfg.i2cScl          = CB_VL53_SCL;
    scan_cfg.i2cClockHz      = CB_VL53_I2C_HZ;
    scan_cfg.sensorFps       = 15;
    scan_cfg.stopOnLock      = true;
    scan_cfg.scanTimeoutMs   = 30000;  // 30s timeout
    scan_cfg.stopOnTimeout   = true;
    scan_cfg.expectedId      = -1;     // No bias
    scan_cfg.profile         = HRScan::Profile::Sensitive10mm;  // matches CV-validated profile

    // New overlay (heatmap + level bubble + progress) takes most of the
    // hr_blackout area; stop button still owns the bottom-left strip.
    scan_cfg.overlay.x         = 0;
    scan_cfg.overlay.y         = 0;
    scan_cfg.overlay.width     = SCREEN_W;
    scan_cfg.overlay.height    = SCREEN_H - 40;
    // The overlay bg is hardcoded black. Flashbang's primary (0x202020) is dark
    // -- meant for its white UI bg -- so it would be near-invisible on black.
    // Use a light accent on Flashbang so all overlay text/borders (title,
    // instruction, legend labels, lock-status, heatmap border, progress, guides)
    // stay readable; the other themes' primary is already light.
    scan_cfg.overlay.themeAccent = (cfg.theme == 2) ? lv_color_hex(0xE0E0E0) : pip_primary();
    scan_cfg.overlay.themeBg     = lv_color_black();
    scan_cfg.overlay.grayscale   = (cfg.theme == 2);  // Flashbang B/W
    scan_cfg.overlay.showLevelBubble = false;  // we render a bigger one bottom-left (below)
    scan_cfg.overlay.titleText       = (hr_cal_aiming || hr_cal_capturing) ? "Calibrating" : "Scan Code";
    // Natural wrap; both imperial and metric so non-US users aren't lost.
    scan_cfg.overlay.instructionText =
        hr_cal_aiming    ? "Aim at a flat matte surface, filling the view. Tap Ready when steady."
      : hr_cal_capturing ? "Hold a flat matte surface ~8 cm (3 in) away, filling the view, steady..."
      :                    "Hold ~8 cm (3 in) away; line the code's 3 corners up with the guide boxes.";
    // Calibration: strip the HR-code chrome (corner guides, legend, lock status,
    // progress) so the aim readout + Ready button aren't buried in a busy overlay.
    if (hr_cal_aiming || hr_cal_capturing) {
        scan_cfg.overlay.showCodeGuides  = false;
        scan_cfg.overlay.showLockStatus  = false;
        scan_cfg.overlay.showProgressBar = false;
    }

    if (!hr_scanner.begin(scan_cfg)) {
        CB_LOGLN("[HR] Scanner init failed");
        // F2 + F6: this early return is the ONE path that leaves scan state armed while
        // hr_scanning is still false, and BOTH stranded resources hang off it.
        //
        // hr_scanning is set at the END of this function's success path, but the caller
        // (hr_scan_start) armed the LED sweep ~30 lines before creating the 50 ms timer that
        // runs this callback. So on this return the sweep is on with hr_scanning == false --
        // and every teardown funnels through hr_scan_stop(), which dead-ends on
        // `if (!hr_scanning) return;`. The four disarm sites are therefore unreachable and the
        // front LEDs keep sweeping until a reboot. The comment 30 lines up even explains why
        // hr_scanning cannot be checked here, which is exactly what defeats the guard.
        //
        // hr_cal_aiming has the same shape: set in hr_cal_start(), cleared only in
        // hr_scan_cleanup() and hr_scan_timeout_cleanup(), neither of which runs on this path.
        // Left set, hr_cal_tick() keeps running every loop and the NEXT Collectibles SCAN
        // silently renders as a calibration -- "Calibrating", aim-at-a-wall text, guides and
        // lock status stripped, MANUAL ENTRY suppressed, and a "Ready" button that
        // recalibrates instead of unlocking a collectible.
        //
        // Clear both HERE rather than adding another hr_scanning-keyed guard, because the
        // whole defect is that hr_scanning is not yet true. Trigger is a genuine sensor fault
        // (VL53L5CX exhaustion or an I2C error) -- the theremin-contention repro was
        // impossible, since hr_scan_start calls theremin_release_sensor() itself.
        neo_scan_sweep_set(false);
        hr_cal_aiming = false;
        hr_cal_capturing = false;
        if (lbl_stask) lv_label_set_text(lbl_stask, "");
        if (hr_blackout) {
            lv_obj_clean(hr_blackout);
            lv_obj_t *msg = lv_label_create(hr_blackout);
            lv_label_set_text(msg, "Sensor init failed");
            lv_obj_set_style_text_color(msg, pip_primary(), 0);
            lv_obj_set_style_text_font(msg, &ui_font_pipboy_16, 0);
            lv_obj_center(msg);
            hr_cancel_cleanup_timer();
            hr_cleanup_timer = lv_timer_create(hr_scan_delayed_cleanup_cb, 2000, NULL);
        }
        return;
    }

    // Remove the "Loading..." label, add the stop button (bottom-RIGHT). Narrower
    // now so a MANUAL ENTRY button fits bottom-LEFT with the level bubble centered.
    lv_obj_clean(hr_blackout);
    lv_obj_t *stop_btn = lv_btn_create(hr_blackout);
    lv_obj_set_size(stop_btn, 104, 32);
    lv_obj_align(stop_btn, LV_ALIGN_BOTTOM_RIGHT, -4, -4);
    lv_obj_set_style_bg_color(stop_btn, pip_disabled(), 0);
    lv_obj_set_style_bg_opa(stop_btn, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(stop_btn, hr_scan_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *stop_lbl = lv_label_create(stop_btn);
    lv_label_set_text(stop_lbl, "STOP SCAN");
    lv_obj_set_style_text_color(stop_lbl, pip_primary(), 0);
    lv_obj_set_style_text_font(stop_lbl, &ui_font_pipboy_14, 0);
    lv_obj_center(stop_lbl);

    // MANUAL ENTRY escape hatch (bottom-LEFT) -- for a tag that won't scan. Not
    // shown during calibration (that flow owns the bottom row with Ready). Tapping
    // tears the scan down + opens the blank grid, deferred out of the touch event
    // (delete-overlay + build-overlay in-event = UAF trap).
    if (!hr_cal_aiming && !hr_cal_capturing) {
        lv_obj_t *manual_btn = lv_btn_create(hr_blackout);
        lv_obj_set_size(manual_btn, 104, 32);
        lv_obj_align(manual_btn, LV_ALIGN_BOTTOM_LEFT, 4, -4);
        lv_obj_set_style_bg_opa(manual_btn, LV_OPA_TRANSP, 0);   // outline = subordinate to STOP
        lv_obj_set_style_border_width(manual_btn, 2, 0);
        lv_obj_set_style_border_color(manual_btn, pip_primary(), 0);
        lv_obj_add_event_cb(manual_btn, [](lv_event_t *e) {
            (void)e;
            lv_async_call([](void *u) { (void)u; hr_scan_stop(); show_manual_grid(NULL, false); }, NULL);
        }, LV_EVENT_CLICKED, NULL);
        lv_obj_t *ml = lv_label_create(manual_btn);
        lv_label_set_long_mode(ml, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(ml, 96);
        lv_label_set_text(ml, "MANUAL ENTRY");
        lv_obj_set_style_text_color(ml, pip_primary(), 0);
        lv_obj_set_style_text_font(ml, &ui_font_pipboy_14, 0);
        lv_obj_set_style_text_align(ml, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(ml);
    }

    // Bigger, more visible tilt/level bubble in the bottom-left (where STOP was).
    // Gravity aiming aid only (does not gate locking). Children of hr_blackout ->
    // self-null on delete so no dangling ptr in hr_level_update.
    hr_level_ring = lv_obj_create(hr_blackout);
    lv_obj_remove_flag(hr_level_ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(hr_level_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(hr_level_ring, 40, 40);
    // Centered along the bottom, between the MANUAL ENTRY (left) + STOP (right)
    // buttons so it no longer collides with the bottom-left button.
    lv_obj_align(hr_level_ring, LV_ALIGN_BOTTOM_MID, 0, -3);
    lv_obj_set_style_radius(hr_level_ring, 20, 0);
    lv_obj_set_style_bg_color(hr_level_ring, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(hr_level_ring, LV_OPA_60, 0);
    lv_obj_set_style_border_width(hr_level_ring, 1, 0);
    lv_obj_set_style_border_color(hr_level_ring, pip_primary(), 0);
    lv_obj_set_style_pad_all(hr_level_ring, 0, 0);
    lv_obj_add_event_cb(hr_level_ring, cb_selfnull_on_delete, LV_EVENT_DELETE, &hr_level_ring);

    hr_level_bub = lv_obj_create(hr_level_ring);
    lv_obj_remove_flag(hr_level_bub, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(hr_level_bub, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(hr_level_bub, 14, 14);
    lv_obj_set_style_radius(hr_level_bub, 7, 0);
    lv_obj_set_style_bg_color(hr_level_bub, pip_primary(), 0);
    lv_obj_set_style_bg_opa(hr_level_bub, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hr_level_bub, 0, 0);
    lv_obj_set_style_pad_all(hr_level_bub, 0, 0);
    lv_obj_set_pos(hr_level_bub, 12, 12);  // centered initially
    lv_obj_add_event_cb(hr_level_bub, cb_selfnull_on_delete, LV_EVENT_DELETE, &hr_level_bub);

    // Calibration AIM phase: a live coverage readout + a "Ready" button. Both are
    // children of hr_blackout (freed with it) and self-null on delete so the
    // per-loop hr_cal_aim_update() can't write through a dangling pointer.
    if (hr_cal_aiming) {
        hr_cal_readout = lv_label_create(hr_blackout);
        lv_label_set_text(hr_cal_readout, "--/64 zones");
        lv_obj_set_style_text_color(hr_cal_readout, pip_highlight(), 0);
        lv_obj_set_style_text_font(hr_cal_readout, &ui_font_pipboy_14, 0);
        lv_obj_set_style_text_align(hr_cal_readout, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(hr_cal_readout, LV_ALIGN_RIGHT_MID, -12, 46);  // right column, BELOW Ready
        lv_obj_add_event_cb(hr_cal_readout, cb_selfnull_on_delete, LV_EVENT_DELETE, &hr_cal_readout);

        hr_cal_ready_btn = lv_btn_create(hr_blackout);
        lv_obj_set_size(hr_cal_ready_btn, 104, 38);
        lv_obj_align(hr_cal_ready_btn, LV_ALIGN_RIGHT_MID, -12, 0);  // right column, ABOVE the readout
        lv_obj_set_style_bg_color(hr_cal_ready_btn, pip_highlight(), 0);
        lv_obj_set_style_bg_opa(hr_cal_ready_btn, LV_OPA_COVER, 0);
        lv_obj_add_event_cb(hr_cal_ready_btn,
            [](lv_event_t *e){ (void)e; hr_cal_do_capture(); }, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(hr_cal_ready_btn, cb_selfnull_on_delete, LV_EVENT_DELETE, &hr_cal_ready_btn);
        lv_obj_t *rl = lv_label_create(hr_cal_ready_btn);
        lv_label_set_text(rl, "Ready");
        lv_obj_set_style_text_color(rl, pip_bg(), 0);
        lv_obj_set_style_text_font(rl, &ui_font_pipboy_16, 0);
        lv_obj_center(rl);
    }

    // IMU shares the scanner's I2C bus (Wire1; LiDAR=0x29, IMU=0x6B).
    // Re-init each scan in case the scanner's bus was torn down.
    hr_imu_ok = hr_imu.begin(hr_scanner.wire());
    CB_LOGF("[HR] IMU: %s\n", hr_imu_ok ? "ok" : "absent");

    // CV decoder reliably handles dense bit patterns (e.g. ID 55) where
    // the legacy ROI search produces tracking-without-decode timeouts.
    // Validated in HRScanGuidance/Minimal -- ~70% per-frame Ok rate.
    hr_scanner.setUseCV(true);

    // DC34-155: the badge ships the anchor/SECDED tag spec (A3-D13), so the UI
    // scan MUST use the anchor decoder. Without this the engine defaults to the
    // legacy near/far CRC decode, which MISREADS a SECDED anchor tag as a
    // garbage ID (e.g. id-16 -> 72). The test harness enables it via `hr_anchor
    // 1`; production must enable it here so a manual UI scan matches. (All
    // physical tags are anchor tags -- there are no legacy tags in the wild.)
    hr_scanner.setUseAnchor(true);

    // Engine-level whitelist: drop noise that flipped into a non-collectible
    // ID before it can vote up to a lock. Without this, IDs like 227 burn
    // ~12 frames of voting before the lock callback can reject -- the user
    // sees the progress bar fill on noise.
    hr_scanner.setIdValidator(hr_id_is_valid);

    hr_scanner.setLockCallback(hr_lock_cb, NULL);
    hr_scan_last_invalid_id = -1;
    hr_invalid_lock_total = 0;
    hr_invalid_lock_last  = -1;
    hr_scanning = true;

    // Looping scan bed (Jeff Kaale via upbeat.io). Decoded WHOLE to PCM (batch
    // path, PSRAM-backed) then looped from the PCM buffer -- no per-loop decoder
    // re-sync, so the wrap is SEAMLESS (the streaming path had an audible gap at
    // the loop point). Stopped via audio_mp3_stop() on every scan-end path.
    if (cfg.sound && cfg.scan_sound) audio_mp3_play(scanning_mp3, scanning_mp3_len, true);
    // Don't write to lbl_stask -- the blackout covers the status bar
    // during scanning anyway, and on cleanup the modal handles feedback.
    CB_LOGLN("[HR] Scan started");
}

// Start HR code scanning - shows loading banner, then defers init
static void hr_scan_start(void) {
    if (hr_scanning) return;

    // Cancel any pending result-banner cleanup timer from a PRIOR scan -- else it
    // fires mid-scan and deletes the overlay we're about to create (UAF -> reboot).
    hr_cancel_cleanup_timer();

    // Mutual exclusion: stop any running tool/geiger (radio) before the
    // scanner claims the badge, then release the sensor from the theremin.
    cb_stop_operation();
    theremin_release_sensor();   // wakes sensor + frees I2C bus 1 (+ clears vl53_begun)

    // KITT/Cylon flourish: if the front LEDs are lit, run a red Larson sweep on
    // them for the duration of the scan (cleared on every scan-end path below).
    if (neo_front_leds_active()) neo_scan_sweep_set(true);

    // Black out + show loading banner immediately
    hr_blackout = lv_obj_create(lv_screen_active());
    // UAF audit: self-null if scr_main is deleted (theme switch) during the HR
    // result-banner window, so the pending one-shot cleanup timer no-ops its guard.
    lv_obj_add_event_cb(hr_blackout, cb_selfnull_on_delete, LV_EVENT_DELETE, &hr_blackout);
    lv_obj_remove_style_all(hr_blackout);
    lv_obj_set_size(hr_blackout, 320, 240);
    lv_obj_set_pos(hr_blackout, 0, 0);
    lv_obj_set_style_bg_color(hr_blackout, lv_color_black(), 0);
    // Fully opaque -- otherwise the underlying collectible-detail pane
    // bleeds through the 80% dimming and competes with the scan overlay's
    // own title/instruction/lock text.
    lv_obj_set_style_bg_opa(hr_blackout, LV_OPA_COVER, 0);
    lv_obj_add_flag(hr_blackout, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *loading = lv_label_create(hr_blackout);
    lv_label_set_text(loading, "Loading scanner...");
    lv_obj_set_style_text_color(loading, pip_primary(), 0);
    lv_obj_set_style_text_font(loading, &ui_font_pipboy_16, 0);
    lv_obj_center(loading);

    // Paint the banner NOW. The 50ms defer below is meant to let LVGL render it
    // before the blocking begin(), but when hr_scan_start runs from an async
    // callback (the cal path) the refresh timer can be beaten by begin_cb, so the
    // banner never shows and the ~2s spin-up looks frozen. Force it synchronously.
    lv_refr_now(NULL);

    // Defer the blocking begin() so LVGL can render the banner first
    lv_timer_create(hr_scan_begin_cb, 50, NULL);
}

// Clean up scan state: end scanner, remove blackout, clear status
// Safe to call even if scanner already disabled itself
static void hr_scan_cleanup(void) {
    hr_scanning = false;
    hr_cal_aiming = false; hr_cal_capturing = false;  // cancel any in-flight calibration
    neo_scan_sweep_set(false);   // stop the KITT sweep, restore normal front LEDs
    audio_mp3_stop();         // safety net: ensure the scan bed never plays past teardown
    // IMU before scanner: scanner.end() tears down Wire1, and a write
    // to a closed bus leaves the I2C peripheral half-latched -- which
    // locks up the next scanner.begin() waiting for an ACK.
    if (hr_imu_ok) hr_imu.end();
    hr_scanner.end();  // cleans up overlay + sensor + I2C
    if (hr_blackout) {
        lv_obj_delete(hr_blackout);
        hr_blackout = NULL;
    }
    if (lbl_stask) lv_label_set_text(lbl_stask, "");
}

// Optional: idx of the collectible to highlight after the success banner
// fades. Set by hr_scan_success_cleanup, consumed by the dismiss path.
static int hr_scan_pending_select = -1;

// Curated dismiss-button labels for the scan success modal. Picked at
// random per success so the button stays fresh on repeated unlocks.
static const char *kHrSuccessDismissWords[] = {
    "Cool",      "Sweet",      "Awesome",   "Nice",     "w00t",
    "Boom",      "Sick",       "Dope",      "Rad",      "Score!",
    "Pwned",     "Rooted",     "Looted",    "Acquired", "Excellent",
    "Got it",    "Heck yes",   "Boo-yah",   "Heyo",     "+1 STAT",
    "Engage",    "Make it so", "Shipped",   "Engaged",  "1337",
    "Groovy",    "Temba, his arms open",
};

// Shared finish path: tear down the scan modal and (on success) navigate
// the Collectibles list to the new entry. Called from both the timer-based
// cleanup (timeout/unknown banners) and the button-based success dismiss.
static void hr_scan_finish_modal(void) {
    hr_cancel_cleanup_timer();        // no-op if the timer cb itself called us
    if (hr_blackout) {
        lv_obj_delete(hr_blackout);
        hr_blackout = NULL;
    }
    // After a success scan, scroll to + select the unlocked collectible
    // so the user sees what they just won. Only fires on the Collectibles
    // tab (cur_div=1, cur_tab=1); ignored otherwise to avoid touching an
    // unrelated screen.
    if (hr_scan_pending_select >= 0 && cur_div == 1 && cur_tab == 1) {
        int idx = hr_scan_pending_select;
        hr_scan_pending_select = -1;
        if (idx >= 0 && idx < (int)coll_count) {
            cur_sel = (int16_t)idx;
            // Rebuild so the newly-collected item shows its title (was
            // labeled '??? Locked' when the list was originally built).
            rebuild_content();
            lv_obj_t *btn = find_coll_btn(idx);
            if (btn) {
                highlight_list_item(left_pane,
                                    (int8_t)lv_obj_get_index(btn));
                lv_obj_scroll_to_view(btn, LV_ANIM_ON);
            }
            show_collectible_detail(idx);
        }
    }
    hr_scan_pending_select = -1;
}

// Delayed cleanup after showing a timeout/unknown banner.
static void hr_scan_delayed_cleanup_cb(lv_timer_t *t) {
    lv_timer_delete(t);
    hr_cleanup_timer = NULL;          // handle now dangling -- clear before anyone cancels
    hr_scan_finish_modal();
}

// Button click on the success-modal dismiss button.
static void hr_scan_success_dismiss_cb(lv_event_t *e) {
    (void)e;
    hr_scan_finish_modal();
    // If this find just crossed a station's collectible gate, announce the new
    // station ~1.5s after this modal is dismissed (chained, per the design).
    lv_timer_t *rt = lv_timer_create(radio_unlock_timer_cb, 1500, NULL);
    lv_timer_set_repeat_count(rt, 1);
}

// Show a timeout/result banner, then clean up after a delay
static void hr_scan_timeout_cleanup(void) {
    hr_scanning = false;
    neo_scan_sweep_set(false);   // stop the KITT sweep, restore normal front LEDs
    audio_mp3_stop();            // stop the looping scan bed (no chime on timeout)
    hr_cal_aiming = false; hr_cal_capturing = false;  // aim/capture ended by timeout
    if (hr_imu_ok) hr_imu.end();  // before scanner; see hr_scan_cleanup
    hr_scanner.end();  // cleans up scan overlay + sensor

    // Reuse blackout to show timeout message
    if (hr_blackout) {
        lv_obj_clean(hr_blackout);
        lv_obj_t *msg = lv_label_create(hr_blackout);
        lv_label_set_text(msg, "Scan timed out");
        lv_obj_set_style_text_color(msg, pip_primary(), 0);
        lv_obj_set_style_text_font(msg, &ui_font_pipboy_16, 0);
        lv_obj_center(msg);
        hr_cancel_cleanup_timer();     // never stack two
        hr_cleanup_timer = lv_timer_create(hr_scan_delayed_cleanup_cb, 1500, NULL);
    }
    if (lbl_stask) lv_label_set_text(lbl_stask, "");
}

// Cleanup path for a successful collectible pickup. Shows a permanent
// "Found: X" modal with a randomly-labeled dismiss button so the user
// gets to read what they just unlocked instead of having it auto-fade.
static void hr_scan_success_cleanup(int id) {
    // UX: show the success image FIRST, shut the LiDAR down LAST. hr_scanner.end()
    // (sensor + I2C teardown) is the slow part; running it before the modal stalls
    // between the lock sound and the image. So build + PAINT the modal, then tear
    // down below. The lock sound already played in hr_lock_cb.
    hr_scanning = false;
    neo_scan_sweep_set(false);   // stop the KITT sweep, restore normal front LEDs

    // Verify-first ("Manually confirm scans"): the unlock was deferred (hr_lock_cb
    // skipped coll_mark_found). Tear the scan down and open the grid PRE-FILLED with
    // the decoded pattern -- a correct scan is one Confirm tap; a wrong one, fix the
    // cells first. The grid's Confirm does the actual unlock + reveal.
    if (cfg.manual_confirm) {
        if (hr_imu_ok) hr_imu.end();
        hr_scanner.end();
        if (hr_blackout) { lv_obj_delete(hr_blackout); hr_blackout = NULL; }
        bool ug[4][4]; HRScan::Engine::encodeUserGrid(id, ug);
        bool flat[16];
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) flat[r * 4 + c] = ug[r][c];
        show_manual_grid(flat, true);   // ONE editable screen: confirm-as-is or fix, then Confirm
        return;
    }

    show_found_reveal(id);   // single shared reveal (art + pattern thumbnail + Fix + dismiss)
    // Leave lbl_stask alone -- the lock callback already wrote
    // "Found: <title>" there and that survives once the blackout is gone.

    // Paint the modal NOW, THEN shut down the sensor/IMU (the slow teardown) so
    // the pause lands after the image is on screen, not before it. (IMU before
    // scanner: scanner.end() tears down Wire1 and a write to a closed bus half-
    // latches the I2C peripheral.)
    lv_refr_now(NULL);
    if (hr_imu_ok) hr_imu.end();
    hr_scanner.end();
}

// Cleanup path for a valid decode that isn't in our collectibles list
// (e.g. a physical code with an ID outside 1-100). Shows the ID so the
// user can distinguish this from a timeout or a real collectible pickup.
static void hr_scan_unknown_cleanup(int id) {
    hr_scanning = false;
    neo_scan_sweep_set(false);   // stop the KITT sweep, restore normal front LEDs
    audio_mp3_stop();            // stop the looping scan bed (no chime on unknown-id)
    if (hr_imu_ok) hr_imu.end();  // power down before the bus is torn down
    hr_scanner.end();
    if (hr_blackout) {
        lv_obj_clean(hr_blackout);
        lv_obj_t *msg = lv_label_create(hr_blackout);
        lv_label_set_text_fmt(msg, "Unknown code: %d", id);
        lv_obj_set_style_text_color(msg, pip_primary(), 0);
        lv_obj_set_style_text_font(msg, &ui_font_pipboy_16, 0);
        lv_obj_center(msg);
        hr_cancel_cleanup_timer();     // TRACK it (same as the timeout/sensor-fail sites) --
        hr_cleanup_timer = lv_timer_create(hr_scan_delayed_cleanup_cb, 2000, NULL);  // else a
                                       // new scan can't cancel it -> UAF on the next overlay.
    }
    // Status bar stays clean -- the modal banner is the user-facing
    // feedback for an unknown-code scan.
}

// Stop HR code scanning (user-initiated or nav change)
static void hr_scan_stop(void) {
    if (!hr_scanning) return;
    audio_mp3_stop();          // stop the looping scan bed
    if (cfg.sound) audio_mp3_play(scan_stop_mp3, scan_stop_mp3_len, false);
    hr_scan_cleanup();
    CB_LOGLN("[HR] Scan stopped");
}

// "Not this? Fix" on the reveal: the result was wrong. Undo it (remove the wrongly-
// attributed id) and open the grid PRE-FILLED with what was decoded, so the user can
// see the read + fix the cells that don't match their tag. Deferred out of the touch
// event (delete + build overlay = UAF trap).
static bool hr_notthis_prefill[16];
static bool hr_notthis_has = false;
static void hr_found_notthis_cb(lv_event_t *e) {
    (void)e;
    int ci = hr_scan_pending_select;
    int wrong_id = (ci >= 0 && ci < (int)coll_count) ? (int)coll_items[ci].id : -1;
    hr_notthis_has = false;
    if (wrong_id >= 0) {
        // Only UNDO an unlock this scan/entry actually created -- never erase a
        // collectible the user already owned (a misread that decodes to an owned id).
        if (hr_found_newly) coll_mark_not_found((uint8_t)wrong_id);
        bool ug[4][4]; HRScan::Engine::encodeUserGrid(wrong_id, ug);
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) hr_notthis_prefill[r * 4 + c] = ug[r][c];
        hr_notthis_has = true;
    }
    hr_scan_pending_select = -1;
    lv_async_call([](void *u) {
        (void)u;
        if (hr_blackout) { lv_obj_delete(hr_blackout); hr_blackout = NULL; }
        show_manual_grid(hr_notthis_has ? hr_notthis_prefill : NULL, true);
    }, NULL);
}

// Shared "Found: <title>" reveal ceremony -- collectible art + title + a randomly-
// labeled dismiss button. Manual-entry Confirm routes through this so a hand-entered
// unlock gets the SAME payoff as a scan. Creates its own full-screen overlay in
// hr_blackout (manual entry has no scan overlay); the dismiss button reuses the
// scanner's hr_scan_success_dismiss_cb -> hr_scan_finish_modal, which navigates to
// the item (via hr_scan_pending_select) and tears the overlay down. (Mirrors the
// modal hr_scan_success_cleanup builds inline; kept separate so the proven scan
// teardown path is untouched.)
static void show_found_reveal(int id) {
    int ci = coll_find_by_id((uint8_t)id);
    hr_scan_pending_select = ci;              // dismiss-cb scrolls to this
    if (hr_blackout) { lv_obj_move_foreground(hr_blackout); lv_obj_clean(hr_blackout); }
    else {
        hr_blackout = lv_obj_create(lv_screen_active());
        lv_obj_add_event_cb(hr_blackout, cb_selfnull_on_delete, LV_EVENT_DELETE, &hr_blackout);
        lv_obj_remove_style_all(hr_blackout);
        lv_obj_set_size(hr_blackout, 320, 240);
        lv_obj_set_pos(hr_blackout, 0, 0);
        lv_obj_set_style_bg_color(hr_blackout, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(hr_blackout, LV_OPA_COVER, 0);
        lv_obj_add_flag(hr_blackout, LV_OBJ_FLAG_CLICKABLE);
    }
    const int half = SCREEN_W / 2;  // 160

    // Left: collectible art -- enlarged to fill the empty left/top margins.
    const lv_image_dsc_t *img_src = (ci >= 0) ? coll_load_image(coll_items[ci].id) : nullptr;
    if (!img_src && ci >= 0) img_src = coll_get_builtin_image(coll_items[ci].id);
    if (img_src) {
        lv_obj_t *img = lv_image_create(hr_blackout);
        lv_image_set_src(img, img_src);
        lv_obj_set_style_image_recolor(img, pip_primary(), 0);
        lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
        lv_image_set_scale(img, coll_fit_scale(img_src, 152));
        lv_obj_align(img, LV_ALIGN_TOP_LEFT, 2, 2);
    }

    // Top-RIGHT: 'Found: <title>'. WRAPS fully -- no ellipsis -- so even the longest name
    // (with parentheticals) fits; the grid below is positioned AFTER measuring this label.
    lv_obj_t *msg = lv_label_create(hr_blackout);
    lv_obj_set_width(msg, half - 12);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    if (ci >= 0) lv_label_set_text_fmt(msg, "Found:\n%s", coll_items[ci].title);
    else         lv_label_set_text_fmt(msg, "Found: ID %d", id);
    lv_obj_set_style_text_color(msg, pip_primary(), 0);
    lv_obj_set_style_text_font(msg, &ui_font_pipboy_14, 0);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(msg, LV_ALIGN_TOP_RIGHT, -6, 6);

    // Measure the title, then sit the pattern grid vertically CENTERED between the title's
    // bottom and the "Got it" button's top -- no overlap no matter how long the title is.
    lv_obj_update_layout(msg);
    int title_bottom = lv_obj_get_y(msg) + lv_obj_get_height(msg);
    const int gotit_top = 240 - 8 - 46;   // dismiss btn is 46 tall, 8 off the bottom

    // Right: the tag's decoded bump pattern (share + verify). Always shown (also fixes it
    // vanishing after Fix->Confirm, since this is now the ONE shared reveal).
    if (ci >= 0) {
        const int grid_sz = 56;
        int grid_y = title_bottom + ((gotit_top - title_bottom) - grid_sz) / 2;
        if (grid_y < title_bottom + 4) grid_y = title_bottom + 4;   // clamp for a tall title
        bool ug[4][4]; HRScan::Engine::encodeUserGrid(id, ug);
        lv_obj_t *th = lv_obj_create(hr_blackout);
        lv_obj_remove_style_all(th);
        lv_obj_set_size(th, grid_sz, grid_sz);
        lv_obj_set_pos(th, half + (half - grid_sz) / 2, grid_y);   // centered in the right half
        lv_obj_remove_flag(th, LV_OBJ_FLAG_SCROLLABLE);
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) {
                lv_obj_t *cell = lv_obj_create(th);
                lv_obj_remove_style_all(cell);
                lv_obj_set_size(cell, 12, 12);
                lv_obj_set_pos(cell, c * 14, r * 14);
                lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_set_style_radius(cell, ug[r][c] ? 5 : 1, 0);
                if (ug[r][c]) {
                    lv_obj_set_style_bg_color(cell, pip_highlight(), 0);
                    lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
                } else {
                    lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
                    lv_obj_set_style_border_width(cell, 1, 0);
                    lv_obj_set_style_border_color(cell, pip_border(), 0);
                }
            }
    }

    // Bottom-LEFT: "< Fix" (correct a wrong result). Opposite corner from "Got it" so
    // they can't be fat-fingered for each other.
    lv_obj_t *fix = lv_btn_create(hr_blackout);
    lv_obj_set_size(fix, 96, 34);
    lv_obj_align(fix, LV_ALIGN_BOTTOM_LEFT, 8, -8);
    lv_obj_set_style_bg_opa(fix, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(fix, 2, 0);
    lv_obj_set_style_border_color(fix, pip_border(), 0);
    lv_obj_add_event_cb(fix, hr_found_notthis_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *fix_lbl = make_label(fix, "< Fix", &ui_font_pipboy_16, pip_primary());
    lv_obj_center(fix_lbl);

    // Bottom-RIGHT: primary dismiss (random fun word).
    const size_t nWords = sizeof(kHrSuccessDismissWords) / sizeof(kHrSuccessDismissWords[0]);
    const char *word = kHrSuccessDismissWords[esp_random() % nWords];
    lv_obj_t *dismiss = lv_btn_create(hr_blackout);
    lv_obj_set_size(dismiss, half - 14, 46);
    lv_obj_align(dismiss, LV_ALIGN_BOTTOM_RIGHT, -6, -8);
    lv_obj_set_style_bg_color(dismiss, pip_primary(), 0);
    lv_obj_set_style_bg_opa(dismiss, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(dismiss, hr_scan_success_dismiss_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *dismiss_lbl = lv_label_create(dismiss);
    lv_label_set_long_mode(dismiss_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(dismiss_lbl, half - 14 - 12);
    lv_label_set_text(dismiss_lbl, word);
    lv_obj_set_style_text_color(dismiss_lbl, lv_color_black(), 0);
    lv_obj_set_style_text_font(dismiss_lbl, &ui_font_pipboy_16, 0);
    lv_obj_set_style_text_align(dismiss_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(dismiss_lbl);
}

// ─── Manual-entry / scan-correction grid (feature/manual-entry-grid, Task 2) ──
// A reusable full-screen 4x4 grid dialog. The user reproduces the raised-bump
// pattern of a physical tag (USER-VIEW: TL/TR/BR corners raised, BL flat, all
// four corners locked structure) on the 12 editable data cells; each toggle is
// live-decoded through the SAME anchor/SECDED engine the scanner uses. Confirm
// enables ONLY when the pattern decodes to a valid catalog id -- the id/identity
// is never revealed pre-Confirm (anti-spoiler). See docs/manual-entry-grid.md §6.
//
// Globals a callback touches self-null on LV_EVENT_DELETE (project UAF trap):
// the overlay, the status label, and the Confirm button. Cell taps repaint via
// the event target (local), so no per-cell global is kept. No lv_timer -- the
// grid is purely event-driven (decode on tap).
static lv_obj_t *mg_overlay     = NULL;   // full-screen overlay (hr_blackout-style)
static lv_obj_t *mg_status_lbl  = NULL;   // "VALID CODE"/"NO VALID CODE"
static lv_obj_t *mg_confirm_btn = NULL;   // gated Confirm button
static bool      mg_grid[4][4];           // current pattern, USER-VIEW [r][c]
static int       mg_result_id   = -1;     // last valid decode, else -1

// Which cells are fixed structure (the 4 corners). Non-corner = editable data.
static const bool mg_is_corner[4][4] = {
    { true,  false, false, true  },
    { false, false, false, false },
    { false, false, false, false },
    { true,  false, false, true  },
};

// Paint one cell to reflect raised/flat and locked/editable state. Bump-style so the
// pattern reads like the physical tag: RAISED cells are domed (big corner radius) +
// filled; FLAT cells are square + hollow. A user holding the tag can pattern-match
// "round-and-lit = raised bump" at a glance without learning an abstract mapping.
static void mg_paint_cell(lv_obj_t *cell, bool raised, bool locked) {
    lv_obj_set_style_border_width(cell, 2, 0);
    lv_obj_set_style_radius(cell, raised ? 16 : 3, 0);   // domed bump vs flat square
    if (locked) {
        // Locked corner: dim + visually distinct so it reads as non-editable.
        lv_obj_set_style_border_color(cell, pip_disabled(), 0);
        if (raised) {
            lv_obj_set_style_bg_color(cell, pip_disabled(), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_50, 0);
        } else {
            lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
        }
    } else if (raised) {
        // Data cell, raised/near: solid amber fill.
        lv_obj_set_style_bg_color(cell, pip_highlight(), 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(cell, pip_primary(), 0);
    } else {
        // Data cell, flat/far: dim empty outline.
        lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(cell, pip_border(), 0);
    }
}

// Re-decode the current grid and gate Confirm + status line. No id is shown.
static void mg_recompute(void) {
    int id = HRScan::Engine::decodeUserGrid(mg_grid);
    bool ok = (id >= 0) && hr_id_is_valid(id);
    if (ok) {
        mg_result_id = id;
        if (mg_status_lbl) {
            lv_label_set_text(mg_status_lbl, "VALID CODE");
            lv_obj_set_style_text_color(mg_status_lbl, pip_highlight(), 0);
        }
        if (mg_confirm_btn) {
            lv_obj_remove_state(mg_confirm_btn, LV_STATE_DISABLED);
            lv_obj_set_style_bg_opa(mg_confirm_btn, LV_OPA_COVER, 0);
        }
    } else {
        mg_result_id = -1;
        if (mg_status_lbl) {
            lv_label_set_text(mg_status_lbl, "NO VALID CODE");
            lv_obj_set_style_text_color(mg_status_lbl, pip_disabled(), 0);
        }
        if (mg_confirm_btn) {
            lv_obj_add_state(mg_confirm_btn, LV_STATE_DISABLED);
            lv_obj_set_style_bg_opa(mg_confirm_btn, LV_OPA_40, 0);
        }
    }
}

// Tear down the overlay (children self-null on delete). Deferred out of button
// events via lv_async_call so we never delete the widget mid-event (UAF trap).
static void mg_close_async(void *unused) {
    (void)unused;
    if (mg_overlay) { lv_obj_delete(mg_overlay); mg_overlay = NULL; }
}

// First-run Clippy-guided helper over the grid (shown once, NVS-gated). Child of
// mg_overlay so it's freed if the grid closes; "Got it!" deletes just the scrim.
static void mg_show_helper(void) {
    if (!mg_overlay) return;
    lv_obj_t *sc = lv_obj_create(mg_overlay);
    lv_obj_remove_style_all(sc);
    lv_obj_set_size(sc, 320, 240);
    lv_obj_set_pos(sc, 0, 0);
    lv_obj_set_style_bg_color(sc, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(sc, LV_OPA_70, 0);
    lv_obj_add_flag(sc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(sc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *box = lv_obj_create(sc);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 288, 190);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, pip_bg(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, pip_highlight(), 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_pad_all(box, 10, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *t = make_label(box,
        "It looks like you're entering a code!\n\n"
        "Tap the squares to match the raised bumps on your tag -- I'll say VALID CODE "
        "when it's a real one, then hit CONFIRM.",
        &ui_font_pipboy_14, pip_primary());
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(t, 264);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_t *ok = lv_btn_create(box);
    lv_obj_set_size(ok, 110, 36);
    lv_obj_align(ok, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(ok, pip_highlight(), 0);
    lv_obj_set_style_bg_opa(ok, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(ok, [](lv_event_t *e) {
        lv_obj_t *b = (lv_obj_t *)lv_event_get_target(e);
        lv_obj_delete(lv_obj_get_parent(lv_obj_get_parent(b)));  // btn -> box -> scrim
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_t *okl = make_label(ok, "Got it!", &ui_font_pipboy_16, pip_bg());
    lv_obj_center(okl);
}

static void mg_cell_cb(lv_event_t *e) {
    int rc = (int)(intptr_t)lv_event_get_user_data(e);
    int r = rc >> 2, c = rc & 3;
    mg_grid[r][c] = !mg_grid[r][c];
    audio_play_click();   // cells are plain lv_obj -> miss the global button click hook
    mg_paint_cell((lv_obj_t *)lv_event_get_target(e), mg_grid[r][c], false);
    mg_recompute();
}

// Confirm commits the unlock. Deferred out of the button event via lv_async_call
// because it deletes the overlay AND rebuilds the collectible screen -- doing that
// synchronously inside the touch event is the documented UAF trap.
static void mg_confirm_finish(void *unused) {
    (void)unused;
    int id = mg_result_id;
    if (mg_overlay) { lv_obj_delete(mg_overlay); mg_overlay = NULL; }
    if (id < 0) return;                       // Confirm was gated on a valid id anyway
    hr_found_newly = !coll_is_found((uint8_t)id);   // snapshot for the "Not this?" undo guard
    coll_mark_found((uint8_t)id);             // persist to NVS (same path as a scan)
    CB_LOGF("[MANUAL] unlocked id=%d\n", id);
    // Same "Found: X" ceremony a scan gets. Its dismiss button (hr_scan_success_
    // dismiss_cb) navigates to the item on the Collectibles list + chains the radio
    // station-unlock check.
    show_found_reveal(id);
}
static void mg_confirm_cb(lv_event_t *e) {
    (void)e;
    lv_async_call(mg_confirm_finish, NULL);
}

static void mg_cancel_cb(lv_event_t *e) {
    (void)e;
    lv_async_call(mg_close_async, NULL);
}

// prefill16: 16 bools row-major in USER-VIEW (r*4+c), or NULL for a blank grid.
// Corner cells are always forced to the fixed structure regardless of prefill.
// from_scan: true = "CONFIRM SCAN" framing, false = "MANUAL ENTRY" framing.
static void show_manual_grid(const bool *prefill16, bool from_scan) {
    // Drop any stale instance (children self-null; belt-and-suspenders reset).
    if (mg_overlay) { lv_obj_delete(mg_overlay); mg_overlay = NULL; }
    mg_status_lbl = NULL;
    mg_confirm_btn = NULL;
    mg_result_id = -1;

    // Seed grid state: corners fixed (TL/TR/BR raised, BL flat), data from prefill.
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            mg_grid[r][c] = prefill16 ? (prefill16[r * 4 + c] != 0) : false;
    mg_grid[0][0] = true;  mg_grid[0][3] = true;   // TL, TR raised
    mg_grid[3][3] = true;  mg_grid[3][0] = false;  // BR raised, BL flat

    // Full-screen opaque overlay (mirrors the hr_blackout pattern).
    mg_overlay = lv_obj_create(lv_screen_active());
    lv_obj_add_event_cb(mg_overlay, cb_selfnull_on_delete, LV_EVENT_DELETE, &mg_overlay);
    lv_obj_remove_style_all(mg_overlay);
    lv_obj_set_size(mg_overlay, 320, 240);
    lv_obj_set_pos(mg_overlay, 0, 0);
    lv_obj_set_style_bg_color(mg_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(mg_overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(mg_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(mg_overlay, LV_OBJ_FLAG_SCROLLABLE);

    // LEFT: 4x4 grid at (8,8), 200x200, 50px pitch, 46px cells (4px gaps).
    lv_obj_t *grid_cont = lv_obj_create(mg_overlay);
    lv_obj_remove_style_all(grid_cont);
    lv_obj_set_size(grid_cont, 200, 200);
    lv_obj_set_pos(grid_cont, 8, 8);
    lv_obj_remove_flag(grid_cont, LV_OBJ_FLAG_SCROLLABLE);
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            lv_obj_t *cell = lv_obj_create(grid_cont);
            lv_obj_remove_style_all(cell);
            lv_obj_set_size(cell, 46, 46);
            lv_obj_set_pos(cell, c * 50 + 2, r * 50 + 2);
            lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_radius(cell, 4, 0);
            if (mg_is_corner[r][c]) {
                lv_obj_remove_flag(cell, LV_OBJ_FLAG_CLICKABLE);
                mg_paint_cell(cell, mg_grid[r][c], true);
            } else {
                lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_event_cb(cell, mg_cell_cb, LV_EVENT_CLICKED,
                                    (void *)(intptr_t)(r * 4 + c));
                mg_paint_cell(cell, mg_grid[r][c], false);
            }
        }
    }

    // RIGHT column: x=214, w=98.
    lv_obj_t *title = make_label(mg_overlay,
        from_scan ? "CONFIRM SCAN" : "MANUAL ENTRY",
        &ui_font_pipboy_18, pip_primary());
    lv_obj_set_width(title, 98);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(title, 214, 6);

    lv_obj_t *caption = make_label(mg_overlay,
        from_scan ? "Tap any wrong squares"
                  : "Tap the raised bumps",
        &ui_font_pipboy_14, pip_primary());
    lv_obj_set_width(caption, 98);
    lv_label_set_long_mode(caption, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(caption, 214, 68);   // extra gap below the 2-line 18px title (y6..~50)

    mg_status_lbl = make_label(mg_overlay, "NO VALID CODE",
                               &ui_font_pipboy_14, pip_disabled());
    lv_obj_set_pos(mg_status_lbl, 214, 122);
    lv_obj_add_event_cb(mg_status_lbl, cb_selfnull_on_delete,
                        LV_EVENT_DELETE, &mg_status_lbl);

    mg_confirm_btn = lv_btn_create(mg_overlay);
    lv_obj_set_size(mg_confirm_btn, 92, 46);
    lv_obj_set_pos(mg_confirm_btn, 214, 148);
    lv_obj_set_style_bg_color(mg_confirm_btn, pip_highlight(), 0);
    lv_obj_set_style_bg_opa(mg_confirm_btn, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(mg_confirm_btn, mg_confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(mg_confirm_btn, cb_selfnull_on_delete,
                        LV_EVENT_DELETE, &mg_confirm_btn);
    lv_obj_t *clbl = make_label(mg_confirm_btn, "CONFIRM",
                                &ui_font_pipboy_16, pip_bg());
    lv_obj_center(clbl);

    lv_obj_t *cancel = lv_btn_create(mg_overlay);   // strictly local -> no self-null
    lv_obj_set_size(cancel, 92, 38);
    lv_obj_set_pos(cancel, 214, 200);
    lv_obj_set_style_bg_opa(cancel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cancel, 2, 0);
    lv_obj_set_style_border_color(cancel, pip_border(), 0);
    lv_obj_add_event_cb(cancel, mg_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *canlbl = make_label(cancel, "CANCEL",
                                  &ui_font_pipboy_16, pip_primary());
    lv_obj_center(canlbl);

    // "Verify each scan" toggle (from_scan context only), bottom-left under the grid.
    // Same setting + wording + polarity as DATA > Settings > SYSTEM: ON = verify; flip
    // OFF here to auto-unlock future scans. A SWITCH not lv_checkbox (the checkbox tick
    // U+F00C is absent from our fonts -> tofu + missed taps). Initial state mirrors cfg.
    if (from_scan) {
        lv_obj_t *sl = make_label(mg_overlay, "Verify each scan", &ui_font_pipboy_14, pip_dim());
        lv_obj_align(sl, LV_ALIGN_BOTTOM_LEFT, 8, -8);
        lv_obj_t *sw = lv_switch_create(mg_overlay);
        lv_obj_set_size(sw, 40, 20);
        lv_obj_align(sw, LV_ALIGN_BOTTOM_LEFT, 150, -6);
        lv_obj_set_style_bg_color(sw, pip_highlight(), LV_PART_INDICATOR | LV_STATE_CHECKED);
        if (cfg.manual_confirm) lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_add_event_cb(sw, [](lv_event_t *e) {
            lv_obj_t *s = (lv_obj_t *)lv_event_get_target(e);
            cfg.manual_confirm = lv_obj_has_state(s, LV_STATE_CHECKED);   // ON = verify each scan
            cfg_save_manual_confirm();
        }, LV_EVENT_VALUE_CHANGED, NULL);
    }

    // Initial decode so a correct prefill arrives with Confirm already enabled.
    mg_recompute();

    // First-run Clippy helper, shown once ever (NVS-gated).
    cfg_prefs.begin(CFG_NAMESPACE, false);
    bool helped = cfg_prefs.getBool("mghelp", false);
    if (!helped) cfg_prefs.putBool("mghelp", true);
    cfg_prefs.end();
    if (!helped) mg_show_helper();
}

// ---- Sensor flat-field calibration ----
static void hr_cal_load(void) {   // restore the 64-zone baseline from NVS at boot
    // cfg_prefs is open/close per-op (ui_config.h) -- it's CLOSED here, so we must
    // open our own handle or the access silently no-ops (cal never restores).
    cfg_prefs.begin(CFG_NAMESPACE, true);
    size_t got = cfg_prefs.getBytes("zonecal", hr_zone_cal, sizeof(hr_zone_cal));
    hr_cal_zones  = cfg_prefs.getInt("calzn", 0);
    hr_cal_avg_mm = cfg_prefs.getInt("calavg", 0);
    cfg_prefs.end();
    if (got == sizeof(hr_zone_cal)) { hr_scanner.setZoneCal(hr_zone_cal); hr_has_cal = true; }
}
static void hr_cal_clear(void) {
    hr_scanner.clearZoneCal();
    memset(hr_zone_cal, 0, sizeof(hr_zone_cal));
    hr_has_cal = false;
    hr_cal_zones = 0; hr_cal_avg_mm = 0;
    cfg_prefs.begin(CFG_NAMESPACE, false);
    cfg_prefs.remove("zonecal");
    cfg_prefs.remove("calzn");
    cfg_prefs.remove("calavg");
    cfg_prefs.end();
    Serial.println("[CAL] cleared");
}
// Brief centered toast, auto-dismissed (delete_delayed is UAF-safe: if the
// screen rebuilds first, the pending delete is dropped with the object).
static void hr_cal_toast(const char *msg) {
    lv_obj_t *toast = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(toast);
    lv_obj_set_size(toast, 260, LV_SIZE_CONTENT);
    lv_obj_align(toast, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(toast, pip_bg(), 0);
    lv_obj_set_style_bg_opa(toast, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(toast, pip_highlight(), 0);
    lv_obj_set_style_border_width(toast, 2, 0);
    lv_obj_set_style_pad_all(toast, 12, 0);
    lv_obj_remove_flag(toast, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = make_label(toast, msg, &ui_font_pipboy_16, pip_primary());
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_delete_delayed(toast, 2600);
}
// Start a capture: begin a scan (LiDAR on + ticking) + flat-field averaging.
// hr_cal_tick() (called from loop) finalizes after the window. Aim at a flat
// matte surface at scan distance so the whole FoV returns valid depths.
// Live coverage readout during the AIM phase (driven every loop by hr_cal_tick).
static void hr_cal_aim_update(void) {
    if (!hr_cal_aiming || !hr_cal_readout) return;
    const int16_t (*mm)[8]  = hr_scanner.lastFilteredMm();
    const bool    (*val)[8] = hr_scanner.lastFilteredValid();
    int zones = 0; long sum = 0;
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++)
            if (val[r][c]) { zones++; sum += mm[r][c]; }
    // Mirror the live reading into the modal-display globals so what the user
    // SAW while aiming is what the modal reports afterward (consistent, and this
    // cal-corrected/filtered value tracks a ruler -- unlike the raw capture mean).
    hr_cal_zones  = zones;
    hr_cal_avg_mm = (zones > 0) ? (int)(sum / (long)zones) : 0;
    char buf[64];
    if (zones <= 0) {
        snprintf(buf, sizeof(buf), "--/64 zones\n(aim at a surface)");
        lv_obj_set_style_text_color(hr_cal_readout, pip_primary(), 0);
    } else {
        // Distance guidance toward the 75-85 mm sweet spot. COLORBLIND-SAFE: the
        // WORD ("too close"/"in range"/"too far") is the primary cue -- it tells
        // the user which way to move without depending on hue. Color (bright
        // highlight when in-range vs normal otherwise) + luminance only reinforce.
        const int LO = 75, HI = 85;
        const char *state; lv_color_t col;
        if      (hr_cal_avg_mm < LO) { state = "too close"; col = pip_primary(); }
        else if (hr_cal_avg_mm > HI) { state = "too far";   col = pip_primary(); }
        else                         { state = "in range";  col = pip_highlight(); }
        snprintf(buf, sizeof(buf), "%d/64 zones\n%d mm  %s", zones, hr_cal_avg_mm, state);
        lv_obj_set_style_text_color(hr_cal_readout, col, 0);
    }
    lv_label_set_text(hr_cal_readout, buf);
}

// AIM phase: start the scanner ranging + the live readout + Ready button (built
// in hr_scan_begin_cb when hr_cal_aiming). No flat-field averaging happens yet.
static void hr_cal_begin_aim(void) {
    if (hr_scanning) return;
    hr_cal_aiming = true;
    hr_scan_start();
    Serial.println("[CAL] aiming -- position a flat surface, then tap Ready");
}

// Ready -> begin the ~5s flat-field capture (the scanner is already ranging).
static void hr_cal_do_capture(void) {
    if (!hr_cal_aiming) return;
    hr_cal_aiming = false;
    if (hr_cal_ready_btn) { lv_obj_delete(hr_cal_ready_btn); hr_cal_ready_btn = NULL; }
    if (hr_cal_readout) {
        // The aim readout was short ("N/64 zones") and auto-sized in the right
        // column. "Hold steady, calibrating..." is long -- auto-size would extend
        // it back across the screen, UNDER the left-column heatmap. Constrain to
        // the right pane width so it wraps and stays clear of the grid.
        lv_obj_set_width(hr_cal_readout, 150);
        lv_obj_set_style_text_align(hr_cal_readout, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(hr_cal_readout, LV_ALIGN_RIGHT_MID, -8, 44);
        lv_label_set_text(hr_cal_readout, "Hold steady,\ncalibrating...");
    }
    hr_scanner.beginCalCapture();
    hr_cal_capturing = true;
    hr_cal_start_ms  = millis();
    Serial.println("[CAL] capturing flat-field...");
}

// Direct capture with NO aim UI -- for the serial `hr_calibrate` cmd (a headless
// caller has no touch to tap Ready). begin() preserves the cal-capture flag set
// here even though hr_scan_start defers the scanner begin.
static void hr_cal_start(void) {
    if (hr_scanning) return;
    hr_cal_zones = 0;   // no aim reading on the serial path -> finalize uses engine mean
    hr_scan_start();
    hr_scanner.beginCalCapture();
    hr_cal_capturing = true;
    hr_cal_start_ms  = millis();
    Serial.println("[CAL] capturing flat-field (direct)...");
}

static void hr_cal_tick(void) {   // call every loop; drives aim readout + finalizes capture
    if (hr_cal_aiming)     { hr_cal_aim_update(); return; }
    if (!hr_cal_capturing) return;
    if (millis() - hr_cal_start_ms < 5000) return;   // LiDAR spin-up (~1.5-2s) + ~40 frames
    hr_cal_capturing = false;
    int16_t cal[64];
    int nz = hr_scanner.finishCalCapture(cal);
    char toast[64];
    if (hr_scanner.hasZoneCal()) {
        memcpy(hr_zone_cal, cal, sizeof(hr_zone_cal));
        hr_has_cal = true;
        // Keep the AIM reading (set live in hr_cal_aim_update -- cal-corrected +
        // filtered, tracks a ruler); fall back to the engine's raw window mean only
        // for the serial aim-less path (hr_cal_start reset hr_cal_zones to 0).
        if (hr_cal_zones <= 0) {
            hr_cal_zones  = hr_scanner.lastCalZones();
            hr_cal_avg_mm = hr_scanner.lastCalAvgMm();
        }
        cfg_prefs.begin(CFG_NAMESPACE, false);
        cfg_prefs.putBytes("zonecal", hr_zone_cal, sizeof(hr_zone_cal));
        cfg_prefs.putInt("calzn", hr_cal_zones);
        cfg_prefs.putInt("calavg", hr_cal_avg_mm);
        cfg_prefs.end();
        int lo = 32767, hi = -32768;
        for (int i = 0; i < 64; i++) { if (cal[i] < lo) lo = cal[i]; if (cal[i] > hi) hi = cal[i]; }
        Serial.printf("[CAL] saved: %d zones, per-zone offset range %d..%d mm\n", nz, lo, hi);
        snprintf(toast, sizeof(toast), "Calibrated\n%d zones", nz);
    } else {
        // Failed capture cleared the live cal -> restore the last good one.
        if (hr_has_cal) hr_scanner.setZoneCal(hr_zone_cal);
        Serial.printf("[CAL] FAILED - only %d/64 zones covered (need 30+); fill the FoV + hold steady, retry\n", nz);
        snprintf(toast, sizeof(toast), "Calibration failed\nfill the view, retry");
    }
    hr_scan_stop();
    hr_cal_toast(toast);
}

// Sensor Calibration modal (opened from Settings > SYSTEM). Sized generously so
// the Close button can't overflow, and it reports coverage (# zones + mean
// distance) so the user can confirm a calibration actually took.
static void show_sensor_cal_modal(void) {
    lv_obj_t *modal = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(modal);
    lv_obj_set_size(modal, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_80, 0);
    lv_obj_remove_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(modal, 0, 0);

    lv_obj_t *box = lv_obj_create(modal);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 312, 236);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, pip_bg(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, pip_highlight(), 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_pad_all(box, 10, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(box, 6, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    make_label(box, "SCANNER CALIBRATION", &ui_font_pipboy_18, pip_highlight());
    lv_obj_t *w = make_label(box,
        "Flattens the LiDAR so codes read clean.\n"
        "Hold a flat matte surface (the blank back\n"
        "of a tag) ~3 in (8 cm) away, filling the\n"
        "view, then Calibrate and hold ~5 seconds.",
        &ui_font_pipboy_14, pip_primary());
    lv_obj_set_style_text_align(w, LV_TEXT_ALIGN_CENTER, 0);

    char st[64];
    if (hr_has_cal && hr_cal_zones > 0)
        snprintf(st, sizeof(st), "Calibrated: %d/64 zones, avg %d mm", hr_cal_zones, hr_cal_avg_mm);
    else if (hr_has_cal)   // a pre-existing cal from before we tracked zones/avg
        snprintf(st, sizeof(st), "Calibrated (recalibrate for details)");
    else
        snprintf(st, sizeof(st), "Not calibrated");
    make_label(box, st, &ui_font_pipboy_14, hr_has_cal ? pip_highlight() : pip_disabled());

    lv_obj_t *row = lv_obj_create(box);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bc = lv_button_create(row);   // Calibrate
    lv_obj_set_size(bc, 92, 32);
    lv_obj_set_style_bg_color(bc, pip_highlight(), 0);
    lv_obj_t *lc = lv_label_create(bc);
    lv_label_set_text(lc, "Calibrate");
    lv_obj_set_style_text_font(lc, &ui_font_pipboy_14, 0);
    lv_obj_set_style_text_color(lc, pip_bg(), 0);
    lv_obj_center(lc);
    lv_obj_add_event_cb(bc, [](lv_event_t *e2) {
        lv_obj_t *b = (lv_obj_t *)lv_event_get_target(e2);
        lv_obj_t *modal = lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(b)));
        // DEFER: deleting this full-screen modal AND building the scanner overlay
        // in the same touch event skips the "Loading scanner..." paint -> the UI
        // looks frozen during the ~2s LiDAR spin-up. Tear the modal down and start
        // the aim scan on the next LVGL cycle so the loading banner renders first.
        lv_async_call([](void *m) {
            lv_obj_delete((lv_obj_t *)m);
            hr_cal_begin_aim();   // aim phase (live preview) -> tap Ready to capture
        }, modal);
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t *bx = lv_button_create(row);   // Clear
    lv_obj_set_size(bx, 92, 32);
    lv_obj_set_style_bg_color(bx, pip_border(), 0);
    lv_obj_t *lx = lv_label_create(bx);
    lv_label_set_text(lx, "Clear");
    lv_obj_set_style_text_font(lx, &ui_font_pipboy_14, 0);
    lv_obj_set_style_text_color(lx, pip_primary(), 0);
    lv_obj_center(lx);
    lv_obj_add_event_cb(bx, [](lv_event_t *e2) {
        lv_obj_t *b = (lv_obj_t *)lv_event_get_target(e2);
        lv_obj_delete(lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(b))));  // -> modal
        hr_cal_clear();
        hr_cal_toast("Calibration cleared");
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t *bcl = lv_button_create(box);  // Close (own row)
    lv_obj_set_size(bcl, 110, 32);
    lv_obj_set_style_bg_color(bcl, pip_border(), 0);
    lv_obj_t *lcl = lv_label_create(bcl);
    lv_label_set_text(lcl, "Close");
    lv_obj_set_style_text_font(lcl, &ui_font_pipboy_14, 0);
    lv_obj_set_style_text_color(lcl, pip_primary(), 0);
    lv_obj_center(lcl);
    lv_obj_add_event_cb(bcl, [](lv_event_t *e2) {
        lv_obj_t *b = (lv_obj_t *)lv_event_get_target(e2);
        lv_obj_delete(lv_obj_get_parent(lv_obj_get_parent(b)));  // button -> box -> modal
    }, LV_EVENT_CLICKED, NULL);
}

// Stop both LiDAR activities (HR scanner + theremin). They share the single
// VL53L5CX on Wire1, so any radio activity (tool/geiger) that starts must
// release the sensor cleanly first. Both calls self-guard -> safe when idle.
static void stop_lidar_activities(void) {
    hr_scan_stop();            // self-guards on hr_scanning
    theremin_release_sensor(); // self-guards on vl53_initialized
}

// Scan button callback
static uint32_t scan_lp_ms = 0;        // Scan-button press-start (press & hold -> manual entry)
static bool     scan_lp_fired = false;
static void hr_scan_btn_cb(lv_event_t *e) {
    (void)e;
    if (scan_lp_fired) { scan_lp_fired = false; return; }  // a hold just opened manual entry
    if (hr_scanning)
        hr_scan_stop();
    else
        hr_scan_start();
}

// Filter state for the collectibles list. When true, only items that
// have been collected appear -- shrinks the list so the user doesn't
// have to scroll past locked entries to get back to the Scan button.
static bool coll_filter_collected_only = false;

// Walk the left_pane looking for the list button whose user_data tag
// matches `coll_idx`. We tag each list button with (idx + 1) at build
// time so the tag stays non-zero (distinguishes from un-tagged children
// like the Scan/Filter row container).
static lv_obj_t *find_coll_btn(int coll_idx) {
    if (!left_pane) return nullptr;
    uint32_t cnt = lv_obj_get_child_count(left_pane);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *child = lv_obj_get_child(left_pane, (int32_t)i);
        if (!child) continue;
        intptr_t tag = (intptr_t)lv_obj_get_user_data(child);
        if (tag > 0 && (tag - 1) == coll_idx) return child;
    }
    return nullptr;
}

static void build_items_collectibles(lv_obj_t *cont) {
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_set_style_pad_gap(cont, 0, 0);

    left_pane = create_left_list(cont);

    // Scan + Filter on a single row at the top of the list.
    lv_obj_t *row = lv_obj_create(left_pane);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_gap(row, 4, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *scan_btn = make_action_btn(row,
                          hr_scanning ? "Stop" : "Scan",
                          hr_scan_btn_cb, NULL);
    lv_obj_set_flex_grow(scan_btn, 1);
    lv_obj_set_style_pad_hor(scan_btn, 4, 0);
    // Press & HOLD (~600ms) the Scan button -> straight to manual entry, skipping the
    // ~2s LIDAR spin-up. Custom timing via PRESSING+millis (LVGL's 400ms LONG_PRESSED is
    // too twitchy on a slow frame); the scan_lp_fired flag suppresses the tap-scan that
    // would otherwise fire on release (see hr_scan_btn_cb). Deferred open (UAF: build
    // overlay out of the touch event).
    lv_obj_add_event_cb(scan_btn, [](lv_event_t *e) {
        lv_event_code_t code = lv_event_get_code(e);
        if (code == LV_EVENT_PRESSED) { scan_lp_ms = millis(); scan_lp_fired = false; }
        else if (code == LV_EVENT_PRESSING) {
            if (!scan_lp_fired && !hr_scanning && millis() - scan_lp_ms >= 600) {
                scan_lp_fired = true;
                lv_async_call([](void *u) { (void)u; show_manual_grid(NULL, false); }, NULL);
            }
        }
    }, LV_EVENT_ALL, NULL);

    // Filter toggles between full list ("All") and collected-only ("Found").
    // Label reflects the *current* view.
    lv_obj_t *filter_btn = make_action_btn(row,
                            coll_filter_collected_only ? "Found" : "All",
                            [](lv_event_t *e) {
                                (void)e;
                                coll_filter_collected_only = !coll_filter_collected_only;
                                cfg_prefs.begin(CFG_NAMESPACE, false);   // handle is closed here
                                cfg_prefs.putBool("collfilt", coll_filter_collected_only);  // persist view
                                cfg_prefs.end();
                                rebuild_content();
                            }, NULL);
    lv_obj_set_flex_grow(filter_btn, 1);
    lv_obj_set_style_pad_hor(filter_btn, 4, 0);

    int firstVisible = -1;
    for (int i = 0; i < (int)coll_count; i++) {
        if (coll_filter_collected_only && !coll_items[i].collected) continue;
        // List labels drop any parenthetical so titles stay scannable when
        // scrolling -- the full title still shows in the detail pane.
        char label_buf[COLL_MAX_TITLE + 2];
        const char *label;
        if (coll_items[i].collected) {
            const char *paren = strchr(coll_items[i].title, '(');
            if (paren && paren > coll_items[i].title) {
                size_t pre = paren - coll_items[i].title;
                while (pre > 0 && coll_items[i].title[pre - 1] == ' ') pre--;
                size_t copy = pre < sizeof(label_buf) - 1
                              ? pre : sizeof(label_buf) - 1;
                memcpy(label_buf, coll_items[i].title, copy);
                label_buf[copy] = '\0';
                label = label_buf;
            } else {
                label = coll_items[i].title;
            }
        } else {
            label = "??? Locked";
        }
        lv_obj_t *btn = make_list_btn(left_pane, label,
                      list_item_tap_cb, (void *)(intptr_t)i);
        // Tag so find_coll_btn can locate this button by coll idx; +1 keeps
        // the tag non-zero so it's distinguishable from the row container.
        lv_obj_set_user_data(btn, (void *)(intptr_t)(i + 1));
        if (!coll_items[i].collected)
            lv_obj_set_style_text_color(btn, pip_dim(), 0);
        if (firstVisible < 0) firstVisible = i;
    }

    right_pane = create_right_detail(cont);

    // Restore selection: keep cur_sel if it's still visible, else first.
    int show_idx = -1;
    if (cur_sel >= 0 && cur_sel < (int)coll_count
        && (!coll_filter_collected_only || coll_items[cur_sel].collected)) {
        show_idx = cur_sel;
    } else if (firstVisible >= 0) {
        show_idx = firstVisible;
    }

    if (show_idx >= 0) {
        cur_sel = (int16_t)show_idx;
        lv_obj_t *btn = find_coll_btn(show_idx);
        if (btn) highlight_list_item(left_pane,
                                      (int8_t)lv_obj_get_index(btn));
        show_collectible_detail(show_idx);
    } else {
        make_label(right_pane, coll_filter_collected_only
                   ? "No collected items yet" : "No collectibles loaded",
                   &ui_font_pipboy_14, pip_dim());
    }
}

// ─────────────────────── DATA > LEDs ───────────────────────────────────────
// Split-pane: left=LED list, right=detail (individual controls or presets).
// Indices 0-7 → cfg.leds[0-7]. Index 8 = Front 5 easter egg. Index 9 = All LEDs (presets only).

static lv_obj_t *led_sl_bright = NULL;
static lv_obj_t *led_sl_r = NULL;
static lv_obj_t *led_sl_g = NULL;
static lv_obj_t *led_sl_b = NULL;

#define LED_SLIDER_W  105   // Fixed slider width - fits in 173px usable

// ── LED config debounce timer ──
#define LED_SAVE_DEBOUNCE_MS  2000
static lv_timer_t *led_save_timer = NULL;

static void led_save_timer_cb(lv_timer_t *t) {
    (void)t;
    cfg_save_leds();
    lv_timer_delete(led_save_timer);
    led_save_timer = NULL;
}

static void led_cfg_mark_dirty(void) {
    if (led_save_timer) {
        lv_timer_reset(led_save_timer);
    } else {
        led_save_timer = lv_timer_create(led_save_timer_cb, LED_SAVE_DEBOUNCE_MS, NULL);
        lv_timer_set_repeat_count(led_save_timer, 1);
    }
}

// ── Custom LED theme support ─────────────────────────────────────────────────
#define LED_THEME_CUSTOM 100
// Left-pane display order: "All LEDs" (id 9) on top, then the four fuses in order
// TL,BL,TR,BR (left column top->bottom, then right) -- ids 3,0,2,1 -- then the
// fronts in UI-number order Front 1,2,3,4 -- which by the reversed front mapping
// (see led_names) is ids 7,6,5,4 -- then Front 5 easter egg (8). Each item keeps its
// STABLE id (= physical LED index) so the detail builder / cfg.leds[] are unaffected.
static const uint8_t led_order[] = {9, 3, 0, 2, 1, 7, 6, 5, 4, 8};
static inline bool led_custom_active(void) { return cfg.led_theme == LED_THEME_CUSTOM; }
static void led_theme_select(int id);   // fwd (defined after build_data_leds)

// Mirror a per-LED edit into the saved Custom store, but only while Custom is the
// active theme (so editing a built-in preset never pollutes the Custom slot).
static void led_custom_sync(int idx) {
    if (led_custom_active() && idx >= 0 && idx < CFG_NUM_LEDS)
        cfg.custom_leds[idx] = cfg.leds[idx];
}

// Apply one of the All-LEDs presets to all 8 physical LEDs, persist,
// and push to hardware. Returns false if preset idx is out of range.
// Preset indices: 0=Mojave, 1=RibbitCity, 2=Flashbang, 3=Rainbow, 4=Off,
//                 5=Overseer (--rift), 6=Space Badge (--rift).
// A non-NULL per-LED palette (pr/pg/pb) overrides the solid base color; with
// anim=Chase the palette rotates around the ring, so multi-color palettes
// "chase each other". Presets 5/6 are compiled into BOTH builds; only the
// All-LEDs buttons that invoke them are gated to --rift.
// Fallout-flavored pace name for the All-LEDs chase speed (1=slowest .. 10=fastest).
static const char* alleds_speed_name(int v) {
    if (v <= 1) return "Severely Overburdened";  // slowest (10.0s)
    if (v <= 3) return "Encumbered";             // slow
    if (v <= 6) return "Leisurely Stroll";       // normal (default 5, ~6.4s)
    if (v <= 8) return "Fast Travel";            // fast
    return "Tunnel Runner";                      // fastest (2.0s)
}

static bool led_apply_preset(int pi) {
    static const uint8_t rb_r[] = {255,255,255,  0,  0, 75,148,255};
    static const uint8_t rb_g[] = {  0,127,255,255,  0,  0,  0,  0};
    static const uint8_t rb_b[] = {  0,  0,  0,  0,255,130,211, 60};
    // Overseer / Space Badge LED chase palettes (--rift, KS-exclusive). Real values
    // from rift_private.h; generic placeholders in public/non-rift builds. The
    // RIFT_LED_* macros are defined in ui_theme.h (included first). See that gate.
    static const uint8_t ov_r[] = { RIFT_LED_OV_R };
    static const uint8_t ov_g[] = { RIFT_LED_OV_G };
    static const uint8_t ov_b[] = { RIFT_LED_OV_B };
    static const uint8_t sb_r[] = { RIFT_LED_SB_R };
    static const uint8_t sb_g[] = { RIFT_LED_SB_G };
    static const uint8_t sb_b[] = { RIFT_LED_SB_B };
    struct P { uint8_t r,g,b,bright,anim,speed; const uint8_t *pr,*pg,*pb; };
    static const P presets[] = {
        {255,144,  0, 180, 2, 5, NULL, NULL, NULL},  // 0 Mojave
        { 32,255, 32, 180, 2, 5, NULL, NULL, NULL},  // 1 Ribbit City
        {255,255,255, 160, 2, 5, NULL, NULL, NULL},  // 2 Flashbang
        {  0,  0,  0, 200, 2, 5, rb_r, rb_g, rb_b},  // 3 Rainbow
        {  0,  0,  0,   0, 0, 5, NULL, NULL, NULL},  // 4 Off
        {  0,  0,  0, 190, 2, 4, ov_r, ov_g, ov_b},  // 5 Overseer
        {  0,  0,  0, 190, 2, 4, sb_r, sb_g, sb_b},  // 6 Space Badge
        { 46,242,255, 170, 1, 3, NULL, NULL, NULL},  // 7 Quanta: cyan slow breathe (reward glow)
    };
    if (pi < 0 || pi >= (int)(sizeof(presets)/sizeof(presets[0]))) return false;
    // Selecting a normal colour preset exits the Rubber Duck theme.
    if (neo_rubber_duck_active) {
        neo_rubber_duck_active = false;
        cfg.led_rubber_duck = false;
        cfg_save_led_rubber_duck();
    }
    const P &p = presets[pi];
    // Preserve the user's chosen chase pace (All-LEDs speed slider) across preset
    // changes - only colors/brightness/anim come from the preset, not the speed.
    uint8_t keep_speed = cfg.leds[0].speed;
    if (keep_speed < 1) keep_speed = p.speed;
    for (int j = 0; j < CFG_NUM_LEDS; j++) {
        if (p.pr) {
            cfg.leds[j].r = p.pr[j]; cfg.leds[j].g = p.pg[j]; cfg.leds[j].b = p.pb[j];
        } else {
            cfg.leds[j].r = p.r; cfg.leds[j].g = p.g; cfg.leds[j].b = p.b;
        }
        cfg.leds[j].brightness = p.bright;
        cfg.leds[j].animation  = p.anim;
        cfg.leds[j].speed      = (p.anim == 2) ? keep_speed : p.speed;
    }
    cfg.led_theme = (uint8_t)pi;   // record the active theme (gates per-LED editing)
    neo_apply_all();
    led_cfg_mark_dirty();
    return true;
}

// Enable the Rubber Duck LED theme (global chase effect; see neopixel_driver.h).
// Persisted so it survives a reboot like the other LED choices.
static void led_apply_rubber_duck(void) {
    neo_rd_pos = 0.0f;
    neo_rubber_duck_active = true;
    cfg.led_rubber_duck = true;
    cfg.led_theme = 99;
    cfg_save_led_rubber_duck();
    led_cfg_mark_dirty();
}

// ── Volume config debounce timer ──
#define VOL_SAVE_DEBOUNCE_MS  2000
static lv_timer_t *vol_save_timer = NULL;

static void vol_save_timer_cb(lv_timer_t *t) {
    (void)t;
    cfg_save_volume();
    lv_timer_delete(vol_save_timer);
    vol_save_timer = NULL;
}

static void vol_cfg_mark_dirty(void) {
    if (vol_save_timer) {
        lv_timer_reset(vol_save_timer);
    } else {
        vol_save_timer = lv_timer_create(vol_save_timer_cb, VOL_SAVE_DEBOUNCE_MS, NULL);
        lv_timer_set_repeat_count(vol_save_timer, 1);
    }
}

// Store slider values into cfg.leds[idx] and push to hardware
static void led_cfg_update_from_sliders(int idx) {
    if (idx < 0 || idx >= CFG_NUM_LEDS) return;
    LedConfig *lc = &cfg.leds[idx];
    if (led_sl_bright) lc->brightness = (uint8_t)lv_slider_get_value(led_sl_bright);
    if (led_sl_r)      lc->r = (uint8_t)lv_slider_get_value(led_sl_r);
    if (led_sl_g)      lc->g = (uint8_t)lv_slider_get_value(led_sl_g);
    if (led_sl_b)      lc->b = (uint8_t)lv_slider_get_value(led_sl_b);
    neo_apply(idx);
    led_custom_sync(idx);
    led_cfg_mark_dirty();
}

// Update brightness slider indicator color to match current RGB
static void led_update_bright_color(void) {
    if (!led_sl_bright || !led_sl_r || !led_sl_g || !led_sl_b) return;
    lv_obj_set_style_bg_color(led_sl_bright,
        lv_color_make(
            (uint8_t)lv_slider_get_value(led_sl_r),
            (uint8_t)lv_slider_get_value(led_sl_g),
            (uint8_t)lv_slider_get_value(led_sl_b)),
        LV_PART_INDICATOR);
}

// ── All-LEDs "Custom" master controls ────────────────────────────────────────
// One brightness + R/G/B set on the All-LEDs screen that writes to ALL 8 LEDs at
// once (per-LED tapping still works for fine-tuning; speed stays on the Chase Speed
// slider). Reuses the led_sl_* slider globals so led_update_bright_color() tints the
// brightness bar. Only meaningful under the Custom theme.
static void led_all_update_appearance(void) {
    uint8_t br = led_sl_bright ? (uint8_t)lv_slider_get_value(led_sl_bright) : 0;
    uint8_t r  = led_sl_r ? (uint8_t)lv_slider_get_value(led_sl_r) : 0;
    uint8_t g  = led_sl_g ? (uint8_t)lv_slider_get_value(led_sl_g) : 0;
    uint8_t b  = led_sl_b ? (uint8_t)lv_slider_get_value(led_sl_b) : 0;
    for (int j = 0; j < CFG_NUM_LEDS; j++) {
        cfg.leds[j].brightness = br;
        cfg.leds[j].r = r; cfg.leds[j].g = g; cfg.leds[j].b = b;
    }
    if (led_custom_active()) memcpy(cfg.custom_leds, cfg.leds, sizeof(cfg.custom_leds));
    neo_apply_all();
    led_cfg_mark_dirty();
}

// All-LEDs master animation dropdown: set every LED's animation at once.
static void led_all_anim_dd_cb(lv_event_t *e) {
    lv_obj_t *dd = (lv_obj_t *)lv_event_get_target(e);
    uint16_t sel = lv_dropdown_get_selected(dd);
    for (int j = 0; j < CFG_NUM_LEDS; j++) cfg.leds[j].animation = (uint8_t)sel;
    if (led_custom_active()) memcpy(cfg.custom_leds, cfg.leds, sizeof(cfg.custom_leds));
    neo_apply_all();
    led_cfg_mark_dirty();
}

// Helper: create a slider row [label] [slider] [value]
static lv_obj_t* make_rgb_slider(lv_obj_t *parent, const char *label_text,
                                 uint32_t indicator_color, uint8_t initial_val,
                                 bool is_color_channel = false) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_add_style(row, &style_container, 0);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row, 4, 0);
    lv_obj_set_style_pad_ver(row, 1, 0);

    lv_obj_t *lbl = make_label(row, label_text, &ui_font_pipboy_14, pip_primary());
    lv_obj_set_style_min_width(lbl, 12, 0);

    lv_obj_t *sl = lv_slider_create(row);
    lv_obj_set_width(sl, LED_SLIDER_W);
    lv_obj_set_height(sl, 8);
    lv_slider_set_range(sl, 0, 255);
    lv_slider_set_value(sl, initial_val, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, pip_border(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, lv_color_hex(indicator_color), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, pip_highlight(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(sl, 2, LV_PART_KNOB);

    char vbuf[4];
    snprintf(vbuf, sizeof(vbuf), "%d", initial_val);
    lv_obj_t *val = make_label(row, vbuf, &ui_font_pipboy_14, pip_dim());
    lv_obj_set_style_min_width(val, 32, 0);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_add_event_cb(sl, [](lv_event_t *e) {
        lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
        lv_obj_t *val_label = (lv_obj_t *)lv_event_get_user_data(e);
        lv_label_set_text_fmt(val_label, "%ld", (long)lv_slider_get_value(slider));
    }, LV_EVENT_VALUE_CHANGED, val);

    if (is_color_channel) {
        lv_obj_add_event_cb(sl, [](lv_event_t *e) {
            (void)e;
            led_update_bright_color();
        }, LV_EVENT_VALUE_CHANGED, NULL);
    }

    return sl;
}

// Animation dropdown callback
static void led_anim_dd_cb(lv_event_t *e) {
    lv_obj_t *dd = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *speed_row = (lv_obj_t *)lv_event_get_user_data(e);
    uint16_t sel = lv_dropdown_get_selected(dd);
    if (sel == 0)
        lv_obj_add_flag(speed_row, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_remove_flag(speed_row, LV_OBJ_FLAG_HIDDEN);
    if (cur_sel >= 0 && cur_sel < CFG_NUM_LEDS) {
        cfg.leds[cur_sel].animation = (uint8_t)sel;
        led_custom_sync(cur_sel);
        led_cfg_mark_dirty();
    }
}

// Build animation dropdown + speed slider for a single LED
static void make_led_animation(lv_obj_t *parent, int led_idx) {
    if (led_idx < 0 || led_idx >= CFG_NUM_LEDS) return;
    LedConfig *lc = &cfg.leds[led_idx];

    make_label(parent, "Animation", &ui_font_pipboy_14, pip_dim());

    // Speed row (built first for dropdown callback reference)
    lv_obj_t *speed_row = lv_obj_create(parent);
    lv_obj_remove_style_all(speed_row);
    lv_obj_add_style(speed_row, &style_container, 0);
    lv_obj_set_size(speed_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(speed_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(speed_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(speed_row, 4, 0);
    lv_obj_set_style_pad_ver(speed_row, 1, 0);
    if (lc->animation == 0)
        lv_obj_add_flag(speed_row, LV_OBJ_FLAG_HIDDEN);

    // Dropdown: None / Breathe / Chase
    lv_obj_t *dd = make_dropdown(parent, "None\nBreathe\nChase");
    lv_dropdown_set_selected(dd, lc->animation);
    lv_obj_add_event_cb(dd, led_anim_dd_cb, LV_EVENT_VALUE_CHANGED, speed_row);

    // Speed slider
    make_label(speed_row, "Speed", &ui_font_pipboy_14, pip_primary());
    lv_obj_t *sl = lv_slider_create(speed_row);
    lv_obj_set_width(sl, LED_SLIDER_W);
    lv_obj_set_height(sl, 8);
    lv_slider_set_range(sl, 1, 10);
    lv_slider_set_value(sl, lc->speed, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, pip_border(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, pip_primary(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, pip_highlight(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(sl, 2, LV_PART_KNOB);

    char sbuf[4];
    snprintf(sbuf, sizeof(sbuf), "%d", lc->speed);
    lv_obj_t *val = make_label(speed_row, sbuf, &ui_font_pipboy_14, pip_dim());
    lv_obj_set_style_min_width(val, 32, 0);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_add_event_cb(sl, [](lv_event_t *e) {
        lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
        lv_obj_t *val_label = (lv_obj_t *)lv_event_get_user_data(e);
        int32_t v = lv_slider_get_value(slider);
        lv_label_set_text_fmt(val_label, "%ld", (long)v);
        if (cur_sel >= 0 && cur_sel < CFG_NUM_LEDS) {
            cfg.leds[cur_sel].speed = (uint8_t)v;
            led_custom_sync(cur_sel);
            led_cfg_mark_dirty();
        }
    }, LV_EVENT_VALUE_CHANGED, val);
}

// ─── LED detail pane builder ────────────────────────────────────────────────

static void led_build_detail(int idx);  // forward decl

static void led_tap_cb(lv_event_t *e) {
    int32_t idx = (int32_t)(intptr_t)lv_event_get_user_data(e);   // stable id (not list pos)
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    highlight_list_item(lv_obj_get_parent(btn), (int8_t)lv_obj_get_index(btn));  // highlight by POSITION
    led_build_detail(idx);
}

static void led_build_detail(int idx) {
    if (!right_pane) return;
    clear_children(right_pane);
    lv_obj_scroll_to_y(right_pane, 0, LV_ANIM_OFF);
    cur_sel = (int16_t)idx;

    make_label(right_pane, led_names[idx], &ui_font_pipboy_18, pip_highlight());

    // ─── Front 5 easter egg (TNG: Chain of Command) ─────────────────
    if (idx == 8) {
        make_label(right_pane, "Brightness", &ui_font_pipboy_14, pip_disabled());
        lv_obj_t *sl1 = lv_slider_create(right_pane);
        lv_obj_set_width(sl1, lv_pct(90));
        lv_obj_set_height(sl1, 8);
        lv_obj_set_style_bg_color(sl1, pip_border(), LV_PART_MAIN);
        lv_obj_set_style_bg_color(sl1, pip_disabled(), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(sl1, pip_disabled(), LV_PART_KNOB);
        lv_obj_set_style_pad_all(sl1, 2, LV_PART_KNOB);
        lv_obj_add_state(sl1, LV_STATE_DISABLED);

        make_label(right_pane, "R", &ui_font_pipboy_14, pip_disabled());
        lv_obj_t *sl2 = lv_slider_create(right_pane);
        lv_obj_set_width(sl2, lv_pct(90));
        lv_obj_set_height(sl2, 8);
        lv_obj_set_style_bg_color(sl2, pip_border(), LV_PART_MAIN);
        lv_obj_set_style_bg_color(sl2, pip_disabled(), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(sl2, pip_disabled(), LV_PART_KNOB);
        lv_obj_set_style_pad_all(sl2, 2, LV_PART_KNOB);
        lv_obj_add_state(sl2, LV_STATE_DISABLED);

        lv_obj_t *sp = lv_obj_create(right_pane);
        lv_obj_remove_style_all(sp);
        lv_obj_set_size(sp, 1, 8);

        lv_obj_t *msg = make_label(right_pane,
            "THERE. ARE.\nFOUR. LIGHTS.",
            &ui_font_pipboy_18, pip_highlight());
        lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(msg, lv_pct(100));
        return;
    }

    // ─── All LEDs - chase-speed slider + colour presets ─────────────
    if (idx == (int)(NUM_LEDS - 1)) {
        // Theme selection: dropdown to the RIGHT of the "Theme" label (one row).
        // Selecting applies via led_theme_select() (which defers the page rebuild
        // through lv_async_call, so deleting this dropdown mid-event is UAF-safe).
        // Rift/ARG-reward themes appear only when available; led_theme_ids[] maps the
        // (conditional) row order back to the theme id the callback needs.
        lv_obj_t *theme_row = lv_obj_create(right_pane);
        lv_obj_remove_style_all(theme_row);
        lv_obj_set_width(theme_row, lv_pct(100));
        lv_obj_set_height(theme_row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(theme_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(theme_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(theme_row, LV_OBJ_FLAG_SCROLLABLE);
        make_label(theme_row, "Theme", &ui_font_pipboy_14, pip_dim());
        static int led_theme_ids[16];
        int nth = 0;
        String topts;
        auto add_theme = [&](const char *name, int id) {
            if (nth) topts += "\n";
            topts += name;
            if (nth < 16) led_theme_ids[nth] = id;
            nth++;
        };
        add_theme("Mojave",      0);
        add_theme("Ribbit City", 1);
        add_theme("Flashbang",   2);
        add_theme("Rainbow",     3);
        add_theme("Custom",      LED_THEME_CUSTOM);  // per-LED editable theme (enables Fuse/Front)
#ifdef BADGE_QUANTUM_RIFT
        add_theme("Overseer",    5);
        add_theme("Space Badge", 6);
#endif
        if (arg_duck_earned())   add_theme("Rubber Duck", 99);  // reward: all P3 dead-ends explored
        if (arg_quanta_earned()) add_theme("Quanta",      7);   // reward LED glow, gated on solve
        add_theme("Off",         4);                            // "Off" always sorts last
        lv_obj_t *theme_dd = make_dropdown(theme_row, topts.c_str());
        lv_obj_set_width(theme_dd, 120);                        // fixed so "Theme" shares the row
        for (int i = 0; i < nth && i < 16; i++)                 // preselect the active theme
            if (led_theme_ids[i] == (int)cfg.led_theme) { lv_dropdown_set_selected(theme_dd, i); break; }
        lv_obj_add_event_cb(theme_dd, [](lv_event_t *e) {
            lv_obj_t *dd = (lv_obj_t *)lv_event_get_target(e);
            int idx = (int)lv_dropdown_get_selected(dd);
            if (idx >= 0 && idx < 16) led_theme_select(led_theme_ids[idx]);
        }, LV_EVENT_VALUE_CHANGED, NULL);

        // Custom theme: one master set of appearance controls (brightness + R/G/B +
        // animation) that drives ALL 8 LEDs at once. Seeded from LED 0 as the
        // representative value; dragging overwrites every LED. Per-LED tapping stays
        // available for fine-tuning. (Speed is the Chase Speed slider below.)
        if (led_custom_active()) {
            make_label(right_pane, "Brightness", &ui_font_pipboy_14, pip_dim());
            led_sl_bright = make_rgb_slider(right_pane, " ", pal.primary, cfg.leds[0].brightness);
            led_sl_r = make_rgb_slider(right_pane, "R", 0xFF0000, cfg.leds[0].r, true);
            led_sl_g = make_rgb_slider(right_pane, "G", 0x00FF00, cfg.leds[0].g, true);
            led_sl_b = make_rgb_slider(right_pane, "B", 0x0000FF, cfg.leds[0].b, true);
            led_update_bright_color();
            auto all_cb = [](lv_event_t *e) { (void)e; led_all_update_appearance(); };
            lv_obj_add_event_cb(led_sl_bright, all_cb, LV_EVENT_VALUE_CHANGED, NULL);
            lv_obj_add_event_cb(led_sl_r, all_cb, LV_EVENT_VALUE_CHANGED, NULL);
            lv_obj_add_event_cb(led_sl_g, all_cb, LV_EVENT_VALUE_CHANGED, NULL);
            lv_obj_add_event_cb(led_sl_b, all_cb, LV_EVENT_VALUE_CHANGED, NULL);

            make_label(right_pane, "Animation", &ui_font_pipboy_14, pip_dim());
            lv_obj_t *all_anim = make_dropdown(right_pane, "None\nBreathe\nChase");
            lv_dropdown_set_selected(all_anim, cfg.leds[0].animation);
            lv_obj_add_event_cb(all_anim, led_all_anim_dd_cb, LV_EVENT_VALUE_CHANGED, NULL);
        }

        // Chase Speed: one slider drives all 8 LEDs' chase pace (read live by the
        // NeoPixel task). The Fallout-flavored value name sits UNDER the slider.
        make_label(right_pane, "Chase Speed", &ui_font_pipboy_14, pip_dim());
        uint8_t cur_spd = cfg.leds[0].speed;
        if (cur_spd < 1 || cur_spd > 10) cur_spd = 5;
        lv_obj_t *sl_wrap = lv_obj_create(right_pane);   // full-width row that centers the slider
        lv_obj_remove_style_all(sl_wrap);
        lv_obj_set_width(sl_wrap, lv_pct(100));
        lv_obj_set_flex_flow(sl_wrap, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(sl_wrap, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(sl_wrap, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_height(sl_wrap, 18);   // taller than the knob so it isn't clipped top/bottom
        lv_obj_t *spd_sl = lv_slider_create(sl_wrap);
        lv_obj_set_width(spd_sl, lv_pct(84));   // narrower than the wrapper; the knob clears the edges and the wrapper centers it
        lv_obj_set_height(spd_sl, 10);
        lv_slider_set_range(spd_sl, 1, 10);
        lv_slider_set_value(spd_sl, cur_spd, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(spd_sl, pip_border(), LV_PART_MAIN);
        lv_obj_set_style_bg_color(spd_sl, pip_primary(), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(spd_sl, pip_highlight(), LV_PART_KNOB);
        lv_obj_set_style_pad_all(spd_sl, 2, LV_PART_KNOB);
        // Flavor name UNDER the slider (created after it so flex lays it out below).
        lv_obj_t *spd_name = make_label(right_pane, alleds_speed_name(cur_spd),
                                        &ui_font_pipboy_16, pip_primary());
        lv_obj_set_width(spd_name, lv_pct(100));
        lv_label_set_long_mode(spd_name, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(spd_name, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_event_cb(spd_sl, [](lv_event_t *e) {
            lv_obj_t *sl = (lv_obj_t *)lv_event_get_target(e);
            lv_obj_t *name_lbl = (lv_obj_t *)lv_event_get_user_data(e);
            int v = lv_slider_get_value(sl);
            lv_label_set_text(name_lbl, alleds_speed_name(v));
            for (int j = 0; j < CFG_NUM_LEDS; j++) cfg.leds[j].speed = (uint8_t)v;
            if (led_custom_active())   // the one Custom edit-path that skipped the sync
                memcpy(cfg.custom_leds, cfg.leds, sizeof(cfg.custom_leds));
            led_cfg_mark_dirty();
        }, LV_EVENT_VALUE_CHANGED, spd_name);
        return;
    }

    // ─── Individual LED (idx 0-7): sliders + animation ──────────────
    if (idx < 0 || idx >= CFG_NUM_LEDS) return;
    // Per-LED editing is only live under the Custom theme; a built-in theme owns
    // all LEDs, so point the user to Custom instead of letting edits be ignored.
    if (!led_custom_active()) {
        lv_obj_t *m = make_label(right_pane,
            "Per-LED editing is part of the CUSTOM theme.\nPick \"All LEDs\" (top) > Custom, then come back.",
            &ui_font_pipboy_16, pip_dim());
        lv_obj_set_width(m, lv_pct(100));
        lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
        return;
    }
    LedConfig *lc = &cfg.leds[idx];

    make_label(right_pane, "Brightness", &ui_font_pipboy_14, pip_dim());
    led_sl_bright = make_rgb_slider(right_pane, " ", pal.primary, lc->brightness);
    led_sl_r = make_rgb_slider(right_pane, "R", 0xFF0000, lc->r, true);
    led_sl_g = make_rgb_slider(right_pane, "G", 0x00FF00, lc->g, true);
    led_sl_b = make_rgb_slider(right_pane, "B", 0x0000FF, lc->b, true);
    led_update_bright_color();

    auto cfg_cb = [](lv_event_t *e) { (void)e; led_cfg_update_from_sliders(cur_sel); };
    lv_obj_add_event_cb(led_sl_bright, cfg_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(led_sl_r, cfg_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(led_sl_g, cfg_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(led_sl_b, cfg_cb, LV_EVENT_VALUE_CHANGED, NULL);

    make_led_animation(right_pane, idx);
}

static void build_data_leds(lv_obj_t *cont) {
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_set_style_pad_gap(cont, 0, 0);

    bool custom = led_custom_active();
    left_pane = create_left_list(cont);
    // Display in led_order (All LEDs on top); each item keeps its stable id. Front 5
    // (id 8) is always dimmed; Fuse/Front (id 0-7) are dimmed unless Custom is active.
    for (int p = 0; p < (int)NUM_LEDS; p++) {
        int id = led_order[p];
        bool dim = (id == 8) || (id < CFG_NUM_LEDS && !custom);
        if (dim) make_list_btn_dim(left_pane, led_names[id], led_tap_cb, (void *)(intptr_t)id);
        else     make_list_btn    (left_pane, led_names[id], led_tap_cb, (void *)(intptr_t)id);
    }

    right_pane = create_right_detail(cont);
    // Default to All LEDs (id 9) on fresh entry; keep cur_sel if it's a valid LED id.
    int sel_id = (cur_sel >= 0 && cur_sel < (int)NUM_LEDS) ? cur_sel : 9;
    int sel_pos = 0;
    for (int p = 0; p < (int)NUM_LEDS; p++) if (led_order[p] == sel_id) { sel_pos = p; break; }
    highlight_list_item(left_pane, (int8_t)sel_pos);
    led_build_detail(sel_id);
}

// ── Custom-theme apply + theme-button dispatch (defined after build_data_leds so
// they can rebuild the LED page). led_theme_select() is forward-declared above. ──
static void led_apply_custom(void) {
    if (!cfg.custom_seeded) {                                  // first ever: seed from what's live
        memcpy(cfg.custom_leds, cfg.leds, sizeof(cfg.custom_leds));
        cfg.custom_seeded = true;
    } else {                                                   // restore the saved Custom look
        memcpy(cfg.leds, cfg.custom_leds, sizeof(cfg.leds));
    }
    if (neo_rubber_duck_active) {                              // leaving Rubber Duck
        neo_rubber_duck_active = false;
        cfg.led_rubber_duck = false;
        cfg_save_led_rubber_duck();
    }
    cfg.led_theme = LED_THEME_CUSTOM;
    neo_apply_all();
    led_cfg_mark_dirty();
}

static void led_theme_refresh_async(void *p) { (void)p; rebuild_content(); }  // rebuild LED page (enable/disable)

static void led_theme_select(int id) {
    if      (id == LED_THEME_CUSTOM) led_apply_custom();
    else if (id == 99)               led_apply_rubber_duck();
    else                             led_apply_preset(id);
    cur_sel = 9;                                   // land back on All LEDs after the switch
    lv_async_call(led_theme_refresh_async, nullptr);  // defer: don't delete the tapped button mid-event
}

// ─────────────────────── DATA > Settings ──────────────────────────────────

// Apply a theme index live: restyle, persist, rebuild the main screen.
// Returns false if idx is out of range or already active.
static bool ui_theme_switch_live(uint8_t idx) {
    if (idx > THEME_CUSTOM) return false;
    // Presets skip a no-op re-apply; the custom theme ALWAYS re-applies (its hue
    // may have changed even though the index is the same).
    if (idx == cur_theme_idx && idx != THEME_CUSTOM) return false;

    ui_theme_apply(idx);
    cfg.theme = idx;
    cfg_save_theme();

    uint8_t save_div = cur_div;
    uint8_t save_tab = cur_tab;

    if (status_timer) { lv_timer_delete(status_timer); status_timer = NULL; }
    if (cb_scan_timer) { lv_timer_delete(cb_scan_timer); cb_scan_timer = NULL; }
    lbl_sbat = NULL; lbl_swifi = NULL; lbl_stask = NULL; lbl_smem = NULL;
    btn_sflash = NULL; lbl_sflash = NULL; btn_shelp = NULL;
    theremin_vol_slider = NULL; settings_vol_slider = NULL;
    theremin_vol_label = NULL; settings_vol_label = NULL;
    memset(theremin_dist_labels,    0, sizeof(theremin_dist_labels));
    memset(theremin_freq_labels,    0, sizeof(theremin_freq_labels));
    memset(theremin_voice_bars,     0, sizeof(theremin_voice_bars));
    memset(theremin_voice_bar_wells,0, sizeof(theremin_voice_bar_wells));
    memset(theremin_voice_dd,       0, sizeof(theremin_voice_dd));
    theremin_enable_btn   = NULL;
    theremin_k_slider     = NULL;
    theremin_k_label      = NULL;
    theremin_agree_slider = NULL;
    theremin_agree_label  = NULL;
    cb_ap_list_area = NULL; cb_sta_list_area = NULL;

    // UAF audit (DC34): tear down subsystems whose globals/timers would outlive
    // scr_main on the theme-switch path. This path is reachable via the serial
    // `theme_set` command regardless of what's on screen, so a monitor/theremin
    // active during a theme switch would leave a 500ms/50ms timer writing to freed
    // widgets. rebuild_content() (the nav path) already does these; this one missed them.
    mon_popup_close();   // Wireshark monitor popup + its mon_timer (primary finding)
    radio_stop();        // SegFault-Tec FM scope timer (DC34-132) — same UAF class
    // DC34-155: an active HR scan's overlay objects (root_/heatmapBox_/cells_[])
    // are children of scr_main with no self-null; the loop keeps calling
    // hr_scanner.tick()->renderHeatmap() with hr_scanning still true, so freeing
    // scr_main here would leave them dangling -> writes to freed memory (trap #1).
    // rebuild_content() already stops the scan on the nav path; this one missed it.
    if (hr_scanning) hr_scan_stop();
    if (theremin_poll_timer) { lv_timer_delete(theremin_poll_timer); theremin_poll_timer = NULL; }
    if (vl53_initialized) {
        vl53_sensor.stopRanging();
        vl53_sensor.setPowerMode(SF_VL53L5CX_POWER_MODE::SLEEP);
        audio_theremin_stop();
        vl53_initialized = false;
    }

    crt_vroll_abort();
    crt_scanline_img = NULL;

    // reboot-UAF review: the saver overlay + the collectible rotate anim are
    // children/writers of scr_main. Dismiss/cancel BEFORE deleting it, or they
    // dangle -- the saver's pointers cause the "screen-dark wedge" when the ARG P5
    // finale flips to THEME_QUANTA while idled into the saver, and the snap-back
    // anim keeps writing a freed card.
    if (screensaver_active) screensaver_dismiss();
    coll_rot_reset();

    // Keep the OLD screen alive until the new one is loaded, then delete it.
    // Deleting scr_main here (while it's still the ACTIVE screen) trips LVGL's
    // "lv_obj_delete: the active screen was deleted" warning and leaves a transient
    // window with no valid active screen. create_main_screen() builds a fresh
    // scr_main and lv_screen_load()s it, so the old one is no longer active by the
    // time we free it -> no warning, no dangling active-screen pointer. The old
    // tree is inert during the rebuild: its timers were stopped and its global
    // widget pointers nulled above, so nothing writes to it. (No leak either way --
    // lv_obj_delete frees the whole old tree; this only fixes the ORDER.)
    lv_obj_t *old_scr = scr_main;
    scr_main = NULL;

    cur_div = save_div;
    cur_tab = save_tab;
    cur_sel = -1;

    create_main_screen();               // builds new scr_main + lv_screen_load()
    if (old_scr) lv_obj_delete(old_scr); // now inactive -> clean free, no warning

    // Restart the scan poller if a scan is STILL RUNNING. It was torn down above (its widgets
    // were about to be freed, correctly) but nothing recreated it, so changing theme mid-scan
    // left the status bar permanently silent while the scan carried on -- the Scan/Stop button
    // still read "Stop" and the list kept filling, so the badge was under-reporting rather than
    // lying, but the count never came back for the rest of that scan. Owner reproduced it on
    // hardware (M3c). This was previously recorded as known-and-unfixed; it is a two-line fix
    // here because the poller's only widgets (lbl_stask, wifi_status_label) are freshly built by
    // create_main_screen() above and both self-null on delete.
    // Guarded on the LIBRARY's own view as well as the UI flag: cb_op_running alone would also
    // be true for a non-scanning tool, and starting a scan poller for one of those would put a
    // scan-shaped metric on screen for a tool that has none.
    if (cb_op_running && cb.isScanning()) cb_start_scan_polling();

    CB_LOGF("[THEME] Switched to '%s'. Free heap: %lu\n",
                  pal.name, (unsigned long)esp_get_free_heap_size());
    return true;
}

// Theme dropdown slot -> theme_presets[] index. Lets the dropdown listing differ
// from theme indices (rift colorways gated; Quanta appended only once earned).
static uint8_t theme_dd_map[8] = {0,1,2,3,4,5,6,7};
static uint8_t theme_dd_count = 3;

static void theme_changed_cb(lv_event_t *e) {
    lv_obj_t *dd = (lv_obj_t *)lv_event_get_target(e);
    uint16_t sel = lv_dropdown_get_selected(dd);
    if (sel >= theme_dd_count) return;
    uint8_t idx = theme_dd_map[sel];
    if (idx == THEME_CUSTOM && !coll_all_found()) {
        // Locked: revert the dropdown to the current theme and tell them how far.
        uint16_t cur_slot = 0;
        for (uint8_t i = 0; i < theme_dd_count; i++) if (theme_dd_map[i] == cur_theme_idx) cur_slot = i;
        lv_dropdown_set_selected(dd, cur_slot);
        int togo = (int)coll_count - coll_count_found(); if (togo < 1) togo = 1;
        char buf[96];
        snprintf(buf, sizeof(buf),
                 "The Custom theme unlocks when you've found every collectible.\n\n%d to go.", togo);
        coll_msg_modal("LOCKED", buf);
        return;
    }
    ui_theme_switch_live(idx);
}

// ─── Completionist custom theme: apply + hue picker + 100% reveal ─────────────
// Apply the custom theme at `hue`, persist, and rebuild the screen. Callers
// inside a touch event MUST defer (rebuild-in-event = UAF) -- use huepick_apply_async.
static void ui_theme_apply_custom(uint16_t hue) {
    g_custom_hue = (hue > 359) ? (uint16_t)(hue % 360) : hue;
    cfg.custom_hue = g_custom_hue;
    ui_theme_switch_live(THEME_CUSTOM);   // re-derives from g_custom_hue + rebuilds
    cfg_save_custom_theme();
}

static lv_obj_t *huepick_modal  = nullptr;
static lv_obj_t *huepick_sample = nullptr;
static lv_obj_t *huepick_sw[4]  = { nullptr, nullptr, nullptr, nullptr };
static uint16_t  huepick_hue    = 190;

static void huepick_refresh_preview(void) {
    float h = (float)huepick_hue;
    uint32_t cols[4] = {
        theme_floor_luma(theme_hsv2rgb(h, 0.80f, 1.0f), 0.42f),   // primary (also sample text)
        theme_floor_luma(theme_hsv2rgb(h, 0.50f, 1.0f), 0.62f),   // highlight
        theme_hsv2rgb(h, 1.0f, 1.0f),                             // accent
        theme_hsv2rgb(h, 1.0f, 0.50f),                            // dim
    };
    for (int i = 0; i < 4; i++)
        if (huepick_sw[i]) lv_obj_set_style_bg_color(huepick_sw[i], lv_color_hex(cols[i]), 0);
    if (huepick_sample) lv_obj_set_style_text_color(huepick_sample, lv_color_hex(cols[0]), 0);
}
static void huepick_close(void) {
    if (huepick_modal) { lv_obj_delete(huepick_modal); huepick_modal = nullptr; }
    huepick_sample = nullptr;
    for (int i = 0; i < 4; i++) huepick_sw[i] = nullptr;
}
static void huepick_apply_async(void *p) { (void)p; ui_theme_apply_custom(huepick_hue); }

static void show_hue_picker(void) {
    huepick_close();
    huepick_hue = cfg.custom_hue;

    huepick_modal = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(huepick_modal);
    lv_obj_set_size(huepick_modal, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(huepick_modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(huepick_modal, LV_OPA_COVER, 0);
    lv_obj_remove_flag(huepick_modal, LV_OBJ_FLAG_SCROLLABLE);
    // Self-null if a screen rebuild (theme switch / nav) deletes it out from under
    // us -- else the next open deletes a dangling pointer (UAF). Same trap class.
    lv_obj_add_event_cb(huepick_modal, cb_selfnull_on_delete, LV_EVENT_DELETE, &huepick_modal);

    lv_obj_t *box = lv_obj_create(huepick_modal);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 300, 226);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, lv_color_black(), 0);   // preview reads on true black
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    // Chrome uses FIXED light colors (not theme colors): the box is forced black
    // for the preview, and a light theme like Flashbang would render its own
    // black-on-white chrome invisible on it.
    lv_obj_set_style_border_color(box, lv_color_hex(0xC0C0C0), 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_pad_all(box, 8, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 6, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    make_label(box, "DESIGN YOUR THEME", &ui_font_pipboy_16, lv_color_hex(0xE0E0E0));
    huepick_sample = make_label(box, "CLIP-BOY 3000", &ui_font_pipboy_18, pip_primary());  // recolored live

    // Live swatches: primary / highlight / accent / dim.
    lv_obj_t *sw_row = lv_obj_create(box);
    lv_obj_remove_style_all(sw_row);
    lv_obj_set_width(sw_row, lv_pct(100));
    lv_obj_set_height(sw_row, 20);
    lv_obj_set_flex_flow(sw_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sw_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(sw_row, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 4; i++) {
        lv_obj_t *s = lv_obj_create(sw_row);
        lv_obj_remove_style_all(s);
        lv_obj_set_size(s, 58, 18);
        lv_obj_set_style_radius(s, 2, 0);
        lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
        huepick_sw[i] = s;
    }

    // Hue slider 0-359, live-updates the preview.
    lv_obj_t *sl = lv_slider_create(box);
    lv_obj_set_width(sl, lv_pct(100));
    lv_obj_set_height(sl, 14);
    lv_obj_set_style_bg_color(sl, lv_color_hex(0x404040), LV_PART_MAIN);        // fixed: visible on black
    lv_obj_set_style_bg_color(sl, lv_color_hex(0x808080), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, lv_color_hex(0xF0F0F0), LV_PART_KNOB);
    lv_slider_set_range(sl, 0, 359);
    lv_slider_set_value(sl, huepick_hue, LV_ANIM_OFF);
    lv_obj_add_event_cb(sl, [](lv_event_t *e) {
        lv_obj_t *s = (lv_obj_t *)lv_event_get_target(e);
        huepick_hue = (uint16_t)lv_slider_get_value(s);
        huepick_refresh_preview();
    }, LV_EVENT_VALUE_CHANGED, NULL);

    // Buttons: Cancel | Apply.
    lv_obj_t *brow = lv_obj_create(box);
    lv_obj_remove_style_all(brow);
    lv_obj_set_width(brow, lv_pct(100));
    lv_obj_set_height(brow, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(brow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(brow, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(brow, LV_OBJ_FLAG_SCROLLABLE);
    auto mkb = [&](const char *txt, lv_event_cb_t cb) {
        lv_obj_t *b = lv_button_create(brow);
        lv_obj_set_size(b, 120, 32);
        lv_obj_set_style_bg_color(b, lv_color_hex(0xE0E0E0), 0);   // fixed light button (theme-independent)
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_font(l, &ui_font_pipboy_16, 0);
        lv_obj_set_style_text_color(l, lv_color_black(), 0);
        lv_obj_center(l);
    };
    mkb("Cancel", [](lv_event_t *e) { (void)e; huepick_close(); });
    mkb("Apply",  [](lv_event_t *e) { (void)e; huepick_close(); lv_async_call(huepick_apply_async, nullptr); });

    huepick_refresh_preview();
    crt_scanlines_raise();
}

// The 100%-collectibles reveal: full-screen celebration -> "Make It Yours" opens
// the hue picker. Fires once (cfg.custom_unlock_seen), ~1.5s after the found modal.
static lv_obj_t *custom_reveal_modal = nullptr;
static void custom_reveal_close(void) {
    if (custom_reveal_modal) { lv_obj_delete(custom_reveal_modal); custom_reveal_modal = nullptr; }
}
static void custom_reveal_open_picker_async(void *p) { (void)p; show_hue_picker(); }

static void show_custom_reveal(void) {
    custom_reveal_close();
    custom_reveal_modal = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(custom_reveal_modal);
    lv_obj_set_size(custom_reveal_modal, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(custom_reveal_modal, pip_bg(), 0);
    lv_obj_set_style_bg_opa(custom_reveal_modal, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(custom_reveal_modal, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(custom_reveal_modal, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(custom_reveal_modal, 14, 0);
    lv_obj_set_style_pad_row(custom_reveal_modal, 8, 0);
    lv_obj_remove_flag(custom_reveal_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(custom_reveal_modal, cb_selfnull_on_delete, LV_EVENT_DELETE, &custom_reveal_modal);

    make_label(custom_reveal_modal, "COLLECTION COMPLETE", &ui_font_pipboy_20, pip_highlight());
    lv_obj_t *body = make_label(custom_reveal_modal,
        "You found every last one.\nDesign your own Clip-Boy theme -\npick a color, the rest follows.",
        &ui_font_pipboy_16, pip_primary());
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(body, lv_pct(90));

    lv_obj_t *btn = lv_button_create(custom_reveal_modal);
    lv_obj_set_size(btn, 200, 38);
    lv_obj_set_style_bg_color(btn, pip_highlight(), 0);
    lv_obj_t *bl = make_label(btn, "Make It Yours", &ui_font_pipboy_18, pip_bg());
    lv_obj_center(bl);
    lv_obj_add_event_cb(btn, [](lv_event_t *e) {
        (void)e;
        custom_reveal_close();
        lv_async_call(custom_reveal_open_picker_async, nullptr);   // defer: opens the picker cleanly
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t *later = lv_button_create(custom_reveal_modal);
    lv_obj_remove_style_all(later);
    lv_obj_t *ll = make_label(later, "Later (find it in Settings)", &ui_font_pipboy_14, pip_dim());
    lv_obj_center(ll);
    lv_obj_add_event_cb(later, [](lv_event_t *e) { (void)e; custom_reveal_close(); }, LV_EVENT_CLICKED, NULL);

    crt_scanlines_raise();
}

// Trigger: fire the reveal once, the first time the collection hits 100%.
static void custom_check_reveal(void) {
    if (cfg.custom_unlock_seen || !coll_all_found()) return;
    cfg.custom_unlock_seen = true;
    cfg_save_custom_theme();
    show_custom_reveal();
}

// Brightness slider callback - applies to display and saves to config
static void brightness_slider_cb(lv_event_t *e) {
    lv_obj_t *sl = (lv_obj_t *)lv_event_get_target(e);
    int32_t val = lv_slider_get_value(sl);
    cfg.brightness = (uint8_t)val;
    lcd_set_brightness((uint8_t)(val * 255 / 100));
    cfg_save_brightness();
    lv_obj_t *lbl = (lv_obj_t *)lv_event_get_user_data(e);
    if (lbl) lv_label_set_text_fmt(lbl, "%ld%%", (long)val);
}

// Display timeout dropdown callback
static void disp_off_cb(lv_event_t *e) {
    lv_obj_t *dd = (lv_obj_t *)lv_event_get_target(e);
    cfg.disp_off = (uint8_t)lv_dropdown_get_selected(dd);
    cfg_save_disp_off();
    ss_settings_update_enables();
    CB_LOGF("[CFG] Display timeout: %d\n", cfg.disp_off);
}

// Tool "terminal"/scan-log font size. The setting lives on the Settings screen,
// so the Tools log is never visible here -- just persist it; cb_create_output_area
// reads term_font() when the Tools output is (re)built. (Do NOT poke
// cb_output_log from here: leaving Tools for Settings destroyed that label.)
static void term_font_cb(lv_event_t *e) {
    lv_obj_t *dd = (lv_obj_t *)lv_event_get_target(e);
    cfg.term_font = (uint8_t)lv_dropdown_get_selected(dd);
    cfg_save_term_font();
    CB_LOGF("[CFG] Terminal font: %d\n", cfg.term_font);
}

// Radio tuning-drift toggle (DATA▸Settings). 0=Disabled 1=Once Per Boot 2=Every
// Access. Disabled is the accessibility off-switch (named in Help).
static void radio_drift_cb(lv_event_t *e) {
    lv_obj_t *dd = (lv_obj_t *)lv_event_get_target(e);
    cfg.radio_drift = (uint8_t)lv_dropdown_get_selected(dd);
    cfg_save_radio_drift();
    Serial.printf("[CFG] Radio tuning drift: %d\n", cfg.radio_drift);
}

// Airplane mode switch callback
//
// Stops any active scan (cb_stop_operation handles both WiFi and BT paths via
// cb.stopScan()).
//
// ⚠ It does NOT power the WiFi radio down, despite the WiFi.mode(WIFI_OFF) below. The ClipBoy
// integration wraps the teardown entry points as unconditional no-ops --
// `__wrap_esp_wifi_stop(void) { return ESP_OK; }` (ClipBoyMarauder.cpp:42), same for
// esp_wifi_deinit / esp_wifi_restore / esp_netif_deinit -- and __wrap_esp_wifi_set_mode
// no-ops once _cb_wifi_hw_up. So airplane mode is a POLICY GATE enforced by the callers that
// check cfg.airplane, not a hardware power-down. Anything that reaches the IDF sniffer
// directly bypasses it (that was F7: the Geiger did exactly this). If you add a radio feature,
// gate it explicitly -- do not assume this switch has made the hardware safe.
//
// The NimBLE controller stays initialized
// because the ClipBoy library forbids deinit (races with the host task on
// core 0). With tool_tap_cb gating radio tools, the BLE radio is idle --
// no scans or advertising can launch while airplane mode is on.
// Apply an airplane-mode change: persist it and run the teardown. Shared by the UI switch and
// the test harness's `cfg_set airplane`, which previously wrote cfg.airplane and saved it
// WITHOUT any of the teardown below -- so every scripted airplane test was fictional: nothing
// was stopped, and a test asserting "the Geiger shut down when airplane came on" was really
// asserting nothing at all. (That is exactly how F3's first test version passed its own
// pre-condition and then failed against a correct fix.) One function, both callers.
static void airplane_apply(bool on) {
    cfg.airplane = on;
    cfg_save_airplane();
    if (cfg.airplane) {
        cb_stop_operation();   // also routes through rad_geiger_force_stop() for the Geiger
        // UNCONDITIONAL radio teardown -- do NOT rely on our own bookkeeping here.
        // cb_stop_operation() opens with `if (cb_op_running)`, and cb_op_running is set only by
        // the UI's own start paths. A scan started from the MARAUDER SERIAL CLI (`scanap`) never
        // sets it, so the entire teardown above -- including FB1's promiscuous clear -- no-ops,
        // and WiFi.mode(WIFI_OFF) below is "a policy gate, not a power-down" which does not stop
        // the library's scan either.
        // MEASURED on hardware 2026-07-26, from a clean boot: `cli scanap` -> lib_scanning=True,
        // promisc=True; then Airplane ON -> STILL lib_scanning=True, promisc=True. So the badge
        // sat in promiscuous RX with the Airplane switch reading engaged. Same class as F7 (the
        // Geiger bypassing the gate) one layer further out, and with the same legal framing: the
        // switch is a promise about the radio, not about the UI's records of the radio.
        // Ask the LIBRARY whether it is scanning, and clear the IDF filter regardless. Both are
        // stop operations, so doing them when nothing is running is a no-op.
        // Order matches FB1's teardown: callback first, then the filter, so no frame can arrive
        // between the two and land in a handler whose consumer is already gone.
        if (cb.isScanning()) cb.stopScan();
        esp_wifi_set_promiscuous_rx_cb(NULL);
        esp_wifi_set_promiscuous(false);
        WiFi.mode(WIFI_OFF);   // NOTE: a policy gate, not a power-down -- see the block above
        CB_LOGLN("[CFG] Airplane ON - WiFi & BT scans disabled");
    } else {
        // WiFi stays off until a scan is started (saves power)
        CB_LOGLN("[CFG] Airplane OFF - WiFi/BT available on demand");
    }
}

static void airplane_cb(lv_event_t *e) {
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    airplane_apply(lv_obj_has_state(sw, LV_STATE_CHECKED));
}

// Mute switch callback (checked = muted)
static void sound_cb(lv_event_t *e) {
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    cfg.sound = !lv_obj_has_state(sw, LV_STATE_CHECKED);
    cfg_save_sound();
    audio_set_volume(cfg.sound ? (cfg.volume / 100.0f) : 0.0f);
    CB_LOGF("[CFG] Mute: %d\n", !cfg.sound);
}

// Volume slider callback - updates shared global, saves config, cross-updates other slider + audio
static void volume_slider_cb(lv_event_t *e) {
    lv_obj_t *sl = (lv_obj_t *)lv_event_get_target(e);
    int32_t val = lv_slider_get_value(sl);
    theremin_volume = (uint8_t)val;
    cfg.volume = (uint8_t)val;
    vol_cfg_mark_dirty();

    // Push to audio hardware. The I2S-level set affects everything
    // (MP3, geiger, theremin); setMasterVolume on top updates the
    // theremin's own gain so live slider moves take effect mid-render
    // instead of waiting for the next aud_theremin.begin().
    audio_set_volume(val / 100.0f);
    audio_theremin_set_volume(val / 100.0f);

    // Update whichever label is associated with this slider
    lv_obj_t *lbl = (lv_obj_t *)lv_event_get_user_data(e);
    if (lbl) lv_label_set_text_fmt(lbl, "%ld%%", (long)val);

    // Cross-update the other slider if it exists
    if (sl == settings_vol_slider && theremin_vol_slider) {
        lv_slider_set_value(theremin_vol_slider, val, LV_ANIM_OFF);
        if (theremin_vol_label) lv_label_set_text_fmt(theremin_vol_label, "%d%%", val);
    } else if (sl == theremin_vol_slider && settings_vol_slider) {
        lv_slider_set_value(settings_vol_slider, val, LV_ANIM_OFF);
        if (settings_vol_label) lv_label_set_text_fmt(settings_vol_label, "%d%%", val);
    }
}

// ─── Screen saver system ─────────────────────────────────────────────────────

static void screensaver_dismiss(void) {
    if (screensaver_overlay) {
        lv_obj_delete(screensaver_overlay);
        screensaver_overlay = NULL;
        screensaver_bar = NULL;
        screensaver_lbl = NULL;
    }
    if (screensaver_dimmed || screensaver_active) {
        lcd_set_brightness((uint8_t)(cfg.brightness * 255 / 100));
        lcd_wakeup();
    }
    // Restore LEDs if they were turned off (idle-saver setting OR Dark Charge)
    if (screensaver_active && (cfg.ss_leds_off || dark_charge_active)) {
        neo_suspend_for_ss = false;
        neo_apply_all();
    }
    dark_charge_active = false;
    screensaver_active = false;
    screensaver_dimmed = false;
    screensaver_hold_start = 0;
    CB_LOGLN("[SS] Screen saver dismissed");
    radio_reminder_on_wake();   // show a reminder that was suppressed under the saver
}

// Respawn a clippy off the right edge (or scatter across screen on first show).
static void clippy_place(int i, bool scatter) {
    int h = clippy_up_img[clippy_sz[i]]->header.h;
    int band = SCREEN_H - h; if (band < 1) band = 1;
    clippy_px[i] = scatter ? (float)(esp_random() % SCREEN_W)
                           : (float)(SCREEN_W + (int)(esp_random() % 48));
    clippy_py[i] = (float)(esp_random() % band);
}

// One animation tick: drift each clippy up-left at 5.25 deg, flap, wrap around.
static void clippy_anim_cb(lv_timer_t *t) {
    (void)t;
    for (int i = 0; i < CLIPPY_N; i++) {
        if (!clippy_spr[i]) continue;
        uint8_t sz = clippy_sz[i];
        float sp = clippy_speed[sz];
        clippy_px[i] -= sp * CLIPPY_COS;   // left
        clippy_py[i] -= sp * CLIPPY_SIN;   // ...and slightly up
        const lv_image_dsc_t *d = clippy_up_img[sz];
        if (clippy_px[i] < -(float)d->header.w || clippy_py[i] < -(float)d->header.h)
            clippy_place(i, false);
        lv_obj_set_pos(clippy_spr[i], (int)clippy_px[i], (int)clippy_py[i]);
        // Flap: toggle wing frame every 4 ticks; per-sprite phase avoids unison.
        uint8_t want = (uint8_t)((++clippy_flap[i] >> 2) & 1);
        if (want != clippy_fr[i]) {
            clippy_fr[i] = want;
            lv_image_set_src(clippy_spr[i], want ? clippy_up_img[sz] : clippy_down_img[sz]);
        }
    }
}

// Single teardown point: fires on EVERY overlay-delete path (dismiss, nav
// rebuild, ui_theme_switch_live / serial theme_set) so the timer can never
// outlive the sprites it writes to -- the project's LVGL-timer UAF trap.
static void clippy_cleanup_cb(lv_event_t *e) {
    (void)e;
    if (clippy_timer) { lv_timer_delete(clippy_timer); clippy_timer = NULL; }
    for (int i = 0; i < CLIPPY_N; i++) clippy_spr[i] = NULL;
}

static void screensaver_activate(void) {
    if (screensaver_active) return;
    // Flying-Clippy is an ARG reward; if progress was reset, fall back to Clip-Boy.
    if (cfg.ss_style == 2 && !arg_quanta_earned()) cfg.ss_style = 0;
    screensaver_active = true;
    screensaver_dimmed = false;
    // reboot-UAF review: silence a playing radio stream (or tuning static) during
    // idle, so the tap-and-hold unlock tone -- lowest priority in the audio chain --
    // isn't masked, and core-0 isn't decoding a multi-minute bed during a low-power
    // idle. The scope-tick poll is gated on !screensaver_active so nothing restarts
    // until dismiss.
    audio_mp3_stream_stop();
    audio_static_stop();
    // FB14: close the live-devices table. Its 500 ms mon_timer kept appending rows -- and
    // forcing a whole-tree relayout via mon_select's lv_obj_scroll_to_view -- with the screen
    // dark and the badge in a pocket, which is both the worst case for the row growth AND
    // pointless work during an idle that exists to save power. Also removes a modal the user
    // cannot see, so the unlock tap lands on the screensaver overlay as intended.
    mon_popup_close();
    // Clip-Boy style keeps a dim backlight to show the image; Blank turns off
    lcd_set_brightness(cfg.ss_style != 1 ? (uint8_t)(cfg.ss_brightness * 255 / 100) : 0);
    // Turn off NeoPixels if configured. Just raise the flag -- the core-0 task
    // does the actual off (avoids the cross-core show() race; see neo tick).
    if (cfg.ss_leds_off) {
        neo_suspend_for_ss = true;
    }

    screensaver_overlay = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(screensaver_overlay);
    lv_obj_set_size(screensaver_overlay, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(screensaver_overlay, 0, 0);
    lv_obj_set_style_bg_color(screensaver_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screensaver_overlay, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screensaver_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(screensaver_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(screensaver_overlay, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(screensaver_overlay, 8, 0);

    // Clip-Boy screensaver: show themed mascot image (A8 alpha mask)
    if (cfg.ss_style == 0) {
        lv_obj_t *img = lv_image_create(screensaver_overlay);
        lv_image_set_src(img, mascot_image());
        lv_obj_set_style_image_recolor(img, pip_primary(), 0);
        lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    }
    // Flying-Clippy screensaver: a parallax swarm drifting up-left, wings flapping.
    // Sprites are IGNORE_LAYOUT so the flex centering (label+bar) leaves them where
    // we place them; created before the label/bar so they sit behind. The anim
    // timer + cleanup-on-delete are wired here (teardown via clippy_cleanup_cb).
    else if (cfg.ss_style == 2) {
        static const uint8_t szmap[CLIPPY_N] = { 0, 0, 1, 1, 1, 2, 2 };
        for (int i = 0; i < CLIPPY_N; i++) {
            clippy_sz[i]   = szmap[i];
            clippy_fr[i]   = 0;
            clippy_flap[i] = (uint8_t)(esp_random() % 8);
            lv_obj_t *s = lv_image_create(screensaver_overlay);
            lv_obj_add_flag(s, LV_OBJ_FLAG_IGNORE_LAYOUT);
            lv_image_set_src(s, clippy_down_img[szmap[i]]);
            lv_obj_set_style_image_recolor(s, pip_primary(), 0);     // match UI theme
            lv_obj_set_style_image_recolor_opa(s, LV_OPA_COVER, 0);
            clippy_spr[i] = s;
            clippy_place(i, true);
            lv_obj_set_pos(s, (int)clippy_px[i], (int)clippy_py[i]);
        }
        lv_obj_add_event_cb(screensaver_overlay, clippy_cleanup_cb, LV_EVENT_DELETE, NULL);
        clippy_timer = lv_timer_create(clippy_anim_cb, 70, NULL);
    }

    screensaver_lbl = lv_label_create(screensaver_overlay);
    lv_label_set_text(screensaver_lbl, "Hold to unlock");
    lv_obj_set_style_text_font(screensaver_lbl, &ui_font_pipboy_18, 0);
    lv_obj_set_style_text_color(screensaver_lbl, pip_primary(), 0);

    screensaver_bar = lv_bar_create(screensaver_overlay);
    // FB7 (audit 2026-07-24): lv_obj grants LV_OBJ_FLAG_CLICKABLE at construction and
    // lv_bar_constructor only removes CHECKABLE/SCROLLABLE -- unlike lv_label/lv_image, which
    // DO drop CLICKABLE. So this 200x12 bar TERMINATED the press: the handlers live on the
    // overlay (PRESSED/PRESSING/RELEASED) and nothing sets EVENT_BUBBLE, so a touch landing on
    // the bar produced no backlight bump, no rising tone, no fill and no unlock -- on the only
    // gesture that wakes the badge. Worse in the SHIPPING default (ss_style 0, the mascot):
    // the 153x192 mascot pushes flex content to 243 px > 240, so start_pos goes negative and
    // the bar sits at the very bottom edge -- exactly where a thumb lands picking the badge
    // up, and what Dark Charge inherits. Reads as "bricked", then a power-button hold.
    lv_obj_remove_flag(screensaver_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(screensaver_bar, 200, 12);
    lv_bar_set_range(screensaver_bar, 0, SCREENSAVER_HOLD_MS);
    lv_bar_set_value(screensaver_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(screensaver_bar, pip_border(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(screensaver_bar, pip_primary(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(screensaver_bar, LV_OPA_COVER, LV_PART_MAIN);

    // Flying-Clippy: keep the swarm clean -- hide the unlock prompt until a touch.
    if (cfg.ss_style == 2) {
        lv_obj_add_flag(screensaver_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(screensaver_bar, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_add_flag(screensaver_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(screensaver_overlay, [](lv_event_t *e) {
        (void)e;
        if (cfg.ss_style == 2) {   // Flying-Clippy: first touch reveals the unlock prompt
            if (screensaver_lbl) lv_obj_remove_flag(screensaver_lbl, LV_OBJ_FLAG_HIDDEN);
            if (screensaver_bar) lv_obj_remove_flag(screensaver_bar, LV_OBJ_FLAG_HIDDEN);
        }
        if (screensaver_hold_start == 0)
            lcd_set_brightness((uint8_t)(cfg.brightness * 255 / 100 / 3));
        screensaver_hold_start = lv_tick_get();
        if (cfg.ss_unlock_tone) audio_tone_start(SS_TONE_LO_HZ);  // start of the rising hold tone
    }, LV_EVENT_PRESSED, NULL);

    lv_obj_add_event_cb(screensaver_overlay, [](lv_event_t *e) {
        (void)e;
        screensaver_hold_start = 0;
        if (screensaver_bar) lv_bar_set_value(screensaver_bar, 0, LV_ANIM_ON);
        audio_tone_stop();  // aborted hold -> fade the tone out (no completion chime)
        // Dark Charge: an aborted unlock returns to fully dark, not the dim saver.
        lcd_set_brightness(dark_charge_active ? 0
                           : (cfg.ss_style != 1 ? (uint8_t)(cfg.ss_brightness * 255 / 100) : 0));
        if (cfg.ss_style == 2) {   // back to the clean swarm
            if (screensaver_lbl) lv_obj_add_flag(screensaver_lbl, LV_OBJ_FLAG_HIDDEN);
            if (screensaver_bar) lv_obj_add_flag(screensaver_bar, LV_OBJ_FLAG_HIDDEN);
        }
    }, LV_EVENT_RELEASED, NULL);

    crt_scanlines_raise();  // scanlines on top of screensaver
    CB_LOGLN("[SS] Screen saver activated");
}

static void screensaver_tick(void) {
    if (screensaver_active && screensaver_hold_start > 0) {
        uint32_t held = lv_tick_elaps(screensaver_hold_start);
        if (screensaver_bar)
            lv_bar_set_value(screensaver_bar, (int32_t)held, LV_ANIM_OFF);
        if (cfg.ss_unlock_tone && cfg.sound) {
            // Log/perceptual pitch sweep so it rises by equal musical steps.
            float p = (float)held / (float)SCREENSAVER_HOLD_MS;
            if (p > 1.0f) p = 1.0f;
            audio_tone_set_freq(SS_TONE_LO_HZ * powf(SS_TONE_HI_HZ / SS_TONE_LO_HZ, p));
        }
        if (held >= SCREENSAVER_HOLD_MS) {
            if (cfg.ss_unlock_tone && cfg.sound)
                audio_tone_blip(SS_TONE_CHIME_HZ, 140, 0.40f);  // soft "unlocked!" confirm note
            screensaver_dismiss();
        }
    }
}

// Exclusive-input serial touch-lock (engaged when cfg.exclusive_input + a puzzle
// owns the serial stream). Overlay + logic defined after the screensaver system.
// Press-and-hold on the lock unlocks (abandons the orphaned session) so a dropped
// terminal can never strand you behind a dead touchscreen.
static lv_obj_t   *exin_overlay    = NULL;
static lv_obj_t   *exin_bar        = NULL;   // hold-to-unlock progress
static lv_timer_t *exin_hold_timer = NULL;   // ticks while the user holds to unlock
static uint32_t    exin_hold_start = 0;
#define EXIN_HOLD_MS 2000
static void exin_poll(void);

static bool cb_radio_is_playing(void);   // defined with the radio system (radio_active lives below)
static bool radio_screen_busy(void);     // first-boot blocking flow / modal is up (defined below)

static void screensaver_check_cb(lv_timer_t *t) {
    (void)t;
    exin_poll();                   // drive the serial touch-lock (engage/disengage)
    if (exin_overlay) return;      // no screensaver while touch is locked
    if (screensaver_active) return;
    // No screensaver while actively DOING something: the theremin, an HR scan, any
    // ClipBoy tool, the geiger counter, or a radio station playing -- OR while a
    // blocking first-boot flow is up (boot POST scroll, acceptable-use notice, intro
    // tour, station modal): the user is playing/reading/deciding, don't blank on them.
    // Reset the inactivity timer so stopping the activity gives a fresh full timeout.
    // F8 (owner-confirmed 2026-07-25): the id-75 fullscreen collectible plays a looping bed,
    // and the screensaver fired straight through it -- screensaver_activate() stops the stream
    // and NOTHING re-arms it, so the music died permanently mid-song. The collectible reveal
    // is a "user is watching/listening" state exactly like a radio station, so it belongs in
    // this inhibit list. AND-ed with the engine's own view rather than the UI flag alone: if
    // anything else silences the stream (Dark Charge force-silences audio), the inhibit lifts
    // instead of latching the screen on forever -- which would be this same bug class inverted.
    if (theremin_poll_timer || hr_scanning || cb_op_running || rad_geiger_active
        || cb_radio_is_playing() || radio_screen_busy()
        || (coll_fs_audio_on && audio_mp3_stream_is_playing())) {
        lv_display_trigger_activity(NULL);  // Reset inactivity counter
        return;
    }
    uint32_t timeout = screensaver_timeouts[cfg.disp_off];
    if (timeout == 0) return;
    uint32_t idle = lv_display_get_inactive_time(NULL);
    if (!screensaver_dimmed && idle >= (timeout * 4 / 5)) {
        screensaver_dimmed = true;
        lcd_set_brightness((uint8_t)(cfg.brightness * 255 / 100 / 3));
        CB_LOGLN("[SS] Dimming display");
    }
    if (idle >= timeout)
        screensaver_activate();
    if (screensaver_dimmed && idle < (timeout * 4 / 5)) {
        screensaver_dimmed = false;
        lcd_set_brightness((uint8_t)(cfg.brightness * 255 / 100));
    }
}

static void screensaver_init(void) {
    if (screensaver_timer) lv_timer_delete(screensaver_timer);
    screensaver_timer = lv_timer_create(screensaver_check_cb, 1000, NULL);
}

// ─── Exclusive input: touch-lock during a serial ARG session ─────────────────
// Engaged when cfg.exclusive_input is on AND a puzzle owns the serial stream
// (arg_session_active). The overlay lives on lv_layer_top() so it rides over
// everything + survives screen/theme rebuilds, is CLICKABLE so it swallows every
// touch, and self-nulls on delete (the project's LVGL-timer-UAF trap). Driven by
// the 1 Hz screensaver poll (engages within ~1 s) + applied instantly on toggle.
// ESCAPE HATCH: press-and-hold the lock for EXIN_HOLD_MS to unlock -- this clears
// arg_session_active, so a DROPPED terminal (no disconnect signal with DTR off)
// can never strand the user behind a dead touchscreen. Teardown is deferred via
// lv_async_call so the hold timer never frees itself inside its own callback.
static void exin_disengage(void);   // fwd
static void exin_disengage_async(void *p) { (void)p; exin_disengage(); }

static void exin_hold_tick(lv_timer_t *t) {
    (void)t;
    if (!exin_hold_start) return;
    uint32_t held = lv_tick_elaps(exin_hold_start);
    if (exin_bar) lv_bar_set_value(exin_bar, (int32_t)held, LV_ANIM_OFF);
    if (held >= EXIN_HOLD_MS) {
        exin_hold_start = 0;            // stop further ticks
        arg_session_active = false;     // abandon the (likely orphaned) serial session
        CB_LOGLN("[EXIN] unlocked by hold");
        lv_async_call(exin_disengage_async, NULL);   // tear down outside the timer cb
    }
}

static void exin_press_cb(lv_event_t *e) {
    (void)e;
    exin_hold_start = lv_tick_get();
    if (exin_bar) lv_obj_remove_flag(exin_bar, LV_OBJ_FLAG_HIDDEN);
    if (!exin_hold_timer) exin_hold_timer = lv_timer_create(exin_hold_tick, 40, NULL);
}

static void exin_release_cb(lv_event_t *e) {
    (void)e;
    exin_hold_start = 0;
    if (exin_hold_timer) { lv_timer_delete(exin_hold_timer); exin_hold_timer = NULL; }
    if (exin_bar) { lv_bar_set_value(exin_bar, 0, LV_ANIM_OFF); lv_obj_add_flag(exin_bar, LV_OBJ_FLAG_HIDDEN); }
}

static void exin_engage(void) {
    if (exin_overlay) return;
    if (screensaver_active) screensaver_dismiss();   // serial in use -> no saver under the lock
    exin_hold_start = 0;
    lv_obj_t *ov = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(ov);
    lv_obj_set_size(ov, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(ov, 0, 0);
    lv_obj_set_style_bg_color(ov, pip_bg(), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);
    lv_obj_add_flag(ov, LV_OBJ_FLAG_CLICKABLE);       // swallow all touch
    lv_obj_remove_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(ov, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ov, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(ov, 16, 0);
    lv_obj_set_style_pad_gap(ov, 8, 0);
    make_label(ov, "TERMINAL MODE", &ui_font_pipboy_20, pip_highlight());
    lv_obj_t *body = make_label(ov,
        "Touch is locked while a serial session is active.\nDrive the badge from your terminal.",
        &ui_font_pipboy_16, pip_primary());
    lv_obj_set_width(body, SCREEN_W - 40);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *hint = make_label(ov,
        "Press and HOLD to unlock -- this ENDS your current serial session "
        "(e.g. the ARG).\nDisable this lock in Settings > Exclusive Input.",
        &ui_font_pipboy_14, pip_highlight());
    lv_obj_set_width(hint, SCREEN_W - 40);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_top(hint, 14, 0);   // blank gap between the body text and this instruction
    exin_bar = lv_bar_create(ov);
    // Same FB7 hazard as screensaver_bar: an lv_bar keeps CLICKABLE and would swallow the
    // hold-to-exit press. Currently masked only because this bar starts HIDDEN (hidden objects
    // are not hit-tested) and is revealed after the press is already in flight -- an accident,
    // not a design. Make it explicit so a future change to the reveal order cannot break the
    // exclusive-input escape hatch, which is the only way out of that mode.
    lv_obj_remove_flag(exin_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(exin_bar, 200, 12);
    lv_bar_set_range(exin_bar, 0, EXIN_HOLD_MS);
    lv_bar_set_value(exin_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(exin_bar, pip_border(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(exin_bar, pip_primary(), LV_PART_INDICATOR);
    lv_obj_add_flag(exin_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(ov, exin_press_cb,   LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(ov, exin_release_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(ov, cb_selfnull_on_delete, LV_EVENT_DELETE, &exin_overlay);
    exin_overlay = ov;
    CB_LOGLN("[EXIN] touch locked (serial session)");
}

static void exin_disengage(void) {
    if (exin_hold_timer) { lv_timer_delete(exin_hold_timer); exin_hold_timer = NULL; }
    exin_hold_start = 0;
    exin_bar = NULL;                  // child of the overlay; freed with it
    if (exin_overlay) {
        lv_obj_delete(exin_overlay);
        exin_overlay = NULL;
        CB_LOGLN("[EXIN] touch unlocked");
    }
}

static void exin_poll(void) {
    bool want = cfg.exclusive_input && arg_session_active;
    if (want && !exin_overlay)      exin_engage();
    else if (!want && exin_overlay) exin_disengage();
}

static void exin_cb(lv_event_t *e) {
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    cfg.exclusive_input = lv_obj_has_state(sw, LV_STATE_CHECKED);
    cfg_save_exclusive_input();
    exin_poll();   // apply immediately (don't wait for the 1 Hz poll)
}

// ─────────────────────── INFO SCREENS ──────────────────────────────────────
// Credits, Legal, Help - replace content area, "< Back" returns to Settings

static void info_back_cb(lv_event_t *e) {
    (void)e;
    rebuild_content();  // returns to Settings
}

// Helper: create a full-width scrollable info page with a Back button
static lv_obj_t* info_page_create(lv_obj_t *cont, const char *title) {
    clear_children(cont);
    content_teardown();   // SB2: this destroys the content pane -- see content_teardown()
    lv_obj_scroll_to_y(cont, 0, LV_ANIM_OFF);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, 4, 0);
    lv_obj_set_style_pad_gap(cont, 4, 0);
    lv_obj_add_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(cont, LV_DIR_VER);
    lv_obj_add_style(cont, &style_scrollbar, LV_PART_SCROLLBAR);

    // Header row: "< Back" (left) + page title (right-aligned) on ONE line, so
    // the title doesn't eat its own row of vertical space.
    lv_obj_t *hdr = lv_obj_create(cont);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_width(hdr, lv_pct(100));
    lv_obj_set_height(hdr, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(hdr, 6, 0);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_button_create(hdr);
    lv_obj_remove_style_all(back);
    lv_obj_add_style(back, &style_list_btn, 0);
    lv_obj_add_style(back, &style_list_btn_pressed, LV_STATE_PRESSED);
    lv_obj_set_width(back, LV_SIZE_CONTENT);
    lv_obj_set_height(back, LV_SIZE_CONTENT);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, "< Back");
    lv_obj_set_style_text_font(bl, &ui_font_pipboy_14, 0);
    lv_obj_set_style_text_color(bl, pip_primary(), 0);
    lv_obj_add_event_cb(back, info_back_cb, LV_EVENT_CLICKED, NULL);

    // Title, right-aligned in the same row.
    lv_obj_t *ttl = make_label(hdr, title, &ui_font_pipboy_18, pip_highlight());
    lv_obj_set_style_text_align(ttl, LV_TEXT_ALIGN_RIGHT, 0);

    return cont;
}

// Helper: add a bulleted line (hanging indent via two-column row)
static void info_add_bullet(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, lv_color_t color) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_gap(row, 4, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    // Bullet character (fixed width)
    lv_obj_t *bullet = lv_label_create(row);
    lv_label_set_text(bullet, "-");
    lv_obj_set_style_text_font(bullet, font, 0);
    lv_obj_set_style_text_color(bullet, color, 0);
    lv_obj_set_style_min_width(bullet, 10, 0);

    // Text (flex-grow, wraps)
    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(lbl, 1);
}

// ─── CREDITS ─────────────────────────────────────────────────────────────

static void show_credits(lv_obj_t *cont) {
    info_page_create(cont, "CREDITS - DEFCON 34");

    // Centered section header preceded by a little vertical space.
    auto section = [&](const char *text) {
        lv_obj_t *sp = lv_obj_create(cont);
        lv_obj_remove_style_all(sp);
        lv_obj_set_size(sp, 1, 10);
        lv_obj_t *h = make_label(cont, text, &ui_font_pipboy_16, pip_highlight());
        lv_obj_set_width(h, lv_pct(100));
        lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    };

    lv_obj_t *intro = make_label(cont,
        "Clip-Boy came to life thanks to a LOT of effort from a lot of places & people.",
        &ui_font_pipboy_14, pip_primary());
    lv_label_set_long_mode(intro, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(intro, lv_pct(100));

    info_add_bullet(cont, "YOU - for supporting me", &ui_font_pipboy_14, pip_primary());
    info_add_bullet(cont, "My family - for surviving another year of Badge Life",
                    &ui_font_pipboy_14, pip_primary());
    info_add_bullet(cont, "BenevolentWorm - our beta tester and the one who got me interested in DEF CON in the first place!",
                    &ui_font_pipboy_14, pip_primary());

    // ── Founding Backers (before the technical credits, per owner) ──
    section("FOUNDING BACKERS");
    lv_obj_t *fb_intro = make_label(cont,
        "All Space Badge Founding Backers - thank you for believing in me!\n\n"
        "A special thank you to the following Founding Backers for their direct "
        "support of Clip-Boy as well:",
        &ui_font_pipboy_14, pip_primary());
    lv_label_set_long_mode(fb_intro, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(fb_intro, lv_pct(100));
    // Alpha-sorted (case-insensitive). New backers go in sorted order.
    static const char *founding_backers[] = {
        "Aaron Lafferty", "Andrew Pinzler", "Asmodeus", "David Ortiz",
        "Definitely not a Fed Jake", "Fink", "K4rm4", "kryptokat",
        "LMAS", "SiliconOddity", "wonkygecko",
    };
    for (int i = 0; i < (int)(sizeof(founding_backers) / sizeof(founding_backers[0])); i++)
        info_add_bullet(cont, founding_backers[i], &ui_font_pipboy_14, pip_primary());

    // ── Technical Credits (libraries, tools, AI) ──
    section("TECHNICAL CREDITS");
    static const char *tech[] = {
        "JustCallMeKoko - for ESP32Marauder & SwitchLib, the backbone of our wireless recon toolset",
        "Phil Schatzmann - for audio-tools, and lieff - for the minimp3 decoder, giving Clip-Boy a voice",
        "Gabor Kiss-Vamosi & the LVGL team - for the UI framework that makes 320x240 feel huge",
        "lovyan03 & tobozo - for LovyanGFX, our display driver",
        "h2zero - for NimBLE-Arduino and painless Bluetooth",
        "SparkFun - for VL53L5CX support that powers the theremin and HR scanner",
        "Adafruit - for NeoPixel, BusIO & MAX1704X libraries",
        "Larabie Fonts - for Monofonto, the UI typeface; Google - for the Noto Emoji fallback (OFL)",
        "Benoit Blanchon - for ArduinoJson",
        "Me-No-Dev & Mathieu Carbou - for AsyncTCP & ESPAsyncWebServer",
        "Yann Collet - for LZ4 compression",
        "Jeff Kaale (via upbeat.io) - for the scanner soundscape",
        "And every other open-source contributor: Ivan Seidel, Daniele Colanardi, Steve Marple, Peter Lerup, STMicroelectronics, and more",
        "Gemini, Adobe Firefly & ChatGPT (images)",
        "Suno (music) & ElevenLabs (synthetic voices) - SegFault-Tec FM radio",
        "Claude Code / Anthropic (code, plus this very screen)",
        "and every human whose work trained them",
    };
    for (int i = 0; i < (int)(sizeof(tech) / sizeof(tech[0])); i++)
        info_add_bullet(cont, tech[i], &ui_font_pipboy_14, pip_primary());

    // ── Special Mentions (IPs, suppliers, the long tail) ──
    section("SPECIAL MENTIONS");
    static const char *special[] = {
        "Every IP owner and creator who inspired a piece of this project",
        "Our suppliers - Waveshare, MakerHawk & Amazon",
        "Anyone whose forum post, blog, or Stack Overflow answer saved me at 2 AM",
        "Anyone I forgot - you know who you are, and I owe you a drink",
    };
    for (int i = 0; i < (int)(sizeof(special) / sizeof(special[0])); i++)
        info_add_bullet(cont, special[i], &ui_font_pipboy_14, pip_primary());

    // Fallout sign-off
    lv_obj_t *spacer = lv_obj_create(cont);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_size(spacer, 1, 6);

    lv_obj_t *signoff = make_label(cont,
        "See you in the Wasteland.",
        &ui_font_pipboy_14, pip_highlight());
    lv_label_set_long_mode(signoff, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(signoff, lv_pct(100));
    lv_obj_set_style_text_align(signoff, LV_TEXT_ALIGN_CENTER, 0);
}

// ─── LEGAL ───────────────────────────────────────────────────────────────

// Funny disclaimers - shown first, then remaining are randomized
static const char *legal_funny[] = {
    "Not responsible for any actions taken while wearing this badge that you \"can totally explain.\"",
    "Side effects may include: compulsive blinking LED syndrome, an unshakeable urge to scan QR codes on strangers' badges, and spontaneous humming of a post-apocalyptic radio jingle.",
    "Not effective against zombie apocalypse, irradiated-wildlife encounters, or a determined mass de-auth from the guy in the Boba Fett helmet.",
    "Do not eat. We say this every year. Someone always tries.",
    "May contain traces of solder flux, hubris, and coherent C++ code. The last one is the least likely.",
    "The Clip-Boy team is not responsible for any existential crises triggered by your L.E.E.T. stats.",
    "No Companion Cubes were harmed in the making of this badge. We cannot speak for what happened during testing.",
    "This badge has been tested on animals. The animals were unimpressed and went back to sleep.",
    "In the event of a water landing, this badge will not function as a flotation device. It will function as a very expensive paperweight.",
    "If this badge becomes sentient, do not make deals with it. We learned this the hard way during firmware testing.",
    "This badge does not grant you root access to anything except your own disappointment.",
    "The WiFi scanner is for authorized testing only. \"I didn't know it was illegal\" is not a valid defense. Neither is \"but my badge told me to.\"",
    "This badge's Geiger counter does not detect actual radiation. If it starts clicking near the hotel breakfast buffet, that's a different kind of problem.",
    "The theremin feature is a musical instrument, not a tool, no matter how badly you play it.",
    "Not a substitute for social skills, though it does make an excellent conversation starter and/or ender.",
    "This badge is not TEMPEST certified. If you're worried about that, you probably shouldn't be at DEFCON.",
    "Warranty void if exposed to: sunlight, moonlight, EMP, the DEFCON network, or sustained eye contact with a fed.",
    "The number of bugs in this firmware is a feature, not a defect. We call it \"organic code.\"",
    "Any resemblance to a certain fictional wrist-mounted personal information processor is purely coincidental and significantly exaggerated.",
    "If you can read this disclaimer, you have better eyesight than the developers who wrote it on a 320x240 screen.",
    "Your odds of reading this entire disclaimer to the end are approximately 3%.",
    "This badge will not help you find enlightenment, your car keys, or a parking spot at the Las Vegas Convention Center.",
    "In the event of nuclear apocalypse, please note that fallout-shelter enrollment is handled separately and this badge does not constitute a reservation.",
    "No novelty currencies were accepted in the production of this badge. We tried. The vending machines said no.",
    "The NeoPixel LEDs are rated for 50,000 hours. The battery is rated for about 8. Plan accordingly.",
    "This badge has more RAM than the computer that landed on the moon and less RAM than you need for one Chrome tab.",
    "The SAOs listed in this badge do not exist. If someone tries to sell you an RTX 7090 Ti SAO, they are lying and you should buy two.",
    "Badge not valid as government-issued ID, proof of hacking ability, or evidence of good life choices.",
    "3 out of 4 Clip-Boy developers recommend not licking the PCB. The fourth one is why we have this disclaimer.",
    "You are now mass-scrolling tiny text on a wrist-mounted screen at a hacker convention. Your life choices have led you to this exact moment. Reflect.",
    "Glitches caused by Wesley Crusher",
    "Excludes carbon-based life forms",
    "May cause spontaneous dancing",
    "Not recommended for time travelers",
    "Does not work in parallel universes",
    "May cause existential crisis",
    "Not responsible for temporal paradoxes",
    "May void warranty on reality",
    "Disclaimer may be better than product",
    "This disclaimer is not responsible for other disclaimers",
    "May cause uncontrollable urge to read more disclaimers",
    "Does not grant superpowers (unfortunately)",
    "Not recommended for use by fictional characters",
    "Do not use as ice cream topping",
    "Remove child before folding",
    "Warning: Cape does not enable user to fly",
    "Not suitable for children aged 36 months or over",
    "Do not iron clothes while wearing them",
    "May contain nuts",
    "Do not iron clothes on body",
    "Do not use near fire, flame, or sparks",
    "Do not turn upside down",
    "Do not drive with sunshield in place",
    "Warning: May contain traces of milk, soy, meat, nuts, fruit, arbitrary food-like or food-adjacent items, tax returns, mayonnaise or coherent C++ code",
    "For external use only",
    "Not intended to diagnose, treat, cure, or prevent any disease",
    "May cause drowsiness",
    "Do not operate heavy machinery after use",
    "Individual results may vary",
    "Do not shake",
    "Contains caffeine",
    "Do not disassemble",
    "May interfere with pacemakers",
    "Not responsible for data loss",
    "Does not connect to the internet",
    "Maximum liability limited to purchase price",
    "Not for highway use",
    "Maximum weight capacity: -1 lbs",
    "Helmet required",
    "Not suitable for commercial use",
    "Not suitable for residential use",
    "Past performance does not guarantee future results",
    "You may lose money",
    "Not FDIC insured",
    "Not available in all states",
    "All sales final",
    "Void where prohibited",
    "One per customer",
    "Not valid in Quebec",
    "Contains hazardous materials",
    "Do not incinerate",
    "May be harmful if swallowed",
    "Not a life-saving device",
    "Batteries not included",
    "Some assembly required",
    "Comes fully assembled",
    "Colors may vary from images shown",
    "Dramatization - do not attempt",
    "Closed course - professional driver",
    "Your mileage may vary",
    "For entertainment use only",
    "Caution: product not entertaining",
};
#define NUM_LEGAL_FUNNY  (sizeof(legal_funny) / sizeof(legal_funny[0]))

// Serious legal disclaimers (placeholder - will be finalized after legal review)
static const char *legal_serious[] = {
    "Clip-Boy is an original, independent creation. It is not manufactured, endorsed, licensed, or sponsored by Bethesda Softworks, ZeniMax Media, Microsoft Corporation, or any other third-party rights holder. All trademarks are the property of their respective owners.",
    "This device constitutes a transformative work of parody and commentary incorporating original artwork, original writing, and original software. All visual assets were created by the Clip-Boy team. References to third-party intellectual properties are made for purposes of humor, satire, and cultural commentary under the fair use doctrine (17 U.S.C. 107) and are not intended to imply affiliation or endorsement.",
    "This device is sold on the basis of its technical functionality as an ESP32 security research platform, LiDAR theremin, and programmable LED controller. No third-party intellectual property is used as a basis for sale.",
    "This badge incorporates open-source software including ESP32Marauder, LVGL, LovyanGFX, NimBLE, audio-tools, and others. All FOSS components retain their original licenses (GPL, MIT, Apache, etc.). We gratefully acknowledge these projects and their maintainers.",
    "Clip-Boy is sold for lawful, authorized use only. It ships passive / listen-only (Sn34k-Boy); a separate research build (Res34rch-Boy) with active capabilities can be loaded by the user, for legitimate security research and authorized testing only. It is the sole responsibility of the user - including anyone who downloads, flashes, installs, or uses it, even secondhand or gifted units - to comply with all applicable laws (for example the Computer Fraud and Abuse Act, 18 USC 1030; the Wiretap Act, 18 USC 2511-2512; 47 USC 333 and FCC rules; plus your state, local, and national equivalents). Use it only on networks, devices, and radio environments you own or have explicit, current authorization and consent to test; do not use it to disrupt, intercept, impersonate, or interfere with anything you are not authorized to touch. Coruscant Productions, LLC, to the fullest extent permitted by law, disclaims liability for misuse or unlawful use of the software, firmware, or hardware.",
    "Indemnification: to the fullest extent permitted by law, you agree to indemnify, defend, and hold harmless Coruscant Productions, LLC and its members, managers, officers, employees, and agents from and against any and all claims, demands, actions, liabilities, damages, losses, costs, and expenses - including reasonable attorneys' fees and court costs - arising out of or relating to (a) your use, misuse, or modification of Clip-Boy, its firmware, or software; (b) your breach of these terms; or (c) your violation of any applicable law or the rights of any third party.",
    "This device complies with FCC Part 15. Operation is subject to two conditions: (1) this device may not cause harmful interference, and (2) this device must accept any interference received. Changes or modifications not expressly approved could void the user's authority to operate the equipment. Users outside the United States are responsible for compliance with applicable radio frequency regulations in their jurisdiction.",
    "This badge is provided \"as is.\" To the maximum extent permitted by applicable law, no warranties are provided, express or implied. Nothing in this notice affects statutory consumer rights that cannot be excluded by law. The creators shall not be liable for any damages arising from use or inability to use this device.",
    "This device may be subject to export control regulations. By acquiring this device, you represent that you will comply with all applicable export and import laws.",
    "For IP inquiries, contact the Clip-Boy team via the Credits screen.",
    "If you are taking legal advice from a conference badge, we have concerns.",
};
#define NUM_LEGAL_SERIOUS  (sizeof(legal_serious) / sizeof(legal_serious[0]))

// Open-source notices shown on-device so the required attribution travels with
// the distributed binary, not just the source repo. ASCII only (pipboy font).
static const char *legal_oss[] = {
    "This firmware is free software under the GNU General Public License v3. It comes with ABSOLUTELY NO WARRANTY. Complete corresponding source and full license texts are at github.com/SafeHazard/Clip-Boy (LICENSE / LICENSE.md).",
    "The Wi-Fi/Bluetooth tools are a fork of ESP32 Marauder by Just Call Me Koko. Marauder and several other parts (LVGL, ArduinoJson, SparkFun VL53L5CX, lz4, and more) are used under permissive MIT/BSD licenses.",
    "ESP32 Marauder is provided under the MIT License: Copyright (c) 2020 Just Call Me Koko. Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the Software), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions: The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.",
    "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY ARISING FROM THE SOFTWARE OR ITS USE.",
    "Audio playback uses arduino-audio-tools (GNU GPL, copyleft) with the minimp3 decoder (public domain). The complete corresponding source for this firmware is available at the repository above.",
};
#define NUM_LEGAL_OSS  (sizeof(legal_oss) / sizeof(legal_oss[0]))

// AI & tooling transparency (mirrors the shop's disclosure).
static const char *legal_ai[] = {
    "Bryce designed the PCB and enclosure, set the feature list, and made every ship-or-cut call. Everything was tested on real (sometimes gloriously flaky) hardware.",
    "Tools used along the way: Claude + Claude Code (code review, PCB review, debugging partner); ESP32 Marauder by justcallmekoko (base for the Wi-Fi/BLE tooling, ported with significant AI-assisted refactoring); Gemini (collectible artwork, prompts written and curated by Bryce); Adobe Firefly (image refinement and cleanup); ChatGPT (a handful of static graphics, including Clippy).",
    "Radio audio (SegFault-Tec FM): AI-generated under commercial licenses -- Suno (music) and ElevenLabs (synthetic voices). The voices are original synthetic voices, never clones of real people. Tones, numbers-station digits and static are generated programmatically.",
    "AI was used as a tool, like Git or a multimeter. The decisions, the design, the bugs, and the jokes are all Bryce's.",
};
#define NUM_LEGAL_AI  (sizeof(legal_ai) / sizeof(legal_ai[0]))

static void show_legal(lv_obj_t *cont) {
    info_page_create(cont, "LEGAL");

    // Serious disclaimers first
    make_label(cont, "THE BORING STUFF", &ui_font_pipboy_16, pip_highlight());

    for (int i = 0; i < NUM_LEGAL_SERIOUS; i++) {
        info_add_bullet(cont, legal_serious[i],
                        &ui_font_pipboy_14, pip_primary());
    }

    // Open-source license notices (on-device, for binary-distribution compliance)
    make_label(cont, "OPEN SOURCE", &ui_font_pipboy_16, pip_highlight());
    for (int i = 0; i < NUM_LEGAL_OSS; i++) {
        info_add_bullet(cont, legal_oss[i], &ui_font_pipboy_14, pip_primary());
    }

    // AI & tooling transparency
    make_label(cont, "AI DISCLOSURE", &ui_font_pipboy_16, pip_highlight());
    for (int i = 0; i < NUM_LEGAL_AI; i++) {
        info_add_bullet(cont, legal_ai[i], &ui_font_pipboy_14, pip_primary());
    }

    // Separator
    lv_obj_t *sep = lv_obj_create(cont);
    lv_obj_remove_style_all(sep);
    lv_obj_set_size(sep, lv_pct(100), 1);
    lv_obj_set_style_bg_color(sep, pip_border(), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);

    // Funny disclaimers: new ones first, rest randomized
    #define LEGAL_NEW_COUNT  30
    uint16_t funny_order[NUM_LEGAL_FUNNY];
    for (uint16_t i = 0; i < NUM_LEGAL_FUNNY; i++) funny_order[i] = i;

    // Fisher-Yates shuffle on the remainder (indices >= LEGAL_NEW_COUNT)
    for (uint16_t i = NUM_LEGAL_FUNNY - 1; i > LEGAL_NEW_COUNT; i--) {
        uint16_t j = LEGAL_NEW_COUNT + (esp_random() % (i - LEGAL_NEW_COUNT + 1));
        uint16_t tmp = funny_order[i];
        funny_order[i] = funny_order[j];
        funny_order[j] = tmp;
    }

    make_label(cont, "OTHER DISCLAIMERS", &ui_font_pipboy_16, pip_highlight());

    for (uint16_t i = 0; i < NUM_LEGAL_FUNNY; i++) {
        info_add_bullet(cont, legal_funny[funny_order[i]],
                        &ui_font_pipboy_14, pip_dim());
    }
}

// ─── HELP (Expandable accordion) ─────────────────────────────────────────

struct HelpItem {
    const char *title;
    const char *text;
};

struct HelpCategory {
    const char *name;
    const HelpItem *items;
    uint8_t count;
};

static const HelpItem help_navigation[] = {
    { "Divisions", "Three divisions on the bottom bar: STATS, ITEMS, DATA. Tap to switch. The lit dot shows which is active." },
    { "Tabs",      "Each division has 3 tabs along the top. Tabs change when you switch divisions." },
    { "Split Pane", "List on the left, details on right. Tap a list item to view its details." },
    { "Status Bar", "Top bar shows battery %, active task, free RAM, a WiFi icon when connected, a ? icon that opens this Help, and an FL button on the right that toggles the front-LED flashlight." },
    { "Flashlight", "Tap FL in the status bar to turn the four front LEDs to full white. Tap again to turn them off. State persists across screens." },
};
static const HelpItem help_collectibles[] = {
    { "How to Scan", "Go to ITEMS > Collectibles and tap Scan. Hold the badge about 3 in. (8 cm) from the code and line the code's 3 corners (Top Left, Top Right, Bottom Right) up with the 3 guide boxes at the grid corners (each corner is about four squares). Hold steady until it locks. Matte codes read best; glossy ones don't. The 'Trilancer' part of your wristband is ~3\"/8cm." },
    { "Reading the Scanner", "The 8x8 grid is a live read-out with a legend on the right: one shade marks empty space, another marks the code surface, and a BOLD WHITE OUTLINE marks each of the 3 corners it has found (the exact colors follow your theme -- read the on-screen legend). Aim to get an outlined corner into each corner of the grid. Hold the scanner ~3\" (8 cm) from the code. The 'Trilancer' part of the wrist strap is that length. The prompt at the bottom of the panel tells you what to fix - Back up (too close), Move closer (too far), Square up & hold (flatter / steadier), then Locked. It won't lock a shaky or half-seen code. By DEFAULT it then shows you the decoded bump pattern to confirm before the item unlocks (see Verify Each Scan). The little bubble is just a level guide - you can scan from any angle; what matters is holding the badge's sensor parallel to the code's face, not level to the ground." },
    { "Scanner Calibration", "If scans are unreliable (odd lighting, a new surface), flat-field-calibrate the LiDAR: DATA > Settings > Scanner Calibration, fill the sensor's view with a flat surface (ideally the back of a 'Scan me with your Clip-Boy' tag) at ~3\" (8 cm) away, tap Calibrate and hold ~5s. Clear reverts to defaults. Note that the 'Trilancer' part of your wrist strap is 3\"/8cm." },
    { "Verify Each Scan", "On by default (DATA > Settings > Verify each scan). After a scan locks, the badge shows the decoded 4x4 bump pattern on a CONFIRM SCAN grid so you can check it before the item unlocks: tap Confirm to accept, or tap cells to fix a mis-read bump, then Confirm. Turn it off to unlock the instant a scan locks (faster, but no safety net)." },
    { "Manual Entry", "For a tag that won't scan (worn, glossy, awkward angle), hand-enter its pattern instead: on the Scan screen tap MANUAL ENTRY, or press and HOLD the Scan button (~0.6s). Read the tag's raised bumps into the 4x4 grid, then Confirm -- it unlocks just like a scan." },
    { "Fix a Wrong Result", "If a scan or manual entry unlocks the wrong item, tap 'Not this? Fix' on the reveal to undo it. It only removes the item that scan/entry just added -- it can never erase something you already had." },
    { "Collected Items", "After collecting an item, the badge scrolls to that item's entry. You'll see a preview image, a description of it and its 'effects', and the unlock pattern to share with friends or friends-to-be in line. Your STATS > Status screen shows all 'effects' you've acquired via your collectibles." },
    { "Rarity Tiers", "Four tiers in order of rarity: Common, Rare, Legendary, and 0-Day. 0-Day is the rarest." },
    { "Locked Items", "Uncollected items show '??? Locked'. Find HR codes around the CON to unlock them. Tap 'All' on the Collectibles screen to hide these entries; tap 'Found' to re-display." },
    { "Missing Tags", "Missed a tag at the CON? That's rough. But we're not monsters. After DC34 you can visit github.com/SafeHazard/Clip-Boy to find unlock codes for any you may have missed." },
    { "Stat Bonuses", "Collected items grant stat bonuses visible on STATS > Status page." },
    { "SD Modding", "Drop a collectibles.csv and 200x200 .a8 images on the SD card to add or override items. See the SD Card Mods section for format details." },
};
static const HelpItem help_tools[] = {
    { "Responsible Use", "Use these tools only on hardware you own or are authorized to touch. Per-tool guidance is in each tool's More Info." },
    { "Overview", "Wireless recon and analysis tools forked from the ESP32 Marauder project. Includes WiFi scanning (APs, stations, beacons, probes), detection of deauth frames and pwnagotchi devices, Bluetooth/BLE scanning, packet capture, plus joining a WiFi network and managing saved networks. Each tool has a More Info panel with details." },
#ifdef CLIPBOY_RES34RCH
    { "Active Research !", "Tools whose name starts with '!' are active research tools - they transmit frames that can disrupt, spoof, or annoy other devices, so they are for authorized testing only. Per-tool guidance is in each tool's More Info." },
    { "BLE Spam \"All\"", "Known limitation: BLE Spam > All (which cycles every Bluetooth spam type at once) can be slow or briefly unresponsive to start - a quirk of driving all payloads on the shared WiFi/BT radio. If it doesn't start within a few seconds, back out and pick a single spam type instead: Sour Apple, Swiftpair, Samsung, Google, and Flipper all start instantly." },
#endif
    { "Categories", "Tap a category header to expand or collapse it. Tap a tool to open its detail page; tap Start (or Scan) on that page to actually run it. Output appears in the right pane." },
    { "WiFi", "WiFi is off by default to save power. It turns on automatically when you start a scan and turns off again when the scan stops." },
    { "Saved Networks", "Tools > Utilities/Lists > Saved Networks lists every WiFi network you've joined, each with its stored password. Tap an entry to delete it. Passwords are kept in the badge's flash without encryption, so clear any you no longer need - especially before lending, gifting, or reselling the badge." },
    { "Live Table", "During AP, station or Bluetooth scans (the Scan tools except BLE Adverts, Analyze > Beacons/Espressif, and Detect > Skimmer Check) the output pane has a Live Devices (table) button that opens a full-screen, Wireshark-style table: columns for type, signal, channel and name. Tap a row for its full detail; use < PREV / NEXT > to step through results, or LATEST to follow the newest as they arrive." },
    { "Network Names", "SSIDs and device names render in the badge's terminal font, which covers Latin, accented, Greek, Cyrillic and common symbols, plus a small set of popular emoji. A glyph it can't draw (other emoji, CJK) shows as a hollow box. Control, zero-width or text-direction characters - tricks used to spoof or hide text - show as a solid box and turn the whole name red. That's on purpose, so an oddball or hostile name can't draw fake or hidden text on screen." },
    { "Airplane Mode", "When airplane mode is on, tools that need WiFi or Bluetooth show a confirm dialog before launching. Choose Turn Off to disable airplane and re-tap the tool to launch it." },
    { "Flock Batteries: What It Matches", "Detect > Flock Batteries looks for a specific Bluetooth broadcast fingerprint: a manufacturer-data block carrying the Xuntong company ID, plus either a device name in one of the shapes those units use (Penguin-NNNNNNNNNN, FS Ext Battery, or ten digits) or no name at all. That is a signature of the hardware as we understand it today, not a guarantee about the product line.\n\nWhat that fingerprint belongs to matters: it is the signature of a Bluetooth accessory that SOME units carry -- a battery pack -- not of the camera. Units running on solar with no battery pack are usually Bluetooth-silent, so this tool can be completely quiet on a street with a camera on every pole. The camera's own always-on radio signature is on WiFi, and the badge does not look for it yet.\n\nSo read a quiet screen carefully. If Flock ships new firmware, changes the broadcast, or fields a model with a different fingerprint, this tool will under-report or miss those units entirely -- and it will look exactly the same as an empty street. A quiet screen means nothing matched the pattern we look for, which is NOT the same as nothing being there. Treat a hit as informative and a miss as inconclusive.\n\nWhat it REPORTS is Bluetooth-only. A unit that is powered down, out of range, or not broadcasting will not appear, and range varies a lot with walls, vehicles and crowds." },
    { "Known: Scanning While Reading", "The AP and station lists are shared by every tool that reads them, and a scan adds to them while you are looking. Known behaviors, all temporary: a row may appear with a blank or partial name or a signal reading of zero; a row's signal figure is the value from when that device was first seen, not a live one; a count in the status bar may briefly disagree with the number of rows on screen; and an action aimed at a device you selected may skip it for a single pass and then resume. None of these damage saved data or require a restart. To avoid them entirely, stop the scan before selecting a target or reading the list closely." },
};
static const HelpItem help_theremin[] = {
    { "How to Play", "Pick a waveform for at least one band on the left. Tap Enable, then move a hand over that band of the LiDAR. Closer hand = higher pitch." },
    { "Voice Zones", "The LiDAR grid is split into 4 vertical bands left-to-right (L, CL, CR, R). Each band can drive its own voice. Hand height inside a band sets pitch." },
    { "Waveforms", "Each voice can be None, Sine, Square, Saw, or Triangle. Changes save immediately." },
    { "Bars + Readouts", "Each bar fills downward as your hand approaches. Numbers under each bar show distance (mm) and frequency (Hz) for that band. OFF = no voice loaded; --- = voice loaded but no hand detected." },
    { "Volume", "Slider sets loudness. Distance affects pitch only -- not volume -- so notes stay even across the play range. The Settings volume slider stays in sync." },
    { "Hold on Dropout", "If the sensor briefly loses your hand, the last note holds for ~100 ms before going silent. Smooths over single-frame sensor blinks during play." },
    { "Tuning (k + ag)", "Under the Enable button. k = how many sensors must agree before a band fires (1=fastest, 8=strictest, default 2). ag = max distance spread allowed among those sensors (5-50mm, default 20). Bigger k or smaller ag rejects glitches but needs a more solid hand presentation." },
    { "Shared Sensor", "Theremin and HR code scanner share the VL53L5CX LiDAR. Only one can be active at a time. Leaving the Theremin tab disables it automatically." },
};
static const HelpItem help_radio[] = {
    { "SegFault-Tec FM", "A simulated radio hiding in ITEMS > SAOs -- styled stations playing pre-recorded audio, not a real receiver. Tap 'Whether Radio' (the one SAO that isn't a joke) to open the dial." },
    { "Stations", "Five stations along the dial. One is always live; the rest unlock as your collectible count climbs -- the rarest signal only after you've collected most of the set." },
    { "Tuning In", "Drag the tuning dial to a station's mark. It's forgiving: get close and hold steady, or release near the mark, and it snaps in. See Settings > Radio Tuning to make stations lock instantly instead." },
    { "The Scope", "The oscilloscope is a visual tuning cue: the waveform comes alive once 'Whether Radio' is tuned in and settles toward a flat line on dead air. It's a picture of the station, not a live feed of radio waves. Want the actual words? Stations with speech carry a Transcript (below) that shows the contents of the 'broadcast'." },
    { "Transcript", "Some stations carry a Transcript button. Tap it for a scrollable readout of what's playing -- captions for every clip, updated live, so the audio is never the only way in." },
    { "Volume + Leaving", "The dial's volume slider stays in sync with the main Volume setting. Leaving the radio (Back, or any nav) stops playback." },
};
static const HelpItem help_leds[] = {
    { "Fuse Lights", "The four fuse and edison lights on the side, labeled by corner -- Fuse TL, BL, TR, BR -- inspired by 1950s retro-future aesthetics." },
    { "Front LEDs", "Front 1-4 are on the front of the enclosure (left to right). The Flashlight (FL) status-bar button overrides whatever color/animation these are set to and forces them to full white until you toggle it off." },
    { "Controls", "Color (RGB sliders) and brightness editing is part of the Custom theme: on the All LEDs row pick Custom. That screen then gives you one master set of brightness/color/animation controls that drive every LED at once; tap any individual LED in the list to fine-tune just that one. Built-in presets drive all LEDs together." },
    { "Animations", "None (static color), Breathe (sinusoidal pulse), or Chase (rotating palette). Speed: 1=slow (10s), 10=fast (2s)." },
    { "Chase Mode", "Colors crossfade smoothly between chase-enabled LEDs. Fuse LEDs chase in warp-core order (through the fiber optic pairs). Speed uses the slowest chase LED." },
    { "All LEDs", "Chase Speed slider sets the pace for all 8 LEDs (Tunnel Runner = fastest ... Severely Overburdened = slowest); it sticks when you switch presets. Presets apply a color scheme to all 8 at once: Mojave (amber chase), Ribbit City (green chase), Flashbang (white chase), Rainbow (ROYGBIV chase), Custom (unlocks the master + per-LED editing -- see Controls), Off." },
    { "Default", "Out of the box, LEDs use the Mojave amber chase preset." },
};
// Ordered to mirror the on-screen Settings sections (DISPLAY / AUDIO / SCREEN & CRT /
// SYSTEM / POWER & RESET) -- the "-- SECTION --" rows are the same headers you see on
// the device, so a Help topic sits in the same cluster as its control. (INFO's
// Credits/Legal/About buttons open their own pages; Scanner Calibration lives under
// Collectibles.) Keep this order in sync with build_data_settings if a row moves.
static const HelpItem help_settings[] = {
    { "-- DISPLAY --", "Look of the screen: Theme (color palette), Brightness, and Terminal Text size." },
    { "Themes", "Mojave: amber. Ribbit City: green. Flashbang: black on white (bring sunglasses). A fourth, Custom, unlocks once you've found every collectible -- pick any hue from a color picker and the whole palette derives itself." },
    { "Brightness", "Slider sets the display backlight brightness from 10% to 100%. Reading from a meter? You're stalling." },
    { "Terminal Text", "Sets the font size of the tool scan/output log: Small, Medium or Large. Larger is easier on the eyes; smaller fits more lines on screen." },
    { "-- AUDIO --", "Sound: Volume, Mute, and the individual Tap / Scanning / Unlock cue toggles." },
    { "Volume", "Slider sets the audio output volume. Drives the theremin and any audio cues. Stays in sync with the slider on the Theremin page." },
    { "Mute", "Switch hard-mutes audio output regardless of the Volume slider position. Volume is preserved when you unmute." },
    { "Tap Sounds", "Click feedback when you tap buttons and controls. Turn off to silence taps while keeping theremin, geiger and other audio. Mute overrides this." },
    { "Scanning Sounds", "Plays an ambient tone bed while the Collectibles HR-code scanner is searching, so you can tell it's working without watching the screen. Turn off to scan silently. Respects Volume and Mute." },
    { "Unlock Tone", "Plays a rising tone while you hold to unlock the screensaver, so you know the badge heard you. Pitch climbs as the bar fills, with a chirp when it unlocks. Respects Volume and Mute." },
    { "-- SCREEN & CRT --", "Idle + retro-display behavior: Screensaver timeout/style, Idle Brightness, Dim LEDs, and the CRT effects." },
    { "Screensaver", "Two rows: 'Screensaver' is the timeout (15s to Never), 'Screensaver Style' picks Clip-Boy mascot or Blank." },
    { "Idle Brightness", "Slider (1-100%) sets how dim the Clip-Boy mascot gets when the screensaver is up. Default 10%. Has no effect when Style is Blank (which always goes fully dark)." },
    { "Dim LEDs When Idle", "When on, the NeoPixels turn off as the screensaver activates. They come back when you wake the screen. Off by default -- the badge looks more alive on a shelf with the LEDs running through the screensaver." },
    { "CRT Scanlines", "Adds semi-transparent horizontal lines over the display for a retro CRT look. Toggle on/off." },
    { "CRT V-Roll", "Occasionally the display slips vertically like a CRT losing V-sync, then snaps back. Random 1-10 min intervals." },
    { "-- SYSTEM --", "Behavior: Verify Each Scan, the Help button, Radio Tuning/Alerts, Airplane, Exclusive Input, and PCAP saving." },
    { "Verify Each Scan", "On by default. After a Collectibles scan locks, the badge shows the decoded bump pattern to confirm (or correct) before the item unlocks. Turn off to unlock the instant a scan locks. Full detail under ITEMS > Collectibles > Verify Each Scan." },
    { "Help Button", "When on, a '?' icon appears in the status bar that opens this Help system from anywhere. On by default. Toggle off if you find it distracting." },
    { "Radio Tuning", "Controls the radio's tune-in feel. Disabled: stations lock the moment you pick them -- set it here if you'd rather skip the dial. Once Per Boot: tune each station in once per power-up, then it stays locked. Every Access: re-tune every time. The dial is forgiving -- get close and hold, or release near the mark, and it snaps in." },
    { "Radio Alerts", "When on, the badge nudges you to check the SegFault-Tec FM radio and pops a note when your collectibles unlock a new station. Turn off to silence both -- you can turn it back on here any time." },
    { "Airplane Mode", "Disables WiFi and Bluetooth. Tools that need radios are blocked until you turn it back off, and turning it on also stops anything already running. Every tool you reach by tapping honors it, including the small Scan buttons on picker pages (they dim while it is on)." },
    // Owner-decided disclosure (2026-07-26), stated as observable fact with no mechanism: the
    // Marauder serial CLI is not covered by the gate (gating it means editing the vendored
    // parser), and airplane is a policy setting rather than a hardware power cut. Third layer of
    // the same disclosure -- README "Known issues" and the Settings screen carry it too.
    { "Device List Limits",
      "The WiFi access-point and station lists grow for as long as a scan runs. Analyze > Probes "
      "keeps up to 100 different network names; once it is full, a name it has not seen before "
      "will not be added, though the counts next to names already listed keep rising. "
      "Scan > BT Devices "
      "is different: it reports about the first 50 distinct devices it meets, and a device that "
      "shows up after that will not appear until you restart the scan -- so in a very "
      "crowded room treat that list as a sample, not a census. Detect > Skimmer Check shares that "
      "same Bluetooth list and the same limit. The Detect tools that hunt one specific kind of "
      "device restart their own scan every couple of seconds, so a device that turns up later "
      "does still appear: AirTags and Flippers are not limited, and Flock keeps up to 50 at a "
      "time. The Pwnagotchi list holds "
      "a hundred and drops its longest-unseen entry to make room for a new arrival. Stop and "
      "start a scan, or use Tools > Utilities/Lists > Clear All, for a fresh count." },
    { "Airplane Mode: Limits", "Two things it does not cover. (1) The USB serial command line: commands typed there can start a scan even while airplane mode is on, and the switch keeps reading as engaged -- if you need certainty that nothing is transmitting or receiving, unplug USB or power the badge off. (2) It is a setting, not a power cut: the radio hardware comes up when the badge boots whatever the setting says. Airplane mode stops the tools and clears the receive filters." },
    { "Exclusive Input", "When on, the touchscreen locks while you are driving an interactive puzzle from a serial terminal -- so touch and serial can't fight each other. A 'TERMINAL MODE' overlay appears; it clears when the serial session ends, or press and HOLD the overlay for 2 seconds to unlock by hand (use that if your terminal disconnects mid-session)." },
    { "Allow PCAP Saving", "Off by default on Sn34k-Boy. Turn on to let packet-capture tools (Analyze > Raw/PCAP, Analyze > EAPOL/PMKID) write a .pcap file to the SD card -- or to internal storage if no card is present. Required for the Capture WPA Handshake workflow; with it off, captures produce no file." },
    { "-- INFO --", "Buttons that open Credits, Legal, About, and this Help system. (Those screens are also covered under Help > Getting Started.)" },
    { "-- POWER & RESET --", "Backup/Restore Progress to SD, Charge with lights off, and the two Reset buttons." },
    { "Backup Progress", "Settings > Backup Progress to SD writes your collectible finds AND your ARG puzzle progress to /collectibles.sav. The backup is tied to THIS badge -- it restores here but not on another badge (so it can't be shared to skip the puzzles). SD card format: a 1 GB FAT partition works. Larger partitions may work but are untested; multi-partition cards are unknown." },
    { "Restore Progress", "Settings > Restore Progress from SD loads /collectibles.sav. It's validated first -- corrupt files, or a backup made on a DIFFERENT badge, are rejected -- then you choose MERGE (keep your progress AND add the file's) or OVERWRITE (replace yours with the file's -- back up first if unsure). Same SD format as Backup (1 GB FAT)." },
    { "Charge with lights off", "Charges the badge with the screen and every NeoPixel dark -- only the hardware charge LED stays lit -- so you can top up on a shelf without the light show. Tap it, confirm, and the badge goes dark; press and HOLD the screen to wake it." },
    { "Resets", "Reset Collectibles: marks all as uncollected. Reset All Settings: factory reset and reboot -- this ALSO clears your collected items (they return to locked). Both at the bottom of Settings with confirmation dialogs." },
};
static const HelpItem help_sdcard[] = {
    { "Getting Started", "You'll need collectibles.csv and png_to_a8.py from github.com/SafeHazard/Clip-Boy." },
    { "Custom Items", "Place collectibles.csv at the SD card root. Same CSV format as built-in data: ID, Title, Source, Tier, Description, then up to three Mod/Stat pairs." },
    { "Custom Images", "Drop raw 200x200 A8 files at /images/<id>.a8 on the SD card, where <id> is the collectible's numeric ID. Convert PNG to A8 with data/png_to_a8.py." },
    { "Load Priority", "SD card overrides LittleFS overrides built-in PROGMEM. Matching IDs get replaced; new IDs get added. Reboot to pick up changes." },
};
static const HelpItem help_screensaver[] = {
    { "Timeout", "Settings > Screensaver (the timeout dropdown, top of the screensaver block) controls how long the screen stays bright before the saver kicks in. Options: 15s, 30s, 60s, 2m, 5m, or Never." },
    { "Styles", "Settings > Screensaver Style: Clip-Boy shows a dim mascot; Blank goes fully dark." },
    { "Idle Brightness", "Settings > Idle Brightness sets how dim the Clip-Boy mascot gets (1-100%, default 10%). The slider greys out when Style is Blank since it has no effect there." },
    { "Wake Up", "Tap and HOLD for 2 seconds to unlock. The progress bar fills as you hold. The hold prevents accidental pocket wakes." },
    { "LED Behavior", "By default, the NeoPixels keep running through the screensaver. Enable Settings > Dim LEDs when idle if you'd rather have them turn off with the screen and restore on wake." },
};
static const HelpItem help_trouble[] = {
    { "First Boot", "Initial boot takes around 10 seconds while SPIFFS and LittleFS format. The screen may stay blank during this; that's expected." },
    { "Boot Screen", "The retro POST screen shows real system info while the badge initializes. If it hangs there for more than ~30 seconds, something has actually gone wrong - try a power cycle." },
    { "Frozen Display", "Hold the power button to force reboot. If that doesn't work, plug in USB-C to give the watchdog something to chew on, or wait for battery drain." },
    { "Reset Settings", "Settings > Reset All Settings restores factory defaults and reboots. Your collected-state bitfield is wiped too - items you've found will return to locked." },
    { "Serial Debug", "Plug in USB-C and open a serial terminal at 115200 baud, 8 data bits, no parity, 1 stop bit (8N1). Good for boot logs and runtime output -- and it's how the ARG's terminal puzzles talk to the badge. New to serial? On Windows try PuTTY or Tera Term; on macOS/Linux run 'screen /dev/tty.usbmodem* 115200' or use minicom; or the Serial Monitor built into the Arduino IDE (cross-platform). Pick the COM/tty port that appears when you plug the badge in." },
    { "PCAP capture is getting slow", "Captures save to /pcaps on the SD card. The card searches that folder every time it saves, so once you have thousands of .pcap files each new capture takes longer -- the badge warns you at 500+ and again at 5000+, and with several thousand the screen can stutter. This only hits heavy users; a handful of captures is fine. Fix: put the card in a computer, copy the .pcap files off /pcaps, then delete them. The badge renumbers automatically from a clean folder -- nothing is harmed, it's just housekeeping." },
};
static const HelpItem help_battery[] = {
    { "Battery Life", "2200mAh 3.7V LiPo. Percentage shown in the status bar. Charge via USB-C." },
    // Owner-supplied and owner-confirmed (2026-07-26). The percentage is measured across the
    // battery, so while USB is supplying charge current it reads high and jumps around -- the
    // charge LED is the honest indicator. Documented rather than "fixed": the reading is not
    // wrong so much as measuring something other than what a user assumes while plugged in.
    { "Percentage While Charging",
      "The battery percentage can read inaccurately while the badge is plugged in to USB. "
      "Trust the charge LED next to the USB port instead, and read the percentage after "
      "unplugging." },
    { "The Two LEDs by USB",
      "GREEN off = fully charged. GREEN on = still charging. RED on = badge powered on. "
      "RED off = badge off. So a badge with green off and red on is charged and running." },
    { "Power Saving", "Use Airplane Mode, lower brightness, set a short screensaver timeout, and disable LEDs or set them to Off." },
};

static const HelpItem help_opensource[] = {
    { "Source Code", "Clip-Boy is open source. The full firmware lives at github.com/SafeHazard/Clip-Boy -- clone it, read it, learn from it." },
    { "Make It Yours", "Fork the repo, mod the code, flash your own build. Lighter-weight mods (custom collectibles + images) need no rebuild at all -- see SD Card Mods." },
    { "Contribute", "Found a bug or built something cool? Open an issue or a pull request on the repo. This badge got better because people shared." },
};

static const HelpItem help_radiation[] = {
    { "What It Is", "Not a real Geiger counter. Detects WiFi deauthentication packets in the area. It listens on 2.4 GHz only -- activity on a 5 GHz or 6 GHz network is invisible to it, whatever the gauge reads." },
    { "The Gauge", "The arc gauge rises as deauth packet rate increases. Think of it as measuring local chaos. The needle stops at 100; above that the label under it reads 100+, because the badge is seeing more than the dial can show." },
    { "Channels", "The selector above the gauge decides which channels the badge listens to. 1/6/11 (the default) covers the three channels almost every 2.4 GHz network uses. 1-14 sweeps everything, but spends only a fourteenth of its time on any one channel, so short bursts are easy to miss. Picking a single channel listens to that one continuously -- best for watching a specific network, but the badge is then deaf to the other thirteen. While the Geiger runs, the status bar shows the mode and the current rate from any screen. On Tools > Analyze > Deauth the status bar names the mode only when it is not 1-14." },
    { "Reading Zero", "A zero can mean three different things: nothing is happening, the activity is on a channel you are not listening to, or it is on 5/6 GHz. If you want certainty about one network, select its channel; if you want breadth, use 1-14." },
    { "Geiger Audio", "The gauge also clicks like a Geiger counter -- faster ticks as the deauth rate climbs, across five levels. Honors the Volume slider and goes silent on Mute." },
    { "Start/Stop", "STATS > Radiation. Tap Start to begin monitoring, Stop to end." },
};

// Plain-English definitions for jargon used elsewhere in Help and Tool Info.
// Keep entries clinical: definitions, not commentary.
static const HelpItem help_glossary[] = {
    { "AP",            "Access Point. The router-side of WiFi - the device that hands out connections." },
    { "BSSID",         "The MAC address of an access point's radio. Identifies a specific physical AP, even when two APs share an SSID." },
    { "SSID",          "The name a WiFi network advertises. The thing you tap on a phone." },
    { "Station",       "Any device that connects to an AP - your phone, laptop, smart bulb, etc." },
    { "Beacon",        "A short broadcast packet APs send roughly 10 times per second to announce themselves." },
    { "Probe",         "A request a phone or laptop sends asking 'is network X here?' for every WiFi name it remembers." },
    { "Deauth",        "A management frame that tells a station to disconnect from its AP. Spoofed deauths are how a device gets knocked off WiFi." },
    { "EAPOL",         "The four-message handshake used when a device authenticates to a WPA/WPA2 network. Captured handshakes can be cracked offline." },
    { "PMKID",         "A field optionally included in the AP's first WPA message. Can sometimes be cracked offline without needing a station to connect." },
    { "RSSI",          "Received Signal Strength Indicator. How loud an AP or station looks to your radio. Closer to zero = stronger." },
    { "WPA / WPA2 / WPA3", "Successive WiFi security protocols. WPA3 uses a different handshake (SAE) than WPA/WPA2 (EAPOL)." },
    { "Pineapple",     "A Hak5 product known for answering every probe request and pretending to be whatever network the phone is looking for. Also slang for any AP exhibiting that behavior." },
    { "Evil Twin",     "A rogue AP that broadcasts the same SSID as a legitimate one to lure devices into connecting." },
    { "Captive Portal","The login page some hotspots show before letting you online." },
    { "BLE",           "Bluetooth Low Energy. The low-power flavor of Bluetooth used by AirTags, fitness trackers, beacons, and modern peripherals." },
    { "OUI",           "Organizationally Unique Identifier. The first three bytes of a MAC address, which identify the chip vendor (Apple, Espressif, etc.)." },
    { "pcap",          "Packet capture file format. What Wireshark and similar tools read." },
};

// Task-oriented recipes that chain multiple tools together.
static const HelpItem help_workflows[] = {
    { "Capture WPA Handshake (own AP)", "0) Enable DATA > Settings > Allow PCAP Saving (off by default on Sn34k) or nothing gets written to file. 1) Tools > Utilities/Lists > Set Channel to your AP's channel. 2) Tools > Scan > APs (full), let it find your AP. 3) Tools > Utilities/Lists > Select AP, pick yours. 4) Tools > Analyze > EAPOL/PMKID, Start. 5) From another device you control on your own network, force a reconnect (toggle its WiFi off/on). 6) Stop, pull the SD card, crack the pcap with hashcat." },
    { "Find an AP physically", "1) Tools > Scan > APs (full), find the BSSID you care about. 2) Utilities/Lists > Select AP, pick it. 3) Tools > Monitor > RSSI, Start. 4) Walk around and watch the dBm value: closer to 0 = closer to the AP." },
#ifdef CLIPBOY_RES34RCH
    { "Populate Beacon List then spam", "1) Tools > Utilities/Lists > Add SSID (type entries) or Gen Rnd SSIDs. 2) Tools > Utilities/Lists > List SSIDs to verify. 3) Tools > Beacon Spam > List, Start; keep it short and only with obviously-fake SSIDs." },
#endif
    { "Is something tracking me?", "1) Tools > Detect > AirTag. 2) Walk a different route. 3) An AirTag that keeps appearing across moves is the worrying signal. Use Apple's 'Find Nearby Item' on an iPhone if you have one." },
    { "Walk the spectrum", "1) Tools > Monitor > Channel Stats. 2) Watch the per-channel rate update live. 3) Quiet conference rooms vs. main floors look very different - calibrates your sense of 'busy'." },
};

// SAOs: dim items are intentional jokes about gear that doesn't exist. Help
// users so they don't think the badge is broken.
static const HelpItem help_jokes[] = {
    { "SAOs", "The dim items under ITEMS > SAOs are jokes about gear that doesn't exist -- enjoy the descriptions. The one exception is 'Whether Radio' (not dimmed): tap it to open a simulated radio -- styled stations playing pre-recorded audio, not a real receiver. See the Simulated Radio section." },
    { "SAO Port", "Clip-Boy supports real SAOs compliant with v1.69bis via the SAO port on the right side of the enclosure. 3V3, GND, SDA and SCL are active; GPIO 1 & 2 are not." },
};

// STATS division pages (Status rollup + the L.E.E.T. system-vitals gag).
static const HelpItem help_stats[] = {
    { "Status Page", "STATS > Status shows the Clip-Boy mascot and totals up the stat bonuses from every collectible you've found." },
    { "L.E.E.T.", "STATS > L.E.E.T. is the badge's tongue-in-cheek riff on a classic RPG character-stats screen. The four rows spell it: cpu Load, mEmory (free DRAM/PSRAM), storagE (flash), upTime -- live system vitals dressed as 'stats', plus battery. The CPU load is a wink, not a real reading." },
};

// Orientation: the tour, the About/version/SKU page, reflashing, first-boot
// consent, and where to find the Legal notice.
static const HelpItem help_about[] = {
    { "Guided Tour", "First boot runs a quick Clip-Boy walkthrough. Replay it any time from DATA > Settings > Help, then tap Tour." },
    { "About", "DATA > Settings > About shows your firmware build, which SKU you have, and the build date." },
    { "Two Builds", "Clip-Boy ships as Sn34k-Boy (listen-only; active transmit tools compiled out) and Res34rch-Boy (adds active research tools). Missing tools means you're on Sn34k -- by design, not a bug. About tells you which you have." },
    { "Reflash", "Update or switch builds at flash.brycebadges.com from a computer using Chrome or Edge. Avoid 'Erase All Flash' -- it wipes your collectibles; a normal update keeps them." },
    { "First-Boot Notice", "On first boot, and again after the terms change, scroll the security-tools notice to the end and accept it before using the badge." },
    { "Credits", "DATA > Settings > Credits lists the people, libraries and open-source tools behind Clip-Boy, plus the contact path for IP inquiries." },
    { "Legal", "DATA > Settings > Legal has the full acceptable-use and interception notice. Short version: only use the tools on hardware you own or are authorized to test." },
};

// Intentional accommodations, framed as features. Keep it about "more than one
// way in", never about who needs it -- inclusive design, not a spotlight.
static const HelpItem help_accessibility[] = {
    { "More Than One Way In", "Clip-Boy tries to never make sound the only channel. Where something is audio, there's usually a way to see or read it too." },
    { "Read the Broadcast", "Every 'Whether Radio' station with speech carries a Transcript button -- a live, scrolling readout of what's playing -- so you can follow a station with the sound all the way off." },
    { "See It, Don't Hear It", "The radio's oscilloscope shows at a glance when a station is tuned in, and the Radiation screen's gauge shows the deauth rate as a moving needle. Both read fine with audio muted." },
    { "Text Size & Contrast", "DATA > Settings > Terminal Text sets the tool log to Small, Medium or Large. Themes include Flashbang, a high-contrast black-on-white palette, and Brightness is adjustable." },
    { "Any Color Vision", "The interface is near-monochrome by design -- one color on a dark (or, in Flashbang, a light) background -- so color is almost never the only cue: brightness, outline and shape carry the meaning, and a theme mostly just changes the hue. A couple of spots add red for emphasis (a flagged suspicious network name, which also shows a warning box, and the Radiation needle), but nothing is conveyed by color alone. The NeoPixel LEDs are decorative -- no status rides on their color." },
    { "Forgiving Controls", "Touch targets are sized for taps, not fine aim. HR codes scan as long as the sensor and code are parallel, and can be entered by hand if a tag won't cooperate; the radio tuning dial snaps in when you get close -- or set Settings > Radio Tuning to lock stations instantly." },
    { "Sound On Your Terms", "Tap, Scanning and Unlock cues each toggle independently, with a hard Mute -- add the audio feedback you want, or turn it all off." },
};

#define HELP_CAT(name, arr) { name, arr, sizeof(arr)/sizeof(arr[0]) }
static const HelpCategory help_categories[] = {
    HELP_CAT("Getting Started",  help_about),
    HELP_CAT("Accessibility",    help_accessibility),
    HELP_CAT("Navigation",       help_navigation),
    HELP_CAT("STATS",            help_stats),
    HELP_CAT("Glossary",         help_glossary),
    HELP_CAT("Tutorials",        help_workflows),
    HELP_CAT("Collectibles",     help_collectibles),
    HELP_CAT("Tools",            help_tools),
    HELP_CAT("Theremin",         help_theremin),
    HELP_CAT("Simulated Radio",  help_radio),
    HELP_CAT("Radiation",        help_radiation),
    HELP_CAT("LEDs",             help_leds),
    HELP_CAT("Settings",         help_settings),
    HELP_CAT("Screensaver",      help_screensaver),
    HELP_CAT("SD Card Mods",     help_sdcard),
    HELP_CAT("SAOs",             help_jokes),
    HELP_CAT("Open Source",      help_opensource),
    HELP_CAT("Trouble?",         help_trouble),
    HELP_CAT("Battery/Power",    help_battery),
};
#define NUM_HELP_CATS  (sizeof(help_categories) / sizeof(help_categories[0]))

static uint16_t help_cats_expanded = 0x0000;  // bit per category

static void help_cat_tap_cb(lv_event_t *e) {
    int32_t c = (int32_t)(intptr_t)lv_event_get_user_data(e);
    help_cats_expanded ^= (1 << c);
    bool expanded = help_cats_expanded & (1 << c);

    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    char buf[40];
    snprintf(buf, sizeof(buf), "%s %s", expanded ? "v" : ">",
             help_categories[c].name);
    lv_label_set_text(lbl, buf);

    // The items container is the next sibling after this button
    lv_obj_t *parent = lv_obj_get_parent(btn);
    int btn_idx = lv_obj_get_index(btn);
    lv_obj_t *items_cont = lv_obj_get_child(parent, btn_idx + 1);
    if (items_cont) {
        if (expanded)
            lv_obj_remove_flag(items_cont, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(items_cont, LV_OBJ_FLAG_HIDDEN);
    }
}

static void help_item_tap_cb(lv_event_t *e) {
    int32_t encoded = (int32_t)(intptr_t)lv_event_get_user_data(e);
    int cat = (encoded >> 8) & 0xFF;
    int item = encoded & 0xFF;
    if (cat < 0 || cat >= (int)NUM_HELP_CATS) return;
    if (item < 0 || item >= help_categories[cat].count) return;

    if (!right_pane) return;
    clear_children(right_pane);
    lv_obj_scroll_to_y(right_pane, 0, LV_ANIM_OFF);

    make_label(right_pane, help_categories[cat].items[item].title,
               &ui_font_pipboy_16, pip_highlight());

    lv_obj_t *txt = make_label(right_pane,
        help_categories[cat].items[item].text,
        &ui_font_pipboy_14, pip_primary());
    lv_label_set_long_mode(txt, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(txt, lv_pct(100));
}

static void show_help(lv_obj_t *cont) {
    clear_children(cont);
    content_teardown();   // SB2: `?` is reachable from EVERY screen, incl. a live poller
    lv_obj_scroll_to_y(cont, 0, LV_ANIM_OFF);
    help_cats_expanded = 0x0000;
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_set_style_pad_gap(cont, 0, 0);

    left_pane = create_left_list(cont);

    // Back + Tour share a row to save vertical real estate -- the
    // expandable category list below is the meat of this screen.
    lv_obj_t *row = lv_obj_create(left_pane);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_gap(row, 4, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back_btn = make_action_btn(row, "< Back", info_back_cb, NULL);
    lv_obj_set_flex_grow(back_btn, 1);
    lv_obj_set_style_pad_hor(back_btn, 4, 0);  // squashed; default 16px clips text

    lv_obj_t *tour_btn = make_action_btn(row, "Tour",
        [](lv_event_t *e) {
            (void)e;
            // Force the modal regardless of cfg.clippy_seen.
            cfg.clippy_seen = false;
            show_clippy_intro();
        }, NULL);
    lv_obj_set_flex_grow(tour_btn, 1);
    lv_obj_set_style_pad_hor(tour_btn, 4, 0);

    // Build expandable categories
    for (int c = 0; c < (int)NUM_HELP_CATS; c++) {
        char buf[40];
        snprintf(buf, sizeof(buf), "> %s", help_categories[c].name);
        lv_obj_t *cat_btn = make_list_btn(left_pane, buf,
            help_cat_tap_cb, (void *)(intptr_t)c);
        lv_obj_set_style_text_color(cat_btn, pip_highlight(), 0);

        // Items container (hidden by default)
        lv_obj_t *items_cont = lv_obj_create(left_pane);
        lv_obj_remove_style_all(items_cont);
        lv_obj_set_size(items_cont, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(items_cont, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(items_cont, 0, 0);
        lv_obj_set_style_pad_gap(items_cont, 0, 0);
        lv_obj_remove_flag(items_cont, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(items_cont, LV_OBJ_FLAG_HIDDEN);

        for (int i = 0; i < help_categories[c].count; i++) {
            int32_t encoded = (c << 8) | i;
            lv_obj_t *btn = make_list_btn(items_cont,
                help_categories[c].items[i].title,
                help_item_tap_cb, (void *)(intptr_t)encoded);
            lv_obj_set_style_pad_left(btn, 8, 0);
        }
    }

    right_pane = create_right_detail(cont);

    // Default view = the "online help" landing: a QR + URL to the phone-friendly
    // hosted Help hub. Centered column so the QR reads well on the dark UI.
    static const char HELP_URL[]  = "https://safehazard.github.io/Clip-Boy";
    static const char HELP_HOST[] = "safehazard.github.io/Clip-Boy";

    lv_obj_t *land = lv_obj_create(right_pane);
    lv_obj_remove_style_all(land);
    lv_obj_set_size(land, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(land, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(land, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(land, 0, 0);
    lv_obj_set_style_pad_gap(land, 4, 0);
    lv_obj_remove_flag(land, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *heading = make_label(land, "FULL HELP ON YOUR PHONE",
                                   &ui_font_pipboy_16, pip_highlight());
    lv_label_set_long_mode(heading, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(heading, lv_pct(100));
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);

    // Light card = quiet zone so a phone camera can lock onto the QR even
    // against the dark badge theme. Dark modules on white, theme-independent.
    lv_obj_t *card = lv_obj_create(land);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 4, 0);
    lv_obj_set_style_pad_all(card, 6, 0);  // white quiet-zone border
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *qr = lv_qrcode_create(card);
    lv_qrcode_set_size(qr, 120);
    lv_qrcode_set_dark_color(qr, lv_color_hex(0x101010));
    lv_qrcode_set_light_color(qr, lv_color_hex(0xFFFFFF));
    lv_qrcode_update(qr, HELP_URL, (uint32_t)(sizeof(HELP_URL) - 1));

    lv_obj_t *url = make_label(land, HELP_HOST,
                               &ui_font_pipboy_14, pip_highlight());
    lv_label_set_long_mode(url, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(url, lv_pct(100));
    lv_obj_set_style_text_align(url, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *hint = make_label(land, "Scan with a phone, or type it in.",
                                &ui_font_pipboy_14, pip_disabled());
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, lv_pct(100));
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
}

// ─── ABOUT ───────────────────────────────────────────────────────────────

#ifdef CLIPBOY_RES34RCH
#define CB_SKU_NAME "Res34rch-Boy"
#define CB_SKU_DESC "(active research)"
#define CB_VARIANT_SKU "res34rch"
#else
#define CB_SKU_NAME "Sn34k-Boy"
#define CB_SKU_DESC "(listen-only)"
#define CB_VARIANT_SKU "sn34k"
#endif

// Compact three-axis build identity (SKU / boot / build) so a unit reports
// exactly which firmware it's running. Each axis is a compile-time flag.
#ifdef BADGE_QUANTUM_RIFT
#define CB_VARIANT_BOOT "rift"
#else
#define CB_VARIANT_BOOT "public"
#endif
#ifdef TEST_HARNESS
#define CB_VARIANT_BUILD "test"
#else
#define CB_VARIANT_BUILD "prod"
#endif
// Post-con reveal (--postcon) shows the P5 unlock code on the keypad; append it
// as a suffix so a unit self-reports it (only present on post-con builds).
#ifdef CLIPBOY_POSTCON
#define CB_VARIANT_POSTCON " +postcon"
#else
#define CB_VARIANT_POSTCON ""
#endif
#define CB_VARIANT CB_VARIANT_SKU " / " CB_VARIANT_BOOT " / " CB_VARIANT_BUILD CB_VARIANT_POSTCON

// Deterministic build stamp (DC34-122). scripts/build.sh writes build_stamp.h
// (#define CB_BUILD_STAMP "<commit-date>+<sha>") so release bins are
// byte-reproducible from their commit. Ad-hoc builds without build.sh fall
// back to the wall-clock (non-reproducible, but those aren't the release path).
#if defined(__has_include)
#  if __has_include("build_stamp.h")
#    include "build_stamp.h"
#  endif
#endif
#ifndef CB_BUILD_STAMP
#  define CB_BUILD_STAMP __DATE__ "  " __TIME__
#endif

static void show_about(lv_obj_t *cont) {
    info_page_create(cont, "ABOUT CLIP-BOY");

    // Top row: build identity on the LEFT, mascot on the RIGHT (better use of
    // the wide screen than a centered mascot stacked above the text).
    lv_obj_t *top = lv_obj_create(cont);
    lv_obj_remove_style_all(top);
    lv_obj_set_width(top, lv_pct(100));
    lv_obj_set_height(top, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(top, 6, 0);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);

    // Build identity - shown on every build so a unit/SKU can be identified.
    lv_obj_t *build_lbl = make_label(top,
        "Firmware: " CB_SKU_NAME "\n"
        CB_SKU_DESC "\n"
        "Variant: " CB_VARIANT "\n"
        "Built: " CB_BUILD_STAMP,
        &ui_font_pipboy_14, pip_highlight());
    lv_label_set_long_mode(build_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(build_lbl, 1);

    // lv_image_set_scale() only transforms the RENDER; the widget still reserves
    // the full 153x192 source box, which inflates this row and pads Clippy with
    // dead space. Give it an explicit footprint at the SAME 153:192 aspect +
    // STRETCH (this LVGL lacks CONTAIN/COVER) so it scales without distortion and
    // the row no longer reserves 192px.
    lv_obj_t *mascot = make_clipboy_image(top);
    lv_obj_set_size(mascot, 64, 80);  // ~4-line build-text height -> ~1 blank line before reflash text
    lv_image_set_inner_align(mascot, LV_IMAGE_ALIGN_STRETCH);

    // Reflash / firmware updates — on-device disclosure of the self-serve flasher.
    // (DC34-82: on-device is an allowed post-purchase channel; the public store
    // stays silent. Wording is neutral and works for both SKUs.)
    lv_obj_t *flash_lbl = make_label(cont,
        "Firmware updates & reflash:\n"
        "flash.brycebadges.com\n\n"
        "From a computer (laptop or desktop, not a phone), open it in Chrome or "
        "Edge, plug the badge in over USB, and follow the prompts. Software "
        "updates and other builds live there too.\n"
        "Tip: don't choose \"Erase All Flash\" when reflashing, or you'll lose "
        "your collectibles.",
        &ui_font_pipboy_14, pip_highlight());
    lv_label_set_long_mode(flash_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(flash_lbl, lv_pct(100));

    lv_obj_t *desc = make_label(cont,
        "Clip-Boy is a retro-futuristic, wrist-mounted DEFCON 34 conference badge. It scans WiFi "
        "and Bluetooth, plays a LiDAR-driven theremin, and unlocks collectibles "
        "when you find HR codes around the con. Tap DATA > Settings > Help to "
        "learn the controls.\n\n"
        "Features:\n"
        "- ESP32 Marauder analysis suite\n"
        "- LiDAR theremin\n"
        "- HR Code collectibles\n"
        "- NeoPixel LED control\n"
        "- Deauth Geiger counter\n\n"
        "Hardware:\n"
        "- ESP32-S3 (8MB PSRAM, 16MB flash)\n"
        "- 2.8\" 320x240 touch display\n"
        "- VL53L5CX 8x8 LiDAR sensor\n"
        "- 8x WS2812B NeoPixel LEDs\n"
        "- Stereo speakers\n"
        "- 2200mAh LiPo battery\n"
        "- SAO connector\n\n"
        "Open source:\n"
        "github.com/SafeHazard/Clip-Boy\n\n"
        "Built with too much caffeine\n"
        "and not enough sleep.",
        &ui_font_pipboy_14, pip_primary());
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(desc, lv_pct(100));
}

// ─────────────────────── DATA > Settings ──────────────────────────────────

// ── Collectible save export/import (SD) — DC34-92 ──────────────────────────
// Top-layer OK modal for result messages.
static void coll_msg_modal(const char *title, const char *msg) {
    lv_obj_t *modal = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(modal);
    lv_obj_set_size(modal, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_80, 0);
    lv_obj_remove_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(modal, 0, 0);

    lv_obj_t *box = lv_obj_create(modal);
    lv_obj_remove_style_all(box);
    // Height auto-fits the message (some are 1 line, some are several); width fixed
    // so the label wraps, capped so it can't exceed the screen.
    lv_obj_set_width(box, 286);
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(box, SCREEN_H - 8, 0);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, pip_bg(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, pip_highlight(), 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_pad_all(box, 12, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(box, 8, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    make_label(box, title, &ui_font_pipboy_18, pip_highlight());
    lv_obj_t *m = make_label(box, msg, &ui_font_pipboy_14, pip_primary());
    lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(m, lv_pct(100));
    lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *ok = lv_button_create(box);
    lv_obj_set_size(ok, 100, 30);
    lv_obj_set_style_bg_color(ok, pip_highlight(), 0);
    lv_obj_t *lo = lv_label_create(ok);
    lv_label_set_text(lo, "OK");
    lv_obj_set_style_text_font(lo, &ui_font_pipboy_14, 0);
    lv_obj_set_style_text_color(lo, pip_bg(), 0);
    lv_obj_center(lo);
    lv_obj_add_event_cb(ok, [](lv_event_t *e) {
        lv_obj_t *b = (lv_obj_t *)lv_event_get_target(e);
        lv_obj_delete(lv_obj_get_parent(lv_obj_get_parent(b)));  // ok -> box -> modal
    }, LV_EVENT_CLICKED, NULL);
}

static const char *coll_import_msg(CollImport r) {
    switch (r) {
        case COLL_IMP_NO_SD:       return "No SD card detected.";
        case COLL_IMP_NO_FILE:     return "No " COLL_SAV_PATH " found. Check the SD card is inserted, then Export one first or copy it over.";
        case COLL_IMP_BAD_SIZE:    return "That file isn't a Clip-Boy save (wrong size).";
        case COLL_IMP_BAD_MAGIC:   return "That file isn't a Clip-Boy save.";
        case COLL_IMP_BAD_VERSION: return "Save is from newer firmware. Update the badge first.";
        case COLL_IMP_BAD_CRC:     return "Save file is corrupted (checksum failed).";
        case COLL_IMP_WRONG_BADGE: return "This backup is from a different badge (or the file is corrupted). A backup only restores on the badge that created it.";
        default:                   return "Import failed.";
    }
}

static void coll_do_export(lv_event_t *e) {
    (void)e;
    if (coll_export_sd())
        coll_msg_modal("BACKED UP",
            "Your finds + ARG progress were saved to the SD card:\n" COLL_SAV_PATH "\n\n"
            "A backup for THIS badge -- it won't restore on another.");
    else
        coll_msg_modal("BACKUP FAILED",
            "Couldn't write to SD. Insert a card formatted as a 1 GB FAT partition.");
}

// ── Import merge/overwrite flow (DC34-92) ──────────────────────────────────
// Validate the SD file FIRST (coll_import_load), then let the user choose
// MERGE (union with current finds) vs OVERWRITE (replace). Overwrite is
// destructive so it gets its own are-you-sure. The validated bits live here
// between the choice modal and applying them.
static CollSaveData s_coll_import_data;   // v2: bits + ARG progress, staged between validate + apply

// Generic full-screen-dim modal + centered box; returns the box via *box_out.
static lv_obj_t *coll_make_modal(lv_obj_t **box_out, int box_h) {
    lv_obj_t *modal = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(modal);
    lv_obj_set_size(modal, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_80, 0);
    lv_obj_remove_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(modal, 0, 0);

    lv_obj_t *box = lv_obj_create(modal);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 292, box_h);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, pip_bg(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, pip_highlight(), 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_pad_all(box, 12, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(box, 6, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    *box_out = box;
    return modal;
}

enum CollBtnStyle { CBTN_PRIMARY, CBTN_OUTLINE, CBTN_GHOST };

// Full-width modal button (PRIMARY = filled/safe-default, OUTLINE = bordered,
// GHOST = dim). Large touch target for 50-yr-old eyes.
static lv_obj_t *coll_modal_btn(lv_obj_t *box, const char *txt, int style, lv_event_cb_t cb) {
    lv_obj_t *b = lv_button_create(box);
    lv_obj_set_width(b, lv_pct(100));
    lv_obj_set_height(b, 32);
    lv_color_t txtcol = pip_primary();
    if (style == CBTN_PRIMARY) {
        lv_obj_set_style_bg_color(b, pip_highlight(), 0);
        txtcol = pip_bg();
    } else if (style == CBTN_OUTLINE) {
        lv_obj_set_style_bg_color(b, pip_bg(), 0);
        lv_obj_set_style_border_color(b, pip_highlight(), 0);
        lv_obj_set_style_border_width(b, 1, 0);
    } else { // CBTN_GHOST
        lv_obj_set_style_bg_color(b, pip_border(), 0);
    }
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &ui_font_pipboy_14, 0);
    lv_obj_set_style_text_color(l, txtcol, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

// Delete the modal owning a button (button -> box -> modal).
static void coll_close_modal_from(lv_obj_t *btn) {
    lv_obj_delete(lv_obj_get_parent(lv_obj_get_parent(btn)));
}

// Count catalog items whose bit is set in `bits` (phantom/blacklist bits not in
// the catalog are ignored) — the true "N finds in file" that will import.
static int coll_count_in_bits(const uint8_t bits[32]) {
    int n = 0;
    for (uint16_t i = 0; i < coll_count; i++) {
        uint8_t id = coll_items[i].id;
        if ((bits[id / 8] >> (id % 8)) & 1) n++;
    }
    return n;
}

static void coll_show_import_choice(void);   // fwd (overwrite "Back" returns here)

static void coll_import_do_merge(lv_event_t *e) {
    coll_close_modal_from((lv_obj_t *)lv_event_get_target(e));
    int added = coll_apply_import(&s_coll_import_data, true);
    rebuild_content();
    char buf[64];
    snprintf(buf, sizeof(buf), "Added %d new.\nYou now have %d total.",
             added, coll_count_found());
    coll_msg_modal("MERGED", buf);
}

static void coll_import_do_overwrite(lv_event_t *e) {
    coll_close_modal_from((lv_obj_t *)lv_event_get_target(e));
    coll_apply_import(&s_coll_import_data, false);
    rebuild_content();
    char buf[48];
    snprintf(buf, sizeof(buf), "You now have %d finds.", coll_count_found());
    coll_msg_modal("OVERWRITTEN", buf);
}

static void coll_import_overwrite_back(lv_event_t *e) {
    coll_close_modal_from((lv_obj_t *)lv_event_get_target(e));
    coll_show_import_choice();   // back to the 3-way choice
}

// OVERWRITE chosen -> destructive are-you-sure (Back is the safe default).
static void coll_import_overwrite_confirm(lv_event_t *e) {
    coll_close_modal_from((lv_obj_t *)lv_event_get_target(e));
    lv_obj_t *box;
    coll_make_modal(&box, 190);
    make_label(box, "OVERWRITE?", &ui_font_pipboy_18, pip_highlight());
    lv_obj_t *w = make_label(box,
        "This DELETES your current finds and replaces them with the file. No undo.",
        &ui_font_pipboy_14, pip_primary());
    lv_label_set_long_mode(w, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(w, lv_pct(100));
    lv_obj_set_style_text_align(w, LV_TEXT_ALIGN_CENTER, 0);
    coll_modal_btn(box, "Back", CBTN_PRIMARY, coll_import_overwrite_back);
    coll_modal_btn(box, "Yes, Erase & Import", CBTN_OUTLINE, coll_import_do_overwrite);
}

static void coll_import_choice_cancel(lv_event_t *e) {
    coll_close_modal_from((lv_obj_t *)lv_event_get_target(e));
}

// The 3-way choice modal shown once the file validates.
static void coll_show_import_choice(void) {
    lv_obj_t *box;
    coll_make_modal(&box, 216);
    char title[32];
    snprintf(title, sizeof(title), "RESTORE - %d FINDS", coll_count_in_bits(s_coll_import_data.bits));
    make_label(box, title, &ui_font_pipboy_18, pip_highlight());
    lv_obj_t *w = make_label(box,
        "Keep your finds and add the file's, or wipe and replace?",
        &ui_font_pipboy_14, pip_primary());
    lv_label_set_long_mode(w, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(w, lv_pct(100));
    lv_obj_set_style_text_align(w, LV_TEXT_ALIGN_CENTER, 0);
    coll_modal_btn(box, "MERGE - Keep + Add",     CBTN_PRIMARY, coll_import_do_merge);
    coll_modal_btn(box, "OVERWRITE - Erase Mine", CBTN_OUTLINE, coll_import_overwrite_confirm);
    coll_modal_btn(box, "Cancel",                 CBTN_GHOST,   coll_import_choice_cancel);
}

// Settings "Import" button: validate the SD file first; a bad file goes
// straight to IMPORT FAILED (no point asking merge/overwrite), a good file
// opens the merge/overwrite choice.
static void coll_import_begin(lv_event_t *e) {
    (void)e;
    CollImport r = coll_import_load(&s_coll_import_data);
    if (r != COLL_IMP_OK) {
        coll_msg_modal("IMPORT FAILED", coll_import_msg(r));
        return;
    }
    coll_show_import_choice();
}

// Settings section header: an accent-colored caption with a top-divider line
// (suppressed on the first). Turns the long scroll into scannable groups so
// related controls (e.g. Volume + Mute) read as one block. Non-interactive.
// Section-jump chips (DATA>Settings): the 6 section headers are captured as they build;
// the chip callbacks scroll to them. File-scope so the no-capture button lambdas can reach it.
static lv_obj_t *settings_jump_hdr[6] = {};

static lv_obj_t* settings_section(lv_obj_t *cont, const char *text, bool first = false) {
    lv_obj_t *h = make_label(cont, text, &ui_font_pipboy_16, pip_highlight());
    lv_obj_set_width(h, lv_pct(100));
    lv_obj_set_style_pad_bottom(h, 2, 0);
    lv_obj_set_style_pad_top(h, first ? 0 : 6, 0);
    if (!first) {
        lv_obj_set_style_border_side(h, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_color(h, pip_border(), 0);
        lv_obj_set_style_border_width(h, 1, 0);
    }
    return h;
}

static void build_data_settings(lv_obj_t *cont) {
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, 4, 0);
    lv_obj_set_style_pad_gap(cont, 4, 0);
    lv_obj_add_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(cont, &style_scrollbar, LV_PART_SCROLLBAR);

    // Reset row refs -- the previous rebuild deleted their LVGL objects.
    ss_style_row_label  = NULL;
    ss_style_dropdown   = NULL;
    ss_dim_row_label    = NULL;
    ss_dim_switch       = NULL;
    ss_bright_row_label = NULL;
    ss_bright_slider    = NULL;
    ss_bright_readout   = NULL;
    for (int i = 0; i < 6; i++) settings_jump_hdr[i] = NULL;

    // Helper: pad before a 40px switch so it right-aligns with slider readout.
    auto add_switch_row = [&](const char *text, bool checked, lv_event_cb_t cb) -> lv_obj_t* {
        lv_obj_t *row = make_uniform_settings_row(cont);
        settings_label(row, text);
        settings_gap(row, 6);
        settings_gap(row, SETTINGS_CONTROL_W - 40);  // push switch to right edge
        lv_obj_t *sw = lv_switch_create(row);
        lv_obj_set_size(sw, 40, 20);
        lv_obj_set_style_bg_color(sw, pip_border(), LV_PART_MAIN);
        lv_obj_set_style_bg_color(sw, pip_primary(), LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(sw, pip_highlight(), LV_PART_KNOB);
        if (checked) lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
        return sw;
    };

    // Helper: dropdown row pinned to SETTINGS_CONTROL_W.
    auto add_dropdown_row = [&](const char *text, const char *opts, uint16_t sel, lv_event_cb_t cb) -> lv_obj_t* {
        lv_obj_t *row = make_uniform_settings_row(cont);
        settings_label(row, text);
        settings_gap(row, 6);
        lv_obj_t *dd = make_dropdown(row, opts);
        lv_dropdown_set_selected(dd, sel);
        lv_obj_set_width(dd, SETTINGS_CONTROL_W);
        lv_obj_add_event_cb(dd, cb, LV_EVENT_VALUE_CHANGED, NULL);
        return dd;
    };

    // --- Section jump-nav: 6 chips across the top; each scrolls its section to the top.
    // They live in the scroll column (scroll away as you page down; re-tap the DATA>Settings
    // tab to return to the top). Headers are captured into settings_jump_hdr[] as each
    // section builds below, so the chip callbacks resolve them at tap time.
    {
        lv_obj_t *jrow = lv_obj_create(cont);
        lv_obj_remove_style_all(jrow);
        lv_obj_set_size(jrow, lv_pct(100), 36);
        lv_obj_set_flex_flow(jrow, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_gap(jrow, 2, 0);
        lv_obj_remove_flag(jrow, LV_OBJ_FLAG_SCROLLABLE);
        static const char *jlabels[6] = { "DISP", "AUDIO", "CRT", "SYS", "INFO", "RESET" };
        for (int i = 0; i < 6; i++) {
            lv_obj_t *b = lv_button_create(jrow);
            lv_obj_remove_style_all(b);
            lv_obj_set_flex_grow(b, 1);
            lv_obj_set_height(b, 36);
            lv_obj_set_style_bg_color(b, pip_bg_dark(), 0);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(b, pip_border(), 0);
            lv_obj_set_style_border_width(b, 1, 0);
            lv_obj_set_style_radius(b, 0, 0);
            lv_obj_set_style_pad_all(b, 0, 0);
            lv_obj_set_style_bg_color(b, pip_highlight(), LV_STATE_PRESSED);
            lv_obj_add_event_cb(b, [](lv_event_t *e){
                int idx = (int)(intptr_t)lv_event_get_user_data(e);
                lv_obj_t *hdr = settings_jump_hdr[idx];
                if (hdr && lv_obj_is_valid(hdr))
                    lv_obj_scroll_to_y(lv_obj_get_parent(hdr), lv_obj_get_y(hdr), LV_ANIM_ON);
            }, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            lv_obj_t *l = lv_label_create(b);
            lv_label_set_text(l, jlabels[i]);
            lv_obj_set_style_text_font(l, &ui_font_pipboy_14, 0);
            lv_obj_set_style_text_color(l, pip_primary(), 0);
            lv_obj_center(l);
        }
    }

    settings_jump_hdr[0] = settings_section(cont, "DISPLAY", /*first=*/true);

    // --- Theme --- base 3, +rift colorways, + Quanta ONCE EARNED (the ARG reward).
    // theme_dd_map[slot] -> theme_presets index (selection mapped in theme_changed_cb).
    {
        String topts = "Mojave\nRibbit City\nFlashbang";
        theme_dd_map[0] = 0; theme_dd_map[1] = 1; theme_dd_map[2] = 2; theme_dd_count = 3;
#ifdef BADGE_QUANTUM_RIFT
        topts += "\nOverseer\nSpace Badge";
        theme_dd_map[3] = THEME_OVERSEER; theme_dd_map[4] = THEME_SPACE_BADGE; theme_dd_count = 5;
#endif
        if (arg_quanta_earned()) {           // gated: only after the trial is complete
            topts += "\nQuanta";
            theme_dd_map[theme_dd_count] = THEME_QUANTA; theme_dd_count++;
        }
        // Completionist reward: ALWAYS listed (so it's discoverable in the dropdown),
        // but shows its locked progress until every collectible is found. Selecting
        // it while locked is rejected + hinted in theme_changed_cb.
        if (coll_all_found()) {
            topts += "\nCustom";
        } else {
            int togo = (int)coll_count - coll_count_found(); if (togo < 1) togo = 1;
            char cl[36]; snprintf(cl, sizeof(cl), "\nCustom (%d to go)", togo);
            topts += cl;
        }
        theme_dd_map[theme_dd_count] = THEME_CUSTOM; theme_dd_count++;
        uint16_t tsel = 0;
        for (uint8_t i = 0; i < theme_dd_count; i++) if (theme_dd_map[i] == cur_theme_idx) tsel = i;
        add_dropdown_row("Theme", topts.c_str(), tsel, theme_changed_cb);
    }

    // Once unlocked, a "Customize" button to (re)open the hue picker. While locked,
    // the dropdown's "Custom (N to go)" entry carries the visible progress, so no
    // extra row is needed then.
    if (coll_all_found()) {
        lv_obj_t *row = make_uniform_settings_row(cont);
        settings_label(row, "Custom Theme");
        settings_gap(row, 6);
        lv_obj_t *b = lv_button_create(row);
        lv_obj_set_style_bg_color(b, pip_highlight(), 0);
        lv_obj_set_height(b, 30);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_t *l = make_label(b, "Customize", &ui_font_pipboy_14, pip_bg());
        lv_obj_center(l);
        lv_obj_add_event_cb(b, [](lv_event_t *e) { (void)e; show_hue_picker(); },
                            LV_EVENT_CLICKED, NULL);
    }

    // Tool log / scan "terminal" text size (Small 14 / Medium 16 / Large 18).
    add_dropdown_row("Terminal Text", "Small\nMedium\nLarge", cfg.term_font, term_font_cb);

    // --- Brightness ---
    {
        lv_obj_t *row = make_uniform_settings_row(cont);
        settings_label(row, "Brightness");
        settings_gap(row, 6);
        lv_obj_t *sl = lv_slider_create(row);
        settings_size_slider(sl);
        lv_slider_set_range(sl, 10, 100);
        lv_slider_set_value(sl, cfg.brightness, LV_ANIM_OFF);
        lv_obj_t *lbl = settings_add_readout(row, cfg.brightness);
        lv_obj_add_event_cb(sl, brightness_slider_cb, LV_EVENT_VALUE_CHANGED, lbl);
    }

    settings_jump_hdr[1] = settings_section(cont, "AUDIO");

    // --- Volume ---
    {
        lv_obj_t *row = make_uniform_settings_row(cont);
        settings_label(row, "Volume");
        settings_gap(row, 6);
        settings_vol_slider = lv_slider_create(row);
        settings_size_slider(settings_vol_slider);
        lv_slider_set_range(settings_vol_slider, 0, 100);
        lv_slider_set_value(settings_vol_slider, theremin_volume, LV_ANIM_OFF);
        settings_vol_label = settings_add_readout(row, theremin_volume);
        lv_obj_add_event_cb(settings_vol_slider, volume_slider_cb,
                            LV_EVENT_VALUE_CHANGED, settings_vol_label);
    }

    // --- Mute (grouped with Volume) ---
    add_switch_row("Mute", !cfg.sound, sound_cb);

    // --- Tap Sounds (UI click, independent of Mute) ---
    add_switch_row("Tap Sounds", cfg.ui_click,
        [](lv_event_t *e) {
            lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
            cfg.ui_click = lv_obj_has_state(sw, LV_STATE_CHECKED);
            cfg_save_ui_click();
        });

    // --- Scanning Sounds (ambient loop while scanning an HR code) ---
    add_switch_row("Scanning Sounds", cfg.scan_sound,
        [](lv_event_t *e) {
            lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
            cfg.scan_sound = lv_obj_has_state(sw, LV_STATE_CHECKED);
            cfg_save_scan_sound();
        });

    // --- Unlock Tone (rising tone during the hold-to-unlock gesture) ---
    add_switch_row("Unlock Tone", cfg.ss_unlock_tone,
        [](lv_event_t *e) {
            lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
            cfg.ss_unlock_tone = lv_obj_has_state(sw, LV_STATE_CHECKED);
            cfg_save_ss_unlock_tone();
        });

    settings_jump_hdr[2] = settings_section(cont, "SCREEN & CRT");

    // --- Screensaver timeout ---
    add_dropdown_row("Screensaver", "15s\n30s\n60s\n2m\n5m\nNever", cfg.disp_off, disp_off_cb);

    // --- Screensaver style ---
    {
        lv_obj_t *row = make_uniform_settings_row(cont);
        ss_style_row_label = settings_label(row, "Screensaver Style");
        settings_gap(row, 6);
        // "Flying Clippy" appears only once the ARG is solved (reward, like the
        // Rubber Ducky LED theme / Quanta) -- non-ARG players never see it.
        // Display order != stored value: cfg.ss_style stays 0=Clip-Boy/1=Blank/
        // 2=Flying Clippy (referenced all over the saver code), but "Blank" must
        // always render last, so ss_style_ids[] maps the row index -> stored value.
        static int ss_style_ids[3];
        int ss_n = 0;
        String ss_opts;
        auto add_ss = [&](const char *name, int id) {
            if (ss_n) ss_opts += "\n";
            ss_opts += name;
            ss_style_ids[ss_n++] = id;
        };
        add_ss("Clip-Boy", 0);
        if (arg_quanta_earned()) add_ss("Flying Clippy", 2);  // ARG-solve reward
        add_ss("Blank", 1);                                   // "Blank" always sorts last
        ss_style_dropdown = make_dropdown(row, ss_opts.c_str());
        for (int i = 0; i < ss_n; i++)                        // preselect the active style
            if (ss_style_ids[i] == (int)cfg.ss_style) { lv_dropdown_set_selected(ss_style_dropdown, i); break; }
        lv_obj_set_width(ss_style_dropdown, SETTINGS_CONTROL_W);
        lv_obj_add_event_cb(ss_style_dropdown, [](lv_event_t *e) {
            lv_obj_t *dd = (lv_obj_t *)lv_event_get_target(e);
            int idx = (int)lv_dropdown_get_selected(dd);
            if (idx >= 0 && idx < 3) cfg.ss_style = (uint8_t)ss_style_ids[idx];
            cfg_save_ss_style();
            ss_settings_update_enables();
        }, LV_EVENT_VALUE_CHANGED, NULL);
    }

    // --- Idle Brightness (only meaningful when screensaver fires + Clip-Boy) ---
    {
        lv_obj_t *row = make_uniform_settings_row(cont);
        ss_bright_row_label = settings_label(row, "Idle Brightness");
        settings_gap(row, 6);
        ss_bright_slider = lv_slider_create(row);
        settings_size_slider(ss_bright_slider);
        lv_slider_set_range(ss_bright_slider, 1, 100);
        lv_slider_set_value(ss_bright_slider, cfg.ss_brightness, LV_ANIM_OFF);
        ss_bright_readout = settings_add_readout(row, cfg.ss_brightness);
        lv_obj_add_event_cb(ss_bright_slider, ss_bright_slider_cb,
                            LV_EVENT_VALUE_CHANGED, ss_bright_readout);
    }

    // --- Dim LEDs when idle ---
    {
        lv_obj_t *row = make_uniform_settings_row(cont);
        ss_dim_row_label = settings_label(row, "Dim LEDs when idle");
        settings_gap(row, 6);
        settings_gap(row, SETTINGS_CONTROL_W - 40);
        ss_dim_switch = lv_switch_create(row);
        lv_obj_set_size(ss_dim_switch, 40, 20);
        lv_obj_set_style_bg_color(ss_dim_switch, pip_border(), LV_PART_MAIN);
        lv_obj_set_style_bg_color(ss_dim_switch, pip_primary(), LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(ss_dim_switch, pip_highlight(), LV_PART_KNOB);
        if (cfg.ss_leds_off) lv_obj_add_state(ss_dim_switch, LV_STATE_CHECKED);
        lv_obj_add_event_cb(ss_dim_switch, [](lv_event_t *e) {
            lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
            cfg.ss_leds_off = lv_obj_has_state(sw, LV_STATE_CHECKED);
            cfg_save_ss_leds_off();
        }, LV_EVENT_VALUE_CHANGED, NULL);
    }

    // Apply enable cascade now that all screensaver-dependent rows exist
    ss_settings_update_enables();

    // --- CRT Scanlines ---
    add_switch_row("CRT Scanlines", cfg.crt_scanlines,
        [](lv_event_t *e) {
            lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
            cfg.crt_scanlines = lv_obj_has_state(sw, LV_STATE_CHECKED);
            cfg_save_crt_scanlines();
            crt_apply();
        });

    // --- CRT V-Roll ---
    add_switch_row("CRT V-Roll", cfg.crt_flicker,
        [](lv_event_t *e) {
            lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
            cfg.crt_flicker = lv_obj_has_state(sw, LV_STATE_CHECKED);
            cfg_save_crt_flicker();
            crt_apply();
        });

    settings_jump_hdr[3] = settings_section(cont, "SYSTEM");

    // --- Verify Scans (verify-first: show the scan's pattern to confirm before unlock) ---
    add_switch_row("Verify each scan", cfg.manual_confirm,
        [](lv_event_t *e) {
            lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
            cfg.manual_confirm = lv_obj_has_state(sw, LV_STATE_CHECKED);
            cfg_save_manual_confirm();
        });

    // --- Help button visibility ---
    add_switch_row("Help Button", cfg.help_btn,
        [](lv_event_t *e) {
            lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
            cfg.help_btn = lv_obj_has_state(sw, LV_STATE_CHECKED);
            cfg_save_help_btn();
            if (btn_shelp) {
                if (cfg.help_btn) lv_obj_remove_flag(btn_shelp, LV_OBJ_FLAG_HIDDEN);
                else              lv_obj_add_flag(btn_shelp, LV_OBJ_FLAG_HIDDEN);
            }
        });

    // --- Tuning Drift (radio tune-in minigame; grouped with Radio Alerts) ---
    add_dropdown_row("Radio Tuning", "Disabled\nOnce Per Boot\nEvery Access", cfg.radio_drift, radio_drift_cb);

    // --- Radio Alerts (reminder nudges + new-station-unlock popups) ---
    // Switch ON = alerts on (cfg.radio_reminder_off == false). Lets a user who
    // opted out from a station modal turn the notifications back on.
    add_switch_row("Radio Alerts", !cfg.radio_reminder_off,
        [](lv_event_t *e) {
            lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
            cfg.radio_reminder_off = !lv_obj_has_state(sw, LV_STATE_CHECKED);
            cfg_save_radio_discovery();
        });

    // --- Airplane mode ---
    add_switch_row("Airplane mode", cfg.airplane, airplane_cb);
    // Exclusive Input -- custom row (no fixed 132px label width, unlike
    // add_switch_row) so the full "(Serial/Screen)" clarifier fits on ONE line; a
    // growing spacer keeps the switch pinned to the right edge.
    {
        lv_obj_t *row = make_uniform_settings_row(cont);
        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, "Exclusive Input (Serial/Screen)");
        lv_obj_set_style_text_font(lbl, &ui_font_pipboy_14, 0);
        lv_obj_set_style_text_color(lbl, pip_primary(), 0);
        lv_obj_t *sp = lv_obj_create(row);
        lv_obj_remove_style_all(sp);
        lv_obj_set_height(sp, 1);
        lv_obj_set_flex_grow(sp, 1);
        lv_obj_t *sw = lv_switch_create(row);
        lv_obj_set_size(sw, 40, 20);
        lv_obj_set_style_bg_color(sw, pip_border(), LV_PART_MAIN);
        lv_obj_set_style_bg_color(sw, pip_primary(), LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(sw, pip_highlight(), LV_PART_KNOB);
        if (cfg.exclusive_input) lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_add_event_cb(sw, exin_cb, LV_EVENT_VALUE_CHANGED, NULL);
    }

    // --- Allow PCAP Saving (DC34-147): gates packet capture to SD/LittleFS.
    //     Fail-safe default: off on Sn34k, on for Res34rch. Drives ClipBoy's
    //     SavePCAP setting. ---
    add_switch_row("Allow PCAP Saving", cfg.allow_pcap,
        [](lv_event_t *e) {
            lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
            cfg.allow_pcap = lv_obj_has_state(sw, LV_STATE_CHECKED);
            cfg_save_allow_pcap();
            // Sets the Marauder in-memory SavePCAP gate (loadSetting reads it). Now fast:
            // the vendored Settings::saveSetting no longer rewrites /settings.json to SPIFFS
            // (that synchronous flash write froze the UI). Persistence is via our NVS
            // cfg.allow_pcap, re-synced to the gate every boot. See THIRD_PARTY.md.
            cb.setSavePCAP(cfg.allow_pcap);
        });

    // --- Sensor Calibration (per-zone LiDAR flat-field for the HR scanner) ---
    {
        lv_obj_t *btn_cal = lv_button_create(cont);
        lv_obj_remove_style_all(btn_cal);
        lv_obj_add_style(btn_cal, &style_list_btn, 0);
        lv_obj_add_style(btn_cal, &style_list_btn_pressed, LV_STATE_PRESSED);
        lv_obj_set_width(btn_cal, lv_pct(100));
        lv_obj_set_height(btn_cal, LV_SIZE_CONTENT);
        lv_obj_t *lbl_cal = lv_label_create(btn_cal);
        lv_label_set_text(lbl_cal, "Scanner Calibration");
        lv_obj_set_style_text_font(lbl_cal, &ui_font_pipboy_16, 0);
        lv_obj_set_style_text_color(lbl_cal, pip_primary(), 0);
        lv_obj_t *cal_arrow = lv_label_create(btn_cal);   // ">" drill-in affordance (opens modal)
        lv_label_set_text(cal_arrow, ">");
        lv_obj_set_style_text_font(cal_arrow, &ui_font_pipboy_16, 0);
        lv_obj_set_style_text_color(cal_arrow, pip_primary(), 0);
        lv_obj_align(cal_arrow, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_add_event_cb(btn_cal, [](lv_event_t *e) {
            (void)e; show_sensor_cal_modal();
        }, LV_EVENT_CLICKED, NULL);
    }

    settings_jump_hdr[4] = settings_section(cont, "INFO");

    // --- Info buttons ---
    struct InfoBtn { const char *name; void (*fn)(lv_obj_t*); };
    static const InfoBtn info_btns[] = {
        { "Credits", show_credits },
        { "Legal",   show_legal },
        { "About",   show_about },
        { "Help",    show_help },
    };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *btn = lv_button_create(cont);
        lv_obj_remove_style_all(btn);
        lv_obj_add_style(btn, &style_list_btn, 0);
        lv_obj_add_style(btn, &style_list_btn_pressed, LV_STATE_PRESSED);
        lv_obj_set_width(btn, lv_pct(100));
        lv_obj_set_height(btn, LV_SIZE_CONTENT);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, info_btns[i].name);
        lv_obj_set_style_text_font(lbl, &ui_font_pipboy_16, 0);
        lv_obj_set_style_text_color(lbl, pip_primary(), 0);
        // ">" affordance, right-aligned, signalling "opens another screen"
        // (vs the inline controls above). Absolute align, not flex.
        lv_obj_t *arrow = lv_label_create(btn);
        lv_label_set_text(arrow, ">");
        lv_obj_set_style_text_font(arrow, &ui_font_pipboy_16, 0);
        lv_obj_set_style_text_color(arrow, pip_primary(), 0);
        lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_add_event_cb(btn, [](lv_event_t *e) {
            void (*fn)(lv_obj_t*) = (void (*)(lv_obj_t*))lv_event_get_user_data(e);
            if (fn && content_obj) fn(content_obj);
        }, LV_EVENT_CLICKED, (void*)info_btns[i].fn);
    }

    settings_jump_hdr[5] = settings_section(cont, "POWER & RESET");

    // --- Backup / Restore Progress (SD) ---
    // Gated on boot-time SD detection: SD.begin() only runs at boot, so if no card
    // was present then, back up / restore can't work -- show a hint, not dead buttons.
    if (!coll_sd_available) {
        lv_obj_t *nosd = make_label(cont,
            "Insert an SD card (1 GB FAT) + reboot\nto back up / restore your progress.",
            &ui_font_pipboy_14, pip_dim());
        lv_label_set_long_mode(nosd, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(nosd, lv_pct(100));
    } else {
        // SD-format hint ABOVE the buttons so cards are formatted right first.
        lv_obj_t *sd_hint = make_label(cont,
            "SD card: format as a 1 GB FAT partition.",
            &ui_font_pipboy_14, pip_dim());
        lv_label_set_long_mode(sd_hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(sd_hint, lv_pct(100));

        lv_obj_t *btn_exp = lv_button_create(cont);
        lv_obj_remove_style_all(btn_exp);
        lv_obj_add_style(btn_exp, &style_list_btn, 0);
        lv_obj_add_style(btn_exp, &style_list_btn_pressed, LV_STATE_PRESSED);
        lv_obj_set_width(btn_exp, lv_pct(100));
        lv_obj_set_height(btn_exp, LV_SIZE_CONTENT);
        lv_obj_t *le = lv_label_create(btn_exp);
        lv_label_set_text(le, "Backup Progress to SD");
        lv_obj_set_style_text_font(le, &ui_font_pipboy_16, 0);
        lv_obj_set_style_text_color(le, pip_primary(), 0);
        lv_obj_add_event_cb(btn_exp, coll_do_export, LV_EVENT_CLICKED, NULL);

        lv_obj_t *btn_imp = lv_button_create(cont);
        lv_obj_remove_style_all(btn_imp);
        lv_obj_add_style(btn_imp, &style_list_btn, 0);
        lv_obj_add_style(btn_imp, &style_list_btn_pressed, LV_STATE_PRESSED);
        lv_obj_set_width(btn_imp, lv_pct(100));
        lv_obj_set_height(btn_imp, LV_SIZE_CONTENT);
        lv_obj_t *li = lv_label_create(btn_imp);
        lv_label_set_text(li, "Restore Progress from SD");
        lv_obj_set_style_text_font(li, &ui_font_pipboy_16, 0);
        lv_obj_set_style_text_color(li, pip_primary(), 0);
        lv_obj_add_event_cb(btn_imp, coll_import_begin, LV_EVENT_CLICKED, NULL);
    }

    // --- Charge with lights off (was Dark Charge) (screen + all LEDs off while charging; power btn wakes) ---
    lv_obj_t *btn_dc = lv_button_create(cont);
    lv_obj_remove_style_all(btn_dc);
    lv_obj_add_style(btn_dc, &style_list_btn, 0);
    lv_obj_add_style(btn_dc, &style_list_btn_pressed, LV_STATE_PRESSED);
    lv_obj_set_width(btn_dc, lv_pct(100));
    lv_obj_set_height(btn_dc, LV_SIZE_CONTENT);
    lv_obj_t *lbl_dc = lv_label_create(btn_dc);
    lv_label_set_text(lbl_dc, "Charge with lights off");
    lv_obj_set_style_text_font(lbl_dc, &ui_font_pipboy_16, 0);
    lv_obj_set_style_text_color(lbl_dc, pip_primary(), 0);
    lv_obj_add_event_cb(btn_dc, [](lv_event_t *e) {
        (void)e;
        lv_obj_t *modal = lv_obj_create(lv_screen_active());
        lv_obj_remove_style_all(modal);
        lv_obj_set_size(modal, SCREEN_W, SCREEN_H);
        lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(modal, LV_OPA_80, 0);
        lv_obj_remove_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(modal, 0, 0);

        lv_obj_t *box = lv_obj_create(modal);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, 268, 150);
        lv_obj_center(box);
        lv_obj_set_style_bg_color(box, pip_bg(), 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(box, pip_highlight(), 0);
        lv_obj_set_style_border_width(box, 2, 0);
        lv_obj_set_style_pad_all(box, 12, 0);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(box, 8, 0);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

        make_label(box, "CHARGE WITH LIGHTS OFF", &ui_font_pipboy_18, pip_highlight());
        lv_obj_t *w = make_label(box,
            "Screen and all LEDs go dark\n(the charge light stays on).\n\n"
            "Press and HOLD the screen to wake.",
            &ui_font_pipboy_14, pip_primary());
        lv_obj_set_style_text_align(w, LV_TEXT_ALIGN_CENTER, 0);

        lv_obj_t *row = lv_obj_create(box);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *bn = lv_button_create(row);
        lv_obj_set_size(bn, 100, 30);
        lv_obj_set_style_bg_color(bn, pip_border(), 0);
        lv_obj_t *ln = lv_label_create(bn);
        lv_label_set_text(ln, "Cancel");
        lv_obj_set_style_text_font(ln, &ui_font_pipboy_14, 0);
        lv_obj_set_style_text_color(ln, pip_primary(), 0);
        lv_obj_center(ln);
        lv_obj_add_event_cb(bn, [](lv_event_t *e2) {
            lv_obj_t *b = (lv_obj_t *)lv_event_get_target(e2);
            lv_obj_delete(lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(b))));
        }, LV_EVENT_CLICKED, NULL);

        lv_obj_t *by = lv_button_create(row);
        lv_obj_set_size(by, 100, 30);
        lv_obj_set_style_bg_color(by, pip_highlight(), 0);
        lv_obj_t *ly = lv_label_create(by);
        lv_label_set_text(ly, "Go Dark");
        lv_obj_set_style_text_font(ly, &ui_font_pipboy_14, 0);
        lv_obj_set_style_text_color(ly, pip_bg(), 0);
        lv_obj_center(ly);
        lv_obj_add_event_cb(by, [](lv_event_t *e2) {
            lv_obj_t *b = (lv_obj_t *)lv_event_get_target(e2);
            lv_obj_delete(lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(b))));
            CB_LOGLN("[UI] Dark Charge");
            // Dark Charge IS the screensaver dark state -- touch stays alive, so
            // the tap-and-hold unlock (rising tone -> chime) wakes it. Set the
            // LED-suspend flag DIRECTLY (the core-0 task does the off). We do NOT
            // use any tft.sleep()-based path here -- that sleeps the touch
            // controller too, so the screen could never be touch-woken.
            neo_flashlight_set(false);
            flashlight_ui_sync();   // F5: keep the status-bar "FL" honest about it
            neo_suspend_for_ss = true;
            audio_theremin_stop();
            // FB11: this used to call audio_geiger_stop() RAW, which silences the engine but
            // leaves rad_geiger_audio_on TRUE -- so rad_geiger_audio_update() then skipped
            // audio_geiger_start() and the NEXT Radiation Start ran with the gauge moving and
            // no ticks at all. This is the instance that proved the class regenerates when
            // each site hand-rolls its own teardown, so go through the one helper.
            rad_geiger_force_stop();
            audio_mp3_stop();
            audio_mp3_stream_stop();   // reboot-UAF review: cover the radio stream
            audio_static_stop();       // ...and tuning static
            audio_tone_stop();
            dark_charge_active = true;
            screensaver_activate();      // "Hold to unlock" overlay + tone path
            lcd_set_brightness(0);        // fully dark (override the ss_style dim)
        }, LV_EVENT_CLICKED, NULL);
    }, LV_EVENT_CLICKED, NULL);

    // (The old "Export / Import Collectibles (SD)" section was a post-merge DUPLICATE
    //  of "Backup / Restore Progress (SD)" above -- both wired to coll_do_export /
    //  coll_import_begin (the v2 badge-bound save = collectibles + ARG + prefs). There
    //  is no collectibles-only path anymore, so the duplicate was removed. The
    //  Backup/Restore section above absorbed its SD-not-present gate.)

    // --- Reset Collectibles ---
    lv_obj_t *btn_rc = lv_button_create(cont);
    lv_obj_remove_style_all(btn_rc);
    lv_obj_add_style(btn_rc, &style_list_btn, 0);
    lv_obj_add_style(btn_rc, &style_list_btn_pressed, LV_STATE_PRESSED);
    lv_obj_set_width(btn_rc, lv_pct(100));
    lv_obj_set_height(btn_rc, LV_SIZE_CONTENT);
    lv_obj_t *lbl_rc = lv_label_create(btn_rc);
    lv_label_set_text(lbl_rc, "Reset Collectibles");
    lv_obj_set_style_text_font(lbl_rc, &ui_font_pipboy_16, 0);
    lv_obj_set_style_text_color(lbl_rc, pip_dim(), 0);
    lv_obj_add_event_cb(btn_rc, [](lv_event_t *e) {
        (void)e;
        lv_obj_t *modal = lv_obj_create(lv_screen_active());
        lv_obj_remove_style_all(modal);
        lv_obj_set_size(modal, SCREEN_W, SCREEN_H);
        lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(modal, LV_OPA_80, 0);
        lv_obj_remove_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(modal, 0, 0);

        lv_obj_t *box = lv_obj_create(modal);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, 260, 140);
        lv_obj_center(box);
        lv_obj_set_style_bg_color(box, pip_bg(), 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(box, pip_highlight(), 0);
        lv_obj_set_style_border_width(box, 2, 0);
        lv_obj_set_style_pad_all(box, 12, 0);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(box, 8, 0);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

        make_label(box, "RESET COLLECTIBLES?",
                   &ui_font_pipboy_18, pip_highlight());

        lv_obj_t *warn = make_label(box,
            "All collectibles will be\nmarked as uncollected.\nThis cannot be undone.",
            &ui_font_pipboy_14, pip_primary());
        lv_obj_set_style_text_align(warn, LV_TEXT_ALIGN_CENTER, 0);

        lv_obj_t *row = lv_obj_create(box);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *btn_no = lv_button_create(row);
        lv_obj_set_size(btn_no, 100, 30);
        lv_obj_set_style_bg_color(btn_no, pip_border(), 0);
        lv_obj_t *lno = lv_label_create(btn_no);
        lv_label_set_text(lno, "Keep Them");
        lv_obj_set_style_text_font(lno, &ui_font_pipboy_14, 0);
        lv_obj_set_style_text_color(lno, pip_primary(), 0);
        lv_obj_center(lno);
        lv_obj_add_event_cb(btn_no, [](lv_event_t *e2) {
            lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e2);
            lv_obj_t *m = lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(btn)));
            lv_obj_delete(m);
        }, LV_EVENT_CLICKED, NULL);

        lv_obj_t *btn_yes = lv_button_create(row);
        lv_obj_set_size(btn_yes, 100, 30);
        lv_obj_set_style_bg_color(btn_yes, pip_highlight(), 0);
        lv_obj_t *lyes = lv_label_create(btn_yes);
        lv_label_set_text(lyes, "Wipe 'Em");
        lv_obj_set_style_text_font(lyes, &ui_font_pipboy_14, 0);
        lv_obj_set_style_text_color(lyes, pip_bg(), 0);
        lv_obj_center(lyes);
        lv_obj_add_event_cb(btn_yes, [](lv_event_t *e2) {
            lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e2);
            lv_obj_t *m = lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(btn)));
            lv_obj_delete(m);
            coll_reset_found();
            CB_LOGLN("[UI] Collectibles reset");
            rebuild_content();
        }, LV_EVENT_CLICKED, NULL);

    }, LV_EVENT_CLICKED, NULL);

    // --- Nuclear Reset (factory reset + reboot) ---
    lv_obj_t *btn_nuke = lv_button_create(cont);
    lv_obj_remove_style_all(btn_nuke);
    lv_obj_add_style(btn_nuke, &style_list_btn, 0);
    lv_obj_add_style(btn_nuke, &style_list_btn_pressed, LV_STATE_PRESSED);
    lv_obj_set_width(btn_nuke, lv_pct(100));
    lv_obj_set_height(btn_nuke, LV_SIZE_CONTENT);
    lv_obj_t *lbl_nuke = lv_label_create(btn_nuke);
    lv_label_set_text(lbl_nuke, "Reset All Settings");
    lv_obj_set_style_text_font(lbl_nuke, &ui_font_pipboy_16, 0);
    lv_obj_set_style_text_color(lbl_nuke, pip_dim(), 0);
    lv_obj_add_event_cb(btn_nuke, [](lv_event_t *e) {
        (void)e;
        lv_obj_t *modal = lv_obj_create(lv_screen_active());
        lv_obj_remove_style_all(modal);
        lv_obj_set_size(modal, SCREEN_W, SCREEN_H);
        lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(modal, LV_OPA_80, 0);
        lv_obj_remove_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(modal, 0, 0);

        lv_obj_t *box = lv_obj_create(modal);
        lv_obj_remove_style_all(box);
        lv_obj_set_width(box, 272);
        lv_obj_set_height(box, LV_SIZE_CONTENT);      // auto-fit the warning + buttons
        lv_obj_set_style_max_height(box, SCREEN_H - 8, 0);
        lv_obj_center(box);
        lv_obj_set_style_bg_color(box, pip_bg(), 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(box, pip_highlight(), 0);
        lv_obj_set_style_border_width(box, 2, 0);
        lv_obj_set_style_pad_all(box, 12, 0);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(box, 8, 0);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

        make_label(box, "CONFIRM NUCLEAR OPTION",
                   &ui_font_pipboy_18, pip_highlight());

        lv_obj_t *warn = make_label(box,
            "Some things never change.\nBut your settings will.\n\nAll config and collectibles\nwill be erased. Badge reboots.",
            &ui_font_pipboy_14, pip_primary());
        lv_label_set_long_mode(warn, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(warn, lv_pct(100));
        lv_obj_set_style_text_align(warn, LV_TEXT_ALIGN_CENTER, 0);

        lv_obj_t *row = lv_obj_create(box);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *btn_no = lv_button_create(row);
        lv_obj_set_size(btn_no, 100, 30);
        lv_obj_set_style_bg_color(btn_no, pip_border(), 0);
        lv_obj_t *lno = lv_label_create(btn_no);
        lv_label_set_text(lno, "Stand Down");
        lv_obj_set_style_text_font(lno, &ui_font_pipboy_14, 0);
        lv_obj_set_style_text_color(lno, pip_primary(), 0);
        lv_obj_center(lno);
        lv_obj_add_event_cb(btn_no, [](lv_event_t *e2) {
            lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e2);
            lv_obj_t *m = lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(btn)));
            lv_obj_delete(m);
        }, LV_EVENT_CLICKED, NULL);

        lv_obj_t *btn_yes = lv_button_create(row);
        lv_obj_set_size(btn_yes, 100, 30);
        lv_obj_set_style_bg_color(btn_yes, pip_highlight(), 0);
        lv_obj_t *lyes = lv_label_create(btn_yes);
        lv_label_set_text(lyes, "Launch It");
        lv_obj_set_style_text_font(lyes, &ui_font_pipboy_14, 0);
        lv_obj_set_style_text_color(lyes, pip_bg(), 0);
        lv_obj_center(lyes);
        lv_obj_add_event_cb(btn_yes, [](lv_event_t *e2) {
            (void)e2;
            CB_LOGLN("[UI] NUCLEAR OPTION CONFIRMED");
            cfg_factory_reset();
            coll_reset_found();
            arg_factory_reset();      // wipe ARG/Quanta state too (separate NVS namespaces)
            cfg_save_all();
            delay(200);
            ESP.restart();
        }, LV_EVENT_CLICKED, NULL);

    }, LV_EVENT_CLICKED, NULL);
}

// ─────────────────────── DATA > Theremin ──────────────────────────────────

// Theremin poll timer: feed VL53L5CX data to ClipBoyTheremin library, update UI
static void theremin_poll_timer_cb(lv_timer_t *t) {
    (void)t;

#ifdef TEST_HARNESS
    // When sensor mock is active, use injected data instead of real sensor
    extern bool test_sensor_is_mocked();
    extern VL53L5CX_ResultsData &test_sensor_get_data();
    if (test_sensor_is_mocked()) {
        audio_theremin_feed(test_sensor_get_data());
        goto theremin_update_ui;
    }
#endif

    if (!vl53_initialized) return;

    static VL53L5CX_ResultsData results;  // ~600 bytes - keep off stack
    if (!vl53_sensor.isDataReady()) return;
    vl53_sensor.getRangingData(&results);

    // Feed sensor data to theremin library
    audio_theremin_feed(results);

#ifdef TEST_HARNESS
    theremin_update_ui:
#endif
    // Per-band update: bar fill (top-anchored, tall = close hand),
    // mm + Hz readouts. Voice slots map 1:1 to bands now.
    uint8_t mask = aud_theremin.activeVoiceMask();
    for (int b = 0; b < 4; b++) {
        if (!theremin_voice_bars[b]) continue;

        bool none = (cfg.theremin_voices[b] == 0);
        bool active = (mask & (1 << b)) != 0;

        if (none) {
            // Voice disabled -- bar empty, OFF in mm slot, blank Hz slot
            lv_obj_set_height(theremin_voice_bars[b], 0);
            if (theremin_dist_labels[b]) lv_label_set_text(theremin_dist_labels[b], "OFF");
            if (theremin_freq_labels[b]) lv_label_set_text(theremin_freq_labels[b], "");
            continue;
        }

        if (active) {
            int   dist = aud_theremin.voiceDistance(b);
            float freq = aud_theremin.voiceFreq(b);

            // Map dist 50..400 mm → fill 100..0 px (tall = close = high pitch)
            int fill_h = 0;
            if (dist > 0) {
                int clamped = dist;
                if (clamped < 50)  clamped = 50;
                if (clamped > AUD_BAND_MAX_DIST_MM) clamped = AUD_BAND_MAX_DIST_MM;
                fill_h = THEREMIN_BAR_H -
                         ((clamped - 50) * THEREMIN_BAR_H) / (AUD_BAND_MAX_DIST_MM - 50);
            }
            lv_obj_set_height(theremin_voice_bars[b], fill_h);

            if (theremin_dist_labels[b])
                lv_label_set_text_fmt(theremin_dist_labels[b], "%d", dist);
            if (theremin_freq_labels[b])
                lv_label_set_text_fmt(theremin_freq_labels[b], "%d", (int)(freq + 0.5f));
        } else {
            lv_obj_set_height(theremin_voice_bars[b], 0);
            if (theremin_dist_labels[b]) lv_label_set_text(theremin_dist_labels[b], "---");
            if (theremin_freq_labels[b]) lv_label_set_text(theremin_freq_labels[b], "----");
        }
    }
}

// Lock (gray out + ignore taps) the voice dropdowns while the theremin is
// active. Changing a voice mid-render silences the synth, so we disable changes
// while it runs -- the user disables, picks a voice, then re-enables. Safe to
// call when the screen isn't built (dropdowns are NULL).
static void theremin_lock_voices(bool locked) {
    for (int i = 0; i < CFG_NUM_THEREMIN_VOICES; i++) {
        if (!theremin_voice_dd[i]) continue;
        if (locked) lv_obj_add_state(theremin_voice_dd[i], LV_STATE_DISABLED);
        else        lv_obj_remove_state(theremin_voice_dd[i], LV_STATE_DISABLED);
    }
}

// Bring the theremin up: claim the VL53L5CX, start the audio synth + poll
// timer. Returns true if it came up. Does the hardware/audio/status-bar work
// but NOT the on-screen toggle button (the harness has no button; the UI
// callback owns the label). Both the UI and the test harness call this so they
// share the exact same start path.
static bool theremin_enable(void) {
    // Mutual exclusion: claim the badge for the theremin. Stop any running
    // tool/geiger (radio) and — critically — any in-flight HR scan, which
    // holds the SAME VL53L5CX on Wire1. Initializing the sensor underneath a
    // live scan is the classic wedge, so the scan must be torn down FIRST.
    cb_stop_operation();             // self-guards: stops tool + geiger
    if (hr_scanning) hr_scan_stop(); // release VL53L5CX from the scanner

    if (!vl53_begun) {
        // First enable (or the first after an HR scan took the sensor): upload
        // the ~84KB firmware via begin(). This is the slow, sensor-churning
        // step — done ONCE, not on every enable. Re-begin-per-enable is what
        // exhausted the VL53L5CX under repeated theremin toggling.
        vl53_wire.begin(CB_VL53_SDA, CB_VL53_SCL, CB_VL53_I2C_HZ);
        delay(50);
        bool found = false;
        for (int attempt = 0; attempt < 3 && !found; attempt++) {
            if (attempt > 0) delay(200);
            found = vl53_sensor.begin(DEFAULT_I2C_ADDR >> 1, vl53_wire);
            CB_LOGF("[THEREMIN] VL53L5CX init attempt %d: %s\n",
                          attempt + 1, found ? "OK" : "FAIL");
        }
        if (!found) {
            CB_LOGLN("[WARN] VL53L5CX init failed after 3 attempts");
            if (lbl_stask) lv_label_set_text(lbl_stask, "Sensor failed!");
            // Surface the failure on band 0's mm slot -- most likely visible
            if (theremin_dist_labels[0])
                lv_label_set_text(theremin_dist_labels[0], "ERR");
            vl53_wire.end();
            return false;
        }
        vl53_begun = true;
    } else if (!vl53_initialized) {
        // Firmware already loaded — just wake the sensor from SLEEP. No 84KB
        // re-upload, so repeated theremin on/off doesn't churn/exhaust it.
        vl53_sensor.setPowerMode(SF_VL53L5CX_POWER_MODE::WAKEUP);
    }
    if (!vl53_initialized) {
        vl53_sensor.setResolution(VL53L5CX_RESOLUTION_8X8);
        vl53_sensor.setRangingFrequency(15);
        vl53_sensor.startRanging();
        vl53_initialized = true;
    }

    // Init theremin library if not already begun.
    //   silenceDistMm = 10000: bypass the library's distance→volume
    //     curve. We don't want hand height to affect loudness -- the
    //     wrapper already gates anything past AUD_BAND_MAX_DIST_MM, so
    //     volume is purely the slider's job.
    //   masterVolume seeded from slider; live updates go through
    //     audio_theremin_set_volume() from the slider callback.
    if (!theremin_begun) {
        ClipTheremin::Config tcfg;
        tcfg.sampleRate    = 44100;
        tcfg.masterVolume  = (float)theremin_volume / 100.0f;
        tcfg.silenceDistMm = 10000;
        aud_theremin.begin(tcfg);
        theremin_begun = true;

        // Restore saved voice selections (4 bands now)
        for (int i = 0; i < CFG_NUM_THEREMIN_VOICES; i++) {
            if (cfg.theremin_voices[i] == 0) continue;  // None
            ClipTheremin::VoiceConfig vcfg;
            vcfg.type      = (ClipTheremin::VoiceType)cfg.theremin_voices[i];
            vcfg.minFreqHz = 100.0f;
            vcfg.maxFreqHz = 2000.0f;
            vcfg.maxVolume = 1.0f;
            aud_theremin.loadVoice(i, vcfg);
        }
    }

    // Push saved tuning to the band detector (cfg was loaded at boot)
    audio_theremin_set_k(cfg.theremin_k);
    audio_theremin_set_agreement(cfg.theremin_agreement_mm);

    audio_theremin_reset_bands();
    audio_theremin_start();

    if (theremin_poll_timer) lv_timer_delete(theremin_poll_timer);
    theremin_poll_timer = lv_timer_create(theremin_poll_timer_cb, 50, NULL);

    if (lbl_stask) lv_label_set_text(lbl_stask, "Theremin active");
    theremin_lock_voices(true);   // can't change voices mid-render (silences synth)
    CB_LOGLN("[THEREMIN] Enabled");
    return true;
}

// Tear the theremin down: stop audio, kill the poll timer, power down the
// VL53L5CX. Safe to call when not active. Shared by UI + harness.
static void theremin_disable(void) {
    audio_theremin_stop();
    if (theremin_poll_timer) {
        lv_timer_delete(theremin_poll_timer);
        theremin_poll_timer = NULL;
    }
    if (vl53_initialized) {
        vl53_sensor.stopRanging();
        vl53_sensor.setPowerMode(SF_VL53L5CX_POWER_MODE::SLEEP);
        vl53_initialized = false;
        CB_LOGLN("[THEREMIN] VL53L5CX powered down");
    }
    if (lbl_stask) lv_label_set_text(lbl_stask, "");
    theremin_lock_voices(false);   // re-enable voice changes now that it's stopped
    // M3 -- zero the readouts. The 50 ms poll timer is the ONLY writer of these widgets, so
    // deleting it above left the four bars filled and the mm/Hz labels frozen at whatever the
    // user's hand was doing at the instant they tapped Disable -- with the synth silent and the
    // sensor asleep. Owner-confirmed. Same "frozen instrument readout" the chart fix in
    // cb_output_cleanup() was written to kill; it simply was not mirrored here. Reuses the
    // existing voice-disabled convention (empty bar / "OFF" / blank Hz) rather than inventing
    // a third visual state.
    for (int b = 0; b < CFG_NUM_THEREMIN_VOICES; b++) {
        if (theremin_voice_bars[b])  lv_obj_set_height(theremin_voice_bars[b], 0);
        if (theremin_dist_labels[b]) lv_label_set_text(theremin_dist_labels[b], "OFF");
        if (theremin_freq_labels[b]) lv_label_set_text(theremin_freq_labels[b], "");
    }
    CB_LOGLN("[THEREMIN] Disabled");
}

static void theremin_toggle_cb(lv_event_t *e) {
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    const char *cur = lv_label_get_text(lbl);

    if (cur[0] == 'E') {
        if (theremin_enable())
            lv_label_set_text(lbl, "Disable");
        // on failure theremin_enable already surfaced the error; button stays
    } else {
        theremin_disable();
        lv_label_set_text(lbl, "Enable");
    }
}

// Tuning slider callbacks: each updates cfg + audio module live, saves
// to NVS on every change (slider gets moved rarely -- wear is negligible).
static void theremin_k_slider_cb(lv_event_t *e) {
    lv_obj_t *sl = (lv_obj_t *)lv_event_get_target(e);
    int32_t val = lv_slider_get_value(sl);
    cfg.theremin_k = (uint8_t)val;
    audio_theremin_set_k((uint8_t)val);
    cfg_save_theremin_k();
    if (theremin_k_label)
        lv_label_set_text_fmt(theremin_k_label, "%ld", (long)val);
}

static void theremin_agree_slider_cb(lv_event_t *e) {
    lv_obj_t *sl = (lv_obj_t *)lv_event_get_target(e);
    int32_t val = lv_slider_get_value(sl);
    cfg.theremin_agreement_mm = (uint8_t)val;
    audio_theremin_set_agreement((uint8_t)val);
    cfg_save_theremin_agreement();
    if (theremin_agree_label)
        lv_label_set_text_fmt(theremin_agree_label, "%ldmm", (long)val);
}

// Voice type dropdown callback
static void theremin_voice_dd_cb(lv_event_t *e) {
    uint8_t slot = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t *dd = (lv_obj_t *)lv_event_get_target(e);
    uint16_t sel = lv_dropdown_get_selected(dd);

    cfg.theremin_voices[slot] = (uint8_t)sel;
    cfg_save_theremin_voices();

    aud_theremin.clearVoice(slot);
    if (sel == 0) return;  // None

    ClipTheremin::VoiceConfig vcfg;
    vcfg.type = (ClipTheremin::VoiceType)sel;  // 1=Sine, 2=Square, 3=Sawtooth, 4=Triangle
    vcfg.minFreqHz = 100.0f;
    vcfg.maxFreqHz = 2000.0f;
    vcfg.maxVolume = 1.0f;

    ClipTheremin::LoadError err = aud_theremin.loadVoice(slot, vcfg);
    if (err != ClipTheremin::LoadError::Ok) {
        CB_LOGF("[THEREMIN] Voice %d load error: %d\n", slot, (int)err);
    }
}

// ── Helper: build one band column on the right pane ────────────────────────
// Each column contains: vertical bar (top-anchored fill), mm readout,
// Hz readout. Bars sit side-by-side so left→right on screen matches
// left→right across the LiDAR FoV -- no need to label the bars themselves.
static void build_theremin_band_column(lv_obj_t *parent, int band) {
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_add_style(col, &style_container, 0);
    lv_obj_set_size(col, THEREMIN_BAR_W + 6, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(col, 2, 0);
    lv_obj_set_style_pad_all(col, 0, 0);

    // Bar "well" (background) -- fixed height, top-anchored fill grows down
    lv_obj_t *well = lv_obj_create(col);
    lv_obj_remove_style_all(well);
    lv_obj_set_size(well, THEREMIN_BAR_W, THEREMIN_BAR_H);
    lv_obj_set_style_bg_color(well, pip_border(), 0);
    lv_obj_set_style_bg_opa(well, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(well, pip_primary(), 0);
    lv_obj_set_style_border_width(well, 1, 0);
    lv_obj_set_style_pad_all(well, 0, 0);
    lv_obj_clear_flag(well, LV_OBJ_FLAG_SCROLLABLE);
    theremin_voice_bar_wells[band] = well;

    // Fill -- anchored to top of well, height set per-frame from the
    // poll timer based on hand distance (close = tall fill).
    lv_obj_t *fill = lv_obj_create(well);
    lv_obj_remove_style_all(fill);
    lv_obj_set_style_bg_color(fill, pip_primary(), 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    lv_obj_set_size(fill, THEREMIN_BAR_W, 0);
    lv_obj_align(fill, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
    theremin_voice_bars[band] = fill;

    // mm readout -- pipboy_14 mono, 4-char field "OFF"/"---"/"1234"
    theremin_dist_labels[band] = make_label(col, "---",
                                            &ui_font_pipboy_14, pip_dim());

    // Hz readout -- same width
    theremin_freq_labels[band] = make_label(col, "----",
                                            &ui_font_pipboy_14, pip_dim());
}

static void build_data_theremin(lv_obj_t *cont) {
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_set_style_pad_gap(cont, 0, 0);

    // ─── Left pane: 4 voice dropdowns + Enable/Disable button ───
    lv_obj_t *lpane = lv_obj_create(cont);
    lv_obj_remove_style_all(lpane);
    lv_obj_add_style(lpane, &style_list_bg, 0);
    lv_obj_set_size(lpane, LEFT_PANE_W, lv_pct(100));
    lv_obj_set_flex_flow(lpane, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(lpane, 4, 0);
    lv_obj_set_style_pad_gap(lpane, 4, 0);
    lv_obj_add_flag(lpane, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(lpane, &style_scrollbar, LV_PART_SCROLLBAR);

    // No "VOICES" header: the per-row L/CL/CR/R labels already say what these are, and the
    // header's height pushed the Enable/Disable button below the fold so the pane had to be
    // scrolled to reach it. That button is the one control on this screen the user MUST be
    // able to hit, so it wins the vertical budget.
    for (int i = 0; i < CFG_NUM_THEREMIN_VOICES; i++) {
        lv_obj_t *row = lv_obj_create(lpane);
        lv_obj_remove_style_all(row);
        lv_obj_add_style(row, &style_container, 0);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(row, 4, 0);
        lv_obj_set_style_pad_ver(row, 0, 0);

        // Spatial label (L/CL/CR/R) -- matches the bar columns on the right
        lv_obj_t *bl = make_label(row, theremin_band_label[i],
                                  &ui_font_pipboy_14, pip_primary());
        lv_obj_set_width(bl, 18);  // fits "CL"/"CR" with breathing room

        theremin_voice_dd[i] = make_dropdown(row, "None\nSine\nSquare\nSaw\nTriangle");
        lv_obj_set_flex_grow(theremin_voice_dd[i], 1);
        // Pip-boy terminal font (on-theme) + tight symmetric padding so the voice
        // name sits vertically centered in a short box (content-sized, not a fixed
        // height that top-biased the text).
        lv_obj_set_style_text_font(theremin_voice_dd[i], &ui_font_pipboy_14, LV_PART_MAIN);
        // Keep the down-chevron on the symbol font -- pipboy has no LV_SYMBOL glyph
        // (it'd render as tofu), so pin the INDICATOR part to montserrat.
        lv_obj_set_style_text_font(theremin_voice_dd[i], &lv_font_montserrat_14, LV_PART_INDICATOR);
        lv_obj_set_style_pad_top(theremin_voice_dd[i], 3, LV_PART_MAIN);
        lv_obj_set_style_pad_bottom(theremin_voice_dd[i], 3, LV_PART_MAIN);
        lv_obj_set_height(theremin_voice_dd[i], LV_SIZE_CONTENT);
        lv_dropdown_set_selected(theremin_voice_dd[i], cfg.theremin_voices[i]);
        // Visibly dim when locked (theremin running -> voice changes disabled)
        lv_obj_set_style_text_opa(theremin_voice_dd[i], LV_OPA_40, LV_STATE_DISABLED);
        lv_obj_set_style_border_opa(theremin_voice_dd[i], LV_OPA_40, LV_STATE_DISABLED);
        lv_obj_add_event_cb(theremin_voice_dd[i], theremin_voice_dd_cb,
                            LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
    }

    // If the theremin is already running (nav back to this screen), lock the voices.
    theremin_lock_voices(theremin_poll_timer != NULL);

    // Enable/Disable button sits right under the voices so it's ALWAYS visible
    // (the old flex-grow spacer pushed it below the fold once the pane filled).
    theremin_enable_btn = make_action_btn(lpane,
                                          theremin_poll_timer ? "Disable" : "Enable",
                                          theremin_toggle_cb, NULL);
    lv_obj_set_width(theremin_enable_btn, lv_pct(100));

    // ── Tuning: k (1-8) + agreement distance (5-50mm) ────────────────
    // Compact horizontal rows: tiny label + flex-grow slider + numeric
    // readout. Live response so Bryce can dial in feel during play.
    {
        lv_obj_t *krow = lv_obj_create(lpane);
        lv_obj_remove_style_all(krow);
        lv_obj_add_style(krow, &style_container, 0);
        lv_obj_set_size(krow, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(krow, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(krow, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(krow, 4, 0);
        lv_obj_set_style_pad_ver(krow, 2, 0);

        lv_obj_t *kl = make_label(krow, "k", &ui_font_pipboy_14, pip_primary());
        lv_obj_set_width(kl, 14);

        theremin_k_slider = lv_slider_create(krow);
        lv_obj_set_flex_grow(theremin_k_slider, 1);
        lv_obj_set_height(theremin_k_slider, 6);
        lv_slider_set_range(theremin_k_slider, 1, 8);
        lv_slider_set_value(theremin_k_slider, cfg.theremin_k, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(theremin_k_slider, pip_border(), LV_PART_MAIN);
        lv_obj_set_style_bg_color(theremin_k_slider, pip_primary(), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(theremin_k_slider, pip_highlight(), LV_PART_KNOB);
        lv_obj_set_style_pad_all(theremin_k_slider, 2, LV_PART_KNOB);

        char kbuf[4];
        snprintf(kbuf, sizeof(kbuf), "%d", cfg.theremin_k);
        theremin_k_label = make_label(krow, kbuf, &ui_font_pipboy_14, pip_dim());
        lv_obj_set_width(theremin_k_label, 14);
        lv_obj_set_style_text_align(theremin_k_label, LV_TEXT_ALIGN_RIGHT, 0);

        lv_obj_add_event_cb(theremin_k_slider, theremin_k_slider_cb,
                            LV_EVENT_VALUE_CHANGED, NULL);
    }

    {
        lv_obj_t *arow = lv_obj_create(lpane);
        lv_obj_remove_style_all(arow);
        lv_obj_add_style(arow, &style_container, 0);
        lv_obj_set_size(arow, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(arow, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(arow, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(arow, 4, 0);
        lv_obj_set_style_pad_ver(arow, 2, 0);

        lv_obj_t *al = make_label(arow, "ag", &ui_font_pipboy_14, pip_primary());
        lv_obj_set_width(al, 16);

        theremin_agree_slider = lv_slider_create(arow);
        lv_obj_set_flex_grow(theremin_agree_slider, 1);
        lv_obj_set_height(theremin_agree_slider, 6);
        lv_slider_set_range(theremin_agree_slider, 5, 50);
        lv_slider_set_value(theremin_agree_slider, cfg.theremin_agreement_mm, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(theremin_agree_slider, pip_border(), LV_PART_MAIN);
        lv_obj_set_style_bg_color(theremin_agree_slider, pip_primary(), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(theremin_agree_slider, pip_highlight(), LV_PART_KNOB);
        lv_obj_set_style_pad_all(theremin_agree_slider, 2, LV_PART_KNOB);

        char abuf[8];
        snprintf(abuf, sizeof(abuf), "%dmm", cfg.theremin_agreement_mm);
        theremin_agree_label = make_label(arow, abuf, &ui_font_pipboy_14, pip_dim());
        lv_obj_set_width(theremin_agree_label, 32);
        lv_obj_set_style_text_align(theremin_agree_label, LV_TEXT_ALIGN_RIGHT, 0);

        lv_obj_add_event_cb(theremin_agree_slider, theremin_agree_slider_cb,
                            LV_EVENT_VALUE_CHANGED, NULL);
    }

    // ─── Right pane: 4 vertical bars + readouts + volume slider ───
    lv_obj_t *rpane = create_right_detail(cont);
    lv_obj_set_style_pad_top(rpane, 4, 0);
    lv_obj_set_style_pad_gap(rpane, 4, 0);

    // Bar row -- 4 columns side by side, no labels (spatial mapping)
    lv_obj_t *brow = lv_obj_create(rpane);
    lv_obj_remove_style_all(brow);
    lv_obj_add_style(brow, &style_container, 0);
    lv_obj_set_size(brow, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(brow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(brow, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(brow, 0, 0);
    lv_obj_set_style_pad_gap(brow, 4, 0);
    lv_obj_clear_flag(brow, LV_OBJ_FLAG_SCROLLABLE);

    for (int b = 0; b < 4; b++) {
        build_theremin_band_column(brow, b);
    }

    // Volume slider with inline % readout (saves a row vs. a separate label)
    lv_obj_t *vol_row = make_settings_row(rpane);
    make_label(vol_row, "Vol", &ui_font_pipboy_14, pip_primary());
    theremin_vol_slider = lv_slider_create(vol_row);
    lv_obj_set_flex_grow(theremin_vol_slider, 1);
    lv_obj_set_height(theremin_vol_slider, 8);
    lv_slider_set_range(theremin_vol_slider, 0, 100);
    lv_slider_set_value(theremin_vol_slider, theremin_volume, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(theremin_vol_slider, pip_border(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(theremin_vol_slider, pip_primary(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(theremin_vol_slider, pip_highlight(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(theremin_vol_slider, 2, LV_PART_KNOB);
    char vbuf[8];
    snprintf(vbuf, sizeof(vbuf), "%d%%", theremin_volume);
    theremin_vol_label = make_label(vol_row, vbuf, &ui_font_pipboy_14, pip_dim());
    lv_obj_set_width(theremin_vol_label, 32);  // inline "100%" fits
    lv_obj_set_style_text_align(theremin_vol_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_add_event_cb(theremin_vol_slider, volume_slider_cb,
                        LV_EVENT_VALUE_CHANGED, theremin_vol_label);
}

// ─────────────────────── CONTENT DISPATCHER ───────────────────────────────

// ── content_teardown: the shared dismantle path ──────────────────────────────
// Extracted from rebuild_content() 2026-07-25 (audit SB2). The project's UAF rule said
// "BOTH teardown paths" (rebuild_content + ui_theme_switch_live) -- but FOUR functions
// destroy the content PANE, and three of them did no teardown at all:
//     rebuild_content()   10890  <- had it
//     info_page_create()   8545  <- Credits / Legal / About
//     show_help()          9183  <- the ALWAYS-VISIBLE status-bar `?`
//     show_radio()        12743
// So tapping `?` while the Geiger (1 Hz) or theremin (50 ms) poller ran freed the pane and
// left those timers writing widget globals that have no self-null -- into memory the Help
// page had already reused. PROVEN on hardware: panic reboot, reset_reason=4 (ESP_RST_PANIC),
// within 3 s, on the default SKU, two taps from boot.
// It also left the VL53L5CX powered with I2C bus 1 held (the documented precondition for the
// HR-scanner wedge), the theremin audibly playing under the Help page, radio_active latched
// true (which suppresses the screensaver forever), and the coll_rot snap-back animation
// pointing at a freed card.
// Callers clear_children() themselves; this only dismantles state. cur_sel is deliberately
// NOT touched -- rebuild_content saves/restores it around this call for split-pane screens.
//
// ⚠ There is a FIFTH destroyer, and it does NOT call this helper: ui_theme_switch_live()
// (~7827) frees the whole scr_main tree. It keeps its own hand-rolled teardown which is not a
// superset of this one -- it omits left_pane/right_pane, rad_scale/rad_val_lbl/rad_toggle_btn,
// led_sl_*, the wifi_* label nulls, cb_output_detach_ui() and the LED/volume NVS flush. Those
// gaps are currently MASKED because create_main_screen() calls rebuild_content() (which runs
// this helper) from inside the switch, and the debounce timers are one-shots that self-fire.
// The masking is accidental, so treat the two lists as drift-prone: anything added here should
// be checked against ui_theme_switch_live(). Making it call this helper is queued rather than
// done, because its build-new-then-delete-old ordering interacts with LV_EVENT_DELETE handlers
// (see cb_selfnull_on_delete) and deserves its own test, not a 2am edit.
static void content_teardown() {
    left_pane = NULL;
    right_pane = NULL;

    // Clear cross-update slider references when leaving their screens
    rad_scale = NULL;
    rad_needle = NULL;
    rad_max_needle = NULL;
    rad_val_lbl = NULL;
    rad_max_lbl = NULL;
    rad_topch_lbl = NULL;
    rad_topbss_lbl = NULL;
    rad_timer_lbl = NULL;
    rad_toggle_btn = NULL;
    theremin_vol_slider = NULL;
    settings_vol_slider = NULL;
    theremin_vol_label  = NULL;
    settings_vol_label  = NULL;
    led_sl_bright = NULL;
    led_sl_r = NULL;
    led_sl_g = NULL;
    led_sl_b = NULL;

    // Clear ClipBoy list area references
    cb_ap_list_area = NULL;
    cb_sta_list_area = NULL;

    // Clear WiFi join UI references
    wifi_ssid_label = NULL;
    wifi_pw_label = NULL;
    wifi_status_label = NULL;
    wifi_scan_btn_label = NULL;
    if (wifi_connect_timer) { lv_timer_delete(wifi_connect_timer); wifi_connect_timer = NULL; }

    // reboot-UAF review: cancel the collectible snap-back anim + null its card refs
    // (anim var=NULL, so LVGL won't auto-cancel it when clear_children frees the card).
    coll_rot_reset();

    // Stop theremin when leaving its tab (sensor + audio + timer)
    if (theremin_poll_timer) {
        lv_timer_delete(theremin_poll_timer);
        theremin_poll_timer = NULL;
        if (lbl_stask) lv_label_set_text(lbl_stask, "");
    }
    if (vl53_initialized) {
        vl53_sensor.stopRanging();
        vl53_sensor.setPowerMode(SF_VL53L5CX_POWER_MODE::SLEEP);
        audio_theremin_stop();
        vl53_initialized = false;
        CB_LOGLN("[VL53] Powered down on nav change");
    }
    memset(theremin_dist_labels,    0, sizeof(theremin_dist_labels));
    memset(theremin_freq_labels,    0, sizeof(theremin_freq_labels));
    memset(theremin_voice_bars,     0, sizeof(theremin_voice_bars));
    memset(theremin_voice_bar_wells,0, sizeof(theremin_voice_bar_wells));
    memset(theremin_voice_dd,       0, sizeof(theremin_voice_dd));
    theremin_enable_btn   = NULL;
    theremin_k_slider     = NULL;
    theremin_k_label      = NULL;
    theremin_agree_slider = NULL;
    theremin_agree_label  = NULL;

    // Flush any pending LED save before leaving LED screen
    if (led_save_timer) {
        cfg_save_leds();
        lv_timer_delete(led_save_timer);
        led_save_timer = NULL;
    }

    // Flush any pending volume save before leaving
    if (vol_save_timer) {
        cfg_save_volume();
        lv_timer_delete(vol_save_timer);
        vol_save_timer = NULL;
    }

    // DON'T kill scan/output timers - they keep running in the background
    // writing to the persistent log buffer. Only detach UI refs so they
    // don't write to destroyed LVGL objects.
    cb_output_detach_ui();
    cb_ap_list_area = NULL;
    cb_sta_list_area = NULL;

    // Stop HR scanner when navigating away
    hr_scan_stop();
    mon_popup_close();   // UAF audit: also close the monitor popup + its timer on nav (defensive)
    radio_stop();        // stop SegFault-Tec FM (audio/scope timers) before its widgets vanish
}

static void rebuild_content() {
    if (!content_obj) return;
    clear_children(content_obj);
    int16_t saved_sel = cur_sel;  // preserve for split-pane rebuilds (-1 = none)
    cur_sel = -1;
    content_teardown();

    lv_obj_remove_style_all(content_obj);
    lv_obj_add_style(content_obj, &style_container, 0);
    lv_obj_set_size(content_obj, SCREEN_W, CONTENT_H);
    lv_obj_set_pos(content_obj, 0, CONTENT_Y);
    lv_obj_remove_flag(content_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(content_obj, pip_bg(), 0);
    lv_obj_set_style_bg_opa(content_obj, LV_OPA_COVER, 0);

    // Restore saved selection for split-pane screens
    cur_sel = saved_sel;

    if (cur_div == 0) {
        switch (cur_tab) {
            case 0: build_stats_status(content_obj); break;
            case 1: build_stats_leet(content_obj); break;
            case 2: build_stats_radiation(content_obj); break;
        }
    } else if (cur_div == 1) {
        switch (cur_tab) {
            case 0: build_items_tools(content_obj); break;
            case 1: build_items_collectibles(content_obj); break;
            case 2: build_split_pane(content_obj, sao_items, NUM_SAOS, true, RADIO_SAO_IDX); break;
        }
    } else if (cur_div == 2) {
        switch (cur_tab) {
            case 0: build_data_leds(content_obj); break;
            case 1: build_data_settings(content_obj); break;
            case 2: build_data_theremin(content_obj); break;
        }
    }
}

// ─────────────────────── TAB BAR ──────────────────────────────────────────

static void tab_tap_cb(lv_event_t *e) {
    int32_t idx = (int32_t)(intptr_t)lv_event_get_user_data(e);
    if (idx == cur_tab) {
        // Idempotent re-tap of the ACTIVE tab: snap BOTH panes to top. The left LIST
        // matters most -- on Collectibles it brings the Scan button back into view.
        // Mis-tap-safe -- never rebuilds or stops a running tool.
        if (content_obj) lv_obj_scroll_to_y(content_obj, 0, LV_ANIM_ON);
        if (left_pane)   lv_obj_scroll_to_y(left_pane, 0, LV_ANIM_ON);
        return;
    }
    cur_tab = (uint8_t)idx;
    for (int i = 0; i < NUM_TABS; i++) {
        lv_obj_remove_style(tab_btns[i], &style_tab_btn_active, 0);
        if (i == cur_tab)
            lv_obj_add_style(tab_btns[i], &style_tab_btn_active, 0);
    }
    rebuild_content();
}

static void rebuild_tabs() {
    if (!tab_bar_obj) return;
    clear_children(tab_bar_obj);
    for (int i = 0; i < NUM_TABS; i++) {
        lv_obj_t *btn = lv_button_create(tab_bar_obj);
        lv_obj_remove_style_all(btn);
        lv_obj_add_style(btn, &style_tab_btn, 0);
        if (i == cur_tab)
            lv_obj_add_style(btn, &style_tab_btn_active, 0);
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_height(btn, lv_pct(100));
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, tab_labels[cur_div][i]);
        lv_obj_center(lbl);
        lv_obj_add_event_cb(btn, tab_tap_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        tab_btns[i] = btn;
    }
}

// ─────────────────────── DIVISION BAR ─────────────────────────────────────

// ── Quanta (reward) theme pulse ───────────────────────────────────────────
// Breathes the PRIMARY cyan (the body text) dim<->bright, in time with the LED
// breathe rate — the reward visibly "glows" without stealing screen real estate.
// Runs ONLY while THEME_QUANTA is active. Recolors labels whose text colour
// matches the last breath value, so ONLY primary-coloured text breathes
// (dim/highlight/accent are left alone). Quantized: the screen tree is walked
// only when the 8-bit colour actually changes (a few times/sec), not per frame.
static lv_timer_t *quanta_pulse_timer = nullptr;
static lv_color_t  quanta_breath_cur;
static bool        quanta_breath_init = false;

static void quanta_breath_recolor(lv_obj_t *obj, lv_color_t from, lv_color_t to) {
    uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(obj, i);
        if (lv_obj_check_type(c, &lv_label_class) &&
            lv_color_eq(lv_obj_get_style_text_color(c, LV_PART_MAIN), from))
            lv_obj_set_style_text_color(c, to, 0);
        quanta_breath_recolor(c, from, to);
    }
}

// Re-sync the baseline on a screen rebuild (new labels start at full pal.primary).
static void quanta_pulse_reset(void) { quanta_breath_init = false; }

static void quanta_pulse_cb(lv_timer_t *t) {
    (void)t;
    if (cur_theme_idx != THEME_QUANTA) return;
    uint32_t bp = pal.primary;
    uint8_t br = (bp >> 16) & 0xFF, bg = (bp >> 8) & 0xFF, bb = bp & 0xFF;
    if (!quanta_breath_init) { quanta_breath_cur = lv_color_make(br, bg, bb); quanta_breath_init = true; }
    // Period == the LED breathe period (sync to LED speed, handoff request).
    float period = neo_speed_to_period(cfg.leds[0].speed);
    if (period < 900.0f) period = 900.0f;
    float ph = (float)(millis() % (uint32_t)period) / period;
    float s  = 0.5f - 0.5f * cosf(ph * 6.2831853f);      // 0..1..0
    float k  = 0.45f + 0.55f * s;                         // dim 0.45x -> full bright
    int   q  = (int)(k * 12.0f + 0.5f); k = (float)q / 12.0f;   // quantize -> bound redraws
    lv_color_t nc = lv_color_make((uint8_t)(br * k), (uint8_t)(bg * k), (uint8_t)(bb * k));
    if (!lv_color_eq(nc, quanta_breath_cur)) {
        quanta_breath_recolor(lv_screen_active(), quanta_breath_cur, nc);
        quanta_breath_cur = nc;
    }
}

static void quanta_pulse_set(bool on) {
    if (on && !quanta_pulse_timer) {
        quanta_pulse_timer = lv_timer_create(quanta_pulse_cb, 40, NULL);
    } else if (!on && quanta_pulse_timer) {
        lv_timer_delete(quanta_pulse_timer);
        quanta_pulse_timer = nullptr;
    }
}

static void update_div_indicators() {
    for (int i = 0; i < NUM_DIVISIONS; i++) {
        lv_obj_remove_style(div_btns[i], &style_div_btn_active, 0);
        if (i == cur_div) {
            lv_label_set_text(div_leds[i], "\xE2\x97\x8F");
            lv_obj_add_style(div_btns[i], &style_div_btn_active, 0);
            lv_obj_set_style_text_color(div_leds[i], pip_highlight(), 0);
        } else {
            lv_label_set_text(div_leds[i], "\xE2\x97\x8B");
            lv_obj_set_style_text_color(div_leds[i], pip_dim(), 0);
        }
    }
    quanta_pulse_set(cur_theme_idx == THEME_QUANTA);  // pulse only on the reward theme
    quanta_pulse_reset();   // re-sync breath baseline to the freshly-built labels
}

static void div_tap_cb(lv_event_t *e) {
    int32_t idx = (int32_t)(intptr_t)lv_event_get_user_data(e);
    // Secret-menu input: record BEFORE the same-division early-return so re-tapping
    // the active division still counts (the sequence may repeat a division).
    arg_secret_record(idx == 0 ? ARG_TOK_STATS : idx == 1 ? ARG_TOK_ITEMS : ARG_TOK_DATA);
    if (idx == cur_div) return;
    cur_div = (uint8_t)idx;
    cur_tab = 0;
    cur_sel = -1;
    update_div_indicators();
    rebuild_tabs();
    rebuild_content();
}

// Programmatic navigation to a division+tab (e.g. tapping the running-task
// indicator in the status bar jumps back to the tool). No-op if already there.
static void goto_div_tab(uint8_t d, uint8_t t) {
    if (cur_div == d && cur_tab == t) return;
    cur_div = d;
    cur_tab = t;
    cur_sel = -1;
    update_div_indicators();
    rebuild_tabs();
    rebuild_content();
}

static void create_div_bar(lv_obj_t *parent) {
    div_bar_obj = lv_obj_create(parent);
    lv_obj_remove_style_all(div_bar_obj);
    lv_obj_add_style(div_bar_obj, &style_div_bar, 0);
    lv_obj_set_size(div_bar_obj, SCREEN_W, DIV_BAR_H);
    lv_obj_set_pos(div_bar_obj, 0, SCREEN_H - DIV_BAR_H);
    lv_obj_remove_flag(div_bar_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(div_bar_obj, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(div_bar_obj, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < NUM_DIVISIONS; i++) {
        lv_obj_t *btn = lv_button_create(div_bar_obj);
        lv_obj_remove_style_all(btn);
        lv_obj_add_style(btn, &style_div_btn, 0);
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_height(btn, lv_pct(100));
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(btn, 4, 0);

        lv_obj_t *led = lv_label_create(btn);
        lv_label_set_text(led, "");     // ditto -- the LED glyph is written by the refresh, but
                                        // an unset LVGL label renders the literal "Text"
        lv_obj_set_style_text_font(led, &ui_font_pipboy_14, 0);
        div_leds[i] = led;

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, div_labels[i]);

        lv_obj_add_event_cb(btn, div_tap_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        div_btns[i] = btn;
    }
    update_div_indicators();
}

// ─────────────────────── MAIN SCREEN BUILDER ──────────────────────────────

static void create_main_screen() {
    // Copy mascot image to PSRAM (29KB flash → PSRAM)
    mascot_init_psram();

    scr_main = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr_main);
    lv_obj_add_style(scr_main, &style_screen_bg, 0);
    lv_obj_set_size(scr_main, SCREEN_W, SCREEN_H);
    lv_obj_remove_flag(scr_main, LV_OBJ_FLAG_SCROLLABLE);

    create_status_bar(scr_main);

    tab_bar_obj = lv_obj_create(scr_main);
    lv_obj_remove_style_all(tab_bar_obj);
    lv_obj_add_style(tab_bar_obj, &style_tab_bar, 0);
    lv_obj_set_size(tab_bar_obj, SCREEN_W, TAB_BAR_H);
    lv_obj_set_pos(tab_bar_obj, 0, STATUS_BAR_H);
    lv_obj_remove_flag(tab_bar_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(tab_bar_obj, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tab_bar_obj, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    content_obj = lv_obj_create(scr_main);
    lv_obj_remove_style_all(content_obj);
    lv_obj_add_style(content_obj, &style_container, 0);
    lv_obj_set_size(content_obj, SCREEN_W, CONTENT_H);
    lv_obj_set_pos(content_obj, 0, CONTENT_Y);
    lv_obj_remove_flag(content_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(content_obj, pip_bg(), 0);
    lv_obj_set_style_bg_opa(content_obj, LV_OPA_COVER, 0);

    create_div_bar(scr_main);
    rebuild_tabs();
    rebuild_content();

    status_timer = lv_timer_create(status_bar_timer_cb, 2000, NULL);

    lv_screen_load(scr_main);

    // Apply CRT effects (scanlines + flicker) from saved config
    crt_apply();

    // Register collectible change callback for live UI refresh
    coll_on_change = rebuild_content;

    CB_LOGF("[NAV] Screen created (%s). Free heap: %lu\n",
                  pal.name, (unsigned long)esp_get_free_heap_size());
}

// ─────────────────────── CRT EFFECTS ──────────────────────────────────────
// Scanline overlay: 320x2 A8 image tiled full-screen. Even row = transparent,
// odd row = semi-opaque black. Non-clickable, sits on top of all content.
// Flicker: LVGL timer at random 4-12s intervals briefly dims backlight.

static void crt_scanlines_create(void) {
    if (crt_scanline_img) return;  // already active

    // Allocate 320x2 A8 tile buffer in PSRAM (640 bytes)
    if (!crt_scanline_buf) {
        crt_scanline_buf = (uint8_t *)heap_caps_malloc(320 * 2, MALLOC_CAP_SPIRAM);
        if (!crt_scanline_buf) {
            CB_LOGLN("[CRT] Scanline buffer alloc failed");
            return;
        }
        // Row 0: transparent (alpha=0), Row 1: semi-dark (alpha=40)
        memset(crt_scanline_buf, 0, 320);           // row 0
        memset(crt_scanline_buf + 320, 40, 320);    // row 1
    }

    // Build image descriptor
    crt_scanline_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    crt_scanline_dsc.header.cf     = LV_COLOR_FORMAT_A8;
    crt_scanline_dsc.header.flags  = 0;
    crt_scanline_dsc.header.w      = 320;
    crt_scanline_dsc.header.h      = 2;
    crt_scanline_dsc.header.stride = 320;
    crt_scanline_dsc.data_size     = 320 * 2;
    crt_scanline_dsc.data          = crt_scanline_buf;

    // Create full-screen image widget, tiled
    crt_scanline_img = lv_image_create(lv_screen_active());
    lv_image_set_src(crt_scanline_img, &crt_scanline_dsc);
    lv_obj_set_size(crt_scanline_img, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(crt_scanline_img, 0, 0);
    lv_image_set_inner_align(crt_scanline_img, LV_IMAGE_ALIGN_TILE);

    // Recolor to black (A8 alpha mask → black scanlines)
    lv_obj_set_style_image_recolor(crt_scanline_img, lv_color_black(), 0);
    lv_obj_set_style_image_recolor_opa(crt_scanline_img, LV_OPA_COVER, 0);

    // Non-clickable - touch passes through to content below
    lv_obj_remove_flag(crt_scanline_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(crt_scanline_img, LV_OBJ_FLAG_FLOATING);  // ignore layout

    CB_LOGLN("[CRT] Scanline overlay created");
}

static void crt_scanlines_destroy(void) {
    if (crt_scanline_img) {
        lv_obj_delete(crt_scanline_img);
        crt_scanline_img = NULL;
    }
    // Keep buffer allocated - cheap to reuse if toggled back on
    CB_LOGLN("[CRT] Scanline overlay removed");
}

// Bring scanline overlay to front (call after creating overlays like screensaver/boot)
static void crt_scanlines_raise(void) {
    if (crt_scanline_img) {
        lv_obj_move_foreground(crt_scanline_img);
    }
}

// --- V-hold roll effect (snapshot wrap) ---
// Simulates CRT vertical hold loss: snapshots scr_main, hides it, and animates
// two stacked copies scrolling bottom-to-top so the image wraps around, with a
// black sync bar between them, horizontal tear, and brightness dip.
// Touching the screen during the roll aborts immediately. Occasionally does
// a second roll at a different speed.

#define CRT_VROLL_DURATION_MS   700   // primary roll duration
#define CRT_VROLL_DURATION2_MS  420   // second roll (faster) when double-firing
#define CRT_VROLL_SYNC_BAR_H     10   // black sync bar height (px)
#define CRT_VROLL_TEAR_PX         3   // +/- horizontal tear magnitude
#define CRT_VROLL_TEAR_PERIOD_MS 60   // tear randomize period
#define CRT_VROLL_DIM_OPA        38   // ~15% dim during roll
#define CRT_VROLL_DOUBLE_PCT     20   // % chance of a second roll per firing

static void crt_vroll_anim_cb(void *var, int32_t y) {
    (void)var;
    if (!crt_vroll_upper || !crt_vroll_lower || !crt_vroll_sync_bar) return;
    // Upper copy at y (scrolls up, off the top), lower copy at y+240 (wraps in from below).
    lv_obj_set_pos(crt_vroll_upper, 0, y);
    lv_obj_set_pos(crt_vroll_lower, crt_vroll_tear_x, y + SCREEN_H);
    // Sync bar sits at the seam (top edge of the lower copy).
    lv_obj_set_pos(crt_vroll_sync_bar, 0, y + SCREEN_H);
}

static void crt_vroll_tear_timer_cb(lv_timer_t *t) {
    (void)t;
    // Randomize horizontal tear in [-CRT_VROLL_TEAR_PX .. +CRT_VROLL_TEAR_PX]
    crt_vroll_tear_x = (int32_t)(esp_random() % (CRT_VROLL_TEAR_PX * 2 + 1)) - CRT_VROLL_TEAR_PX;
}

static void crt_vroll_abort() {
    if (!crt_vroll_active) return;

    // Kill any running animations bound to our sentinel var
    lv_anim_delete(&crt_vroll_anim_var, NULL);

    if (crt_vroll_tear_timer) { lv_timer_delete(crt_vroll_tear_timer); crt_vroll_tear_timer = NULL; }

    // Tear down overlays in reverse creation order
    if (crt_vroll_catcher)  { lv_obj_delete(crt_vroll_catcher);  crt_vroll_catcher  = NULL; }
    if (crt_vroll_sync_bar) { lv_obj_delete(crt_vroll_sync_bar); crt_vroll_sync_bar = NULL; }
    if (crt_vroll_dim)      { lv_obj_delete(crt_vroll_dim);      crt_vroll_dim      = NULL; }
    if (crt_vroll_upper)    { lv_obj_delete(crt_vroll_upper);    crt_vroll_upper    = NULL; }
    if (crt_vroll_lower)    { lv_obj_delete(crt_vroll_lower);    crt_vroll_lower    = NULL; }

    // Un-hide the real screen
    if (scr_main) {
        lv_obj_remove_flag(scr_main, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(scr_main, 0, 0);
    }

    // Free snapshot buffer (lv_draw_buf_t allocated inside lv_snapshot_take)
    if (crt_vroll_snap) {
        lv_draw_buf_destroy(crt_vroll_snap);
        crt_vroll_snap = NULL;
    }

    crt_vroll_active = false;
    crt_vroll_double = false;
}

static void crt_vroll_touch_cb(lv_event_t *e) {
    (void)e;
    crt_vroll_abort();
}

static void crt_vroll_done_cb(lv_anim_t *a) {
    (void)a;
    crt_vroll_abort();
}

static void crt_vroll_start_anim(uint32_t duration_ms, lv_anim_completed_cb_t done_cb, lv_anim_path_cb_t path_cb) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &crt_vroll_anim_var);
    lv_anim_set_values(&a, 0, -SCREEN_H);  // bottom-to-top: upper copy scrolls off the top
    lv_anim_set_duration(&a, duration_ms);
    lv_anim_set_exec_cb(&a, crt_vroll_anim_cb);
    lv_anim_set_completed_cb(&a, done_cb);
    lv_anim_set_path_cb(&a, path_cb);
    lv_anim_start(&a);
}

static void crt_vroll_first_done_cb(lv_anim_t *a) {
    if (!crt_vroll_active) return;  // aborted during first roll
    // Snap visuals back to y=0 instantly -- at y=-SCREEN_H the lower copy is at
    // position 0, which is visually identical to y=0 with the upper copy.
    crt_vroll_anim_cb(NULL, 0);
    crt_vroll_start_anim(CRT_VROLL_DURATION2_MS, crt_vroll_done_cb, lv_anim_path_ease_in);
}

static void crt_flicker_fire_cb(lv_timer_t *t) {
    (void)t;
    if (screensaver_active) goto reschedule;
    if (crt_vroll_active) goto reschedule;
    if (!scr_main) goto reschedule;
    // Suppress the V-roll during an HR scan or an active theremin: the rolling
    // screen is distracting while the user holds steady to scan or watches the
    // theremin bars. (Just skip this firing; the next is rescheduled as usual.)
    // Reads the INTENT (wants_active), not the render flag: SB3's duck-and-resume clears
    // aud_theremin_active for the ~100 ms of a tap click, and sampling the render flag in
    // that window would let a V-roll fire while the user is watching the bars.
    if (hr_scanning || audio_theremin_wants_active()) goto reschedule;

    {
        // Snapshot the current screen into PSRAM-backed RGB565 buffer
        crt_vroll_snap = lv_snapshot_take(scr_main, LV_COLOR_FORMAT_RGB565);
        if (!crt_vroll_snap) {
            CB_LOGLN("[CRT] snapshot_take failed, skipping roll");
            goto reschedule;
        }

        crt_vroll_active = true;
        crt_vroll_double = ((esp_random() % 100) < CRT_VROLL_DOUBLE_PCT);
        crt_vroll_tear_x = 0;

        lv_obj_t *top = lv_layer_top();

        // Upper copy at y=0 (will scroll up and off the top)
        crt_vroll_upper = lv_image_create(top);
        lv_image_set_src(crt_vroll_upper, crt_vroll_snap);
        lv_obj_set_pos(crt_vroll_upper, 0, 0);
        lv_obj_remove_flag(crt_vroll_upper, LV_OBJ_FLAG_CLICKABLE);

        // Lower copy initially below the screen -- wraps in as upper scrolls off
        crt_vroll_lower = lv_image_create(top);
        lv_image_set_src(crt_vroll_lower, crt_vroll_snap);
        lv_obj_set_pos(crt_vroll_lower, 0, SCREEN_H);
        lv_obj_remove_flag(crt_vroll_lower, LV_OBJ_FLAG_CLICKABLE);

        // Brightness-dip overlay (above snapshots, below sync bar)
        crt_vroll_dim = lv_obj_create(top);
        lv_obj_remove_style_all(crt_vroll_dim);
        lv_obj_set_size(crt_vroll_dim, SCREEN_W, SCREEN_H);
        lv_obj_set_pos(crt_vroll_dim, 0, 0);
        lv_obj_set_style_bg_color(crt_vroll_dim, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(crt_vroll_dim, CRT_VROLL_DIM_OPA, 0);
        lv_obj_remove_flag(crt_vroll_dim, LV_OBJ_FLAG_CLICKABLE);

        // Sync bar (pure black, above dim so it reads as truly black)
        crt_vroll_sync_bar = lv_obj_create(top);
        lv_obj_remove_style_all(crt_vroll_sync_bar);
        lv_obj_set_size(crt_vroll_sync_bar, SCREEN_W, CRT_VROLL_SYNC_BAR_H);
        lv_obj_set_pos(crt_vroll_sync_bar, 0, SCREEN_H);
        lv_obj_set_style_bg_color(crt_vroll_sync_bar, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(crt_vroll_sync_bar, LV_OPA_COVER, 0);
        lv_obj_remove_flag(crt_vroll_sync_bar, LV_OBJ_FLAG_CLICKABLE);

        // Transparent full-screen touch catcher for abort-on-press
        crt_vroll_catcher = lv_obj_create(top);
        lv_obj_remove_style_all(crt_vroll_catcher);
        lv_obj_set_size(crt_vroll_catcher, SCREEN_W, SCREEN_H);
        lv_obj_set_pos(crt_vroll_catcher, 0, 0);
        lv_obj_add_flag(crt_vroll_catcher, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(crt_vroll_catcher, crt_vroll_touch_cb, LV_EVENT_PRESSED, NULL);

        // Hide the real screen so LVGL skips rendering its entire subtree
        lv_obj_add_flag(scr_main, LV_OBJ_FLAG_HIDDEN);

        // Periodic tear randomizer
        crt_vroll_tear_timer = lv_timer_create(crt_vroll_tear_timer_cb, CRT_VROLL_TEAR_PERIOD_MS, NULL);

        // Kick off the roll
        lv_anim_completed_cb_t done = crt_vroll_double ? crt_vroll_first_done_cb : crt_vroll_done_cb;
        crt_vroll_start_anim(CRT_VROLL_DURATION_MS, done, lv_anim_path_ease_in_out);
    }

reschedule:
    // Randomize next roll: 60-600 seconds
    uint32_t next_ms = 60000 + (esp_random() % 540000);
    lv_timer_set_period(crt_flicker_timer, next_ms);
}

static void crt_flicker_start(void) {
    if (crt_flicker_timer) return;
    // Fire immediately on enable, then random 60-600s intervals
    crt_flicker_timer = lv_timer_create(crt_flicker_fire_cb, 100, NULL);
    CB_LOGLN("[CRT] V-roll timer started");
}

static void crt_flicker_stop(void) {
    crt_vroll_abort();
    if (crt_flicker_timer) {
        lv_timer_delete(crt_flicker_timer);
        crt_flicker_timer = NULL;
    }
    CB_LOGLN("[CRT] V-roll timer stopped");
}

// Apply CRT settings (called from create_main_screen and settings toggles)
static void crt_apply(void) {
    if (cfg.crt_scanlines) crt_scanlines_create();
    else                   crt_scanlines_destroy();

    if (cfg.crt_flicker)   crt_flicker_start();
    else                   crt_flicker_stop();
}

// ─────────────────────── BOOT SCREEN (POST SEQUENCE) ─────────────────────
// Retro BIOS / Pip-Boy boot splash. Lines appear one at a time via timer.
// Real system values mixed with Fallout-universe flavor.

#define BOOT_LINE_DELAY_MS   150   // ms between lines
#define BOOT_HOLD_MS        1200   // pause after last line before dismiss
#define BOOT_MAX_LINES        28

static lv_obj_t   *boot_overlay = NULL;
static lv_obj_t   *boot_label   = NULL;
static lv_timer_t *boot_timer   = NULL;
static uint8_t     boot_line    = 0;

// Lines are built at runtime (some contain live data)
static char boot_lines[BOOT_MAX_LINES][52];  // 51 chars + null (fits 320px @ pipboy_14)
static uint8_t boot_line_count = 0;

static void boot_build_lines(void) {
    boot_line_count = 0;
    auto L = [&](const char *fmt, ...) {
        if (boot_line_count >= BOOT_MAX_LINES) return;
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(boot_lines[boot_line_count], sizeof(boot_lines[0]), fmt, ap);
        va_end(ap);
        boot_line_count++;
    };

    // Gather live data
    uint32_t psram_kb = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024;
    uint32_t psram_free_kb = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024;
    uint32_t flash_kb = ESP.getFlashChipSize() / 1024;
    int bat_pct = BAT_Get_Percentage(BAT_Get_Volts());
    if (bat_pct > 100) bat_pct = 100;
    if (bat_pct < 0) bat_pct = 0;

    L("******* CLIP-OS(R) V3.4.0 *******");
    L("");
    L("COPYRIGHT 2075 CLIPPY INDUSTRIES(R)");
    L("CLIP-BOY 3000 FIELD TERMINAL");
    L("DEFCON 34 LIMITED EDITION");
    L("BIOS DATE 08/07/75  SERIAL: CB3K-DC34");
    L("");
    L("MAIN PROCESSOR: ESP32-S3 @ 240 MHz");
    L("PSRAM: %luK ...................... OK", (unsigned long)psram_kb);
    L("FLASH: %luK .................... OK", (unsigned long)flash_kb);
    L("");
    L("POST: DISPLAY 320x240 TFT ...... OK");
    L("POST: CST328 TOUCH PANEL ....... OK");
    L("POST: NEOPIXEL 8x WS2812B ..... OK");
    L("POST: I2S STEREO AUDIO ......... OK");
    L("POST: VL53L5CX 8x8 LiDAR ...... OK");
    L("POST: SD/MMC INTERFACE ......... OK");
    L("");
    L("INITIALIZING CLIP-OS EXECUTIVE...");
    L("LOADING ROM(1): COLLECTIBLES [%d]", coll_count);
    L("IMAGE DATA: 3700K -> SPIRAM ... OK");
    L("PSRAM FREE: %luK / %luK",
      (unsigned long)psram_free_kb, (unsigned long)psram_kb);
    L("SCANNING FOR DATA TAPE ......... NONE");
    L("");
    L("BATTERY: %d%%", bat_pct);
    L("");
    L("CLIP-OS(R) V3.4.0 READY.");
    L("ROM. ROM never changes.");
}

static void boot_timer_cb(lv_timer_t *t) {
    (void)t;
    if (!boot_overlay || !boot_label) {
        if (boot_timer) { lv_timer_delete(boot_timer); boot_timer = NULL; }
        return;
    }

    if (boot_line < boot_line_count) {
        // Append next line
        if (boot_line == 0) {
            lv_label_set_text(boot_label, boot_lines[0]);
        } else {
            lv_label_ins_text(boot_label, LV_LABEL_POS_LAST, "\n");
            lv_label_ins_text(boot_label, LV_LABEL_POS_LAST, boot_lines[boot_line]);
        }
        // Auto-scroll to bottom
        lv_obj_scroll_to_y(boot_overlay, LV_COORD_MAX, LV_ANIM_OFF);
        boot_line++;

        // After last line, pause then dismiss
        if (boot_line >= boot_line_count) {
            lv_timer_set_period(boot_timer, BOOT_HOLD_MS);
        }
    } else {
        // Hold period elapsed - dismiss
        lv_obj_delete(boot_overlay);
        boot_overlay = NULL;
        boot_label = NULL;
        lv_timer_delete(boot_timer);
        boot_timer = NULL;
        CB_LOGLN("[BOOT] Boot screen dismissed");
    }
}

// ── Early boot splash ────────────────────────────────────────────────────
// The animated POST sequence (show_boot_screen) is timer-driven, so it can
// only advance once loop() is pumping lv_timer_handler. But the heavy init
// (cb.begin / RunSetup, ~5s) blocks setup() before loop() ever runs, leaving
// the panel black the whole time even though the display is live much earlier.
// This static splash is drawn right after display init and flushed with
// lv_refr_now(), so the Clip-Boy mascot shows within ~1.6s while the slow
// init proceeds underneath. Removed just before the animated POST starts.
// Uses a fixed amber + the PROGMEM mascot so it has no dependency on theme
// styles or the PSRAM image cache (neither is ready this early).
static lv_obj_t *boot_splash = NULL;

static void show_boot_splash(void) {
    if (boot_splash) return;
    boot_splash = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(boot_splash);
    lv_obj_set_size(boot_splash, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(boot_splash, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(boot_splash, LV_OPA_COVER, 0);
    lv_obj_clear_flag(boot_splash, LV_OBJ_FLAG_SCROLLABLE);

    const lv_color_t amber = lv_color_hex(0xFFB200);

    lv_obj_t *m = lv_image_create(boot_splash);
    lv_image_set_src(m, &ClipBoyGS153x192);
    lv_obj_set_style_image_recolor(m, amber, 0);
    lv_obj_set_style_image_recolor_opa(m, LV_OPA_COVER, 0);
    lv_obj_align(m, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t *lbl = lv_label_create(boot_splash);
    lv_label_set_text(lbl, "CLIP-OS BOOTING");
    lv_obj_set_style_text_font(lbl, &ui_font_pipboy_14, 0);
    lv_obj_set_style_text_color(lbl, amber, 0);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -14);

    lv_refr_now(NULL);  // push to panel immediately, before the blocking init
}

static void hide_boot_splash(void) {
    if (boot_splash) { lv_obj_delete(boot_splash); boot_splash = NULL; }
}

// ── Quantum-rift boot variant (BADGE_QUANTUM_RIFT) ───────────────────────────
// A "moddable boot screen" feature. Two 320x240 RGB565 images drive it:
//   img_rift_loading -- the loading scene (shown ~1.5s)
//   img_rift_clippy  -- the second scene (shown while the blocking init runs)
//
// The ART is swappable and the private art is NOT shipped publicly:
//   - rift_boot_placeholder.c (committed) defines BOTH images as a generic
//     "CUSTOM BOOT SCREEN" placeholder. It compiles ONLY when no user art is
//     present (guarded by !__has_include("rift_art_present.h")).
//   - To use your own: drop rift_loading.png + rift_loading_with_clippy.png in
//     images/rift/ and run scripts/build_rift_screens.py. That writes
//     rift_loading_img.c + rift_clippy_img.c (defining the two images) and the
//     rift_art_present.h marker -- all git-ignored, so they stay local and the
//     placeholder switches off. See docs/rift-boot-screen.md.
//
// Sequence: rift_show_static() (~1.5s) -> rift_show_clippy() (swaps the scene,
// then the ~5s blocking ClipBoy init runs behind the frozen static scene) ->
// rift_finish() removes the overlay; main + ClipOS POST take over.
#ifdef BADGE_QUANTUM_RIFT
extern "C" {
    LV_IMAGE_DECLARE(img_rift_loading);   // 320x240 RGB565 (user art or placeholder)
    LV_IMAGE_DECLARE(img_rift_clippy);    // 320x240 RGB565 (user art or placeholder)
}

#define RIFT_HOLD_MS  1500   // loading scene shown ~1.5s before the second scene

static lv_obj_t *rift_overlay = nullptr;

// Drive LVGL forward for `ms` while loop() isn't running yet.
static void rift_pump(uint32_t ms) {
    uint32_t t0 = millis();
    do { lv_timer_handler(); delay(4); } while (millis() - t0 < ms);
}

// Loading scene. Drawn BEFORE the blocking init to mask the black screen.
static void rift_show_static(void) {
    if (rift_overlay) return;
    rift_overlay = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(rift_overlay);
    lv_obj_set_size(rift_overlay, SCREEN_W, SCREEN_H);
    lv_obj_clear_flag(rift_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *img = lv_image_create(rift_overlay);
    lv_image_set_src(img, &img_rift_loading);
    lv_obj_set_pos(img, 0, 0);
    lv_refr_now(NULL);
}

// Second scene -- swaps in the pre-composited image. It sits frozen while the
// blocking ClipBoy init runs behind it (static, so the freeze is invisible).
static void rift_show_clippy(void) {
    if (!rift_overlay) return;
    lv_obj_t *img = lv_image_create(rift_overlay);
    lv_image_set_src(img, &img_rift_clippy);
    lv_obj_set_pos(img, 0, 0);
    lv_refr_now(NULL);   // scene is up; the blocking init now runs behind it.
}

// Remove the rift/Clippy overlay after the init, just before the main screen +
// ClipOS POST take over.
static void rift_finish(void) {
    if (!rift_overlay) return;
    lv_obj_delete(rift_overlay);   // deletes the Clippy child too
    rift_overlay = nullptr;
    CB_LOGLN("[BOOT] Quantum rift intro complete");
}
#endif // BADGE_QUANTUM_RIFT

static void show_boot_screen(void) {
    boot_build_lines();
    boot_line = 0;

    boot_overlay = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(boot_overlay);
    lv_obj_set_size(boot_overlay, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(boot_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(boot_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(boot_overlay, 8, 0);
    lv_obj_set_style_pad_gap(boot_overlay, 0, 0);
    lv_obj_set_scroll_dir(boot_overlay, LV_DIR_VER);
    lv_obj_add_style(boot_overlay, &style_scrollbar, LV_PART_SCROLLBAR);
    // Self-null if scr_main is deleted out from under us (a serial theme_set
    // during the boot animation deletes scr_main -> these would dangle and
    // boot_timer_cb's non-NULL guard would write freed memory). LVGL-UAF trap.
    lv_obj_add_event_cb(boot_overlay, cb_selfnull_on_delete, LV_EVENT_DELETE, &boot_overlay);

    boot_label = lv_label_create(boot_overlay);
    lv_label_set_text(boot_label, "");
    lv_obj_set_style_text_font(boot_label, &ui_font_pipboy_14, 0);
    lv_obj_set_style_text_color(boot_label, pip_primary(), 0);
    lv_obj_set_width(boot_label, SCREEN_W - 16);
    lv_label_set_long_mode(boot_label, LV_LABEL_LONG_WRAP);
    lv_obj_add_event_cb(boot_label, cb_selfnull_on_delete, LV_EVENT_DELETE, &boot_label);

    boot_timer = lv_timer_create(boot_timer_cb, BOOT_LINE_DELAY_MS, NULL);
    crt_scanlines_raise();  // scanlines on top of boot overlay too
    CB_LOGLN("[BOOT] Boot screen started");
}

// ─────────────────────── FIRST-BOOT LEGAL NOTICE ─────────────────────────
// Full-screen acknowledgment shown once on first boot (or after factory reset).
// Stored in NVS - user must tap "I Understand" to proceed.

// ─── Clippy Tour ──────────────────────────────────────────────────────────
// Linear sequence of speech-bubble steps walking the user through the badge.
// Each step optionally navigates the underlying UI to a target screen and
// pulses an amber ring around the relevant element so the user can see what
// Clip-Boy is talking about.

struct TourStep {
    const char *text;
    int8_t  div;       // -1 = don't navigate
    int8_t  tab;
    int16_t hx, hy;    // highlight position (top-left)
    int16_t hw, hh;    // highlight size (0 = no highlight on this step)
    int16_t bubble_y;  // explicit bubble Y so highlight + relevant context
                       // (top tabs + bottom div bar) are both visible
    int16_t arrow_x;   // -1 = no arrow; else x-CENTER of a flashing down-arrow
                       // near the bottom edge, pointing at a physical component
};

static const TourStep kTourSteps[] = {
    // ── Physical hardware: ONE item per step, each with a flashing down-arrow
    //    pointing at the bottom EDGE. No nav (div=-1) so the bottom bar stays
    //    put as the landmark; bubble rides high (y=6) so it never covers the
    //    arrow. arrow_x values are letter positions on the bottom bar (3 equal
    //    cells, [dot]+gap+label centered): 'S' in ITEMS=182, mid=223,
    //    'D'-'A' in DATA=265, STATS->ITEMS edge=114.
    // 0: POWER
    { "Hi! I'm Clip-Boy -- let me show you the PHYSICAL buttons on the bottom "
      "edge of the badge (the arrows point right at them).\n\n"
      "This one is POWER (under the 'S' in ITEMS). Press and HOLD it to power "
      "the badge ON.",
      -1, -1,  0, 0, 0, 0,  6,  182 },
    // 1: RESET
    { "RESET is the physical button about midway between POWER and the "
      "bootloader button. Press it to power the badge OFF.",
      -1, -1,  0, 0, 0, 0,  6,  223 },
    // 2: BOOTLOADER
    { "BOOTLOADER is the physical button under the gap between 'D' and 'A' in "
      "DATA. Hold it before/through a reboot to enter recovery (bootloader) mode.",
      -1, -1,  0, 0, 0, 0,  6,  265 },
    // 3: microSD slot
    { "The microSD slot runs along the edge under STATS-into-ITEMS. It takes "
      "a FAT-formatted card (1GB is plenty), contacts facing UP.",
      -1, -1,  0, 0, 0, 0,  6,  114 },
    // 4: orientation -- division bar (ring highlight, no arrow)
    { "Now the screen. The bottom bar has three sections: STATS, ITEMS, "
      "DATA. Tap one to switch sections (after the tour).",
      -1, -1,  0, 214, 320, 26,  60,  -1 },
    // 5: STATS -- bubble in middle so user sees tabs (top) AND div bar (bottom)
    { "STATS is your character. Collected items, L.E.E.T. score, "
      "ambient radiation. Each tab up top shows a different view.",
      0, 0,  0, 16, 320, 26,  60,  -1 },
    // 6: ITEMS / Collectibles + Scan
    { "ITEMS holds your gear. Collectibles is where you scan HR "
      "codes around DEFCON to unlock new finds.",
      1, 1,  0, 42, 135, 36,  124,  -1 },
    // 6b: ITEMS / SAOs -- Whether Radio
    { "ITEMS > SAOs lists add-ons. Most are jokes... but tap 'Whether Radio' -- "
      "whether it's a radio or not is still unclear.",
      1, 2,  0, 42, 135, 36,  124,  -1 },
    // 7: DATA -- same middle-bubble pattern
    { "DATA is the toy box: LEDs, the theremin, and settings.",
      2, 0,  0, 16, 320, 26,  60,  -1 },
    // 8: status bar -- middle bubble so user sees status bar (top) and
    // the rest of the screen below
    { "Top of the screen: battery and free RAM on the left; a "
      "flashlight and a \"?\" on the right. Tap \"?\" any time "
      "for help.",
      0, 0,  0, 0, 320, 16,  60,  -1 },
    // 9: open source
    { "One more thing: Clip-Boy is open source: firmware, the works.\n\n"
      "Grab it at github.com/SafeHazard/Clip-Boy and make it your own.",
      -1, -1,  0, 0, 0, 0,  60,  -1 },
    // 10: done
    { "That's it. Re-launch this tour any time from DATA > Settings "
      "> Help, then tap Tour.\n\n"
      "Have fun and research responsibly -- only on gear you own or are authorized to test.",
      -1, -1,  0, 0, 0, 0,  6,  -1 },
};

static constexpr int kNumTourSteps =
    (int)(sizeof(kTourSteps) / sizeof(kTourSteps[0]));

static int       g_tour_step  = -1;
static lv_obj_t *g_tour_modal = nullptr;

static void show_clippy_tour_step(int n);

static void clippy_tour_close(void) {
    if (g_tour_modal) {
        lv_obj_delete(g_tour_modal);
        g_tour_modal = nullptr;
    }
    g_tour_step = -1;
}

static void clippy_tour_advance(void) {
    int next = g_tour_step + 1;
    if (next >= kNumTourSteps) {
        clippy_tour_close();
        return;
    }
    show_clippy_tour_step(next);
}

static void show_clippy_tour_step(int n) {
    if (n < 0 || n >= kNumTourSteps) {
        clippy_tour_close();
        return;
    }
    const TourStep &s = kTourSteps[n];

    // Navigate the underlying screen so the highlight lines up with what
    // the speech bubble describes.
    if (s.div >= 0) {
        cur_div = (uint8_t)s.div;
        cur_tab = (uint8_t)s.tab;
        rebuild_content();
    }

    if (g_tour_modal) {
        lv_obj_delete(g_tour_modal);
        g_tour_modal = nullptr;
    }
    g_tour_step = n;

    // Dim overlay; takes input so taps on the underlying UI don't sneak past.
    g_tour_modal = lv_obj_create(lv_screen_active());
    // Class sweep 2026-07-25: this was the LAST screen-parented modal global with no
    // LV_EVENT_DELETE handler, and ui_theme_switch_live() (which frees the whole scr_main
    // tree) never nulled it -- so it dangled non-NULL. Two consequences, both bad:
    //   1) `radio_screen_busy()` reads `g_tour_modal != nullptr` and is polled by the 1 Hz
    //      screensaver check, so a stale non-NULL means the screensaver NEVER fires again for
    //      the rest of the boot -- which also silently defeats Dark Charge (implemented as the
    //      screensaver dark state) and suppresses radio nudges forever.
    //   2) clippy_tour_close() and show_clippy_tour_step() both lv_obj_delete() it with no
    //      validity guard, so re-running the tour is a delete on freed heap.
    // Now identity-checked-self-nulled like every sibling (g_station_modal, huepick_modal,
    // custom_reveal_modal, coll_fullscreen_overlay, hr_blackout, mon_modal, kb_modal).
    lv_obj_add_event_cb(g_tour_modal, cb_selfnull_on_delete, LV_EVENT_DELETE, &g_tour_modal);
    lv_obj_remove_style_all(g_tour_modal);
    lv_obj_set_size(g_tour_modal, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(g_tour_modal, 0, 0);
    lv_obj_set_style_bg_color(g_tour_modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_tour_modal, LV_OPA_60, 0);
    lv_obj_add_flag(g_tour_modal, LV_OBJ_FLAG_CLICKABLE);

    // Pulsing highlight ring around the target rect.
    if (s.hw > 0 && s.hh > 0) {
        lv_obj_t *ring = lv_obj_create(g_tour_modal);
        lv_obj_remove_style_all(ring);
        lv_obj_set_pos(ring, s.hx, s.hy);
        lv_obj_set_size(ring, s.hw, s.hh);
        lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(ring, 3, 0);
        lv_obj_set_style_border_color(ring, pip_highlight(), 0);
        lv_obj_set_style_radius(ring, 4, 0);

        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, ring);
        lv_anim_set_values(&a, LV_OPA_40, LV_OPA_COVER);
        lv_anim_set_duration(&a, 700);
        lv_anim_set_playback_duration(&a, 700);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_exec_cb(&a, [](void *obj, int32_t v) {
            lv_obj_set_style_border_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
        });
        lv_anim_start(&a);
    }

    // Flashing down-arrow pointing at a physical component on the bottom edge
    // (hardware steps). Bobs up/down just above the div bar to draw the eye to
    // the right horizontal spot; the bubble text names the landmark.
    if (s.arrow_x >= 0) {
        lv_obj_t *arr = lv_label_create(g_tour_modal);
        lv_label_set_text(arr, LV_SYMBOL_DOWN);
        lv_obj_set_style_text_font(arr, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(arr, pip_highlight(), 0);
        lv_obj_update_layout(arr);
        int aw = lv_obj_get_width(arr);
        // Bottom out at the very screen edge so the arrow points PAST the div bar
        // to the physical button below, not at a UI element. Tip reaches ~y=240.
        int ay = 216;
        lv_obj_set_pos(arr, s.arrow_x - aw / 2, ay);

        lv_anim_t aa;
        lv_anim_init(&aa);
        lv_anim_set_var(&aa, arr);
        lv_anim_set_values(&aa, ay - 12, ay);   // bob up, finish the downward cycle at the edge
        lv_anim_set_duration(&aa, 450);
        lv_anim_set_playback_duration(&aa, 450);
        lv_anim_set_repeat_count(&aa, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_exec_cb(&aa, [](void *obj, int32_t v) {
            lv_obj_set_y((lv_obj_t *)obj, v);
        });
        lv_anim_start(&aa);
    }

    // Speech bubble: a content-sized vertical stack (indicator / text /
    // button row). Sizing to content means the text can NEVER run under the
    // buttons (the old fixed-116px box let long steps overflow behind them).
    // bubble_y is the PREFERRED top; a tall step that would run off the
    // bottom is slid up so it stays fully on-screen.
    int bubble_w = SCREEN_W - 12;
    int inner_w  = bubble_w - 16;  // minus pad_all(8) * 2

    lv_obj_t *bubble = lv_obj_create(g_tour_modal);
    lv_obj_remove_style_all(bubble);
    lv_obj_set_width(bubble, bubble_w);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(bubble, pip_bg(), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bubble, 2, 0);
    lv_obj_set_style_border_color(bubble, pip_primary(), 0);
    lv_obj_set_style_radius(bubble, 6, 0);
    lv_obj_set_style_pad_all(bubble, 8, 0);
    lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(bubble, 5, 0);
    lv_obj_remove_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);

    // Step indicator "n/N" -- its own right-aligned row so the Back button
    // can't hide it (it used to share the bottom-left corner with Back).
    char step_buf[8];
    snprintf(step_buf, sizeof(step_buf), "%d/%d", n + 1, kNumTourSteps);
    lv_obj_t *step_lbl = lv_label_create(bubble);
    lv_obj_set_width(step_lbl, inner_w);
    lv_label_set_text(step_lbl, step_buf);
    lv_obj_set_style_text_align(step_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(step_lbl, pip_dim(), 0);
    lv_obj_set_style_text_font(step_lbl, &ui_font_pipboy_14, 0);

    lv_obj_t *txt = lv_label_create(bubble);
    lv_label_set_long_mode(txt, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(txt, inner_w);
    lv_label_set_text(txt, s.text);
    lv_obj_set_style_text_color(txt, pip_primary(), 0);
    lv_obj_set_style_text_font(txt, &ui_font_pipboy_14, 0);

    // Button row: evenly spaced; absent buttons (Back on first / Skip on
    // last) just let the rest spread out.
    lv_obj_t *brow = lv_obj_create(bubble);
    lv_obj_remove_style_all(brow);
    lv_obj_set_width(brow, inner_w);
    lv_obj_set_height(brow, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(brow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(brow, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(brow, LV_OBJ_FLAG_SCROLLABLE);

    auto make_tour_btn = [](lv_obj_t *parent, const char *label,
                            lv_event_cb_t cb) {
        lv_obj_t *b = lv_btn_create(parent);
        lv_obj_set_size(b, 78, 26);
        lv_obj_set_style_bg_color(b, pip_primary(), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, label);
        lv_obj_set_style_text_color(l, lv_color_black(), 0);
        lv_obj_set_style_text_font(l, &ui_font_pipboy_14, 0);
        lv_obj_center(l);
        return b;
    };

    bool first = (n == 0);
    bool last  = (n == kNumTourSteps - 1);

    if (!first) {
        make_tour_btn(brow, "< Back", [](lv_event_t *e) {
            (void)e;
            int prev = g_tour_step - 1;
            if (prev < 0) prev = 0;
            show_clippy_tour_step(prev);
        });
    }
    if (!last) {
        make_tour_btn(brow, "Skip", [](lv_event_t *e) {
            (void)e; clippy_tour_close();
        });
    }
    make_tour_btn(brow, last ? "Done" : "Next >", [](lv_event_t *e) {
        (void)e; clippy_tour_advance();
    });

    // Resolve the content height, then clamp the bubble fully on-screen.
    lv_obj_update_layout(bubble);
    int bh = lv_obj_get_height(bubble);
    int by = s.bubble_y;
    if (by + bh > SCREEN_H - 2) by = SCREEN_H - 2 - bh;
    if (by < 2) by = 2;
    lv_obj_set_pos(bubble, 6, by);
}

// ─── "New station detected" popup (radio feature) ─────────────────────────
// Reusable modal: a centered card with a radio image (Clippy placeholder for
// now), the station name, and two buttons -- Close (dismiss) and View (runs
// on_view, then dismiss; on_view may be NULL). Call from the radio feature when
// a new station is discovered.
static lv_obj_t *g_station_modal   = nullptr;
static lv_obj_t *g_station_never_ckb = nullptr;   // "don't nag me" toggle (if present)
static void    (*g_station_view_cb)(void) = nullptr;

static void station_modal_close(void) {
    g_station_never_ckb = nullptr;   // opt-out is committed by the Close handler, not here
    if (g_station_modal) { lv_obj_delete(g_station_modal); g_station_modal = nullptr; }
    g_station_view_cb = nullptr;
}

// FYI shown after a user opts out of radio alerts -- tells them the full effect
// and where to undo it. Deferred so we're not creating a modal inside the Close
// button's own event.
// Two-way confirm BEFORE committing an opt-out: warns about the full cost (no
// reminders AND no new-content/reward alerts) and lets them change their mind.
static void radio_optout_confirm_async(void *p) {
    (void)p;
    lv_obj_t *modal = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(modal);
    lv_obj_set_size(modal, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_80, 0);
    lv_obj_remove_flag(modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *box = lv_obj_create(modal);
    lv_obj_remove_style_all(box);
    lv_obj_set_width(box, 290);
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(box, SCREEN_H - 8, 0);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, pip_bg(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, pip_highlight(), 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_pad_all(box, 12, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(box, 8, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    make_label(box, "TURN OFF RADIO ALERTS?", &ui_font_pipboy_16, pip_highlight());
    lv_obj_t *m = make_label(box,
        "You'll stop getting radio reminders AND you won't be told when your "
        "collectibles unlock new stations and badge content.\n\n"
        "You can turn alerts back on in Settings > Radio Alerts.",
        &ui_font_pipboy_14, pip_primary());
    lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(m, lv_pct(100));
    lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *row = lv_obj_create(box);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    auto mkb = [&](const char *txt, lv_color_t bg, lv_color_t txtcol, lv_event_cb_t cb) {
        lv_obj_t *b = lv_button_create(row);
        lv_obj_set_size(b, 118, 32);
        lv_obj_set_style_bg_color(b, bg, 0);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_font(l, &ui_font_pipboy_14, 0);
        lv_obj_set_style_text_color(l, txtcol, 0);
        lv_obj_center(l);
    };
    // "Keep On" is the prominent/safe default; "Turn Off" is dim but still readable.
    mkb("Keep On", pip_highlight(), pip_bg(), [](lv_event_t *e) {
        lv_obj_t *b = (lv_obj_t *)lv_event_get_target(e);
        lv_obj_delete(lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(b))));  // b->row->box->modal
    });
    mkb("Turn Off", pip_border(), pip_primary(), [](lv_event_t *e) {
        cfg.radio_reminder_off = true;
        cfg_save_radio_discovery();
        lv_obj_t *b = (lv_obj_t *)lv_event_get_target(e);
        lv_obj_delete(lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(b))));
    });
    crt_scanlines_raise();
}

static void show_new_station_modal(const char *station_name, const char *subtitle,
                                   void (*on_view)(void), bool allow_optout) {
    station_modal_close();              // never stack two
    g_station_view_cb = on_view;

    // Dim full-screen overlay (clickable so taps don't fall through to the UI).
    g_station_modal = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(g_station_modal);
    lv_obj_set_size(g_station_modal, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(g_station_modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_station_modal, LV_OPA_60, 0);
    lv_obj_add_flag(g_station_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_station_modal, cb_selfnull_on_delete, LV_EVENT_DELETE, &g_station_modal);

    // Centered card.
    lv_obj_t *card = lv_obj_create(g_station_modal);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 264, allow_optout ? 232 : 204);   // taller when the opt-out toggle is shown
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, pip_bg(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, pip_highlight(), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 6, 0);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, 3, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "NEW STATION DETECTED");
    lv_obj_set_style_text_font(title, &ui_font_pipboy_16, 0);
    lv_obj_set_style_text_color(title, pip_highlight(), 0);

    // Radio image -- Clippy placeholder for now (A8, theme-recolored). Scaled to
    // ~62x78 so the card layout is stable when real art is swapped in.
    lv_obj_t *img = lv_image_create(card);
    lv_image_set_src(img, mascot_image());
    lv_obj_set_style_image_recolor(img, pip_primary(), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_image_set_scale(img, 74);        // ~0.29x of 153x192 -> ~44x56 (kept small so the card fits 240px)
    // An image's LAYOUT footprint is its NATIVE size (153x192), not the scaled
    // size -- without an explicit box it shoves the name + buttons off the card.
    // Pin a small box and center the scaled art inside it.
    lv_obj_set_size(img, 46, 56);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);

    lv_obj_t *name = lv_label_create(card);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, lv_pct(100));
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(name, station_name ? station_name : "");
    lv_obj_set_style_text_font(name, &ui_font_pipboy_18, 0);
    lv_obj_set_style_text_color(name, pip_primary(), 0);

    // Optional subtitle -- e.g. "your collection pulled it out of the static"
    // (makes clear that collecting is what reveals stations).
    if (subtitle && subtitle[0]) {
        lv_obj_t *sub = lv_label_create(card);
        lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(sub, lv_pct(100));
        lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(sub, subtitle);
        lv_obj_set_style_text_font(sub, &ui_font_pipboy_14, 0);
        lv_obj_set_style_text_color(sub, pip_dim(), 0);
    }

    // Optional "don't nag me" toggle. A checkable button with an ASCII [ ]/[X]
    // prefix -- NOT lv_checkbox, whose checkmark needs a symbol font the pip-boy
    // face lacks (would render tofu). Read in station_modal_close().
    if (allow_optout) {
        lv_obj_t *ck = lv_button_create(card);
        lv_obj_remove_style_all(ck);
        lv_obj_add_flag(ck, LV_OBJ_FLAG_CHECKABLE);
        lv_obj_set_width(ck, lv_pct(100));
        lv_obj_set_height(ck, LV_SIZE_CONTENT);
        lv_obj_t *ckl = lv_label_create(ck);
        lv_label_set_text(ckl, "[ ] Don't nag me about the radio");
        lv_obj_set_style_text_font(ckl, &ui_font_pipboy_14, 0);
        lv_obj_set_style_text_color(ckl, pip_dim(), 0);
        lv_obj_center(ckl);
        lv_obj_add_event_cb(ck, [](lv_event_t *e) {
            lv_obj_t *b = (lv_obj_t *)lv_event_get_target(e);
            lv_obj_t *l = lv_obj_get_child(b, 0);
            bool on = lv_obj_has_state(b, LV_STATE_CHECKED);
            lv_label_set_text(l, on ? "[X] Don't nag me about the radio"
                                    : "[ ] Don't nag me about the radio");
        }, LV_EVENT_VALUE_CHANGED, NULL);
        g_station_never_ckb = ck;
    }

    // Buttons: Close | View.
    lv_obj_t *brow = lv_obj_create(card);
    lv_obj_remove_style_all(brow);
    lv_obj_set_width(brow, lv_pct(100));
    lv_obj_set_height(brow, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(brow, 2, 0);
    lv_obj_set_flex_flow(brow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(brow, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(brow, LV_OBJ_FLAG_SCROLLABLE);

    auto mkbtn = [&](const char *txt, lv_event_cb_t cb) {
        lv_obj_t *b = lv_button_create(brow);
        lv_obj_set_size(b, 112, 30);
        lv_obj_set_style_bg_color(b, pip_highlight(), 0);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_font(l, &ui_font_pipboy_16, 0);
        lv_obj_set_style_text_color(l, pip_bg(), 0);
        lv_obj_center(l);
    };
    mkbtn("Close", [](lv_event_t *e){
        (void)e;
        // Opt-out commits only via Close (tapping View = engaging, so ignore it).
        bool optout = g_station_never_ckb &&
                      lv_obj_has_state(g_station_never_ckb, LV_STATE_CHECKED);
        station_modal_close();
        if (optout && !cfg.radio_reminder_off)
            lv_async_call(radio_optout_confirm_async, nullptr);   // confirm (2-way) before committing
    });
    mkbtn("View",  [](lv_event_t *e){
        (void)e;
        void (*cb)(void) = g_station_view_cb;
        station_modal_close();          // tear down first
        if (cb) cb();                   // then run the caller's View action
    });

    crt_scanlines_raise();              // keep CRT scanlines on top, if enabled
}

// ─── SegFault-Tec FM radio (DC34-131 shell) ───────────────────────────────
// Full-screen sub-UI built into content_obj (info-page pattern): a station
// list (left), a signal/oscilloscope area (upper-right, animated in DC34-132),
// and tuned-station meta + volume + Back (right). Entry = the SegFault-Tec FM
// SAO. Audio (DC34-134) and the live scope/drift (DC34-132/133) land next;
// this slice is the static shell + tuning + lifecycle.
//
// LIFECYCLE: every widget lives in content_obj, so rebuild_content() (division/
// tab nav) deletes them — radio_stop() runs first (teardown hook below) and
// the Back button routes through rebuild_content() too. Refs self-null here so
// the 132/134 timer/audio additions inherit a UAF-safe teardown.

#define RADIO_SCOPE_N  36              // waveform sample points across the scope
#define RADIO_SCOPE_W  174            // scope inner draw width  (right pane - pads/border)
#define RADIO_SCOPE_H  48             // scope inner draw height

// DC34-133 tuning-drift lock — forgiving by design (band-not-point + dwell-snap).
#define RADIO_TUNE_BAND   10          // |dial - target| <= this = in the band
#define RADIO_TUNE_INNER   3          // <= this = instant snap-lock (dead center)
#define RADIO_TUNE_DWELL   5          // in-band, low-velocity ticks (×50ms) to auto-lock

static bool       radio_active     = false;
// Screensaver exemption accessor (fwd-declared up by screensaver_check_cb, which
// lives above the radio system). A tuned-in station counts as "actively doing
// something" so the display timeout can't blank mid-broadcast.
static bool cb_radio_is_playing(void) { return radio_active; }
static int8_t     radio_cur        = 0;        // tuned station index
static lv_obj_t  *radio_list       = NULL;     // left station list
static lv_obj_t  *radio_scope_line = NULL;     // DC34-132 oscilloscope trace (lv_line)
static lv_timer_t*radio_scope_timer= NULL;     // ~20fps scope animator
static uint32_t   radio_scope_phase= 0;        // advances each tick
static lv_point_precise_t radio_scope_pts[RADIO_SCOPE_N];  // lv_line keeps this ptr — must be static
static lv_obj_t  *radio_name_lbl   = NULL;     // tuned station name
static lv_obj_t  *radio_meta_lbl   = NULL;     // "88.5 FM ~ VOICE"
static lv_obj_t  *radio_vol_slider = NULL;
static lv_obj_t  *radio_tune_slider= NULL;     // DC34-133 tuning dial
static bool       radio_drifting   = false;    // mid tune-in (off-band) for the current station
static int        radio_tune_target= 50;       // dial value that locks the current station
static int        radio_dwell      = 0;        // consecutive in-band low-velocity ticks
static int        radio_tune_last  = -1;       // previous dial value (velocity gate)
static uint16_t   radio_locked_mask= 0;        // this-boot: stations already tuned-in (Once Per Boot)
// Playlist state (defined here so radio_scope_tick can poll it; functions below).
// Shuffle-bag model: a "deck" for speech + a deck for [music] clips. Every clip plays
// once per cycle (even distribution, no clustering) vs independent random draws. When a
// station's `alternate` flag is set (WGHOUL/TabStreet) the player ALTERNATES speech<->
// music (radio talk/song format); otherwise everything shares ONE bag (single even shuffle).
// Rebuilt (reshuffled) each time a station begins playing.
static int16_t    radio_bag_sp[32], radio_bag_mu[32];   // g_radio_clips indices, shuffled
static uint8_t    radio_bag_sp_n = 0, radio_bag_mu_n = 0;
static uint8_t    radio_bag_sp_pos = 0, radio_bag_mu_pos = 0;   // cursor into each deck
static bool       radio_next_music = false;    // alternation toggle (starts on speech)
static int        radio_pl_n   = 0;            // total available clips (sp_n + mu_n)
static int        radio_pl_cur = -1;           // g_radio_clips index now playing (-1 = none)
static bool       radio_pl_on  = false;
static lv_obj_t  *radio_transcript_lbl = nullptr;  // live Transcript label (self-nulls on delete)

static void radio_apply_display(void);         // fwd — meta+scope reflect drift/lock state
static void radio_lock_in(void);               // fwd — snap + lock the current station
static void radio_audio_update(void);          // fwd — start/stop bed + static for the tuned station
static void radio_pl_advance(void);            // fwd — roll the next random clip (called on EOF)

// Is station i available? Count-based unlock (locked lineup, 2026-06-30):
// gate_count = collectibles you must have found (0 = always on). Spread 0/1/25/
// 50/75; the all-found custom-theme reward is handled separately (see build doc).
static bool radio_station_unlocked(int i) {
    if (i < 0 || i >= (int)NUM_RADIO_STATIONS) return false;
    uint8_t need = radio_stations[i].gate_count;
    if (need == 0) return true;                        // always available (e.g. the ARG numbers station)
    return coll_count_found() >= (int)need;            // unlocked once enough collectibles are found
}

static const char *radio_scope_name(radio_scope_t s) {
    switch (s) {
        case RSCOPE_TALK:    return "VOICE";
        case RSCOPE_MUSIC:   return "MUSIC";
        case RSCOPE_NUMBERS: return "NUMBERS";
        case RSCOPE_BEACON:  return "BEACON";
        default:             return "DEAD AIR";
    }
}

// Teardown — stop audio + scope/drift timers before the widgets vanish.
// Idempotent. Called from BOTH UAF teardown paths (rebuild_content +
// ui_theme_switch_live). 134 hangs audio_radio_stop() here.
static void radio_stop(void) {
    if (!radio_active) return;
    if (radio_scope_timer) { lv_timer_delete(radio_scope_timer); radio_scope_timer = NULL; }
    audio_mp3_stream_stop();            // DC34-134: stop any streaming station audio
    audio_static_stop();                // stop tuning hiss
    radio_pl_on = false; radio_pl_cur = -1;
    radio_active     = false;
    radio_drifting   = false;          // radio_locked_mask persists across opens (this boot)
    radio_dwell      = 0;
    radio_tune_last  = -1;
    radio_list       = NULL;
    radio_scope_line = NULL;
    radio_name_lbl   = NULL;
    radio_meta_lbl   = NULL;
    radio_vol_slider = NULL;
    radio_tune_slider= NULL;
}

// Cheap deterministic noise in [-1,1] for the spiky/hiss waveforms.
static inline float radio_hashf(int n) {
    uint32_t h = (uint32_t)n * 2654435761u;
    h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
    return ((float)(h & 0xFFFF) / 32768.0f) - 1.0f;
}

// Synthetic waveform value in [-1,1] for sample column i at phase ph. Each
// scope archetype has a distinct visual signature (design §4d). DC34-134 can
// later modulate amplitude from the live audio RMS; this is the idle look.
static float radio_scope_wave(radio_scope_t sc, int i, uint32_t ph) {
    float x = (float)i, p = (float)ph;
    switch (sc) {
        case RSCOPE_MUSIC:                       // smooth rolling
            return 0.70f * sinf(x * 0.45f + p * 0.12f)
                 + 0.20f * sinf(x * 0.90f - p * 0.07f);
        case RSCOPE_TALK: {                      // spiky, syllabic
            float env = 0.40f + 0.45f * sinf(p * 0.18f + x * 0.05f);
            return env * (0.60f * sinf(x * 1.7f + p * 0.5f)
                        + 0.40f * radio_hashf(i * 7 + (int)ph));
        }
        case RSCOPE_NUMBERS: {                   // cold sine + periodic blip
            float base = 0.35f * sinf(x * 0.8f + p * 0.2f);
            bool blip = ((((int)ph / 6) + i) % 17) == 0;
            return base + (blip ? 0.65f : 0.0f);
        }
        case RSCOPE_BEACON: {                    // slow pulse, mostly quiet
            float pulse = sinf(p * 0.08f);
            float env = pulse > 0.6f ? (pulse - 0.6f) * 2.5f : 0.0f;
            return env * sinf(x * 1.2f + p * 0.4f);
        }
        case RSCOPE_DEAD:
        default:                                 // flatline + hiss
            return 0.12f * radio_hashf(i * 13 + (int)ph);
    }
}

static void radio_scope_tick(lv_timer_t *t) {
    (void)t;
    if (!radio_active || !radio_scope_line) return;
    radio_scope_phase++;

    // Playlist: when the current clip finishes (stream idle), roll the next random one.
    if (radio_pl_on && !radio_drifting && !aud_mp3s_active && !screensaver_active) radio_pl_advance();

    // DC34-133: while drifting, poll the dial for a forgiving lock. Dead-center
    // snaps instantly; in-band + held-steady auto-locks after a short dwell.
    if (radio_drifting && radio_tune_slider) {
        int v = (int)lv_slider_get_value(radio_tune_slider);
        int d = abs(v - radio_tune_target);
        audio_static_set_level((float)d / 40.0f);   // loud off-band, fades to clean at lock
        if (d <= RADIO_TUNE_INNER) {
            radio_lock_in();
        } else if (d <= RADIO_TUNE_BAND) {
            if (radio_tune_last >= 0 && abs(v - radio_tune_last) <= 1) radio_dwell++;
            else radio_dwell = 0;
            if (radio_dwell >= RADIO_TUNE_DWELL) radio_lock_in();
        } else {
            radio_dwell = 0;
        }
        radio_tune_last = v;
    } else if (radio_tune_slider && cfg.radio_drift != 0 && radio_station_unlocked(radio_cur)) {
        // Locked, but the dial stays LIVE: detune past the band and the station
        // drops back into static (unless drift is Disabled, which keeps it pinned).
        int v = (int)lv_slider_get_value(radio_tune_slider);
        if (abs(v - radio_tune_target) > RADIO_TUNE_BAND) {
            radio_drifting  = true;
            radio_dwell     = 0;
            radio_tune_last = -1;
            radio_apply_display();   // meta -> "<<< TUNING >>>", scope -> dead-air
            radio_audio_update();    // stop the bed, bring the static back
        }
    }

    bool unlocked = radio_station_unlocked(radio_cur);
    // Drifting (off-band) or gated stations read as dead-air static until locked.
    radio_scope_t sc = (radio_drifting || !unlocked) ? RSCOPE_DEAD
                                                     : radio_stations[radio_cur].scope;
    const int mid = RADIO_SCOPE_H / 2, amp = mid - 2;
    for (int i = 0; i < RADIO_SCOPE_N; i++) {
        float v = radio_scope_wave(sc, i, radio_scope_phase);
        if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
        radio_scope_pts[i].x = 2 + (lv_value_precise_t)((float)i * (RADIO_SCOPE_W - 4) / (RADIO_SCOPE_N - 1));
        radio_scope_pts[i].y = mid - (lv_value_precise_t)(v * amp);
    }
    lv_line_set_points(radio_scope_line, radio_scope_pts, RADIO_SCOPE_N);
}

// Each station gets a DISTINCT, evenly-spaced dial slot (for N stations: 0, 100/(N-1),
// ... 100 -> 0/25/50/75/100 for 5), then the slots are randomly PERMUTED across the
// stations once per boot. The old `30 + (i*9)%41` packed all 5 into 30..66 (9 apart) so
// they clustered and the "hunt" was a chore. Playlist-style: spread + fresh each boot,
// zero dup risk. radio_assign_slots() is called at boot (setup) + lazily on first use.
static uint8_t radio_slot[NUM_RADIO_STATIONS];
static bool    radio_slots_ready = false;
static void radio_assign_slots(void) {
    const int n = (int)NUM_RADIO_STATIONS;
    for (int i = 0; i < n; i++)
        radio_slot[i] = (n > 1) ? (uint8_t)((i * 100) / (n - 1)) : 50;   // 0,25,50,75,100
    for (int i = n - 1; i > 0; i--) {                                    // Fisher-Yates shuffle
        int j = (int)(esp_random() % (uint32_t)(i + 1));
        uint8_t t = radio_slot[i]; radio_slot[i] = radio_slot[j]; radio_slot[j] = t;
    }
    radio_slots_ready = true;
}
// Where on the dial station i locks (its shuffled distinct slot). Stable within a boot.
static int radio_tune_target_for(int i) {
    if (!radio_slots_ready) radio_assign_slots();
    if (i < 0 || i >= (int)NUM_RADIO_STATIONS) return 50;
    return (int)radio_slot[i];
}

// Should tuning to station i require the drift minigame right now?
static bool radio_should_drift(int i) {
    if (cfg.radio_drift == 0) return false;             // Disabled (accessibility off-switch)
    if (!radio_station_unlocked(i)) return false;       // gated stations are pure static anyway
    if (cfg.radio_drift == 1 && (radio_locked_mask & (1u << i))) return false; // Once Per Boot, done
    return true;                                        // Every Access, or first tune this boot
}

// Update the meta line + scope color to reflect the current drift/lock state.
static void radio_apply_display(void) {
    const RadioStation *st = &radio_stations[radio_cur];
    bool unlocked = radio_station_unlocked(radio_cur);
    if (radio_name_lbl) lv_label_set_text(radio_name_lbl, st->name);
    if (radio_meta_lbl) {
        if (radio_drifting) {
            lv_label_set_text(radio_meta_lbl, "<<<  TUNING  >>>");
            lv_obj_set_style_text_color(radio_meta_lbl, pip_disabled(), 0);
        } else {
            char buf[40];
            snprintf(buf, sizeof(buf), "%u.%u FM  ~  %s",
                     (unsigned)(st->freq_dkhz / 10), (unsigned)(st->freq_dkhz % 10),
                     unlocked ? radio_scope_name(st->scope) : "WEAK SIGNAL");
            lv_label_set_text(radio_meta_lbl, buf);
            lv_obj_set_style_text_color(radio_meta_lbl,
                                        unlocked ? pip_dim() : pip_disabled(), 0);
        }
    }
    if (radio_scope_line)
        lv_obj_set_style_line_color(radio_scope_line,
            (radio_drifting || !unlocked) ? pip_disabled() : pip_primary(), 0);
}

// Start/stop the tuned station's streamed bed to match the current state.
// Called whenever selection/drift/lock changes; always stops first so switching
// stations or drifting silences cleanly. MVP: only Pirate 0x7C00 has a bed.
// Loudness/mute are handled downstream by aud_volume; audio is not RF so
// airplane mode is irrelevant here.
// ── Radio playlist: a station's clips (audio/<prefix>-*.mp3, embedded in
// g_radio_clips[]) played in RANDOM order on an endless loop. Some clips are
// gated -- the πr8 r4di0 songs 1-2/1-3 unlock once ARG puzzle 1 is complete.
// (State lives up in the radio-globals block so radio_scope_tick can poll it.)
static bool radio_clip_available(const char *name) {
    if (!strcmp(name, "1-2") || !strcmp(name, "1-3")) return arg_flag(ARG_P1_RADIO);
    return true;
}
// Fisher-Yates shuffle of a bag of clip indices (esp_random = uniform HW RNG).
static void radio_bag_shuffle(int16_t *bag, uint8_t n) {
    for (int i = (int)n - 1; i > 0; i--) {
        int j = (int)(esp_random() % (uint32_t)(i + 1));
        int16_t t = bag[i]; bag[i] = bag[j]; bag[j] = t;
    }
}
// Build the shuffle decks for a station. `alternate` stations split clips into a speech
// deck + a [music] deck (the player then alternates the two); non-alternating stations
// funnel EVERYTHING into the speech deck = one combined even shuffle. Reshuffled each call.
static void radio_pl_build(uint8_t prefix, bool alternate) {
    int16_t prev = (int16_t)radio_pl_cur;      // don't immediately replay this on re-tune
    radio_bag_sp_n = radio_bag_mu_n = 0;
    radio_bag_sp_pos = radio_bag_mu_pos = 0;
    radio_next_music = false;                  // alternation starts on speech
    radio_pl_cur = -1;
    char pre[6]; snprintf(pre, sizeof(pre), "%u-", (unsigned)prefix);
    size_t plen = strlen(pre);
    for (int i = 0; i < g_radio_clip_count; i++) {
        const RadioClip *c = &g_radio_clips[i];
        if (strncmp(c->name, pre, plen)) continue;
        if (!radio_clip_available(c->name)) continue;
        // littlefs clip (data==NULL): only queue it if the file is actually on the FS --
        // a missing bed just makes the station quieter (or drops alternation), never a reboot.
        if (!c->data && (!c->path || !LittleFS.exists(c->path))) continue;
        bool music = (c->caption && !strcmp(c->caption, "[music]"));
        if (alternate && music) { if (radio_bag_mu_n < 32) radio_bag_mu[radio_bag_mu_n++] = (int16_t)i; }
        else                    { if (radio_bag_sp_n < 32) radio_bag_sp[radio_bag_sp_n++] = (int16_t)i; }
    }
    radio_bag_shuffle(radio_bag_sp, radio_bag_sp_n);
    radio_bag_shuffle(radio_bag_mu, radio_bag_mu_n);
    if (radio_bag_sp_n > 1 && radio_bag_sp[0] == prev) {   // seam-guard the very first pick
        int k = 1 + (int)(esp_random() % (uint32_t)(radio_bag_sp_n - 1));
        int16_t t = radio_bag_sp[0]; radio_bag_sp[0] = radio_bag_sp[k]; radio_bag_sp[k] = t;
    }
    if (radio_bag_mu_n > 1 && radio_bag_mu[0] == prev) {
        int k = 1 + (int)(esp_random() % (uint32_t)(radio_bag_mu_n - 1));
        int16_t t = radio_bag_mu[0]; radio_bag_mu[0] = radio_bag_mu[k]; radio_bag_mu[k] = t;
    }
    radio_pl_n = radio_bag_sp_n + radio_bag_mu_n;
}
// Pull the next clip. Alternate speech<->music when BOTH decks have clips; else draw from
// the single non-empty deck. A deck reshuffles when exhausted (fresh cycle), so every clip
// is heard once per rotation -- even distribution, no clumping.
static void radio_pl_advance(void) {
    if (radio_pl_n <= 0) { radio_pl_cur = -1; return; }
    bool use_music;
    if (radio_bag_sp_n && radio_bag_mu_n) { use_music = radio_next_music; radio_next_music = !radio_next_music; }
    else                                  { use_music = (radio_bag_mu_n > 0); }
    int16_t *bag = use_music ? radio_bag_mu   : radio_bag_sp;
    uint8_t  n   = use_music ? radio_bag_mu_n : radio_bag_sp_n;
    uint8_t *pos = use_music ? &radio_bag_mu_pos : &radio_bag_sp_pos;
    if (*pos >= n) {                           // deck exhausted -> reshuffle a fresh cycle
        radio_bag_shuffle(bag, n);
        if (n > 1 && bag[0] == radio_pl_cur) { // avoid replaying the same clip across the seam
            int k = 1 + (int)(esp_random() % (uint32_t)(n - 1));
            int16_t t = bag[0]; bag[0] = bag[k]; bag[k] = t;
        }
        *pos = 0;
    }
    radio_pl_cur = bag[(*pos)++];
    const RadioClip *c = &g_radio_clips[radio_pl_cur];
    if (c->data) audio_mp3_stream_play_progmem(c->data, c->len, false);  // PROGMEM clip
    else         audio_mp3_stream_play_file(LittleFS, c->path, false);   // littlefs clip (verified to exist)
    // no loop on either -- EOF drives the next pick
    if (radio_transcript_lbl) {   // live-refresh an open Transcript window to the new clip
        const char *cap = c->caption;
        lv_label_set_text(radio_transcript_lbl, (cap && cap[0]) ? cap : "(No transcript for this clip.)");
    }
}
// Caption of the clip currently playing (for the Transcript button).
static const char *radio_current_caption(void) {
    return (radio_pl_cur >= 0 && radio_pl_cur < g_radio_clip_count)
           ? g_radio_clips[radio_pl_cur].caption : nullptr;
}

static void radio_audio_update(void) {
    audio_mp3_stream_stop();
    radio_pl_on = false; radio_pl_cur = -1;
    if (!radio_active) { audio_static_stop(); return; }
    if (radio_drifting) {                 // tuning in: hiss (level set by the drift tick), no bed
        audio_static_start();
        return;
    }
    audio_static_stop();                  // locked/settled: kill the hiss
    if (!radio_station_unlocked(radio_cur)) return;  // gated = weak signal, no audio
    radio_pl_build(radio_stations[radio_cur].clip_prefix, radio_stations[radio_cur].alternate);
    if (radio_pl_n == 0) return;          // no clips wired for this station
    radio_pl_on = true;
    radio_pl_advance();                   // start on a random clip; scope tick advances on EOF
}

// Lock the current station: snap the dial to center, mark it tuned-in for the
// boot, clear drift. DC34-134 will fade the static out + chirp a lock blip here.
static void radio_lock_in(void) {
    if (!radio_drifting) return;
    radio_drifting = false;
    radio_dwell = 0;
    radio_locked_mask |= (1u << radio_cur);
    if (radio_tune_slider) lv_slider_set_value(radio_tune_slider, radio_tune_target, LV_ANIM_ON);
    radio_apply_display();
    radio_audio_update();               // DC34-134: start the station bed on lock
}

// Tune to station i — highlight + (maybe) start the drift minigame. Locked
// gated stations tune to a "weak signal" state (the find is knowing WHAT
// unlocks them); unlocked stations may drift per the DATA▸Settings toggle.
static void radio_select(int i) {
    if (i < 0 || i >= (int)NUM_RADIO_STATIONS) return;
    radio_cur = (int8_t)i;
    radio_tune_target = radio_tune_target_for(i);
    radio_drifting = radio_should_drift(i);
    radio_dwell = 0;
    radio_tune_last = -1;

    if (radio_tune_slider) {
        if (radio_drifting) {
            // Start the dial well off-target so there's a tune-in to perform.
            int start = (radio_tune_target < 50) ? radio_tune_target + 35
                                                 : radio_tune_target - 35;
            if (start < 0) start = 0; else if (start > 100) start = 100;
            lv_slider_set_value(radio_tune_slider, start, LV_ANIM_OFF);
            lv_obj_remove_state(radio_tune_slider, LV_STATE_DISABLED);
        } else {
            radio_locked_mask |= (1u << i);   // no drift needed → already tuned-in
            lv_slider_set_value(radio_tune_slider, radio_tune_target, LV_ANIM_OFF);
        }
    }
    if (radio_list) highlight_list_item(radio_list, (int8_t)i);
    radio_apply_display();
    radio_audio_update();               // DC34-134: (re)start or silence the bed for this station
}

// Release the dial inside the band = lock immediately (forgiveness: no dwell).
static void radio_tune_released_cb(lv_event_t *e) {
    (void)e;
    if (!radio_drifting || !radio_tune_slider) return;
    int v = (int)lv_slider_get_value(radio_tune_slider);
    if (abs(v - radio_tune_target) <= RADIO_TUNE_BAND) radio_lock_in();
}

static void radio_station_tap_cb(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    radio_select(i);
}

// Tapping an undiscovered ("????") station: don't tune -- tell the player how
// many more collectibles they need. Gate is a COUNT, so this is exact + honest,
// and nudges more hunting.
static void radio_locked_tap_cb(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= (int)NUM_RADIO_STATIONS) return;
    int need = (int)radio_stations[i].gate_count - coll_count_found();
    if (need < 1) need = 1;   // it just crossed; treat as 1 (unlock modal will catch up)
    char msg[112];
    snprintf(msg, sizeof(msg),
             "A signal is buried in the static here.\n\nUnlock %d more collectible%s to discover this station.",
             need, need == 1 ? "" : "s");
    coll_msg_modal("UNKNOWN SIGNAL", msg);
}

static void radio_vol_cb(lv_event_t *e) {
    lv_obj_t *sl = (lv_obj_t *)lv_event_get_target(e);
    int32_t val = lv_slider_get_value(sl);
    theremin_volume = (uint8_t)val;
    cfg.volume = (uint8_t)val;
    vol_cfg_mark_dirty();
    audio_set_volume(val / 100.0f);            // actually change the audio (was a no-op)
    audio_theremin_set_volume(val / 100.0f);
}

// Transcript window (accessibility): a scrollable, readable modal showing the
// tuned station's verbatim caption. For a cipher station (numbers), a readable
// static window beats a scrolling ticker — you must copy the hex. Self-contained
// on the active screen; self-deletes on Close (no global/timer => UAF-safe).
static void show_radio_transcript(void) {
    const RadioStation *st = &radio_stations[radio_cur];

    lv_obj_t *modal = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(modal);
    lv_obj_set_size(modal, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_80, 0);
    lv_obj_remove_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(modal, LV_OBJ_FLAG_CLICKABLE);   // absorb edge taps (else they fall through to div/tab -> nav strands the modal)

    lv_obj_t *box = lv_obj_create(modal);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 300, 224);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, pip_bg(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, pip_highlight(), 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_pad_all(box, 8, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 6, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    char tbuf[48];
    snprintf(tbuf, sizeof(tbuf), "%s", st->name);
    lv_obj_t *title = make_label(box, tbuf, &ui_font_pipboy_16, pip_highlight());
    lv_obj_set_width(title, lv_pct(100));
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    // Scrollable transcript area (grows to fill; the label wraps + scrolls).
    lv_obj_t *scroll = lv_obj_create(box);
    lv_obj_remove_style_all(scroll);
    lv_obj_set_width(scroll, lv_pct(100));
    lv_obj_set_flex_grow(scroll, 1);
    lv_obj_set_style_pad_all(scroll, 2, 0);
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
    const char *cap = radio_current_caption();   // the clip playing right now
    lv_obj_t *txt = make_label(scroll,
        (cap && cap[0]) ? cap : "(No transcript playing right now.)",
        &ui_font_pipboy_14, pip_primary());
    lv_label_set_long_mode(txt, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(txt, lv_pct(100));
    // Live-update: radio_pl_advance() rewrites this label when the clip changes.
    // Self-null on delete so the shuffle poll never writes a freed label (UAF).
    radio_transcript_lbl = txt;
    lv_obj_add_event_cb(txt, cb_selfnull_on_delete, LV_EVENT_DELETE, &radio_transcript_lbl);

    lv_obj_t *close = lv_button_create(box);
    lv_obj_remove_style_all(close);
    lv_obj_add_style(close, &style_list_btn, 0);
    lv_obj_add_style(close, &style_list_btn_pressed, LV_STATE_PRESSED);
    lv_obj_set_width(close, lv_pct(100));
    lv_obj_set_height(close, LV_SIZE_CONTENT);
    lv_obj_t *cl = make_label(close, "Close", &ui_font_pipboy_16, pip_primary());
    lv_obj_set_width(cl, lv_pct(100));
    lv_obj_set_style_text_align(cl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_event_cb(close, [](lv_event_t *e) {
        lv_obj_delete((lv_obj_t *)lv_event_get_user_data(e));
    }, LV_EVENT_CLICKED, modal);

    crt_scanlines_raise();
}

static void show_radio(lv_obj_t *cont) {
    clear_children(cont);
    content_teardown();   // SB2: also fixes left_pane/right_pane dangling after this page
    radio_stop();                       // clean slate (idempotent)
    radio_active = true;

    lv_obj_remove_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_set_style_pad_gap(cont, 0, 0);

    // ── Left: station list ────────────────────────────────────────────────
    radio_list = create_left_list(cont);
    for (int i = 0; i < (int)NUM_RADIO_STATIONS; i++) {
        if (radio_station_unlocked(i))
            make_list_btn(radio_list, radio_stations[i].name, radio_station_tap_cb, (void *)(intptr_t)i);
        else
            // Undiscovered stations stay in the list as "????" (a lure to hunt more
            // collectibles); tapping shows how many more are needed, doesn't tune.
            make_list_btn_dim(radio_list, "????", radio_locked_tap_cb, (void *)(intptr_t)i);
    }

    // Opening the radio = the user has now seen the station list, so mark every
    // currently-unlocked station "viewed". The 10-min/2nd-boot reminder then only
    // re-fires when a NEW station later unlocks (see radio_reminder_check()).
    {
        uint8_t seen = 0;
        for (int i = 0; i < (int)NUM_RADIO_STATIONS; i++)
            if (radio_station_unlocked(i)) seen |= (uint8_t)(1u << i);
        if ((cfg.radio_viewed_mask & seen) != seen) {
            cfg.radio_viewed_mask |= seen;
            cfg_save_radio_discovery();
        }
    }

    // ── Right: scope + meta + tuning + volume + Back ──────────────────────
    lv_obj_t *right = lv_obj_create(cont);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, RIGHT_PANE_W, lv_pct(100));
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(right, 4, 0);
    lv_obj_set_style_pad_row(right, 2, 0);
    lv_obj_remove_flag(right, LV_OBJ_FLAG_SCROLLABLE);

    // Oscilloscope box — DC34-132 lv_line trace draws inside.
    lv_obj_t *scope = lv_obj_create(right);
    lv_obj_remove_style_all(scope);
    lv_obj_set_size(scope, lv_pct(100), 52);
    lv_obj_set_style_bg_color(scope, pip_bg(), 0);
    lv_obj_set_style_bg_opa(scope, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(scope, pip_border(), 0);
    lv_obj_set_style_border_width(scope, 1, 0);
    lv_obj_set_style_radius(scope, 2, 0);
    lv_obj_remove_flag(scope, LV_OBJ_FLAG_SCROLLABLE);
    radio_scope_line = lv_line_create(scope);
    lv_obj_set_style_line_width(radio_scope_line, 2, 0);
    lv_obj_set_style_line_color(radio_scope_line, pip_primary(), 0);
    lv_obj_set_style_line_rounded(radio_scope_line, true, 0);
    lv_obj_add_event_cb(radio_scope_line, cb_selfnull_on_delete, LV_EVENT_DELETE, &radio_scope_line);
    radio_scope_phase = 0;
    radio_scope_timer = lv_timer_create(radio_scope_tick, 50, NULL);   // ~20fps

    // Tuned station name + meta line.
    radio_name_lbl = make_label(right, radio_stations[0].name, &ui_font_pipboy_18, pip_highlight());
    lv_label_set_long_mode(radio_name_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(radio_name_lbl, lv_pct(100));
    lv_obj_set_style_text_align(radio_name_lbl, LV_TEXT_ALIGN_CENTER, 0);

    radio_meta_lbl = make_label(right, "", &ui_font_pipboy_14, pip_dim());
    lv_obj_set_width(radio_meta_lbl, lv_pct(100));
    lv_obj_set_style_text_align(radio_meta_lbl, LV_TEXT_ALIGN_CENTER, 0);

    // ── slider-row helper (whole row is the touch target — DC34-133 §forgiveness)
    auto make_slider_row = [&](const char *cap, bool no_fill) -> lv_obj_t* {
        lv_obj_t *row = lv_obj_create(right);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(row, 4, 0);
        lv_obj_set_style_pad_right(row, 9, 0);   // reserve room so the knob doesn't run off the screen edge
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *l = make_label(row, cap, &ui_font_pipboy_14, pip_dim());
        lv_obj_set_style_min_width(l, 34, 0);
        lv_obj_t *sl = lv_slider_create(row);
        lv_obj_set_flex_grow(sl, 1);
        lv_obj_set_height(sl, 10);
        lv_obj_set_style_bg_color(sl, pip_border(), LV_PART_MAIN);
        // no_fill (the tuning dial) = a uniform track, no level fill: make the
        // indicator transparent. Done HERE (external overrides on LV_PART_INDICATOR
        // were being ignored on this slider). Otherwise tint the fill as normal.
        if (no_fill)
            lv_obj_set_style_bg_opa(sl, LV_OPA_TRANSP, LV_PART_INDICATOR);
        else
            lv_obj_set_style_bg_color(sl, pip_highlight(), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(sl, pip_highlight(), LV_PART_KNOB);
        lv_obj_set_style_pad_all(sl, 3, LV_PART_KNOB);
        return sl;
    };

    // Tuning dial (DC34-133). 0..100; drift starts off-band, lock snaps to center.
    // no_fill=true -> the knob rides a uniform slot (a dial, not a level).
    radio_tune_slider = make_slider_row("TUNE", /*no_fill=*/true);
    lv_slider_set_range(radio_tune_slider, 0, 100);
    lv_obj_add_event_cb(radio_tune_slider, radio_tune_released_cb, LV_EVENT_RELEASED, NULL);
    // Move the TUNE row directly under the scope (index 1), above the station name,
    // so it isn't crowded against VOL (which stays down by the buttons).
    lv_obj_move_to_index(lv_obj_get_parent(radio_tune_slider), 1);

    // Volume (drives cfg.volume; DC34-134 also drives audio live).
    radio_vol_slider = make_slider_row("VOL", /*no_fill=*/false);
    lv_slider_set_range(radio_vol_slider, 0, 100);
    lv_slider_set_value(radio_vol_slider, cfg.volume, LV_ANIM_OFF);
    lv_obj_add_event_cb(radio_vol_slider, radio_vol_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Bottom row: [Transcript] [Back]. Transcript opens the verbatim caption
    // (accessibility); Back routes through rebuild_content() (calls radio_stop()).
    lv_obj_t *brow = lv_obj_create(right);
    lv_obj_remove_style_all(brow);
    lv_obj_set_width(brow, lv_pct(100));
    lv_obj_set_height(brow, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(brow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(brow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(brow, 4, 0);
    lv_obj_remove_flag(brow, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tbtn = lv_button_create(brow);
    lv_obj_remove_style_all(tbtn);
    lv_obj_add_style(tbtn, &style_list_btn, 0);
    lv_obj_add_style(tbtn, &style_list_btn_pressed, LV_STATE_PRESSED);
    lv_obj_set_flex_grow(tbtn, 3);
    lv_obj_set_height(tbtn, LV_SIZE_CONTENT);
    lv_obj_t *tl = make_label(tbtn, "Transcript", &ui_font_pipboy_14, pip_primary());
    lv_obj_set_width(tl, lv_pct(100));
    lv_obj_set_style_text_align(tl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_event_cb(tbtn, [](lv_event_t *e) { (void)e; show_radio_transcript(); },
                        LV_EVENT_CLICKED, NULL);

    lv_obj_t *back = lv_button_create(brow);
    lv_obj_remove_style_all(back);
    lv_obj_add_style(back, &style_list_btn, 0);
    lv_obj_add_style(back, &style_list_btn_pressed, LV_STATE_PRESSED);
    lv_obj_set_flex_grow(back, 2);
    lv_obj_set_height(back, LV_SIZE_CONTENT);
    lv_obj_t *bl = make_label(back, "Back", &ui_font_pipboy_14, pip_primary());
    lv_obj_set_style_text_align(bl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(bl, lv_pct(100));
    lv_obj_add_event_cb(back, info_back_cb, LV_EVENT_CLICKED, NULL);

    // Tune to the first UNLOCKED station (early game that's Pirate 0x7C00; the
    // locked ones are "????" and must not auto-select).
    int first_unlocked = -1;
    for (int i = 0; i < (int)NUM_RADIO_STATIONS; i++)
        if (radio_station_unlocked(i)) { first_unlocked = i; break; }
    radio_select(first_unlocked >= 0 ? first_unlocked : 0);
    crt_scanlines_raise();
}

// ─── Radio "signal detected" discovery + unlock nudges (triggers A + B) ───────
// A = a reminder that fires from a boot timer if there's an unlocked-but-unviewed
//     station (2nd boot -> ~4s, else -> 10 min); B = fires ~1.5s after a
//     collectible-found modal when a find crosses a station's collectible gate.
// Both reuse show_new_station_modal (View -> open the radio + tune). "viewed" is
// set when the radio list is opened; "announced" is set once per unlock.
static int g_radio_view_target = -1;

static void radio_view_async(void *p) {
    (void)p;
    int idx = g_radio_view_target; g_radio_view_target = -1;
    goto_div_tab(1, 2);                          // ITEMS > SAOs
    show_radio(content_obj);                     // open the radio
    if (idx >= 0 && radio_station_unlocked(idx)) radio_select(idx);   // tune to the announced one
}
// Deferred (rebuilding the screen inside the modal's button event would UAF).
static void radio_view_cb(void) { lv_async_call(radio_view_async, nullptr); }

// One station just surfaced. credit=true -> the collectible-earned copy (drives
// more hunting); false -> the Pirate 0x7C00 ARG discovery (no collectible credit).
static void radio_announce_one(int idx, bool credit) {
    if (idx < 0 || idx >= (int)NUM_RADIO_STATIONS) return;
    g_radio_view_target = idx;
    show_new_station_modal(radio_stations[idx].name,
        credit ? "Your collection pulled it from the static. Keep hunting for more."
               : "Broadcasting down on the noise floor. Tune in.",
        radio_view_cb, /*allow_optout=*/true);
}
// Several unlocked at once (e.g. a .sav import jumped the found-count past 2+ gates).
static void radio_announce_many(int count) {
    g_radio_view_target = -1;                    // View just opens the radio list
    char sub[112];
    snprintf(sub, sizeof(sub),
             "%d new stations surfaced from your collection. Keep hunting for more.", count);
    show_new_station_modal("Multiple Signals", sub, radio_view_cb, /*allow_optout=*/true);
}

// Trigger B: announce any collectible-gated stations that just crossed their gate
// (once each). Call after the collectible-found modal is dismissed.
// Mark every currently-unlocked collectible-gated station as already-announced, so a
// BULK collectible change (coll add all / SD restore / a save loaded at boot) does
// NOT spuriously "new station!" them on the next real unlock. Only stations that
// cross their gate AFTER this runs will announce. Called from the bulk-load paths.
void radio_sync_announced(void) {
    for (int i = 0; i < (int)NUM_RADIO_STATIONS; i++)
        if (radio_stations[i].gate_count > 0 && radio_station_unlocked(i))
            cfg.radio_announced_mask |= (uint8_t)(1u << i);
    cfg_save_radio_discovery();
}

// Policy version the on-device consent reflects (acceptable_use.md v0.3 -> 3).
// BUMP this whenever the acceptable-use terms change so re-flashed units that
// previously accepted an OLDER policy are re-prompted (DC34-102 #2). Defined here
// (ahead of show_legal_notice) so radio_screen_busy() below can gate on it.
#define LEGAL_POLICY_VERSION 3

// A radio nudge/announce must NEVER pop over a blocking first-boot flow -- the boot
// POST scroll, the acceptable-use notice, or the intro tour -- nor over an active
// scan or an existing station modal. It steals the screen, and its "View" button
// opens the radio (audible tuning static) mid-acceptance. Callers defer while true.
static bool radio_screen_busy(void) {
    return boot_overlay != nullptr                     // boot POST still on screen
        || cfg.legal_ack_ver < LEGAL_POLICY_VERSION    // acceptable-use not yet accepted
        || g_tour_modal != nullptr                     // first-boot intro tour up
        || g_station_modal != nullptr                  // a station modal already showing
        || hr_blackout || hr_scanning;                 // a scan is mid-flight
}

static void radio_check_unlocks(void) {
    if (cfg.radio_reminder_off) return;
    int newly[NUM_RADIO_STATIONS], n = 0;
    for (int i = 0; i < (int)NUM_RADIO_STATIONS; i++) {
        if (radio_stations[i].gate_count == 0) continue;             // Pirate isn't a collectible unlock
        if (radio_station_unlocked(i) && !(cfg.radio_announced_mask & (1u << i)))
            newly[n++] = i;
    }
    if (n == 0) return;
    for (int k = 0; k < n; k++) cfg.radio_announced_mask |= (uint8_t)(1u << newly[k]);
    cfg_save_radio_discovery();
    if (n == 1) radio_announce_one(newly[0], true);
    else        radio_announce_many(n);
}
static void radio_unlock_timer_cb(lv_timer_t *t) {
    lv_timer_delete(t);
    // Don't pop the station modal on top of the scan/found modal (or a running scan,
    // or an existing station modal). Wait until the screen is clear, re-checking each
    // cycle so a modal that appears DURING the wait also defers us. Bounded so a stuck
    // overlay can't poll forever -- radio_reminder_check() re-surfaces it later.
    static uint8_t defer_tries = 0;
    if (radio_screen_busy()) {   // scan/station-modal OR a first-boot flow (boot/legal/tour) -> wait
        if (defer_tries++ < 12) {
            lv_timer_t *rt = lv_timer_create(radio_unlock_timer_cb, 1200, NULL);
            lv_timer_set_repeat_count(rt, 1);
        }
        return;
    }
    defer_tries = 0;
    // Hitting 100% collectibles takes over the moment (the custom-theme reveal);
    // otherwise announce any newly-unlocked radio station.
    if (!cfg.custom_unlock_seen && coll_all_found()) { custom_check_reveal(); return; }
    radio_check_unlocks();
}

// Trigger A: the reminder nudge.
static bool radio_reminder_pending = false;   // fired under the screensaver -> show on wake
static bool radio_has_unviewed_unlocked(void) {
    for (int i = 0; i < (int)NUM_RADIO_STATIONS; i++)
        if (radio_station_unlocked(i) && !(cfg.radio_viewed_mask & (1u << i))) return true;
    return false;
}
static void radio_reminder_fire(lv_timer_t *t) {
    lv_timer_delete(t);
    static uint8_t rf_defer = 0;
    if (cfg.radio_reminder_off || !radio_has_unviewed_unlocked()) { rf_defer = 0; return; }
    if (screensaver_active) { radio_reminder_pending = true; return; }   // wait until dismissed
    if (radio_screen_busy()) {   // boot POST / acceptable-use / tour / scan up -> defer, re-check shortly
        if (rf_defer++ < 30) {   // ~60s of 2s polls, then give up (re-armed next boot)
            lv_timer_t *rt = lv_timer_create(radio_reminder_fire, 2000, NULL);
            lv_timer_set_repeat_count(rt, 1);
        } else rf_defer = 0;
        return;
    }
    rf_defer = 0;
    for (int i = 0; i < (int)NUM_RADIO_STATIONS; i++)
        if (radio_station_unlocked(i) && !(cfg.radio_viewed_mask & (1u << i))) {
            radio_announce_one(i, radio_stations[i].gate_count > 0);
            return;
        }
}
// Called from screensaver_dismiss(): if a reminder was suppressed under the saver,
// show it a moment after the screen comes back (not mid-dismiss).
static void radio_reminder_on_wake(void) {
    if (!radio_reminder_pending) return;
    radio_reminder_pending = false;
    lv_timer_t *t = lv_timer_create(radio_reminder_fire, 800, NULL);   // brief settle after wake
    lv_timer_set_repeat_count(t, 1);
}
// Arm at boot: 2nd+ boot fires the reminder in ~4s; 1st boot waits 10 min (so a
// player has time to explore before being nudged toward the radio).
static void radio_reminder_arm(void) {
    if (cfg.radio_reminder_off || !radio_has_unviewed_unlocked()) return;
    // A web-flash factory reset double-boots (app partition write, then littlefs),
    // so cfg.boot_count is already >= 2 on the user's very FIRST real boot -- using
    // it alone nudges a brand-new user in 4s, on top of the first-boot flows. Treat
    // "hasn't finished the first-boot intro" (clippy intro / acceptable-use) as a
    // first boot so a new user still gets the 10-min explore grace.
    bool first_experience = !cfg.clippy_seen || cfg.legal_ack_ver < LEGAL_POLICY_VERSION;
    uint32_t delay = (cfg.boot_count >= 2 && !first_experience) ? 4000u : 600000u;  // 4s returning / 10min new
    lv_timer_t *t = lv_timer_create(radio_reminder_fire, delay, NULL);
    lv_timer_set_repeat_count(t, 1);
}

// ─── Clippy first-boot intro ──────────────────────────────────────────────

// First-boot Clippy intro: friendly mascot + speech bubble nudging the
// user toward the Help page. Only fires once -- gated by cfg.clippy_seen.
// Called from the legal-notice accept handler so the two modals don't
// fight for the screen on first boot.
static void show_clippy_intro(void) {
    if (cfg.clippy_seen) return;

    lv_obj_t *modal = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(modal);
    lv_obj_set_size(modal, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(modal, pip_bg(), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_COVER, 0);

    // Mascot: 153x192 image, anchored bottom-left. Theme-tinted via recolor.
    lv_obj_t *clippy = lv_image_create(modal);
    lv_image_set_src(clippy, &ClipBoyGS153x192);
    lv_obj_set_style_image_recolor(clippy, pip_primary(), 0);
    lv_obj_set_style_image_recolor_opa(clippy, LV_OPA_COVER, 0);
    // Scale to fit ~140px tall so we leave room for the speech bubble.
    lv_image_set_scale(clippy, 192);  // 192/256 ≈ 0.75x → ~115x144 px
    lv_obj_align(clippy, LV_ALIGN_BOTTOM_LEFT, 4, -4);

    // Speech bubble: bordered box, top-right of screen.
    lv_obj_t *bubble = lv_obj_create(modal);
    lv_obj_remove_style_all(bubble);
    lv_obj_set_size(bubble, 196, 178);
    lv_obj_align(bubble, LV_ALIGN_TOP_RIGHT, -6, 6);
    lv_obj_set_style_bg_color(bubble, pip_bg(), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bubble, 2, 0);
    lv_obj_set_style_border_color(bubble, pip_primary(), 0);
    lv_obj_set_style_radius(bubble, 6, 0);
    lv_obj_set_style_pad_all(bubble, 8, 0);
    lv_obj_set_scroll_dir(bubble, LV_DIR_NONE);

    lv_obj_t *txt = lv_label_create(bubble);
    lv_label_set_long_mode(txt, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(txt, 178);
    // Natural wrap -- the LV_LABEL_LONG_WRAP engine handles line breaks.
    // Re-launch hint at the bottom in dimmer text so users always know
    // how to come back, regardless of which button they pick.
    lv_label_set_text(txt,
        "Hi! I'm Clip-Boy. It looks like "
        "you're trying to use a DEFCON "
        "badge. Want a quick tour?\n\n"
        "(Re-launch from DATA > Settings > Help.)");
    lv_obj_set_style_text_color(txt, pip_primary(), 0);
    lv_obj_set_style_text_font(txt, &ui_font_pipboy_14, 0);
    lv_obj_align(txt, LV_ALIGN_TOP_LEFT, 0, 0);

    // Two buttons at bottom of bubble, with a clear gap between them.
    auto make_clippy_btn = [](lv_obj_t *parent, const char *text,
                              lv_event_cb_t cb) {
        lv_obj_t *b = lv_btn_create(parent);
        lv_obj_set_size(b, 80, 26);
        lv_obj_set_style_bg_color(b, pip_primary(), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, text);
        lv_obj_set_style_text_color(l, lv_color_black(), 0);
        lv_obj_set_style_text_font(l, &ui_font_pipboy_14, 0);
        lv_obj_center(l);
        return b;
    };

    // "Sure!" - dismisses the intro modal and starts the tour at step 0.
    // "Later" - just dismisses. Both persist clippy_seen.
    lv_obj_t *btn_yes = make_clippy_btn(bubble, "Sure!",
        [](lv_event_t *e) {
            cfg.clippy_seen = true;
            cfg_save_clippy_seen();
            lv_obj_t *b = (lv_obj_t *)lv_event_get_target(e);
            lv_obj_delete(lv_obj_get_parent(lv_obj_get_parent(b)));
            show_clippy_tour_step(0);
        });
    lv_obj_align(btn_yes, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t *btn_no = make_clippy_btn(bubble, "Later",
        [](lv_event_t *e) {
            cfg.clippy_seen = true;
            cfg_save_clippy_seen();
            lv_obj_t *b = (lv_obj_t *)lv_event_get_target(e);
            lv_obj_delete(lv_obj_get_parent(lv_obj_get_parent(b)));
        });
    lv_obj_align(btn_no, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    crt_scanlines_raise();  // scanlines on top of intro
}

// LEGAL_POLICY_VERSION is defined earlier (before the radio nudge helpers, which
// gate on cfg.legal_ack_ver < LEGAL_POLICY_VERSION so a nudge can't pop over the
// acceptable-use notice). See its definition above radio_screen_busy().

static lv_obj_t *s_legal_accept_btn = nullptr;
static lv_obj_t *s_legal_hint       = nullptr;
static bool      s_legal_armed      = false;

// Arm the Accept button once the consent body has been scrolled to the end
// (scroll-to-enable, DC34-102 #1). Idempotent.
static void legal_arm_accept(void) {
    if (s_legal_armed || !s_legal_accept_btn) return;
    s_legal_armed = true;
    lv_obj_add_flag(s_legal_accept_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(s_legal_accept_btn, pip_highlight(), 0);
    if (s_legal_hint) lv_obj_add_flag(s_legal_hint, LV_OBJ_FLAG_HIDDEN);
}

static void legal_body_scroll_cb(lv_event_t *e) {
    lv_obj_t *body = (lv_obj_t *)lv_event_get_target(e);
    if (lv_obj_get_scroll_bottom(body) <= 4) legal_arm_accept();  // at the end
}

static void show_legal_notice(void) {
    if (cfg.legal_ack_ver >= LEGAL_POLICY_VERSION) return;  // accepted this policy already

    s_legal_accept_btn = nullptr;
    s_legal_hint       = nullptr;
    s_legal_armed      = false;

    lv_obj_t *modal = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(modal);
    lv_obj_set_size(modal, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(modal, pip_bg(), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(modal, 10, 0);
    lv_obj_set_flex_flow(modal, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(modal, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(modal, 6, 0);
    lv_obj_remove_flag(modal, LV_OBJ_FLAG_SCROLLABLE);  // the BODY scrolls, not the modal

#ifdef CLIPBOY_RES34RCH
    make_label(modal, "ACTIVE RESEARCH BUILD", &ui_font_pipboy_18, pip_highlight());
#else
    make_label(modal, "SECURITY TOOLS NOTICE", &ui_font_pipboy_18, pip_highlight());
#endif

    // Scrollable body: flex-grows to fill between the fixed title and button.
    lv_obj_t *body = lv_obj_create(modal);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(body, 8, 0);
    lv_obj_set_style_pad_right(body, 6, 0);  // room for the scrollbar
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_add_style(body, &style_scrollbar, LV_PART_SCROLLBAR);

#ifdef CLIPBOY_RES34RCH
    lv_obj_t *tldr = make_label(body,
        "TL;DR  This is the Res34rch-Boy build. It can TRANSMIT - deauth, beacon "
        "and probe floods, SAE, BLE spam, evil portal - as well as capture traffic. "
        "Use it ONLY on targets you own or are authorized in writing to test.",
        &ui_font_pipboy_14, pip_highlight());
    lv_obj_t *txt = make_label(body,
        "These tools actively disrupt, inject, and intercept. Running them against "
        "networks, devices, or people you are not authorized to test is illegal in "
        "most places - computer-misuse, wiretap, and RF / spectrum law (for example "
        "47 USC 333).\n\n"
        "You take on full responsibility for everything this build can do.\n\n"
        "Provided as-is; to the maximum extent permitted by law, no warranties are "
        "provided.",
        &ui_font_pipboy_14, pip_primary());
    lv_obj_t *sub = make_label(body,
        "By proceeding, I accept full responsibility for lawful, authorized use "
        "only, and I agree to the Acceptable Use terms (incl. indemnification) in "
        "DATA > Settings > Legal.",
        &ui_font_pipboy_14, pip_dim());
    const char *accept_label = "I Accept - Authorized Targets Only";
#else
    lv_obj_t *tldr = make_label(body,
        "TL;DR  This badge captures WiFi and Bluetooth traffic - including "
        "handshakes and raw packets. That is interception, even though it never "
        "transmits. Use it ONLY on networks and devices you own or are authorized "
        "to test.",
        &ui_font_pipboy_14, pip_highlight());
    lv_obj_t *txt = make_label(body,
        "Clip-Boy (Sn34k-Boy) is listen-only at the radio - it does not transmit. "
        "But capturing handshakes (EAPOL / PMKID) and raw packets can sweep up "
        "credentials and private data, which the law treats as interception (for "
        "example the Wiretap Act). \"Passive\" is not the same as \"harmless.\"\n\n"
        "Unauthorized capture or access may violate the laws where you are.\n\n"
        "Provided as-is; to the maximum extent permitted by law, no warranties are "
        "provided.",
        &ui_font_pipboy_14, pip_primary());
    lv_obj_t *sub = make_label(body,
        "By proceeding, I accept responsibility for lawful use, only on networks "
        "and systems I own or have explicit authorization and consent to test, and "
        "I agree to the Acceptable Use terms (incl. indemnification) in "
        "DATA > Settings > Legal.",
        &ui_font_pipboy_14, pip_dim());
    const char *accept_label = "I Understand & Accept";
#endif
    lv_label_set_long_mode(tldr, LV_LABEL_LONG_WRAP); lv_obj_set_width(tldr, lv_pct(100));
    lv_label_set_long_mode(txt,  LV_LABEL_LONG_WRAP); lv_obj_set_width(txt,  lv_pct(100));
    lv_label_set_long_mode(sub,  LV_LABEL_LONG_WRAP); lv_obj_set_width(sub,  lv_pct(100));

    lv_obj_add_event_cb(body, legal_body_scroll_cb, LV_EVENT_SCROLL, NULL);

    // Hint under the body; hidden once the user reaches the end.
    s_legal_hint = make_label(modal, "Scroll to the end to continue",
                              &ui_font_pipboy_14, pip_dim());

    // Fixed Accept button -- starts DISABLED (dim + non-clickable) until the
    // body is read to the end. No per-SKU exception: scroll-gate is mandatory.
    lv_obj_t *btn = lv_button_create(modal);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_set_height(btn, 34);
    lv_obj_set_style_bg_color(btn, pip_border(), 0);  // dim = disabled
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICKABLE);   // not tappable until armed
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, accept_label);
    lv_obj_set_style_text_font(bl, &ui_font_pipboy_14, 0);
    lv_obj_set_style_text_color(bl, pip_bg(), 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(btn, [](lv_event_t *e) {
        cfg.legal_ack     = true;
        cfg.legal_ack_ver = LEGAL_POLICY_VERSION;
        cfg_save_legal_ack();
        CB_LOGF("[LEGAL] User accepted acceptable-use notice (policy v%d)\n",
                      LEGAL_POLICY_VERSION);
        lv_obj_t *b = (lv_obj_t *)lv_event_get_target(e);
        lv_obj_delete(lv_obj_get_parent(b));
        // Chain into Clippy first-boot intro so the two modals don't race at boot.
        show_clippy_intro();
    }, LV_EVENT_CLICKED, NULL);
    s_legal_accept_btn = btn;

    // If the whole notice fits without scrolling, there's nothing to scroll to --
    // arm immediately so the user isn't stuck.
    lv_obj_update_layout(modal);
    if (lv_obj_get_scroll_bottom(body) <= 4) legal_arm_accept();

    crt_scanlines_raise();  // scanlines on top of legal notice
}
