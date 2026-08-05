#!/usr/bin/env py -3
"""test_teardown_paths.py -- regression tests for the 2026-07-24 audit ship-blockers.

Every test here MUST FAIL on firmware at 5d1c8112 (bug present) and pass after the fix.
That ordering is the point: a test written after the fix only proves the code still does
what it currently does.

  SB1  nested kb_open  -> "+ Add Network" adds nothing, then wedges the UI, then reboots
  SB2  show_help()     -> third content-teardown path; live timers write freed widgets
  SB3  status-bar tap  -> silently kills a running theremin, UI still claims it is active
  FB7  screensaver     -> the hold-to-unlock BAR swallows the only wake gesture
  R1   status bar      -> manual station scan reports a stale AP count (Res34rch only)

Needs a --test build (TEST_HARNESS) and the `find` command (added 2026-07-25).
Several of these can CRASH or WEDGE the badge -- that is the bug -- so each test recovers
with a reboot before the next one runs.

  CLIPBOY_PORT=COM4 py -3 Clip-Boy/scripts/tests/test_teardown_paths.py
  py -3 Clip-Boy/scripts/tests/test_teardown_paths.py --port COM5
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Harness

RESULTS = []
# Set once in main() so record() can auto-capture without every test signature growing an argument.
# Module-global rather than plumbed through: record() is called from ~20 places and threading a
# harness handle through all of them is exactly the kind of change that misses the 21st.
CURRENT_H = None


def _a(x):
    """ASCII-safe for printing. Badge labels legitimately contain non-ASCII (the pip-boy
    font covers 0x20-0x2EFF), and a Windows cp1252 console raises UnicodeEncodeError on
    them -- which would crash the suite instead of reporting the test result."""
    return str(x).encode("ascii", "replace").decode("ascii")


def _labels(h):
    return [t for t in ((h.cmd("text") or {}).get("texts") or []) if isinstance(t, str)]


def _join_ssid(h):
    """The Join WiFi page's SSID, to prove Add Network no longer retargets it."""
    r = h.cfg_get("wifi_ssid") or {}
    return r.get("value") if r.get("ok") else None


def record(name, passed, detail, blocker=True, skipped=False):
    """`skipped=True` prints [SKIP], never [PASS].

    A skip that prints PASS is how an un-run test reads as a passing one. R1 printed
    "[PASS] R1: skipped: Sn34k build has no TAT_STA tools" on a badge that was demonstrably
    res34rch (its `Deauth` category was there when queried seconds later -- cat_pos had read a
    TRUNCATED tool_list). The suite summary was honest, because the row was blocker=False and
    outside the tally, but the per-line output said PASS and that is what a human reads."""
    RESULTS.append({"name": name, "passed": passed, "detail": detail,
                    "blocker": blocker, "skipped": skipped})
    tag = "SKIP" if skipped else ("PASS" if passed else ("FAIL" if blocker else "warn"))
    print(_a(f"  [{tag}] {name}: {detail}"))
    # AUTO-CAPTURE on anything that is not a plain pass. Every other observation this suite makes
    # goes through the same object-tree walk, so a failure explained only from tree readings can be
    # explained WRONGLY -- the framebuffer is the one independent channel. Skips are captured too:
    # a skip is a claim about the device ("this SKU lacks the tool") and the pixels either support
    # it or they do not. Never on a pass: each shot costs a couple of seconds and ~230 KB.
    if (not passed or skipped) and CURRENT_H is not None:
        p = CURRENT_H.snap_on_failure(name)
        if p:
            RESULTS[-1]["shot"] = p
            print(f"        (screenshot: {os.path.basename(p)})")


def uptime(h):
    """millis() from the badge, or None. Reboot detection needs this: a crash + auto-reboot
    leaves the badge answering pings normally, so liveness proves nothing."""
    st = h.state() or {}
    return st.get("uptime_ms")


def rebooted(h, prev_uptime):
    """Did the badge restart since `prev_uptime`? Also returns the reset reason (4=PANIC,
    5=INT_WDT, 6=TASK_WDT, 3=SW)."""
    st = h.state() or {}
    now = st.get("uptime_ms")
    rr = st.get("reset_reason")
    if now is None or prev_uptime is None:
        return False, rr
    return (now < prev_uptime), rr


def alive(h, tries=2):
    """Is the badge still answering? A crash is the SB2 symptom, not a test error."""
    for _ in range(tries):
        try:
            if (h.ping() or {}).get("ok"):
                return True
        except Exception:
            pass
        time.sleep(0.5)
    return False


def recover(h):
    """Reboot back to a known state; several tests deliberately wedge the UI."""
    try:
        h.reboot_and_wait()
        h.cmd("skip_boot")
        return alive(h)
    except Exception as e:
        print(f"    (recover failed: {e})")
        return False


# ── SB1 ──────────────────────────────────────────────────────────────────────
def test_sb1_nested_keyboard(h):
    """kb_open's OK handler runs the done-callback BEFORE kb_close(). Add Network nests a
    second kb_open for the password, so kb_close() deletes the NEW modal and orphans the
    old one with kb_modal == NULL -> Cancel becomes a no-op and OK NULL-derefs."""
    print("\n-- SB1: nested kb_open (Utilities > Saved Networks > + Add Network) --")
    up = h.cat_pos("Utilities/Lists")
    if up is None:
        record("SB1", False, "Utilities/Lists category not found", blocker=True)
        return
    h.cmd(f"tool_open {up} 6")          # Saved Networks
    h.wait(700)
    # Clear rows this test left behind on earlier runs. Saved networks persist in NVS, and a
    # growing list pushes "+ Add Network" below the content pane -- where its reported
    # coordinates land on the division bar, so the tap navigates instead of opening the
    # keyboard. Self-cleaning keeps the fixture bounded and the button reachable.
    for _ in range(8):
        rows = [t for t in _labels(h) if t.startswith("cb") and ("(pw)" in t or "(open)" in t)]
        if not rows:
            break
        if not h.tap_text("=X", settle_ms=700):
            break
        h.cmd(f"tool_open {up} 6")
        h.wait(500)
    if not h.tap_text("Add Network", settle_ms=900):
        record("SB1", False,
               f"could not tap '+ Add Network' ({getattr(h, 'last_tap_error', None)})",
               blocker=True)
        return
    if not h.find("Cancel"):
        # Diagnostics: this failure is intermittent (~40% in-suite, 0% in every isolated
        # replica), and two hypotheses have already been refuted -- button placement below the
        # fold, and the X-delete button's synchronous rebuild_content(). Capture the state
        # instead of guessing a third time.
        st = h.state() or {}
        lab = _labels(h)
        print(f"     [diag] uptime={st.get('uptime_ms')} reset={st.get('reset_reason')} "
              f"div={st.get('div')} tab={st.get('tab')} sel={st.get('sel')} "
              f"boot_visible={st.get('boot_visible')} screensaver={st.get('screensaver')}")
        print(f"     [diag] retry find(Cancel)={_a(h.find('Cancel'))}")
        print(f"     [diag] 'Cancel' in labels={'Cancel' in lab} "
              f"'Network SSID' in labels={'Network SSID' in lab}")
        print(f"     [diag] labels={_a(lab[:18])}")
        record("SB1", False, "no keyboard modal appeared after tapping Add Network")
        return
    # UNIQUE per run. Saved networks persist in NVS, so a fixed name let this test pass on a
    # row left by a PREVIOUS run: it asserted only that a row containing the name existed, and
    # an earlier run's "cbtest (open)" satisfied that while this run's write went unverified.
    # A test that can pass on a previous run's artifact is not testing anything.
    ssid = f"cb{int(time.time()) % 100000}"
    h.kb_type(ssid)
    h.wait(300)
    ok_resp = h.kb_ok()                  # commits the SSID -> should open the PASSWORD prompt
    h.wait(900)

    pw_prompt = h.find("Password")
    kb_still_up = bool(h.find("Cancel"))
    # The "second OK" is the buggy-build probe (it is the tap that NULL-derefs when the modal
    # was orphaned). Only issue it when the password prompt did NOT appear. On a FIXED build a
    # speculative second OK commits the password step with an empty textarea -- saving the
    # network as "(open)" -- so the diagnostic itself caused the failure it then reported.
    # Firmware is right to accept it: the prompt is literally "Password (blank=open)", and the
    # 250 ms commit guard only suppresses an accidental FAST double-tap, not a deliberate OK
    # ~900 ms later.
    second_ok = None
    crashed = False
    if not pw_prompt:
        second_ok = h.kb_ok()
        h.wait(600)
        crashed = not alive(h)

    if pw_prompt:
        # Coverage gap closed (adversarial review 2026-07-25): passing as soon as "Password"
        # appears proved only that the nesting no longer eats the new modal. It could not
        # detect a LEAKED outer modal (the new one is on top, so find() succeeds either way),
        # and it never exercised the kb_pending_ssid half of the fix at all.
        join_ssid_before = _join_ssid(h)
        h.kb_type("hunter2")
        h.wait(500)
        h.kb_ok()
        h.wait(1200)
        leftover = bool(h.find("Cancel"))          # any keyboard still up == leaked modal
        # Re-open the page before looking for the row: the password callback ends with
        # rebuild_content(), which returns to the Tools CATEGORY LIST, not this detail pane.
        # Checking labels straight after the save looked like "not saved" when the credential
        # was in fact stored -- the test was reading the wrong screen.
        h.cmd(f"tool_open {up} 6")
        h.wait(900)
        saved = [t for t in _labels(h) if ssid in t]
        join_ssid_after = _join_ssid(h)
        if leftover:
            record("SB1", False, "network flow completed but a keyboard modal is STILL up "
                                 "(outer modal leaked)")
        elif not saved:
            record("SB1", False, f"password step completed but '{ssid}' is not in Saved Networks")
        elif not any("(pw)" in t for t in saved):
            record("SB1", False, f"row saved WITHOUT the password: {saved!r} -- the second "
                                 f"prompt committed blind (expected '(pw)', got '(open)')")
        elif join_ssid_before is not None and join_ssid_after != join_ssid_before:
            record("SB1", False, f"Add Network clobbered the Join WiFi SSID "
                                 f"({join_ssid_before!r} -> {join_ssid_after!r})")
        else:
            record("SB1", True, f"full SSID->password->save flow works; saved={saved[:1]} "
                                f"(password stored); no leftover modal; Join WiFi untouched")
    elif kb_still_up and not (second_ok or {}).get("ok"):
        detail = ("orphaned keyboard: no password prompt, a modal is still on screen, and "
                  "kb_ok reports no modal open"
                  + (" -- BADGE CRASHED on the second OK" if crashed else ""))
        record("SB1", False, detail)
    else:
        record("SB1", False, f"unexpected state: pw={bool(pw_prompt)} kb_up={kb_still_up} "
                             f"ok1={(ok_resp or {}).get('ok')} ok2={(second_ok or {}).get('ok')} "
                             f"crashed={crashed}")


