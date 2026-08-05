#pragma once
// neopixel_driver.h — NeoPixel driver for Clip-Boy badge
//
// 8 physical WS2812B LEDs: 4 fuse (0-3) + 4 front (4-7).
// Fuse layout: 0=BL, 1=BR, 2=TR, 3=TL. Fiber pairs: 0↔3 (left), 1↔2 (right).
//
// Animations: 0=None (static), 1=Breathe, 2=Chase
// Chase: colors crossfade around a ring. Fuse warp-core order: 0→3→2→1→0
//        (BL→TL→TR→BR, flowing through both fiber pairs).
//
// Included from ui_test.ino AFTER ui_config.h and clipboy_pins.h.

#include <Adafruit_NeoPixel.h>
#include "clipboy_pins.h"

static Adafruit_NeoPixel neo_strip(CB_NEOPIXEL_COUNT, CB_NEOPIXEL_PIN,
                                    NEO_GRB + NEO_KHZ800);

// Per-LED breathe phase
static float neo_breathe_phase[CB_NEOPIXEL_COUNT] = {};

// Chase state
static float neo_chase_phase = 0.0f;

// ─── Rubber Duck theme ─────────────────────────────────────────────────────
// All LEDs ducky-yellow except an orange "head" + a black LED trailing it,
// chasing along front 4-1 then fuse 4,3,2,1, then repeating. A global effect
// (not expressible per-LED), so it takes over the render when active.
static bool    neo_rubber_duck_active = false;
static float   neo_rd_pos = 0.0f;               // continuous head position (crossfade, not hop)
// front 4,3,2,1 = LED idx 7,6,5,4 ; fuse 4,3,2,1 = idx 3,2,1,0
static const uint8_t neo_rd_path[CB_NEOPIXEL_COUNT] = {7, 6, 5, 4, 3, 2, 1, 0};
#define NEO_RD_STEP_MS  190                     // pace: ms per step

// Dirty flag — skip show() when nothing changed
static bool neo_dirty = false;

// Flashlight mode: when true, LEDs 4-7 (the 4 front-facing) are forced
// to full white, overriding whatever animation/color they're configured for.
// RAM-only state, resets to off on boot. Toggled via status-bar button.
static volatile bool neo_flashlight_active = false;

static inline void neo_flashlight_set(bool on) { neo_flashlight_active = on; }
static inline bool neo_flashlight_is_on(void)  { return neo_flashlight_active; }
static inline void neo_flashlight_toggle(void) { neo_flashlight_active = !neo_flashlight_active; }

// ─── Scan sweep (KITT / Cylon "Larson scanner") ────────────────────────────
// While a collectible scan runs, the 4 FRONT LEDs (idx 4-7) drop to black and a
// red (255,0,0) "eye" sweeps 4→7→4, ping-pong, with a short decay TRAIL (the LED
// the eye just left fades toward black over ~1 LED rather than snapping off).
// Front-4 override only — the fuse LEDs (0-3) keep their normal animation.
// Rendered from the core-0 task (the ONLY core that may touch neo_strip, same as
// the flashlight); the caller gates it on the front LEDs actually being lit.
static volatile bool neo_scan_sweep_active = false;
static uint8_t neo_scan_red[4] = {0, 0, 0, 0};   // decay buffer: red level per front LED
static float   neo_scan_pos    = 0.0f;           // eye position 0..3 across the front 4
static int8_t  neo_scan_dir    = 1;              // +1 = 4→7, -1 = 7→4
static volatile int8_t neo_scan_sweep_force = -1; // A/B override (serial led_sweep): -1 auto, 0 off, 1 on
#define NEO_SCAN_SPEED_LPS    3.0f               // eye speed (LEDs/sec; crosses the 4 in ~1s). Slowed from
                                                 // 6.0: the front LEDs share the sensor's face + reflect off
                                                 // the held-up tag into the SPADs, and a fast-moving bright
                                                 // point modulates the per-zone ambient faster than the
                                                 // sensor can track -> slower lock. A stately KITT is gentler.
#define NEO_SCAN_TRAIL_TAU_MS 130.0f             // trail decay time-constant (longer, to suit the slower eye)
#define NEO_SCAN_PEAK         64                 // peak emitted red (0-255) cap. Keeps the eye visibly red but
                                                 // reflects far less light into the ToF FoV than the old full
                                                 // 180. (User brightness still lowers it further; never raises.)

