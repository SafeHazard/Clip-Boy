#pragma once
// arg_core.h — Clip-Boy DC34 ARG: core state, NVRAM schema, per-badge MAC
// derivation, and the Quanta unlock gate. See `puzzle handoff.md` (source of
// truth). Header-only static, matching project convention.
//
// Integrity model (handoff §8): the unlock that matters is enforced by the
// per-badge HMAC at P5 (can't forge another badge's unlock), NOT by hiding
// commands. A user who flashes their own debug build can only rearrange flags
// on a badge they already own and still cannot forge P5. Public source is fine.

#include <Preferences.h>
#include "esp_mac.h"   // esp_efuse_mac_get_default — eFuse base MAC (per-badge spine)

// ─── Progress bitmask (handoff §2.1) ───────────────────────────────────────
#define ARG_P1_RADIO    0x01
#define ARG_P2_HACK     0x02
#define ARG_P3_DORK     0x04
#define ARG_P4_CAPTCHA  0x08
#define ARG_P5_PHONE    0x10
#define ARG_ALL_COMPLETE 0x1F

// theme_active enum (handoff §9 default) — TODO(data): confirm enum values.
#define ARG_THEME_DEFAULT 0
#define ARG_THEME_QUANTA  1
#define ARG_THEME_ZENITH  2

#define ARG_NVS_NS "arg"   // Preferences namespace (P5 crypto uses "arg_unlock")

// ─── Persistent state (handoff §2.1) ───────────────────────────────────────
// Scalar keys live here; the big blobs (p3 dork save-state, p5 challenge) are
// owned + (de)serialized by their puzzle modules to keep this struct small.
struct ArgState {
    uint8_t  progress;            // completion bitmask
    uint8_t  discovered;          // 0=undiscovered, 1=startgame run >=1
    uint8_t  radio_dismiss_count; // radio modal dismissals (opt-out logic)
    uint8_t  abandoned;           // 0=on path, 1=opted out (grants Zenith)
    uint8_t  theme_active;        // 0=Default, 1=Quanta, 2=Zenith
    uint16_t p4_correct;          // P4 lifetime CORRECT answers (unlock at 256)
    uint16_t p4_answered;         // P4 lifetime non-empty answers (GLaDOS jab at 34)
};
static ArgState arg = {0, 0, 0, 0, ARG_THEME_DEFAULT, 0, 0};

static Preferences arg_prefs;
static bool arg_duck_unlocked = false;  // Rubber Ducky LED theme earned (P3 full-exploration reward)
// A puzzle owns the serial line stream (interactive). Declared here (not in
// arg_clipcli.h) so the UI's exclusive-input touch-lock (ui_nav.h) can read it.
static bool arg_session_active = false;

// ─── eFuse base MAC — the per-badge anti-replay spine (handoff §2.8) ────────
// All per-badge derivation hangs off these 6 bytes. Read once, cached.
static uint8_t arg_mac_buf[6] = {0};
static bool    arg_mac_loaded = false;

static const uint8_t *arg_mac(void) {
    if (!arg_mac_loaded) {
        if (esp_efuse_mac_get_default(arg_mac_buf) != ESP_OK)
            esp_read_mac(arg_mac_buf, ESP_MAC_WIFI_STA);  // fallback (derived, still per-badge)
        arg_mac_loaded = true;
    }
    return arg_mac_buf;
}

// P1 short-ID: "last group of Echo's hex = MAC[0:2]" (handoff §2.8/§4.2).
static uint16_t arg_p1_shortid(void) {
    const uint8_t *m = arg_mac();
    return (uint16_t)((m[0] << 8) | m[1]);
}

// A stable per-badge 32-bit seed for content selection (P2 word/bracket roles,
// P3 passphrase). Mixes all 6 MAC bytes (FNV-1a) so adjacent badges diverge.
static uint32_t arg_mac_seed(void) {
    const uint8_t *m = arg_mac();
    uint32_t h = 2166136261u;
    for (int i = 0; i < 6; i++) { h ^= m[i]; h *= 16777619u; }
    return h ? h : 0xA5A5A5A5u;  // never 0 (some PRNGs dislike a 0 seed)
}

// Per-badge P3 passphrase: pronounceable CVCVCV from MAC bytes 3-5 (handoff §2.8).
// Lives here (not arg_p3_dork.h) so the L.E.E.T. "issued callsign" display can also
// show it — the canonical, always-on source a stuck player reads when Queue points
// them at the L.E.E.T. screen. Single source of truth shared with p3_say()'s check.
static void p3_passphrase(char out[8]) {
    const uint8_t *m = arg_mac();
    static const char C[] = "BDFGKLMNPRST", V[] = "AEIOU";
    out[0]=C[m[3]%12]; out[1]=V[(m[3]/12)%5];
    out[2]=C[m[4]%12]; out[3]=V[(m[4]/12)%5];
    out[4]=C[m[5]%12]; out[5]=V[(m[5]/12)%5];
    out[6]='\0';
}

