#!/usr/bin/env py -3
"""test_flock_liveness.py -- prove Detect > Flock Batteries can still show it is HEARING a device.

(Renamed from "Flock Safety" 2026-07-27: the matcher detects the BATTERY PACK accessory, not the
camera. The old wording here promised the camera -- see memory flock_battery_reality.)

WHY THIS EXISTS
Dedup (56f5184a) fixed unbounded heap growth in `flock_devices` but cost the tool its only live
feedback: the list stops growing on a re-sighting, so a plain "1 Flock" sat frozen and looked
exactly like a hung scan -- in the one tool where "is it still there?" IS the question.

The remedy is an AGE ("1 Flock, 4s ago"), not an advert count. That choice was made on
measurement, not taste: the pre-fix control logged 13 adverts in 45 s (~0.29/s) and
WiFiScan::main() cycles the BLE scan off/on about every second, so a counter would itself have
sat frozen for several refreshes at a time -- reproducing the symptom it was meant to cure.

WHY AN AGE IS TESTABLE AND A COUNTER IS NOT
A monotonic counter can only ever read "went up". "Stopped rising" and "never rose" look the
same, so a test asserting `count > 0` passes for the wrong reasons. An age reads BOTH ways in a
single run, which is what this test exploits:

  CONTROL  (emitter OFF): the age must RISE. That proves the readout is alive and that this
           observable is CAPABLE of reporting the bad state. Run it FIRST -- if the age never
           moves, everything after it is vacuous and this test says so.
  STIMULUS (emitter ON) : the age must DROP to ~0. That proves the stimulus actually landed.

Both directions, same run, same build. Neither alone would be worth much.

  CLIPBOY_DUT=COM4 py -3 scripts/tests/test_flock_liveness.py
  (kalipi must be reachable; the emitter is kalipi_stim `ble-raw 04ffc80900`)

⚠ detect_emitters.py honours only CLIPBOY_DUT (default COM11) -- NOT --port/CLIPBOY_PORT. With
two badges attached, an unset CLIPBOY_DUT silently measures the wrong device.
"""
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from harness import Harness            # noqa: E402
import tool_suite as TS                # noqa: E402  (same kalipi transport detect_emitters uses)

AGE_RE = re.compile(r"\d+\s*Flock,\s*\d+[sm] ago")
FAILED, PASSED, CANNOT = [], [], []


def record(name, ok, msg):
    (PASSED if ok else FAILED).append(name)
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}: {msg}")


def cannot(name, why):
    CANNOT.append(name)
    print(f"  [warn] {name}: cannot test: {why}")


def status_age(h):
    """(flock_count, age_seconds) from detect_counts, or (None, None) if nothing heard yet.

    ⚠ Reads the DATA, not the status-bar string, and that is a deliberate limitation to state
    rather than paper over. `lbl_stask` keeps whatever `th_cmd_tool_start` wrote into it, because
    nothing in the harness calls `cb_start_scan_polling()` -- so `cb_scan_timer` is never created
    and `cb_scan_poll_cb` never runs to overwrite the label. (NOT "cb_op_running stays false":
    the harness DOES set that flag. The corrected mechanism means the fix is one line in
    test_harness.h, not a refactor.) Measured: with the
    Flock tool running and flock=1, the bar read "Flock Safety".

    ⚠ BUT THE RENDERED STRING IS NOT UNREACHABLE. An earlier version of this docstring said it
    was, and that only a human tapping START could confirm it -- and then repeated the claim
    three lines below its own retraction. Both wrong. `ui_theme_switch_live()` ends with
    `if (cb_op_running && cb.isScanning()) cb_start_scan_polling();` and `theme_set` calls it,
    so switching theme while the tool runs creates the poller. Proven on hardware: the bar read
    `['Flock Safety']` before and `['1 Flock, 5s ago']` after, then `8s ago`. test_rendered()
    below keeps that before/after pair as its own control.
    """
    dc = h.cmd("detect_counts") or {}
    age_ms = dc.get("flock_age")
    if age_ms is None or age_ms < 0:
        return None, None
    return dc.get("flock"), int(age_ms) // 1000


def kal(secs):
    """Emit the Flock (Xuntong) BLE signature for `secs`. Returns the kalipi reply, or None.

    ⚠ Goes through tool_suite.kalipi() -- the SAME transport detect_emitters.py uses -- rather
    than a hand-rolled `ssh … /tmp/kalipi_stim.py`. My first version did the latter, assumed the
    script was already deployed, AND DISCARDED THE RETURN VALUE. The emitter therefore never
    fired and the test reported "no age shown", which reads exactly like a broken age display.
    The test was right to say cannot-test; the STIMULUS was what was broken. Check the response
    of every command you issue.
    """
    return TS.kalipi("ble-raw", "04ffc80900", str(secs), timeout=secs + 15)


