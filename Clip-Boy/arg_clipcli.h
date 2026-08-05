#pragma once
// arg_clipcli.h — DC34 ARG `clipcli` command surface (handoff §2.4) + the
// reset/confirm + hint-gate state machine. Line-based for now (one-shot
// commands); the char-level editor (echo/backspace, §2.2) lands with the first
// interactive puzzle session, which needs non-blocking input.
//
// Integration (ui_test.ino loop): when a serial line arrives, route it here iff
//   arg_is_capturing() || line starts with "clipcli"
// so bare confirm responses (Y/N) and (later) in-session input are captured too.
//
// Voice: institutional cheer over catastrophe; recognition rewards, ignorance
// never punishes; never disclose how many puzzles remain (§2.5).

#include "arg_core.h"

// ─── Capture state (so the loop routes follow-up lines to us) ───────────────
enum ArgCapture {
    ARG_CAP_NONE = 0,
    ARG_CAP_CONFIRM_RESET_ALL,
    ARG_CAP_CONFIRM_RESET_N,
    ARG_CAP_CONFIRM_ABANDON,   // P1 "never show again" -> Zenith (handoff §4.1)
    ARG_CAP_HINT_GATE2,        // hint call 2: "Sure? [Y/N]"
    ARG_CAP_HINT_GATE3,        // hint call 3+: answer a random product
    // ARG_CAP_SESSION_* added by puzzle modules
};
static ArgCapture arg_cap = ARG_CAP_NONE;
static int        arg_cap_arg = 0;     // e.g. which puzzle N for reset
static bool       arg_chal_plain = false;  // `clipcli challenge --plain` (§6.1 reduced noise)
static int        arg_hint_prod_a = 0, arg_hint_prod_b = 0;  // gate-3 challenge

// Puzzle modules set arg_session_active (now declared in arg_core.h, so the UI
// exclusive-input touch-lock can read it) + arg_session_line when they own the
// serial line stream. arg_session_line returns true if it consumed the line.
static bool (*arg_session_line)(const char *line) = nullptr;  // returns handled

static inline bool arg_is_capturing(void) {
    return arg_cap != ARG_CAP_NONE || arg_session_active;
}

// ─── small helpers ─────────────────────────────────────────────────────────
static bool arg_is_yes(const char *s) {
    return s && (s[0] == 'y' || s[0] == 'Y');
}
static bool arg_is_no(const char *s) {
    return s && (s[0] == 'n' || s[0] == 'N');
}

// Per-puzzle hooks — function pointers a puzzle module sets in its init. Null
// until then, so the dispatcher is complete + testable before the puzzles exist.
static void (*arg_challenge_enter)(void)      = nullptr;
static void (*arg_challenge_replay)(int)      = nullptr;
static void (*arg_hint_dispatch)(int)         = nullptr;
static void (*arg_reset_puzzle_substate)(int) = nullptr;  // clears blob/counters for P<n>
static bool (*arg_unlock_fn)(const char *)    = nullptr;  // P5 `clipcli unlock <code>` (arg_p5_call.h)

// Which puzzle is the lowest incomplete? 1..5, or 0 if all done.
static int arg_current_puzzle(void) {
    if (!arg_flag(ARG_P1_RADIO))   return 1;
    if (!arg_flag(ARG_P2_HACK))    return 2;
    if (!arg_flag(ARG_P3_DORK))    return 3;
    if (!arg_flag(ARG_P4_CAPTCHA)) return 4;
    if (!arg_flag(ARG_P5_PHONE))   return 5;
    return 0;
}

