#pragma once
// ═══════════════ Clip-Boy ARG finale — badge-side unlock subsystem ═══════════
// Spec: "CLIP-BOY ARG UNLOCK HANDOFF.md". Crypto core (MUST match the phone
// system byte-for-byte):
//   code = HMAC-SHA256(secret32, "%04u"(nonce))         // nonce hashed as 4
//          first 4 bytes -> BE uint32 -> % 100000000 -> //   ASCII digits, no NUL
//          "%08u"                                        // zero-padded to 8
// The 32-byte secret is injected via the gitignored secret.h (64-char hex);
// secret.h.example documents the requirement. A fresh clone falls back to the
// example so it still compiles (codes won't match the live system).
//
// This header carries the CRYPTO + STATE/LOCKOUT logic. The LVGL numpad UI and
// theme-unlock wiring live in ui_nav.h (separate). Construction is verified by
// arg_self_test() against a TEST key cross-checked with Python hmac, so when the
// real key is injected the §8 acceptance vectors follow by construction.

#include <Arduino.h>
#include <Preferences.h>
#include <esp_random.h>
#include "mbedtls/md.h"

// Key selection, highest priority first:
//   1. -DARG_HMAC_SECRET_HEX_RAW=<64 unquoted hex chars>  (release builds)
//   2. the gitignored real secret.h                        (local dev builds)
//   3. secret.h.example, the all-zero placeholder          (public/clean clones)
//
// (1) exists because the PUBLISHED source trees intentionally ship the all-zero
// placeholder (the key must never land in the GPLv3 source), but the SHIPPED
// BINARIES must carry the real key or the P5 phone unlock can never validate --
// the badge would verify against zeros while the live IVR uses the real secret.
// Injecting at COMPILE time keeps the key out of the staged/published tree
// entirely. The token is passed UNQUOTED (no shell/arduino-cli quote escaping)
// and stringified here.
#if defined(ARG_HMAC_SECRET_HEX_RAW)
  #define ARG_SECRET_STR2(s) #s
  #define ARG_SECRET_STR(s)  ARG_SECRET_STR2(s)
  #define ARG_HMAC_SECRET_HEX ARG_SECRET_STR(ARG_HMAC_SECRET_HEX_RAW)
#elif __has_include("secret.h")
  #include "secret.h"
#else
  #include "secret.h.example"
#endif

// Post-con sunset build gates the offline "No phone?" self-unlock button. The
// key ships in BOTH builds (offline verify needs it); only the button is gated.
#ifndef CLIPBOY_POST_CON
#define CLIPBOY_POST_CON 0
#endif

// ─────────────────────────── crypto core ───────────────────────────────────

