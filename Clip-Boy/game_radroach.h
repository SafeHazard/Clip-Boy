#pragma once
// game_radroach.h — "Radroach Ronin", a tiny Fruit-Ninja parody for Clip-Boy.
//
// ONE game, TWO limbs:
//   Finger  — you cut roaches by swiping the touchscreen. This is the whole
//             game. A cut lands only if the blade segment crosses a roach AND
//             the finger is moving faster than RADRO_MIN_BLADE_SPEED.
//   Hand    — five cut roaches light the katana. Once it's lit, wave a hand
//             across the VL53L5CX cone and the KATANA WHIRLWIND fires: a flurry
//             of sword strokes that clears the screen and eats rad-barrels
//             harmlessly. The lidar does nothing else -- it is the special-move
//             gesture, not a way to play.
//
// Drop-in: put this file in Clip-Boy/ and include it AFTER ui_nav.h (it reuses
// that file's VL53 globals, claim path and pip theme) and after audio_driver.h.
// See Clip-Boy.ino.
//
// Launch:  radroach_menu_open();   // title screen with the best score
//     or   radroach_open();        // straight into a run
//
// INTEGRATION NOTES (verified against the tree, not guessed):
//   [S1] Sensor claim mirrors theremin_enable(): vl53_sensor, vl53_wire,
//        vl53_begun, vl53_initialized, DEFAULT_I2C_ADDR, cb_stop_operation(),
//        hr_scanning / hr_scan_stop(). The claim happens once at game start,
//        because begin() uploads ~84KB of sensor firmware and blocks for
//        seconds -- doing that mid-run when the meter fills would be brutal.
//   [S2] Exit PARKS the sensor (stopRanging + SLEEP, vl53_begun kept) the way
//        theremin_disable() does. Full release -- theremin_release_sensor(),
//        which also drops vl53_begun and Wire1 -- is the tree's HANDOFF path
//        and belongs to whoever claims next, not to us. Calling it on every
//        exit re-uploads 84KB of sensor firmware per open, which ui_nav.h says
//        is what "exhausted the VL53L5CX under repeated theremin toggling".
//   [A1] Slash SFX: the embedded symbols are scan_ok_mp3 / scan_ok_mp3_len.
//        (The snd_ prefix is only the FILE name, snd_scan_ok_mp3.h.) Swap for
//        a real "slice" clip when you have one.
//   [L1] Line points are v9 lv_point_precise_t (LVGL 9.2.0).
//   [T1] The whirlwind gesture honours the harness sensor mock, so the lidar
//        path is testable off-hardware with two `sensor_mock` frames.
//
// If the sensor never comes up, the game is still completely playable: the
// meter just says the whirlwind is unavailable and you cut with your finger.

#include <lvgl.h>
#include <Preferences.h>

// ─── Tunables ───────────────────────────────────────────────────────────────
#define RADRO_TICK_MS         33      // ~30 fps physics/render
#define RADRO_MAX_ROACH       10      // live sprites cap (well under memory)
#define RADRO_START_LIVES     3
#define RADRO_GRAVITY         0.42f   // px / tick^2  (parabolic toss)
#define RADRO_ROACH_R         18      // hit radius (px)
// Cut gate in px/SECOND, not px/frame. It has to be rate-independent because
// RADRO_INDEV_MS below changes how often a swipe is sampled: at the badge's idle
// 33 ms the old "9 px per sample" meant ~270 px/s, but at 10 ms sampling the
// same constant would silently demand 900 px/s and most cuts would stop landing.
#define RADRO_MIN_BLADE_PX_S  270.0f

// LVGL timing while a run is on screen. The badge idles at LV_DEF_REFR_PERIOD
// (33 ms), which SAMPLES the touch panel at ~30 Hz and redraws at ~30 Hz -- up
// to ~66 ms between fingertip and blade, which is the swipe lag you can feel.
// It is not the CPU: measured on hardware, the main loop turns over ~143 times
// a second during a run (harness `fps`, which counts lv_timer_handler calls), so
// LVGL is idle most of the time. Both are restored to LV_DEF_REFR_PERIOD on
// exit, so nothing outside the game changes.
// Both chosen ON HARDWARE, by playing it. The badge idles at LV_DEF_REFR_PERIOD
// (33 ms), which samples the touch panel at ~30 Hz and redraws at ~30 Hz -- up
// to ~66 ms between fingertip and blade.
//
// Do not "optimise" these by watching the harness `fps` counter. It counts
// lv_timer_handler() calls per second, so it FALLS as these periods shrink --
// not because the badge is struggling but because each turn now does real work
// instead of spinning. Input-to-photon latency is the thing that matters and
// that counter cannot see it. Sweep by feel with `radroach_timing` instead.
//
// Restored to LV_DEF_REFR_PERIOD in radro_exit(), so nothing outside a run is
// affected -- these are LVGL globals, not per-screen settings.
#define RADRO_INDEV_MS        10      // touch sampling during a run (~100 Hz)
#define RADRO_REFR_MS         15      // display refresh during a run (~66 fps)

// Whirlwind gesture (lidar). Not a way to play -- just a hand-wave detector.
#define RADRO_LIDAR_HZ        60
#define RADRO_HAND_MAX_MM     380     // ignore targets farther than this
#define RADRO_SWEEP_VCOL      0.06f   // frac-column/tick that STARTS a wave
#define RADRO_REARM_VCOL      0.02f   // must fall below this to re-arm
#define RADRO_GESTURE_COOL_MS 400     // floor between accepted waves

// Charge-up: N finger kills light the katana.
#define RADRO_CHARGE_KILLS    5
#define RADRO_FLURRY_N        9       // sword strokes in the whirlwind
#define RADRO_FLURRY_STEPS    12      // fade frames
#define RADRO_FLURRY_STEP_MS  38

#define RADRO_NVS_NS          "radroach"   // high score lives here

// ─── State ──────────────────────────────────────────────────────────────────
typedef struct {
    lv_obj_t *obj;      // the sprite (NULL = free slot)
    float     x, y;     // center, px
    float     vx, vy;   // px/tick
    bool      hazard;   // rad-barrel: cutting it ends your run
} radro_roach_t;

