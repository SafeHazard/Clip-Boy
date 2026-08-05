#pragma once
// arg_p2_hack.h — P2 "The Hack" (handoff §6.1): a retro green-screen password-crack terminal.
// Extract the password from a memory dump. Same-length candidate words hidden in
// ASCII trash with fake hex gutters; guess gives "Entry denied. Likeness=N"
// (letters in the correct position). 4 attempts ("allowance"), then the board
// re-rolls a new correct word + resets attempts (no hard fail). Bracket pairs in
// the trash, typed whole (e.g. <7#x>), either purge a dud candidate or replenish
// the allowance — role MAC-seeded; single-use. Correct word + bracket roles are
// per-badge (§2.8); the trash layout is fresh each enter. `--plain` reduces noise.

#include "arg_clipcli.h"

#define P2_WLEN     8       // all candidates 8 letters
#define P2_NSHOW    7       // candidates shown
#define P2_NBRACK   6       // bracket tokens on the board
#define P2_ROWS     16      // content rows (x2 columns)
#define P2_CELLW    14      // content width per column cell

// Retro-hacker 8-letter pool (share letters -> interesting Likeness). Curated ASCII.
// All entries MUST be exactly P2_WLEN (8) letters, A-Z only. Themed on the
// collectibles catalog's franchises + hacker/sci-fi flavor. Deliberately grouped
// so EVERY initial that appears has >=2 words — p2_build() guarantees the shown
// set includes a same-initial decoy for the answer, so the "starts with X" hint
// narrows to >=2 candidates instead of solving the puzzle outright.
static const char *P2_POOL[] = {
    // A
    "AUTOBOTS","ALLSPARK","ALLIANCE",
    // B
    "BACKDOOR","BLUEPILL",
    // C
    "COVENANT","CIRCUITS",
    // D
    "DARKMODE","DATALORE","DEATHRAY","DELOREAN","DARKSIDE",
    // F
    "FIREWALL","FIRMWARE",
    // G
    "GIGAWATT","GONDOLIN",
    // H
    "HOLOGRID","HOLODECK","HOGWARTS","HARDWARE",
    // I
    "IDENTITY","ISENGARD",
    // M
    "MARAUDER","MEGATRON","MONOLITH",
    // N
    "NEUTRINO","NANOBOTS",
    // O
    "OVERSEER","OVERRIDE","OVERLORD",
    // P
    "PALANTIR","PODRACER","POKEBALL","PATRONUS",
    // R
    "RADSTORM","REDSHIRT",
    // S
    "STARBASE","STARSHIP","STARGATE","SPARTANS","SERENITY",
    // T
    "TRIBBLES","TRIFORCE","TOMSERVO",
    // W
    "WARPCORE","WARPPIPE","WHITEHAT",
};
#define P2_POOLN ((int)(sizeof(P2_POOL)/sizeof(P2_POOL[0])))

struct P2Bracket { char tok[10]; uint8_t role; bool consumed; };  // role 0=dud-remove 1=allowance

static char     p2_cand[P2_NSHOW][P2_WLEN+1];
static bool     p2_dud_removed[P2_NSHOW];
static int      p2_correct = 0;          // index into p2_cand
static int      p2_allow   = 4;
static P2Bracket p2_brk[P2_NBRACK];
static bool     p2_active = false;
static bool     p2_replay = false;
static bool     p2_built  = false;       // a playthrough's board exists (persists across quit; cleared on win/reset)
static char     p2_board[P2_ROWS][2][P2_CELLW+1];   // rendered content cells


static const char P2_TRASH[] = "!@#$%^&*-+=._:;,?/|~";   // non-bracket filler

static char p2_trash_ch(void){ return P2_TRASH[esp_random() % (sizeof(P2_TRASH)-1)]; }

static void p2_make_bracket(P2Bracket *b, int idx) {
    static const char opn[] = "<[({", cls[] = ">])}";
    int k = idx % 4;
    int n = 2 + (esp_random() % 3);          // 2..4 content chars
    char *t = b->tok; *t++ = opn[k];
    for (int i = 0; i < n; i++) *t++ = p2_trash_ch();
    *t++ = cls[k]; *t = '\0';
    b->role = (uint8_t)(esp_random() & 1);   // role randomized per playthrough
    b->consumed = false;
}

