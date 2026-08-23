#pragma once
// test_harness.h — Automated test harness for Clip-Boy badge
//
// Conditionally compiled via -DTEST_HARNESS build flag.
// Provides serial protocol for programmatic UI testing:
//   - Touch injection (secondary LVGL input device)
//   - Screen capture via lv_snapshot
//   - UI state queries (division, tab, widget tree, visible text)
//   - Audio buffer capture (lock-free ring buffer)
//   - VL53L5CX sensor mock injection
//   - Navigation commands
//   - Boot screen dismissal
//
// Protocol: STX (\x02) prefix + JSON command + newline
// Response: STX prefix + JSON response + newline [+ optional binary payload]

#ifdef TEST_HARNESS

#include <lvgl.h>
#include <esp_heap_caps.h>

// ─── Version ──────────────────────────────────────────────────────────────
#define TH_VERSION "1.1.0"
#define TH_STX     '\x02'

// ─── FPS counter ──────────────────────────────────────────────────────────
// Tracks lv_timer_handler() call rate — measures actual UI frame delivery.
// Call th_fps_tick() right after each lv_timer_handler() in loop().

static volatile uint32_t th_fps_count    = 0;  // Frames in current window
static volatile uint32_t th_fps_value    = 0;  // Last computed FPS
static volatile uint32_t th_fps_min      = 999; // Minimum FPS seen since reset
static volatile uint32_t th_fps_max      = 0;  // Maximum FPS seen since reset
static volatile uint32_t th_fps_last_sec = 0;  // Timestamp of last calc
static volatile uint32_t th_fps_samples  = 0;  // Number of 1s samples
static volatile uint64_t th_fps_sum      = 0;  // Sum of all FPS samples (for avg)

static inline void th_fps_tick(void) {
    th_fps_count++;
    uint32_t now = millis();
    if (now - th_fps_last_sec >= 1000) {
        th_fps_value = th_fps_count;
        th_fps_count = 0;
        th_fps_last_sec = now;
        if (th_fps_value < th_fps_min) th_fps_min = th_fps_value;
        if (th_fps_value > th_fps_max) th_fps_max = th_fps_value;
        th_fps_sum += th_fps_value;
        th_fps_samples++;
    }
}

static inline void th_fps_reset(void) {
    th_fps_min = 999;
    th_fps_max = 0;
    th_fps_sum = 0;
    th_fps_samples = 0;
}

// ui_nav.h statics are already visible since this file is included after it.
// No extern declarations needed — cur_div, cur_tab, cur_sel, div_labels,
// tab_labels, screensaver_active, hr_scanning, boot_overlay, boot_timer,
// boot_label, scr_main, rebuild_content(), rebuild_tabs(),
// update_div_indicators() are all static in ui_nav.h and in scope here.

// ─── Touch injection ──────────────────────────────────────────────────────

static lv_indev_t  *th_indev        = NULL;
static volatile int16_t th_touch_x  = 0;
static volatile int16_t th_touch_y  = 0;
static volatile bool th_touch_pressed = false;

static void th_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    data->point.x = th_touch_x;
    data->point.y = th_touch_y;
    data->state   = th_touch_pressed ? LV_INDEV_STATE_PRESSED
                                     : LV_INDEV_STATE_RELEASED;
}

// ─── Audio ring buffer (lock-free, SPSC) ──────────────────────────────────

#define TH_AUDIO_RING_SIZE  8192  // stereo frames (32KB)
#define TH_AUDIO_RING_MASK  (TH_AUDIO_RING_SIZE - 1)

static int16_t *th_audio_ring = NULL;  // [TH_AUDIO_RING_SIZE * 2] in PSRAM
static volatile uint32_t th_audio_wr  = 0;  // write index (frames)
static volatile uint32_t th_audio_rd  = 0;  // read index (frames)

// Called from audio task (core 0) — must be lock-free
void test_audio_capture(const int16_t *samples, size_t stereo_frames) {
    if (!th_audio_ring) return;
    for (size_t i = 0; i < stereo_frames; i++) {
        uint32_t wi = (th_audio_wr + i) & TH_AUDIO_RING_MASK;
        th_audio_ring[wi * 2]     = samples[i * 2];
        th_audio_ring[wi * 2 + 1] = samples[i * 2 + 1];
    }
    th_audio_wr = (th_audio_wr + stereo_frames) & TH_AUDIO_RING_MASK;
}

// ─── Sensor mock ──────────────────────────────────────────────────────────

static bool th_sensor_mock = false;
// VL53L5CX_ResultsData is defined via SparkFun includes in ui_nav.h
static VL53L5CX_ResultsData th_sensor_data;

bool test_sensor_is_mocked() { return th_sensor_mock; }
VL53L5CX_ResultsData &test_sensor_get_data() { return th_sensor_data; }

// ─── JSON helpers (minimal, no ArduinoJson dependency) ────────────────────

static void th_send_ok(const char *cmd) {
    Serial.printf("%c{\"ok\":true,\"cmd\":\"%s\"}\n", TH_STX, cmd);
}

static void th_send_ok_json(const char *cmd, const char *extra_json) {
    Serial.printf("%c{\"ok\":true,\"cmd\":\"%s\",%s}\n", TH_STX, cmd, extra_json);
}

static void th_send_err(const char *cmd, const char *msg) {
    Serial.printf("%c{\"ok\":false,\"cmd\":\"%s\",\"error\":\"%s\"}\n",
                  TH_STX, cmd, msg);
}

// ─── Widget tree walker ───────────────────────────────────────────────────

static void th_walk_tree(lv_obj_t *obj, int depth) {
    if (!obj) return;
    int32_t x = lv_obj_get_x(obj);
    int32_t y = lv_obj_get_y(obj);
    int32_t w = lv_obj_get_width(obj);
    int32_t h = lv_obj_get_height(obj);
    bool hidden = lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);

    // Identify type
    const char *type = "obj";
    if (lv_obj_check_type(obj, &lv_label_class))    type = "label";
    else if (lv_obj_check_type(obj, &lv_button_class))   type = "button";
    else if (lv_obj_check_type(obj, &lv_bar_class))      type = "bar";
    else if (lv_obj_check_type(obj, &lv_slider_class))   type = "slider";
    else if (lv_obj_check_type(obj, &lv_switch_class))   type = "switch";
    else if (lv_obj_check_type(obj, &lv_dropdown_class)) type = "dropdown";
    else if (lv_obj_check_type(obj, &lv_list_class))     type = "list";
    else if (lv_obj_check_type(obj, &lv_image_class))    type = "image";
    else if (lv_obj_check_type(obj, &lv_checkbox_class)) type = "checkbox";
    else if (lv_obj_check_type(obj, &lv_textarea_class)) type = "textarea";

    // Get text for labels
    const char *text = "";
    if (lv_obj_check_type(obj, &lv_label_class)) {
        text = lv_label_get_text(obj);
        if (!text) text = "";
    }

    // Print JSON line (one per widget, streamed)
    Serial.printf("{\"d\":%d,\"type\":\"%s\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,"
                  "\"hidden\":%s,\"text\":\"",
                  depth, type, (int)x, (int)y, (int)w, (int)h,
                  hidden ? "true" : "false");
    // Escape text for JSON safety
    if (text[0]) {
        for (const char *p = text; *p; p++) {
            if (*p == '"') Serial.print("\\\"");
            else if (*p == '\\') Serial.print("\\\\");
            else if (*p == '\n') Serial.print("\\n");
            else if (*p == '\r') Serial.print("\\r");
            else if (*p == '\t') Serial.print("\\t");
            else if ((uint8_t)*p < 0x20) Serial.printf("\\u%04x", (unsigned)*p);
            else Serial.write(*p);
        }
    }
    Serial.println("\"}");

    // Recurse into children
    uint32_t cnt = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < cnt; i++) {
        th_walk_tree(lv_obj_get_child(obj, i), depth + 1);
    }
}


// ─── Command handlers ─────────────────────────────────────────────────────