static lv_obj_t       *radro_scr        = NULL;   // our screen (lv_obj_create(NULL))
static lv_obj_t       *radro_prev_scr   = NULL;   // restore on exit
static lv_obj_t       *radro_blade      = NULL;   // lv_line for the finger trail
static lv_obj_t       *radro_lbl_score  = NULL;
static lv_obj_t       *radro_lbl_lives  = NULL;
static lv_obj_t       *radro_lbl_chg    = NULL;   // whirlwind charge meter
static lv_obj_t       *radro_btn_exit   = NULL;   // mid-game bail-out
static lv_obj_t       *radro_go_panel   = NULL;   // game-over card
static lv_timer_t     *radro_tick       = NULL;
static lv_timer_t     *radro_lidar_tmr  = NULL;

static radro_roach_t   radro_roach[RADRO_MAX_ROACH];
static int             radro_score       = 0;
static int             radro_lives       = RADRO_START_LIVES;
static bool            radro_over        = false;
static uint32_t        radro_spawn_acc   = 0;     // ms accumulator

// finger blade: the segment between the last two press points
static lv_point_precise_t radro_blade_pts[2];      // [L1]
static bool            radro_have_prev_pt = false;
static float           radro_px = 0, radro_py = 0; // previous touch point
static uint32_t        radro_last_touch_ms = 0;    // for px/s, not px/sample

// lidar centroid tracking + wave edge detection
static float           radro_col     = 0;          // fractional column 0..3
static float           radro_prevcol = 0;
static bool            radro_hand    = false;
static bool            radro_armed   = true;       // wave re-armed (hand slowed)
static uint32_t        radro_last_gesture_ms = 0;

// charge-up
static int             radro_charge   = 0;         // 0..RADRO_CHARGE_KILLS
static bool            radro_charged  = false;     // whirlwind is lit

// flurry VFX
static lv_obj_t          *radro_flurry[RADRO_FLURRY_N]  = {};
static lv_point_precise_t radro_flurry_pts[RADRO_FLURRY_N][2];
static lv_timer_t        *radro_flurry_tmr  = NULL;
static uint8_t            radro_flurry_step = 0;

// Hit SFX, decoded ONCE per run. audio_mp3_play() costs ~233 ms of blocked main
// loop per call on this hardware (30 ms vTaskDelay in audio_mp3_stop, plus a
// full MP3 decode), which is a visible freeze every time you cut something.
// Decoded here at open, replayed through audio_pcm16_play() for microseconds.
static int16_t        *radro_sfx_pcm    = NULL;   // PSRAM, we own it
static size_t          radro_sfx_frames = 0;

// persistence + sensor ownership
static Preferences     radro_prefs;
static uint16_t        radro_hiscore   = 0;
static bool            radro_new_hi    = false;
static bool            radro_died_on_barrel = false;   // how the run ended
static bool            radro_hi_loaded = false;
static bool            radro_claimed   = false;    // WE claimed the VL53
static bool            radro_sensor_ok = false;    // whirlwind is available
// Test-harness only: pauses spawn/physics/miss so a scripted run can assert
// exact counts instead of racing the 1.2 s spawn timer. Never set in normal play.
static bool            radro_frozen    = false;

// forward decls
static void radro_exit(void);
// Teardown deletes radro_scr, and radro_exit() is reached from CLICKED handlers
// ON radro_scr and its children -- freeing an object inside its own event
// callback is the LVGL UAF trap this tree already dodges with lv_async_call
// (see arg_p5_call.h). Defer it.
static void radro_exit_async(void *p) { (void)p; radro_exit(); }

// ─── High score (NVS) ────────────────────────────────────────────────────────
// Own namespace, same open-write-close idiom as arg_core.h's arg_save_u16().
static void radro_hiscore_load(void) {
    if (radro_hi_loaded) return;
    radro_prefs.begin(RADRO_NVS_NS, true);
    radro_hiscore = radro_prefs.getUShort("hi", 0);
    radro_prefs.end();
    radro_hi_loaded = true;
}
static void radro_hiscore_store(uint16_t v) {
    radro_prefs.begin(RADRO_NVS_NS, false);
    radro_prefs.putUShort("hi", v);
    radro_prefs.end();
    radro_hiscore   = v;
    radro_hi_loaded = true;
}

// ─── Sprite (procedural placeholder — zero asset dependency) ─────────────────
// A dark rounded body + highlight border. Swap for lv_image_set_src() + a real
// radroach sprite later; the game core never touches the art.
static lv_obj_t *radro_make_sprite(bool hazard) {
    lv_obj_t *s = lv_obj_create(radro_scr);
    lv_obj_remove_style_all(s);
    // lv_obj_create() sets LV_OBJ_FLAG_CLICKABLE in its constructor
    // (lv_obj.c: obj->flags = LV_OBJ_FLAG_CLICKABLE). Left on, a sprite EATS the
    // press that starts on top of it, so a cut begun over a roach never reaches
    // radro_scr's PRESSING handler and slices nothing.
    lv_obj_clear_flag(s, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s, RADRO_ROACH_R * 2, RADRO_ROACH_R * 2);
    lv_obj_set_style_radius(s, RADRO_ROACH_R, 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s, hazard ? lv_color_hex(0x551111)
                                        : lv_color_hex(0x3a4a20), 0); // radroach olive
    lv_obj_set_style_border_width(s, 2, 0);
    lv_obj_set_style_border_color(s, hazard ? lv_color_hex(0xff4040) : pip_highlight(), 0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s, LV_OBJ_FLAG_IGNORE_LAYOUT);
    // little "eyes"/antennae hint so it reads as a bug, not a coin
    lv_obj_t *a = lv_label_create(s);
    lv_label_set_text(a, hazard ? "!" : "\\../");
    lv_obj_set_style_text_font(a, &ui_font_pipboy_14, 0);
    lv_obj_set_style_text_color(a, hazard ? lv_color_hex(0xff8080) : pip_primary(), 0);
    lv_obj_center(a);
    return s;
}