# ── SB2 ──────────────────────────────────────────────────────────────────────
def test_sb2_help_teardown(h):
    """The status-bar `?` calls show_help(content_obj), which clear_children()s the pane
    without any teardown. The geiger (1 Hz) and theremin (50 Hz) timers keep writing widget
    globals that have no self-null, into memory show_help has already reused."""
    print("\n-- SB2: show_help() as a third teardown path --")
    # `armed` is a PRECONDITION probe, not decoration: this test's whole premise is that a
    # live poller writes into memory show_help() has reused. If arming silently fails there is
    # no orphaned timer, nothing can crash, and the test passes forever while testing nothing.
    # It is a ship-blocker test (it caught a real ESP_RST_PANIC), so it must refuse to score a
    # pass it did not earn.
    for label, arm, disarm, armed, settle in (
        ("geiger", lambda: h.geiger_start(), lambda: h.geiger_stop(),
         lambda: bool((h.tool_state() or {}).get("geiger_active")), 3000),
        ("theremin", lambda: (h.sensor_mock(",".join(["120"] * 64)), h.theremin_start()),
         lambda: h.theremin_stop(),
         lambda: bool((h.state() or {}).get("theremin_want")), 3000),
    ):
        h.nav(0, 2) if label == "geiger" else h.nav(2, 2)
        h.wait(700)
        arm()
        h.wait(800)
        if not armed():
            record(f"SB2/{label}", False,
                   f"cannot test: the {label} poller did not arm, so there is no live timer to "
                   f"collide with show_help() and a 'survived' result would be vacuous",
                   blocker=True)
            try:
                disarm()
            except Exception:
                pass
            continue
        before = (h.heap() or {}).get("free_dram")
        up0 = uptime(h)
        h.cmd("info_show help")          # identical entry point to the `?` handler
        h.wait(settle)                   # let the orphaned timer fire several times
        if not alive(h):
            record(f"SB2/{label}", False,
                   "badge stopped responding after `?` with the poller running "
                   "(freed-widget write, did not recover on its own)")
            recover(h)
            continue
        # A UAF here CRASHES and the badge auto-reboots, which a liveness ping cannot see.
        crashed, rr = rebooted(h, up0)
        after = (h.heap() or {}).get("free_dram")
        txt = h.cmd("text") or {}
        labels = [t for t in (txt.get("texts") or []) if isinstance(t, str)]
        help_up = any("Help" in t or "Back" in t for t in labels)
        try:
            disarm()
        except Exception:
            pass
        if crashed:
            record(f"SB2/{label}", False,
                   f"badge REBOOTED within {settle} ms of `?` with the {label} poller live "
                   f"(reset_reason={rr}; 4=PANIC 5=INT_WDT 6=TASK_WDT) -- freed-widget write")
        elif help_up:
            record(f"SB2/{label}", True,
                   f"survived {settle} ms on Help with the {label} poller live, no reboot"
                   + (f" (dram {before}->{after})" if (before and after) else ""))
        else:
            record(f"SB2/{label}", False,
                   f"no reboot, but Help did not render either (labels={labels[:6]})")
        h.nav(0, 0)
        h.wait(500)



# ── SB2b (regression from the SB2 fix itself) ────────────────────────────────
def test_sb2b_theme_switch_keeps_geiger_live(h):
    """A theme change must not freeze the Radiation gauge.

    ui_theme_switch_live() builds the NEW screen before freeing the old one, so the
    builders re-point rad_* at fresh widgets and THEN the old widgets' LV_EVENT_DELETE
    handlers run. An unconditional self-null therefore wiped pointers that referred to live
    widgets: needle + every stat label froze permanently while the tick audio kept playing
    and the status bar still claimed the Geiger was active. rad_poll_cb is all `if (ptr)`,
    so every write was silently dropped. Caught by adversarial review, not by a human.

    Detection: the elapsed-time label must keep advancing across a theme switch.
    """
    print("")
    print("-- SB2b: theme switch must not freeze the Radiation readouts --")
    saved_theme = (h.cfg_get("theme") or {}).get("value")
    try:
        h.nav(0, 2)
        h.wait(700)
        h.geiger_start()
        h.wait(1500)

        def elapsed():
            for t in _labels(h):
                if t.count(":") == 2 and len(t) == 8 and t.replace(":", "").isdigit():
                    return t
            return None

        before = elapsed()
        if before is None:
            record("SB2b", False, "could not find the elapsed-time label; cannot test",
                   blocker=False)
            return
        # Must be a DIFFERENT index -- ui_theme_switch_live early-returns on a no-op.
        target = 1 if (saved_theme in (0, "0", None)) else 0
        h.cmd(f"theme_set {target}")
        h.wait(1500)
        t1 = elapsed()
        h.wait(2500)
        t2 = elapsed()
        if t1 is None or t2 is None:
            record("SB2b", False, f"elapsed label missing after theme switch (t1={t1} t2={t2})")
        elif t1 == t2:
            record("SB2b", False, f"Radiation readouts FROZE after the theme switch "
                                  f"(elapsed stuck at {t2}) -- rad_* globals were nulled while "
                                  f"pointing at live widgets")
        else:
            record("SB2b", True, f"readouts kept advancing across the theme switch "
                                 f"({before} -> {t1} -> {t2})")
    finally:
        try:
            h.geiger_stop()
            if saved_theme is not None:
                h.cmd(f"theme_set {saved_theme}")
        except Exception:
            pass


