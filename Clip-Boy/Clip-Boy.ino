/*******************************************************************************
 * ui_test.ino - Clip-Boy V3 (Hardware Integration)
 *
 * Copyright (C) 2026 The Clip-Boy Authors
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License version 3, as published by the
 * Free Software Foundation. It is distributed WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details (LICENSE / LICENSE.md).
 *
 * Waveshare ESP32-S3-Touch-LCD-2.8 (320x240), LVGL 9.2
 * Uses LovyanInit-Waveshare for display/touch init.
 *
 * V3: Full hardware integration:
 *   - ClipBoyMarauder for WiFi/BT hacking functions
 *   - Adafruit_NeoPixel for 8 LEDs (4 fuse + 4 front)
 *   - audio-tools for I2S tone generation (theremin)
 *   - SparkFun_VL53L5CX for theremin distance sensing
 ******************************************************************************/

#include "cb_log.h"        // CB_LOG* — status notices silenced in release (kept in --test)

// H1: true once a USB host has actually drained our CDC TX (see cb_serial_tx_relax below).
// Declared up here so the test harness can report it -- without a read-back, "the badge got
// faster" and "the badge was never reflashed" are indistinguishable readings.
static bool cb_serial_seen = false;
// millis() at which a host first drained us, or 0 if never. The BOOLEAN ALONE IS USELESS as a
// control: the very act of connecting to read it latches it true, so "the fix was in force all
// window" and "the fix never engaged" both report true. The TIMESTAMP separates them --
// (uptime - cb_serial_seen_ms) small means it only latched when the reader attached just now.
static uint32_t cb_serial_seen_ms = 0;
// Runtime gate for CB_LOG chatter (extern'd in cb_log.h). The harness raises it
// around binary transfers so core-0 tasks can't splice log lines into the
// screenshot/audio byte stream (USB-CDC session-desync fix).
volatile bool cb_serial_quiet = false;

// Enlarge the Arduino loopTask stack from the 8KB default to 16KB. Deep tool paths
// run synchronously in loop() (the serial cmd handler / a UI tap dispatches into
// ClipBoy) and Evil Portal START in particular -- cold-WiFi init + AP bring-up +
// ~12 captive-portal handler registrations + DNS + String ops -- sits right at the
// 8KB limit and INTERMITTENTLY overflowed it: "Stack canary watchpoint triggered
// (loopTask)" -> reboot (backtrace pointed at the RTOS switch, canary=0xa5a5a5a5).
// 16KB gives ample headroom for the deepest tool path. (Root-cause fix for the
// Evil Portal start reboot; helps any deep tool.)
SET_LOOP_TASK_STACK_SIZE(16 * 1024);
#include <ws_lcd_setup.h>
#include <WiFi.h>
#include <SPIFFS.h>       // ClipBoy's data partition -- mounted/formatted early (before the
#include <LittleFS.h>     // startup sound) so a first-boot format never starves the audio task
#include "BAT_Driver.h"
#include "PWR_Key.h"

// ─── Hardware pin definitions ──────────────────────────────────────────────
#include "clipboy_pins.h"

// ─── ClipBoy/Marauder ─────────────────────────────────────────────────────
#include <ClipBoyMarauder.h>
#include "CommandLine.h"
ClipBoyMarauder cb;
CommandLine cli_obj;

// ─── Theme (no cross-deps) ──────────────────────────────────────────────────
#include "ui_theme.h"

// ─── Persistent config (NVS) ────────────────────────────────────────────────
#include "ui_config.h"
#include "arg_core.h"      // DC34 ARG: progress state, per-badge MAC, unlock gate

// ─── Collectibles data system ──────────────────────────────────────────────
#include "ui_collectibles.h"

// ─── Hardware drivers (depend on ui_config.h for cfg struct) ───────────────
#include "neopixel_driver.h"
#include "audio_driver.h"

// ─── Tool "More Info" descriptions ──────────────────────────────────────────
#include "tool_info.h"

// ─── Navigation + all UI code ───────────────────────────────────────────────
#include "ui_nav.h"

// ─── DC34 ARG: clipcli dispatcher + puzzle modules (after ui_nav for UI use) ──
#include "arg_clipcli.h"
#include "arg_p2_hack.h"
#include "arg_p3_dork.h"
#include "arg_p4_captcha.h"
#include "arg_unlock.h"        // P5 per-badge HMAC crypto (nonce-based, matches IVR)
#include "arg_p5_call.h"

// ─── Test harness (conditional, enabled via -DTEST_HARNESS build flag) ──────
#ifdef TEST_HARNESS
#include "test_harness.h"
#endif