// force_hazard: -1 = the normal ~12% roll, 0 = always a roach, 1 = always a
// rad-barrel. Only the test harness passes anything but -1 -- a spawn table you
// can't control makes every kill/hazard test a coin flip.
static void radro_spawn(int force_hazard = -1) {
    for (int i = 0; i < RADRO_MAX_ROACH; i++) {
        if (radro_roach[i].obj) continue;
        radro_roach_t *r = &radro_roach[i];
        r->hazard = (force_hazard >= 0) ? (force_hazard != 0)
                                        : (random(0, 100) < 12);   // ~12% rad-barrels
        r->x  = random(40, SCREEN_W - 40);
        r->y  = SCREEN_H + RADRO_ROACH_R;           // launch from below
        r->vx = (random(-25, 25)) / 10.0f;          // -2.5..2.5
        r->vy = -(random(90, 130)) / 10.0f;         // -9.0..-13.0 (up)
        r->obj = radro_make_sprite(r->hazard);
        lv_obj_set_pos(r->obj, (int)r->x - RADRO_ROACH_R, (int)r->y - RADRO_ROACH_R);
        // Sprites are created last but must DRAW first, or they fly over the
        // score, the lives counter, the EXIT chip and the blade trail.
        lv_obj_move_background(r->obj);
        return;
    }
}

// Returns true if this kill SCORED. Callers play ONE SFX per cut, not one per
// roach: audio_mp3_play() pre-empts itself, so N calls in a tick = one beep and
// N wasted decodes.
// barrels_safe: the whirlwind vaporises rad-barrels instead of dying to them.
static bool radro_kill(int i, bool by_player, bool barrels_safe = false) {
    radro_roach_t *r = &radro_roach[i];
    if (!r->obj) return false;
    bool scored = false;
    if (by_player) {
        if (r->hazard) {
            if (!barrels_safe) {
                radro_lives = 0;                  // cut a barrel = run over
                radro_died_on_barrel = true;
                // The miss path below updates this label; this one used to not,
                // so the HUD kept reading "3" while lives was already 0 and the
                // run ended for no visible reason. Owner hit it on hardware.
                if (radro_lbl_lives) lv_label_set_text(radro_lbl_lives, "0");
            }
        } else {
            radro_score++;
            scored = true;
            if (radro_lbl_score) lv_label_set_text_fmt(radro_lbl_score, "%d", radro_score);
        }
    }
    lv_obj_del(r->obj);
    r->obj = NULL;
    return scored;
}

// distance from point P to segment AB, for the blade hit-test
static float radro_pt_seg(float px, float py, float ax, float ay, float bx, float by) {
    float dx = bx - ax, dy = by - ay;
    float len2 = dx * dx + dy * dy;
    float t = len2 > 0.0001f ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0;
    if (t < 0) t = 0; else if (t > 1) t = 1;
    float cx = ax + t * dx, cy = ay + t * dy;
    return sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy));
}

static int radro_live_count(void) {
    int n = 0;
    for (int i = 0; i < RADRO_MAX_ROACH; i++) if (radro_roach[i].obj) n++;
    return n;
}

// ─── Charge meter ────────────────────────────────────────────────────────────
static void radro_charge_hud(void) {
    if (!radro_lbl_chg) return;
    if (!radro_sensor_ok) {
        lv_label_set_text(radro_lbl_chg, "NO SENSOR - NO WHIRLWIND");
        lv_obj_set_style_text_color(radro_lbl_chg, lv_color_hex(0xff6060), 0);
    } else if (radro_charged) {
        lv_label_set_text(radro_lbl_chg, "WHIRLWIND READY - WAVE A HAND");
        lv_obj_set_style_text_color(radro_lbl_chg, pip_highlight(), 0);
    } else {
        lv_label_set_text_fmt(radro_lbl_chg, "KATANA %d/%d",
                              radro_charge, RADRO_CHARGE_KILLS);
        lv_obj_set_style_text_color(radro_lbl_chg, pip_primary(), 0);
    }
}

// ─── Flurry VFX ──────────────────────────────────────────────────────────────
// The whirlwind's sword strokes: full-screen cuts that fade out together.
// Cheap: N lv_line objects + one repeat-counted timer, torn down on the last
// frame.
static void radro_flurry_clear(void) {
    if (radro_flurry_tmr) { lv_timer_del(radro_flurry_tmr); radro_flurry_tmr = NULL; }
    for (int i = 0; i < RADRO_FLURRY_N; i++) {
        if (radro_flurry[i]) { lv_obj_del(radro_flurry[i]); radro_flurry[i] = NULL; }
    }
}

static void radro_flurry_cb(lv_timer_t *t) {
    (void)t;
    radro_flurry_step++;
    if (radro_flurry_step >= RADRO_FLURRY_STEPS) {
        // Last frame: drop the strokes, then let LVGL reap the timer itself (its
        // repeat count is exhausted) rather than freeing a timer mid-callback.
        for (int i = 0; i < RADRO_FLURRY_N; i++) {
            if (radro_flurry[i]) { lv_obj_del(radro_flurry[i]); radro_flurry[i] = NULL; }
        }
        radro_flurry_tmr = NULL;
        return;
    }
    lv_opa_t o = (lv_opa_t)(LV_OPA_COVER -
                            (LV_OPA_COVER * radro_flurry_step) / RADRO_FLURRY_STEPS);
    for (int i = 0; i < RADRO_FLURRY_N; i++) {
        if (radro_flurry[i]) lv_obj_set_style_line_opa(radro_flurry[i], o, 0);
    }
}

