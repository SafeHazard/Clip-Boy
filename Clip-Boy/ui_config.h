#pragma once
// ui_config.h - Persistent user settings for Clip-Boy badge
//
// Uses ESP32 NVS (Preferences) to store/restore user settings across reboots.
// Header-only: all functions are static, included from ui_test.ino.
//
// Settings stored:
//   theme       (uint8)  - 0=Mojave, 1=Ribbit City, 2=Flashbang
//   brightness  (uint8)  - 10-100 (display brightness %)
//   volume      (uint8)  - 0-100 (theremin/speaker volume)
//   disp_off    (uint8)  - display timeout index (0=15s,1=30s,2=60s,3=2m,4=5m,5=Never)
//   airplane    (bool)   - airplane mode on/off
//   sound       (bool)   - sound on/off
//   leds        (48 bytes) - per-LED config blob (8 LEDs x 6 bytes)
//   thr_voices  (8 bytes) - theremin voice types per slot (0=None..4=Triangle)
//   ss_style    (uint8)  - screensaver style (0=Clip-Boy, 1=Blank, 2=Flying Clippy)
//   ss_bright   (uint8)  - 1-100 (Clip-Boy screensaver dimness, percent of full)
//   ss_leds     (bool)   - turn off LEDs during screensaver
//   ss_clock    (bool)   - show the idle clock on the screensaver
//   tz          (uint8)  - time zone index into cb_timezones[] (ui_nav.h)
//   crt_scan    (bool)   - CRT scanline overlay
//   crt_flick   (bool)   - CRT flicker effect
//   help_btn    (bool)   - show '?' help button in the status bar

#include <Preferences.h>

#define CFG_NAMESPACE "clipboy"

// Default values
#define CFG_DEF_THEME       0
#define CFG_DEF_BRIGHTNESS  80
#define CFG_DEF_VOLUME      75
#define CFG_DEF_DISP_OFF    2     // 60s
#define CFG_DEF_AIRPLANE    false
#define CFG_DEF_SOUND       true
// 4 voice "bands", left→right, each owning 2 columns of the 8x8 LiDAR
// grid (16 zones per band). See audio_driver.h::audio_theremin_feed for
// the band-averaging that drives each voice slot.
#define CFG_NUM_THEREMIN_VOICES 4
#define CFG_DEF_THEREMIN_VOICE  0     // None
// Band-detection tuning: k = required-confirmation count (need k zones
// agreeing within agreement_mm of each other before the band fires).
// k=1 = pure closest-zone (fast, jumpy); k=2 default = closest plus one
// confirmer (resists single-zone glitches).
#define CFG_DEF_THEREMIN_K       2     // 1-8
#define CFG_DEF_THEREMIN_AGREE   20    // 5-50 mm
#define CFG_DEF_SS_STYLE        0     // 0=Clip-Boy, 1=Blank, 2=Flying Clippy
#define CFG_DEF_SS_BRIGHTNESS   10    // Clip-Boy screensaver dimness (1-100%)
#define CFG_DEF_SS_LEDS_OFF    false  // Keep LEDs running during screensaver by default
#define CFG_DEF_SS_CLOCK       true   // Idle clock on by default: it is the reason the badge runs NTP at all
// Index into cb_timezones[] in ui_nav.h. Kept as a bare literal because
// ui_config.h is included FIRST and must not depend on ui_nav.h; ui_nav.h
// static_asserts it against the real table size at the definition site.
// 2 = US Central, the zone the clock was hardcoded to before it was a setting,
// so an existing badge that never opens the picker behaves exactly as it did.
#define CFG_DEF_TZ             2
#define CFG_DEF_LED_RUBBER_DUCK false // Rubber Duck LED theme off by default
#define CFG_DEF_CRT_SCANLINES  true   // on by default — the CRT look is core to the aesthetic
#define CFG_DEF_CRT_FLICKER    true
#define CFG_DEF_HELP_BTN       true
#define CFG_DEF_SS_UNLOCK_TONE true    // rising tone during hold-to-unlock
#define CFG_DEF_SCAN_SOUND     true    // ambient sound loop while scanning an HR code
#define CFG_DEF_MANUAL_CONFIRM true    // verify-first (DEFAULT ON): show the scan's pattern to
                                       // confirm before it unlocks; off = fast auto-unlock