static inline void neo_scan_sweep_set(bool on) {
    if (on && !neo_scan_sweep_active) {          // rising edge: reset the eye to the left end
        neo_scan_pos = 0.0f; neo_scan_dir = 1;
        neo_scan_red[0] = neo_scan_red[1] = neo_scan_red[2] = neo_scan_red[3] = 0;
    }
    neo_scan_sweep_active = on;
}

// Effective render gate. The force flag (serial `led_sweep`) overrides the auto
// activation for A/B testing; -1 = normal gated behavior (the shipped default).
static inline bool neo_scan_sweep_render(void) {
    return (neo_scan_sweep_force == 1) ? true
         : (neo_scan_sweep_force == 0) ? false
         : neo_scan_sweep_active;
}

// Screensaver suppression: when true, the animation tick stops touching the
// strip so neo_off() actually sticks. Flashlight overrides this (rescue path
// in case someone hits the flashlight while the screensaver is up).
static volatile bool neo_suspend_for_ss = false;
static bool neo_sleep_off_done = false;  // core-0 task offs the strip ONCE on suspend

// True if any of the 4 FRONT LEDs (idx 4-7) is currently lit (non-zero color at
// non-zero brightness) and not screensaver-suppressed — i.e. the scan sweep has
// something to override. The caller (hr_scan_start) uses this to honor "only if
// the LEDs are active when a scan kicks off". (Defined after neo_suspend_for_ss.)
static inline bool neo_front_leds_active(void) {
    if (neo_suspend_for_ss) return false;        // screensaver / dark-charge: LEDs off
    for (int i = 4; i < CB_NEOPIXEL_COUNT; i++) {
        LedConfig *lc = &cfg.leds[i];
        if (lc->brightness > 0 && (lc->r || lc->g || lc->b)) return true;
    }
    return false;
}

// ─── show() rate tracking (test harness) ──────────────────────────────────
static volatile uint32_t neo_show_count    = 0;  // Total show() calls
static volatile uint32_t neo_show_window   = 0;  // Calls in current 1s window
static volatile uint32_t neo_show_rate     = 0;  // Calls/sec (updated each second)
static volatile uint32_t neo_show_last_sec = 0;  // Timestamp of last rate calc

static inline void neo_show_tracked(void) {
    neo_strip.show();
    neo_show_count++;
    neo_show_window++;
    uint32_t now = millis();
    if (now - neo_show_last_sec >= 1000) {
        neo_show_rate = neo_show_window;
        neo_show_window = 0;
        neo_show_last_sec = now;
    }
}

// Previous pixel values for dirty detection (static LEDs)
static uint32_t neo_prev[CB_NEOPIXEL_COUNT] = {};

// Chase ring order: fuse warp-core (BL→TL→TR→BR) then front sequential
static const uint8_t CHASE_ORDER[] = {0, 3, 2, 1, 4, 5, 6, 7};

// ─── Speed → period mapping ────────────────────────────────────────────────
// Speed 1 = 10000ms (slow), Speed 10 = 2000ms (fast)
static inline float neo_speed_to_period(uint8_t speed) {
    if (speed < 1) speed = 1;
    if (speed > 10) speed = 10;
    return 10000.0f - (speed - 1) * (8000.0f / 9.0f);
}

// ─── Init ──────────────────────────────────────────────────────────────────

static void neo_init(void) {
    neo_strip.begin();
    neo_strip.setBrightness(255);  // We handle brightness per-LED via scaling
    neo_show_tracked();
    memset(neo_breathe_phase, 0, sizeof(neo_breathe_phase));
    neo_chase_phase = 0.0f;
    neo_dirty = false;
    memset(neo_prev, 0, sizeof(neo_prev));
    CB_LOGLN("[OK] NeoPixels initialized");
}

// ─── Apply single LED from config (immediate, no show) ─────────────────────

static void neo_apply(int idx) {
    if (idx < 0 || idx >= CB_NEOPIXEL_COUNT) return;
    LedConfig *lc = &cfg.leds[idx];
    float bf = lc->brightness / 255.0f;
    uint8_t r = (uint8_t)(lc->r * bf);
    uint8_t g = (uint8_t)(lc->g * bf);
    uint8_t b = (uint8_t)(lc->b * bf);
    neo_strip.setPixelColor(idx, r, g, b);
    neo_dirty = true;
}