// ─────────────────────── SETUP ──────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    // Native USB-CDC: cap TX blocking so a slow/absent reader can't stall the loop
    // into a watchdog reboot. Without this, a full TX buffer (verbose --test logging
    // outracing the reader, OR a USB host that enumerates the CDC but never reads it)
    // blocks Serial.write for seconds -> loop stalls -> reboot. 50ms delivers logs to
    // an active reader while dropping under pressure instead of hanging.
    Serial.setTxTimeoutMs(50);
    delay(200);
    Serial.println("\n=== CLIP-BOY V3 (HARDWARE INTEGRATION) ===");
    // ⚠ The 50 ms above is KEPT for the whole of setup() on purpose: the boot banner on
    // the line above is what production/flash_core.py's read_boot_banner() greps for, and
    // dropping to 0 here would make that WARN column noisy across the remaining fleet
    // flashes. cb_serial_tx_relax() at the END of setup() takes it to 0.

    BAT_Init();
    PWR_Init();

    if (!lcd_init()) {
        CB_LOGLN("[FAIL] Display init failed!");
        while (1) delay(100);
    }
    CB_LOGLN("[OK] Display + touch + LVGL initialized");

    // Global tap sound: one indev hook plays a click for every lv_button-class
    // widget, so no individual button needs to register its own callback.
    ui_install_global_click_sound();

    // Mask the black screen during the ~5s blocking ClipBoy init.
    //   normal build: mascot splash (stays up through the init).
    //   rift build:   rift ~1.5s, then draw Clippy -- Clippy then sits frozen
    //                 masking the init (it's static, so the freeze is invisible).
#ifdef BADGE_QUANTUM_RIFT
    rift_show_static();
    rift_pump(RIFT_HOLD_MS);   // rift "opening" beat ~1.5s
    rift_show_clippy();        // draw Clippy; the init below runs behind it
#else
    show_boot_splash();