#define CFG_DEF_UI_CLICK       true    // tap/click sound on buttons & controls
#define CFG_DEF_TERM_FONT      0       // tool log font: 0=Small(14) 1=Medium(16) 2=Large(18)
// Deauth/Radiation channel policy: 0 = hop all 14, 200 = hop 1/6/11, 1-14 = lock that channel.
// 200 mirrors CB_DEAUTH_HOP_TRI in WiFiScan.h -- kept as a literal because ui_config.h must not
// pull in the Marauder headers. The two are asserted equal in ui_nav.h at the one call site.
#define CFG_DEF_DEAUTH_CHAN  200       // default 1/6/11: ~3x the duty cycle of a 14-wide walk
#define CFG_DEF_RADIO_DRIFT    1       // radio tuning drift: 0=Disabled 1=Once Per Boot 2=Every Access
#define CFG_DEF_EXCLUSIVE_INPUT false  // lock touch while a serial ARG session is active
#ifdef CLIPBOY_RES34RCH
#define CFG_DEF_ALLOW_PCAP     true    // Res34rch-Boy: PCAP capture on by default
#else
#define CFG_DEF_ALLOW_PCAP     false   // Sn34k-Boy: fail-safe, PCAP capture off by default
#endif

// Per-LED settings - 8 physical LEDs (4 fuse + 4 front)
#define CFG_NUM_LEDS 8

struct LedConfig {
    uint8_t brightness;  // 0-255
    uint8_t r, g, b;     // RGB color
    uint8_t animation;   // 0=None, 1=Breathe, 2=Chase
    uint8_t speed;       // 1-10
};

// Runtime config struct - loaded on boot, modified by UI, saved on change
struct ClipBoyConfig {
    uint8_t theme;
    uint8_t brightness;
    uint8_t volume;
    uint8_t disp_off;
    bool    airplane;
    bool    sound;
    LedConfig leds[CFG_NUM_LEDS];
    LedConfig custom_leds[CFG_NUM_LEDS]; // saved "Custom" LED theme (presets never overwrite it)
    uint8_t   led_theme;                 // active LED theme: preset id 0-7, 99=Rubber Duck, 100=Custom
    bool      custom_seeded;             // false until Custom is first picked (then seeds from current LEDs)
    uint8_t theremin_voices[CFG_NUM_THEREMIN_VOICES];
    uint8_t theremin_k;            // 1-8: confirmation count for band detection
    uint8_t theremin_agreement_mm; // 5-50: max spread among k closest zones
    uint8_t ss_style;       // 0=Clip-Boy, 1=Blank, 2=Flying Clippy
    uint8_t ss_brightness;  // Clip-Boy screensaver dimness (1-100%)
    bool    ss_leds_off;    // Turn off LEDs when screensaver activates
    bool    ss_clock;       // Show the idle clock in the screensaver corner
    uint8_t tz;             // Time zone: index into cb_timezones[] (ui_nav.h)
    bool    led_rubber_duck; // Rubber Duck LED theme active (global chase effect)
    bool    crt_scanlines;  // CRT scanline overlay
    bool    crt_flicker;    // CRT flicker effect
    bool    help_btn;       // Show '?' help button in status bar
    bool    exclusive_input; // Lock the touchscreen while a serial ARG session is active
    bool    ss_unlock_tone; // Rising tone while holding to unlock the screensaver
    bool    scan_sound;     // Ambient sound loop while scanning an HR code
    bool    manual_confirm; // Verify-first: show the grid to confirm/correct before a scan unlocks
    bool    ui_click;       // Tap/click sound on buttons & controls (independent of Mute)
    uint8_t term_font;      // tool log font: 0=Small(14) 1=Medium(16) 2=Large(18)
    uint8_t deauth_chan;    // 0=hop 1-14, 200=hop 1/6/11, 1-14=lock. Radiation + Analyze>Deauth.
    uint8_t radio_drift;    // radio tuning drift: 0=Disabled 1=Once Per Boot 2=Every Access
    bool    allow_pcap;     // allow PCAP capture to SD/LittleFS (off Sn34k / on Res34rch)
    bool    legal_ack;      // legacy bool (true once any notice was accepted)
    uint8_t legal_ack_ver;  // policy version accepted (0=none); re-prompt when < LEGAL_POLICY_VERSION
    bool    clippy_seen;    // true = user has seen the Clippy first-boot intro
    uint8_t radio_viewed_mask;    // bit per station the user has opened/tuned (drives the reminder)
    uint8_t radio_announced_mask; // bit per station already announced by the "signal detected" modal
    bool    radio_reminder_off;   // "Never show again" chosen for the radio reminder nudge
    uint8_t boot_count;           // increments (capped) each boot; for the 2nd-boot reminder rule
    uint16_t custom_hue;          // completionist custom theme hue (0-359)
    bool    custom_unlock_seen;   // true once the 100%-collectibles reveal has fired
};

static ClipBoyConfig cfg;
static Preferences cfg_prefs;

// ─────────────────────── LOAD ─────────────────────────────────────────────