// ─── status / startgame (idempotent, §2.4) ─────────────────────────────────
static void arg_print_status(bool full) {
    int cur = arg_current_puzzle();
    Serial.println();
    if (arg_quanta_earned()) {
        Serial.println(F("[SIGNAL_9] Trial complete. You are the operator now. Quanta is yours."));
        // fall through: `status all` still prints the full log so far below.
    } else {
        // Never disclose count/remaining (§2.5).
        if (!arg.discovered) {
            Serial.println(F("[SIGNAL_9] A signal you booted into is still running an old trial."));
        }
        if (cur == 0) {
            Serial.println(F("[SIGNAL_9] All trials cleared — finish at the phone step. See your badge."));
        } else if (cur == 1) {
            // P1 has no terminal challenge — it's the signal you tune in via startgame.
            Serial.println(F("[SIGNAL_9] The signal won't talk to a recording, only a live rig."));
            Serial.println(F("           Type 'clipcli startgame' to tune in."));
        } else if (cur == 5) {
            // Finale: re-surface how to open the reward keypad (the P4 knock is only
            // printed once at P4-win; repeat it here so it's re-retrievable).
            Serial.println(F("[SIGNAL_9] One step left — prove you're real at the phone step."));
            Serial.println(F("           Open the reward keypad: 'clipcli challenge', or the chrome knock --"));
            Serial.println(F("           tap in order:  STATS . light . light . DATA . ITEMS  (\"light\" = flashlight)."));
        } else {
            Serial.println(F("[SIGNAL_9] The signal won't talk to a recording, only a live rig."));
            Serial.println(F("           Type 'clipcli challenge' to face what's in front of you."));
        }
    }
    if (full) {
        // status all: narrative-so-far (beats for completed stages), no counts.
        Serial.println(F("--- log so far ---"));
        if (arg_flag(ARG_P1_RADIO))   Serial.println(F("  * You tuned the signal in."));
        if (arg_flag(ARG_P2_HACK))    Serial.println(F("  * You pulled the password from the dump."));
        if (arg_flag(ARG_P3_DORK))    Serial.println(F("  * You found the queue and were let through."));
        if (arg_flag(ARG_P4_CAPTCHA)) Serial.println(F("  * You out-stubborned the gatekeeper."));
        if (arg_flag(ARG_P5_PHONE))   Serial.println(F("  * You proved you were real."));
    }
}

static void arg_cmd_startgame(void) {
    arg_set_discovered();
    arg_set_flag(ARG_P1_RADIO);   // default (§9): running startgame once sets P1. TODO(data)
    Serial.println(F("[CLIP] clipcli online. 'clipcli status' anytime; 'clipcli challenge' to proceed."));
    arg_print_status(false);
}

// ─── help (§2.4 + §2.6 connect info) ────────────────────────────────────────
static void arg_cmd_help(void) {
    Serial.println(F("clipcli — commands:"));
    Serial.println(F("  startgame / status   where you stand (idempotent)"));
    Serial.println(F("  status all           the story so far"));
    Serial.println(F("  challenge            face / resume the current trial"));
    Serial.println(F("  start <N>            replay a trial (read-only)"));
    Serial.println(F("  reset <N|all>        redo a trial / restart (destructive, warns)"));
    Serial.println(F("  hint                 a nudge for the current trial"));
    Serial.println(F("  unlock <code>        enter a reward code (final step)"));
    Serial.println(F("  help                 this list"));
    Serial.println(F("USB serial: 115200 8N1, press Enter after each command."));
    Serial.println(F("  Windows: PuTTY (Serial). macOS/Linux: screen /dev/tty.* 115200."));
#ifdef CLIPBOY_DEBUG
    Serial.println(F("  [debug] marshal <N> <complete|reset> | marshal --dump"));
#endif
}

// ─── reset (§2.5, with confirm) ─────────────────────────────────────────────
static void arg_cmd_reset(const char *arg_s) {
    if (arg_s && (arg_s[0] == 'a')) {   // "all"
        Serial.println(F("This will require you to complete everything again. Continue? [Y/N]"));
        arg_cap = ARG_CAP_CONFIRM_RESET_ALL;
        return;
    }
    int n = arg_s ? atoi(arg_s) : 0;
    if (n < 1 || n > 5) { Serial.println(F("reset <N|all> — N is which trial to redo.")); return; }
    Serial.printf("This will only reset progress on puzzle %d. All others remain as-is. Continue? [Y/N]\r\n", n);
    arg_cap = ARG_CAP_CONFIRM_RESET_N; arg_cap_arg = n;
}