#endif

    // Load saved settings from NVS
    cfg_load();
    hr_cal_load();      // restore per-zone LiDAR flat-field calibration
    cfg_prefs.begin(CFG_NAMESPACE, true);   // handle is closed after cfg_load(); open our own
    coll_filter_collected_only = cfg_prefs.getBool("collfilt", false);  // restore Collectibles All/Found view
    cfg_prefs.end();

    // Bring audio up EARLY (right after config load) so the startup sound plays ASAP --
    // during the slow WiFi auto-connect + ClipBoy init below -- at the saved volume, unless
    // muted. audio_init is self-contained (I2S + PSRAM + its own core-0 task) and no-ops if
    // called twice, so this is safe ahead of cb.begin()/neo_init().
    audio_init(cfg.volume / 100.0f);
    cfg_boot_tick();   // bump the (capped) boot counter (radio reminder rule)

    // FORMAT-BEFORE-SOUND. On the first boot after a fresh flash a blank data partition
    // is FORMATTED on its first mount; an ESP32-S3 flash erase disables the flash cache,
    // which stalls the flash-resident core-0 audio task -> the startup PCM underruns and
    // the sound stutters ("CPU pegged"). Crucially, a browser Factory flash writes littlefs
    // but NOT ClipBoy's SPIFFS, so on that path SPIFFS is the partition that formats and
    // starves the audio -- which a littlefs-only check misses. Fix: mount/format BOTH data
    // partitions HERE, synchronously, BEFORE the sound plays. On a normal boot both mount
    // instantly (no format) -> the sound still greets you immediately; on a first/format
    // boot the format completes first (the boot mascot lingers a beat) -> the sound then
    // plays CLEANLY, never contended. Covers every path (Factory, erase_region, SD-less)
    // regardless of flash order, and (unlike the old skip) the chime plays on EVERY boot.
    // Both begins are idempotent -> cb.begin()/coll_init() below reuse the mounts (no-op).
    // Explicit "littlefs" label dodges the SPIFFS/LittleFS mount-label confusion (lv_conf note).
    uint32_t fmt_t0 = millis();
    bool lfs_ok    = LittleFS.begin(true, "/littlefs", 10, "littlefs");  // our 7.4MB (radio beds)
    bool spiffs_ok = SPIFFS.begin(true);                                 // ClipBoy's 448KB data
    CB_LOGF("[BOOT] reset=%d littlefs=%d spiffs=%d fmt=%lums -> startup sound %s\n",
            (int)esp_reset_reason(), lfs_ok, spiffs_ok,
            (unsigned long)(millis() - fmt_t0), cfg.sound ? "play" : "muted");
    if (!cfg.sound) audio_set_volume(0.0f);
    else            audio_mp3_play(startup_mp3, startup_mp3_len, false);
    arg_core_init();   // DC34 ARG progress + per-badge MAC (after NVS available)
    arg_init();        // P5 crypto: load/generate per-badge nonce + unlock state (NVS)
    arg_clipcli_init();// wire challenge/hint/reset dispatch to the puzzle registry
    p2_register();     // P2 (hacking puzzle)
    p3_register();     // P3 Dork
    p4_register();     // P4 Captcha
    p5_register();     // P5 The Call (wires clipcli unlock)
    wifi_creds_load();

    // Check for saved WiFi credentials (reboot-to-connect flow).
    // Must happen BEFORE cb.begin() touches WiFi.
    {
        Preferences wp;
        wp.begin("wifi_join", true);  // read-only
        String saved_ssid = wp.getString("ssid", "");
        String saved_pw   = wp.getString("pw", "");
        wp.end();

        if (saved_ssid.length() > 0) {
            CB_LOGF("[WIFI] Auto-connecting to: %s\n", saved_ssid.c_str());
            WiFi.mode(WIFI_STA);
            WiFi.begin(saved_ssid.c_str(),
                       saved_pw.length() > 0 ? saved_pw.c_str() : NULL);

            // Wait up to 10s for connection
            int tries = 0;
            while (WiFi.status() != WL_CONNECTED && tries < 20) {
                delay(500);
                Serial.print(".");
                tries++;
            }
            if (WiFi.status() == WL_CONNECTED) {
                CB_LOGF("\n[WIFI] Connected! IP: %s\n",
                    WiFi.localIP().toString().c_str());
                // Save SSID for display, clear creds from NVS
                strncpy(wifi_join_ssid, saved_ssid.c_str(), sizeof(wifi_join_ssid) - 1);
            } else {
                CB_LOGLN("\n[WIFI] Auto-connect failed");
                WiFi.disconnect();
            }
            // Clear saved creds - one-shot
            Preferences wpc;
            wpc.begin("wifi_join", false);
            wpc.clear();
            wpc.end();
        }
    }

    // Ensure Arduino WiFi layer is fully initialized (event loop, STA netif,
    // event handlers) BEFORE cb.begin().  ClipBoy's RunSetup calls esp_wifi_init /
    // set_mode / start DIRECTLY (bypassing Arduino), and on IDF 4.4+ it does NOT
    // create the default event loop or STA netif.  Once esp_wifi_start succeeds
    // the --wrap linker shims lock _cb_wifi_hw_up=true, making all subsequent
    // esp_wifi_init/start/set_mode calls no-ops - including the ones inside
    // Arduino's WiFi.mode().  If Arduino never gets a chance to run its low-level
    // init first, the event loop and netif are missing, and esp_wifi_connect()
    // later crashes on Core 0 (InstrFetchProhibited @ PC 0x00000000) when the
    // WiFi task tries to dispatch events through a null handler.
    //
    // Calling WiFi.mode(WIFI_STA) here triggers Arduino's wifiLowLevelInit(),
    // which creates the event loop, STA netif, and registers event handlers.
    // This happens before cb.begin() locks everything out.  If the auto-connect
    // block above already called WiFi.mode(), this is a harmless no-op.
    //
    // Boot STA-only (NOT AP_STA): Sn34k-Boy is listen-only and must transmit
    // nothing, and an APSTA badge beacons a default 'ESP_xxxxxx' AP while idle.
    // The active-transmit tools (Res34rch-only) need the AP interface for
    // esp_wifi_80211_tx(WIFI_IF_AP,...), so they switch to APSTA ON DEMAND at
    // tool start (cb.setRawTxMode(true) in dispatch_clipboy_action) and restore
    // STA on stop (cb_stop_operation) -- no AP is broadcast when idle. That
    // on-demand switch uses __real_esp_wifi_set_mode to bypass the --wrap shim
    // that would otherwise no-op it (which is the bug that left every WIFI_IF_AP
    // transmit radiating nothing when the badge was locked to STA-only).
    WiFi.mode(WIFI_STA);

#if defined(CB_WIFI_PS_OFF) && CB_WIFI_PS_OFF
    // EXPERIMENT BUILD ONLY (CB_EXTRA_DEFS='-DCB_WIFI_PS_OFF=1'). NOT a shipping default.
    // MEASURED 2026-07-28: `state.wifi_ps` reads 1 (WIFI_PS_MIN_MODEM) on a stock badge, both at
    // idle and with a PASSIVE tool running in promiscuous mode -- the Arduino core defaults
    // _sleepEnabled to MIN_MODEM on non-S2 targets (WiFiGeneric.cpp:761-765) and applies it on
    // ARDUINO_EVENT_WIFI_STA_START, which the WiFi.mode() above triggers. After cb.begin() sets
    // _cb_wifi_hw_up, __wrap_esp_wifi_set_ps (ClipBoyMarauder.cpp:63) no-ops every further
    // attempt, so THIS LINE IS THE ONLY WINDOW in which power-save can still be changed.
    // Whether MIN_MODEM actually costs received frames for an UNASSOCIATED STA in promiscuous
    // mode is the open question -- flag confirmed, cost unmeasured. This define exists to price
    // it by A/B on the calibrated rig, not to fix anything.
    WiFi.setSleep(false);