# ── SB3 ──────────────────────────────────────────────────────────────────────
def test_sb3_statusbar_kills_theremin(h):
    """audio_mp3_play() pre-empts the theremin, and audio_play_click() routes into it for ANY
    button-class widget -- including the always-present status-bar buttons. Pre-fix the synth
    died permanently (bare flag clear, no re-arm outside theremin_enable) while the button
    still read Disable and the bars froze. Owner-confirmed on hardware 2026-07-25.

    Post-fix (CB_THEREMIN_DUCK_CLICK=1) the click DUCKS it: theremin_active drops for the
    ~100 ms clip, then the core-0 task restores it from theremin_want.

    This test earned three assertions the original lacked -- it reported "survived" without
    ever proving the tap landed, which is why SB3 read as non-reproducible for a full day:
      1. PRECONDITIONS read from the DEVICE (cfg.sound / cfg.ui_click). Source defaults are
         not device state; with either off, audio_play_click early-returns and nothing
         happens no matter how broken the code is.
      2. A CONTROL assertion -- the flashlight must toggle. The FL button is 26x16 px at the
         very top edge; without an independent side effect, "the theremin survived" and "my
         tap missed the button" are the same reading.
      3. want-vs-active, so a genuine duck is not scored as a kill."""
    print("\n-- SB3: status-bar tap must not kill a running theremin --")
    restore = {}
    try:
        # -- 1. preconditions, device-side, and VERIFY the writes took (a silently rejected
        #       cfg_set looks exactly like a successful one).
        for key in ("sound", "ui_click"):
            cur = (h.cfg_get(key) or {}).get("value")
            if cur is not True:
                restore[key] = cur
                h.cfg_set(key, True)
                back = (h.cfg_get(key) or {}).get("value")
                if back is not True:
                    record("SB3", False,
                           f"cannot test: cfg.{key} is {cur!r} and cfg_set was rejected "
                           f"(read back {back!r}) -- no click would be produced",
                           blocker=False)
                    return

        h.nav(2, 2)
        h.wait(700)
        h.sensor_mock(",".join(["120"] * 64))
        h.theremin_start()
        h.wait(900)
        st = h.state() or {}
        if not st.get("theremin_active") or not st.get("theremin_want"):
            record("SB3", False,
                   f"cannot test: theremin did not start "
                   f"(active={st.get('theremin_active')!r} want={st.get('theremin_want')!r})",
                   blocker=True)
            return
        fl_before = bool(st.get("flashlight"))

        # -- 2. stimulus: the one always-present button whose handler does NOT tear the
        #       theremin down (help/nav/division buttons all would, masking the bug).
        if not h.tap_text("FL", settle_ms=900):
            record("SB3", False,
                   f"cannot test: could not locate the status-bar FL button "
                   f"({getattr(h, 'last_tap_error', 'no reason given')})", blocker=True)
            return

        # -- 3. CONTROL: did the tap actually reach the button?
        st2 = h.state() or {}
        if bool(st2.get("flashlight")) == fl_before:
            record("SB3", False,
                   "cannot test: flashlight did not toggle, so the tap never landed on the "
                   "FL button -- any theremin reading here would be meaningless",
                   blocker=True)
            return

        # Well past the ~100 ms click plus the task's reconcile, so a duck has resolved.
        h.wait(1200)
        st3 = h.state() or {}
        active = bool(st3.get("theremin_active"))
        want = bool(st3.get("theremin_want"))

        if want and active:
            # Deliberately does NOT name a mechanism. This test asserts only that the synth is
            # still running after the tap, which is true of BOTH shipped modes -- duck-and-resume
            # (CB_THEREMIN_DUCK_CLICK=1) and silent-click (=0, the default since 2026-07-26). The
            # message used to say "ducked and resumed", which described behaviour that no longer
            # happens on the default build: a passing test that narrates the wrong mechanism is
            # how a future reader concludes a feature is active when it is compiled out.
            record("SB3", True, "theremin still running after a status-bar tap "
                                "(true of both silent-click and duck-and-resume modes)")
        elif want and not active:
            record("SB3", False,
                   "SB3: theremin_want is still set but theremin_active is false 1.2 s after "
                   "the tap -- the synth is dead while the UI still shows it enabled "
                   "(pre-fix: bare flag clear; post-fix: the resume never fired)")
        else:
            record("SB3", False,
                   "theremin_want was CLEARED by a status-bar tap -- an explicit stop ran "
                   "where only a duck should have (check audio_theremin_stop callers)")
    finally:
        try:
            h.theremin_stop()
        except Exception:
            pass
        for key, val in restore.items():
            try:
                h.cfg_set(key, bool(val))
            except Exception:
                pass


# ── FB7 ──────────────────────────────────────────────────────────────────────
def test_fb7_screensaver_bar(h):
    """lv_bar keeps LV_OBJ_FLAG_CLICKABLE, so a press landing on the progress bar is
    terminated there and never reaches the overlay's handlers -- no backlight bump, no
    tone, no unlock. In the default mascot style the bar sits at the bottom edge."""
    print("\n-- FB7: screensaver hold-bar swallows the unlock press --")
    saved_style = (h.cfg_get("ss_style") or {}).get("value")
    saved_off = (h.cfg_get("disp_off") or {}).get("value")
    try:
        h.cfg_set("ss_style", 1)         # Blank: bar is dead-centre, deterministic
        h.cfg_set("disp_off", 0)
        h.wait(400)
        armed = False
        for _ in range(30):
            h.wait(1000)
            if (h.state() or {}).get("screensaver"):
                armed = True
                break
        if not armed:
            record("FB7", False, "screensaver never armed; cannot test", blocker=False)
            return
        # Press ON the bar (y~135 in Blank style) and hold well past the 2 s unlock.
        h.touch(160, 135, "press")
        time.sleep(2.6)
        on_bar = bool((h.state() or {}).get("screensaver"))
        h.touch(160, 135, "release")
        h.wait(600)
        # Control: the label just above the bar DOES unlock, proving the press path works.
        if on_bar:
            h.touch(160, 110, "press")
            time.sleep(2.6)
            on_label = bool((h.state() or {}).get("screensaver"))
            h.touch(160, 110, "release")
            h.wait(400)
            if not on_label:
                record("FB7", False, "press on the hold-bar did NOT unlock, while a press "
                                     "10 px above (the label) did -- the bar eats the event")
            else:
                record("FB7", False, "neither the bar nor the label unlocked -- "
                                     "different problem, investigate", blocker=False)
        else:
            record("FB7", True, "press on the hold-bar unlocked the screensaver")
    finally:
        if saved_style is not None:
            h.cfg_set("ss_style", saved_style)
        if saved_off is not None:
            h.cfg_set("disp_off", saved_off)


# ── FB8 ──────────────────────────────────────────────────────────────────────
def test_fb8_battery_not_zero_at_boot(h):
    """The smoothing ring was zero-initialised while BAT_Get_Volts() always divided by the
    full NUM_SAMPLES, so the first reading was ~1/10th of the true voltage -- below the
    3.25 V floor of the percentage table, i.e. 0%. The first caller is the boot POST line,
    so every badge told its buyer the battery was dead on the very first frame, and the
    status bar then read BAT 0% for ~10 more ticks.

    Detection: reboot, then read the status bar as early as possible. On a charged badge it
    must not say 0%.
    """
    print("")
    print("-- FB8: battery must not read 0% straight after boot --")
    if not recover(h):                    # fresh boot
        record("FB8", False, "could not reboot the badge", blocker=False)
        return
    readings = []
    for _ in range(4):
        for t in _labels(h):
            if t.startswith("BAT "):
                readings.append(t)
                break
        h.wait(700)
    if not readings:
        record("FB8", False, "no BAT label found in the status bar", blocker=False)
        return
    first = readings[0]
    if first.strip() == "BAT 0%":
        record("FB8", False, f"status bar reads {first!r} immediately after boot "
                             f"(sequence {readings}) -- ring diluted by zero samples")
    else:
        record("FB8", True, f"battery reads {first!r} right after boot (sequence {readings})")


# ── FB11/FB12 ────────────────────────────────────────────────────────────────
def test_fb11_geiger_audio_latch(h):
    """Stopping the Geiger by any route must release its tick audio.

    `rad_geiger_active = false` appeared at nine sites; only the Radiation Stop button ran the
    full teardown. The others skipped rad_geiger_audio_update(0) -- the only route to
    audio_geiger_stop() -- so `aud_geiger_active` latched TRUE for the session. That branch
    sits ABOVE aud_tone_active in the core-0 priority chain, so the screensaver tap-and-hold
    rising tone and the unlock chime were DEAD while the hold still unlocked, silently.
    Audible clicking only continued if the frozen rate was nonzero, which is why a quiet bench
    never caught it -- so assert the LATCH STATE, not the sound.
    """
    print("")
    print("-- FB11/FB12: geiger tick audio must not stay latched after a stop --")
    sp = h.cat_pos("Scan")
    routes = [
        ("geiger_stop cmd", lambda: h.geiger_stop()),
        ("tool start over geiger", lambda: h.tool_start(sp, 0)),
    ]
    failures = []
    for name, stop in routes:
        h.nav(0, 2)
        h.wait(600)
        h.geiger_start()
        h.wait(1200)
        if not (h.state() or {}).get("geiger_audio"):
            # Latch only arms when cfg.sound is on; without it the route is untestable.
            record("FB11", False, f"geiger_audio never armed (cfg.sound off?) -- cannot test",
                   blocker=False)
            return
        stop()
        h.wait(1000)
        st = h.state() or {}
        if st.get("geiger_audio"):
            failures.append(name)
        try:
            h.tool_stop()
        except Exception:
            pass
        h.wait(400)
    if failures:
        record("FB11", False, f"tick audio still latched after: {', '.join(failures)} "
                              f"-- the screensaver unlock tone is dead for the session")
    else:
        record("FB11", True, "tick audio released on every stop route tested")