static void p2_build(void) {
    // Per-playthrough random (built once per playthrough — see p2_built). Pick the
    // answer, then GUARANTEE a same-initial decoy is also shown so the "starts with
    // X" hint narrows to >=2 candidates rather than solving the puzzle outright.
    int cwi = esp_random() % P2_POOLN;                  // answer's pool index
    char ci0 = P2_POOL[cwi][0];
    int partner = -1, np = 0;                           // reservoir-pick one same-initial decoy
    for (int i = 0; i < P2_POOLN; i++)
        if (i != cwi && P2_POOL[i][0] == ci0 && (esp_random() % (++np)) == 0) partner = i;

    int chosen[P2_NSHOW]; int nc = 0;
    chosen[nc++] = cwi;
    if (partner >= 0) chosen[nc++] = partner;
    while (nc < P2_NSHOW) {                             // fill remaining slots, distinct
        int r = esp_random() % P2_POOLN; bool dup = false;
        for (int k = 0; k < nc; k++) if (chosen[k] == r) { dup = true; break; }
        if (!dup) chosen[nc++] = r;
    }
    for (int i = nc-1; i > 0; i--) { int j = esp_random()%(i+1); int t=chosen[i]; chosen[i]=chosen[j]; chosen[j]=t; }  // shuffle slots
    for (int i = 0; i < P2_NSHOW; i++) {
        strncpy(p2_cand[i], P2_POOL[chosen[i]], P2_WLEN+1); p2_dud_removed[i] = false;
        if (chosen[i] == cwi) p2_correct = i;          // track where the answer landed
    }
    p2_allow = 4;
    for (int i = 0; i < P2_NBRACK; i++) p2_make_bracket(&p2_brk[i], i);

    // Layout (fresh each enter, esp_random): fill cells w/ trash, stamp items.
    for (int r = 0; r < P2_ROWS; r++)
        for (int c = 0; c < 2; c++) {
            for (int k = 0; k < P2_CELLW; k++) p2_board[r][c][k] = p2_trash_ch();
            p2_board[r][c][P2_CELLW] = '\0';
        }
    int cells = P2_ROWS*2, order[P2_ROWS*2];
    for (int i = 0; i < cells; i++) order[i] = i;
    for (int i = cells-1; i > 0; i--) { int j = esp_random()%(i+1); int t=order[i]; order[i]=order[j]; order[j]=t; }
    int o = 0;
    auto stamp = [&](const char *s){ int cell=order[o++]; int r=cell/2,c=cell%2; int len=strlen(s);
                                     int off = (P2_CELLW-len)>0 ? esp_random()%(P2_CELLW-len+1) : 0;
                                     memcpy(&p2_board[r][c][off], s, len); };
    for (int i = 0; i < P2_NSHOW; i++) stamp(p2_cand[i]);
    for (int i = 0; i < P2_NBRACK; i++) stamp(p2_brk[i].tok);
}

static void p2_render(void) {
    if (arg_chal_plain) {
        Serial.println(F("\r\n-- CLIP-LINK (plain) --"));
        Serial.printf("ATTEMPTS LEFT: %d\r\n", p2_allow);
        Serial.println(F("Candidates:"));
        for (int i = 0; i < P2_NSHOW; i++) if (!p2_dud_removed[i]) Serial.printf("   %s\r\n", p2_cand[i]);
        Serial.println(F("Bracket sequences on the board:"));
        for (int i = 0; i < P2_NBRACK; i++) if (!p2_brk[i].consumed) Serial.printf("   %s\r\n", p2_brk[i].tok);
        Serial.print(F("> "));
        return;
    }
    Serial.println(F("\r\nCLIPPY INDUSTRIES CLIP-LINK PROTOCOL"));
    Serial.println(F("ENTER PASSWORD NOW"));
    Serial.printf("\r\n%d ATTEMPT(S) LEFT : ", p2_allow);
    for (int i = 0; i < p2_allow; i++) Serial.print(F("# "));
    Serial.println();
    uint16_t addr = 0xF000 + (esp_random() & 0x0F00);
    for (int r = 0; r < P2_ROWS; r++) {
        Serial.printf("0x%04X %s   0x%04X %s\r\n", addr, p2_board[r][0], addr+0x100, p2_board[r][1]);
        addr += 0x0C;
    }
    Serial.print(F("> "));
}

static void p2_reroll(void) {
    // Fallout lockout: new correct word among remaining candidates + reset allowance.
    int live[P2_NSHOW], n = 0;
    for (int i = 0; i < P2_NSHOW; i++) if (!p2_dud_removed[i]) live[n++] = i;
    p2_correct = live[esp_random() % n];
    p2_allow = 4;
    Serial.println(F("\r\n!! TERMINAL LOCKED !! ...rebooting link... allowance restored."));
}

static void p2_win(void) {
    if (!p2_replay) arg_set_flag(ARG_P2_HACK);
    Serial.println(F("\r\n>> EXACT MATCH."));
    Serial.println(F("[SIGNAL_9] Password accepted. You read the dump. Good. 'clipcli challenge' for what's next."));
    p2_active = false; p2_built = false; arg_session_end();
}