static void cfg_led_defaults(void) {
    // Default: Mojave amber chase (alternating amber + black)
    for (int i = 0; i < CFG_NUM_LEDS; i++) {
        cfg.leds[i].brightness = 180;
        if (i % 2 == 0) {
            cfg.leds[i].r = 255;
            cfg.leds[i].g = 176;
            cfg.leds[i].b = 0;
        } else {
            cfg.leds[i].r = 0;
            cfg.leds[i].g = 0;
            cfg.leds[i].b = 0;
        }
        cfg.leds[i].animation = 2;  // Chase
        cfg.leds[i].speed = 5;
    }
    memcpy(cfg.custom_leds, cfg.leds, sizeof(cfg.custom_leds));  // Custom defaults to the base look
    cfg.led_theme     = 0;       // Mojave (matches the default amber chase above)
    cfg.custom_seeded = false;   // first Custom pick will seed from whatever's live then
}

static void cfg_load(void) {
    cfg_prefs.begin(CFG_NAMESPACE, true);  // read-only

    cfg.theme          = cfg_prefs.getUChar("theme",      CFG_DEF_THEME);
    cfg.brightness     = cfg_prefs.getUChar("brightness", CFG_DEF_BRIGHTNESS);
    cfg.volume         = cfg_prefs.getUChar("volume",     CFG_DEF_VOLUME);
    cfg.disp_off       = cfg_prefs.getUChar("disp_off",   CFG_DEF_DISP_OFF);
    cfg.airplane       = cfg_prefs.getBool ("airplane",   CFG_DEF_AIRPLANE);
    cfg.sound          = cfg_prefs.getBool ("sound",      CFG_DEF_SOUND);

    // LED config - stored as a raw byte blob
    cfg_led_defaults();
    {
        size_t led_size = sizeof(cfg.leds);
        size_t got = cfg_prefs.getBytes("leds", cfg.leds, led_size);
        if (got != led_size && got != 0) {
            CB_LOGF("[CFG] LED blob size mismatch (%u != %u), using defaults\n",
                          (unsigned)got, (unsigned)led_size);
            cfg_led_defaults();
        }
    }
    // Custom LED theme: an independent store presets never overwrite, plus the
    // active theme id + the "seeded" flag.
    {
        size_t cs = sizeof(cfg.custom_leds);
        size_t cgot = cfg_prefs.getBytes("cleds", cfg.custom_leds, cs);
        if (cgot != cs && cgot != 0) memcpy(cfg.custom_leds, cfg.leds, cs);  // fallback: mirror live
    }
    cfg.led_theme     = cfg_prefs.getUChar("ledtheme", 0);
    cfg.custom_seeded = cfg_prefs.getBool ("cseed", false);

    // Theremin voices - stored as 8-byte blob
    memset(cfg.theremin_voices, CFG_DEF_THEREMIN_VOICE, CFG_NUM_THEREMIN_VOICES);
    {
        size_t got = cfg_prefs.getBytes("thr_voice", cfg.theremin_voices, CFG_NUM_THEREMIN_VOICES);
        if (got != CFG_NUM_THEREMIN_VOICES && got != 0) {
            CB_LOGF("[CFG] Theremin blob size mismatch (%u != %u), using defaults\n",
                          (unsigned)got, (unsigned)CFG_NUM_THEREMIN_VOICES);
            memset(cfg.theremin_voices, CFG_DEF_THEREMIN_VOICE, CFG_NUM_THEREMIN_VOICES);
        }
    }

    cfg.theremin_k            = cfg_prefs.getUChar("thr_k",     CFG_DEF_THEREMIN_K);
    cfg.theremin_agreement_mm = cfg_prefs.getUChar("thr_agree", CFG_DEF_THEREMIN_AGREE);

    cfg.ss_style = cfg_prefs.getUChar("ss_style", CFG_DEF_SS_STYLE);
    cfg.ss_brightness = cfg_prefs.getUChar("ss_bright", CFG_DEF_SS_BRIGHTNESS);
    cfg.ss_leds_off = cfg_prefs.getBool("ss_leds", CFG_DEF_SS_LEDS_OFF);
    cfg.ss_clock = cfg_prefs.getBool("ss_clock", CFG_DEF_SS_CLOCK);
    // Not range-clamped here: the table it indexes lives in ui_nav.h. cb_tz_posix()
    // falls back to CFG_DEF_TZ on an out-of-range value, which is what a downgrade
    // to a build with a shorter table would produce.
    cfg.tz = cfg_prefs.getUChar("tz", CFG_DEF_TZ);
    cfg.led_rubber_duck = cfg_prefs.getBool("rubduck", CFG_DEF_LED_RUBBER_DUCK);
    cfg.crt_scanlines = cfg_prefs.getBool("crt_scan", CFG_DEF_CRT_SCANLINES);
    cfg.crt_flicker   = cfg_prefs.getBool("crt_flick", CFG_DEF_CRT_FLICKER);
    cfg.help_btn      = cfg_prefs.getBool("help_btn", CFG_DEF_HELP_BTN);
    cfg.ss_unlock_tone = cfg_prefs.getBool("ss_unlocktone", CFG_DEF_SS_UNLOCK_TONE);
    cfg.scan_sound     = cfg_prefs.getBool("scansound", CFG_DEF_SCAN_SOUND);
    cfg.manual_confirm = cfg_prefs.getBool("manconf", CFG_DEF_MANUAL_CONFIRM);
    cfg.ui_click       = cfg_prefs.getBool("ui_click", CFG_DEF_UI_CLICK);
    cfg.term_font      = cfg_prefs.getUChar("term_font", CFG_DEF_TERM_FONT);
    cfg.deauth_chan    = cfg_prefs.getUChar("deauthchan", CFG_DEF_DEAUTH_CHAN);
    cfg.radio_drift    = cfg_prefs.getUChar("radiodrift", CFG_DEF_RADIO_DRIFT);
    cfg.allow_pcap     = cfg_prefs.getBool("allow_pcap", CFG_DEF_ALLOW_PCAP);
    cfg.exclusive_input = cfg_prefs.getBool("exinput", CFG_DEF_EXCLUSIVE_INPUT);
    cfg.legal_ack     = cfg_prefs.getBool("legal_ack", false);
    cfg.legal_ack_ver = cfg_prefs.getUChar("legal_ack_ver", 0);
    cfg.clippy_seen = cfg_prefs.getBool("clippy_seen", false);
    cfg.radio_viewed_mask    = cfg_prefs.getUChar("rad_viewed", 0);
    cfg.radio_announced_mask = cfg_prefs.getUChar("rad_annc", 0);
    cfg.radio_reminder_off   = cfg_prefs.getBool("rad_remoff", false);
    cfg.boot_count           = cfg_prefs.getUChar("boot_cnt", 0);
    cfg.custom_hue           = cfg_prefs.getUShort("customhue", 190);
    cfg.custom_unlock_seen   = cfg_prefs.getBool("cust_seen", false);

    cfg_prefs.end();

    if (cfg.custom_hue > 359) cfg.custom_hue = 190;
    g_custom_hue = cfg.custom_hue;   // feed the theme engine's active hue

    // Clamp values
    if (cfg.theme > THEME_CUSTOM) cfg.theme = 0;   // allow the custom (completionist) theme index
    if (cfg.brightness < 10)  cfg.brightness = 10;
    if (cfg.brightness > 100) cfg.brightness = 100;
    if (cfg.volume > 100)     cfg.volume = 100;
    if (cfg.disp_off > 5)     cfg.disp_off = CFG_DEF_DISP_OFF;
    if (cfg.term_font > 2)    cfg.term_font = CFG_DEF_TERM_FONT;
    // ALLOWLIST, deliberately not a range check: the 1/6/11 sentinel (200) sits ABOVE the
    // valid channel range, so a `> 14` test would reject it, and a `> 200` test would admit
    // every bogus value in between. A corrupt byte must fall back to the default rather than
    // become "locked to channel 14" -- i.e. a detection tool quietly deaf to 13 channels.
    if (!(cfg.deauth_chan == 0 || cfg.deauth_chan == 200 ||
          (cfg.deauth_chan >= 1 && cfg.deauth_chan <= 14)))
        cfg.deauth_chan = CFG_DEF_DEAUTH_CHAN;
    if (cfg.radio_drift > 2)  cfg.radio_drift = CFG_DEF_RADIO_DRIFT;

    // Clamp LED values - animation 0=None, 1=Breathe, 2=Chase
    for (int i = 0; i < CFG_NUM_LEDS; i++) {
        if (cfg.leds[i].animation > 2) cfg.leds[i].animation = 0;
        if (cfg.leds[i].speed < 1 || cfg.leds[i].speed > 10) cfg.leds[i].speed = 5;
    }

    // Clamp theremin voices
    for (int i = 0; i < CFG_NUM_THEREMIN_VOICES; i++) {
        if (cfg.theremin_voices[i] > 4) cfg.theremin_voices[i] = 0;
    }

    // Clamp theremin tuning
    if (cfg.theremin_k < 1)             cfg.theremin_k = 1;
    if (cfg.theremin_k > 8)             cfg.theremin_k = 8;
    if (cfg.theremin_agreement_mm < 5)  cfg.theremin_agreement_mm = 5;
    if (cfg.theremin_agreement_mm > 50) cfg.theremin_agreement_mm = 50;

    if (cfg.ss_style > 2) cfg.ss_style = CFG_DEF_SS_STYLE;  // 2=Flying Clippy (ARG reward)
    if (cfg.ss_brightness < 1)   cfg.ss_brightness = 1;
    if (cfg.ss_brightness > 100) cfg.ss_brightness = 100;

    CB_LOGF("[CFG] Loaded: theme=%d bright=%d vol=%d disp=%d air=%d snd=%d\n",
                  cfg.theme, cfg.brightness, cfg.volume, cfg.disp_off,
                  cfg.airplane, cfg.sound);
}