static void th_cmd_ping() {
    char buf[128];
    snprintf(buf, sizeof(buf),
             "\"version\":\"%s\",\"uptime\":%lu,\"heap\":%lu,\"psram\":%lu",
             TH_VERSION,
             (unsigned long)millis(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    th_send_ok_json("ping", buf);
}

static void th_cmd_heap() {
    char buf[128];
    snprintf(buf, sizeof(buf),
             "\"dram\":%lu,\"psram\":%lu,\"min_dram\":%lu",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    th_send_ok_json("heap", buf);
}

#include <esp_wifi.h>   // esp_wifi_get_ps() -- direct read of the power-save flag

// Read the live WiFi power-save mode. Returns -1 if the driver is not up or the call fails --
// distinguishable from 0 (NONE), so "could not read" never masquerades as "sleep is off".
static int th_wifi_ps_now() {
    wifi_ps_type_t ps;
    if (esp_wifi_get_ps(&ps) != ESP_OK) return -1;
    return (int)ps;
}

#if defined(CB_WIFI_PS_EXPERIMENT) && CB_WIFI_PS_EXPERIMENT
extern "C" esp_err_t cb_experiment_force_wifi_ps(int type);
// EXPERIMENT ONLY. `wifi_ps_set <0|1|2>` bypasses the --wrap that normally no-ops power-save
// changes, so modem sleep can be toggled at RUNTIME on one badge. That makes a WITHIN-BADGE
// crossover possible (ON/OFF/ON epochs, same position, same stimulus), which is strictly better
// than a between-badge comparison -- the confound that wasted two measurement rounds tonight.
// Echoes the DRIVER's value after the call, not the value asked for: a setter that reports its
// own argument proves nothing.
static void th_cmd_wifi_ps_set(const String &args) {
    int want = args.toInt();
    esp_err_t e = cb_experiment_force_wifi_ps(want);
    char b[96];
    snprintf(b, sizeof(b), "\"asked\":%d,\"err\":%d,\"now\":%d", want, (int)e, th_wifi_ps_now());
    th_send_ok_json("wifi_ps_set", b);
}
#endif

static void th_cmd_state() {
    // uptime_ms + reset_reason added 2026-07-25: without them a crash-and-auto-reboot is
    // INVISIBLE to a test. A liveness ping passes (the badge finished rebooting), and the
    // post-boot default screen just looks like "the UI did something odd" -- which is
    // exactly how a use-after-free reboot got misread as "the Help page did not render".
    // uptime going BACKWARDS between two reads is the reliable crash signal; reset_reason
    // then says whether it was a panic (ESP_RST_PANIC=4 / INT_WDT=5 / TASK_WDT=6) or an
    // orderly restart (ESP_RST_SW=3).
    // buf sized with headroom: snprintf TRUNCATES silently, and a clipped tail yields
    // invalid JSON, which surfaces as an unrelated parse failure in whatever test is
    // running rather than as "the state command overflowed". Grow this when adding fields.
    char buf[512];
    snprintf(buf, sizeof(buf),
             "\"div\":%u,\"tab\":%u,\"sel\":%d,"
             "\"div_name\":\"%s\",\"tab_name\":\"%s\","
             "\"screensaver\":%s,\"hr_scanning\":%s,"
             "\"boot_visible\":%s,"
             "\"theremin_active\":%s,\"theremin_want\":%s,"
             "\"flashlight\":%s,"
             "\"geiger_audio\":%s,\"coll_fs_audio\":%s,"
             // serial_seen: has a USB host actually DRAINED our CDC TX this boot? This is the
             // read-back control for the H1 fix. false => TX timeout is 0 => the fix is in
             // force. Without it, "this badge got faster" cannot be told apart from "this
             // badge was never reflashed" -- a stale flash has produced confident results
             // about phantom firmware on this project before.
             // wifi_ps: the ACTUAL WiFi power-save mode, read from the driver via
             // esp_wifi_get_ps() -- 0=NONE 1=MIN_MODEM 2=MAX_MODEM, -1 = call failed.
             // Direct read, not an inference. The question it settles: the Arduino core defaults
             // _sleepEnabled to WIFI_PS_MIN_MODEM on everything except the S2
             // (WiFiGeneric.cpp:761-765) and applies it on ARDUINO_EVENT_WIFI_STA_START (:1041),
             // which our WiFi.mode(WIFI_STA) triggers BEFORE cb.begin() sets _cb_wifi_hw_up --
             // after which __wrap_esp_wifi_set_ps (ClipBoyMarauder.cpp:63) no-ops every further
             // attempt, so nothing can turn it off for the rest of the boot. If this reads 1,
             // every passive tool has been listening with the radio duty-cycled, which would cap
             // sensitivity independently of the CDC-stall fix. Three inferential probes have given
             // confident WRONG answers on this project; this is the flag itself.
             // wifi_ps = the DRIVER (esp_wifi_get_ps); wifi_ps_member = Arduino's
             // WiFiGenericClass::_sleepEnabled. If they DISAGREE, something re-applied
             // power-save at the driver level after Arduino set it -- which is the
             // discriminator for why setSleep(false) before cb.begin() did not stick.
             "\"wifi_ps\":%d,\"wifi_ps_member\":%d,"
             "\"serial_seen\":%s,\"serial_seen_ms\":%lu,"
             "\"uptime_ms\":%lu,\"reset_reason\":%d",
             cur_div, cur_tab, cur_sel,
             div_labels[cur_div], tab_labels[cur_div][cur_tab],
             screensaver_active ? "true" : "false",
             hr_scanning ? "true" : "false",
             (boot_overlay != NULL) ? "true" : "false",
             aud_theremin_active ? "true" : "false",
             // SB3: `active` alone cannot distinguish "the synth was KILLED by a tap click"
             // from "it is briefly DUCKED under a ~100ms click and will resume". `want` is
             // the user-level intent (Enable/Disable), so want=true + active=false outside a
             // clip is the SB3 signature. Without both, a test samples one flag and guesses.
             audio_theremin_wants_active() ? "true" : "false",
             // SB3 CONTROL ASSERTION: the status-bar FL button is the stimulus that test
             // taps, and it is 26x16 px at the very top edge. Without an independent side
             // effect proving the tap LANDED, "the theremin survived" and "my tap missed the
             // button" are the same reading -- which is exactly why SB3 read as non-repro.
             neo_flashlight_is_on() ? "true" : "false",
             // FB11 detection: the tick-audio latch. Either flag stuck TRUE with the Geiger
             // inactive means the core-0 geiger branch is pinned above aud_tone_active, so the
             // screensaver unlock tone is dead for the session. Silent on a quiet bench, which
             // is exactly why it shipped -- so expose the STATE rather than rely on hearing it.
             (rad_geiger_audio_on || aud_geiger_active) ? "true" : "false",
             // F8: the id-75 fullscreen collectible's looping bed. Exposed as the SAME
             // expression the screensaver inhibit uses (UI flag AND the engine's own view),
             // so a test asserts the exact condition that gates the behaviour rather than a
             // proxy for it -- and a latched UI flag with a dead stream reads false here.
             (coll_fs_audio_on && audio_mp3_stream_is_playing()) ? "true" : "false",
             th_wifi_ps_now(), (int)WiFi.getSleep(),
             cb_serial_seen ? "true" : "false",
             (unsigned long)cb_serial_seen_ms,
             (unsigned long)millis(), (int)esp_reset_reason());
    th_send_ok_json("state", buf);
}

static void th_cmd_touch(const String &args) {
    // Parse: "x y press|release|tap"
    int x = 0, y = 0;
    char action[16] = {0};
    if (sscanf(args.c_str(), "%d %d %15s", &x, &y, action) < 2) {
        th_send_err("touch", "usage: touch x y [press|release|tap]");
        return;
    }
    if (action[0] == 0) strcpy(action, "tap");

    if (strcmp(action, "press") == 0) {
        th_touch_x = (int16_t)x;
        th_touch_y = (int16_t)y;
        th_touch_pressed = true;
        th_send_ok("touch");
    } else if (strcmp(action, "release") == 0) {
        th_touch_pressed = false;
        th_send_ok("touch");
    } else if (strcmp(action, "tap") == 0) {
        th_touch_x = (int16_t)x;
        th_touch_y = (int16_t)y;
        th_touch_pressed = true;
        // Let LVGL process the press for 2 tick cycles
        lv_timer_handler();
        delay(50);
        lv_timer_handler();
        th_touch_pressed = false;
        lv_timer_handler();
        delay(20);
        lv_timer_handler();
        th_send_ok("touch");
    } else {
        th_send_err("touch", "unknown action, use press|release|tap");
    }
}

static void th_cmd_swipe(const String &args) {
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0, steps = 10;
    if (sscanf(args.c_str(), "%d %d %d %d %d", &x1, &y1, &x2, &y2, &steps) < 4) {
        th_send_err("swipe", "usage: swipe x1 y1 x2 y2 [steps]");
        return;
    }
    if (steps < 2) steps = 2;
    if (steps > 50) steps = 50;

    th_touch_pressed = true;
    for (int i = 0; i <= steps; i++) {
        th_touch_x = (int16_t)(x1 + (x2 - x1) * i / steps);
        th_touch_y = (int16_t)(y1 + (y2 - y1) * i / steps);
        lv_timer_handler();
        delay(15);
    }
    th_touch_pressed = false;
    lv_timer_handler();
    delay(20);
    lv_timer_handler();
    th_send_ok("swipe");
}

static void th_cmd_nav(const String &args) {
    int div_idx = 0, tab_idx = 0;
    if (sscanf(args.c_str(), "%d %d", &div_idx, &tab_idx) < 2) {
        th_send_err("nav", "usage: nav div tab");
        return;
    }
    if (div_idx < 0 || div_idx >= 3 || tab_idx < 0 || tab_idx >= 3) {
        th_send_err("nav", "div and tab must be 0-2");
        return;
    }
    cur_div = (uint8_t)div_idx;
    cur_tab = (uint8_t)tab_idx;
    cur_sel = -1;
    update_div_indicators();
    rebuild_tabs();
    rebuild_content();
    th_send_ok("nav");
}

// DC34-104: lv_snapshot_take() MUST run inside the LVGL draw context (the same
// context lv_timer_handler() and the CRT V-roll snapshot use). The serial
// command path runs in loop() AFTER lv_timer_handler() returns — when the
// SW draw worker (LV_USE_OS=FREERTOS) is parked — so the snapshot's internal
//   while (layer.draw_task_head) { lv_draw_dispatch_wait_for_request(); ... }
// blocks forever on any screen whose draw doesn't finish in one inline
// dispatch pass (i.e. everything except the simple Status screen). Confirmed:
// the V-roll, which calls the identical lv_snapshot_take from an LVGL timer,
// works on every screen, and dropping LV_DRAW_SW_DRAW_UNIT_CNT to 1 did NOT
// help — it's the calling context, not the unit count.
//
// Fix: the command handler just schedules an lv_async_call and returns; the
// async callback (which lv_timer_handler() runs on the next loop iteration, in
// the correct draw context) takes the snapshot AND streams the reply. We do
// NOT re-pump lv_timer_handler() from the command path — that re-entrancy
// crashed the device.
static void th_snapshot_async_cb(void *unused) {
    (void)unused;
    lv_obj_t *screen = lv_screen_active();
    lv_draw_buf_t *buf = screen ? lv_snapshot_take(screen, LV_COLOR_FORMAT_RGB565) : NULL;
    if (!buf || !buf->data) {
        th_send_err("screenshot", "snapshot alloc failed");
        if (buf) lv_draw_buf_destroy(buf);
        return;
    }
    uint32_t w = buf->header.w;
    uint32_t h = buf->header.h;
    uint32_t stride = buf->header.stride;
    uint32_t data_size = stride * h;

    char hdr[128];
    snprintf(hdr, sizeof(hdr),
             "\"width\":%lu,\"height\":%lu,\"stride\":%lu,\"format\":\"rgb565\",\"size\":%lu",
             (unsigned long)w, (unsigned long)h,
             (unsigned long)stride, (unsigned long)data_size);
    // Silence CB_LOG on the core-0 audio/neopixel tasks for the duration of the
    // binary transfer: without this, a log line spliced BETWEEN the header and
    // the payload (or between chunks) offsets the length-delimited stream the
    // host can't resync -> permanent session desync. (USB-CDC wedge root cause.)
    bool _pq_ss = cb_serial_quiet;   // save/restore so a session-wide `quiet 1` survives
    cb_serial_quiet = true;
    th_send_ok_json("screenshot", hdr);

    // Send raw binary pixel data in chunks.
    // USB-CDC on ESP32-S3 has a 64-byte endpoint; Serial.flush() returns when
    // the local FIFO is handed to the USB stack, NOT when the host has drained
    // the packets. A single 153 KB Serial.write() + one flush can leave the
    // last several endpoint packets unread on the host side, which then get
    // slurped into the NEXT command's response. Chunk the write with
    // per-chunk flush + small delay so USB has time to drain.
    const uint8_t *p = (const uint8_t *)buf->data;
    size_t remaining = data_size;
    const size_t chunk_sz = 1024;
    while (remaining > 0) {
        size_t n = (remaining > chunk_sz) ? chunk_sz : remaining;
        Serial.write(p, n);
        Serial.flush();
        p += n;
        remaining -= n;
    }
    delay(80);  // final host-drain window before next command
    cb_serial_quiet = _pq_ss;

    lv_draw_buf_destroy(buf);
}

static void th_cmd_screenshot() {
    // Defer snapshot + reply into the LVGL draw context (see note above).
    lv_async_call(th_snapshot_async_cb, NULL);
}

static void th_cmd_tree() {
    Serial.printf("%c{\"ok\":true,\"cmd\":\"tree\",\"start\":true}\n", TH_STX);
    lv_obj_t *screen = lv_screen_active();
    if (screen) th_walk_tree(screen, 0);
    Serial.printf("%c{\"ok\":true,\"cmd\":\"tree\",\"end\":true}\n", TH_STX);
}

static void th_text_collect(lv_obj_t *obj, String &out) {
    if (!obj) return;
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return;
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *text = lv_label_get_text(obj);
        if (text && text[0]) {
            if (out.length() > 0) out += ',';
            out += '"';
            for (const char *p = text; *p; p++) {
                if (*p == '"')       out += "\\\"";
                else if (*p == '\\') out += "\\\\";
                else if (*p == '\n') out += "\\n";
                else if ((uint8_t)*p >= 0x20) out += *p;
            }
            out += '"';
        }
    }
    uint32_t cnt = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < cnt; i++)
        th_text_collect(lv_obj_get_child(obj, i), out);
}

// ── find <substring>: locate a widget by its label text and return TAPPABLE coords ──
// Added 2026-07-25. Without this, every touch-driven repro needed a human finger:
// `tree` reports only a node COUNT (it streams the detail to Serial, which the session
// bridge's line reader drops), and `text` gives strings with no geometry. So bugs behind
// a real button -- the nested-keyboard wedge, the status-bar FL tap, the screensaver
// hold-bar -- were classed "physically unprovable" when they are merely un-addressable.
//
// Returns, for each label whose text CONTAINS the query: the label's own centre, plus the
// centre of its nearest CLICKABLE ancestor (buttons put their label in a child, so tapping
// the label's centre is usually right but the ancestor is what actually handles the event)
// and that ancestor's geometry. Coordinates are screen-absolute, ready for `touch x y tap`.
static void th_find_collect(lv_obj_t *obj, const char *needle, String &out, int &n,
                            bool exact) {
    if (!obj || n >= 12) return;
    if (!lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        if (lv_obj_check_type(obj, &lv_label_class)) {
            const char *text = lv_label_get_text(obj);
            bool hit = text && text[0] &&
                       (exact ? (strcmp(text, needle) == 0) : (strstr(text, needle) != NULL));
            if (hit) {
                lv_obj_update_layout(obj);
                lv_area_t a;
                lv_obj_get_coords(obj, &a);
                // Walk up to the nearest clickable ancestor (the actual event target).
                lv_obj_t *hit = obj;
                for (lv_obj_t *p = obj; p; p = lv_obj_get_parent(p)) {
                    if (lv_obj_has_flag(p, LV_OBJ_FLAG_CLICKABLE)) { hit = p; break; }
                }
                lv_area_t h;
                lv_obj_get_coords(hit, &h);
                if (n) out += ',';
                out += "{\"text\":\"";
                for (const char *p = text; *p; p++) {
                    if (*p == '"')       out += "\\\"";
                    else if (*p == '\\') out += "\\\\";
                    else if (*p == '\n') out += "\\n";
                    else if ((uint8_t)*p >= 0x20) out += *p;
                }
                // ⚠ onscreen: is the tap point actually inside the display AND not scrolled
                // out of the content pane? A widget can be laid out below the visible area
                // (a long list pushes the trailing button past CONTENT_Y+CONTENT_H) and
                // lv_obj_get_coords() still reports those out-of-view coordinates. A test
                // that taps them hits whatever IS at that pixel -- for a list overflowing the
                // content pane that is the division bar, so the tap NAVIGATES instead, and the
                // test reports a confusing downstream failure ("no keyboard modal appeared")
                // instead of "your target was off-screen". Report it so callers can refuse.
                int32_t hx = (h.x1 + h.x2) / 2, hy = (h.y1 + h.y2) / 2;
                bool onscreen = (hx >= 0 && hx < SCREEN_W && hy >= 0 && hy < SCREEN_H);
                // Also require the point to lie inside every scrollable ancestor's box, which
                // is what actually clips it.
                for (lv_obj_t *p = lv_obj_get_parent(hit); p && onscreen; p = lv_obj_get_parent(p)) {
                    if (!lv_obj_has_flag(p, LV_OBJ_FLAG_SCROLLABLE)) continue;
                    lv_area_t pa; lv_obj_get_coords(p, &pa);
                    if (hy < pa.y1 || hy > pa.y2 || hx < pa.x1 || hx > pa.x2) onscreen = false;
                }
                out += "\",\"x\":" + String((a.x1 + a.x2) / 2) +
                       ",\"y\":" + String((a.y1 + a.y2) / 2) +
                       ",\"hit_x\":" + String(hx) +
                       ",\"hit_y\":" + String(hy) +
                       ",\"hit_w\":" + String(h.x2 - h.x1 + 1) +
                       ",\"hit_h\":" + String(h.y2 - h.y1 + 1) +
                       ",\"onscreen\":" + String(onscreen ? "true" : "false") +
                       ",\"clickable\":" + String(hit != obj ? "true" : "false") + "}";
                n++;
            }
        }
    } else {
        return;   // hidden subtree: nothing in it is tappable
    }
    uint32_t cnt = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < cnt; i++)
        th_find_collect(lv_obj_get_child(obj, i), needle, out, n, exact);
}

static void th_cmd_find(const String &args) {
    String q = args;
    q.trim();
    // Leading '=' selects EXACT match. Substring matching is convenient but a footgun for
    // short queries: `find L` matched the status-bar "FL" button and quietly tested the
    // wrong widget.
    bool exact = false;
    if (q.startsWith("=")) { exact = true; q = q.substring(1); q.trim(); }
    if (q.length() == 0) { th_send_err("find", "usage: find [=]<label text>"); return; }
    String body;
    body.reserve(1024);
    int n = 0;
    lv_obj_t *screen = lv_screen_active();
    if (screen) th_find_collect(screen, q.c_str(), body, n, exact);
    String wrapper = "\"query\":\"" + q + "\",\"exact\":" + (exact ? "true" : "false") +
                     ",\"count\":" + String(n) + ",\"hits\":[" + body + "]";
    th_send_ok_json("find", wrapper.c_str());
}

static void th_cmd_text() {
    // Build the entire "texts":[...] payload as a single String, then emit
    // on one line so the session bridge's line-based reader sees the full
    // JSON object. Previously streamed as multi-line, which truncated at
    // the header for session-mode clients.
    String body;
    body.reserve(4096);
    lv_obj_t *screen = lv_screen_active();
    if (screen) th_text_collect(screen, body);
    String wrapper = "\"texts\":[";
    wrapper += body;
    wrapper += "]";
    th_send_ok_json("text", wrapper.c_str());
}

static void th_cmd_skip_boot() {
    // Dismiss boot overlay
    if (boot_overlay) {
        lv_obj_delete(boot_overlay);
        boot_overlay = NULL;
        boot_label = NULL;
        if (boot_timer) { lv_timer_delete(boot_timer); boot_timer = NULL; }
    }
    th_send_ok("skip_boot");
}

static void th_cmd_sensor_mock(const String &args) {
    // Expect 64 distance values as comma-separated integers
    // Format: "d0,d1,...,d63" — all treated as valid (status=5)
    memset(&th_sensor_data, 0, sizeof(th_sensor_data));
    int idx = 0;
    int pos = 0;
    while (idx < 64 && pos < (int)args.length()) {
        int val = 0;
        bool neg = false;
        if (args[pos] == '-') { neg = true; pos++; }
        while (pos < (int)args.length() && args[pos] >= '0' && args[pos] <= '9') {
            val = val * 10 + (args[pos] - '0');
            pos++;
        }
        if (neg) val = -val;
        th_sensor_data.distance_mm[idx] = (int16_t)val;
        th_sensor_data.target_status[idx] = 5;  // valid
        th_sensor_data.nb_target_detected[idx] = 1;
        idx++;
        if (pos < (int)args.length() && args[pos] == ',') pos++;
    }
    // Fill remaining with invalid
    for (; idx < 64; idx++) {
        th_sensor_data.distance_mm[idx] = 0;
        th_sensor_data.target_status[idx] = 0;
        th_sensor_data.nb_target_detected[idx] = 0;
    }
    th_sensor_mock = true;
    th_send_ok("sensor_mock");
}

static void th_cmd_sensor_real() {
    th_sensor_mock = false;
    th_send_ok("sensor_real");
}

static void th_cmd_audio_capture(const String &args) {
    if (!th_audio_ring) {
        th_send_err("audio_capture", "ring buffer not allocated");
        return;
    }
    int frames = 4096;
    sscanf(args.c_str(), "%d", &frames);
    if (frames < 1) frames = 1;
    if (frames > TH_AUDIO_RING_SIZE) frames = TH_AUDIO_RING_SIZE;

    // Read available frames from ring buffer
    uint32_t wr = th_audio_wr;
    uint32_t rd = th_audio_rd;
    uint32_t avail = (wr - rd) & TH_AUDIO_RING_MASK;
    if ((uint32_t)frames > avail) frames = (int)avail;

    uint32_t byte_count = (uint32_t)frames * 4;  // stereo int16_t = 4 bytes/frame
    char hdr[128];
    snprintf(hdr, sizeof(hdr),
             "\"frames\":%d,\"channels\":2,\"sample_rate\":44100,\"bits\":16,\"size\":%lu",
             frames, (unsigned long)byte_count);
    bool _pq_au = cb_serial_quiet;   // no core-0 log splicing into the PCM byte stream
    cb_serial_quiet = true;
    th_send_ok_json("audio_capture", hdr);

    // Send raw PCM data
    for (int i = 0; i < frames; i++) {
        uint32_t ri = (rd + i) & TH_AUDIO_RING_MASK;
        Serial.write((uint8_t *)&th_audio_ring[ri * 2], 4);
    }
    Serial.flush();
    delay(20);
    cb_serial_quiet = _pq_au;

    th_audio_rd = (rd + frames) & TH_AUDIO_RING_MASK;
}

static void th_cmd_quiet(const String &args) {
    // Silence ALL CB_LOG status chatter for the session so log lines can't
    // collide with the STX protocol / sd_exists text responses (the text-only
    // variant of the USB-CDC session wedge -- worst in the SD-probe-heavy pcap
    // matrix). Host sets `quiet 1` at session start. Binary transfers save/restore
    // this, so a session-wide quiet survives a screenshot. `quiet 0` re-enables.
    int v = 1;
    if (args.length()) sscanf(args.c_str(), "%d", &v);
    cb_serial_quiet = (v != 0);
    char buf[32];
    snprintf(buf, sizeof(buf), "\"quiet\":%s", cb_serial_quiet ? "true" : "false");
    th_send_ok_json("quiet", buf);
}

static void th_cmd_neopixel_state() {
    // Report current LED states from NeoPixel driver
    char buf[512];
    int pos = snprintf(buf, sizeof(buf), "\"leds\":[");
    for (int i = 0; i < CB_NEOPIXEL_COUNT; i++) {
        uint32_t c = neo_strip.getPixelColor(i);
        uint8_t r = (c >> 16) & 0xFF;
        uint8_t g = (c >> 8) & 0xFF;
        uint8_t b = c & 0xFF;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "%s{\"r\":%u,\"g\":%u,\"b\":%u}",
                        i > 0 ? "," : "", r, g, b);
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]");
    th_send_ok_json("neopixel_state", buf);
}

static void th_cmd_fps(const String &args) {
    if (args == "reset") {
        th_fps_reset();
        th_send_ok("fps");
        return;
    }
    uint32_t avg = th_fps_samples > 0 ? (uint32_t)(th_fps_sum / th_fps_samples) : 0;
    char buf[128];
    // fps_max exists because the per-tool table asks for low/hi/avg, and because a min alone
    // cannot distinguish "this tool is heavy" from "one 1 s window was heavy". `samples` is
    // reported so a reader can tell an idle window (samples 0 -> every field 0) from a genuine
    // zero-FPS stall; treating an unsampled window as 0 fps is how a vacuous reading gets in.
    snprintf(buf, sizeof(buf),
             "\"fps\":%lu,\"fps_min\":%lu,\"fps_max\":%lu,\"fps_avg\":%lu,\"samples\":%lu",
             (unsigned long)th_fps_value,
             (unsigned long)(th_fps_min == 999 ? 0 : th_fps_min),
             (unsigned long)th_fps_max,
             (unsigned long)avg,
             (unsigned long)th_fps_samples);
    th_send_ok_json("fps", buf);
}