static void radro_flurry_fire(void) {
    if (!radro_scr) return;
    radro_flurry_clear();
    radro_flurry_step = 0;
    for (int i = 0; i < RADRO_FLURRY_N; i++) {
        // Alternate steep and shallow strokes so it reads as a flurry of cuts
        // rather than a fan. All coords stay on-screen: lv_line takes its self
        // size from the largest point, and negative points clip.
        if (i & 1) {                                   // top -> bottom
            radro_flurry_pts[i][0].x = random(0, SCREEN_W);
            radro_flurry_pts[i][0].y = 0;
            radro_flurry_pts[i][1].x = random(0, SCREEN_W);
            radro_flurry_pts[i][1].y = SCREEN_H;
        } else {                                       // left -> right
            radro_flurry_pts[i][0].x = 0;
            radro_flurry_pts[i][0].y = random(0, SCREEN_H);
            radro_flurry_pts[i][1].x = SCREEN_W;
            radro_flurry_pts[i][1].y = random(0, SCREEN_H);
        }
        lv_obj_t *l = lv_line_create(radro_scr);
        lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_line_width(l, (i & 1) ? 3 : 5, 0);
        lv_obj_set_style_line_color(l, pip_highlight(), 0);
        lv_obj_set_style_line_rounded(l, true, 0);
        lv_line_set_points(l, radro_flurry_pts[i], 2);
        radro_flurry[i] = l;
    }
    radro_flurry_tmr = lv_timer_create(radro_flurry_cb, RADRO_FLURRY_STEP_MS, NULL);
    lv_timer_set_repeat_count(radro_flurry_tmr, RADRO_FLURRY_STEPS);
}

// ─── The two ways to kill something ──────────────────────────────────────────

// A finger cut along segment A->B. This is the ONLY thing that feeds the charge
// meter -- the whirlwind is the payout, not another earner.
// Returns how many roaches it scored.
static int radro_cut(float ax, float ay, float bx, float by) {
    int scored = 0;
    for (int i = 0; i < RADRO_MAX_ROACH; i++) {
        if (!radro_roach[i].obj) continue;
        if (radro_pt_seg(radro_roach[i].x, radro_roach[i].y, ax, ay, bx, by)
                <= RADRO_ROACH_R && radro_kill(i, true)) scored++;
    }
    if (scored) {
        audio_pcm16_play(radro_sfx_pcm, radro_sfx_frames);     // [A1]
        radro_charge += scored;
        if (radro_charge >= RADRO_CHARGE_KILLS) {
            radro_charge  = RADRO_CHARGE_KILLS;
            radro_charged = true;
        }
        radro_charge_hud();
    }
    return scored;
}

// The katana whirlwind: spend the charge, clear the screen, spare the barrels.
// Returns false (keeping the charge) if it isn't lit, or if there is nothing on
// screen -- waving at thin air must not burn the reward.
static bool radro_whirlwind(void) {
    if (!radro_charged || radro_live_count() == 0) return false;

    radro_charged = false;
    radro_charge  = 0;
    if (radro_blade) lv_obj_add_flag(radro_blade, LV_OBJ_FLAG_HIDDEN);
    radro_flurry_fire();

    int scored = 0;
    for (int i = 0; i < RADRO_MAX_ROACH; i++) {
        if (!radro_roach[i].obj) continue;
        // barrels_safe: the reward never punishes you.
        if (radro_kill(i, true, /*barrels_safe=*/true)) scored++;
    }
    // Whirlwind kills deliberately do NOT feed the next charge, or a crowded
    // screen would re-light it instantly and cascade forever.
    // Same clip as an ordinary cut for now -- the flurry carries the drama. A
    // distinct whirlwind clip would just be a second decode at open. [A1]
    if (scored) audio_pcm16_play(radro_sfx_pcm, radro_sfx_frames);
    radro_charge_hud();
    return true;
}

// ─── Finger input ────────────────────────────────────────────────────────────
// PRESSING fires each frame the finger is down; we keep the last two points as
// the blade segment and test every roach against it.
static void radro_touch_cb(lv_event_t *e) {
    (void)e;
    if (radro_over) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    uint32_t now = millis();
    uint32_t dt  = now - radro_last_touch_ms;
    radro_last_touch_ms = now;

    if (radro_have_prev_pt && dt > 0) {
        float dx = p.x - radro_px, dy = p.y - radro_py;
        // px per SECOND, so the gate means the same thing at any sample rate.
        float speed = sqrtf(dx * dx + dy * dy) * 1000.0f / (float)dt;
        // draw the blade trail
        radro_blade_pts[0].x = (int)radro_px; radro_blade_pts[0].y = (int)radro_py;
        radro_blade_pts[1].x = p.x;           radro_blade_pts[1].y = p.y;
        lv_line_set_points(radro_blade, radro_blade_pts, 2);
        lv_obj_clear_flag(radro_blade, LV_OBJ_FLAG_HIDDEN);

        if (speed >= RADRO_MIN_BLADE_PX_S)
            radro_cut(radro_px, radro_py, p.x, p.y);
    }
    radro_px = p.x; radro_py = p.y; radro_have_prev_pt = true;
}
static void radro_touch_release_cb(lv_event_t *e) {
    (void)e;
    radro_have_prev_pt = false;
    radro_last_touch_ms = 0;
    if (radro_blade) lv_obj_add_flag(radro_blade, LV_OBJ_FLAG_HIDDEN);
}

// ─── Whirlwind gesture (lidar) ───────────────────────────────────────────────
// Reads a 4x4 frame at 60 Hz and tracks the nearest-valid-zone column centroid.
// A wave is EDGE-triggered: it fires once when lateral speed crosses
// RADRO_SWEEP_VCOL, and won't fire again until the hand slows back below
// RADRO_REARM_VCOL. Without that edge the 60 Hz poll would re-trigger on every
// frame of a single wave. The gesture does nothing unless the katana is lit, so
// idle hand-waving never affects a run.
static void radro_lidar_cb(lv_timer_t *t) {
    (void)t;
    if (radro_over) return;

    static VL53L5CX_ResultsData res;                 // ~600B, keep off stack
    bool have = false;
#ifdef TEST_HARNESS
    // [T1] Same mock gate theremin_poll_timer_cb uses, so two `sensor_mock`
    // frames drive the whole gesture path with no sensor attached.
    extern bool test_sensor_is_mocked();
    extern VL53L5CX_ResultsData &test_sensor_get_data();
    if (test_sensor_is_mocked()) { res = test_sensor_get_data(); have = true; }
#endif
    if (!have) {
        if (!vl53_initialized) return;
        if (!vl53_sensor.isDataReady()) return;
        vl53_sensor.getRangingData(&res);
    }

    // 4x4: idx = row*4 + col. Weight each valid, near zone by closeness.
    float wsum = 0, colsum = 0; int nvalid = 0;
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            int idx = row * 4 + col;
            uint8_t st = res.target_status[idx];
            if (st != 5 && st != 9) continue;
            int16_t d = res.distance_mm[idx];
            if (d <= 0 || d > RADRO_HAND_MAX_MM) continue;
            float w = (float)(RADRO_HAND_MAX_MM - d);   // closer = heavier
            colsum += w * col; wsum += w; nvalid++;
        }
    }
    radro_prevcol = radro_col;
    if (nvalid >= 2) {
        radro_col  = colsum / wsum;                     // 0..3 fractional
        float vcol = fabsf(radro_col - radro_prevcol);
        bool was_hand = radro_hand; radro_hand = true;
        uint32_t now = millis();

        if (vcol < RADRO_REARM_VCOL) radro_armed = true;   // hand settled

        if (was_hand && radro_armed && vcol >= RADRO_SWEEP_VCOL &&
            (now - radro_last_gesture_ms) >= RADRO_GESTURE_COOL_MS) {
            radro_armed = false;
            // Only spend the cooldown on a wave that actually did something, so
            // a wasted wave (nothing on screen) can be retried immediately.
            if (radro_whirlwind()) radro_last_gesture_ms = now;
        }
    } else {
        radro_hand  = false;
        radro_armed = true;                              // hand left the cone
    }
}

