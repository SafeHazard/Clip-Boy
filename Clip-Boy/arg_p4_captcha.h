#pragma once
// arg_p4_captcha.h — P4 "The Captcha" (handoff §6.4). An infinite "prove you're
// not a bot" math troll that rejects every answer with absurd reasons. Two ways
// out, same completion bit:
//   1) Clever: the FIRST operand cycles the universal payload 88 89 90 90 89
//      (= ASCII "XYZZY"); notice it, type XYZZY.  (Zork deep cut.)
//   2) Attrition: 256 CORRECT answers -> the counter "overflows" and it gives up.
// Counters (persisted in ArgState): p4_answered (+1 per non-empty NUMERIC try;
// GLaDOS jab at 34) and p4_correct (+1 only when correct; unlock at 256).
//
// Serial core is complete + fuzz-testable here. The P5 keypad is reached via the
// SECRET-MENU tap sequence on the real UI chrome (arg_secret[] in ui_nav.h — the
// old quadrant "tap ritual" screen was cut, owner's call). A P4 win teaches that
// sequence as its serial clue (see p4_win below); P5 hint/status re-surface it.

#include "arg_clipcli.h"

// Universal payload — MUST be identical for every badge (handoff §2.8). ASCII
// X(88) Y(89) Z(90) Z(90) Y(89).
static const uint8_t P4_XYZZY[5] = {88, 89, 90, 90, 89};
#define P4_UNLOCK_CORRECT 256   // attrition overflow (0x100)
#define P4_GLADOS_AT      34    // DC34 jab

static uint8_t  p4_cycle = 0;        // index into P4_XYZZY (reset on enter, §6.4)
static uint8_t  p4_op_a = 88;        // first operand = payload slot
static uint8_t  p4_op_b = 2;         // second operand = random ~2..20
static bool     p4_in_session = false;
static bool     p4_replay = false;       // read-only replay (clipcli start 4): no mutations (review H1)

static void p4_save_counters(void) {     // debounced persist (review N1)
    arg_save_u16("p4ok",  arg.p4_correct);
    arg_save_u16("p4ans", arg.p4_answered);
}

// ── helpers ────────────────────────────────────────────────────────────────
static bool p4_is_blank(const char *s) {
    if (!s) return true;
    for (; *s; s++) if (!isspace((unsigned char)*s)) return false;
    return true;
}
static bool p4_is_number(const char *s) {     // non-empty, all digits (after trim)
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) return false;
    bool any = false;
    for (; *s; s++) {
        if (isspace((unsigned char)*s)) break;   // trailing space ok
        if (!isdigit((unsigned char)*s)) return false;
        any = true;
    }
    return any;
}
static bool p4_is_xyzzy(const char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    const char *want = "XYZZY";
    for (int i = 0; i < 5; i++) { if (toupper((unsigned char)s[i]) != want[i]) return false; }
    const char *t = s + 5; while (*t && isspace((unsigned char)*t)) t++;
    return *t == '\0';
}

static void p4_new_problem(void) {
    p4_op_a = P4_XYZZY[p4_cycle];
    p4_cycle = (uint8_t)((p4_cycle + 1) % 5);
    p4_op_b = (uint8_t)(2 + (esp_random() % 19));   // 2..20 (§6.4 ~20x20)
    Serial.printf("\r\nCAPTCHA #%u: prove you are human. What is %u x %u?\r\n> ",
                  (unsigned)arg.p4_answered + 1, p4_op_a, p4_op_b);
}

// Absurd rejection reasons (handoff §6.4 voice). Rotates; never reveals correctness.
static void p4_reject(bool was_correct) {
    static const char *reasons[] = {
        "Rejected: too slow. A real human panics faster.",
        "Rejected: too fast. Suspiciously eager.",
        "Rejected: your numerals had an accent I didn't care for.",
        "Rejected: correct, but I didn't care for your tone.",
        "Rejected: I award you no points, and may the signal have mercy on your soul.",
        "Rejected: that's exactly what a bot pretending to be human would say.",
    };
    uint8_t i = (uint8_t)(esp_random() % (sizeof(reasons) / sizeof(reasons[0])));
    (void)was_correct;   // by design: same sarcasm whether right or wrong
    Serial.println(reasons[i]);
}

// One original "Still Alive"-style line (§6.4 serenade, default on). Text only.
static void p4_serenade(void) {
    static const char *lines[] = {
        "  (the signal hums) ... still counting. still counting.",
        "  (the signal hums) ... this was a triumph. I'm making a note here.",
        "  (the signal hums) ... you're doing great. that's a lie, but you're doing.",
    };
    Serial.println(lines[esp_random() % (sizeof(lines)/sizeof(lines[0]))]);
}