// Clear the first-boot onboarding flags so the next reboot replays the
// disclaimer + Clippy intro. Test-only; lets the harness exercise the
// first-boot flow (e.g. the legal-notice / tour ordering) without a full
// NVS erase that would also wipe collectibles.
static void th_cmd_onboarding_reset() {
    cfg.legal_ack     = false;
    cfg.legal_ack_ver = 0;       // re-prompt the versioned consent
    cfg.clippy_seen   = false;
    cfg_save_legal_ack();
    cfg_save_clippy_seen();
    th_send_ok("onboarding_reset");
}

// Inverse of onboarding_reset: put a FRESH-NVS badge into a known test-ready
// state so an automated suite isn't blocked by the first-boot flow. Accepts the
// legal consent at the CURRENT policy version, marks the Clippy intro seen, and
// silences radio alerts (the "new station" / nudge modals pop mid-test otherwise).
// Idempotent. Test-only. Run once right after the session opens on a fresh badge.
static void th_cmd_onboarding_accept() {
    cfg.legal_ack     = true;
    cfg.legal_ack_ver = LEGAL_POLICY_VERSION;   // satisfies the versioned re-prompt gate
    cfg.clippy_seen   = true;
    cfg.radio_reminder_off = true;              // no "new station" / nudge popups during tests
    cfg_save_legal_ack();
    cfg_save_clippy_seen();
    cfg_save_radio_discovery();
    th_send_ok("onboarding_accept");
}

// Jump straight to a Clippy tour step (0-based) so the tour's bubble layout
// can be screenshotted without touch-driving the intro. Test-only.
static void th_cmd_tour_step(const String &args) {
    int n = args.toInt();
    if (n < 0 || n >= kNumTourSteps) { th_send_err("tour_step", "out of range"); return; }
    show_clippy_tour_step(n);
    th_send_ok("tour_step");
}

static void th_cmd_led_rate() {
    char buf[128];
    snprintf(buf, sizeof(buf),
             "\"rate\":%lu,\"total\":%lu",
             (unsigned long)neo_show_rate,
             (unsigned long)neo_show_count);
    th_send_ok_json("led_rate", buf);
}

static void th_cmd_tool_list() {
    // Enumerate the COMPILED tool categories. SKU-specific: the Sn34k-Boy build
    // omits the ACTIVE RESEARCH cats entirely, so the host can verify which
    // categories exist in each binary by name/id (robust to array compaction).
    char buf[700];
    int n = 0;
    n += snprintf(buf + n, sizeof(buf) - n,
                  "\"count\":%d,\"cats\":[", (int)NUM_TOOL_CATS);
    for (uint8_t i = 0; i < NUM_TOOL_CATS && n < (int)sizeof(buf) - 80; i++) {
        n += snprintf(buf + n, sizeof(buf) - n,
                      "%s{\"id\":%d,\"name\":\"%s\",\"items\":%d}",
                      i ? "," : "", tool_categories[i].id,
                      tool_categories[i].name, tool_categories[i].count);
    }
    snprintf(buf + n, sizeof(buf) - n, "]");
    th_send_ok_json("tool_list", buf);
}

static void th_cmd_tool_state() {
    // Report current tool/ClipBoy operation state.
    // 640: snprintf truncates SAFELY but a truncated response is INVALID JSON, which the reader
    // discards as a raw line -- so the failure mode of a too-small buffer here is "the command
    // mysteriously times out", not a clipped field. Keep headroom when adding fields.
    char buf[640];
    const char *name = cb_op_name ? cb_op_name : "";
    // Escape name for JSON
    char ename[64] = {0};
    int ei = 0;
    for (const char *p = name; *p && ei < 60; p++) {
        if (*p == '"' || *p == '\\') { ename[ei++] = '\\'; }
        ename[ei++] = *p;
    }
    // `lib_scanning` is the LIBRARY's own view (WiFiScan::currentScanMode != OFF), as opposed
    // to `running`, which is only the UI's bookkeeping. FB5 is precisely a disagreement
    // between the two: three Join-WiFi handlers cleared cb_op_running without ever calling
    // cb.stopScan(), so the UI said "Stopped" while cb.loop() kept driving
    // wifi_scan_obj.main() -- and on Res34rch that means the radio was still TRANSMITTING.
    // Every badge-side counter reported "not running", which is exactly why the audit called
    // this unprovable without an external RF witness. Exposing the library state makes it
    // provable on one badge: assert running == false AND lib_scanning == false.
    // FB1 needs the DRIVER's own view of promiscuous mode, read straight from IDF. Every
    // indirect probe tried first was VACUOUS: `pkt_counters` reads all-zero even during a
    // live scan that is finding APs (it is fed only by the raw-sniffer callbacks, not by
    // Scan/Beacons), and `detect_counts.ap` cannot be used either -- the AP list PERSISTS
    // across tool switches by design, and clearing it first ALSO stops repopulating it even
    // while a scan runs. Both produced confident wrong answers. esp_wifi_get_promiscuous()
    // measures exactly what FB1 changes, on one badge, with no RF rig and no inference.
    bool th_promisc = false;
    if (esp_wifi_get_promiscuous(&th_promisc) != ESP_OK) th_promisc = false;
    // R1 needs the STATUS-BAR TASK SLOT ITSELF, not a text search. The old test picked "the
    // first text on the screen containing 'APs' or 'STAs'" and matched the tool-list ROW
    // "APs (full)" -- a left-pane menu label, never the status bar -- so both its readings
    // were the same string and it scored a vacuous pass. `stask` is lbl_stask's own text
    // (the widget R1's fix drives) and `manual_kind` is cb_manual_scan_kind, the exact
    // variable the fix binds at tool start. Reporting both separates "the binding is wrong"
    // (R1 itself) from "the poller phrased it wrong" (a status-bar bug one layer up).
    char estask[48] = {0};
    if (lbl_stask) {
        const char *st = lv_label_get_text(lbl_stask);
        int si = 0;
        for (const char *p = st; p && *p && si < 44; p++) {
            if (*p == '"' || *p == '\\') estask[si++] = '\\';
            estask[si++] = *p;
        }
    }
    snprintf(buf, sizeof(buf),
             "\"running\":%s,\"name\":\"%s\",\"encoded\":%d,"
             "\"scan_timer\":%s,\"output_timer\":%s,"
             "\"geiger_active\":%s,\"hr_scanning\":%s,\"theremin_active\":%s,"
             "\"lib_scanning\":%s,\"promisc\":%s,"
             "\"stask\":\"%s\",\"manual_kind\":%d,"
             // Monitor > RSSI freshness. `rssi_appends` is the load-bearing one: it proves the
             // trace HALTED rather than merely that the label said so.
             "\"rssi_quiet_ms\":%lu,\"rssi_appends\":%lu",
             cb_op_running ? "true" : "false",
             ename,
             (int)cb_op_encoded,
             cb_scan_timer ? "true" : "false",
             cb_output_timer ? "true" : "false",
             rad_geiger_active ? "true" : "false",
             hr_scanning ? "true" : "false",
             (vl53_initialized || audio_theremin_is_active()) ? "true" : "false",
             cb.isScanning() ? "true" : "false",
             th_promisc ? "true" : "false",
             estask,
             (int)cb_manual_scan_kind,
             (unsigned long)(cb_rssi_pkts_valid ? (millis() - cb_rssi_last_rx_ms) : 0),
             (unsigned long)cb_rssi_appends);
    th_send_ok_json("tool_state", buf);
}

static void th_cmd_geiger_start() {
    if (rad_geiger_active) {
        th_send_ok_json("geiger_start", "\"already\":true");
        return;
    }
    // Call the SAME function the UI button calls. This used to be a hand-copied duplicate of
    // rad_toggle_cb's Start branch, which had already drifted once -- so the harness was
    // testing a private near-copy of Geiger startup rather than the code that ships. In
    // particular an airplane-mode gate added to the UI path would not have been exercised here
    // at all, and the test would have passed while the gate went unverified.
    if (!rad_geiger_start()) {
        th_send_err("geiger_start", "blocked by airplane mode");
        return;
    }
    th_send_ok("geiger_start");
}

static void th_cmd_geiger_stop() {
    // Was a hand-rolled copy of the firmware teardown. Sharing rad_geiger_force_stop() means
    // the harness can never drift from what the badge actually does -- the divergence that let
    // FB11 hide: `geiger_start` and this both looked complete while six UI paths did not.
    rad_geiger_force_stop();
    if (lbl_stask) lv_label_set_text(lbl_stask, "");
    th_send_ok("geiger_stop");
}

static void th_cmd_tool_start(const String &args) {
    // Start a tool by category and item index.
    // Format: "cat item" e.g. "0 0" for WiFi Scan > APs (full)
    int cat = 0, item = 0;
    if (sscanf(args.c_str(), "%d %d", &cat, &item) < 2) {
        th_send_err("tool_start", "usage: tool_start cat item");
        return;
    }
    if (cat < 0 || cat >= NUM_TOOL_CATS) {
        th_send_err("tool_start", "invalid category");
        return;
    }
    if (item < 0 || item >= (int)tool_categories[cat].count) {
        th_send_err("tool_start", "invalid item");
        return;
    }
    // Stop any running operation first
    if (cb_op_running) cb_stop_operation();
    // FB11: was a bare flag clear -> the tick audio stayed latched, so a later screensaver
    // hold had no tone. Use the same helper the firmware uses.
    rad_geiger_force_stop();
    // Release the LiDAR activities (HR scan + theremin) before the radio.
    stop_lidar_activities();

    const char *name = tool_categories[cat].items[item].name;
    // Ask the SAME launch question the UI asks (tool_tap_cb). Dispatching directly bypassed the
    // airplane gate, so a scripted `cfg_set airplane true` + `tool_start` would start a radio
    // tool and reach the IDF sniffer with the Airplane switch reading engaged -- and the gate
    // had no automated coverage at all. Returns a distinct error so a test can assert BLOCKED
    // rather than infer it from side effects.
    if (!tool_launch_allowed((uint8_t)cat, (uint8_t)item)) {
        th_send_err("tool_start", "blocked by airplane mode");
        return;
    }
    if (pcap_file_tool_gated(tool_categories[cat].id, (uint8_t)item)) {
        th_send_err("tool_start", "PCAP Saving off -- enable it in DATA > Settings");
        return;
    }
    int32_t encoded = tool_encode((uint8_t)cat, (uint8_t)item);
    cb_ensure_wifi();
    // Dispatch by STABLE category id (not array position) - matches the UI path
    // so harness starts stay correct in the Sn34k build where active cats are
    // compiled out and positions compact.
    dispatch_clipboy_action(tool_categories[cat].id, (uint8_t)item);
    cb_op_running = true;
    cb_op_name = name;
    cb_op_encoded = encoded;
    if (lbl_stask) lv_label_set_text(lbl_stask, name);

    char buf[128];
    snprintf(buf, sizeof(buf),
             "\"cat\":%d,\"item\":%d,\"name\":\"%s\"",
             cat, item, name);
    th_send_ok_json("tool_start", buf);
}

static void th_cmd_tool_stop() {
    if (cb_op_running) {
        cb_stop_operation();
    }
    rad_geiger_force_stop();   // FB11: one shared teardown, firmware + harness
    audio_mp3_stop();  // Stop any playing sound
    th_send_ok("tool_stop");
}

// Recursively find the tool's START/STOP action button (by label text) so the
// harness can drive the REAL run path. The button is created by every
// show_tool_* builder via make_action_btn(..., cb_tool_start_stop_cb, ...) with
// a "> START <" / "> STOP <" label.
static lv_obj_t *th_find_start_btn(lv_obj_t *parent) {
    if (!parent) return NULL;
    uint32_t n = lv_obj_get_child_count(parent);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(parent, i);
        if (lv_obj_has_class(c, &lv_button_class)) {
            lv_obj_t *l = lv_obj_get_child(c, 0);
            if (l && lv_obj_has_class(l, &lv_label_class)) {
                const char *t = lv_label_get_text(l);
                if (t && (strstr(t, "START") || strstr(t, "STOP"))) return c;
            }
        }
        lv_obj_t *deep = th_find_start_btn(c);
        if (deep) return deep;
    }
    return NULL;
}

// tool_open <cat> <item>: drive a tool the way a USER does -- navigate to
// ITEMS > Tools, render the tool's detail pane (show_tool_detail), then fire the
// START button. Unlike tool_start (which only dispatches the Marauder op), this
// builds the live output pane + starts the UI pollers, so a screenshot captures
// exactly what the user sees during execution. Reports whether the run started.
static void th_cmd_tool_open(const String &args) {
    int cat = 0, item = 0;
    if (sscanf(args.c_str(), "%d %d", &cat, &item) < 2) {
        th_send_err("tool_open", "usage: tool_open cat item");
        return;
    }
    if (cat < 0 || cat >= NUM_TOOL_CATS) {
        th_send_err("tool_open", "invalid category");
        return;
    }
    if (item < 0 || item >= (int)tool_categories[cat].count) {
        th_send_err("tool_open", "invalid item");
        return;
    }
    // Same launch gate as tool_start / the UI: tool_open FIRES the START button below, so it
    // launches the tool and must not bypass the airplane check either.
    if (!tool_launch_allowed((uint8_t)cat, (uint8_t)item)) {
        th_send_err("tool_open", "blocked by airplane mode");
        return;
    }
    // Land on ITEMS (div 1) > Tools (tab 0) so the split-pane + right_pane exist,
    // exactly like a user tapping into the Tools screen.
    // NOTE: this block is goto_div_tab()'s body minus its same-screen early return. Left
    // duplicated deliberately for now -- goto_div_tab() additionally records arg_secret_record()
    // on the division tap, and calling it here would inject ARG secret-menu input from every
    // scripted tool_open. Collapsing them needs a no-ARG variant, which is a change to shipping
    // code for a test-only benefit; queued, not done a week from DEF CON.
    if (cur_div != 1 || cur_tab != 0) {
        cur_div = 1; cur_tab = 0; cur_sel = -1;
        update_div_indicators();
        rebuild_tabs();
        rebuild_content();
    }
    // Render this tool's detail pane (the real row-tap path).
    show_tool_detail((uint8_t)cat, (uint8_t)item);
    // Fire the START button just like a tap -> builds live output + pollers.
    lv_obj_t *btn = th_find_start_btn(right_pane);
    bool started = false;
    if (btn) {
        // Fire the START exactly like a tap. Do NOT pump lv_timer_handler() here:
        // the click just created the scan/output poll timers, and re-entering
        // lv_timer_handler() from the serial command path runs them immediately
        // (one does a blocking ap-scan sweep) -> ~30-46s hang. The normal loop()
        // pumps them on the next iteration; the (async) screenshot cmd renders
        // the live pane. Same code a tap runs, just without the re-pump.
        lv_obj_send_event(btn, LV_EVENT_CLICKED, NULL);
        started = true;
    }
    char buf[160];
    const char *name = tool_categories[cat].items[item].name;
    snprintf(buf, sizeof(buf),
             "\"cat\":%d,\"item\":%d,\"name\":\"%s\",\"btn\":%s,\"running\":%s",
             cat, item, name, started ? "true" : "false",
             cb_op_running ? "true" : "false");
    th_send_ok_json("tool_open", buf);
}