# ── FB5/FB6 ──────────────────────────────────────────────────────────────────
def test_fb5_joinwifi_stop_really_stops(h):
    """Join WiFi's Scan/Stop button: the label must match what a tap does (FB6), and a stop
    must actually stop the LIBRARY, not just the UI's bookkeeping (FB5).

    FB5: the handler cleared cb_op_running but never called cb.stopScan() -- the only thing
    that clears WiFiScan::currentScanMode -- so cb.loop() kept driving wifi_scan_obj.main()
    (where all active TX lives) with the status bar reading "Stopped". Detected via
    `lib_scanning`, the library's own view: every UI-side signal says "not running", and that
    disagreement IS the bug. This is what made the audit call it unprovable without an
    external RF witness.

    FB6: the label asked strstr(cb_op_name,"Scan") while the action branched on bare
    cb_op_running, so with an unrelated tool running it read "Scan" but the tap merely STOPPED
    that tool -- two taps to scan. Correct behaviour now: label "Scan" -> a tap STARTS a scan.
    """
    print("")
    print("-- FB5/FB6: Join WiFi Scan/Stop -- label==action, and stop really stops --")
    np_ = h.cat_pos("Network")
    sp = h.cat_pos("Scan")
    if np_ is None or sp is None:
        record("FB5", False, "Network/Scan category not found", blocker=True)
        return

    def open_join_and_reveal_button():
        """Open Join WiFi and scroll until the Scan/Stop button is actually tappable.
        The page is taller than the content pane, so the button starts below it -- my own
        `onscreen` guard caught exactly this, which is why the scroll is explicit."""
        h.cmd(f"tool_open {np_} 0")
        h.wait(900)
        for _ in range(6):
            for want in ("=Stop", "=Scan"):
                hits = h.find_exact(want[1:])
                if hits and hits[0].get("onscreen"):
                    return want, hits[0]
            h.swipe(160, 190, 160, 90)      # drag content up to reveal the button row
            h.wait(500)
        return None, None

    # ── Phase 1 (FB6): an UNRELATED tool is running -> label "Scan" must START a scan ──
    h.tool_start(sp, 3)                      # Scan > BT Devices: a tool whose name lacks "Scan"
    h.wait(2000)
    want, hit = open_join_and_reveal_button()
    if not want:
        record("FB5", False, "could not bring the Scan/Stop button on-screen", blocker=True)
        try: h.tool_stop()
        except Exception: pass
        return
    phase1_label = want[1:]
    h.touch(hit["hit_x"], hit["hit_y"], "tap")
    h.wait(2500)
    st1 = h.tool_state() or {}
    started_scan = bool(st1.get("running")) and "Scan" in (st1.get("name") or "")
    # POSITIVE CONTROL for the observable this test hinges on. Phase 2 asserts
    # lib_scanning went FALSE; if the getter were broken and always read False, that
    # assertion would pass forever. Prove it reads TRUE with a scan actually running.
    lib_pos = st1.get("lib_scanning")

    # ── Phase 2 (FB5): now an AP scan IS running -> a tap must stop UI *and* library ──
    want2, hit2 = open_join_and_reveal_button()
    if not want2:
        record("FB5", False, "button not reachable for the stop phase", blocker=True)
        return
    phase2_label = want2[1:]
    h.touch(hit2["hit_x"], hit2["hit_y"], "tap")
    h.wait(1800)
    st2 = h.tool_state() or {}
    ui_running = bool(st2.get("running"))
    lib_scanning = st2.get("lib_scanning")
    try:
        h.tool_stop()
    except Exception:
        pass

    if lib_scanning is None:
        record("FB5", False, "firmware does not report lib_scanning (stale build?)", blocker=False)
        return
    if started_scan and lib_pos is not True:
        record("FB5", False,
               f"cannot test the FB5 half: lib_scanning read {lib_pos!r} while a scan WAS "
               f"running (UI running={st1.get('running')}, name={st1.get('name')!r}), so a "
               f"False reading after the stop would prove nothing", blocker=False)
        return
    problems = []
    if phase1_label != "Scan":
        problems.append(f"phase1 label was {phase1_label!r}, expected 'Scan' with a BT tool running")
    if not started_scan:
        problems.append(f"tapping 'Scan' did not start a scan (running={st1.get('running')}, "
                        f"name={st1.get('name')!r}) -- label said Scan but the tap only stopped "
                        f"the other tool, i.e. two taps to scan")
    if phase2_label != "Stop":
        problems.append(f"phase2 label was {phase2_label!r}, expected 'Stop' during an AP scan")
    if ui_running:
        problems.append("UI still reports running after the stop tap")
    if lib_scanning is True:
        problems.append("lib_scanning STILL TRUE -- cb.stopScan() was never called, so the "
                        "library keeps scanning (and on Res34rch, TRANSMITTING) while the UI "
                        "says stopped")
    if problems:
        record("FB5", False, "; ".join(problems))
    else:
        record("FB5", True, "label matched the action in both phases; stop cleared BOTH the UI "
                            "and the library (lib_scanning=false)")


# ── FB1 ──────────────────────────────────────────────────────────────────────
def test_fb1_promiscuous_torn_down(h):
    """After a tool stop, the badge must stop RECEIVING frames.

    shutdownWiFi()'s esp_wifi_set_promiscuous(false) sits behind `if (!wifi_connected)`, and
    that flag latches true on the first cb.loop() -- so promiscuous mode and its RX callback
    survived every tool stop. Detection needs no RF rig: with nothing running, the packet
    counters must not climb.
    """
    print("")
    print("-- FB1: promiscuous mode must be off after a tool stop --")
    sp = h.cat_pos("Scan")

    # POSITIVE CONTROL first: prove the observable can read TRUE, otherwise a false reading
    # after the stop means nothing. Three earlier attempts at this test were vacuous for
    # exactly this reason -- see the comment on `promisc` in test_harness.h.
    h.tool_start(sp, 0)
    h.wait(4000)
    on = (h.tool_state() or {}).get("promisc")
    if on is None:
        record("FB1", False, "cannot test: this build's tool_state has no 'promisc' field "
                             "(needs the FB1 harness observable)", blocker=False)
        return
    if not on:
        record("FB1", False, "cannot test: promisc read FALSE while a scan is running, so the "
                             "observable cannot distinguish the states", blocker=True)
        return

    h.tool_stop()
    h.wait(1500)
    st = h.tool_state() or {}
    off = bool(st.get("promisc"))
    print(f"  promisc: running=True -> stopped={off}; lib_scanning={st.get('lib_scanning')}")
    if st.get("lib_scanning"):
        record("FB1", False, "lib_scanning still true after tool_stop")
    elif off:
        record("FB1", False, "promiscuous mode is STILL ENABLED after tool_stop -- the badge "
                             "keeps receiving and burning power; FB1's teardown did not take")
    else:
        record("FB1", True, "promiscuous mode enabled while scanning and disabled after stop")


def test_fb1b_capture_reinit(h):
    """FB1's REGRESSION guard, not its feature test.

    FB1 adds esp_wifi_set_promiscuous_rx_cb(NULL) + set_promiscuous(false) to
    cb_stop_operation(). The hazard is the SECOND start: this same function once broke
    Raw/PCAP's promiscuous re-init with a WiFi.mode(WIFI_OFF) and the capture path stayed
    dead until a power cycle. A test that only proves "receiving stops after a stop" would
    score that regression as a PASS -- it is the same observable.

    So: capture, stop, capture AGAIN, and require the counters to climb the second time.
    """
    print("")
    print("-- FB1b: promiscuous must be RE-ARMED on a tool start after a previous stop --")
    ap = h.cat_pos("Analyze")
    sp = h.cat_pos("Scan")
    if ap is None or sp is None:
        record("FB1b", False, "cannot test: Analyze/Scan category not found", blocker=False)
        return

    # Cycle 1: run and stop a tool -- this is what executes FB1's new teardown.
    h.tool_start(sp, 0)
    h.wait(3000)
    h.tool_stop()
    h.wait(1500)
    mid = bool((h.tool_state() or {}).get("promisc"))

    # Cycle 2: a passive sniffer must bring promiscuous back up. Analyze[0] is "Beacons";
    # do NOT use Analyze[3] Raw/PCAP -- on a stock Sn34k build the PCAP-saving gate returns
    # early and the tool never starts at all (verified: lib_scanning=False, name='').
    h.tool_start(ap, 0)
    h.wait(2500)
    st = h.tool_state() or {}
    back = bool(st.get("promisc"))
    print(f"  promisc: after stop={mid} -> after restart={back}; "
          f"lib_scanning={st.get('lib_scanning')} name={st.get('name')!r}")
    h.tool_stop()

    if not st.get("lib_scanning"):
        record("FB1b", False, "the sniffer reports lib_scanning=false after start -- it never "
                              "came up on the second cycle, so re-arm cannot be judged")
    elif not back:
        record("FB1b", False,
               "promiscuous was NOT re-enabled by a tool start after a previous stop -- FB1's "
               "teardown is not being re-armed, so every capture after the first is dead. "
               "This is the exact regression cb_stop_operation has caused before; revert FB1")
    else:
        record("FB1b", True, "promiscuous re-armed on the second tool start after a stop")