#endif

    // ClipBoy/Marauder init
    if (!cb.begin()) {
        CB_LOGLN("[WARN] Clip-Boy init failed - tool functions unavailable");
    } else {
        CB_LOGLN("[OK] Clip-Boy initialized");
    }
    cli_obj.RunSetup();

    // Push the persisted deauth channel policy into the radio layer once at boot.
    // Without this the WiFiScan member keeps its compiled-in default while NVS (and both
    // dropdowns) show whatever the user chose -- so a badge booted straight into Radiation
    // would sample a different channel set than the UI claims. Every other hardware-touching
    // persisted setting on this badge has an explicit apply-at-boot; this is that.
    // RunDeauthScan re-asserts it on every start as well, which is what covers the CLI.
    cb.setDeauthChannel(cfg.deauth_chan);

    // Persistent log buffer for tool output (PSRAM)
    cb_log_buf_init();

    // NeoPixel init + apply saved LED config
    neo_init();
    neo_apply_all();
    neo_rubber_duck_active = cfg.led_rubber_duck;  // restore Rubber Duck theme if it was active
    neo_start_core0_task();  // Move animation tick to core 0

    // (Audio was initialized early, right after cfg_load, so the startup sound plays ASAP.)

    // ARG reward reconciliation: if the puzzle set is fully complete but the Quanta
    // look was never applied (e.g. an out-of-order finish on older firmware), apply
    // it now so the payoff isn't permanently lost. (Live out-of-order finishes are
    // handled by arg_on_complete_fn; neo_init already ran above so the LED preset sticks.)
    // ⚠ Gate on arg.theme_active, NOT on `cfg.theme != THEME_QUANTA`. This is a ONE-SHOT
    // reconciliation (see the comment above), but `cfg.theme != THEME_QUANTA` is exactly the
    // state an ARG completer creates the moment they pick any other theme -- Quanta appears in
    // the DATA > Settings dropdown once earned (ui_nav.h:10402), so choosing Mojave is a
    // supported one-tap action. The old condition therefore re-fired on EVERY boot and
    // persisted, silently reverting:
    //   - the user's chosen theme, including the completionist Custom theme + its custom_hue;
    //   - all 8 per-LED colour/brightness/animation settings, via led_apply_preset(7);
    //   - the Rubber Ducky LED theme, a SEPARATE ARG reward (P3) that Clip-Boy.ino:248
    //     restores 12 lines above and led_apply_preset() then clears AND un-persists.
    // One ARG reward ate another, every boot, for the most engaged users on the badge.
    // arg.theme_active exists for precisely this and was read by nothing but the SD
    // serializer and a clipcli status print. A live P5 finisher already calls
    // arg_set_theme(ARG_THEME_QUANTA) (arg_p5_call.h:44), so they never reach this path;
    // recording it here makes the retro-fix behave the same way, and stops the clipcli
    // Zenith selection from being clobbered too.
    if (arg_quanta_earned() && arg.theme_active == ARG_THEME_DEFAULT) {
        cfg.theme = THEME_QUANTA;
        cfg_save_theme();
        led_apply_preset(7);   // Quanta LED glow
        arg_set_theme(ARG_THEME_QUANTA);   // record it so this cannot re-fire next boot
    }

    // Apply saved theme
    ui_theme_init();
    if (cfg.theme != 0) ui_theme_apply(cfg.theme);
    CB_LOGLN("[OK] Theme initialized");

    // Do NOT call WiFi.mode(WIFI_OFF) here - the --wrap shims make
    // esp_wifi_stop/deinit into no-ops, so WiFi.mode(WIFI_OFF) wouldn't
    // actually turn WiFi off anyway.  WiFi stays running; power savings
    // come from toggling promiscuous mode between scans.

    // Load collectibles from LittleFS (fallback to compiled-in). This also
    // mounts the shared SD card on the badge's HSPI bus (coll_init_sd).
    coll_init();

    // PCAP single-owner (DC34-147): the badge owns SD.begin (HSPI, above), so
    // tell ClipBoy the card is up -- its own initSD ran first and failed/got
    // clobbered, leaving sd_obj.supported=false and PCAP writing nowhere. Then
    // sync the SavePCAP gate to our "Allow PCAP Saving" setting (fail-safe:
    // default off on Sn34k, on for Res34rch).
    cb.setSDAvailable(coll_sd_available);
    cb.setSavePCAP(cfg.allow_pcap);