// Forward a command to a serial handler, and REPORT WHICH ONE TOOK IT.
//
// ⚠ This used to claim (in its own comment) that it forwarded to "the normal serial CLI
// handler" while actually handling only `coll ...` and `heap` -- everything else fell through
// to an unconditional th_send_ok("cli"). So `cli scanap` returned {"ok":true} and executed
// NOTHING, and any test built on it would have passed while never touching the CLI. A command
// that reports success for work it did not do is worse than one that does not exist.
//
// Now: known harness-side handlers first (unchanged behaviour for existing callers), then a real
// fallthrough to `cli_obj.runCommand()` -- the same entry point Clip-Boy.ino:632 uses for a typed
// line, so the REAL Marauder parser and handlers run. The response says which path was taken, so
// a caller can never again mistake "acknowledged" for "executed".
//
// This is what makes the Marauder CLI testable at all. Two things are reachable only this way:
// the CLI-only deref sites (RunAPInfo, the `list -s` station walk), and whether the CLI honours
// Airplane mode -- our gate is `tool_launch_allowed` in the UI layer, and the string "airplane"
// appears nowhere in CommandLine.cpp or WiFiScan.cpp.
// The CLI prints to Serial as plain text; harness.py skips non-JSON lines and reads on to this
// ack. Keep commands narrow -- a very chatty one can exceed the reader's line budget.
static void th_cmd_cli(const String &args) {
    String line = args;
    line.trim();
    if (line.length() == 0) {
        th_send_err("cli", "usage: cli <command>");
        return;
    }
    const char *via = "marauder";
#ifdef COLL_DEBUG
    if (line.startsWith("coll ")) {
        coll_process_serial(line);
        via = "coll";
    } else
#endif
    if (line == "heap") {
        Serial.printf("Free DRAM: %lu bytes\n",
            (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        Serial.printf("Free PSRAM: %lu bytes\n",
            (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        via = "harness";
    } else {
        cli_obj.runCommand(line);       // the REAL CLI -- same call loop() makes for a typed line
    }
    char esc[96] = {0};
    int ei = 0;
    for (const char *p = line.c_str(); *p && ei < 92; p++) {
        if (*p == '"' || *p == '\\') esc[ei++] = '\\';
        esc[ei++] = *p;
    }
    char buf[192];
    snprintf(buf, sizeof(buf), "\"ran\":\"%s\",\"via\":\"%s\"", esc, via);
    th_send_ok_json("cli", buf);
}

// ─── Keyboard / text input ────────────────────────────────────────────────

static void th_cmd_kb_type(const String &args) {
    // Type text into the active keyboard modal's textarea.
    // If a keyboard modal is open (kb_modal != NULL), sets the textarea text
    // directly via LVGL API — no touch simulation needed.
    if (!kb_modal) {
        th_send_err("kb_type", "no keyboard modal open");
        return;
    }
    // Textarea is child 1 of kb_modal (0=title, 1=ta, 2=btn_row, 3=kb)
    lv_obj_t *ta = lv_obj_get_child(kb_modal, 1);
    if (!ta) {
        th_send_err("kb_type", "textarea not found in modal");
        return;
    }
    lv_textarea_set_text(ta, args.c_str());
    lv_timer_handler();  // Process the text change
    th_send_ok("kb_type");
}

static void th_cmd_kb_ok() {
    // Press the OK button on the active keyboard modal.
    if (!kb_modal) {
        th_send_err("kb_ok", "no keyboard modal open");
        return;
    }
    // OK button is child 1 of btn_row (child 2 of modal)
    lv_obj_t *btn_row = lv_obj_get_child(kb_modal, 2);
    if (!btn_row) { th_send_err("kb_ok", "btn_row not found"); return; }
    lv_obj_t *btn_ok = lv_obj_get_child(btn_row, 1);
    if (!btn_ok) { th_send_err("kb_ok", "ok button not found"); return; }
    // Simulate click on the OK button
    lv_obj_send_event(btn_ok, LV_EVENT_CLICKED, NULL);
    lv_timer_handler();
    th_send_ok("kb_ok");
}

static void th_cmd_kb_cancel() {
    if (!kb_modal) {
        th_send_err("kb_cancel", "no keyboard modal open");
        return;
    }
    lv_obj_t *btn_row = lv_obj_get_child(kb_modal, 2);
    if (!btn_row) { th_send_err("kb_cancel", "btn_row not found"); return; }
    lv_obj_t *btn_cancel = lv_obj_get_child(btn_row, 0);
    if (!btn_cancel) { th_send_err("kb_cancel", "cancel button not found"); return; }
    lv_obj_send_event(btn_cancel, LV_EVENT_CLICKED, NULL);
    lv_timer_handler();
    th_send_ok("kb_cancel");
}

// ─── AP scan + select ─────────────────────────────────────────────────────

static void th_cmd_ap_scan(const String &args) {
    // Scan for APs and optionally select one by SSID.
    // Usage: ap_scan             — just scan, return results
    //        ap_scan shipship    — scan and select matching SSID
    //
    // Uses the AP+Station scan (WIFI_SCAN_AP_STA / promiscuous beacon sniff) and
    // drives the channel hop MANUALLY here. The single-channel TARGET_AP scan
    // only saw beacons on the parked channel, so it missed a target on another
    // channel (shipship on ch8). But we cannot rely on the firmware's own
    // channelHop() either: it runs from the main loop(), and this command BLOCKS
    // the main loop, so loop() never executes during the wait and the radio
    // stays parked. Instead we cb.setChannel() across 1-13 ourselves so the
    // promiscuous callback accumulates APs band-wide. selectAP-by-SSID then
    // finds the target wherever it lives. (~2 sweeps, ~16s < session timeout.)
    cb_stop_operation();
    cb_ensure_wifi();
    cb.clearAPs();
    cb.scanAPsAndStations();

    for (int sweep = 0; sweep < 2; sweep++) {
        for (int ch = 1; ch <= 13; ch++) {
            cb.setChannel(ch);
            delay(600);   // dwell — long enough to catch a ~100ms beacon interval
        }
    }
    // cb_stop_operation(), not a bare cb.stopScan(). The bare call clears the library's scan
    // MODE but leaves the ESP-IDF promiscuous filter and its RX callback installed (that is
    // exactly what the FB1 fix added), and skips cb.finishCapture() + cb_output_cleanup(). So
    // this command left the badge RECEIVING while tool_state.running read false -- which
    // silently POLLUTES the FB1 assertion in test_teardown_paths.py (it checks
    // tool_state.promisc after a stop), and conversely would let a genuine FB1 regression be
    // blamed on the harness. Use the same path the UI uses.
    cb_stop_operation();

    int count = cb.getAPCount();
    String target = args;
    target.trim();

    // Build response with AP list
    int selected = -1;
    char buf[256];
    snprintf(buf, sizeof(buf), "\"count\":%d", count);

    if (target.length() > 0) {
        for (int i = 0; i < count; i++) {
            CBAccessPointInfo ap;
            if (!cb.getAP(i, ap)) continue;
            ap.essid[sizeof(ap.essid) - 1] = '\0';
            if (String(ap.essid) == target) {
                cb.selectAP(i);
                selected = i;
                break;
            }
        }
        char sel_buf[128];
        snprintf(sel_buf, sizeof(sel_buf),
                 ",\"target\":\"%s\",\"selected\":%d",
                 target.c_str(), selected);
        strncat(buf, sel_buf, sizeof(buf) - strlen(buf) - 1);
    }

    th_send_ok_json("ap_scan", buf);
}

static void th_cmd_wifijoin(const String &args) {
    // Join a WiFi network so THIS badge becomes a client (station) -- used to stand up a
    // controlled target on a test AP for the station-directed tools (Bad Msg / Sleep /
    // Deauth Stations). Usage: wifijoin <ssid> <password>
    //
    // Replicates the UI Join-WiFi preconditions EXACTLY (ui_nav.h ~5599): stop any running
    // op, force-stop the Geiger (it is not cb_op_running), clear the promiscuous RX callback
    // (joinWiFi's documented precondition), suspend audio across the blocking Core-1 join.
    // Airplane-gated like the tool launchers.
    String a = args; a.trim();
    int sp = a.indexOf(' ');
    if (sp <= 0) { th_send_err("wifijoin", "usage: wifijoin <ssid> <password>"); return; }
    String ssid = a.substring(0, sp); ssid.trim();
    String pw = a.substring(sp + 1); pw.trim();
    if (ssid.length() == 0) { th_send_err("wifijoin", "empty ssid"); return; }
    if (cfg.airplane) { th_send_err("wifijoin", "blocked by airplane mode"); return; }

    if (cb_op_running) cb_stop_operation();
    rad_geiger_force_stop();
    esp_wifi_set_promiscuous_rx_cb(NULL);
    esp_wifi_set_promiscuous(false);
    audio_suspend();
    bool ok = cb.joinWiFi(ssid, pw);
    audio_resume();
    char buf[64];
    snprintf(buf, sizeof(buf), "\"ssid\":\"%s\",\"joined\":%s", ssid.c_str(), ok ? "true" : "false");
    th_send_ok_json("wifijoin", buf);
}

static void th_cmd_sta_select(const String &args) {
    // Scan APs+Stations and select an AP + a station under it, so the
    // station-targeted deauth (cat 6,2 / the targeted-deauth scan mode) has a
    // target. That tool sends a DIRECTED deauth to the selected station's MAC
    // from the AP's BSSID -- unlike the broadcast Deauth-Discovered, a directed
    // deauth reliably drops a client (brcmfmac/PMF resist broadcast).
    // Usage: sta_select <ap_ssid> [sta_mac]
    //   sta_mac given -> select that station; omitted -> first station of the AP.
    // Drives the channel hop manually (same reason as ap_scan): the blocking
    // wait freezes the firmware's main-loop channelHop().
    String a = args; a.trim();
    int sp = a.indexOf(' ');
    String ssid = sp > 0 ? a.substring(0, sp) : a;
    String macWant = sp > 0 ? a.substring(sp + 1) : "";
    ssid.trim(); macWant.trim(); macWant.toLowerCase();
    if (ssid.length() == 0) { th_send_err("sta_select", "usage: sta_select <ssid> [mac]"); return; }

    cb_stop_operation();
    cb_ensure_wifi();
    cb.clearAPs();
    cb.scanAPsAndStations();
    // Sweep once to discover APs...
    for (int ch = 1; ch <= 13; ch++) { cb.setChannel(ch); delay(500); }
    // ...then PARK on the target AP's channel and dwell, so the scanner sniffs
    // the station<->AP data frames it needs to LINK a station to that AP. A
    // hopping sweep only dwells ~500ms per channel -- not enough to catch a
    // client's traffic and associate it. The station must be actively sending.
    int apChan = 0, apCnt0 = cb.getAPCount();
    for (int i = 0; i < apCnt0; i++) {
        CBAccessPointInfo ap;
        if (!cb.getAP(i, ap)) continue;
        ap.essid[sizeof(ap.essid) - 1] = '\0';
        if (String(ap.essid) == ssid) { apChan = ap.channel; break; }
    }
    if (apChan >= 1 && apChan <= 14) {
        cb.setChannel(apChan);
        for (int t = 0; t < 18; t++) delay(500);   // ~9s dwell on the AP channel
    }
    cb_stop_operation();   // same FB1-pollution reason as th_cmd_ap_scan above

    // Selection. When a MAC is given, select the AP the STATION is actually
    // LINKED to (its real association) -- NOT the AP that merely matches the
    // SSID string. The badge often makes a duplicate AP entry: it learns a
    // BSSID from data frames (and links the station to THAT entry) before it
    // decodes the SSID into a separate named entry. The station-targeted deauth
    // walks the selected AP's own station list, so we must select the entry the
    // station hangs off of. Its bssid/channel are correct regardless of essid.
    int apSel = -1, staSel = -1, staCount = cb.getStationCount();
    char staMac[18] = "";
    if (macWant.length() > 0) {
        for (int i = 0; i < staCount; i++) {
            CBStationInfo st;
            if (!cb.getStation(i, st)) continue;
            char m[18];
            snprintf(m, sizeof(m), "%02x:%02x:%02x:%02x:%02x:%02x",
                     st.mac[0], st.mac[1], st.mac[2], st.mac[3], st.mac[4], st.mac[5]);
            if (macWant != String(m)) continue;
            if (st.apIndex >= 0) { cb.selectAP(st.apIndex); apSel = st.apIndex; }
            cb.selectStation(i);
            staSel = i;
            strncpy(staMac, m, sizeof(staMac) - 1);
            break;
        }
    }
    // Fallback (no MAC given, or the MAC's station wasn't linked): SSID -> AP ->
    // first station under it.
    if (staSel < 0) {
        int apCount = cb.getAPCount();
        for (int i = 0; i < apCount && apSel < 0; i++) {
            CBAccessPointInfo ap;
            if (!cb.getAP(i, ap)) continue;
            ap.essid[sizeof(ap.essid) - 1] = '\0';
            if (String(ap.essid) == ssid) { cb.selectAP(i); apSel = i; }
        }
        for (int i = 0; i < staCount && apSel >= 0; i++) {
            CBStationInfo st;
            if (!cb.getStation(i, st) || st.apIndex != apSel) continue;
            char m[18];
            snprintf(m, sizeof(m), "%02x:%02x:%02x:%02x:%02x:%02x",
                     st.mac[0], st.mac[1], st.mac[2], st.mac[3], st.mac[4], st.mac[5]);
            cb.selectStation(i);
            staSel = i;
            strncpy(staMac, m, sizeof(staMac) - 1);
            break;
        }
    }

    // Report the selected AP's channel: a station linked from data frames can
    // land on a duplicate AP entry whose channel wasn't set by a beacon. If it's
    // 0/wrong, the targeted deauth transmits on the wrong channel and misses.
    // Fix: if the linked entry's channel is unset, copy it from a same-BSSID
    // beacon-named entry so the deauth targets the right channel.
    int selChan = 0;
    if (apSel >= 0) {
        CBAccessPointInfo sap;
        if (cb.getAP(apSel, sap)) {
            selChan = sap.channel;
            if (selChan < 1 || selChan > 14) {
                // Find a same-BSSID entry that has a valid channel and re-target it.
                int apCnt = cb.getAPCount();
                for (int i = 0; i < apCnt; i++) {
                    CBAccessPointInfo o;
                    if (i == apSel || !cb.getAP(i, o)) continue;
                    if (memcmp(o.bssid, sap.bssid, 6) == 0 && o.channel >= 1 && o.channel <= 14) {
                        cb.setChannel(o.channel);   // park the radio on the real channel
                        selChan = o.channel;
                        break;
                    }
                }
            }
        }
    }

    char buf[224];
    snprintf(buf, sizeof(buf),
             "\"ap\":\"%s\",\"ap_sel\":%d,\"ap_chan\":%d,\"sta_count\":%d,\"sta_sel\":%d,\"sta_mac\":\"%s\"",
             ssid.c_str(), apSel, selChan, staCount, staSel, staMac);
    th_send_ok_json("sta_select", buf);
}

static void th_cmd_sta_list(const String &args) {
    // Diagnostic: dump detected stations (MAC + which AP it's linked to; apIndex
    // -1 = unassociated). Capped at 10 to stay under the CDC TX buffer (a ~2KB
    // printf overruns it and the line is dropped -> harness timeout). Optional
    // arg = a MAC substring filter, so a specific client can always be found
    // even in a crowded list.
    String want = args; want.trim(); want.toLowerCase();
    int n = cb.getStationCount();
    String out = "\"count\":" + String(n) + ",\"stations\":[";
    int shown = 0;
    for (int i = 0; i < n && shown < 10; i++) {
        CBStationInfo st;
        if (!cb.getStation(i, st)) continue;
        char m[18];
        snprintf(m, sizeof(m), "%02x:%02x:%02x:%02x:%02x:%02x",
                 st.mac[0], st.mac[1], st.mac[2], st.mac[3], st.mac[4], st.mac[5]);
        if (want.length() > 0 && String(m).indexOf(want) < 0) continue;
        if (shown) out += ",";
        out += "{\"mac\":\"" + String(m) + "\",\"ap\":" + String(st.apIndex) + "}";
        shown++;
    }
    out += "]";
    th_send_ok_json("sta_list", out.c_str());
}

static void th_cmd_ap_list(const String &args) {
    // Diagnostic (READ-ONLY, no scan, no state change): dump the AP list's
    // MEMBERSHIP -- index, essid, BSSID, channel, rssi, packets, selected. The
    // count alone can't answer "one entry or a wrong-channel DUPLICATE for this
    // BSSID?" -- the station-directed tools target the linked entry's channel,
    // so a same-BSSID duplicate learned from data frames at a stale channel makes
    // them miss. `sta_select` reports only the SELECTED entry's channel and
    // echoes the input SSID, so it can't tell a same-BSSID duplicate from a
    // mis-link to an unrelated AP -- this can. The stock `list -a` CLI prints
    // essid but no BSSID AND its plain-text output is dropped by test_bridge
    // (non-STX lines), so it is unreadable from automation; this is the JSON
    // reader. Optional arg = an essid OR bssid substring filter. Capped at 12
    // like sta_list/bt_list: a ~2KB printf overruns the CDC TX buffer.
    String want = args; want.trim(); want.toLowerCase();
    int n = cb.getAPCount();
    String out = "\"count\":" + String(n) + ",\"aps\":[";
    int shown = 0;
    for (int i = 0; i < n && shown < 12; i++) {
        CBAccessPointInfo ap;
        if (!cb.getAP(i, ap)) continue;
        ap.essid[sizeof(ap.essid) - 1] = '\0';
        char b[18];
        snprintf(b, sizeof(b), "%02x:%02x:%02x:%02x:%02x:%02x",
                 ap.bssid[0], ap.bssid[1], ap.bssid[2],
                 ap.bssid[3], ap.bssid[4], ap.bssid[5]);
        // Untrusted essid: an unescaped quote/backslash makes the line
        // unparseable, which a test reads as a rig failure. Escape both.
        String essid = String(ap.essid);
        essid.replace("\\", "\\\\");
        essid.replace("\"", "\\\"");
        String lc_e = essid; lc_e.toLowerCase();
        if (want.length() > 0 &&
            lc_e.indexOf(want) < 0 && String(b).indexOf(want) < 0) continue;
        if (shown) out += ",";
        out += "{\"i\":" + String(i) +
               ",\"essid\":\"" + essid + "\"" +
               ",\"bssid\":\"" + String(b) + "\"" +
               ",\"ch\":" + String(ap.channel) +
               ",\"rssi\":" + String(ap.rssi) +
               ",\"pkts\":" + String(ap.packets) +
               ",\"sel\":" + String(ap.selected ? 1 : 0) + "}";
        shown++;
    }
    out += "]";
    th_send_ok_json("ap_list", out.c_str());
}

static void th_cmd_mac_track(const String &args) {
    // Diagnostic (READ-ONLY): dump the MAC-Tracker top-talkers table (Monitor >
    // MAC Tracker / DETECT_FOLLOW). Mirrors the shipping cb_poll_mac_tracker
    // (ui_nav.h:4245): getMACTrackerTop10 scans the static POD array mac_entries[]
    // (WiFiScan.h:733 -- NOT a LinkedList) and COPIES winners into this stack
    // buffer. That is the SAME unsynchronised read the shipping status-bar poller
    // (ui_nav.h:2856) already does from this loop thread, so it is race/UAF-safe:
    // no heap, no node cache, worst case a benign torn uint16 frame_count. Optional
    // arg = MAC substring filter. Proves MAC Tracker RECEIVES: an injected emitter's
    // src MAC must appear with frames>0 (vs a fabricated MAC that never does).
    String want = args; want.trim(); want.toLowerCase();
    MacEntry top[10];
    uint8_t k = cb.getMACTrackerTop10(top, MacSortMode::MOST_FRAMES);
    String out = "\"count\":" + String(k) + ",\"macs\":[";
    int shown = 0;
    for (int i = 0; i < k && shown < 10; i++) {
        char m[18];
        snprintf(m, sizeof(m), "%02x:%02x:%02x:%02x:%02x:%02x",
                 top[i].mac[0], top[i].mac[1], top[i].mac[2],
                 top[i].mac[3], top[i].mac[4], top[i].mac[5]);
        if (want.length() > 0 && String(m).indexOf(want) < 0) continue;
        if (shown) out += ",";
        out += "{\"mac\":\"" + String(m) + "\",\"frames\":" + String(top[i].frame_count) +
               ",\"rssi\":" + String(top[i].rssi) + "}";
        shown++;
    }
    out += "]";
    th_send_ok_json("mac_track", out.c_str());
}

static void th_cmd_bt_list() {
    // Diagnostic: dump the BT device list's MEMBERSHIP, not just its size. The count alone
    // cannot distinguish evict-oldest from drop-new once the list is full -- both pin the size
    // at the cap forever. Identities can: with eviction, a MAC absent from the first full
    // sample turns up in a later one, so the union across samples exceeds the cap. With
    // drop-new the membership freezes on whatever was seen first. See test_bt_eviction.py.
    // Capped like sta_list: a ~2KB printf overruns the CDC TX buffer and the line is dropped.
    int n = cb.getBTDeviceCount();
    String out = "\"count\":" + String(n) + ",\"devices\":[";
    int shown = 0;
    for (int i = 0; i < n && shown < 12; i++) {
        CBBTDeviceInfo d;
        if (!cb.getBTDevice(i, d)) continue;
        // BLE names are untrusted. An unescaped quote or backslash makes this line
        // unparseable, which a test reads as a rig failure rather than as the hostile name it
        // is; a raw control char can also truncate the reply. Escape, and drop anything the
        // parser cannot carry -- the MAC is what this test keys on, the name is a convenience.
        String nm;
        for (const char *p = d.name; *p; p++) {
            char c = *p;
            if (c == '"' || c == '\\') { nm += '\\'; nm += c; }
            else if ((uint8_t)c >= 0x20 && (uint8_t)c < 0x7f) nm += c;
            else nm += '.';
        }
        if (shown) out += ",";
        out += "{\"mac\":\"" + String(d.mac) + "\",\"name\":\"" + nm +
               "\",\"rssi\":" + String(d.rssi) + "}";
        shown++;
    }
    out += "],\"shown\":" + String(shown);
    th_send_ok_json("bt_list", out.c_str());
}

extern WiFiScan wifi_scan_obj;   // global Marauder scan object (ClipBoyMarauder.cpp)

#ifdef CLIPBOY_RES34RCH   // Station-directed active-TX TEST cmds. They call res34rch-only WiFiScan
                          // senders (sendEapolBagMsg1/sendAssociationSleep/sendDeauthFrame), which
                          // are compiled out of Sn34k -> a Sn34k --test build fails to LINK without
                          // this gate (found 2026-07-30 building sn34k --test from staging).
static void th_cmd_deauth_sta(const String &args) {
    // Directed station deauth, bypassing the AP list/dedup entirely.
    // Usage: deauth_sta <client_mac> <ap_bssid> <chan>
    // Spoofs <ap_bssid> as the source and deauths <client_mac> on <chan>. Uses
    // deauthManual (the manual directed-deauth mode), whose main loop calls
    // sendDeauthFrame(src_mac, set_channel, dst_mac) -- so we drive it by MAC +
    // channel directly. This sidesteps the duplicate-AP-entry wrong-channel bug
    // that made the AP-list-based targeted deauth (6,2) miss.
    char dst[20] = {0}, bss[20] = {0}; int chan = 0;
    if (sscanf(args.c_str(), "%19s %19s %d", dst, bss, &chan) < 3) {
        th_send_err("deauth_sta", "usage: deauth_sta <client_mac> <ap_bssid> <chan>");
        return;
    }
    if (chan < 1 || chan > 14) { th_send_err("deauth_sta", "chan must be 1-14"); return; }
    uint8_t sm[6];
    if (sscanf(bss, "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx",
               &sm[0], &sm[1], &sm[2], &sm[3], &sm[4], &sm[5]) != 6) {
        th_send_err("deauth_sta", "bad ap_bssid (aa:bb:cc:dd:ee:ff)"); return;
    }
    if (cb_op_running) cb_stop_operation();
    cb_ensure_wifi();
    // Start manual deauth. dispatch (cat 6) brings up APSTA so WIFI_IF_AP TX works.
    dispatch_clipboy_action(6, 1);   // deauthManual
    cb_op_running = true;
    cb_op_name = "Deauth (station)";
    if (lbl_stask) lv_label_set_text(lbl_stask, cb_op_name);
    // Set the directed params AFTER start (the tool's start path + setMac already ran, so
    // they can't clobber these). The per-frame sendDeauthFrame re-parks the radio
    // to set_channel each call, so this holds even mid-run.
    wifi_scan_obj.dst_mac = String(dst);
    memcpy(wifi_scan_obj.src_mac, sm, 6);
    wifi_scan_obj.set_channel = (uint8_t)chan;
    wifi_scan_obj.changeChannel(chan);
    char buf[128];
    snprintf(buf, sizeof(buf), "\"client\":\"%s\",\"bssid\":\"%s\",\"chan\":%d", dst, bss, chan);
    th_send_ok_json("deauth_sta", buf);
}

// Shared helper: parse "<client_mac> <ap_bssid> <chan> [count]" -> dm/bm/chan/count.
// Returns "" on success or an error string.
static const char *th_parse_sta_dir(const String &args, uint8_t dm[6], uint8_t bm[6],
                                    int &chan, int &count, int defCount) {
    char dst[20] = {0}, bss[20] = {0}; chan = 0; count = defCount;
    int got = sscanf(args.c_str(), "%19s %19s %d %d", dst, bss, &chan, &count);
    if (got < 3) return "usage: <client_mac> <ap_bssid> <chan> [count]";
    if (chan < 1 || chan > 14) return "chan must be 1-14";
    if (sscanf(dst, "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx",
               &dm[0], &dm[1], &dm[2], &dm[3], &dm[4], &dm[5]) != 6) return "bad client_mac";
    if (sscanf(bss, "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx",
               &bm[0], &bm[1], &bm[2], &bm[3], &bm[4], &bm[5]) != 6) return "bad ap_bssid";
    if (count < 1) count = 1;
    if (count > 5000) count = 5000;
    return "";
}

static void th_cmd_badmsg_sta(const String &args) {
    // Directed malformed-EAPOL (Flood > Bad Msg) to ONE client, EXPLICIT params. Mirrors
    // deauth_sta, and exists for the same reason: the AP-list path (Bad Msg Target 7,2)
    // links the client to a stale-channel DUPLICATE AP entry (ch6 vs the real ch11) and
    // transmits on the wrong channel -> misses. This bursts sendEapolBagMsg1 directly with
    // the channel we pass, so there is no AP-entry channel to be wrong.
    // Usage: badmsg_sta <client_mac> <ap_bssid> <chan> [count]
    uint8_t dm[6], bm[6]; int chan, count;
    const char *err = th_parse_sta_dir(args, dm, bm, chan, count, 1200);
    if (err[0]) { th_send_err("badmsg_sta", err); return; }
    if (cb_op_running) cb_stop_operation();
    cb_ensure_wifi();
    cb.setRawTxMode(true);   // APSTA up: sendEapolBagMsg1 uses esp_wifi_80211_tx(WIFI_IF_AP)
    for (int i = 0; i < count; i++)
        wifi_scan_obj.txBadMsgDirected(bm, chan, dm);
    cb.setRawTxMode(false);  // restore STA-only (as cb_stop_operation would)
    char buf[128];
    snprintf(buf, sizeof(buf), "\"client\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"chan\":%d,\"sent\":%d",
             dm[0], dm[1], dm[2], dm[3], dm[4], dm[5], chan, count);
    th_send_ok_json("badmsg_sta", buf);
}

static void th_cmd_sleep_sta(const String &args) {
    // Directed power-save (Flood > Sleep) to ONE client, EXPLICIT params. Same rationale as
    // badmsg_sta. sendAssociationSleep embeds an ESSID in the frame, so it is taken as the
    // 5th token; defaults to the empty string if omitted.
    // Usage: sleep_sta <client_mac> <ap_bssid> <chan> <count> [essid]
    uint8_t dm[6], bm[6]; int chan, count;
    char dst[20] = {0}, bss[20] = {0}, essid[36] = {0};
    int got = sscanf(args.c_str(), "%19s %19s %d %d %35s", dst, bss, &chan, &count, essid);
    if (got < 4) { th_send_err("sleep_sta", "usage: sleep_sta <client_mac> <ap_bssid> <chan> <count> [essid]"); return; }
    const char *err = th_parse_sta_dir(args, dm, bm, chan, count, 1200);
    if (err[0]) { th_send_err("sleep_sta", err); return; }
    if (cb_op_running) cb_stop_operation();
    cb_ensure_wifi();
    cb.setRawTxMode(true);
    for (int i = 0; i < count; i++)
        wifi_scan_obj.txSleepDirected(essid, bm, chan, dm);
    cb.setRawTxMode(false);
    char buf[160];
    snprintf(buf, sizeof(buf),
             "\"client\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"chan\":%d,\"sent\":%d,\"essid\":\"%s\"",
             dm[0], dm[1], dm[2], dm[3], dm[4], dm[5], chan, count, essid);
    th_send_ok_json("sleep_sta", buf);
}
#endif // CLIPBOY_RES34RCH -- station-directed active-TX test cmds

// ─── SD card operations ───────────────────────────────────────────────────

static void th_cmd_sd_list(const String &args) {
    String path = args.length() > 0 ? args : "/";
    path.trim();
    if (!coll_sd_available) {
        coll_init_sd();
    }
    if (!coll_sd_available) {
        th_send_err("sd_list", "SD card not available");
        return;
    }
    File dir = SD.open(path.c_str());
    if (!dir || !dir.isDirectory()) {
        th_send_err("sd_list", "not a directory");
        if (dir) dir.close();
        return;
    }
    // Build file list as single-line JSON (avoids multi-line parsing issues)
    String json = "\"files\":[";
    bool first = true;
    int count = 0;
    File f = dir.openNextFile();
    while (f && count < 50) {  // Cap at 50 entries to avoid buffer overflow
        if (!first) json += ",";
        json += "{\"name\":\"";
        json += f.name();
        json += "\",\"size\":";
        json += String((unsigned long)f.size());
        json += ",\"dir\":";
        json += f.isDirectory() ? "true" : "false";
        json += "}";
        first = false;
        count++;
        f = dir.openNextFile();
    }
    dir.close();
    json += "],\"count\":";
    json += String(count);
    th_send_ok_json("sd_list", json.c_str());
}

static void th_cmd_sd_read(const String &args) {
    String path = args;
    path.trim();
    if (!coll_sd_available) {
        th_send_err("sd_read", "SD card not available");
        return;
    }
    File f = SD.open(path.c_str(), "r");
    if (!f) {
        th_send_err("sd_read", "file not found");
        return;
    }
    size_t sz = f.size();
    if (sz > 64 * 1024) {
        f.close();
        th_send_err("sd_read", "file too large (max 64KB)");
        return;
    }
    // Read first 256 bytes as preview (for text files)
    char preview[257] = {0};
    size_t preview_len = f.read((uint8_t *)preview, 256);
    f.close();
    // Escape preview for JSON
    String escaped = "";
    for (size_t i = 0; i < preview_len; i++) {
        char c = preview[i];
        if (c == '"') escaped += "\\\"";
        else if (c == '\\') escaped += "\\\\";
        else if (c == '\n') escaped += "\\n";
        else if (c == '\r') escaped += "\\r";
        else if (c == '\t') escaped += "\\t";
        else if ((uint8_t)c < 0x20) continue;  // skip control chars
        else escaped += c;
    }
    String json = "\"path\":\"" + String(path) + "\",\"size\":" + String((unsigned long)sz) +
                  ",\"preview\":\"" + escaped + "\"";
    th_send_ok_json("sd_read", json.c_str());
}

static void th_cmd_sd_write(const String &args) {
    // Format: "path content..." — first token is path, rest is content
    int sep = args.indexOf(' ');
    if (sep <= 0) {
        th_send_err("sd_write", "usage: sd_write /path content");
        return;
    }
    String path = args.substring(0, sep);
    String content = args.substring(sep + 1);
    path.trim();

    if (!coll_sd_available) {
        th_send_err("sd_write", "SD card not available");
        return;
    }
    File f = SD.open(path.c_str(), "w");
    if (!f) {
        th_send_err("sd_write", "cannot create file");
        return;
    }
    f.print(content);
    f.close();
    char hdr[128];
    snprintf(hdr, sizeof(hdr), "\"path\":\"%s\",\"bytes\":%d",
             path.c_str(), (int)content.length());
    th_send_ok_json("sd_write", hdr);
}

static void th_cmd_sd_exists(const String &args) {
    String path = args;
    path.trim();
    if (path.length() == 0) {
        th_send_err("sd_exists", "usage: sd_exists /path");
        return;
    }
    if (!coll_sd_available) {
        th_send_err("sd_exists", "SD card not available");
        return;
    }
    // SD.exists() can be slow or hang for some paths — use File open as alternative
    File f = SD.open(path.c_str(), "r");
    bool exists = (f != false);
    if (f) f.close();
    char hdr[128];
    snprintf(hdr, sizeof(hdr), "\"path\":\"%s\",\"exists\":%s",
             path.c_str(), exists ? "true" : "false");
    th_send_ok_json("sd_exists", hdr);
}

static void th_cmd_sd_rm(const String &args) {
    String path = args;
    path.trim();
    if (path.length() == 0) {
        th_send_err("sd_rm", "usage: sd_rm /path");
        return;
    }
    if (!coll_sd_available) {
        th_send_err("sd_rm", "SD card not available");
        return;
    }
    bool ok = SD.remove(path.c_str());
    char hdr[128];
    snprintf(hdr, sizeof(hdr), "\"path\":\"%s\",\"removed\":%s",
             path.c_str(), ok ? "true" : "false");
    th_send_ok_json("sd_rm", hdr);
}

// Stream an MP3 from the SD card via the streaming decoder (DC34-134 bring-up).
// Usage: mp3stream /path.mp3 [loop]  |  mp3stream stop
static void th_cmd_mp3stream(const String &args) {
    String a = args; a.trim();
    if (a == "stop") {
        audio_mp3_stream_stop();
        th_send_ok_json("mp3stream", "\"stopped\":true");
        return;
    }
    if (a == "numbers") {   // play clip 1-1 (πr8 r4di0) via the PROGMEM manifest
        for (int i = 0; i < g_radio_clip_count; i++)
            if (!strcmp(g_radio_clips[i].name, "1-1")) {
                audio_mp3_stream_play_progmem(g_radio_clips[i].data, g_radio_clips[i].len, true);
                break;
            }
        th_send_ok_json("mp3stream", "\"source\":\"progmem:1-1\",\"loop\":true");
        return;
    }
    bool loop = false;
    if (a.endsWith(" loop")) { loop = true; a = a.substring(0, a.length() - 5); a.trim(); }
    if (a.length() == 0) {
        th_send_err("mp3stream", "usage: mp3stream /path.mp3 [loop] | mp3stream stop");
        return;
    }
    if (!coll_sd_available) { th_send_err("mp3stream", "SD card not available"); return; }
    File f = SD.open(a.c_str(), "r");
    if (!f) { th_send_err("mp3stream", "open failed"); return; }
    f.close();
    audio_mp3_stream_play_file(SD, a.c_str(), loop);
    char hdr[160];
    snprintf(hdr, sizeof(hdr), "\"path\":\"%s\",\"loop\":%s", a.c_str(), loop ? "true" : "false");
    th_send_ok_json("mp3stream", hdr);
}

static void th_cmd_hr_scan_start() {
    if (hr_scanning) {
        th_send_ok_json("hr_scan_start", "\"already\":true");
        return;
    }
    // Call the same function the UI SCAN button calls
    hr_scan_start();
    // Pump LVGL to let the deferred begin timer fire (50ms)
    for (int i = 0; i < 5; i++) {
        lv_timer_handler();
        delay(20);
    }
    th_send_ok_json("hr_scan_start", hr_scanning ? "\"active\":true" : "\"active\":false");
}

// Dismiss a lingering scan/success modal (the sweep harness sends this between
// tags so successive locks don't STACK modals -> heap exhaustion / crash). This
// is a TEST-ONLY concern: real users must tap the modal to dismiss it before they
// can start another scan, so modals can never stack in normal use.
static void th_cmd_hr_dismiss() {
    hr_scan_finish_modal();
    for (int i = 0; i < 3; i++) { lv_timer_handler(); delay(10); }
    th_send_ok("hr_dismiss");
}

static void th_cmd_hr_scan_stop() {
    if (hr_scanning) {
        hr_scan_stop();
    }
    th_send_ok("hr_scan_stop");
}

static void th_cmd_theremin_start() {
    if (vl53_initialized || audio_theremin_is_active()) {
        th_send_ok_json("theremin_start", "\"already\":true");
        return;
    }
    // Same path the UI Enable button calls (claims the VL53L5CX, stops any
    // running tool/geiger/HR scan via the mutual-exclusion teardown).
    bool ok = theremin_enable();
    th_send_ok_json("theremin_start", ok ? "\"active\":true" : "\"active\":false");
}

static void th_cmd_theremin_stop() {
    theremin_disable();   // self-guards; safe when not active
    th_send_ok("theremin_stop");
}

static void th_cmd_hr_feed(const String &args) {
    // Feed mock VL53L5CX data to the HR scanner (if scanning).
    // Format: 64 comma-separated distance values
    if (!hr_scanning) {
        th_send_err("hr_feed", "HR scanner not active");
        return;
    }
    // Check if scanner has already stopped (lock or timeout)
    if (!hr_scanner.isEnabled()) {
        th_send_err("hr_feed", "scanner stopped (lock or timeout)");
        return;
    }
    VL53L5CX_ResultsData mock_data;
    memset(&mock_data, 0, sizeof(mock_data));
    int idx = 0;
    int pos = 0;
    while (idx < 64 && pos < (int)args.length()) {
        int val = 0;
        bool neg = false;
        if (args[pos] == '-') { neg = true; pos++; }
        while (pos < (int)args.length() && args[pos] >= '0' && args[pos] <= '9') {
            val = val * 10 + (args[pos] - '0');
            pos++;
        }
        if (neg) val = -val;
        mock_data.distance_mm[idx] = (int16_t)val;
        mock_data.target_status[idx] = 5;
        mock_data.nb_target_detected[idx] = 1;
        idx++;
        if (pos < (int)args.length() && args[pos] == ',') pos++;
    }
    hr_scanner.feedMockData(mock_data);
    th_send_ok("hr_feed");
}

// Toggle the HR spec v2 anchor/fiducial decoder (DC34-155) at runtime, so the
// anchor test prints can be scanned without a rebuild. `hr_anchor 1` = anchor
// spec, `hr_anchor 0` = legacy. Default off.
static void th_cmd_hr_anchor(const String &args) {
    bool en = (args.toInt() != 0);
    hr_scanner.setUseAnchor(en);
    char buf[24];
    snprintf(buf, sizeof(buf), "\"anchor\":%s", en ? "true" : "false");
    th_send_ok_json("hr_anchor", buf);
}

// A/B the lock policy without reflashing: `hr_lockpolicy 1` = window-plurality
// vote (default), `hr_lockpolicy 0` = legacy consecutive-run + 2-clean gate.
// Anchor mode only. Used by scripts/hr_spec/sweep_harness.py for the old-vs-new run.
static void th_cmd_hr_lockpolicy(const String &args) {
    bool en = (args.toInt() != 0);
    hr_scanner.setVoteLock(en);
    char buf[24];
    snprintf(buf, sizeof(buf), "\"votelock\":%s", en ? "true" : "false");
    th_send_ok_json("hr_lockpolicy", buf);
}

// Pin the guard corner for fixed-orientation tags: `hr_guard 2` = BL, 0=TL 1=TR
// 3=BR, -1 = legacy weakest-blob. Scan a known-good tag with `hr_debug` to read
// weakest= for the sensor-frame guard corner, then pin it here.
static void th_cmd_hr_guard(const String &args) {
    int corner = args.length() ? args.toInt() : -1;
    hr_scanner.setFixedGuardCorner(corner);
    char buf[24];
    snprintf(buf, sizeof(buf), "\"guard_pin\":%d", corner);
    th_send_ok_json("hr_guard", buf);
}

// Dump the last processFrame's intermediates (sampled bits, decode status,
// threshold, per-cell votes, 8x8 view) to Serial for scan diagnosis. Hold the
// tag, then run this; capture with scripts/hr_spec/hr_debug.py (the raw multi-
// line dump isn't STX-framed, so the bridge won't relay it).
static void th_cmd_hr_debug() {
    Serial.println(F("--- HR_DEBUG_BEGIN ---"));
    hr_scanner.dumpDebug(Serial);
    Serial.println(F("--- HR_DEBUG_END ---"));
    th_send_ok("hr_debug");
}
// Flat-field sensor calibration: aim at a flat matte surface ~3in away, then
// send hr_calibrate. It starts a brief scan, averages the flat field, stores the
// per-zone baseline to NVS, and prints "[CAL] saved..." from the loop ~2.5s later.
static void th_cmd_hr_calibrate() {
    hr_cal_start();
    th_send_ok("hr_calibrate");
}
static void th_cmd_hr_cal_clear() {
    hr_cal_clear();
    th_send_ok("hr_cal_clear");
}

// Manual-entry decode (feature/manual-entry-grid, Task 1). Decode a hand-entered
// user-view 4x4 tag into a collectible id via HRScan::Engine::decodeUserGrid,
// reusing the anchor/SECDED decoder. Arg = 16 '0'/'1' chars, ROW-MAJOR
// (r0c0 r0c1 r0c2 r0c3 r1c0 ...), '1' = raised bump in the orientation the user
// SEES the tag (TL/TR/BR corners raised, BL flat).
// Prints "manual_decode: id=<n>" on success, or "manual_decode: invalid".
static void th_cmd_manual_decode(const String &args) {
    String s = args; s.trim();
    // Emit an STX+JSON result (harness-readable); id=-1 on any invalid input.
    auto emit = [](int id) {
        Serial.printf("%c{\"ok\":%s,\"cmd\":\"manual_decode\",\"id\":%d}\n",
                      TH_STX, id >= 0 ? "true" : "false", id);
    };
    if (s.length() != 16) { emit(-1); return; }
    bool grid[4][4];
    for (int i = 0; i < 16; i++) {
        char ch = s[i];
        if (ch != '0' && ch != '1') { emit(-1); return; }
        grid[i / 4][i % 4] = (ch == '1');
    }
    emit(HRScan::Engine::decodeUserGrid(grid));
}

// Manual-entry grid dialog (feature/manual-entry-grid, Task 2). Opens the
// full-screen grid over serial so we can eyeball + tap it. No arg -> blank grid
// ("MANUAL ENTRY"). 16 '0'/'1' chars (row-major USER-VIEW) -> pre-filled grid
// ("CONFIRM SCAN"). Corners are forced by show_manual_grid regardless of prefill.
static void th_cmd_manual_grid(const String &args) {
    String s = args; s.trim();
    if (s.length() == 0) {
        show_manual_grid(NULL, false);
        th_send_ok("manual_grid");   // cmd field must match the sent cmd (bridge matching)
        return;
    }
    if (s.length() != 16) {
        th_send_err("manual_grid", "need 0 or 16 chars");
        return;
    }
    bool prefill[16];
    for (int i = 0; i < 16; i++) {
        char ch = s[i];
        if (ch != '0' && ch != '1') { th_send_err("manual_grid", "chars must be 0/1"); return; }
        prefill[i] = (ch == '1');
    }
    show_manual_grid(prefill, true);
    th_send_ok("manual_grid");   // cmd field must match the sent cmd (bridge matching)
}

// ─── Config (cfg_get / cfg_set) ──────────────────────────────────────────
// Scalar settings with individual NVS save paths. Blob fields (leds,
// theremin_voices) aren't exposed here — they'd need per-index commands.

static bool th_cfg_parse_bool(const String &s, bool &out) {
    String v = s; v.trim(); v.toLowerCase();
    if (v == "true" || v == "1" || v == "on")  { out = true;  return true; }
    if (v == "false" || v == "0" || v == "off") { out = false; return true; }
    return false;
}

static void th_cmd_cfg_get(const String &args) {
    String key = args; key.trim();
    char hdr[96];
    if (key == "theme")      snprintf(hdr, sizeof(hdr), "\"key\":\"theme\",\"value\":%d", cfg.theme);
    else if (key == "brightness") snprintf(hdr, sizeof(hdr), "\"key\":\"brightness\",\"value\":%d", cfg.brightness);
    else if (key == "volume") snprintf(hdr, sizeof(hdr), "\"key\":\"volume\",\"value\":%d", cfg.volume);
    else if (key == "disp_off") snprintf(hdr, sizeof(hdr), "\"key\":\"disp_off\",\"value\":%d", cfg.disp_off);
    else if (key == "airplane") snprintf(hdr, sizeof(hdr), "\"key\":\"airplane\",\"value\":%s", cfg.airplane ? "true" : "false");
    else if (key == "sound")    snprintf(hdr, sizeof(hdr), "\"key\":\"sound\",\"value\":%s", cfg.sound ? "true" : "false");
    else if (key == "ui_click") snprintf(hdr, sizeof(hdr), "\"key\":\"ui_click\",\"value\":%s", cfg.ui_click ? "true" : "false");
    else if (key == "ss_style") snprintf(hdr, sizeof(hdr), "\"key\":\"ss_style\",\"value\":%d", cfg.ss_style);
    else if (key == "ss_leds")  snprintf(hdr, sizeof(hdr), "\"key\":\"ss_leds\",\"value\":%s", cfg.ss_leds_off ? "true" : "false");
    else if (key == "ss_clock") snprintf(hdr, sizeof(hdr), "\"key\":\"ss_clock\",\"value\":%s", cfg.ss_clock ? "true" : "false");
    else if (key == "tz")       snprintf(hdr, sizeof(hdr), "\"key\":\"tz\",\"value\":%d", cfg.tz);
    else if (key == "crt_scan") snprintf(hdr, sizeof(hdr), "\"key\":\"crt_scan\",\"value\":%s", cfg.crt_scanlines ? "true" : "false");
    else if (key == "crt_flick") snprintf(hdr, sizeof(hdr), "\"key\":\"crt_flick\",\"value\":%s", cfg.crt_flicker ? "true" : "false");
    else if (key == "allow_pcap") snprintf(hdr, sizeof(hdr), "\"key\":\"allow_pcap\",\"value\":%s", cfg.allow_pcap ? "true" : "false");
    else { th_send_err("cfg_get", "unknown key"); return; }
    th_send_ok_json("cfg_get", hdr);
}

static void th_cmd_cfg_set(const String &args) {
    int space = args.indexOf(' ');
    if (space <= 0) { th_send_err("cfg_set", "usage: cfg_set key value"); return; }
    String key = args.substring(0, space); key.trim();
    String val = args.substring(space + 1); val.trim();
    int ival = val.toInt();
    bool bval;
    char hdr[96];

    if (key == "theme") {
        if (ival < 0 || ival >= NUM_THEMES) { th_send_err("cfg_set", "theme out of range"); return; }
        cfg.theme = (uint8_t)ival; cfg_save_theme();
    } else if (key == "brightness") {
        if (ival < 10 || ival > 100) { th_send_err("cfg_set", "brightness must be 10-100"); return; }
        cfg.brightness = (uint8_t)ival; cfg_save_brightness();
    } else if (key == "volume") {
        if (ival < 0 || ival > 100) { th_send_err("cfg_set", "volume must be 0-100"); return; }
        cfg.volume = (uint8_t)ival; cfg_save_volume();
        audio_set_volume(cfg.sound ? (cfg.volume / 100.0f) : 0.0f);
    } else if (key == "disp_off") {
        if (ival < 0 || ival > 5) { th_send_err("cfg_set", "disp_off must be 0-5"); return; }
        cfg.disp_off = (uint8_t)ival; cfg_save_disp_off();
    } else if (key == "airplane") {
        if (!th_cfg_parse_bool(val, bval)) { th_send_err("cfg_set", "expect bool"); return; }
        // Call the same helper the Settings switch calls, so a scripted airplane toggle runs
        // the REAL teardown (stop operations, WiFi off). Writing cfg.airplane directly made
        // every airplane test fictional -- nothing was ever stopped.
        airplane_apply(bval);
    } else if (key == "allow_pcap") {
        if (!th_cfg_parse_bool(val, bval)) { th_send_err("cfg_set", "expect bool"); return; }
        cfg.allow_pcap = bval; cfg_save_allow_pcap(); cb.setSavePCAP(cfg.allow_pcap);
    } else if (key == "exinput") {
        if (!th_cfg_parse_bool(val, bval)) { th_send_err("cfg_set", "expect bool"); return; }
        cfg.exclusive_input = bval; cfg_save_exclusive_input();
    } else if (key == "sound") {
        if (!th_cfg_parse_bool(val, bval)) { th_send_err("cfg_set", "expect bool"); return; }
        cfg.sound = bval; cfg_save_sound();
        audio_set_volume(cfg.sound ? (cfg.volume / 100.0f) : 0.0f);
    } else if (key == "ui_click") {
        if (!th_cfg_parse_bool(val, bval)) { th_send_err("cfg_set", "expect bool"); return; }
        cfg.ui_click = bval; cfg_save_ui_click();
    } else if (key == "ss_style") {
        if (ival < 0 || ival > 2) { th_send_err("cfg_set", "ss_style must be 0-2"); return; }
        cfg.ss_style = (uint8_t)ival; cfg_save_ss_style();
    } else if (key == "ss_leds") {
        if (!th_cfg_parse_bool(val, bval)) { th_send_err("cfg_set", "expect bool"); return; }
        cfg.ss_leds_off = bval; cfg_save_ss_leds_off();
    } else if (key == "ss_clock") {
        if (!th_cfg_parse_bool(val, bval)) { th_send_err("cfg_set", "expect bool"); return; }
        cfg.ss_clock = bval; cfg_save_ss_clock();
    } else if (key == "tz") {
        if (ival < 0 || ival >= CB_TZ_COUNT) { th_send_err("cfg_set", "tz out of range"); return; }
        cfg.tz = (uint8_t)ival; cfg_save_tz(); cb_tz_apply();
    } else if (key == "crt_scan") {
        if (!th_cfg_parse_bool(val, bval)) { th_send_err("cfg_set", "expect bool"); return; }
        cfg.crt_scanlines = bval; cfg_save_crt_scanlines();
    } else if (key == "crt_flick") {
        if (!th_cfg_parse_bool(val, bval)) { th_send_err("cfg_set", "expect bool"); return; }
        cfg.crt_flicker = bval; cfg_save_crt_flicker();
    } else {
        th_send_err("cfg_set", "unknown key");
        return;
    }
    snprintf(hdr, sizeof(hdr), "\"key\":\"%s\",\"value\":\"%s\"", key.c_str(), val.c_str());
    th_send_ok_json("cfg_set", hdr);
}

// ─── Theme + LED control ─────────────────────────────────────────────────

static void th_cmd_info_show(const String &args) {
    String a = args; a.trim(); a.toLowerCase();
    if (!content_obj) { th_send_err("info_show", "no content object"); return; }
    if      (a == "credits") show_credits(content_obj);
    else if (a == "legal")   show_legal(content_obj);
    else if (a == "about")   show_about(content_obj);
    else if (a == "help")    show_help(content_obj);
    else if (a == "radio")   show_radio(content_obj);
    else if (a == "huepicker") show_hue_picker();          // custom-theme hue picker (overlay)
    else if (a == "reveal")    show_custom_reveal();        // 100%-collectibles reveal (overlay)
    else { th_send_err("info_show", "page must be credits|legal|about|help|radio|huepicker|reveal"); return; }
    char hdr[48];
    snprintf(hdr, sizeof(hdr), "\"page\":\"%s\"", a.c_str());
    th_send_ok_json("info_show", hdr);
}

static void th_cmd_theme_set(const String &args) {
    String a = args; a.trim();
    if (a.length() == 0) { th_send_err("theme_set", "usage: theme_set idx (0..NUM_THEMES-1)"); return; }
    int idx = a.toInt();
    if (idx < 0 || idx > THEME_CUSTOM) { th_send_err("theme_set", "idx out of range"); return; }
    bool changed = ui_theme_switch_live((uint8_t)idx);   // THEME_CUSTOM (6) = the completionist theme
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "\"idx\":%d,\"changed\":%s", idx, changed ? "true" : "false");
    th_send_ok_json("theme_set", hdr);
}

static void th_cmd_led_set(const String &args) {
    // Format: led_set idx r g b brightness anim speed
    int idx, r, g, b, bright, anim, speed;
    int got = sscanf(args.c_str(), "%d %d %d %d %d %d %d",
                     &idx, &r, &g, &b, &bright, &anim, &speed);
    if (got < 7) {
        th_send_err("led_set", "usage: led_set idx r g b brightness anim speed");
        return;
    }
    if (idx < 0 || idx >= CFG_NUM_LEDS) { th_send_err("led_set", "idx out of range"); return; }
    if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) { th_send_err("led_set", "rgb 0-255"); return; }
    if (bright < 0 || bright > 255) { th_send_err("led_set", "brightness 0-255"); return; }
    if (anim < 0 || anim > 2) { th_send_err("led_set", "anim 0-2"); return; }
    if (speed < 1 || speed > 10) { th_send_err("led_set", "speed 1-10"); return; }
    LedConfig *lc = &cfg.leds[idx];
    lc->r = (uint8_t)r; lc->g = (uint8_t)g; lc->b = (uint8_t)b;
    lc->brightness = (uint8_t)bright;
    lc->animation  = (uint8_t)anim;
    lc->speed      = (uint8_t)speed;
    neo_apply(idx);
    cfg_save_leds();
    char hdr[96];
    snprintf(hdr, sizeof(hdr), "\"idx\":%d,\"r\":%d,\"g\":%d,\"b\":%d", idx, r, g, b);
    th_send_ok_json("led_set", hdr);
}

// A/B: force the KITT scan-sweep render on/off independent of LED state, to test
// whether the sweep affects scan speed/accuracy. -1 = normal (gated), 0 = off, 1 = on.
static void th_cmd_led_sweep(const String &args) {
    int v = args.length() ? args.toInt() : -1;
    if (v < -1) v = -1; else if (v > 1) v = 1;
    neo_scan_sweep_force = (int8_t)v;
    char buf[32];
    snprintf(buf, sizeof(buf), "\"sweep_force\":%d", v);
    th_send_ok_json("led_sweep", buf);
}

static void th_cmd_led_preset(const String &args) {
    String a = args; a.trim();
    if (a.length() == 0) {
        th_send_err("led_preset", "usage: led_preset idx (0=Mojave, 1=RibbitCity, 2=Flashbang, 3=Rainbow, 4=Off)");
        return;
    }
    int pi = a.toInt();
    if (!led_apply_preset(pi)) {
        th_send_err("led_preset", "idx must be 0-4");
        return;
    }
    char hdr[48];
    snprintf(hdr, sizeof(hdr), "\"idx\":%d", pi);
    th_send_ok_json("led_preset", hdr);
}

static void th_cmd_reboot() {
    th_send_ok("reboot");
    Serial.flush();
    delay(100);
    ESP.restart();
}

// ─── Audio-suspend crash regression (C1) ─────────────────────────────────
// For each render source: activate it, run audio_suspend()+resume() (which ends +
// re-inits I2S), then stop it. Pre-fix, a source whose flag audio_suspend() didn't
// clear kept the core-0 task writing to the freed I2S -> reboot. A reboot = the serial
// output stops at the last source printed; reaching "ALL OK" means no race.
static void th_cmd_audio_suspend_test(const String &args) {
    (void)args;
    Serial.println(F("[SUSPTEST] begin (reboot=FAIL: note the last source printed)"));
    #define TH_SUSP_ONE(nm, startx, stopx) do {          \
        Serial.print(F("[SUSPTEST] " nm " ... "));        \
        startx; delay(60);                                 \
        audio_suspend(); delay(20); audio_resume();        \
        stopx;                                             \
        Serial.println(F("OK"));                           \
    } while (0)
    TH_SUSP_ONE("static", audio_static_start(), audio_static_stop());
    TH_SUSP_ONE("sonar",  audio_sonar_start(),  audio_sonar_stop());
    TH_SUSP_ONE("geiger", (audio_geiger_start(), audio_geiger_set_rate(3)), audio_geiger_stop());
    TH_SUSP_ONE("tone",   audio_tone_start(440.0f), audio_tone_stop());
    TH_SUSP_ONE("stream", audio_mp3_stream_play_progmem(scanning_mp3, scanning_mp3_len, false),
                          audio_mp3_stream_stop());
    #undef TH_SUSP_ONE
    Serial.println(F("[SUSPTEST] ALL OK -- no reboot with any source active during suspend"));
    // Final STX+JSON result for the harness. If any source rebooted core 0, execution
    // never reaches here -> the harness times out -> the test fails (correct).
    Serial.printf("%c{\"ok\":true,\"cmd\":\"audio_suspend_test\",\"sources\":5,\"reboot\":false}\n", TH_STX);
}

// ─── Radio announce-mask dump (#6 re-announce regression) ─────────────────
// After a bulk collectible change (coll add all / SD restore), every currently-unlocked
// GATED station must already be in cfg.radio_announced_mask so it can't spuriously
// "new station!" on the next unlock. Prints the masks + a synced verdict for a script.
static void th_cmd_radio_announce_dump(const String &args) {
    (void)args;
    uint8_t gated_unlocked = 0;
    for (int i = 0; i < (int)NUM_RADIO_STATIONS; i++)
        if (radio_stations[i].gate_count > 0 && radio_station_unlocked(i))
            gated_unlocked |= (uint8_t)(1u << i);
    bool synced = (cfg.radio_announced_mask & gated_unlocked) == gated_unlocked;
    Serial.printf("%c{\"ok\":true,\"cmd\":\"radio_announce_dump\",\"announced\":%u,"
                  "\"gated_unlocked\":%u,\"synced\":%s}\n",
                  TH_STX, cfg.radio_announced_mask, gated_unlocked, synced ? "true" : "false");
}

// ─── Capture counters + Raw channel (DC34: kalipi end-to-end EAPOL test) ─────
static void th_cmd_pkt_counters() {
    CBPacketCounters pc = cb.getPacketCounters();
    char hdr[416];
    // Both counter families: the *Frames family (raw sniffer) and the num* family
    // (Analyze/Monitor display accumulators). Different tools feed different fields,
    // so the test runner asserts against whichever one a given tool actually moves.
    snprintf(hdr, sizeof(hdr),
        "\"mgmt\":%lu,\"data\":%lu,\"beacon\":%lu,\"deauth\":%lu,\"eapol\":%lu,"
        "\"req\":%lu,\"resp\":%lu,\"numProbe\":%d,\"nbeacon\":%d,\"ndeauth\":%d,"
        "\"neapol\":%d,\"ceapol\":%lu,\"sae\":%lu,\"bt_frames\":%d",
        (unsigned long)pc.mgmtFrames, (unsigned long)pc.dataFrames,
        (unsigned long)pc.beaconFrames, (unsigned long)pc.deauthFrames,
        (unsigned long)pc.eapolFrames, (unsigned long)pc.reqFrames,
        (unsigned long)pc.respFrames, pc.numProbe, pc.numBeacon, pc.numDeauth,
        pc.numEapol, (unsigned long)pc.completeEapol, (unsigned long)pc.saeFrames,
        cb.getBTFrames());   // BLE Adverts (1.4): total adverts seen (was no readback)
    th_send_ok_json("pkt_counters", hdr);
}

// Channel Stats (2.3): per-channel activity histogram (was no serial readback).
// getChannelActivity() returns counts[14]; report the array + summary so the test
// runner can assert traffic registered on a stimulated channel.
static void th_cmd_channel_activity() {
    CBChannelActivity ca = cb.getChannelActivity();
    int total = 0, max_ch = 0, max_val = 0, active = 0;
    char arr[128]; int p = 0;
    for (int i = 0; i < 14; i++) {
        int v = ca.counts[i];
        total += v;
        if (v > 0) active++;
        if (v > max_val) { max_val = v; max_ch = i + 1; }
        p += snprintf(arr + p, sizeof(arr) - p, "%s%d", i ? "," : "", v);
    }
    char hdr[256];
    snprintf(hdr, sizeof(hdr),
        "\"counts\":[%s],\"total\":%d,\"max_ch\":%d,\"max_val\":%d,\"active_ch\":%d,\"page\":%d",
        arr, total, max_ch, max_val, active, cb.getChannelActivityPage());
    th_send_ok_json("channel_activity", hdr);
}

// Clip-Boy (DC34): detection/list counts for the kalipi tool-suite runner. AP/STA/SSID
// have no count accessor, so iterate the readers (bounded); BT/detection lists expose
// counts directly. Lets a host runner assert e.g. airtag>0, esp>0, or ssid>=1 cheaply
// without scraping the UI log. Read-only; safe to poll while a tool runs.
static void th_cmd_detect_counts() {
    int ap = 0, sta = 0, ssid = 0;
    CBAccessPointInfo apo; while (ap   < 256 && cb.getAP(ap, apo))       ap++;
    CBStationInfo     sto; while (sta  < 512 && cb.getStation(sta, sto)) sta++;
    CBSSIDInfo        sso; while (ssid < 256 && cb.getSSID(ssid, sso))   ssid++;
    // Age (ms) of the freshest Flock sighting, or -1 if nothing has been heard. This is the ONLY
    // scriptable view of the liveness fix.
    // ⚠ CORRECTED 2026-07-27: an earlier version of this note said "cb_op_running stays false"
    // under a scripted start. IT DOES NOT -- th_cmd_tool_start sets cb_op_running/cb_op_name/
    // cb_op_encoded and writes the tool NAME into lbl_stask a few lines below, which is exactly
    // what a probe observed on hardware ("Flock Safety" in the bar while flock=1). The real
    // reason the rendered age is unreachable is narrower: nothing in this file calls
    // cb_start_scan_polling(), which lives only in cb_tool_start_stop_cb and the inline Scan
    // buttons -- so cb_scan_timer is never created and cb_scan_poll_cb never runs to overwrite
    // that label. The distinction matters: the wrong diagnosis implies the fix is the dropped
    // #55 cb_tool_begin refactor, when it is actually ONE LINE here. Not taken days from ship
    // because the poller also drives cb_populate_ap_list and the "Done:" branch, so enabling it
    // would change the behaviour of every existing tool_start test.
    // So: expose the underlying value. It asserts what the firmware change actually does -- a
    // dedup hit refreshes last_seen -- and reads BOTH ways: climbs with no emitter, drops on a
    // re-sighting. The RENDERED string still needs a human tapping START.
    // ⚠ Gated on cb_op_running deliberately: flock_devices is NOT cleared by StopScan (only by
    // resetDisplayAccumulators at tool start), so without the gate this would keep climbing for
    // a tool that is not running -- a rising, live-looking number describing a dead scan, which
    // is the stale-result-store shape this project has been bitten by repeatedly.
    long flock_age = -1;
    int fcount = cb.getFlockDeviceCount();
    uint32_t newest = 0; bool have = false;
    // flock_serial: the PARSED serial of the newest sighting. Exposed because the parser's own
    // `Serial:` print (WiFiScan.cpp:1502) is UNREACHABLE as a test observable three ways over --
    // test_bridge.py:157 drops non-STX lines, :123 reset_input_buffer()s before every command, and
    // the CDC TX timeout makes the badge DROP console bytes whenever nothing is
    // draining (see cb_serial_tx_relax in Clip-Boy.ino -- that timeout is now 0 until a host reads).
    // A framed field is retryable (detect_counts is in harness _RETRYABLE) and immune to all three.
    // ⚠ This is the ONLY way to test the vendored AD parser's serial extraction from a script.
    char newest_serial[33]; newest_serial[0] = '\0';
    for (int i = 0; i < fcount; i++) {
        CBFlockInfo fi;
        if (!cb.getFlockDevice(i, fi)) continue;
        if (fi.last_seen == 0) continue;   // torn get() returns a zeroed entry AND true; see ui_nav.h
        if (!have || (int32_t)(fi.last_seen - newest) > 0) {
            newest = fi.last_seen; have = true;
            // Whitelist-copy: the parser only ever appends 'T'/'N'/digits, but a quote or backslash
            // reaching the JSON would make the reply unparseable, and an unparseable reply reads as
            // a RIG failure rather than as the parser bug this field exists to expose.
            size_t w = 0;
            for (size_t s = 0; s < sizeof(fi.serial) && fi.serial[s] && w < sizeof(newest_serial) - 1; s++) {
                char c = fi.serial[s];
                newest_serial[w++] = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                                     (c >= 'a' && c <= 'z') ? c : '?';
            }
            newest_serial[w] = '\0';
        }
    }
    if (have && cb_op_running) flock_age = (long)(millis() - newest);
    // Gated on cb_op_running for the SAME reason flock_age is: flock_devices survives a StopScan, so
    // an ungated serial is satisfiable by a PREVIOUS test arm's device with no tool running -- which
    // would make arm N pass on arm N-1's result (the stale-result-store shape, bit us 4x).
    if (!(have && cb_op_running)) newest_serial[0] = '\0';
    // probe_reqs: the HIGHEST request count across the probe-SSID list.
    // `probe` (list size) alone cannot distinguish the two states that matter once
    // CB_PROBE_SSID_LIST_CAP is reached: a list that is full but still counting hits on the
    // names it already holds, versus one that has stopped processing probes altogether. Both
    // pin `probe` at the cap. The badge's Help and README both promise the FIRST behaviour
    // ("the counts beside names already listed keep rising"), so that promise needs an
    // observable or it is an untested user-facing claim.
    // Reads both ways by construction: climbs whenever a known SSID is re-heard, and stays
    // flat if probe processing has stopped -- so it can come out bad.
    int probe_n = cb.getProbeSSIDCount();
    int probe_reqs = 0;
    for (int i = 0; i < probe_n; i++) {
        CBProbeSSIDInfo pi;
        if (cb.getProbeSSID(i, pi) && pi.requests > probe_reqs) probe_reqs = pi.requests;
    }
    char hdr[512];   // grown from 448 for probe_reqs: a truncated reply is unparseable JSON,
                     // which reads as a RIG failure rather than as whatever it was measuring.
    snprintf(hdr, sizeof(hdr),
        "\"ap\":%d,\"sta\":%d,\"ssid\":%d,\"bt\":%d,\"airtag\":%d,\"flipper\":%d,"
        "\"flock\":%d,\"flock_age\":%ld,\"flock_serial\":\"%s\","
        "\"pwn\":%d,\"probe\":%d,\"probe_reqs\":%d,\"esp\":%d,\"multissid\":%d",
        ap, sta, ssid, cb.getBTDeviceCount(), cb.getAirTagCount(), cb.getFlipperCount(),
        cb.getFlockDeviceCount(), flock_age, newest_serial,
        cb.getPwnagotchiCount(), probe_n, probe_reqs,
        cb.getPinescanCount(), cb.getMultiSSIDCount());
    th_send_ok_json("detect_counts", hdr);
}

// delay_stats [reset] -- what the sniffer delay(random(0,10)) calls actually cost.
//
// [0] = the probe path  (WiFiScan.cpp, Analyze > Probes)
// [1] = the deauth path (Analyze > Deauth AND the Geiger)
// Only those two of the nine sites are both per-frame and reachable from a UI tool.
//
// us_per_hit is the number that answers the question. Divide the WINDOW's us by the window's
// elapsed time to get the duty cycle -- that, not the hit count, is what "these delays throttle
// passive tools" would have to mean.
//
// ⚠ Read this as a DELTA across a measured window: `delay_stats reset`, run the tool, read again.
// The counters are monotonic since boot and both sites accumulate across every tool that uses
// their callback, so an absolute reading answers no question at all.
extern volatile uint32_t cb_dly_hits[2];
extern volatile uint32_t cb_dly_us[2];

static void th_cmd_delay_stats(const String &args) {
    String a = args; a.trim();
    if (a == "reset") {
        cb_dly_hits[0] = cb_dly_hits[1] = 0;
        cb_dly_us[0]   = cb_dly_us[1]   = 0;
        th_send_ok("delay_stats");
        return;
    }
    uint32_t h0 = cb_dly_hits[0], u0 = cb_dly_us[0];
    uint32_t h1 = cb_dly_hits[1], u1 = cb_dly_us[1];
    char buf[192];
    snprintf(buf, sizeof(buf),
        "\"probe_hits\":%lu,\"probe_us\":%lu,\"probe_us_per_hit\":%lu,"
        "\"deauth_hits\":%lu,\"deauth_us\":%lu,\"deauth_us_per_hit\":%lu,\"uptime_ms\":%lu",
        (unsigned long)h0, (unsigned long)u0, (unsigned long)(h0 ? u0 / h0 : 0),
        (unsigned long)h1, (unsigned long)u1, (unsigned long)(h1 ? u1 / h1 : 0),
        (unsigned long)millis());
    th_send_ok_json("delay_stats", buf);
}

// deauth_channel [mode] -- get/set the Radiation + Analyze>Deauth channel policy.
//   0 = hop 1-14 · 200 = hop 1/6/11 · 1-14 = lock that channel
//
// A SETTER is required, not just a UI control: the setting PERSISTS to NVS, so one manual
// bench change would otherwise poison every later suite run with no scripted way back to
// hop-all. Tests must state the mode they need rather than inherit whatever a human left.
//
// `live` is read from the DRIVER (esp_wifi_get_channel), not from our bookkeeping, because
// changeChannel() discards esp_wifi_set_channel()'s return value -- so set_channel records
// what we asked for, not what took effect. Asserting a lock worked requires `live`.
static void th_cmd_deauth_channel(const String &args) {
    String a = args; a.trim();
    if (a.length()) {
        long v = a.toInt();
        if (!(v == 0 || v == 200 || (v >= 1 && v <= 14))) {
            th_send_err("deauth_channel", "mode must be 0 (1-14), 200 (1/6/11), or 1-14");
            return;
        }
        cfg.deauth_chan = (uint8_t)v;
        cfg_save_deauth_chan();
        cb.setDeauthChannel((uint8_t)v);
    }
    char buf[128];
    snprintf(buf, sizeof(buf),
             "\"mode\":%u,\"radio_mode\":%u,\"live\":%u,\"running\":%s",
             (unsigned)cfg.deauth_chan, (unsigned)cb.getDeauthChannelMode(),
             (unsigned)cb.getLiveChannel(), rad_geiger_active ? "true" : "false");
    th_send_ok_json("deauth_channel", buf);
}

static void th_cmd_raw_channel(const String &args) {
    String a = args; a.trim();
    int ch = a.toInt();                 // 0 = hop (all), 1-14 = lock
    if (ch < 0 || ch > 14) { th_send_err("raw_channel", "0-14 (0=hop all)"); return; }
    raw_capture_channel = (uint8_t)ch;
    if (cb_op_running) cb.setRawCaptureChannel(raw_capture_channel);   // apply live if capturing
    char hdr[48];
    snprintf(hdr, sizeof(hdr), "\"channel\":%d", ch);
    th_send_ok_json("raw_channel", hdr);
}

// ─── Command dispatcher ──────────────────────────────────────────────────

// ─── PCAP full-directory graceful-fail test (exercises Buffer::createFile) ────
// th_pcap_limit lowers the bounded filename search so the "directory full" path is
// reachable without 1000 real files; th_pcap_fill pre-creates the colliding names;
// th_pcap_clear tidies up. Run a pcap tool (e.g. Analyze > Raw/PCAP = "packet_monitor")
// after fill+limit and the UI should show the graceful "PCAP NOT SAVED" warning.
static void th_cmd_pcap_limit(const String &args) {
    int n = args.toInt();
    if (n < 1) { th_send_err("pcap_limit", "need n>=1"); return; }
    cb_pcap_name_limit = n;
    char j[40]; snprintf(j, sizeof j, "\"limit\":%d", n);
    th_send_ok_json("pcap_limit", j);
}
static void th_cmd_pcap_fill(const String &args) {
    if (!coll_sd_available) coll_init_sd();
    if (!coll_sd_available) { th_send_err("pcap_fill", "SD not available"); return; }
    int sp = args.indexOf(' ');
    String name = (sp < 0 ? args : args.substring(0, sp)); name.trim();
    int count = (sp < 0 ? 0 : args.substring(sp + 1).toInt());
    if (name.length() == 0 || count < 1) { th_send_err("pcap_fill", "need <name> <count>"); return; }
    SD.mkdir("/pcaps");
    int made = 0;
    for (int i = 0; i < count; i++) {
        String p = "/pcaps/" + name + "_" + String(i) + ".pcap";
        File f = SD.open(p.c_str(), FILE_WRITE);
        if (f) { f.close(); made++; }
    }
    char j[56]; snprintf(j, sizeof j, "\"made\":%d,\"of\":%d", made, count);
    th_send_ok_json("pcap_fill", j);
}
static void th_cmd_pcap_clear(const String &args) {
    if (!coll_sd_available) coll_init_sd();
    if (!coll_sd_available) { th_send_err("pcap_clear", "SD not available"); return; }
    // SD.remove() is an O(N) FAT name-scan; deleting N files is O(N^2). On a huge
    // /pcaps (the 15k-file test dir) a single unbounded pass starved the loop-task WDT
    // and rebooted the badge. Bound the work per call (default 1500, override
    // `pcap_clear <max>`) and yield periodically so the caller drains it in chunks
    // instead of one WDT-tripping sweep. Reports whether files remain.
    int max = args.length() ? args.toInt() : 1500;
    if (max < 1) max = 1500;
    int removed = 0;
    bool more = false;
    File dir = SD.open("/pcaps");
    if (dir && dir.isDirectory()) {
        File f = dir.openNextFile();
        while (f) {
            String nm = f.name();
            f.close();
            if (removed >= max) { more = true; break; }
            String full = nm.startsWith("/") ? nm : (String("/pcaps/") + nm);
            if (SD.remove(full.c_str())) removed++;
            if ((removed & 0x7F) == 0) delay(1);   // feed the loop-task WDT
            f = dir.openNextFile();
        }
    }
    if (dir) dir.close();
    if (!more) SD.rmdir("/pcaps");                 // only tidy the dir once it's empty
    char j[64]; snprintf(j, sizeof j, "\"removed\":%d,\"more\":%s",
                         removed, more ? "true" : "false");
    th_send_ok_json("pcap_clear", j);
}
static void th_cmd_pcap_hint(const String &args) {
    // Force the NVS name-index so the full-dir / card-swap / pile-up paths are testable.
    // createFile now reads "seq" (falling back to the legacy "next"); write BOTH so the
    // owner can still simulate a high count and drive the O(1) pile-up warning tiers.
    Preferences p; p.begin("pcap", false);
    uint32_t n = (uint32_t)args.toInt();
    p.putULong("seq", n);
    p.putULong("next", n);
    uint32_t got = p.getULong("seq", 0);
    p.end();
    char j[48]; snprintf(j, sizeof j, "\"seq\":%lu", (unsigned long)got);
    th_send_ok_json("pcap_hint", j);
}

static void th_dispatch(const String &line) {
    // line has STX already stripped, is the JSON content minus the STX
    // We use simple string parsing instead of a JSON library
    // Expected format: {"cmd":"name","args":"..."} or {"cmd":"name"}
    // But for simplicity, we accept plain text too: "cmd args..."

    String cmd, args;
    int space = line.indexOf(' ');
    if (space > 0) {
        cmd = line.substring(0, space);
        args = line.substring(space + 1);
        args.trim();
    } else {
        cmd = line;
    }
    cmd.trim();

    if      (cmd == "ping")           th_cmd_ping();
    else if (cmd == "heap")           th_cmd_heap();
#if defined(CB_WIFI_PS_EXPERIMENT) && CB_WIFI_PS_EXPERIMENT
    else if (cmd == "wifi_ps_set")     th_cmd_wifi_ps_set(args);
#endif
    else if (cmd == "state")          th_cmd_state();
    else if (cmd == "touch")          th_cmd_touch(args);
    else if (cmd == "swipe")          th_cmd_swipe(args);
    else if (cmd == "nav")            th_cmd_nav(args);
    else if (cmd == "screenshot")     th_cmd_screenshot();
    else if (cmd == "tree")           th_cmd_tree();
    else if (cmd == "text")           th_cmd_text();
    else if (cmd == "find")           th_cmd_find(args);
    else if (cmd == "skip_boot")      th_cmd_skip_boot();
    else if (cmd == "sensor_mock")    th_cmd_sensor_mock(args);
    else if (cmd == "sensor_real")    th_cmd_sensor_real();
    else if (cmd == "audio_capture")  th_cmd_audio_capture(args);
    else if (cmd == "neopixel_state") th_cmd_neopixel_state();
    else if (cmd == "quiet")          th_cmd_quiet(args);
    else if (cmd == "fps")            th_cmd_fps(args);
    else if (cmd == "led_rate")       th_cmd_led_rate();
    else if (cmd == "tool_state")   th_cmd_tool_state();
    else if (cmd == "tool_list")    th_cmd_tool_list();
    else if (cmd == "geiger_start")   th_cmd_geiger_start();
    else if (cmd == "geiger_stop")    th_cmd_geiger_stop();
    else if (cmd == "tool_start")   th_cmd_tool_start(args);
    else if (cmd == "tool_open")    th_cmd_tool_open(args);
    else if (cmd == "tool_stop")    th_cmd_tool_stop();
    else if (cmd == "pkt_counters") th_cmd_pkt_counters();
    else if (cmd == "detect_counts") th_cmd_detect_counts();
    else if (cmd == "delay_stats")   th_cmd_delay_stats(args);
    else if (cmd == "deauth_channel") th_cmd_deauth_channel(args);
    else if (cmd == "bt_list") th_cmd_bt_list();
    else if (cmd == "channel_activity") th_cmd_channel_activity();
    else if (cmd == "raw_channel")  th_cmd_raw_channel(args);
    else if (cmd == "kb_type")        th_cmd_kb_type(args);
    else if (cmd == "kb_ok")          th_cmd_kb_ok();
    else if (cmd == "kb_cancel")      th_cmd_kb_cancel();
    else if (cmd == "ap_scan")        th_cmd_ap_scan(args);
    else if (cmd == "wifijoin")       th_cmd_wifijoin(args);
    else if (cmd == "sta_select")     th_cmd_sta_select(args);
    else if (cmd == "sta_list")       th_cmd_sta_list(args);
    else if (cmd == "ap_list")        th_cmd_ap_list(args);
    else if (cmd == "mac_track")      th_cmd_mac_track(args);
#ifdef CLIPBOY_RES34RCH
    else if (cmd == "deauth_sta")     th_cmd_deauth_sta(args);
    else if (cmd == "badmsg_sta")     th_cmd_badmsg_sta(args);
    else if (cmd == "sleep_sta")      th_cmd_sleep_sta(args);
#endif
    else if (cmd == "sd_list")        th_cmd_sd_list(args);
    else if (cmd == "sd_read")        th_cmd_sd_read(args);
    else if (cmd == "sd_write")       th_cmd_sd_write(args);
    else if (cmd == "sd_exists")      th_cmd_sd_exists(args);
    else if (cmd == "sd_rm")          th_cmd_sd_rm(args);
    else if (cmd == "pcap_limit")     th_cmd_pcap_limit(args);
    else if (cmd == "pcap_fill")      th_cmd_pcap_fill(args);
    else if (cmd == "pcap_clear")     th_cmd_pcap_clear(args);
    else if (cmd == "pcap_hint")      th_cmd_pcap_hint(args);
    else if (cmd == "cli")            th_cmd_cli(args);
    else if (cmd == "mp3stream")      th_cmd_mp3stream(args);
    else if (cmd == "hr_scan_start")  th_cmd_hr_scan_start();
    else if (cmd == "hr_dismiss")     th_cmd_hr_dismiss();
    else if (cmd == "hr_scan_stop")   th_cmd_hr_scan_stop();
    else if (cmd == "theremin_start") th_cmd_theremin_start();
    else if (cmd == "theremin_stop")  th_cmd_theremin_stop();
    else if (cmd == "hr_feed")        th_cmd_hr_feed(args);
    else if (cmd == "hr_anchor")      th_cmd_hr_anchor(args);
    else if (cmd == "hr_lockpolicy")  th_cmd_hr_lockpolicy(args);
    else if (cmd == "hr_guard")       th_cmd_hr_guard(args);
    else if (cmd == "hr_debug")       th_cmd_hr_debug();
    else if (cmd == "hr_calibrate")   th_cmd_hr_calibrate();
    else if (cmd == "hr_cal_clear")   th_cmd_hr_cal_clear();
    else if (cmd == "manual_decode")  th_cmd_manual_decode(args);
    else if (cmd == "manual_grid")    th_cmd_manual_grid(args);
    else if (cmd == "audio_suspend_test")  th_cmd_audio_suspend_test(args);
    else if (cmd == "radio_announce_dump") th_cmd_radio_announce_dump(args);
    else if (cmd == "cfg_get")        th_cmd_cfg_get(args);
    else if (cmd == "cfg_set")        th_cmd_cfg_set(args);
    else if (cmd == "theme_set")      th_cmd_theme_set(args);
    else if (cmd == "led_set")        th_cmd_led_set(args);
    else if (cmd == "led_sweep")      th_cmd_led_sweep(args);
    else if (cmd == "led_preset")     th_cmd_led_preset(args);
    else if (cmd == "info_show")      th_cmd_info_show(args);
    else if (cmd == "onboarding_reset") th_cmd_onboarding_reset();
    else if (cmd == "onboarding_accept") th_cmd_onboarding_accept();
    else if (cmd == "tour_step")      th_cmd_tour_step(args);
    else if (cmd == "reboot")         th_cmd_reboot();
    else th_send_err("unknown", cmd.c_str());
}

// ─── Serial intercept (called from loop) ─────────────────────────────────

static bool test_harness_process_serial() {
    // Check if next byte is STX — if so, this is a test command
    if (Serial.peek() != TH_STX) return false;

    Serial.read();  // consume STX
    String line = Serial.readStringUntil('\n');
    line.trim();

    // Strip JSON wrapper if present: {"cmd":"xxx"} → just parse the cmd
    // For simplicity, accept both JSON and plain text formats
    if (line.startsWith("{")) {
        // Minimal JSON parse — extract cmd and args
        int cmd_start = line.indexOf("\"cmd\":\"");
        if (cmd_start >= 0) {
            cmd_start += 7;
            int cmd_end = line.indexOf("\"", cmd_start);
            String cmd = line.substring(cmd_start, cmd_end);

            String args = "";
            int args_start = line.indexOf("\"args\":\"");
            if (args_start >= 0) {
                args_start += 8;
                int args_end = line.indexOf("\"", args_start);
                args = line.substring(args_start, args_end);
            }
            th_dispatch(cmd + " " + args);
            return true;
        }
    }

    // Plain text format: "command args..."
    th_dispatch(line);
    return true;
}

// ─── Init ─────────────────────────────────────────────────────────────────

static void test_harness_init() {
    // Register secondary touch input device
    th_indev = lv_indev_create();
    lv_indev_set_type(th_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(th_indev, th_touch_read_cb);

    // The global tap-sound hook is installed by ui_install_global_click_sound() from setup()
    // (Clip-Boy.ino:110), which walks the indev list AS IT EXISTS AT THAT MOMENT -- and this
    // indev is created 239 lines later (Clip-Boy.ino:349). So harness-injected taps produced
    // NO click sound, silently diverging from real touch: audio_play_click() was unreachable
    // from every scripted tap, and any defect that rides the click was untestable by
    // construction. That is exactly how SB3 (a tap click killing the theremin) read as
    // "confirmed in source, does not reproduce on hardware" for a full day, and why its
    // regression test PASSED against a deliberately fault-injected build.
    // Register it here so a harness tap is the same event sequence as a finger.
    lv_indev_add_event_cb(th_indev, ui_global_click_sound_cb, LV_EVENT_PRESSED, NULL);

    // Allocate audio ring buffer in PSRAM
    th_audio_ring = (int16_t *)heap_caps_malloc(
        TH_AUDIO_RING_SIZE * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (th_audio_ring) {
        memset(th_audio_ring, 0, TH_AUDIO_RING_SIZE * 2 * sizeof(int16_t));
    }

    Serial.printf("%c{\"event\":\"harness_ready\",\"version\":\"%s\"}\n",
                  TH_STX, TH_VERSION);
}

#endif // TEST_HARNESS