// ─── Apply all LEDs from config (immediate, no show) ────────────────────────

static void neo_apply_all(void) {
    for (int i = 0; i < CB_NEOPIXEL_COUNT; i++) {
        LedConfig *lc = &cfg.leds[i];
        float bf = lc->brightness / 255.0f;
        neo_strip.setPixelColor(i,
            (uint8_t)(lc->r * bf),
            (uint8_t)(lc->g * bf),
            (uint8_t)(lc->b * bf));
    }
    neo_dirty = true;
}

// ─── All off (sleep mode) ──────────────────────────────────────────────────

static void neo_off(void) {
    for (int i = 0; i < CB_NEOPIXEL_COUNT; i++)
        neo_strip.setPixelColor(i, 0, 0, 0);
    neo_show_tracked();
    neo_dirty = false;
    memset(neo_prev, 0, sizeof(neo_prev));
}

// ─── Helper: pack RGB for dirty comparison ─────────────────────────────────

static inline uint32_t neo_pack(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

// ─── Helper: set pixel with dirty tracking ─────────────────────────────────

static inline void neo_set(uint8_t idx, uint8_t r, uint8_t g, uint8_t b) {
    uint32_t packed = neo_pack(r, g, b);
    if (neo_prev[idx] != packed) {
        neo_strip.setPixelColor(idx, r, g, b);
        neo_prev[idx] = packed;
        neo_dirty = true;
    }
}

// ─── Scan sweep render (front 4 LEDs) ──────────────────────────────────────
// One frame of the KITT/Cylon eye. Advances the eye (ping-pong 0..3), decays the
// trail time-based (framerate-independent), lights the head full red. Writes via
// neo_set so it dirty-tracks + flushes with the rest of the frame. dt = ms since
// the last tick. Front-4 override only; called after the base render (see tick).
static void neo_render_scan_sweep(uint32_t dt) {
    float step = NEO_SCAN_SPEED_LPS * (float)dt / 1000.0f;
    neo_scan_pos += neo_scan_dir * step;
    if (neo_scan_pos >= 3.0f)      { neo_scan_pos = 3.0f; neo_scan_dir = -1; }
    else if (neo_scan_pos <= 0.0f) { neo_scan_pos = 0.0f; neo_scan_dir =  1; }

    float keep = expf(-(float)dt / NEO_SCAN_TRAIL_TAU_MS);   // fraction of trail retained this tick
    // CROSSFADE the eye between the two straddling LEDs (the next one FADES IN as
    // the current fades out) so motion is smooth instead of snapping LED-to-LED.
    int   lo = (int)neo_scan_pos;                            // 0..3
    if (lo > 3) lo = 3;
    int   hi = (lo < 3) ? lo + 1 : 3;
    float f  = neo_scan_pos - (float)lo;                     // 0..1 toward hi
    for (int i = 0; i < 4; i++) {
        float eye = (i == lo) ? (1.0f - f) : (i == hi && hi != lo) ? f : 0.0f;  // 0..1 eye weight
        float v   = (float)neo_scan_red[i] * keep;           // decaying trail
        if (eye * 255.0f > v) v = eye * 255.0f;              // eye overrides where brighter
        neo_scan_red[i] = (uint8_t)(v + 0.5f);               // logical 0..255 (decay buffer)
        // Emit the red capped at NEO_SCAN_PEAK (and never above the user's own
        // brightness). The front LEDs reflect off the tag into the nearby 940nm
        // ToF sensor, so a bright eye adds ambient onto the SPADs and slows the
        // lock; capping the peak keeps the KITT flair with negligible scan cost.
        uint8_t cap = cfg.leds[4 + i].brightness < NEO_SCAN_PEAK ? cfg.leds[4 + i].brightness : NEO_SCAN_PEAK;
        uint8_t out = (uint8_t)((float)neo_scan_red[i] * ((float)cap / 255.0f) + 0.5f);
        neo_set((uint8_t)(4 + i), out, 0, 0);   // dim red eye + trail, on black
    }
}

// ─── Animation tick — call from loop() ─────────────────────────────────────
//
// Rate-limited to ~50Hz. Handles static, breathe, and chase animations.
// Only calls show() when pixel data actually changed.

static void neo_animation_tick(void) {
    uint32_t now = millis();
    static uint32_t neo_tick_last = 0;
    uint32_t tick_dt = now - neo_tick_last;
    if (tick_dt < 20) return;  // ~50Hz
    neo_tick_last = now;

    // Sleep / screensaver: turn the strip OFF here (from core 0, the only core
    // that touches neo_strip) ONCE, then leave it dark. Doing the off from the
    // task -- not from core 1 via neo_off() -- avoids a cross-core show() race
    // where a core-1 off and a concurrent core-0 repaint fight and the LEDs
    // freeze ON. Flashlight overrides (rescue path).
    if (neo_suspend_for_ss && !neo_flashlight_active) {
        if (!neo_sleep_off_done) {
            for (int i = 0; i < CB_NEOPIXEL_COUNT; i++)
                neo_strip.setPixelColor(i, 0, 0, 0);
            neo_show_tracked();
            memset(neo_prev, 0, sizeof(neo_prev));
            neo_dirty = false;
            neo_sleep_off_done = true;
        }
        return;
    }
    neo_sleep_off_done = false;

    // ─── Rubber Duck theme: takes over the whole strip ──────────────────
    if (neo_rubber_duck_active) {
        // Continuous head position so the orange head + black trail CROSSFADE
        // between LEDs instead of hopping. Advances one slot per NEO_RD_STEP_MS.
        neo_rd_pos += (float)tick_dt / (float)NEO_RD_STEP_MS;
        while (neo_rd_pos >= CB_NEOPIXEL_COUNT) neo_rd_pos -= CB_NEOPIXEL_COUNT;
        int   lo  = (int)neo_rd_pos;                              // current slot
        float f   = neo_rd_pos - (float)lo;                       // 0..1 into the step
        int   hi  = (lo + 1) % CB_NEOPIXEL_COUNT;                 // orange fades lo -> hi
        int   prv = (lo + CB_NEOPIXEL_COUNT - 1) % CB_NEOPIXEL_COUNT;  // black fades prv -> lo
        // Per slot: ducky-yellow base, with orange + black overlaid by crossfade
        // weight (at slot lo the orange hands off to black; weights sum to 1).
        for (int j = 0; j < CB_NEOPIXEL_COUNT; j++) {
            float ow = (j == lo) ? (1.0f - f) : (j == hi) ? f : 0.0f;   // orange weight
            float bw = (j == prv) ? (1.0f - f) : (j == lo) ? f : 0.0f;  // black weight
            float yw = 1.0f - ow - bw;                                  // yellow remainder
            neo_set(neo_rd_path[j],
                    (uint8_t)(200 * yw + 215 * ow),
                    (uint8_t)(165 * yw +  60 * ow),
                    0);   // black contributes nothing to RGB
        }
        // Scan sweep overrides the front 4 (fuse LEDs keep the duck chase); the
        // flashlight rescue below still wins over it.
        if (neo_scan_sweep_render()) neo_render_scan_sweep(tick_dt);
        // Flashlight rescue still wins over the theme
        if (neo_flashlight_active)
            for (int i = 4; i < CB_NEOPIXEL_COUNT; i++) neo_set(i, 255, 255, 255);
        neo_show_tracked();
        return;
    }

    bool has_chase = false;

    // ─── Pass 1: Static and Breathe LEDs ────────────────────────────────
    for (int i = 0; i < CB_NEOPIXEL_COUNT; i++) {
        LedConfig *lc = &cfg.leds[i];

        if (lc->animation == 0) {
            // Static — set color, dirty-track for skip optimization
            float bf = lc->brightness / 255.0f;
            neo_set(i,
                (uint8_t)(lc->r * bf),
                (uint8_t)(lc->g * bf),
                (uint8_t)(lc->b * bf));
            continue;
        }

        if (lc->animation == 2) {
            has_chase = true;
            continue;  // handled in pass 2
        }

        // Breathe (animation == 1)
        float period_ms = neo_speed_to_period(lc->speed);
        float delta = (tick_dt / period_ms) * 6.28318530f;
        neo_breathe_phase[i] += delta;
        if (neo_breathe_phase[i] > 6.28318530f)
            neo_breathe_phase[i] -= 6.28318530f;

        float scale = 0.5f + 0.5f * sinf(neo_breathe_phase[i]);
        float bf = (lc->brightness / 255.0f) * scale;
        neo_set(i,
            (uint8_t)(lc->r * bf),
            (uint8_t)(lc->g * bf),
            (uint8_t)(lc->b * bf));
    }

    // ─── Pass 2: Chase animation ────────────────────────────────────────
    if (has_chase) {
        // Collect chase-enabled LEDs in canonical warp-core order
        uint8_t ring[CB_NEOPIXEL_COUNT];
        uint8_t ring_count = 0;
        uint8_t min_speed = 10;

        for (int i = 0; i < CB_NEOPIXEL_COUNT; i++) {
            uint8_t pi = CHASE_ORDER[i];
            if (cfg.leds[pi].animation == 2) {
                ring[ring_count++] = pi;
                if (cfg.leds[pi].speed < min_speed)
                    min_speed = cfg.leds[pi].speed;
            }
        }

        if (ring_count >= 2) {
            float cn = (float)ring_count;
            float period_ms = neo_speed_to_period(min_speed);

            // Advance shared chase phase
            neo_chase_phase += (tick_dt / period_ms) * cn;
            neo_chase_phase = fmodf(neo_chase_phase, cn);

            // Interpolate each LED's color from the rotating palette
            for (uint8_t c = 0; c < ring_count; c++) {
                uint8_t pi = ring[c];
                LedConfig *lc = &cfg.leds[pi];

                float pos = fmodf((float)c + neo_chase_phase, cn);
                int idx_a = (int)pos;
                if (idx_a >= ring_count) idx_a = 0;  // safety clamp
                int idx_b = (idx_a + 1) % ring_count;
                float frac = pos - (float)idx_a;

                LedConfig *ca = &cfg.leds[ring[idx_a]];
                LedConfig *cb = &cfg.leds[ring[idx_b]];

                float r = ca->r + (cb->r - ca->r) * frac;
                float g = ca->g + (cb->g - ca->g) * frac;
                float b = ca->b + (cb->b - ca->b) * frac;

                float bf = lc->brightness / 255.0f;
                neo_set(pi,
                    (uint8_t)(r * bf),
                    (uint8_t)(g * bf),
                    (uint8_t)(b * bf));
            }
        } else if (ring_count == 1) {
            // Single chase LED — show static color
            uint8_t pi = ring[0];
            LedConfig *lc = &cfg.leds[pi];
            float bf = lc->brightness / 255.0f;
            neo_set(pi,
                (uint8_t)(lc->r * bf),
                (uint8_t)(lc->g * bf),
                (uint8_t)(lc->b * bf));
        }
    }

    // ─── Scan sweep override: KITT/Cylon red eye on the front 4 during a scan ──
    // Runs after normal animation (overwrites the front-4 configured colors);
    // the flashlight below still wins over it. Fuse LEDs (0-3) are untouched.
    if (neo_scan_sweep_render()) {
        neo_render_scan_sweep(tick_dt);
    }

    // ─── Flashlight override: force front 4 LEDs to full white ──────────
    // Runs after normal animation so whatever the LEDs would have shown is
    // simply overwritten. Dirty-tracked via neo_set so repeated frames are
    // free once the pixels settle at white.
    if (neo_flashlight_active) {
        for (int i = 4; i < CB_NEOPIXEL_COUNT; i++) {
            neo_set(i, 255, 255, 255);
        }
    }

    // ─── Flush to hardware only when pixels changed ─────────────────────
    if (neo_dirty) {
        neo_show_tracked();
        neo_dirty = false;
    }
}

// ─── Core 0 animation task ───────────────────────────────────────────────
// Moves LED animation processing off core 1 (LVGL) to core 0 so NeoPixel
// updates don't compete with UI rendering for CPU time.

static TaskHandle_t neo_task_handle = NULL;

static void neo_animation_task(void *param) {
    (void)param;
    for (;;) {
        neo_animation_tick();
        vTaskDelay(pdMS_TO_TICKS(20));  // ~50Hz, matches tick rate limiter
    }
}

static void neo_start_core0_task(void) {
    if (neo_task_handle) return;  // Already running
    xTaskCreatePinnedToCore(
        neo_animation_task,
        "neo_anim",
        2048,         // Stack (animation is lightweight)
        NULL,
        1,            // Low priority — below audio (core 0) and LVGL (core 1)
        &neo_task_handle,
        0             // Core 0
    );
    CB_LOGLN("[OK] NeoPixel animation moved to core 0");
}