def _status_task_text(h):
    """Whatever the centre status-bar task slot is currently asserting."""
    txt = h.cmd("text") or {}
    for t in (txt.get("texts") or []):
        if isinstance(t, str) and "Geiger" in t:
            return t
    return ""


# ── F3 ───────────────────────────────────────────────────────────────────────
def test_f3_geiger_status_label_cleared(h):
    """rad_geiger_force_stop() -- self-billed as "the ONE way to stop the Geiger" -- reset the
    needle, button, timer and audio but never the status-bar text, so "Geiger active" stuck on
    EVERY screen permanently. cb_stop_operation() does clear it, but from a site inside its
    `if (cb_op_running)` block while it calls the helper from outside it, and rad_toggle_cb
    never sets cb_op_running -- so that clear could never fire for the Geiger.
    Route used here: Airplane ON (the cleanest of the three)."""
    print("")
    print("-- F3: 'Geiger active' must not stick in the status bar after a forced stop --")
    saved_air = (h.cfg_get("airplane") or {}).get("value")
    try:
        if saved_air:
            h.cfg_set("airplane", False)
            if (h.cfg_get("airplane") or {}).get("value") is not False:
                record("F3", False, "cannot test: could not turn airplane mode off",
                       blocker=False)
                return
        h.nav(0, 2)
        h.wait(700)
        h.geiger_start()
        h.wait(1200)
        # POSITIVE CONTROL: the label must actually be asserting the Geiger first.
        if "Geiger" not in _status_task_text(h):
            record("F3", False, "cannot test: status bar never showed 'Geiger active', so its "
                                "disappearance later would prove nothing", blocker=True)
            return
        # Force-stop by a route that is NOT the Radiation Stop button.
        h.cfg_set("airplane", True)
        h.wait(1200)
        # Leave the Radiation page: the complaint is that it lies on every OTHER screen too.
        h.nav(0, 0)
        h.wait(800)
        left = _status_task_text(h)
        if left:
            record("F3", False, f"status bar still asserts {left!r} on STATS>Status after the "
                                f"Geiger was force-stopped by airplane mode")
        else:
            record("F3", True, "status-bar task slot cleared on a forced Geiger stop")
    finally:
        try:
            h.cfg_set("airplane", bool(saved_air))
        except Exception:
            pass


# ── F7 ───────────────────────────────────────────────────────────────────────
def test_f7_geiger_respects_airplane(h):
    """rad_toggle_cb had no cfg.airplane check, so the Geiger reached
    esp_wifi_start() + esp_wifi_set_promiscuous(true) with the Airplane switch reading engaged.
    Asserted against tool_state.promisc -- the driver's own view -- because that is the state
    that actually matters, not the UI's bookkeeping."""
    print("")
    print("-- F7: the Geiger must not enter promiscuous RX while airplane mode is on --")
    saved_air = (h.cfg_get("airplane") or {}).get("value")
    try:
        h.nav(0, 2)
        h.wait(700)
        h.cfg_set("airplane", True)
        if (h.cfg_get("airplane") or {}).get("value") is not True:
            record("F7", False, "cannot test: could not turn airplane mode on", blocker=False)
            return
        h.wait(800)
        h.geiger_start()          # goes through the SAME rad_geiger_start() the UI button uses
        h.wait(1500)
        st = h.tool_state() or {}
        promisc = st.get("promisc")
        geiger = bool(st.get("geiger_active"))
        print(f"  airplane=True -> geiger_active={geiger} promisc={promisc}")
        if promisc is None:
            record("F7", False, "cannot test: no 'promisc' field (stale build?)", blocker=False)
            return
        if promisc or geiger:
            record("F7", False,
                   f"Geiger started with airplane mode ON (geiger_active={geiger}, "
                   f"promisc={promisc}) -- the badge is in promiscuous RX while the Airplane "
                   f"switch reads engaged")
        else:
            record("F7", True, "Geiger refused to start under airplane mode; promisc stayed off")
    finally:
        try:
            h.geiger_stop()
        except Exception:
            pass
        try:
            h.cfg_set("airplane", bool(saved_air))
        except Exception:
            pass


# ── F1 ───────────────────────────────────────────────────────────────────────
def test_f1_inline_scan_mutex(h):
    """The inline "Scan" button on the AP/station pickers shares the radio with whatever tool
    is running. Tapping it silently stopped that tool and started scanning, while the tool's
    own "> STOP <" button (label written ONCE at build time) and its lv_chart kept asserting
    it still ran -- owner-confirmed: tapping "> STOP <" afterwards STARTS a scan.

    Asserts the button's `clickable` flag, which the `find` command reports directly."""
    print("")
    print("-- F1: inline Scan must be disabled while another tool owns the radio --")
    ap = h.cat_pos("Analyze")
    mon = h.cat_pos("Monitor")
    if ap is None or mon is None:
        record("F1", False, "cannot test: Analyze/Monitor category not found", blocker=False)
        return

    # Assert BEHAVIOUR, not appearance. `find` reports the nearest CLICKABLE ANCESTOR, so once
    # the button itself loses LV_OBJ_FLAG_CLICKABLE the walk continues up and reports a parent
    # as clickable=True -- a disabled button and a live one look identical through that lens.
    # Tapping and checking whether a scan actually started is the property we care about anyway.
    def tap_scan_and_see_if_a_scan_started():
        if not h.tap_text("=Scan", settle_ms=1500):
            return None                    # button not found/reachable
        h.wait(1200)
        name = (h.tool_state() or {}).get("name") or ""
        return "Scanning" in name

    # ⚠ REWRITTEN for the NARROWED rule. The first version of this test asserted the original
    # broad rule ("refuse while ANY other tool runs") and was never updated when the mutex was
    # narrowed to same-page-only after the owner found that the broad rule created a dead end:
    # a tool running on ANOTHER page left the inline Scan permanently dim with no way to stop
    # that tool from there. Cross-page stealing is now INTENDED and matches how tapping any
    # tool's own START behaves (a full cb_stop_operation teardown), so a test that flags it
    # would have failed a correct build. An adversarial review caught the mismatch.
    #
    # The narrowed rule is also easier to test: `tool_open` deliberately fires the START button
    # ("Fire the START button just like a tap", test_harness.h), so opening the RSSI page RUNS
    # RSSI -- which makes cb_op_encoded == this page's encoding, i.e. exactly the same-page case
    # the mutex is supposed to refuse. No second tool needed.
    try:
        h.tool_stop()
        h.wait(800)

        # SAME-PAGE case: tool_open builds the RSSI pane AND starts RSSI on it.
        h.cmd(f"tool_open {mon} 2")
        h.wait(2000)
        st_busy = h.tool_state() or {}
        if not st_busy.get("running"):
            record("F1", False, "cannot test: tool_open did not leave a tool running, so the "
                                "same-page case cannot be set up", blocker=True)
            return
        same_page_started = tap_scan_and_see_if_a_scan_started()
        if same_page_started is None:
            record("F1", False, "cannot test: no inline 'Scan' button on the RSSI page",
                   blocker=True)
            return

        # POSITIVE CONTROL: with nothing running, the SAME tap must start a scan. Without this,
        # "no scan started" above could just mean the tap never landed.
        h.tool_stop()
        h.wait(1200)
        idle_started = tap_scan_and_see_if_a_scan_started()
        print(f"  same-page tool running: scan started={same_page_started}   idle: scan started="
              f"{idle_started} (busy tool was {st_busy.get('name')!r})")
        if not idle_started:
            record("F1", False, "cannot test: tapping Scan with nothing running did not start a "
                                "scan, so the same-page reading proves nothing", blocker=True)
            return
        if same_page_started:
            record("F1", False,
                   "tapping inline Scan while THIS page's own tool is running STARTED a scan -- "
                   "it silently stopped that tool, leaving its '> STOP <' button and chart "
                   "asserting it still runs")
        else:
            record("F1", True, "inline Scan refuses while this page's own tool runs, and starts "
                               "a scan when idle")
    finally:
        try:
            h.tool_stop()
        except Exception:
            pass