static void arg_do_reset_all(void) {
    arg_session_active = false; arg_session_line = nullptr;   // drop any active puzzle session
    arg_reset_scalars();
    for (int n = 1; n <= 5; n++) if (arg_reset_puzzle_substate) arg_reset_puzzle_substate(n);
    Serial.println(F("[CLIP] Reset. The trial is fresh. 'clipcli status' to begin."));
}
static void arg_do_reset_n(int n) {
    static const uint8_t masks[6] = {0, ARG_P1_RADIO, ARG_P2_HACK, ARG_P3_DORK, ARG_P4_CAPTCHA, ARG_P5_PHONE};
    arg_session_active = false; arg_session_line = nullptr;   // drop any active puzzle session
    arg_clear_flag(masks[n]);
    if (arg_reset_puzzle_substate) arg_reset_puzzle_substate(n);
    Serial.println(F("[CLIP] That trial is reset. 'clipcli challenge' when ready."));
}

// ─── hint (§2.7 tiered + gated) ─────────────────────────────────────────────
static uint8_t arg_hint_count[6] = {0};   // per-puzzle hint-call count (1..5)

static void arg_cmd_hint(void) {
    int cur = arg_current_puzzle();
    if (cur == 0) { Serial.println(F("Nothing left to hint — you're at the final step.")); return; }
    // Availability: hint for N only after N-1 complete (§2.7). cur IS the lowest
    // incomplete, so N-1 is by definition complete — always available here.
    uint8_t c = ++arg_hint_count[cur];
    if (c == 1) {
        if (arg_hint_dispatch) arg_hint_dispatch(1);
        else Serial.println(F("[CLIP] Look at what the signal is actually asking. Slow down."));
    } else if (c == 2) {
        Serial.println(F("[CLIP] A real hint, then. Sure? [Y/N]"));
        arg_cap = ARG_CAP_HINT_GATE2;
    } else {
        // gate 3+: random small product, random EACH TIME (§2.7 kills answer-sharing).
        arg_hint_prod_a = 2 + (int)(esp_random() % 19);
        arg_hint_prod_b = 2 + (int)(esp_random() % 19);
        Serial.printf("[CLIP] Sure you're sure? What's %d x %d?\r\n", arg_hint_prod_a, arg_hint_prod_b);
        arg_cap = ARG_CAP_HINT_GATE3;
    }
}

// ─── confirm/gate follow-up handler (capture path) ──────────────────────────
static void arg_handle_capture(const char *line) {
    ArgCapture cap = arg_cap;
    arg_cap = ARG_CAP_NONE;   // consume; re-set below if needed
    switch (cap) {
        case ARG_CAP_CONFIRM_RESET_ALL:
            if (arg_is_yes(line)) arg_do_reset_all();
            else Serial.println(F("[CLIP] Cancelled. Nothing changed."));
            break;
        case ARG_CAP_CONFIRM_RESET_N:
            if (arg_is_yes(line)) arg_do_reset_n(arg_cap_arg);
            else Serial.println(F("[CLIP] Cancelled. Nothing changed."));
            break;
        case ARG_CAP_CONFIRM_ABANDON:
            if (arg_is_yes(line)) {
                arg.abandoned = 1; arg_save_u8("abandoned", 1);
                arg_set_theme(ARG_THEME_ZENITH);
                Serial.println(F("[CLIP] Enlightenment by refusal. Zenith granted. The signal goes quiet."));
            } else Serial.println(F("[CLIP] Good. Stay on the path."));
            break;
        case ARG_CAP_HINT_GATE2:
            if (arg_is_yes(line)) { if (arg_hint_dispatch) arg_hint_dispatch(2);
                                    else Serial.println(F("[CLIP] (mechanical hint — puzzle-specific)")); }
            else Serial.println(F("[CLIP] Suit yourself."));
            break;
        case ARG_CAP_HINT_GATE3:
            if (atoi(line) == arg_hint_prod_a * arg_hint_prod_b) {
                if (arg_hint_dispatch) arg_hint_dispatch(3);
                else Serial.println(F("[CLIP] (near-explicit hint — puzzle-specific)"));
            } else Serial.println(F("[CLIP] Wrong. The signal respects effort, not guesses. Try hint again."));
            break;
        default: break;
    }
}