static void p4_win(const char *how) {
    arg_set_flag(ARG_P4_CAPTCHA);   // win condition sets the P4 bit (§6.4)
    Serial.printf("\r\n[SIGNAL_9] %s\r\n", how);
    // P4's reward = the secret-menu knock that opens the reward keypad (a hidden
    // tap sequence on the real UI chrome; see arg_secret[] in ui_nav.h).
    Serial.println(F("[CLIP] A maintenance hatch clicks open. The knock is in the chrome itself --"));
    Serial.println(F("[CLIP]   tap, in order, anywhere on the badge:  STATS . light . light . DATA . ITEMS"));
    Serial.println(F("[CLIP]   (\"light\" = the flashlight button). That opens the reward keypad."));
    p4_in_session = false;
    arg_session_end();
}

// ── session line handler (returns true if consumed) ─────────────────────────
static bool p4_line(const char *line) {
    // Let clipcli commands interleave (status/hint/etc.) without leaving P4.
    const char *p = line; while (*p == ' ') p++;
    if (strncmp(p, "clipcli", 7) == 0) return false;   // dispatcher handles it

    if (!strcasecmp(p,"quit")||!strcasecmp(p,"exit")||!strcasecmp(p,"leave")||!strcasecmp(p,"q")) {
        p4_in_session = false; arg_session_end();   // back to the CLI; lifetime counters in NVS untouched
        Serial.println(F("You step away from the challenge."));
        Serial.println(F("[CLIP] 'clipcli challenge' to return."));
        return true;
    }

    // XYZZY — the clever win (not a numeric attempt; doesn't touch counters).
    if (p4_is_xyzzy(line)) {
        if (p4_replay) {   // replay is read-only: acknowledge, don't unlock (review H1)
            Serial.println(F("\r\n[CLIP] (replay) The word still works. Nothing was saved."));
            p4_in_session = false; arg_session_end(); return true;
        }
        p4_win("A hollow voice says 'fool.' ...the signal made me say that. Accepted.");
        return true;
    }
    // Empty/whitespace: anti-spam — counts toward NEITHER counter (§6.4). Re-prompt.
    if (p4_is_blank(line)) {
        Serial.print(F("(say something) > "));
        return true;
    }
    // Non-numeric, non-XYZZY: absurd reject, counts neither. NOTE: must NOT imply
    // "numbers only" — that would steer players away from the XYZZY word-path.
    if (!p4_is_number(line)) {
        Serial.println(F("Rejected. (Not because it's not a number -- I reject on principle, not on type.)"));
        Serial.print(F("> "));
        return true;
    }
    // Numeric attempt. (Replay is read-only: no counters, no save, no win — H1.)
    long ans = atol(line);
    bool correct = (ans == (long)p4_op_a * (long)p4_op_b);
    if (!p4_replay) {
        arg.p4_answered++;
        if (arg.p4_answered == P4_GLADOS_AT) {
            Serial.println();
            Serial.println(F("[ACHIEVEMENT: Diligence] 34 problems in a system that gives you nothing."));
            Serial.println(F("Subjects will do almost anything if they think they're progressing."));
            Serial.println(F("That is not a compliment."));
        }
        if (correct) {
            arg.p4_correct++;
            if (arg.p4_correct >= P4_UNLOCK_CORRECT) {
                p4_save_counters();   // flush on win
                p4_win("Fine. FINE. The counter overflowed and I am tired. You win by sheer attrition.");
                return true;
            }
            if ((arg.p4_correct % 16) == 0) p4_serenade();   // occasional on-brand line
        }
        // Debounce NVS (review N1): persist every 8th answer — the 34/256 thresholds
        // tolerate losing a few on a power-cut; avoids a flash-write per keystroke.
        if ((arg.p4_answered % 8) == 0) p4_save_counters();
    }
    p4_reject(correct);
    p4_new_problem();
    return true;
}

static void p4_enter(bool replay) {
    p4_replay = replay;           // read-only when replaying (review H1)
    p4_cycle = 0;                 // everyone sees 88 first (§6.4 cycle reset)
    p4_in_session = true;
    arg_session_active = true;
    arg_session_line   = p4_line;
    if (replay) Serial.println(F("\r\n[CLIP] (replay) The gatekeeper doesn't remember you. Read-only."));
    Serial.println(F("\r\n=== ACCESS GATE ==="));
    Serial.println(F("The rig wants proof you're not a bot. It is infinite. It is not impressed."));
    Serial.println(F("Answer the math. Or don't. (Type a clipcli command anytime.)"));
    p4_new_problem();
}

static void p4_hint(int level) {
    switch (level) {
        case 1:  Serial.println(F("[CLIP] You won't out-compute this. Look at the questions themselves.")); break;
        case 2:  Serial.println(F("[CLIP] Not every number is filler. Watch the FIRST number each time.")); break;
        default: Serial.println(F("[CLIP] Collect the first numbers: 88 89 90 90 89. Read them as ASCII. Type that word.")); break;
    }
}

static void p4_reset(void) {
    arg.p4_correct = 0; arg.p4_answered = 0;
    arg_save_u16("p4ok", 0); arg_save_u16("p4ans", 0);
    p4_cycle = 0;
}

// Register P4 in the puzzle table. Call from setup() (after arg_clipcli_init).
static void p4_register(void) {
    arg_puzzles[4].enter = p4_enter;
    arg_puzzles[4].hint  = p4_hint;
    arg_puzzles[4].reset = p4_reset;
}