static int arg_hexnib(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static size_t arg_hexdecode(const char *hex, uint8_t *out, size_t cap) {
    size_t n = 0;
    for (size_t i = 0; hex[i] && hex[i + 1] && n < cap; i += 2) {
        int hi = arg_hexnib(hex[i]), lo = arg_hexnib(hex[i + 1]);
        if (hi < 0 || lo < 0) break;
        out[n++] = (uint8_t)((hi << 4) | lo);
    }
    return n;
}

// Core construction (matches the verified server-side JS). out_code >= 9 bytes.
static void arg_compute(const uint8_t *secret, size_t slen,
                        const char *nonce_ascii, size_t nlen, char out_code[9]) {
    uint8_t digest[32];
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_hmac(info, secret, slen, (const uint8_t *)nonce_ascii, nlen, digest);
    uint32_t u32 = ((uint32_t)digest[0] << 24) | ((uint32_t)digest[1] << 16) |
                   ((uint32_t)digest[2] << 8)  | ((uint32_t)digest[3]);
    uint32_t code = u32 % 100000000u;
    snprintf(out_code, 9, "%08u", code);
}

// Real-secret code for a nonce value (0-9999). Decodes the hex key once.
static void arg_code_for_nonce(uint16_t nonce, char out_code[9]) {
    static uint8_t key[32];
    static size_t  klen = 0;
    if (!klen) klen = arg_hexdecode(ARG_HMAC_SECRET_HEX, key, sizeof key);
    char nb[5];
    snprintf(nb, sizeof nb, "%04u", (unsigned)(nonce % 10000));  // zero-padded, exactly 4
    arg_compute(key, klen, nb, 4, out_code);
}

// Constant-time 8-char compare (avoid first-mismatch early-exit timing leak).
static bool arg_ct_equal8(const char *a, const char *b) {
    uint8_t diff = 0;
    for (int i = 0; i < 8; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

// ─────────────────── state + NVS persistence (§4) ──────────────────────────
// Namespace "arg_unlock": nonce(u16), attempts(u8), unlocked(u8).

static Preferences arg_nvs;
static uint16_t arg_nonce    = 0;
static uint8_t  arg_attempts = 0;
static bool     arg_unlocked = false;
static uint32_t arg_cooldown_until = 0;   // millis() target; in-session only (no RTC)

static void arg_init(void) {
    arg_nvs.begin("arg_unlock", true);   // read-only first
    bool has_nonce = arg_nvs.isKey("nonce");
    arg_nonce    = arg_nvs.getUShort("nonce", 0);
    arg_attempts = arg_nvs.getUChar("attempts", 0);
    arg_unlocked = arg_nvs.getUChar("unlocked", 0) != 0;
    arg_nvs.end();
    // First time reaching the system: draw one nonce and persist it so it
    // survives walk-away/reboot (the user may call later). Never regenerate
    // except on first-ever or after a successful unlock (§3).
    if (!has_nonce) {
        arg_nonce = (uint16_t)(esp_random() % 10000);
        arg_nvs.begin("arg_unlock", false);
        arg_nvs.putUShort("nonce", arg_nonce);
        arg_nvs.end();
    }
}

static void arg_set_unlocked(void) {
    arg_unlocked = true;
    arg_attempts = 0;
    arg_nvs.begin("arg_unlock", false);
    arg_nvs.putUChar("unlocked", 1);
    arg_nvs.putUChar("attempts", 0);
    arg_nvs.end();
}

// ─────────────────── lockout (§5) — gentle, escalating, NEVER permanent ─────
// 1-5 wrong: immediate retry. Then escalating in-session cooldown: 30s, 2m, 5m,
// hold 5m. Count persists (a power-cycle skips the current wait but not the
// count, so the next wrong attempt re-enters cooldown). No permanent lockout.

static uint32_t arg_cooldown_ms_for(uint8_t attempts) {
    if (attempts < 5) return 0;
    if (attempts == 5) return 30u  * 1000u;
    if (attempts == 6) return 120u * 1000u;
    return 300u * 1000u;   // hold at 5 min
}
static uint32_t arg_cooldown_remaining(void) {
    uint32_t now = millis();
    return (arg_cooldown_until > now) ? (arg_cooldown_until - now) : 0;
}

// Try an 8-char entered code against the current nonce. Returns true on unlock.
// On failure, increments+persists the attempt count and arms the cooldown.
static bool arg_try_code(const char *entered) {
    char expect[9];
    arg_code_for_nonce(arg_nonce, expect);
    if (entered && strlen(entered) == 8 && arg_ct_equal8(entered, expect)) {
        arg_set_unlocked();
        return true;
    }
    if (arg_attempts < 255) arg_attempts++;
    arg_nvs.begin("arg_unlock", false);
    arg_nvs.putUChar("attempts", arg_attempts);
    arg_nvs.end();
    arg_cooldown_until = millis() + arg_cooldown_ms_for(arg_attempts);
    return false;
}

// ─────────────────── construction self-test (§8 / §8.5) ─────────────────────
// Uses a TEST key (bytes 0x00..0x1f), NOT the real secret, so it proves the
// byte-encoding/truncation/padding are correct independent of the credential.
// Expected values were cross-checked with Python `hmac` on the host. If these
// pass, the real-key §8 acceptance vectors pass by construction.
static int arg_self_test(void) {
    uint8_t tk[32];
    for (int i = 0; i < 32; i++) tk[i] = (uint8_t)i;
    struct { const char *n; const char *e; } v[] = {
        {"0000", "80301119"}, {"0001", "73310772"}, {"0042", "48412927"},
        {"1234", "73596262"}, {"4471", "89471855"}, {"9999", "49023041"},
    };
    int fails = 0; char out[9];
    for (auto &t : v) {
        arg_compute(tk, 32, t.n, strlen(t.n), out);
        bool ok = (strcmp(out, t.e) == 0);
        Serial.printf("[ARG] testkey nonce %s -> %s expect %s [%s]\n",
                      t.n, out, t.e, ok ? "PASS" : "FAIL");
        if (!ok) fails++;
    }
    // Canary: the unpadded "42" must NOT collide with padded "0042".
    arg_compute(tk, 32, "42", 2, out);
    bool canary = (strcmp(out, "48412927") != 0) && (strcmp(out, "13505682") == 0);
    Serial.printf("[ARG] canary '42' -> %s (!= 0042) [%s]\n", out, canary ? "OK" : "FAIL");
    if (!canary) fails++;
    // Real-secret sanity: show whether secret.h is still the zeroed placeholder.
    {
        uint8_t k[32]; size_t kl = arg_hexdecode(ARG_HMAC_SECRET_HEX, k, sizeof k);
        bool zeroed = true;
        for (size_t i = 0; i < kl; i++) if (k[i]) { zeroed = false; break; }
        Serial.printf("[ARG] secret.h: %s (%u bytes)\n",
                      zeroed ? "PLACEHOLDER (zeroed) - inject real key to go live"
                             : "non-zero key present", (unsigned)kl);
    }
    Serial.printf("[ARG] self-test: %s\n", fails ? "SOME FAILED" : "ALL PASS");
    return fails;
}