#ifdef CLIPBOY_DEBUG
// ─── marshal (DEBUG/TEST BUILDS ONLY, §8) ───────────────────────────────────
// Integrity is the P5 per-badge HMAC, NOT hidden commands; marshal only lets an
// owner rearrange flags on a badge they already own. Compiled out of release.
static void arg_cmd_marshal(const char *args) {
    if (!args || !*args) { Serial.println(F("marshal <N> <complete|reset> | marshal --dump")); return; }
    if (strncmp(args, "--dump", 6) == 0) {
        const uint8_t *m = arg_mac();
        Serial.printf("[MARSHAL] progress=0x%02X discovered=%u abandoned=%u theme=%u\r\n",
                      arg.progress, arg.discovered, arg.abandoned, arg.theme_active);
        Serial.printf("[MARSHAL] p4_correct=%u p4_answered=%u radio_dismiss=%u\r\n",
                      arg.p4_correct, arg.p4_answered, arg.radio_dismiss_count);
        Serial.printf("[MARSHAL] MAC=%02X:%02X:%02X:%02X:%02X:%02X shortid=%04X seed=%08X\r\n",
                      m[0],m[1],m[2],m[3],m[4],m[5], arg_p1_shortid(), arg_mac_seed());
        return;
    }
    int n = atoi(args);
    const char *verb = strchr(args, ' ');
    if (n < 1 || n > 5 || !verb) { Serial.println(F("marshal <N> <complete|reset>")); return; }
    verb++;
    static const uint8_t masks[6] = {0, ARG_P1_RADIO, ARG_P2_HACK, ARG_P3_DORK, ARG_P4_CAPTCHA, ARG_P5_PHONE};
    if (strncmp(verb, "complete", 8) == 0) { arg_set_flag(masks[n]);   Serial.printf("[MARSHAL] P%d complete (0x%02X)\r\n", n, arg.progress); }
    else if (strncmp(verb, "reset", 5) == 0) { arg_clear_flag(masks[n]); if (arg_reset_puzzle_substate) arg_reset_puzzle_substate(n); Serial.printf("[MARSHAL] P%d reset (0x%02X)\r\n", n, arg.progress); }
    else Serial.println(F("marshal <N> <complete|reset>"));
}
#endif

// ─── main entry (called from loop when capturing or line starts "clipcli") ──
static void arg_clipcli_line(const char *line) {
    // 1) pending confirm / hint gate ALWAYS first — else an active puzzle session
    //    would eat the Y/N (or the numeric gate-3 answer) the prompt asked for,
    //    rejecting it and corrupting counters (adversarial review M2).
    if (arg_cap != ARG_CAP_NONE) { arg_handle_capture(line); return; }
    // 2) active interactive puzzle session owns the line
    if (arg_session_active && arg_session_line) { if (arg_session_line(line)) return; }
    // 3) command parse: "clipcli <sub> [args]"
    const char *p = line;
    while (*p == ' ') p++;
    if (strncmp(p, "clipcli", 7) != 0) return;   // not ours
    p += 7;
    while (*p == ' ') p++;
    const char *args = strchr(p, ' ');
    if (args) args++;  // points past first arg-separator (sub-args)

    if (*p == '\0' || strncmp(p, "status", 6) == 0 || strncmp(p, "startgame", 9) == 0) {
        if (strncmp(p, "status all", 10) == 0) { arg_print_status(true); return; }
        if (strncmp(p, "startgame", 9) == 0)   { arg_cmd_startgame(); return; }
        arg_print_status(false); return;
    }
    if (strncmp(p, "help", 4) == 0)      { arg_cmd_help(); return; }
    if (strncmp(p, "challenge", 9) == 0) { arg_chal_plain = (args && strstr(args, "--plain") != nullptr);
                                           if (arg_challenge_enter) arg_challenge_enter();
                                           else Serial.println(F("[CLIP] (the trial is being prepared — check back)")); return; }
    if (strncmp(p, "start", 5) == 0)     { int n = args ? atoi(args) : 0; if (arg_challenge_replay) arg_challenge_replay(n);
                                           else Serial.println(F("[CLIP] (replay unavailable yet)")); return; }
    if (strncmp(p, "reset", 5) == 0)     { arg_cmd_reset(args); return; }
    if (strncmp(p, "hint", 4) == 0)      { arg_cmd_hint(); return; }
    if (strncmp(p, "unlock", 6) == 0)    { if (arg_unlock_fn) arg_unlock_fn(args);
                                           else Serial.println(F("[CLIP] Enter the code on your badge's keypad.")); return; }
#ifdef CLIPBOY_DEBUG
    if (strncmp(p, "marshal", 7) == 0)   { arg_cmd_marshal(args); return; }
#endif
    Serial.println(F("[CLIP] Unknown command. 'clipcli help' for the list."));
}