static bool p2_line(const char *line) {
    const char *p = line; while (*p == ' ') p++;
    if (strncmp(p, "clipcli", 7) == 0) return false;   // let commands interleave

    if (!strcasecmp(p,"quit")||!strcasecmp(p,"exit")||!strcasecmp(p,"leave")||!strcasecmp(p,"q")) {
        p2_active = false; arg_session_end();   // back to the CLI (board persists; re-enter to resume it)
        Serial.println(F("Terminal session closed."));
        Serial.println(F("[CLIP] 'clipcli challenge' to jack back in."));
        return true;
    }

    // 1) exact bracket token (case + chars), single-use
    for (int i = 0; i < P2_NBRACK; i++) {
        if (!p2_brk[i].consumed && strcmp(p, p2_brk[i].tok) == 0) {
            p2_brk[i].consumed = true;
            if (p2_brk[i].role == 1) { p2_allow = 4; Serial.println(F(">> ALLOWANCE REPLENISHED.")); }
            else {
                int victim = -1;
                for (int k = 0; k < P2_NSHOW; k++) if (k != p2_correct && !p2_dud_removed[k]) { victim = k; break; }
                if (victim >= 0) { p2_dud_removed[victim] = true; Serial.printf(">> DUD REMOVED: '%s' purged from candidates.\r\n", p2_cand[victim]); }
                else Serial.println(F(">> No duds remain to purge."));
            }
            Serial.print(F("> ")); return true;
        }
    }
    // looks like a bracket but no match
    if (*p=='<'||*p=='['||*p=='('||*p=='{') { Serial.println(F("...no such sequence on this board.")); Serial.print(F("> ")); return true; }

    // 2) a word guess (uppercase, match a live candidate)
    char up[40]; int n = 0;
    for (const char *q = p; *q && n < 39; q++) if (!isspace((unsigned char)*q)) up[n++] = toupper((unsigned char)*q);
    up[n] = '\0';
    if (n == 0) { Serial.print(F("> ")); return true; }   // blank = no-op (§2.2)

    int guess = -1;
    for (int i = 0; i < P2_NSHOW; i++) if (!p2_dud_removed[i] && strcmp(up, p2_cand[i]) == 0) { guess = i; break; }
    if (guess < 0) { Serial.println(F("Not a sequence on the board. Pick a word you can see.")); Serial.print(F("> ")); return true; }

    if (guess == p2_correct) { p2_win(); return true; }
    int like = 0;
    for (int k = 0; k < P2_WLEN; k++) if (p2_cand[guess][k] == p2_cand[p2_correct][k]) like++;
    p2_allow--;
    Serial.printf("Entry denied. Likeness=%d\r\n", like);
    if (p2_allow <= 0) p2_reroll();
    Serial.print(F("> "));
    return true;
}

static void p2_enter(bool replay) {
    p2_replay = replay; p2_active = true;
    // Build a fresh board only at the START of a playthrough; resume the same board
    // (and its per-playthrough password) across quit/re-entry so hints stay valid and
    // progress isn't lost. Replay is always a throwaway fresh build (read-only).
    if (replay) p2_build();
    else if (!p2_built) { p2_build(); p2_built = true; }
    arg_session_active = true; arg_session_line = p2_line;
    Serial.println(F("\r\n=== MEMORY DUMP ACQUIRED ==="));
    Serial.println(F("The rig grabbed a page while it connected. The password is in here somewhere."));
    Serial.println(F("Type a word you see. Brackets [like <this>] do things — type them whole."));
    if (replay) Serial.println(F("(replay) read-only — nothing you do here is saved."));
    p2_render();
}

static void p2_hint(int level) {
    // Start (or keep) this playthrough's board so the hint matches the board you'll
    // resume with. The password is per-playthrough random but built ONCE (p2_built),
    // so a hint after `quit` — or before you've entered — points at the real answer.
    if (!p2_built) { p2_build(); p2_built = true; }
    switch (level) {
        case 1:  Serial.println(F("[CLIP] The password is on-theme. You'll know it when you see it.")); break;
        case 2:  Serial.println(F("[CLIP] Hunt the brackets. Typed whole, they remove a dud or reset your tries.")); break;
        default: Serial.printf("[CLIP] It starts with '%c' and it's %d letters.\r\n", p2_cand[p2_correct][0], P2_WLEN); break;
    }
}

static void p2_reset(void) { p2_active = false; p2_built = false; /* fresh board next playthrough; no NVS substate beyond the P2 bit */ }

static void p2_register(void) {
    arg_puzzles[2].enter = p2_enter;
    arg_puzzles[2].hint  = p2_hint;
    arg_puzzles[2].reset = p2_reset;
}