// ─────────────────────── SAVE (individual fields) ─────────────────────────
// Save only the changed field to minimize NVS wear.

static void cfg_save_theme(void) {
    cfg_prefs.begin(CFG_NAMESPACE, false);
    cfg_prefs.putUChar("theme", cfg.theme);
    cfg_prefs.end();
}

static void cfg_save_brightness(void) {
    cfg_prefs.begin(CFG_NAMESPACE, false);
    cfg_prefs.putUChar("brightness", cfg.brightness);
    cfg_prefs.end();
}

static void cfg_save_volume(void) {
    cfg_prefs.begin(CFG_NAMESPACE, false);
    cfg_prefs.putUChar("volume", cfg.volume);
    cfg_prefs.end();
}

static void cfg_save_disp_off(void) {
    cfg_prefs.begin(CFG_NAMESPACE, false);
    cfg_prefs.putUChar("disp_off", cfg.disp_off);
    cfg_prefs.end();
}

static void cfg_save_airplane(void) {
    cfg_prefs.begin(CFG_NAMESPACE, false);
    cfg_prefs.putBool("airplane", cfg.airplane);
    cfg_prefs.end();
}

static void cfg_save_exclusive_input(void) {
    cfg_prefs.begin(CFG_NAMESPACE, false);
    cfg_prefs.putBool("exinput", cfg.exclusive_input);
    cfg_prefs.end();
}