// ─── Puzzle registry + dispatch ─────────────────────────────────────────────
// Each puzzle module fills arg_puzzles[N] in its init; the dispatcher routes
// `challenge`/`hint`/`reset N` to the current (or named) puzzle. A puzzle's
// enter() sets arg_session_active + arg_session_line so subsequent serial lines
// flow to it (handoff §2.4 auto-resume).
struct ArgPuzzle {
    void (*enter)(bool replay);   // start/resume; replay=true => read-only (§2.5/§7)
    void (*hint)(int level);
    void (*reset)(void);          // clear this puzzle's substate
};
static ArgPuzzle arg_puzzles[6] = {};

static void arg_dispatch_challenge(void) {
    int c = arg_current_puzzle();
    if (c == 0) { Serial.println(F("[CLIP] All trials cleared. Finish at the phone step on your badge.")); return; }
    // P1 isn't a terminal trial — it's the signal you tune in. There is no
    // arg_puzzles[1] module; the entry point is `startgame`. Route there instead
    // of the generic "still being prepared" dead-end.
    if (c == 1 && !arg_puzzles[1].enter) {
        Serial.println(F("[CLIP] This one isn't cracked in a terminal — it's the signal itself. Run 'clipcli startgame' to tune in."));
        return;
    }
    if (arg_puzzles[c].enter) arg_puzzles[c].enter(false);
    else Serial.println(F("[CLIP] (this trial is still being prepared — check back)"));
}
static void arg_dispatch_replay(int n) {
    if (n >= 1 && n <= 5 && arg_puzzles[n].enter) { arg_puzzles[n].enter(true); }
    else Serial.println(F("[CLIP] No such trial to replay."));
}
static void arg_dispatch_hint(int level) {
    int c = arg_current_puzzle();
    if (c == 1 && !arg_puzzles[1].hint) {
        Serial.println(F("[CLIP] Nothing to crack yet — the signal wants you to 'clipcli startgame'."));
        return;
    }
    if (c >= 1 && c <= 5 && arg_puzzles[c].hint) arg_puzzles[c].hint(level);
    else Serial.println(F("[CLIP] No hint available here."));
}
static void arg_dispatch_reset(int n) {
    if (n >= 1 && n <= 5 && arg_puzzles[n].reset) arg_puzzles[n].reset();
}

// Wire the dispatcher into the command hooks. Call once from setup() after the
// puzzle modules have registered (they register at their own init).
static void arg_clipcli_init(void) {
    arg_challenge_enter      = arg_dispatch_challenge;
    arg_challenge_replay     = arg_dispatch_replay;
    arg_hint_dispatch        = arg_dispatch_hint;
    arg_reset_puzzle_substate = arg_dispatch_reset;
}

// Helper a puzzle calls to hand the serial stream back when it completes/exits.
static void arg_session_end(void) {
    arg_session_active = false;
    arg_session_line   = nullptr;
}