// Play-surface button: a plain lv_obj, deliberately NOT lv_btn.
//
// ui_install_global_click_sound() (ui_nav.h) registers its hook on the INPUT
// DEVICE, so it fires on every screen ever loaded and plays a tap sound for
// anything of lv_button_class. On the play surface that click is a problem
// twice over:
//   * it routes through audio_mp3_play() -> audio_mp3_stop(), leaving a
//     driver-owned clip in the single player slot, so the NEXT kill has to
//     retire it and eats a 30 ms vTaskDelay right when the game should feel
//     tight;
//   * it is the exact caller that would free our borrowed SFX buffer if the
//     ownership flag in audio_driver.h were ever weakened.
// Plain lv_obj is not button-class, so the hook skips it -- the same trick the
// collectibles rows use (see ui_nav.h "Rows are plain lv_obj"). We give up
// lv_button's built-in pressed styling, so it is spelled out below.
//
// The TITLE screen keeps real lv_btn on purpose: it is a menu like any other on
// the badge and should click, and radroach_open() retires that clip anyway.
//
// If you convert these back to lv_btn_create, radroach_state's "btns" count
// goes above zero and radroach_test.py fails on purpose.
static lv_obj_t *radro_flat_btn(lv_obj_t *parent, int w, int h, lv_event_cb_t cb) {
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, pip_bg(), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 3, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, pip_highlight(), 0);
    lv_obj_set_style_bg_color(b, pip_highlight(), LV_STATE_PRESSED);   // tap feedback
    lv_obj_set_style_bg_opa(b, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

// ─── Game over ───────────────────────────────────────────────────────────────
// Start a fresh run WITHOUT tearing the screen down. Deliberately not
// radro_exit() + radroach_open(): that would park and re-claim the sensor and
// re-decode the hit SFX (~200 ms) for no reason. The screen, the sensor claim
// and the decoded clip are all still perfectly good.
static void radro_restart(void) {
    if (!radro_scr) return;
    radro_flurry_clear();
    if (radro_go_panel) { lv_obj_del(radro_go_panel); radro_go_panel = NULL; }
    for (int i = 0; i < RADRO_MAX_ROACH; i++) {
        if (radro_roach[i].obj) { lv_obj_del(radro_roach[i].obj); radro_roach[i].obj = NULL; }
    }
    radro_score = 0; radro_lives = RADRO_START_LIVES; radro_over = false;
    radro_spawn_acc = 0; radro_have_prev_pt = false; radro_last_touch_ms = 0;
    radro_charge = 0; radro_charged = false; radro_new_hi = false;
    radro_died_on_barrel = false;
    radro_hand = false; radro_col = radro_prevcol = 0;
    radro_armed = true; radro_last_gesture_ms = 0;
    if (radro_lbl_score) lv_label_set_text(radro_lbl_score, "0");
    if (radro_lbl_lives) lv_label_set_text_fmt(radro_lbl_lives, "%d", radro_lives);
    radro_charge_hud();
}
// Both buttons live ON the card that these tear down, so defer past the event.
static void radro_restart_async(void *p) { (void)p; radro_restart(); }

static lv_obj_t *radro_go_btn(lv_obj_t *parent, const char *txt, lv_align_t al,
                              int x, int y, int w, lv_event_cb_t cb) {
    lv_obj_t *b = radro_flat_btn(parent, w, 32, cb);
    lv_obj_align(b, al, x, y);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &ui_font_pipboy_14, 0);
    lv_obj_set_style_text_color(l, pip_highlight(), 0);
    lv_obj_center(l);
    return b;
}