static void cfg_save_sound(void) {
    cfg_prefs.begin(CFG_NAMESPACE, false);
    cfg_prefs.putBool("sound", cfg.sound);
    cfg_prefs.end();
}

static void cfg_save_ss_style(void) {
    cfg_prefs.begin(CFG_NAMESPACE, false);
    cfg_prefs.putUChar("ss_style", cfg.ss_style);
    cfg_prefs.end();
}

static void cfg_save_ss_brightness(void) {
    cfg_prefs.begin(CFG_NAMESPACE, false);
    cfg_prefs.putUChar("ss_bright", cfg.ss_brightness);
    cfg_prefs.end();
}

static void cfg_save_ss_leds_off(void) {
    cfg_prefs.begin(CFG_NAMESPACE, false);
    cfg_prefs.putBool("ss_leds", cfg.ss_leds_off);
    cfg_prefs.end();
}

static void cfg_save_ss_clock(void) {
    cfg_prefs.begin(CFG_NAMESPACE, false);
    cfg_prefs.putBool("ss_clock", cfg.ss_clock);
    cfg_prefs.end();
}

static void cfg_save_tz(void) {
    cfg_prefs.begin(CFG_NAMESPACE, false);
    cfg_prefs.putUChar("tz", cfg.tz);
    cfg_prefs.end();
}

static void cfg_save_led_rubber_duck(void) {
    cfg_prefs.begin(CFG_NAMESPACE, false);
    cfg_prefs.putBool("rubduck", cfg.led_rubber_duck);
    cfg_prefs.end();
}

static void cfg_save_crt_scanlines(void) {
    cfg_prefs.begin(CFG_NAMESPACE, false);
    cfg_prefs.putBool("crt_scan", cfg.crt_scanlines);
    cfg_prefs.end();
}

static void cfg_save_crt_flicker(void) {
    cfg_prefs.begin(CFG_NAMESPACE, false);
    cfg_prefs.putBool("crt_flick", cfg.crt_flicker);
    cfg_prefs.end();
}

static void cfg_save_ss_unlock_tone(void) {
    cfg_prefs.begin(CFG_NAMESPACE, false);
    cfg_prefs.putBool("ss_unlocktone", cfg.ss_unlock_tone);
    cfg_prefs.end();
}
static void cfg_save_scan_sound(void) {
    cfg_prefs.begin(CFG_NAMESPACE, false);
    cfg_prefs.putBool("scansound", cfg.scan_sound);
    cfg_prefs.end();
}
static void cfg_save_manual_confirm(void) {
    cfg_prefs.begin(CFG_NAMESPACE, false);
    cfg_prefs.putBool("manconf", cfg.manual_confirm);
    cfg_prefs.end();
}

static void cfg_save_ui_click(void) {
    cfg_prefs.begin(CFG_NAMESPACE, false);
    cfg_prefs.putBool("ui_click", cfg.ui_click);
    cfg_prefs.end();
}

static void cfg_save_deauth_chan(void) {
    cfg_prefs.begin(CFG_NAMESPACE, false);
    cfg_prefs.putUChar("deauthchan", cfg.deauth_chan);   // key <= 15 chars (Preferences limit)
    cfg_prefs.end();
}