# ── F8 ───────────────────────────────────────────────────────────────────────
def test_f8_collectible_audio_inhibits_screensaver(h):
    """The id-75 fullscreen collectible plays a looping bed; the screensaver fired straight
    through it, and screensaver_activate() stops the stream with nothing to re-arm it, so the
    music died permanently mid-song (owner-confirmed). The reveal is a "user is
    watching/listening" state exactly like a radio station and belongs in the inhibit list."""
    print("")
    print("-- F8: the collectible reveal's audio must inhibit the screensaver --")
    saved_off = (h.cfg_get("disp_off") or {}).get("value")
    try:
        h.cmd("coll add 75")
        h.wait(400)
        h.nav(1, 1)                       # ITEMS > Collectibles
        h.wait(1200)
        # SET THE FILTER TO FOUND-ONLY FIRST. This is why F8 was unrunnable by script for days:
        # collectible 75 sits deep in a 95-item list, so its row is not reachable by a tap. The
        # owner found the workaround on hardware -- switching the filter to "Found" shrinks the
        # list to the handful that are collected, which puts 75 near the top. The button shows
        # the CURRENT view ("All" or "Found"), so tapping it while it reads "All" enables the
        # filter. Assert the flip instead of assuming it: if the label does not change, the list
        # is still 95 long and the tap below would fail for a reason that has nothing to do with
        # what F8 tests.
        if h.find_exact("All"):
            h.tap_text("=All", settle_ms=1000)
        if not h.find_exact("Found"):
            record("F8", False,
                   "cannot test: could not switch the Collectibles filter to Found-only "
                   "(button still reads 'All'), so collectible 75 stays out of reach",
                   blocker=False, skipped=False)
            return
        # tap_text_scrolled, not tap_text: even filtered, the row can sit below the fold.
        if not h.tap_text_scrolled("SheetmetalCon", settle_ms=1200):
            record("F8", False, "cannot test: could not select collectible 75 "
                                f"({getattr(h, 'last_tap_error', None)})", blocker=False)
            return
        # Tap the detail card to go fullscreen (it is a plain lv_obj, not button-class).
        h.touch(240, 120, "tap")
        h.wait(2500)
        if not (h.state() or {}).get("coll_fs_audio"):
            record("F8", False, "cannot test: the fullscreen reveal's audio is not playing, so "
                                "an un-fired screensaver would prove nothing", blocker=False)
            return
        h.cfg_set("disp_off", 0)          # 15 s -- shortest timeout
        h.wait(500)
        fired = False
        for _ in range(26):
            time.sleep(1)
            if (h.state() or {}).get("screensaver"):
                fired = True
                break
        still_playing = bool((h.state() or {}).get("coll_fs_audio"))
        print(f"  screensaver_fired={fired} audio_still_playing={still_playing}")
        if fired:
            record("F8", False, "the screensaver fired over the fullscreen collectible and "
                                "killed its looping audio, which never comes back")
        else:
            record("F8", True, "screensaver inhibited while the collectible reveal is playing")
    finally:
        try:
            h.touch(160, 120, "tap")      # dismiss fullscreen
            h.wait(600)
            if saved_off is not None:
                h.cfg_set("disp_off", saved_off)
        except Exception:
            pass


# ── F4 ───────────────────────────────────────────────────────────────────────
def test_f4_joinwifi_does_not_deafen_geiger(h):
    """Join WiFi's CONNECT handler gates cb_stop_operation() on cb_op_running but kills the
    promiscuous RX callback UNCONDITIONALLY. The Geiger never sets cb_op_running, so its
    sniffer was torn out while rad_geiger_active stayed true: gauge pinned at 0, elapsed timer
    still counting, button still reading "Stop" -- a confident "the air is clean here".

    The fix stops the Geiger honestly via rad_geiger_force_stop(). So the assertion is NOT
    "the geiger survives" -- it is "the geiger is not left claiming to run while deaf".
    Uses tool_state.geiger_active, which is rad_geiger_active itself, so this measures the
    exact flag the UI renders from rather than a proxy for it.
    """
    print("")
    print("-- F4: Join WiFi must not leave a deafened Geiger claiming to run --")
    np_ = h.cat_pos("Network")
    if np_ is None:
        record("F4", False, "cannot test: Network category not found", blocker=False)
        return
    try:
        # POSITIVE CONTROL: the Geiger must actually come up first, else "not running" after
        # the join proves nothing at all.
        h.nav(0, 2)
        h.wait(700)
        h.geiger_start()
        h.wait(1500)
        st = h.tool_state() or {}
        if not st.get("geiger_active"):
            record("F4", False, "cannot test: the Geiger did not start", blocker=True)
            return

        # Open Join WiFi and fire its CONNECT button the way a user does. We do not need the
        # join to SUCCEED -- the defect is in the teardown that runs before joinWiFi is called.
        h.cmd(f"tool_open {np_} 0")
        h.wait(1200)

        # PRECONDITION: an SSID must be set, or the CONNECT handler hits
        #     if (!wifi_join_ssid[0]) { ...("Enter an SSID first"); return; }
        # and returns BEFORE any teardown -- so the Geiger is never touched and this test
        # reads "still running" for entirely the wrong reason. My first RED here was exactly
        # that false positive: right observable, wrong cause.
        def reveal_and_tap(label):
            for _ in range(6):
                hits = h.find(label) or []
                if hits and hits[0].get("onscreen"):
                    h.touch(hits[0]["hit_x"], hits[0]["hit_y"], "tap")
                    h.wait(900)
                    return True
                h.swipe(230, 190, 230, 90)      # drag the right pane up
                h.wait(500)
            return False

        if not reveal_and_tap("Edit"):
            record("F4", False, "cannot test: could not reach the SSID Edit button",
                   blocker=False)
            return
        h.kb_type("f4target")
        h.wait(300)
        h.kb_ok()
        h.wait(900)

        # The Join WiFi page is taller than the content pane, so CONNECT starts BELOW the fold
        # (reported at y=518). Scroll it into view -- my own `onscreen` guard refuses to tap it
        # otherwise, which is the whole point of that guard: tapping those coordinates blind
        # would land on the division bar and navigate instead.
        if not reveal_and_tap("CONNECT"):
            record("F4", False, "cannot test: could not bring CONNECT on-screen "
                                f"({getattr(h, 'last_tap_error', None)})", blocker=False)
            return
        h.wait(2500)

        # CONTROL: prove the handler actually PROCEEDED past its SSID guard. If the status text
        # still says "Enter an SSID first", the teardown never ran and any geiger reading below
        # is meaningless.
        labels = _labels(h)
        if any("Enter an SSID" in t for t in labels):
            record("F4", False, "cannot test: CONNECT refused with 'Enter an SSID first' -- the "
                                "handler returned before its teardown, so the geiger state "
                                "proves nothing", blocker=False)
            return

        st2 = h.tool_state() or {}
        still_claims = bool(st2.get("geiger_active"))
        print(f"  geiger_active: before join=True -> after CONNECT={still_claims}")
        if still_claims:
            record("F4", False,
                   "the Geiger still claims to be running after Join WiFi tore out its "
                   "promiscuous RX callback -- the gauge will read 0/s forever while the "
                   "button says Stop and the timer keeps counting")
        else:
            record("F4", True, "Join WiFi stopped the Geiger honestly instead of deafening it")
    finally:
        try:
            h.geiger_stop()
        except Exception:
            pass