#ifdef CLIPBOY_RES34RCH
    // Seed the Evil Portal EXAMPLE templates (shipped in littlefs, res34rch only)
    // onto the SD card so users can copy/edit them on a computer. Only if the card
    // is present and the folder isn't already there -- never clobber user edits.
    // These demos capture nothing; the user must deliberately edit them (see the
    // folder's README) to run an authorized portal.
    // Guard on a specific FILE (not the dir): a prior seed that created the dir
    // but failed to copy (e.g. littlefs not yet populated) must still re-seed.
    if (coll_sd_available && !SD.exists("/examples/evil_portal/README.txt")) {
        SD.mkdir("/examples");
        SD.mkdir("/examples/evil_portal");
        static const char *ep_files[] = { "index.html", "ap.config.txt", "README.txt" };
        int seeded = 0;
        for (const char *fn : ep_files) {
            String lp = String("/examples/evil_portal/") + fn;
            File src = LittleFS.open(lp, "r");
            if (!src) continue;
            File dst = SD.open(lp.c_str(), "w");
            if (!dst) { src.close(); continue; }
            uint8_t io[256];
            while (src.available()) {
                int n = src.read(io, sizeof(io));
                if (n > 0) dst.write(io, n);
            }
            dst.close(); src.close();
            seeded++;
        }
        CB_LOGF("[EP] Seeded %d Evil Portal example file(s) -> SD /examples/evil_portal/\n", seeded);
    }
#endif

    // Copy compiled-in images from PROGMEM to PSRAM (~622KB)
    coll_images_init_psram();

    // Apply saved brightness
    lcd_set_brightness((uint8_t)(cfg.brightness * 255 / 100));

    // Apply saved volume to shared global
    theremin_volume = cfg.volume;

#ifdef BADGE_QUANTUM_RIFT
    // Init's done (Clippy masked it). Remove the rift/Clippy overlay; the main
    // screen + ClipOS POST take over next.
    rift_finish();
#endif

    create_main_screen();
    screensaver_init();
#ifndef BADGE_QUANTUM_RIFT
    hide_boot_splash();   // Remove the static splash; the animated POST replaces it
#endif
    show_legal_notice();  // First-boot disclaimer; MUST be accepted first. Its Accept
                          // handler chains into show_clippy_intro(), so the tour never
                          // races with / draws over the unaccepted disclaimer.
    if (cfg.legal_ack)    // Already acknowledged (e.g. a unit updated from an older
        show_clippy_intro();  // build): no notice on screen to race, so show the intro now.
    show_boot_screen();   // Retro POST sequence overlay (on top of everything)
    CB_LOGLN("[OK] Main screen created");

    // Radio discovery nudge: if there's an unlocked-but-unviewed station, arm the
    // reminder (2nd+ boot -> ~4s, first boot -> 10 min). No-op if opted out.
    radio_reminder_arm();

#ifdef TEST_HARNESS
    test_harness_init();
    CB_LOGLN("[OK] Test harness initialized");