static void cfg_save_term_font(void) {
    cfg_prefs.begin(CFG_NAMESPACE, false);
    cfg_prefs.putUChar("term_font", cfg.term_font);
    cfg_prefs.end();
}

static void cfg_save_radio_drift(void) {
    cfg_prefs.begin(CFG_NAMESPACE, false);
    cfg_prefs.putUChar("radiodrift", cfg.radio_drift);
    cfg_prefs.end();
}

static void cfg_save_allow_pcap(void) {
    cfg_prefs.begin(CFG_NAMESPACE, false);
    cfg_prefs.putBool("allow_pcap", cfg.allow_pcap);
    cfg_prefs.end();
}

static void cfg_save_help_btn(void) {
    cfg_prefs.begin(CFG_NAMESPACE, false);
    cfg_prefs.putBool("help_btn", cfg.help_btn);
    cfg_prefs.end();
}

static void cfg_save_theremin_voices(void) {
    cfg_prefs.begin(CFG_NAMESPACE, false);
    cfg_prefs.putBytes("thr_voice", cfg.theremin_voices, CFG_NUM_THEREMIN_VOICES);
    cfg_prefs.end();
    CB_LOGLN("[CFG] Theremin voices saved");
}

static void cfg_save_theremin_k(void) {
    cfg_prefs.begin(CFG_NAMESPACE, false);
    cfg_prefs.putUChar("thr_k", cfg.theremin_k);
    cfg_prefs.end();
}

static void cfg_save_theremin_agreement(void) {
    cfg_prefs.begin(CFG_NAMESPACE, false);
    cfg_prefs.putUChar("thr_agree", cfg.theremin_agreement_mm);
    cfg_prefs.end();
}

static void cfg_save_leds(void) {
    cfg_prefs.begin(CFG_NAMESPACE, false);
    cfg_prefs.putBytes("leds", cfg.leds, sizeof(cfg.leds));
    cfg_prefs.putBytes("cleds", cfg.custom_leds, sizeof(cfg.custom_leds));
    cfg_prefs.putUChar("ledtheme", cfg.led_theme);
    cfg_prefs.putBool ("cseed", cfg.custom_seeded);
    cfg_prefs.end();
    CB_LOGLN("[CFG] LED settings saved");
}

// ─────────────────────── SAVE ALL ─────────────────────────────────────────
// For bulk operations (e.g. factory reset restore).

static void cfg_save_all(void) {
    cfg_prefs.begin(CFG_NAMESPACE, false);
    cfg_prefs.putUChar("theme",      cfg.theme);
    cfg_prefs.putUChar("brightness", cfg.brightness);
    cfg_prefs.putUChar("volume",     cfg.volume);
    cfg_prefs.putUChar("disp_off",   cfg.disp_off);
    cfg_prefs.putBool ("airplane",   cfg.airplane);
    cfg_prefs.putBool ("sound",      cfg.sound);
    cfg_prefs.putBytes("leds",       cfg.leds, sizeof(cfg.leds));
    cfg_prefs.putBytes("cleds",      cfg.custom_leds, sizeof(cfg.custom_leds));
    cfg_prefs.putUChar("ledtheme",   cfg.led_theme);
    cfg_prefs.putBool ("cseed",      cfg.custom_seeded);
    cfg_prefs.putBytes("thr_voice",  cfg.theremin_voices, CFG_NUM_THEREMIN_VOICES);
    cfg_prefs.putUChar("thr_k",      cfg.theremin_k);
    cfg_prefs.putUChar("thr_agree",  cfg.theremin_agreement_mm);
    cfg_prefs.putUChar("ss_style",   cfg.ss_style);
    cfg_prefs.putUChar("ss_bright",  cfg.ss_brightness);
    cfg_prefs.putBool ("ss_leds",   cfg.ss_leds_off);
    cfg_prefs.putBool ("ss_clock",  cfg.ss_clock);
    cfg_prefs.putUChar("tz",        cfg.tz);
    cfg_prefs.putBool ("rubduck",   cfg.led_rubber_duck);
    cfg_prefs.putBool ("crt_scan",  cfg.crt_scanlines);
    cfg_prefs.putBool ("crt_flick", cfg.crt_flicker);
    cfg_prefs.putBool ("help_btn",  cfg.help_btn);
    cfg_prefs.putBool ("ss_unlocktone", cfg.ss_unlock_tone);
    cfg_prefs.putBool ("scansound",    cfg.scan_sound);
    cfg_prefs.putBool ("manconf",      cfg.manual_confirm);
    cfg_prefs.putBool ("ui_click",  cfg.ui_click);
    cfg_prefs.putUChar("term_font", cfg.term_font);
    cfg_prefs.putUChar("deauthchan", cfg.deauth_chan);
    cfg_prefs.putUChar("radiodrift", cfg.radio_drift);
    cfg_prefs.putBool ("allow_pcap", cfg.allow_pcap);
    cfg_prefs.putBool ("exinput",   cfg.exclusive_input);
    cfg_prefs.putUChar("rad_viewed", cfg.radio_viewed_mask);
    cfg_prefs.putUChar("rad_annc",   cfg.radio_announced_mask);
    cfg_prefs.putBool ("rad_remoff", cfg.radio_reminder_off);
    cfg_prefs.putUShort("customhue", cfg.custom_hue);
    cfg_prefs.putBool ("cust_seen", cfg.custom_unlock_seen);
    cfg_prefs.end();
    CB_LOGLN("[CFG] All settings saved");
}