// ─── NVRAM load / save (handoff §2.1: survive reboot + serial disconnect) ───
static void arg_load(void) {
    arg_prefs.begin(ARG_NVS_NS, true);   // read-only
    arg.progress            = arg_prefs.getUChar ("progress",  0);
    arg.discovered          = arg_prefs.getUChar ("discovered", 0);
    arg.radio_dismiss_count = arg_prefs.getUChar ("radiodism", 0);
    arg.abandoned           = arg_prefs.getUChar ("abandoned", 0);
    arg.theme_active        = arg_prefs.getUChar ("theme",     ARG_THEME_DEFAULT);
    arg.p4_correct          = arg_prefs.getUShort("p4ok",      0);
    arg.p4_answered         = arg_prefs.getUShort("p4ans",     0);
    arg_duck_unlocked       = arg_prefs.getUChar ("duck",      0);
    arg_prefs.end();
}

// Write a single scalar field by re-opening RW (Preferences keys are tiny; we
// write progress flags immediately on each completion per §2.1).
static void arg_save_u8(const char *key, uint8_t v)   { arg_prefs.begin(ARG_NVS_NS, false); arg_prefs.putUChar(key, v);  arg_prefs.end(); }
static void arg_save_u16(const char *key, uint16_t v) { arg_prefs.begin(ARG_NVS_NS, false); arg_prefs.putUShort(key, v); arg_prefs.end(); }

// Wipe ALL ARG state for a factory reset: puzzle progress ("arg") + P5 crypto
// state ("arg_unlock"). Without this, arg_quanta_earned() survives a reset, so the
// Quanta theme + Flying Clippy screensaver (both gated on it) linger post-nuke.
// The badge reboots right after, so in-memory state reloads fresh from the cleared NVS.
static void arg_factory_reset(void) {
    arg_prefs.begin(ARG_NVS_NS, false); arg_prefs.clear(); arg_prefs.end();
    Preferences u; u.begin("arg_unlock", false); u.clear(); u.end();
}

// ─── Flag operations ───────────────────────────────────────────────────────
static inline bool arg_flag(uint8_t mask)      { return (arg.progress & mask) == mask; }
static inline bool arg_all_complete(void)      { return arg.progress == ARG_ALL_COMPLETE; }

// Fired ONCE when the set first reaches all-complete, in ANY order — so a player
// who finishes P4 last (after entering P5 early) still gets the Quanta payoff.
// Wired to arg_apply_quanta_reward by p5_register().
static void (*arg_on_complete_fn)(void) = nullptr;

static void arg_set_flag(uint8_t mask) {
    if ((arg.progress & mask) == mask) return;   // idempotent
    bool was_complete = arg_all_complete();
    arg.progress |= mask;
    arg_save_u8("progress", arg.progress);        // immediate persist (§2.1)
    if (!was_complete && arg_all_complete() && arg_on_complete_fn)
        arg_on_complete_fn();                     // reward fires even on out-of-order finish
}

static void arg_clear_flag(uint8_t mask) {
    if (!(arg.progress & mask)) return;
    arg.progress &= (uint8_t)~mask;
    arg_save_u8("progress", arg.progress);
}

static void arg_set_discovered(void) {
    if (arg.discovered) return;
    arg.discovered = 1;
    arg_save_u8("discovered", 1);
}

static void arg_set_theme(uint8_t t) {
    if (arg.theme_active == t) return;
    arg.theme_active = t;
    arg_save_u8("theme", t);
}

// ─── Quanta unlock gate (handoff §2.1/§7.5) ────────────────────────────────
// The bitmask alone never unlocks: P5's bit is set ONLY after a valid per-badge
// HMAC handshake (arg_p5_*), so progress==0x1F already implies the handshake.
// arg_quanta_earned() is the single source of truth the theme/LED gating reads.
static bool arg_quanta_earned(void) {
    return arg_all_complete();   // P5 bit ⇒ HMAC validated (see arg_p5_call.h)
}

// Rubber Ducky LED theme: earned by visiting ALL four P3 dead-ends (the exploration
// reward Queue grants — see arg_p3_dork.h p3_win). Gates the "Rubber Duck" LED preset
// the same way arg_quanta_earned() gates "Quanta".
static bool arg_duck_earned(void) { return arg_duck_unlocked; }
static void arg_set_duck(void) {
    if (!arg_duck_unlocked) { arg_duck_unlocked = true; arg_save_u8("duck", 1); }
}

// Convenience for the §2.4 `reset all`: wipe progress + scalar substate.
// (Puzzle blobs are cleared by their own modules — see arg_reset_all() wiring.)
static void arg_reset_scalars(void) {
    arg.progress = 0; arg.discovered = 0; arg.radio_dismiss_count = 0;
    arg.abandoned = 0; arg.p4_correct = 0; arg.p4_answered = 0;
    arg.theme_active = ARG_THEME_DEFAULT;
    arg_duck_unlocked = false;
    arg_prefs.begin(ARG_NVS_NS, false);
    arg_prefs.putUChar("progress", 0);   arg_prefs.putUChar("discovered", 0);
    arg_prefs.putUChar("radiodism", 0);  arg_prefs.putUChar("abandoned", 0);
    arg_prefs.putUChar("theme", ARG_THEME_DEFAULT);
    arg_prefs.putUShort("p4ok", 0);      arg_prefs.putUShort("p4ans", 0);
    arg_prefs.putUChar("duck", 0);
    arg_prefs.end();
}

static void arg_core_init(void) {
    arg_load();
    (void)arg_mac();   // warm the cache early (before any per-badge derivation)
}