// A real card with real buttons, replacing the old "tap anywhere to exit".
// That version dumped you straight back to the SAO list: the tick added a
// CLICKED handler to the whole screen the instant your last life went, so
// LIFTING THE FINGER that was still mid-swipe fired it immediately and the
// score was gone before it rendered.
static void radro_show_game_over(void) {
    radro_go_panel = lv_obj_create(radro_scr);
    lv_obj_remove_style_all(radro_go_panel);
    lv_obj_set_size(radro_go_panel, 250, 168);
    lv_obj_center(radro_go_panel);
    lv_obj_set_style_bg_color(radro_go_panel, pip_bg(), 0);
    lv_obj_set_style_bg_opa(radro_go_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(radro_go_panel, 2, 0);
    lv_obj_set_style_border_color(radro_go_panel, pip_highlight(), 0);
    lv_obj_set_style_radius(radro_go_panel, 4, 0);
    lv_obj_clear_flag(radro_go_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(radro_go_panel, LV_OBJ_FLAG_CLICKABLE);   // swallow stray taps

    lv_obj_t *t = lv_label_create(radro_go_panel);
    lv_label_set_text(t, "GAME OVER");
    lv_obj_set_style_text_font(t, &ui_font_pipboy_20, 0);
    lv_obj_set_style_text_color(t, pip_highlight(), 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 8);

    // Why the run ended. Without this a barrel death is indistinguishable from
    // running out of lives, which reads as the game quitting on you.
    lv_obj_t *why = lv_label_create(radro_go_panel);
    if (radro_died_on_barrel) {
        lv_label_set_text(why, "SLICED A RAD-BARREL");
        lv_obj_set_style_text_color(why, lv_color_hex(0xff6060), 0);
    } else {
        lv_label_set_text(why, "OUT OF LIVES");
        lv_obj_set_style_text_color(why, pip_primary(), 0);
    }
    lv_obj_set_style_text_font(why, &ui_font_pipboy_14, 0);
    lv_obj_align(why, LV_ALIGN_TOP_MID, 0, 34);

    lv_obj_t *sc = lv_label_create(radro_go_panel);
    lv_label_set_text_fmt(sc, "SCORE  %d", radro_score);
    lv_obj_set_style_text_font(sc, &ui_font_pipboy_18, 0);
    lv_obj_set_style_text_color(sc, pip_primary(), 0);
    lv_obj_align(sc, LV_ALIGN_TOP_MID, 0, 54);

    lv_obj_t *bs = lv_label_create(radro_go_panel);
    if (radro_new_hi) {
        lv_label_set_text_fmt(bs, "NEW BEST!  %u", (unsigned)radro_hiscore);
        lv_obj_set_style_text_color(bs, pip_highlight(), 0);
    } else {
        lv_label_set_text_fmt(bs, "BEST  %u", (unsigned)radro_hiscore);
        lv_obj_set_style_text_color(bs, pip_primary(), 0);
    }
    lv_obj_set_style_text_font(bs, &ui_font_pipboy_14, 0);
    lv_obj_align(bs, LV_ALIGN_TOP_MID, 0, 80);

    radro_go_btn(radro_go_panel, "PLAY AGAIN", LV_ALIGN_BOTTOM_LEFT, 8, -10, 132,
        [](lv_event_t *e){ (void)e; lv_async_call(radro_restart_async, NULL); });
    radro_go_btn(radro_go_panel, "EXIT", LV_ALIGN_BOTTOM_RIGHT, -8, -10, 92,
        [](lv_event_t *e){ (void)e; lv_async_call(radro_exit_async, NULL); });
}

// ─── Game tick: physics, spawn, miss, render ─────────────────────────────────
// One frame of the world. allow_spawn is false only for the harness's
// radroach_step, which advances physics without the spawn timer adding sprites
// a script did not ask for.
static void radro_step(bool allow_spawn) {
    if (radro_over) return;

    if (allow_spawn) {
        radro_spawn_acc += RADRO_TICK_MS;
        // SIGNED on purpose. This was uint32_t, and at score 61 the subtraction
        // 1200 - 1220 wrapped to ~4.29e9 instead of going negative. The clamp
        // below was written to catch exactly that and cannot, because the wrap
        // already happened -- so radro_spawn_acc could never reach it and the
        // game simply stopped producing roaches, forever, mid-run. Found by
        // playing: "it just stopped generating roaches ... 63 points".
        int32_t spawn_every = 1200 - (int32_t)radro_score * 20;     // ramps up
        if (spawn_every < 350) spawn_every = 350;                   // difficulty floor
        if (radro_spawn_acc >= (uint32_t)spawn_every) {
            radro_spawn_acc = 0;
            radro_spawn();
        }
    }

    for (int i = 0; i < RADRO_MAX_ROACH; i++) {
        radro_roach_t *r = &radro_roach[i];
        if (!r->obj) continue;
        r->vy += RADRO_GRAVITY;
        r->x  += r->vx;
        r->y  += r->vy;
        // fell off the bottom while descending = a miss (unless a harmless barrel)
        if (r->y - RADRO_ROACH_R > SCREEN_H && r->vy > 0) {
            if (!r->hazard) {
                radro_lives--;
                if (radro_lbl_lives) lv_label_set_text_fmt(radro_lbl_lives, "%d", radro_lives);
            }
            lv_obj_del(r->obj); r->obj = NULL;
            continue;
        }
        lv_obj_set_pos(r->obj, (int)r->x - RADRO_ROACH_R, (int)r->y - RADRO_ROACH_R);
    }

    if (radro_lives <= 0) {
        radro_over = true;
        radro_hiscore_load();
        radro_new_hi = ((uint16_t)radro_score > radro_hiscore);
        if (radro_new_hi) radro_hiscore_store((uint16_t)radro_score);

        radro_show_game_over();
    }
}

static void radro_tick_cb(lv_timer_t *t) {
    (void)t;
    if (radro_over) return;

    // A player mid-wave generates NO touch events, so the idle screensaver
    // (screensaver_check_cb in ui_nav.h) would activate during a run and yank
    // the screen away. Its busy-list is all tool/radio flags we can't set from
    // here, so poke the same inactivity counter it reads instead.
    lv_display_trigger_activity(NULL);

    if (radro_frozen) return;          // harness hold; screen stays live
    radro_step(true);
}

// ─── Lifecycle ───────────────────────────────────────────────────────────────
// Mirror of the theremin's sensor claim ([S1]) but 4x4 @ 60 Hz for a gesture.
static bool radro_sensor_claim(void) {
    cb_stop_operation();
    if (hr_scanning) hr_scan_stop();               // release VL53 from scanner
    if (!vl53_begun) {
        vl53_wire.begin(CB_VL53_SDA, CB_VL53_SCL, CB_VL53_I2C_HZ);
        delay(50);
        bool found = false;
        for (int a = 0; a < 3 && !found; a++) {
            if (a) delay(200);
            found = vl53_sensor.begin(DEFAULT_I2C_ADDR >> 1, vl53_wire);
        }
        if (!found) { vl53_wire.end(); return false; }
        vl53_begun = true;
    } else if (!vl53_initialized) {
        vl53_sensor.setPowerMode(SF_VL53L5CX_POWER_MODE::WAKEUP);
    }
    if (!vl53_initialized) {
        vl53_sensor.setResolution(VL53L5CX_RESOLUTION_4X4);
        vl53_sensor.setRangingFrequency(RADRO_LIDAR_HZ);
        vl53_sensor.startRanging();
        vl53_initialized = true;
    }
    return true;
}

// Small EXIT chip, top-centre. The game owns the whole screen (no tab bar), so
// without it the only way out is losing three lives.
// Safe alongside the finger blade: LVGL delivers PRESSING to whatever was under
// the FIRST touch, so a cut dragged across the chip never presses it, and a cut
// that starts on it is a deliberate tap.
static lv_obj_t *radro_make_exit_btn(void) {
    lv_obj_t *b = radro_flat_btn(radro_scr, 56, 22,
        [](lv_event_t *e){ (void)e; lv_async_call(radro_exit_async, NULL); });
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, pip_border(), 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, "EXIT");
    lv_obj_set_style_text_font(l, &ui_font_pipboy_14, 0);
    lv_obj_set_style_text_color(l, pip_border(), 0);
    lv_obj_center(l);
    return b;
}

// Retune LVGL's input-sampling and screen-refresh timers. Called with the fast
// game values on open and with LV_DEF_REFR_PERIOD on exit, so the change is
// scoped to a run and the rest of the badge keeps its power-friendly 33 ms.
static void radro_set_lv_timing(uint32_t indev_ms, uint32_t refr_ms) {
    for (lv_indev_t *ind = lv_indev_get_next(NULL); ind; ind = lv_indev_get_next(ind)) {
        if (lv_indev_get_type(ind) != LV_INDEV_TYPE_POINTER) continue;
        lv_timer_t *rt = lv_indev_get_read_timer(ind);
        if (rt) lv_timer_set_period(rt, indev_ms);
    }
    lv_timer_t *rft = lv_display_get_refr_timer(NULL);   // NULL = default display
    if (rft) lv_timer_set_period(rft, refr_ms);
}

void radroach_open(void) {
    radro_score = 0; radro_lives = RADRO_START_LIVES; radro_over = false;
    radro_spawn_acc = 0; radro_have_prev_pt = false;
    radro_hand = false; radro_col = radro_prevcol = 0;
    radro_armed = true; radro_last_gesture_ms = 0; radro_last_touch_ms = 0;
    radro_charge = 0; radro_charged = false; radro_new_hi = false;
    radro_died_on_barrel = false;
    radro_frozen = false; radro_sensor_ok = false; radro_go_panel = NULL;
    radro_flurry_tmr = NULL; radro_flurry_step = 0;
    for (int i = 0; i < RADRO_FLURRY_N; i++) radro_flurry[i] = NULL;
    for (int i = 0; i < RADRO_MAX_ROACH; i++) radro_roach[i].obj = NULL;
    radro_hiscore_load();

    // One decode for the whole run. Costs ~200 ms here instead of ~233 ms on
    // every single kill. A NULL result just means a silent game, never a crash:
    // audio_pcm16_play() no-ops on a null buffer.
    if (!radro_sfx_pcm)
        radro_sfx_pcm = aud_mp3_decode(scan_ok_mp3, scan_ok_mp3_len, &radro_sfx_frames);
    // Retire any driver-owned clip still installed (the UI tap that launched us).
    // audio_pcm16_play() would otherwise have to do it on the FIRST kill, and
    // audio_mp3_stop() costs a 30 ms vTaskDelay -- measured as a lone 31 ms
    // spike on shot #1 while every later shot ran ~0.9 ms. Pay it here, where
    // the sensor claim is already blocking, so no shot in the run is special.
    audio_mp3_stop();

    radro_prev_scr = lv_screen_active();
    radro_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(radro_scr, pip_bg(), 0);
    lv_obj_set_style_bg_opa(radro_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(radro_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_screen_load(radro_scr);

    radro_set_lv_timing(RADRO_INDEV_MS, RADRO_REFR_MS);

    // finger blade trail (hidden until the first swipe)
    radro_blade = lv_line_create(radro_scr);
    lv_obj_clear_flag(radro_blade, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_line_width(radro_blade, 4, 0);
    lv_obj_set_style_line_color(radro_blade, pip_highlight(), 0);
    lv_obj_set_style_line_rounded(radro_blade, true, 0);
    lv_obj_add_flag(radro_blade, LV_OBJ_FLAG_HIDDEN);

    // HUD
    radro_lbl_score = lv_label_create(radro_scr);
    lv_label_set_text(radro_lbl_score, "0");
    lv_obj_set_style_text_color(radro_lbl_score, pip_highlight(), 0);
    lv_obj_set_style_text_font(radro_lbl_score, &ui_font_pipboy_20, 0);
    lv_obj_align(radro_lbl_score, LV_ALIGN_TOP_LEFT, 8, 6);

    radro_lbl_lives = lv_label_create(radro_scr);
    lv_label_set_text_fmt(radro_lbl_lives, "%d", radro_lives);
    lv_obj_set_style_text_color(radro_lbl_lives, lv_color_hex(0xff6060), 0);
    lv_obj_set_style_text_font(radro_lbl_lives, &ui_font_pipboy_16, 0);
    lv_obj_align(radro_lbl_lives, LV_ALIGN_TOP_RIGHT, -8, 8);

    radro_lbl_chg = lv_label_create(radro_scr);
    lv_obj_set_style_text_font(radro_lbl_chg, &ui_font_pipboy_14, 0);
    lv_obj_align(radro_lbl_chg, LV_ALIGN_BOTTOM_LEFT, 8, -6);

    radro_btn_exit = radro_make_exit_btn();

    // The finger IS the game, so touch is always live.
    lv_obj_add_flag(radro_scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(radro_scr, radro_touch_cb,         LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(radro_scr, radro_touch_release_cb, LV_EVENT_RELEASED, NULL);

#ifdef TEST_HARNESS
    extern bool test_sensor_is_mocked();
    if (test_sensor_is_mocked()) {
        // Mocked frames come from the harness, not the bus -- don't claim, and
        // don't park on exit either (radro_claimed stays false).
        radro_sensor_ok = true;
        radro_charge_hud();
        radro_lidar_tmr = lv_timer_create(radro_lidar_cb, 1000 / RADRO_LIDAR_HZ, NULL);
        radro_tick = lv_timer_create(radro_tick_cb, RADRO_TICK_MS, NULL);
        return;
    }
#endif

    // The first claim of a session uploads ~84KB of sensor firmware and blocks
    // for seconds. Paint a word about it first, or the badge just looks hung.
    lv_label_set_text(radro_lbl_chg, "WAKING SENSOR...");
    lv_obj_set_style_text_color(radro_lbl_chg, pip_primary(), 0);
    lv_refr_now(NULL);

    if (radro_sensor_claim()) {
        radro_claimed   = true;
        radro_sensor_ok = true;
        radro_lidar_tmr = lv_timer_create(radro_lidar_cb, 1000 / RADRO_LIDAR_HZ, NULL);
    }
    // A dead sensor just means no whirlwind. The finger game is untouched.
    radro_charge_hud();

    radro_tick = lv_timer_create(radro_tick_cb, RADRO_TICK_MS, NULL);
}

static void radro_exit(void) {
    radro_set_lv_timing(LV_DEF_REFR_PERIOD, LV_DEF_REFR_PERIOD);
    // Stop BEFORE freeing: the audio task may still be reading our buffer, and
    // audio_mp3_stop() both clears the pointer and waits out any in-flight
    // write. It will not free it -- the borrow flag says the buffer is ours.
    if (radro_sfx_pcm) {
        audio_mp3_stop();
        free(radro_sfx_pcm);
        radro_sfx_pcm    = NULL;
        radro_sfx_frames = 0;
    }
    radro_flurry_clear();
    if (radro_tick)      { lv_timer_del(radro_tick);      radro_tick = NULL; }
    if (radro_lidar_tmr) { lv_timer_del(radro_lidar_tmr); radro_lidar_tmr = NULL; }
    // [S2] Park the VL53L5CX: stop ranging and drop it to SLEEP, exactly what
    // theremin_disable() does when the user is done with it. See the [S2] note
    // at the top for why this is NOT theremin_release_sensor().
    if (radro_claimed) {
        if (vl53_initialized) {
            vl53_sensor.stopRanging();
            vl53_sensor.setPowerMode(SF_VL53L5CX_POWER_MODE::SLEEP);
            vl53_initialized = false;
        }
        radro_claimed = false;
    }
    radro_sensor_ok = false;
    if (radro_prev_scr) lv_screen_load(radro_prev_scr);
    if (radro_scr)      { lv_obj_del(radro_scr); radro_scr = NULL; }
    // Deleting the screen frees every sprite with it, so these pointers are now
    // dangling. radroach_open() clears them on the way in, but anything that
    // reads the array while the game is CLOSED -- radro_live_count() in the
    // harness state dump did exactly this, reporting "open:false, roaches:6" --
    // is looking at freed memory. Null them here, at the point they die.
    for (int i = 0; i < RADRO_MAX_ROACH; i++) radro_roach[i].obj = NULL;
    radro_blade = radro_lbl_score = radro_lbl_lives = NULL;
    radro_lbl_chg = radro_btn_exit = radro_go_panel = NULL;
}

// ─── Title screen ────────────────────────────────────────────────────────────
// Self-contained. Hooked from ITEMS > SAOs > "Radroach Ronin" (see GAME_SAO_IDX
// in ui_nav.h), but callable from anywhere.
static lv_obj_t *radro_menu_scr  = NULL;
static lv_obj_t *radro_menu_prev = NULL;

static lv_obj_t *radro_menu_btn(lv_obj_t *parent, const char *txt, int y,
                                lv_event_cb_t cb) {
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, 220, 40);
    lv_obj_align(b, LV_ALIGN_CENTER, 0, y);
    lv_obj_set_style_bg_color(b, pip_bg(), 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, pip_highlight(), 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, pip_highlight(), 0);
    lv_obj_set_style_text_font(l, &ui_font_pipboy_16, 0);
    lv_obj_center(l);
    return b;
}

static void radro_menu_close(void) {
    if (radro_menu_prev) lv_screen_load(radro_menu_prev);
    // Async delete: every caller runs inside a CLICKED handler on a BUTTON that
    // lives on radro_menu_scr, so a direct del would free the tree the event is
    // still walking. (Same trap as radro_exit_async.)
    if (radro_menu_scr)  { lv_obj_delete_async(radro_menu_scr); radro_menu_scr = NULL; }
}

void radroach_menu_open(void) {
    radro_hiscore_load();
    radro_menu_prev = lv_screen_active();
    radro_menu_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(radro_menu_scr, pip_bg(), 0);
    lv_obj_set_style_bg_opa(radro_menu_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(radro_menu_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_screen_load(radro_menu_scr);

    lv_obj_t *title = lv_label_create(radro_menu_scr);
    lv_label_set_text(title, "RADROACH RONIN");
    lv_obj_set_style_text_color(title, pip_highlight(), 0);
    lv_obj_set_style_text_font(title, &ui_font_pipboy_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *how = lv_label_create(radro_menu_scr);
    lv_label_set_text_fmt(how,
        "Swipe to cut. %d cuts light the katana.\n"
        "Then wave a hand at the sensor\nfor the WHIRLWIND.",
        RADRO_CHARGE_KILLS);
    lv_obj_set_style_text_align(how, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(how, pip_primary(), 0);
    lv_obj_set_style_text_font(how, &ui_font_pipboy_14, 0);
    lv_obj_align(how, LV_ALIGN_TOP_MID, 0, 34);

    lv_obj_t *best = lv_label_create(radro_menu_scr);
    lv_label_set_text_fmt(best, "BEST %u", (unsigned)radro_hiscore);
    lv_obj_set_style_text_color(best, pip_highlight(), 0);
    lv_obj_set_style_text_font(best, &ui_font_pipboy_16, 0);
    lv_obj_align(best, LV_ALIGN_TOP_MID, 0, 88);

    radro_menu_btn(radro_menu_scr, "PLAY", 46,
        [](lv_event_t *e){ (void)e; radro_menu_close(); radroach_open(); });
    radro_menu_btn(radro_menu_scr, "Back", 92,
        [](lv_event_t *e){ (void)e; radro_menu_close(); });
}