static void cfg_save_legal_ack(void) {
    cfg_prefs.begin(CFG_NAMESPACE, false);
    cfg_prefs.putBool("legal_ack", cfg.legal_ack);
    cfg_prefs.putUChar("legal_ack_ver", cfg.legal_ack_ver);
    cfg_prefs.end();
}

static void cfg_save_clippy_seen(void) {
    cfg_prefs.begin(CFG_NAMESPACE, false);
    cfg_prefs.putBool("clippy_seen", cfg.clippy_seen);
    cfg_prefs.end();
}

// Radio "signal detected" discovery state (viewed / announced / reminder opt-out).
static void cfg_save_radio_discovery(void) {
    cfg_prefs.begin(CFG_NAMESPACE, false);
    cfg_prefs.putUChar("rad_viewed", cfg.radio_viewed_mask);
    cfg_prefs.putUChar("rad_annc",   cfg.radio_announced_mask);
    cfg_prefs.putBool ("rad_remoff", cfg.radio_reminder_off);
    cfg_prefs.end();
}

// Completionist custom theme: chosen hue + the "reveal already fired" flag.
static void cfg_save_custom_theme(void) {
    cfg_prefs.begin(CFG_NAMESPACE, false);
    cfg_prefs.putUShort("customhue", cfg.custom_hue);
    cfg_prefs.putBool  ("cust_seen", cfg.custom_unlock_seen);
    cfg_prefs.end();
}

// Call once from setup() after cfg_load(): bump the (capped) boot counter so the
// "2nd boot" reminder rule can fire, and persist it.
static void cfg_boot_tick(void) {
    if (cfg.boot_count < 250) cfg.boot_count++;
    cfg_prefs.begin(CFG_NAMESPACE, false);
    cfg_prefs.putUChar("boot_cnt", cfg.boot_count);
    cfg_prefs.end();
}

// ─────────────────────── FACTORY RESET ────────────────────────────────────

static void wifi_creds_wipe_all(void);  // defined in the SAVED WIFI NETWORKS section below

static void cfg_factory_reset(void) {
    cfg_prefs.begin(CFG_NAMESPACE, false);
    cfg_prefs.clear();
    cfg_prefs.end();

    cfg.theme          = CFG_DEF_THEME;
    cfg.brightness     = CFG_DEF_BRIGHTNESS;
    cfg.volume         = CFG_DEF_VOLUME;
    cfg.disp_off       = CFG_DEF_DISP_OFF;
    cfg.airplane       = CFG_DEF_AIRPLANE;
    cfg.sound          = CFG_DEF_SOUND;
    cfg_led_defaults();
    memset(cfg.theremin_voices, CFG_DEF_THEREMIN_VOICE, CFG_NUM_THEREMIN_VOICES);
    cfg.theremin_k            = CFG_DEF_THEREMIN_K;
    cfg.theremin_agreement_mm = CFG_DEF_THEREMIN_AGREE;
    cfg.ss_style = CFG_DEF_SS_STYLE;
    cfg.ss_brightness = CFG_DEF_SS_BRIGHTNESS;
    cfg.ss_leds_off = CFG_DEF_SS_LEDS_OFF;
    cfg.ss_clock = CFG_DEF_SS_CLOCK;
    cfg.tz = CFG_DEF_TZ;
    cfg.led_rubber_duck = CFG_DEF_LED_RUBBER_DUCK;
    cfg.crt_scanlines = CFG_DEF_CRT_SCANLINES;
    cfg.crt_flicker   = CFG_DEF_CRT_FLICKER;
    cfg.help_btn      = CFG_DEF_HELP_BTN;
    cfg.ss_unlock_tone = CFG_DEF_SS_UNLOCK_TONE;
    cfg.scan_sound     = CFG_DEF_SCAN_SOUND;
    cfg.manual_confirm = CFG_DEF_MANUAL_CONFIRM;
    cfg.ui_click       = CFG_DEF_UI_CLICK;
    cfg.term_font      = CFG_DEF_TERM_FONT;
    cfg.deauth_chan    = CFG_DEF_DEAUTH_CHAN;
    cfg.radio_drift    = CFG_DEF_RADIO_DRIFT;
    cfg.allow_pcap     = CFG_DEF_ALLOW_PCAP;
    cfg.radio_viewed_mask    = 0;   // fresh radio discovery state
    cfg.radio_announced_mask = 0;
    cfg.radio_reminder_off   = false;
    cfg.custom_hue           = 190;
    cfg.custom_unlock_seen   = false;
    g_custom_hue             = cfg.custom_hue;
    // boot_count is intentionally NOT reset (a factory reset is still a boot)

    // Sanitize saved WiFi creds too. They live in separate NVS namespaces (so a
    // normal settings tweak doesn't lose them), but "Reset All Settings" MUST
    // wipe them -- otherwise a plaintext home/corp PSK survives a pre-resale
    // reset and is recoverable from a flash dump.
    wifi_creds_wipe_all();

    CB_LOGLN("[CFG] Factory reset");
}

