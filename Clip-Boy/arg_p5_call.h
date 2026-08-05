#pragma once
// arg_p5_call.h — P5 "The Call" (handoff §6.5). The finale: prove you're a real
// person via the phone/IVR challenge-response, then claim Quanta.
//
// Crypto reuse: the verified per-badge HMAC lives in arg_unlock.h (nonce-based,
// matches the live IVR — code = HMAC-SHA256(secret, "%04u"(nonce)) -> 8 digits).
// The "challenge" shown to the player IS arg_nonce; they SMS it to 775-CLIP-BOY,
// get the 8-digit response, and enter it on the on-screen keypad OR via
// `clipcli unlock <code>` (serial alt, §6.5 default: allow both).
//
// On success: set the P5 bit; if that completes the set (0x1F) apply the Quanta
// reward (theme + LED glow + SIGNAL_9 sign-off) — the ARG payoff, end to end.
//
// The on-screen 0-9 numpad (show_p5_numpad) and its entry points are BUILT: the
// secret-menu tap sequence (arg_secret[] in ui_nav.h -> arg_reveal_keypad_fn) and,
// over serial, `clipcli challenge`. The P4-win serial clue teaches the sequence and
// P5 hint/status re-surface it. TODO(data): CLIPBOY_POST_CON offline "No phone?"
// button (arg_unlock already carries the gate). Serial path is complete + testable.

#include "arg_clipcli.h"
#include "arg_unlock.h"

// On-screen numpad modal globals — declared early so arg_quanta_reward_async can
// close the keypad even when the theme switch is a no-op (re-entry after Quanta).
static lv_obj_t *p5_np_modal  = nullptr;
static lv_obj_t *p5_np_disp   = nullptr;   // entered-digits display
static lv_obj_t *p5_np_msg    = nullptr;   // status/error line

// The payoff. Switch to Quanta (theme + LED). The actual UI rebuild is DEFERRED
// via lv_async_call so it's safe to trigger from inside a touch event (the numpad
// OK button) — a synchronous theme rebuild there would delete the live widget.
static void arg_quanta_reward_async(void *p) {
    (void)p;
    // Close the keypad explicitly. If Quanta is ALREADY the live theme (re-entry),
    // ui_theme_switch_live() below early-returns and never deletes scr_main, so the
    // modal would otherwise linger undismissable. Sync delete is safe here (async
    // ctx, not mid-event); the LV_EVENT_DELETE self-null clears p5_np_modal, so the
    // scr_main teardown — when it DOES run — won't double-free it.
    if (p5_np_modal) lv_obj_delete(p5_np_modal);
    led_apply_preset(7);                 // Quanta LED glow (cyan breathe)
    ui_theme_switch_live(THEME_QUANTA);  // applies + saves cfg.theme + rebuilds UI
}
static void arg_apply_quanta_reward(void) {
    arg_set_theme(ARG_THEME_QUANTA);            // ARG record (handoff theme_active)
    Serial.println(F("\r\n[SIGNAL_9] ...handshake verified. You're real, and you did the work."));
    Serial.println(F("[SIGNAL_9] The trial is yours. Operator status granted. Welcome to Quanta."));
    lv_async_call(arg_quanta_reward_async, nullptr);
}

// Verify an entered response code. Shared by `clipcli unlock` and (later) the
// numpad submit. Returns true if it unlocked.
static bool p5_submit_code(const char *code) {
    if (!code || !*code) { Serial.println(F("[CLIP] usage: clipcli unlock <code>")); return false; }
    uint32_t cd = arg_cooldown_remaining();
    if (cd > 0) { Serial.printf("[CLIP] Too many tries. Wait %lus and call again.\r\n", (unsigned long)(cd/1000)); return false; }
    if (arg_try_code(code)) {
        arg_set_flag(ARG_P5_PHONE);   // if this completes the set, the arg_on_complete hook fires the reward
        if (!arg_all_complete())
            Serial.println(F("[CLIP] Code accepted — but an earlier trial is unfinished. 'clipcli status'."));
        return true;
    }
    Serial.println(F("[CLIP] That code isn't right. Re-read it, or call again for a fresh read."));
    return false;
}

// ── On-screen 0-9 numpad (handoff §6.5: touch entry of the response code) ────
static char      p5_entry[10] = {0};
static uint8_t   p5_entry_n   = 0;
// p5_np_modal/disp/msg are declared near the top (the reward needs them early).
static const char *p5_np_map[] = {"1","2","3","\n","4","5","6","\n","7","8","9","\n","DEL","0","OK",""};

static void p5_np_refresh(void) {
    if (p5_np_disp) lv_label_set_text(p5_np_disp, p5_entry_n ? p5_entry : "----");
}