# ── F7b ──────────────────────────────────────────────────────────────────────
def test_f7b_tool_start_respects_airplane(h):
    """The tool-side airplane gate had ZERO automated coverage, because tool_start and
    tool_open dispatched directly instead of asking tool_tap_cb's question. So a scripted
    `cfg_set airplane true` + `tool_start` would start a radio tool and reach esp_wifi_start()
    / esp_wifi_set_promiscuous(true) with the Airplane switch reading engaged -- F7 one layer
    up. Now both route through tool_launch_allowed() and return a distinct error.

    Asserts the DRIVER's own view (tool_state.promisc) as well as the refusal, so this cannot
    pass on a badge that merely declined to update its UI."""
    print("")
    print("-- F7b: a scripted tool start must respect airplane mode --")
    sp = h.cat_pos("Scan")
    if sp is None:
        record("F7b", False, "cannot test: Scan category not found", blocker=False)
        return
    saved_air = (h.cfg_get("airplane") or {}).get("value")
    try:
        # POSITIVE CONTROL: with airplane OFF the same call must SUCCEED, else "blocked" below
        # proves nothing (the tool might be broken for an unrelated reason).
        h.cfg_set("airplane", False)
        h.wait(600)
        h.tool_start(sp, 0)
        h.wait(2500)
        st_on = h.tool_state() or {}
        if not st_on.get("running"):
            record("F7b", False, "cannot test: the tool would not start even with airplane OFF",
                   blocker=True)
            return
        h.tool_stop()
        h.wait(1200)

        # Now with airplane ON the same call must be refused.
        h.cfg_set("airplane", True)
        if (h.cfg_get("airplane") or {}).get("value") is not True:
            record("F7b", False, "cannot test: could not turn airplane on", blocker=False)
            return
        h.wait(800)
        resp = h.tool_start(sp, 0) or {}
        h.wait(2000)
        st = h.tool_state() or {}
        blocked = (resp.get("ok") is False) and ("airplane" in str(resp.get("error", "")).lower())
        print(f"  airplane ON -> tool_start ok={resp.get('ok')} err={resp.get('error')!r} "
              f"running={st.get('running')} promisc={st.get('promisc')}")
        if st.get("running") or st.get("promisc"):
            record("F7b", False,
                   f"a scripted tool start under airplane mode RAN (running={st.get('running')}, "
                   f"promisc={st.get('promisc')}) -- the badge is on the air with the Airplane "
                   f"switch engaged")
        elif not blocked:
            record("F7b", False, f"tool did not run, but tool_start did not report the airplane "
                                 f"refusal either (resp={resp!r}) -- a test cannot distinguish "
                                 f"'blocked' from 'failed for another reason'")
        else:
            record("F7b", True, "scripted tool start refused under airplane mode; promisc off")
    finally:
        try:
            h.tool_stop()
        except Exception:
            pass
        try:
            h.cfg_set("airplane", bool(saved_air))
        except Exception:
            pass


# ── FB10 ─────────────────────────────────────────────────────────────────────
def test_fb10_bt_list_dedup(h):
    """bt_devices->add() had no match check at any of its four sites, and NimBLE re-reports the
    same address for the whole scan -- so a BLE scan grew the list without bound inside one run.

    The assertion is ENVIRONMENT-INDEPENDENT, which is what makes it usable on any bench:
    NimBLE tracks at most 50 distinct addresses per scan run, so bt > 50 is by itself proof of
    duplicate accumulation. It does not depend on how many real devices are in the room -- only
    that at least a few are, which the positive control checks."""
    print("")
    print("-- FB10: the BT device list must dedup by address --")
    sp = h.cat_pos("Scan")
    if sp is None:
        record("FB10", False, "cannot test: Scan category not found", blocker=False)
        return
    try:
        h.tool_stop()
        h.wait(800)
        h.tool_start(sp, 3)               # Scan > BT Devices
        h.wait(2000)
        if not (h.tool_state() or {}).get("running"):
            record("FB10", False, "cannot test: the BT scan would not start", blocker=True)
            return
        # Let it run long enough that a non-deduping list would blow past 50.
        for _ in range(6):
            time.sleep(5)
        bt = (h.cmd("detect_counts") or {}).get("bt")
        print(f"  bt device count after ~30 s of scanning: {bt}")
        if bt is None:
            record("FB10", False, "cannot test: detect_counts has no 'bt' field", blocker=False)
            return
        # POSITIVE CONTROL: if the room is empty the count proves nothing either way.
        if bt == 0:
            record("FB10", False, "cannot test: no BT devices seen at all, so a low count "
                                  "cannot distinguish dedup from an empty room", blocker=False)
            return
        if bt > 50:
            record("FB10", False,
                   f"bt_devices holds {bt} entries -- NimBLE reports at most 50 distinct "
                   f"addresses per run, so anything above that is the same devices appended "
                   f"repeatedly (unbounded heap growth inside one scan)")
        else:
            record("FB10", True, f"bt count {bt} <= 50 distinct-address ceiling; dedup holding")
    finally:
        try:
            h.tool_stop()
        except Exception:
            pass