// ─────────────────────── SAVED WIFI NETWORKS ──────────────────────────────
// Stored in separate NVS namespace so factory reset doesn't wipe them.
// Max 8 networks, each SSID (33 bytes) + password (65 bytes) = 98 bytes.

#define WIFI_CRED_MAX       8
#define WIFI_CRED_NAMESPACE "wifi_creds"

struct WiFiCred {
    char ssid[33];
    char pw[65];
};

static WiFiCred wifi_creds[WIFI_CRED_MAX];
static uint8_t  wifi_cred_count = 0;

static void wifi_creds_load(void) {
    Preferences p;
    p.begin(WIFI_CRED_NAMESPACE, true);
    wifi_cred_count = p.getUChar("count", 0);
    if (wifi_cred_count > WIFI_CRED_MAX) wifi_cred_count = 0;
    if (wifi_cred_count > 0) {
        size_t expect = wifi_cred_count * sizeof(WiFiCred);
        size_t got = p.getBytes("list", wifi_creds, expect);
        if (got != expect) {
            CB_LOGF("[WIFI] Cred blob mismatch (%u != %u), clearing\n",
                          (unsigned)got, (unsigned)expect);
            wifi_cred_count = 0;
        }
    }
    p.end();
    CB_LOGF("[WIFI] Loaded %d saved networks\n", wifi_cred_count);
}

static void wifi_creds_save(void) {
    Preferences p;
    p.begin(WIFI_CRED_NAMESPACE, false);
    p.putUChar("count", wifi_cred_count);
    if (wifi_cred_count > 0)
        p.putBytes("list", wifi_creds, wifi_cred_count * sizeof(WiFiCred));
    else
        p.remove("list");
    p.end();
    CB_LOGF("[WIFI] Saved %d networks\n", wifi_cred_count);
}

static int wifi_creds_find(const char *ssid) {
    for (int i = 0; i < wifi_cred_count; i++)
        if (strncmp(wifi_creds[i].ssid, ssid, 32) == 0) return i;
    return -1;
}

static bool wifi_creds_add(const char *ssid, const char *pw) {
    // Update existing or add new
    int idx = wifi_creds_find(ssid);
    if (idx < 0) {
        if (wifi_cred_count >= WIFI_CRED_MAX) return false;  // full
        idx = wifi_cred_count++;
    }
    strncpy(wifi_creds[idx].ssid, ssid, 32);
    wifi_creds[idx].ssid[32] = '\0';
    strncpy(wifi_creds[idx].pw, pw, 64);
    wifi_creds[idx].pw[64] = '\0';
    wifi_creds_save();
    return true;
}

static void wifi_creds_remove(int idx) {
    if (idx < 0 || idx >= wifi_cred_count) return;
    for (int i = idx; i < wifi_cred_count - 1; i++)
        wifi_creds[i] = wifi_creds[i + 1];
    wifi_cred_count--;
    wifi_creds_save();
}

static void wifi_creds_clear(void) {
    wifi_cred_count = 0;
    wifi_creds_save();
}

// Full sanitize for factory reset: erase the saved-networks namespace AND any
// one-shot "wifi_join" creds from NVS, and zero the in-RAM copy. Uses clear()
// (not the per-key remove in wifi_creds_save) so no plaintext PSK lingers.
static void wifi_creds_wipe_all(void) {
    Preferences p;
    p.begin(WIFI_CRED_NAMESPACE, false); p.clear(); p.end();
    p.begin("wifi_join", false);         p.clear(); p.end();
    memset(wifi_creds, 0, sizeof(wifi_creds));
    wifi_cred_count = 0;
    CB_LOGLN("[WIFI] Saved networks + join creds wiped");
}