def main():
    print("=" * 68)
    print("TEST: Flock liveness -- the age must read BOTH ways")
    print("=" * 68)
    h = Harness(port=os.environ.get("CLIPBOY_DUT"))
    try:
        h.tool_stop()
        h.wait(600)
        sp = h.cat_pos("Detect")
        if sp is None:
            cannot("FLOCK-AGE", "Detect category not found")
            return summarise()
        # CONTROL for FLOCK-RENDERED: before the tool runs, the bar must NOT show an age.
        pre_bar = [t for t in ((h.cmd("text") or {}).get("texts") or [])
                   if isinstance(t, str) and AGE_RE.search(t)]

        # ⚠ tool_open, NOT tool_start. tool_open navigates to ITEMS > Tools, calls
        # show_tool_detail() (the real row-tap path) and then FIRES THE REAL START BUTTON found
        # by walking the object tree -- so it builds the output and scan pollers exactly like a
        # user tap. tool_start dispatches the action directly and never touches the button, so
        # cb_start_scan_polling() is never reached and lbl_stask keeps the tool name.
        # Measured side by side on COM4: tool_start -> bar ['Flock Safety'];
        # tool_open -> bar ['1 Flock, 4s ago'] plus a live log pane.
        # (This also means the project note "scripted tool starts exercise NONE of the
        # output/scan pollers" is true of tool_start only -- tool_open does exercise them.)
        r = h.cmd(f"tool_open {sp} 3") or {}
        h.wait(2500)
        if not r.get("ok") or not r.get("running"):
            cannot("FLOCK-AGE", f"the Flock tool would not start via tool_open (btn={r.get('btn')!r} "
                                f"running={r.get('running')!r})")
            return summarise()

        # ── STIMULUS FIRST, only to get one device into the list ────────────────────────
        # The age cannot exist until something has been heard once.
        try:
            TS.deploy()
        except Exception as e:                             # noqa: BLE001
            cannot("FLOCK-AGE", f"could not deploy kalipi_stim: {e}")
            return summarise()
        if not kal(12):
            cannot("FLOCK-AGE", "the kalipi emitter did not run -- no stimulus was applied, so "
                                "an absent age would prove nothing about the badge")
            return summarise()
        time.sleep(3)
        cnt, age = status_age(h)
        if cnt is None:
            cannot("FLOCK-AGE",
                   "detect_counts reports no flock_age -- either nothing was detected (is the "
                   "kalipi emitter reaching the badge?) or this build predates the flock_age field")
            return summarise()
        record("FLOCK-SEEN", True, f"badge reports an age: {cnt} Flock, {age}s since last heard")

        # ── CONTROL: emitter OFF, the age MUST RISE ─────────────────────────────────────
        # This is the assertion that gives the next one meaning. If the age cannot climb, the
        # readout is frozen and a low reading later would prove nothing at all.
        c0, a0 = status_age(h)
        time.sleep(14)
        c1, a1 = status_age(h)
        if a1 is None or a0 is None:
            cannot("FLOCK-AGE-RISES", "status bar stopped reporting an age mid-run")
            return summarise()
        if a1 > a0:
            record("FLOCK-AGE-RISES", True,
                   f"with no emitter the age climbed {a0}s -> {a1}s, so the readout is alive "
                   f"and CAN report the bad state")
        else:
            record("FLOCK-AGE-RISES", False,
                   f"age did not climb with the emitter off ({a0}s -> {a1}s) -- the display is "
                   f"frozen, so it cannot distinguish a present camera from a dead scan")
            return summarise()

        # ── STIMULUS: emitter ON, the age MUST DROP ─────────────────────────────────────
        if not kal(20):
            cannot("FLOCK-AGE-RESETS", "emitter failed on the second burst")
            return summarise()
        time.sleep(6)
        c2, a2 = status_age(h)
        if a2 is None:
            cannot("FLOCK-AGE-RESETS", "status bar stopped reporting an age")
            return summarise()
        if a2 < a1:
            record("FLOCK-AGE-RESETS", True,
                   f"the emitter pulled the age back down {a1}s -> {a2}s, so a re-sighting "
                   f"refreshes last_seen through the dedup path")
        else:
            record("FLOCK-AGE-RESETS", False,
                   f"age did not fall while emitting ({a1}s -> {a2}s) -- the dedup hit is not "
                   f"refreshing last_seen, so the tool still cannot show it is hearing anything")

        # ── the RENDERED string ─────────────────────────────────────────────────────────
        # No theme hack needed: tool_open already started the poller that owns lbl_stask. The
        # control is `pre_bar`, captured before the tool ran -- if an age was already on screen
        # then, this run cannot attribute the render to this tool.
        shown = [t for t in ((h.cmd("text") or {}).get("texts") or [])
                 if isinstance(t, str) and AGE_RE.search(t)]
        if pre_bar:
            cannot("FLOCK-RENDERED",
                   f"an age was already on the status bar before the tool started ({pre_bar!r}), "
                   f"so this run cannot attribute the render to Flock")
        elif shown:
            record("FLOCK-RENDERED", True,
                   f"status bar renders the age: {shown[0]!r} (absent before start) -- the string "
                   f"itself is verified, not just the underlying field")
        else:
            record("FLOCK-RENDERED", False,
                   "the age is in detect_counts but is NOT reaching lbl_stask")

        # ── dedup must still hold: the age moved, the LIST did not ──────────────────────
        n = (h.cmd("detect_counts") or {}).get("flock")
        if n is None:
            cannot("FLOCK-DEDUP-INTACT", "detect_counts has no 'flock' field")
        elif n == 1:
            record("FLOCK-DEDUP-INTACT", True,
                   "flock count is still 1 for one emitter -- the age moved, the list did not")
        else:
            record("FLOCK-DEDUP-INTACT", False,
                   f"flock count is {n} for a single emitter address; expected 1. The liveness "
                   f"change may have reverted the dedup fix (unbounded append)")
        return summarise()
    finally:
        try:
            h.tool_stop()
        except Exception:                                  # noqa: BLE001
            pass


def summarise():
    print("=" * 68)
    for n in CANNOT:
        print(f"  warn: {n} -- NOT RUN; this is not a pass")
    print(f"passed={len(PASSED)} failed={len(FAILED)} not-run={len(CANNOT)}")
    print("=" * 68)
    return 1 if (FAILED or not PASSED) else 0


if __name__ == "__main__":
    sys.exit(main())