#endif

    CB_LOGF("[OK] Free heap: %lu bytes\n",
                  (unsigned long)esp_get_free_heap_size());
    CB_LOGF("[OK] Free PSRAM: %lu bytes\n",
                  (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    CB_LOGLN("[OK] Setup complete. ROM... ROM never changes.");

    // ── H1 fix: restore the core's zero TX timeout while no host is reading us ──
    // MEASURED (2026-07-27, 3 co-located badges, common zero, constant stimulus): a badge
    // with nothing draining serial processes 2.2-2.6x FEWER management frames than one with
    // a monitor attached (4569 drained vs 1762 / 2079). Saturation proved it: the drained
    // badge scaled 2.30 -> 5.86 frames/s with offered load while both undrained badges
    // stayed pinned at ~2.2-2.7/s. Every passive WiFi tool is affected -- the promiscuous
    // RX callbacks carry 159 unguarded Serial.* calls across 10 callbacks, including the
    // Geiger (deauthSnifferCallback) and Scan APs (apSnifferCallbackFull).
    //
    // Root cause: HWCDC statically inits tx_timeout_ms = 0 and its own comment (HWCDC.cpp
    // :79-86) says that is DELIBERATE so an unplugged badge never delays on a full TX
    // queue. Our unconditional setTxTimeoutMs(50) at boot destroyed that property, so with
    // no host draining the 256-byte ring every write blocks in the WiFi driver task.
    //
    // Why a polled level-read and NOT Serial.onEvent(CONNECTED):
    //   1. The CONNECTED event is a ONE-SHOT EDGE posted from the ISR, and it is silently
    //      DISCARDED when no handler is registered yet (arduino_hw_cdc_event_post returns
    //      ESP_FAIL on a NULL loop handle). It fires ms after Serial.begin(), i.e. before
    //      any registration here -- the handler would never run and the badge would sit at
    //      100 ms forever. A no-op plus a regression.
    //   2. Registering ANY handler creates the event loop, after which the CDC ISR posts a
    //      TX event per 64-byte chunk and an RX event per packet into a priority-5 task --
    //      new ISR work on the very path we are trying to unblock. Today those posts are a
    //      NULL check and nothing else.
    // (bool)Serial reads `initial_empty` -- the exact latch CONNECTED is derived from -- as
    // a LEVEL, so there is no edge to miss and no event machinery.
    Serial.setTxTimeoutMs(0);
}

// Re-arm the 50 ms timeout the moment a host actually starts reading us, so an attached
// monitor still gets complete output. Cheap: one bool test until it latches, then nothing.
// ⚠ Timeout 0 must NOT be in force while a host is reading -- HWCDC::write does a
// non-blocking partial send then a blocking remainder, so at 0 ANY write larger than the
// free ring space is truncated at ~256 bytes even with a fast reader. That would corrupt
// the harness's framed JSON (detect_counts emits ~480 B; screenshots write 1 KB chunks).
static inline void cb_serial_tx_relax() {
    if (!cb_serial_seen && Serial) {
        cb_serial_seen = true;
        cb_serial_seen_ms = millis();
        Serial.setTxTimeoutMs(50);
    }
}

// ─────────────────────── LOOP ───────────────────────────────────────────────

void loop() {
    cb_serial_tx_relax();
    PWR_Loop();   // power button: medium hold = restart, >=3s = shutdown (latch)
    {
        cb.loop();
        lv_timer_handler();
#ifdef TEST_HARNESS
        th_fps_tick();
#endif
        screensaver_tick();
        hr_cal_tick();   // finalize a pending sensor calibration (may stop the scan)
        if (hr_scanning) {
            hr_scanner.tick();

            // Feed the overlay heatmap; drive our bottom-left level bubble
            // (the overlay's corner bubble is disabled -- showLevelBubble=false).
            hr_scanner.feedOverlaySensor();
            float roll = 0.0f, pitch = 0.0f;
            if (hr_imu_ok) hr_imu.readLevel(roll, pitch);
            hr_level_update(roll, pitch);

            // While the engine has the user's tag in view (orient marker
            // found -> 'Hold Steady' prompt), bump the scan-timeout
            // forward. progress > 0 alone misses the case where the
            // orient marker is found but CRC keeps failing -- the
            // engine still wants the user to hold steady.
            const auto &r = hr_scanner.result();
            if (r.prompt == HRScan::Prompt::HoldSteady ||
                r.progress > 0) {
                hr_scanner.resetTimeout();
            }

            // Scanner auto-stops on lock, timeout, or unknown ID. Three paths:
            //   - real timeout: library's own timedOut flag with no valid lock
            //   - unknown code: hr_lock_cb saw a decode whose ID isn't in our
            //     collectibles list (e.g. physical code 128 vs our 1-100 range)
            //   - success: a valid collectible ID locked and was persisted
            if (!hr_scanner.isEnabled()) {
                const auto &res = hr_scanner.result();
                const bool real_timeout = res.timedOut && res.lockedId < 0;
                const int  unknown_id   = real_timeout ? -1 : hr_scan_last_invalid_id;
                const char *path = real_timeout ? "TIMEOUT"
                                 : unknown_id >= 0 ? "UNKNOWN"
                                 : "SUCCESS";
                CB_LOGF("[HR] Scan ended: timedOut=%d lockedId=%d unknownId=%d -> %s\n",
                              (int)res.timedOut, res.lockedId, unknown_id, path);
                if (real_timeout) {
                    audio_sonar_stop();
                    if (cfg.sound) audio_mp3_play(scan_stop_mp3, scan_stop_mp3_len, false);
                    hr_scan_timeout_cleanup();
                } else if (unknown_id >= 0) {
                    audio_sonar_stop();
                    if (cfg.sound) audio_mp3_play(scan_stop_mp3, scan_stop_mp3_len, false);
                    hr_scan_unknown_cleanup(unknown_id);
                    hr_scan_last_invalid_id = -1;
                } else {
                    // Lock fired on a valid ID - scan_ok already playing; let
                    // it finish while a "Found: <title>" banner is held for
                    // ~1.6s so the user sees what they unlocked.
                    hr_scan_success_cleanup(res.lockedId);
                }
            }
        }
        // neo_animation_tick() now runs on core 0 via neo_start_core0_task()
        audio_loop();
    }

    // Serial CLI - our commands + Marauder CLI
    if (Serial.available()) {
#ifdef TEST_HARNESS
        // Test harness intercepts STX-prefixed commands
        if (test_harness_process_serial()) { /* handled */ }
        else
#endif
      {
        String line = Serial.readStringUntil('\n');
        line.trim();
        if (line.length() > 0) {
            bool handled = false;
            // DC34 ARG: capture follow-up lines (confirm Y/N, in-session) or any
            // line starting with "clipcli". (handoff §2.2/§2.4)
            if (arg_is_capturing() || line.startsWith("clipcli")) {
                arg_clipcli_line(line.c_str());
                handled = true;
            }
#ifdef COLL_DEBUG
            if (line.startsWith("coll ")) {
                coll_process_serial(line);
                handled = true;
            }
#endif
#ifdef RADIO_PCM_TEST
            if (!handled && line == "radiolfs") {   // seed + stream from littlefs (perf test)
                radio_pcm_test_lfs();
                handled = true;
            }
            if (!handled && line == "radioprog") {   // stream from PROGMEM (A/B baseline)
                audio_pcm_play(radio_test_pcm_ulaw, radio_test_pcm_len, radio_test_pcm_rate, false);
                handled = true;
            }
            if (!handled && line == "radiostop") {
                audio_pcm_stop();
                handled = true;
            }
#endif
#ifdef TEST_HARNESS
            if (!handled && line == "glyphtest") {   // SSID/glyph substitution test screen
                show_glyph_test();
                handled = true;
            }
            if (!handled && line == "stationtest") {  // radio "new station detected" modal (real path)
                radio_announce_one(4, false);   // Pirate 0x7C00; View navigates to the radio + tunes it
                handled = true;
            }
            if (!handled && line.startsWith("settheme ")) {  // apply theme by index (prototype)
                int n = line.substring(9).toInt();
                ui_theme_switch_live((uint8_t)n);   // proper teardown (deletes old scr_main +
                                                    // status_timer) -- matches the real dropdown;
                                                    // apply+create_main_screen leaked them each call
                CB_LOGF("[THEME] applied %d (%s)\n", n,
                              (n >= 0 && n < NUM_THEMES) ? theme_presets[n].name : "?");
                handled = true;
            }
            if (!handled && line.startsWith("gototab ")) {   // nav div+tab (for screenshots)
                int d = line.substring(8).toInt();
                int sp = line.indexOf(' ', 8);
                int t = (sp > 0) ? line.substring(sp + 1).toInt() : 0;
                goto_div_tab((uint8_t)d, (uint8_t)t);
                CB_LOGF("[NAV] div %d tab %d\n", d, t);
                handled = true;
            }
            if (!handled && line.startsWith("therem ")) {    // theremin on/off (verify voice-lock)
                String a = line.substring(7); a.trim();
                if (a == "on") CB_LOGF("[THEREMIN] enable=%d\n", theremin_enable());
                else { theremin_disable(); CB_LOGLN("[THEREMIN] disabled"); }
                handled = true;
            }
            if (!handled && line.startsWith("rubduck ")) {   // rubber-duck LED theme on/off
                String a = line.substring(8); a.trim();
                if (a == "on") { led_apply_rubber_duck(); CB_LOGLN("[LED] rubber duck ON"); }
                else { led_apply_preset(4); CB_LOGLN("[LED] off"); }
                handled = true;
            }
            if (!handled && line == "p5code") {   // reveal THIS badge's expected P5 code (debug/test only)
                char c[9]; arg_code_for_nonce(arg_nonce, c);
                Serial.printf("[P5TEST] nonce=%04u code=%s\n", arg_nonce, c);
                handled = true;
            }
            if (!handled && line == "p3pass") {   // reveal THIS badge's P3 Queue passphrase (debug/test only)
                char w[8]; p3_passphrase(w);
                Serial.printf("[P3TEST] passphrase=%s\n", w);
                handled = true;
            }
            if (!handled && line == "p5screen") {  // show the P5 numpad (render test)
                show_p5_numpad();
                Serial.println("[P5TEST] numpad shown");
                handled = true;
            }
            if (!handled && line == "secret") {  // fire the secret-menu reveal (render test)
                if (arg_reveal_keypad_fn) arg_reveal_keypad_fn();
                Serial.println("[ARGTEST] secret-menu keypad revealed");
                handled = true;
            }
            if (!handled && line == "p3map") {   // dump the full P3 text-adventure map (rooms/items/NPCs/verbs)
                p3_map_dump();
                handled = true;
            }
            if (!handled && line == "pcapstart") {   // DC34-147 PCAP capture test
                cb.setSavePCAP(true);
                cb.setChannel(1);
                cb.sniffRaw();                        // raw packet monitor -> startPcap
                Serial.println("[PCAPTEST] capture started (ch1, raw). Run 'pcapstop' after a few seconds.");
                handled = true;
            }
            if (!handled && line == "pcapstop") {     // DC34-147 stop + verify .pcap
                cb.stopScan();
                auto verify = [](fs::FS &fsx, const char *label) {
                    File root = fsx.open("/");
                    if (!root) { Serial.printf("[PCAPTEST] %s: no root\n", label); return; }
                    String newest; size_t nsz = 0;
                    for (File f = root.openNextFile(); f; f = root.openNextFile()) {
                        String n = f.name();
                        if (n.endsWith(".pcap") && f.size() >= nsz) { newest = n; nsz = f.size(); }
                        f.close();
                    }
                    root.close();
                    if (!newest.length()) { Serial.printf("[PCAPTEST] %s: no .pcap found\n", label); return; }
                    String path = newest.startsWith("/") ? newest : ("/" + newest);
                    File pf = fsx.open(path, FILE_READ);
                    if (!pf) { Serial.printf("[PCAPTEST] %s: open failed %s\n", label, path.c_str()); return; }
                    uint8_t h[24]; int rd = pf.read(h, 24); size_t sz = pf.size(); pf.close();
                    bool magic = (rd >= 4 && h[0]==0xd4 && h[1]==0xc3 && h[2]==0xb2 && h[3]==0xa1);
                    Serial.printf("[PCAPTEST] %s: %s size=%u magic=%s\n", label, path.c_str(),
                                  (unsigned)sz, magic ? "OK(a1b2c3d4)" : "BAD");
                    Serial.print("[PCAPTEST] head:");
                    for (int i = 0; i < rd; i++) Serial.printf(" %02x", h[i]);
                    Serial.println();
                };
                if (coll_sd_available) verify(SD, "SD");
                verify(LittleFS, "LittleFS");
                cb.setSavePCAP(cfg.allow_pcap);       // restore the user's setting
                handled = true;
            }
#endif
            if (!handled && line == "cbhelp") {
                Serial.println("=== Clip-Boy Commands ===");
#ifdef COLL_DEBUG
                Serial.println("coll add <id>    - mark collectible as found");
                Serial.println("coll add all     - mark all as found");
                Serial.println("coll remove <id> - mark as not found");
                Serial.println("coll list        - list all collectibles");
                Serial.println("coll reset       - reset all to uncollected");
#endif
                Serial.println("heap             - show free memory");
                Serial.println("wifijoin <ssid>  - test WiFi join (blocking)");
                Serial.println("wifistatus       - show WiFi state");
                Serial.println("reboot           - restart badge");
                Serial.println("help             - Marauder commands");
                handled = true;
            }
            if (!handled && line.startsWith("wifijoin ")) {
                String ssid = line.substring(9);
                ssid.trim();
                // AIRPLANE GATE. This handler lives OUTSIDE the `#ifdef TEST_HARNESS` block, so
                // despite its [TEST] logging it SHIPS -- and it associates and transmits. It had
                // no airplane check, so `wifijoin <ssid>` over USB serial would join a network
                // with the Airplane switch reading engaged. Found by the 2026-07-26 gating trace;
                // it was not on the known-issues list. Cheap to gate because it is OUR code, not
                // the vendored Marauder parser (which is documented as ungated instead).
                // Set `handled` and fall through -- every later branch tests `!handled`, so this
                // skips the join without a `continue` (this block is not inside a loop; the
                // enclosing scope is `if (Serial.available())` in loop(), so `continue` would not
                // compile).
                if (cfg.airplane) {
                    Serial.println("[CB] wifijoin blocked: Airplane Mode is on "
                                   "(DATA > Settings > Airplane Mode)");
                    handled = true;
                } else {
                Serial.printf("[TEST] WiFi.status before: %d\n", WiFi.status());
                Serial.printf("[TEST] WiFi.getMode: %d\n", WiFi.getMode());
                Serial.printf("[TEST] Calling cb.joinWiFi(%s, \"\")\n", ssid.c_str());
                bool ok = cb.joinWiFi(ssid, "");
                Serial.printf("[TEST] joinWiFi -> %s\n", ok ? "CONNECTED" : "FAILED");
                Serial.printf("[TEST] WiFi.status after: %d\n", WiFi.status());
                handled = true;
                }   // end else (airplane off)
            }
            if (!handled && line == "wifistatus") {
                Serial.printf("WiFi.status: %d\n", WiFi.status());
                Serial.printf("WiFi.getMode: %d\n", WiFi.getMode());
                Serial.printf("WiFi.SSID: %s\n", WiFi.SSID().c_str());
                Serial.printf("WiFi.localIP: %s\n", WiFi.localIP().toString().c_str());
                esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                Serial.printf("STA netif: %s\n", netif ? "exists" : "NULL!");
                handled = true;
            }
            if (!handled && line == "heap") {
                Serial.printf("Free DRAM: %lu bytes\n",
                    (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
                Serial.printf("Free PSRAM: %lu bytes\n",
                    (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
                handled = true;
            }
            if (!handled && line == "reboot") {
                Serial.println("Rebooting...");
                delay(100);
                ESP.restart();
            }
            if (!handled) {
                // Forward to Marauder CLI
                cli_obj.runCommand(line);
            }
            Serial.print("> ");
        }
      } // end else (non-test-harness serial)
    }

    delay(5);
}