# ── R1 (Res34rch only) ───────────────────────────────────────────────────────
def test_r1_manual_scan_kind(h):
    """dispatch_clipboy_action() clears cb_manual_scan_kind unconditionally, but
    cb_tool_execute_cb never sets cb_op_encoded -- so an EXECUTE tap mid-manual-station-scan
    flips the status bar to a frozen leftover AP count."""
    print("\n-- R1: EXECUTE mid-manual-station-scan flips the status bar (Res34rch) --")
    # cat_pos now RAISES when it cannot read a complete tool_list, so `None` here means the
    # category is genuinely compiled out rather than "the response was short". Before that fix
    # this branch fired on a res34rch badge and reported a skip as a pass.
    if h.cat_pos("Deauth") is None:
        record("R1", True, "Sn34k build has no TAT_STA tools -- nothing to test",
               blocker=False, skipped=True)
        return

    MON_STA = 1   # enum MonType { MON_AP, MON_STA, MON_BT } -- ui_nav.h:1327

    # PRECONDITION 1 -- a MATCHED AP. Stations are only associated with a SELECTED AP, so
    # without one the picker's inline Scan has nothing to enumerate and the run that this
    # test needs never happens. The previous version ran a plain AP scan here and never
    # selected anything, which is why it could only ever report "cannot test".
    target = os.environ.get("R1_AP_SSID", "shipship")
    sel = h.ap_scan(target) or {}          # blocking band sweep, ~16 s; selects by SSID
    # DISTINGUISH A FAILED COMMAND FROM A SUBSTANTIVE ZERO. The first version of this check read
    # `count` straight out of the response, so an ap_scan that TIMED OUT (it blocks the badge
    # ~15-22 s vs the bridge's old 10 s deadline) produced `count: 0` and was reported as "no AP
    # in range" -- a rig failure dressed as an RF observation, which is the same defect this
    # suite keeps finding in the firmware. Check `ok` before believing any number in here.
    if not sel.get("ok", False):
        record("R1", False,
               f"cannot test: ap_scan did not complete ({sel.get('error', 'no response')}) -- "
               f"this is a HARNESS failure, not an absence of APs. ap_scan blocks the badge "
               f"~15-22 s; raise CLIPBOY_SESSION_TIMEOUT.", blocker=False)
        return
    if sel.get("selected", -1) < 0:
        record("R1", False,
               f"cannot test: ap_scan completed and saw {sel.get('count', 0)} AP(s), none named "
               f"'{target}'. Set R1_AP_SSID=<ssid> to an AP that is on the air here.",
               blocker=False)
        return

    dp = h.cat_pos("Deauth")
    # Deauth items are { 0: "! Discovered" TAT_AP, 1: "! Manual" TAT_AP, 2: "! Stations" TAT_STA }
    # (ui_nav.h cat_deauth[]). This used to open item 0 while its comment claimed "! Stations",
    # so R1 has NEVER been on the station picker: it landed on an AP-targeted tool, tapped that
    # page's own inline Scan, and then measured a status bar that no station scan had bound.
    # The index is verified by a precondition below rather than trusted, because an index is
    # exactly the kind of thing a category reorg rots silently.
    h.cmd(f"tool_open {dp} 2")
    h.wait(700)
    page = _labels(h)
    if not any("Stations" in t for t in page):
        record("R1", False,
               f"cannot test: Deauth[2] is not the Stations picker -- on-screen labels were "
               f"{_a(page[:14])}. Re-check cat_deauth[] in ui_nav.h.", blocker=False)
        return
    # STOP the page's own tool before using the inline Scan. `tool_open` FIRES the START button
    # (test_harness.h:940), so the Stations tool is RUNNING as soon as the page opens -- and F1's
    # mutex then correctly refuses this page's inline Scan, because that is precisely the
    # "another operation owns the radio" case it was added for. R1 is about the MANUAL station
    # scan (no tool behind it, cb_op_encoded == -1), which is what a user gets when they sit on
    # the picker with nothing running and tap Scan. Without this stop, the tap is refused, no
    # kind is bound, and the test can only ever report cannot-test.
    h.tool_stop()
    h.wait(700)
    # EXACT match. `tap_text("Scan")` is a substring search and the LEFT PANE carries the tool
    # category row "> Scan", so the first hit can be a navigation row rather than this page's
    # inline button -- tapping it leaves the Stations page entirely while still returning True.
    # Same trap as `find L` matching "FL" (documented in the harness notes).
    if not h.tap_text("=Scan", settle_ms=1500):
        record("R1", False,
               f"could not tap the station picker's inline Scan button "
               f"({getattr(h, 'last_tap_error', None)})", blocker=False)
        return

    # Read the STATUS-BAR TASK SLOT ITSELF (tool_state.stask = lbl_stask's text) plus
    # cb_manual_scan_kind. The old helper took "the first text on screen containing 'APs'
    # or 'STAs'" and matched the tool-list ROW "APs (full)" -- a left-pane menu label that
    # never changes -- so before and after were the same string for reasons that had nothing
    # to do with the fix, and the test scored a pass. Never infer a widget's text from a
    # screen-wide search when the widget can be read directly.
    def bar(h):
        st = h.tool_state() or {}
        return st.get("stask", ""), st.get("manual_kind", -1)

    before, kind_before = bar(h)
    # PRECONDITION 2 / POSITIVE CONTROL: the manual station scan must actually be running
    # AND bound, or "unchanged" proves nothing.
    if kind_before != MON_STA or "STAs" not in before:
        record("R1", False,
               f"cannot test: manual station scan is not the bound owner before the EXECUTE "
               f"tap (stask='{before}', manual_kind={kind_before}, want {MON_STA}) -- any "
               f"'unchanged' reading would be vacuous", blocker=False)
        return

    np_ = h.cat_pos("Network")
    h.cmd(f"tool_open {np_} 1")          # Rnd STA MAC (TAT_IMMEDIATE)
    h.wait(500)
    h.tap_text("EXECUTE", settle_ms=1200)
    after, kind_after = bar(h)
    try:
        h.tool_stop()
    except Exception:
        pass
    if kind_after == MON_STA and "STAs" in after:
        record("R1", True,
               f"status bar stayed station-scoped across the EXECUTE tap "
               f"('{before}' -> '{after}', manual_kind {kind_before} -> {kind_after})")
    else:
        record("R1", False,
               f"status bar flipped mid-scan: '{before}' -> '{after}' "
               f"(manual_kind {kind_before} -> {kind_after})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None)
    ap.add_argument("--only", default=None, help="run a single test by prefix (sb1/sb2/sb3/fb7/r1)")
    a = ap.parse_args()

    print("=" * 68)
    print("TEST: audit teardown/lifetime regressions (must FAIL pre-fix)")
    print("=" * 68)
    # R1 needs `ap_scan`, which drives the channel hop itself and BLOCKS the badge's main loop
    # for ~15-22 s. The bridge's default read deadline is 10 s, so the response never arrives in
    # time and the command returns a timeout -- which R1 then read as `count: 0` and reported as
    # "no AP in range". Raise the deadline BEFORE the Harness is built (the bridge reads this
    # env var once, at session start). Per the bridge's own note, a larger deadline only bounds
    # the MAX wait, so fast commands are unaffected.
    os.environ.setdefault("CLIPBOY_SESSION_TIMEOUT", "30")
    print(f"suite: bridge read deadline {os.environ['CLIPBOY_SESSION_TIMEOUT']}s "
          f"(ap_scan blocks the badge ~15-22s)")
    h = Harness(port=a.port)
    global CURRENT_H
    CURRENT_H = h                  # enables record()'s auto-capture on failures/skips
    # ── PIN THE DISPLAY TIMEOUT TO "Never" FOR THE WHOLE SUITE ────────────────────────────
    # ROOT CAUSE of SB1's long-standing ~40%-in-suite / 0%-isolated flake, found 2026-07-26 from
    # its own diagnostics: `screensaver=True` at the moment of the tap, uptime 100 s. The shipping
    # default is disp_off=2 (60 s), and **serial commands do not count as user activity** -- only
    # a touch resets the idle timer. SB1's self-cleaning loop drives the UI entirely through
    # `tool_open`, so the badge sits "idle" for the whole setup, the screensaver arms, and the
    # tap on "+ Add Network" is swallowed by the unlock overlay. The keyboard never opens and the
    # test blames the keyboard. Any test whose scripted phase outlives 60 s has the same exposure,
    # which is why this is pinned suite-wide rather than patched into SB1.
    # ⚠ Two tests NEED the screensaver (FB7's hold-bar, F8's audio inhibit). They already set
    # disp_off themselves and restore what they read, so inside this window they read 5, set 0,
    # and put 5 back -- correct without change. Do NOT "simplify" this by disabling the
    # screensaver inside the Harness: that would silently defeat exactly those two tests.
    # Restored in the finally below, including on KeyboardInterrupt, so the badge is not left
    # with the screensaver permanently off.
    ss_saved = None
    try:
        ss_saved = (h.cmd("cfg_get disp_off") or {}).get("value")
        h.cmd("cfg_set disp_off 5")            # 5 = Never
        print(f"suite: display timeout pinned to Never for the run (was {ss_saved})")
    except Exception as e:                      # noqa: BLE001
        print(f"suite: WARNING could not pin the display timeout ({e}) -- "
              f"touch tests may be swallowed by the screensaver")
    try:
        tests = [("sb1", test_sb1_nested_keyboard), ("sb2", test_sb2_help_teardown),
                 ("sb2b", test_sb2b_theme_switch_keeps_geiger_live),
                 ("sb3", test_sb3_statusbar_kills_theremin), ("fb7", test_fb7_screensaver_bar),
                 ("fb8", test_fb8_battery_not_zero_at_boot),
                 ("fb11", test_fb11_geiger_audio_latch),
                 ("fb5", test_fb5_joinwifi_stop_really_stops),
                 ("fb1", test_fb1_promiscuous_torn_down),
                 ("fb1b", test_fb1b_capture_reinit),
                 ("f3", test_f3_geiger_status_label_cleared),
                 ("f7", test_f7_geiger_respects_airplane),
                 ("f1", test_f1_inline_scan_mutex),
                 ("f8", test_f8_collectible_audio_inhibits_screensaver),
                 ("f4", test_f4_joinwifi_does_not_deafen_geiger),
                 ("f7b", test_f7b_tool_start_respects_airplane),
                 ("fb10", test_fb10_bt_list_dedup),
                 ("r1", test_r1_manual_scan_kind)]
        for key, fn in tests:
            if a.only and not key.startswith(a.only.lower()):
                continue
            try:
                fn(h)
            except Exception as e:
                record(key.upper(), False, f"test raised: {type(e).__name__}: {e}")
            # Reboot between tests UNCONDITIONALLY. `alive()` is not sufficient: SB1 leaves
            # the badge answering serial normally while its UI is wedged behind an orphaned
            # full-screen modal, so a liveness check passes and the next test would run
            # against a dead UI and report a bogus failure.
            print("    recovering (reboot) before the next test")
            recover(h)
            # NO per-test re-assert of disp_off here. `cfg_set disp_off` persists to NVS, so the
            # suite-wide pin already survives every inter-test reboot -- a re-assert was pure
            # defence in depth, and it fired a serial write in the single most fragile moment
            # there is: immediately after a hard reboot, while Windows is tearing down and
            # re-creating the CDC device node. "A redundant guard firing at the wrong moment is
            # indistinguishable from the original defect" is a rule in CLAUDE.md, and this is
            # what it looks like. If FB7 or F8 ever fail to restore, fix it in THEIR finally.
    finally:
        # Put the user's setting back. A suite that leaves the screensaver permanently off has
        # changed the device it was measuring.
        try:
            if ss_saved is not None:
                h.cmd(f"cfg_set disp_off {int(ss_saved)}")
                print(f"suite: display timeout restored to {ss_saved}")
        except Exception as e:                   # noqa: BLE001
            print(f"suite: WARNING could not restore the display timeout ({e}) -- "
                  f"set DATA > Settings > Screensaver by hand")
        try:
            h.close()
        except Exception:
            pass

    print("\n" + "=" * 68)
    hard = [r for r in RESULTS if r["blocker"]]
    passed = sum(1 for r in hard if r["passed"])
    skips = [r for r in RESULTS if r.get("skipped")]
    # Every non-blocking non-pass is a case that DID NOT RUN. They are named below, but they were
    # missing from the headline, so "17/17 blocking checks pass" read as full coverage on a run
    # where cases had quietly degraded to cannot-test. A headline that does not account for every
    # case is where un-run work hides, so make the counts add up.
    cannot = [r for r in RESULTS if not r["blocker"] and not r["passed"] and not r.get("skipped")]
    print(f"RESULTS: {passed}/{len(hard)} blocking checks pass"
          + (f"  ({len(skips)} skipped -- NOT tested)" if skips else "")
          + (f"  ({len(cannot)} could-not-test -- NOT tested)" if cannot else "")
          + f"  [{len(RESULTS)} cases recorded]")
    # List skips explicitly. "17/17" next to a silent skip invites the reader to believe the
    # skipped case was covered; naming them keeps the headline honest.
    for r in skips:
        print(_a(f"  SKIP: {r['name']} -- {r['detail']}"))
    for r in RESULTS:
        if not r["passed"] and not r.get("skipped"):
            print(_a(f"  {'FAIL' if r['blocker'] else 'warn'}: {r['name']} -- {r['detail']}"))
    print("=" * 68)
    return 0 if passed == len(hard) else 1


if __name__ == "__main__":
    sys.exit(main())