static void p5_np_event(lv_event_t *e) {
    lv_obj_t *bm = (lv_obj_t *)lv_event_get_target(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(bm);
    const char *t = lv_buttonmatrix_get_button_text(bm, id);
    if (!t) return;
    audio_play_click();   // buttonmatrix isn't lv_button-class, so the global tap-sound hook skips it
    if (!strcmp(t, "DEL")) { if (p5_entry_n) p5_entry[--p5_entry_n] = '\0'; }
    else if (!strcmp(t, "OK")) {
        if (p5_submit_code(p5_entry)) {
            p5_entry_n = 0; p5_entry[0] = '\0';
            // All-complete -> arg_quanta_reward_async closes the keypad. Otherwise
            // (valid code, an earlier trial unfinished) close it HERE so it can't
            // linger undismissable. The LV_EVENT_DELETE self-null clears p5_np_modal.
            if (!arg_all_complete() && p5_np_modal) lv_obj_delete_async(p5_np_modal);
            return;
        }
        if (p5_np_msg) lv_label_set_text(p5_np_msg, "Not right. Re-read the code.");
        p5_entry_n = 0; p5_entry[0] = '\0';
    }
    else if (p5_entry_n < 8 && t[0] >= '0' && t[0] <= '9') { p5_entry[p5_entry_n++] = t[0]; p5_entry[p5_entry_n] = '\0'; }
    p5_np_refresh();
}

// Already unlocked -> there is no code to enter, so a keypad would reject every code
// as "wrong". Show a brief tap-to-dismiss notice instead. Reuses p5_np_modal so the
// self-null-on-delete + teardown paths are identical to the keypad's.
static void show_p5_already_unlocked(void) {
    p5_np_modal = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(p5_np_modal);
    lv_obj_set_size(p5_np_modal, 320, 240);
    lv_obj_set_style_bg_color(p5_np_modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(p5_np_modal, LV_OPA_70, 0);
    lv_obj_add_flag(p5_np_modal, LV_OBJ_FLAG_CLICKABLE);   // swallow taps behind it
    lv_obj_add_event_cb(p5_np_modal, cb_selfnull_on_delete, LV_EVENT_DELETE, &p5_np_modal);
    lv_obj_add_event_cb(p5_np_modal, [](lv_event_t *e){   // tap anywhere to dismiss
        (void)e;
        if (p5_np_modal) { lv_obj_delete_async(p5_np_modal); p5_np_modal = nullptr; }
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *lbl = lv_label_create(p5_np_modal);
    lv_label_set_text(lbl, "ALREADY UNLOCKED\n\nQuanta is yours.\n\n(tap to close)");
    lv_obj_set_style_text_font(lbl, &ui_font_pipboy_18, 0);
    lv_obj_set_style_text_color(lbl, pip_highlight(), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(lbl);
}

static void show_p5_numpad(void) {
    if (p5_np_modal) return;
    if (arg_flag(ARG_P5_PHONE)) { show_p5_already_unlocked(); return; }
    p5_entry_n = 0; p5_entry[0] = '\0';
    p5_np_modal = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(p5_np_modal);
    lv_obj_set_size(p5_np_modal, 320, 240);
    lv_obj_set_style_bg_color(p5_np_modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(p5_np_modal, LV_OPA_70, 0);
    lv_obj_add_flag(p5_np_modal, LV_OBJ_FLAG_CLICKABLE);
    // Self-null on delete: ANY teardown (X button, the reward, a serial theme_set
    // that frees scr_main) clears the pointer -> no dangling reopen-block / soft-lock.
    lv_obj_add_event_cb(p5_np_modal, cb_selfnull_on_delete, LV_EVENT_DELETE, &p5_np_modal);

    lv_obj_t *card = lv_obj_create(p5_np_modal);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 316, 238);          // nearly full-screen so the keys can be big
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, pip_bg(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, pip_highlight(), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_pad_all(card, 2, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    char hdr[40]; snprintf(hdr, sizeof hdr, "REWARD CODE  (rig %04u)", arg_nonce);
    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, hdr);
    lv_obj_set_style_text_font(title, &ui_font_pipboy_14, 0);
    lv_obj_set_style_text_color(title, pip_highlight(), 0);

#ifdef CLIPBOY_POSTCON
    // Post-con (AFTER DEFCON 34): the phone IVR is retired, so REVEAL the expected
    // code for the current nonce right on the keypad — computed by the SAME function
    // the verifier uses (arg_code_for_nonce), so it always matches arg_try_code().
    // Fully #ifdef-guarded: normal builds are byte-unchanged. Only reached in the
    // unsolved path (show_p5_numpad early-returns to show_p5_already_unlocked when
    // ARG_P5_PHONE is already set), so this never shows for an already-won badge.
    {
        char pc_code[9];
        arg_code_for_nonce(arg_nonce, pc_code);
        char pc_line[40];
        snprintf(pc_line, sizeof pc_line, "Post-con: enter %s", pc_code);
        lv_obj_t *pc_lbl = lv_label_create(card);
        lv_label_set_text(pc_lbl, pc_line);
        lv_obj_set_style_text_font(pc_lbl, &ui_font_pipboy_14, 0);
        lv_obj_set_style_text_color(pc_lbl, pip_highlight(), 0);
    }
#endif  // CLIPBOY_POSTCON

    // Close (X, top-right) — the keypad was previously inescapable. delete_async so
    // we never free the live modal mid-event (the LVGL UAF class).
    lv_obj_t *xbtn = lv_button_create(card);
    lv_obj_add_flag(xbtn, LV_OBJ_FLAG_IGNORE_LAYOUT);   // float over the flex column
    lv_obj_remove_style_all(xbtn);
    lv_obj_set_size(xbtn, 34, 26);
    lv_obj_align(xbtn, LV_ALIGN_TOP_RIGHT, 2, -1);
    lv_obj_set_style_bg_opa(xbtn, LV_OPA_TRANSP, 0);
    lv_obj_t *xl = lv_label_create(xbtn);
    lv_label_set_text(xl, "X");
    lv_obj_set_style_text_font(xl, &ui_font_pipboy_20, 0);
    lv_obj_set_style_text_color(xl, pip_highlight(), 0);
    lv_obj_center(xl);
    lv_obj_add_event_cb(xbtn, [](lv_event_t *e){
        (void)e;
        audio_play_click();
        if (p5_np_modal) { lv_obj_delete_async(p5_np_modal); p5_np_modal = nullptr; }
    }, LV_EVENT_CLICKED, nullptr);

    p5_np_disp = lv_label_create(card);
    lv_label_set_text(p5_np_disp, "----");
    lv_obj_set_style_text_font(p5_np_disp, &ui_font_pipboy_20, 0);
    lv_obj_set_style_text_color(p5_np_disp, pip_primary(), 0);
    lv_obj_add_event_cb(p5_np_disp, cb_selfnull_on_delete, LV_EVENT_DELETE, &p5_np_disp);

    lv_obj_t *bm = lv_buttonmatrix_create(card);
    lv_buttonmatrix_set_map(bm, p5_np_map);
    lv_obj_set_size(bm, lv_pct(100), 192);    // tall -> ~46px button rows (big touch targets)
    lv_obj_set_flex_grow(bm, 1);
    lv_obj_set_style_text_font(bm, &ui_font_pipboy_20, 0);
    lv_obj_set_style_pad_all(bm, 3, LV_PART_MAIN);   // small gaps between keys -> bigger keys
    lv_obj_set_style_bg_color(bm, pip_bg_dark(), LV_PART_ITEMS);
    lv_obj_set_style_text_color(bm, pip_primary(), LV_PART_ITEMS);
    lv_obj_set_style_border_color(bm, pip_border(), LV_PART_ITEMS);
    lv_obj_set_style_border_width(bm, 1, LV_PART_ITEMS);
    lv_obj_set_style_radius(bm, 4, LV_PART_ITEMS);
    lv_obj_add_event_cb(bm, p5_np_event, LV_EVENT_VALUE_CHANGED, nullptr);

    p5_np_msg = lv_label_create(card);
    lv_label_set_text(p5_np_msg, "Call 775-CLIP-BOY, then key the code.");
    lv_obj_set_style_text_font(p5_np_msg, &ui_font_pipboy_14, 0);
    lv_obj_set_style_text_color(p5_np_msg, pip_dim(), 0);
    lv_obj_add_event_cb(p5_np_msg, cb_selfnull_on_delete, LV_EVENT_DELETE, &p5_np_msg);
}

static void p5_enter(bool replay) {
    (void)replay;
    show_p5_numpad();   // on-screen entry (handoff §6.5: keypad OR serial)
    Serial.println(F("\r\n=== THE CALL ==="));
    Serial.println(F("The signal won't trust a recording. Prove you're a person."));
    Serial.println(F("Call +1-775-CLIP-BOY (775-254-7269)."));
    Serial.printf ("Your rig's number is: %04u  — SMS it to that line.\r\n", arg_nonce);
    Serial.println(F("You'll get a code back. Enter it on your badge keypad, or here:"));
    Serial.println(F("    clipcli unlock <code>"));
}

static void p5_hint(int level) {
    switch (level) {
        case 1:  Serial.println(F("[CLIP] No keypad on screen? Open it with the chrome knock -- tap, in order:"));
                 Serial.println(F("[CLIP]   STATS . light . light . DATA . ITEMS  (\"light\" = the flashlight button)."));
                 Serial.println(F("[CLIP]   (Or, over serial, 'clipcli challenge' opens it directly.)")); break;
        case 2:  Serial.println(F("[CLIP] The number on your screen is the key. SMS it to 775-CLIP-BOY; it answers with a code. Don't guess it.")); break;
        default: Serial.println(F("[CLIP] Call/text +1-775-254-7269, send the number on screen, type back what it says (keypad or 'clipcli unlock <code>').")); break;
    }
}

static void p5_reset(void) { /* nonce is durable per-badge; the dispatcher clears the P5 bit */ }

static void p5_register(void) {
    arg_puzzles[5].enter = p5_enter;
    arg_puzzles[5].hint  = p5_hint;
    arg_puzzles[5].reset = p5_reset;
    arg_unlock_fn = p5_submit_code;   // wire `clipcli unlock <code>`
    arg_reveal_keypad_fn = show_p5_numpad;  // wire the secret-menu tap sequence -> reveal keypad
    arg_on_complete_fn = arg_apply_quanta_reward;  // fire the reward the moment the set completes (any order)
}
