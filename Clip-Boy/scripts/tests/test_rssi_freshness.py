#!/usr/bin/env py -3
"""test_rssi_freshness.py -- Monitor > RSSI must SAY when the target stopped transmitting.

Owner-specified (2026-07-26). The defect this guards: `ap.rssi` is written only when a frame from
that BSSID arrives and nothing ages it, so a flat trace meant EITHER a steady signal OR a dead
target, and nothing on screen distinguished them. Proven on the rig: the readout sat at -44 dBm,
pixel-identical, 25 s after the beacon was killed and the injector interface downed.

The fix watches the selected AP's `packets` counter (the only available evidence a frame arrived --
AccessPoint has no last-seen field), HALTS the trace after CB_RSSI_STALL_MS of silence, and shows
"no packets for Ns" above the graph. This test drives the full cycle the owner asked for:

    beacon up -> trace moves -> beacon KILLED -> counter climbs to ~10 s AND the trace freezes
    -> beacon back up (SAME SSID + BSSID) -> counter resets AND the trace resumes

...twice, because a one-shot pass does not show the recovery path is repeatable (the owner's
"bonus points for a 2nd round"). `rogueap` conveniently hardcodes BSSID 00:13:37:aa:bb:cc and
SSID "Pineapple", so a restart really is the same AP rather than a look-alike.

WHY IT ASSERTS ON `rssi_appends`: reading the on-screen label would only prove the LABEL changed.
`rssi_appends` is a monotonic count of points actually appended to the trace, so "the graph
halted" is measured directly instead of inferred. Not advancing while stalled, and advancing
again after recovery, is the whole claim.

    CLIPBOY_PORT=COM4 py -3 Clip-Boy/scripts/tests/test_rssi_freshness.py
    py -3 Clip-Boy/scripts/tests/test_rssi_freshness.py --port COM5 --rounds 2
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Harness
import test_signal_loss as sl          # reuse the kalipi plumbing (kali, kali_beacon_fanout, ...)

TARGET_SSID = "Pineapple"
TARGET_CHAN = "6"
QUIET_TARGET_MS = 10000               # owner's "watch the timer tick up to ~10s"
STALL_MS = 2000                       # must match CB_RSSI_STALL_MS in ui_nav.h
RESULTS = []


def record(name, ok, detail):
    RESULTS.append({"name": name, "ok": ok, "detail": detail})
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}: {detail}")


def cannot(name, detail):
    RESULTS.append({"name": name, "ok": None, "detail": detail})
    print(f"  [CANNOT-TEST] {name}: {detail}")


def rs(h):
    """(quiet_ms, appends) from tool_state -- the two numbers this test is about."""
    t = h.tool_state() or {}
    return int(t.get("rssi_quiet_ms", -1)), int(t.get("rssi_appends", -1))


def wait_for(fn, timeout_s, poll_s=1.0):
    """Poll until fn() is truthy; return its value or None on timeout."""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        v = fn()
        if v:
            return v
        time.sleep(poll_s)
    return None


def one_round(h, rnd):
    tag = f"round{rnd}"
    print(f"\n-- RSSI freshness, round {rnd} --")

    # ── target ALIVE ────────────────────────────────────────────────────────────────────
    mu = sl.kali("mon-up", "wlan1", TARGET_CHAN)
    if not mu.get("ok"):
        cannot(tag, f"kalipi monitor mode did not come up: {mu}")
        return
    bg = sl.kali_beacon_fanout(300, TARGET_CHAN)
    try:
        time.sleep(3)
        sel = h.ap_scan(TARGET_SSID) or {}
        if not sel.get("ok"):
            cannot(tag, f"ap_scan did not complete ({sel.get('error')}) -- harness, not RF")
            return
        if sel.get("selected", -1) < 0:
            cannot(tag, f"'{TARGET_SSID}' not selectable (saw {sel.get('count', 0)} APs) -- the "
                        f"beacon may not be reaching the badge")
            return
        mon = h.cat_pos("Monitor")
        if mon is None:
            cannot(tag, "Monitor category not found")
            return
        h.cmd(f"tool_open {mon} 2")            # Monitor > RSSI
        time.sleep(4)

        # POSITIVE CONTROL: the trace must actually be moving before "it stopped" means anything.
        q0, a0 = rs(h)
        if a0 < 0:
            cannot(tag, "tool_state has no rssi_appends -- firmware predates this test")
            return
        time.sleep(4)
        q1, a1 = rs(h)
        if a1 <= a0:
            cannot(tag, f"the trace is not advancing while the target IS transmitting "
                        f"(appends {a0} -> {a1}, quiet {q1} ms) -- nothing below would be "
                        f"meaningful")
            return
        record(f"{tag}/alive", True,
               f"trace advancing with the target up (appends {a0} -> {a1}, quiet {q1} ms)")

        # ── target KILLED ───────────────────────────────────────────────────────────────
        sl.kill_all(bg)
        bg = None
        sl.kali("mon-down")

        got = wait_for(lambda: (rs(h)[0] >= QUIET_TARGET_MS) or None, timeout_s=25)
        q2, a2 = rs(h)
        if not got:
            record(f"{tag}/quiet-counter", False,
                   f"quiet counter only reached {q2} ms in 25 s (wanted >= {QUIET_TARGET_MS})")
            return
        record(f"{tag}/quiet-counter", True,
               f"counter reached {q2} ms after the target stopped")

        # THE LOAD-BEARING ASSERTION: the trace must be FROZEN, not merely labelled stalled.
        time.sleep(4)
        q3, a3 = rs(h)
        if a3 != a2:
            record(f"{tag}/halt", False,
                   f"the trace kept advancing while stalled (appends {a2} -> {a3}) -- a dead "
                   f"target still looks like a live one")
            return
        record(f"{tag}/halt", True,
               f"trace frozen across 4 s of silence (appends held at {a3}, quiet {q3} ms)")

        # ── target BACK, same SSID + BSSID ──────────────────────────────────────────────
        mu2 = sl.kali("mon-up", "wlan1", TARGET_CHAN)
        if not mu2.get("ok"):
            cannot(f"{tag}/recover", f"could not bring monitor mode back up: {mu2}")
            return
        bg = sl.kali_beacon_fanout(120, TARGET_CHAN)
        # The badge hops 14 channels, so re-acquisition is not instant. Allow 40 s and report the
        # actual figure rather than loosening the assertion until it passes.
        back = wait_for(lambda: (rs(h)[0] < STALL_MS) or None, timeout_s=40)
        q4, a4 = rs(h)
        if not back:
            record(f"{tag}/recover", False,
                   f"quiet counter never reset after the target returned (still {q4} ms after "
                   f"40 s) -- appends {a3} -> {a4}")
            return
        time.sleep(3)
        q5, a5 = rs(h)
        if a5 <= a4:
            record(f"{tag}/recover", False,
                   f"counter reset (quiet {q4} ms) but the trace did not resume "
                   f"(appends {a4} -> {a5})")
            return
        record(f"{tag}/recover", True,
               f"counter reset to {q4} ms and the trace resumed (appends {a4} -> {a5})")
    finally:
        sl.kill_all(bg)
        try:
            sl.kali("mon-down")
        except Exception:
            pass
        try:
            h.cmd("tool_stop")
        except Exception:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None)
    ap.add_argument("--rounds", type=int, default=2)
    a = ap.parse_args()

    # ap_scan blocks the badge ~15-22 s; the default bridge deadline is 10 s.
    os.environ.setdefault("CLIPBOY_SESSION_TIMEOUT", "30")
    print("=" * 70)
    print("TEST: Monitor > RSSI loss-of-signal indicator (owner spec, 2026-07-26)")
    print("=" * 70)
    sl.deploy_stim()

    h = Harness(port=a.port)
    ss_saved = None
    try:
        # Same screensaver pin as the other batteries: this test idles well past the display
        # timeout, and a locked screen stops nothing here but makes any screenshot useless.
        ss_saved = (h.cmd("cfg_get disp_off") or {}).get("value")
        h.cmd("cfg_set disp_off 5")
        h.cmd("cfg_set airplane false")
        for r in range(1, max(1, a.rounds) + 1):
            one_round(h, r)
            if r < a.rounds:
                h.reboot_and_wait()
                h.cmd("skip_boot")
    finally:
        try:
            if ss_saved is not None:
                h.cmd(f"cfg_set disp_off {int(ss_saved)}")
        except Exception:
            pass
        h.close()

    print("\n" + "=" * 70)
    hard = [r for r in RESULTS if r["ok"] is not None]
    passed = sum(1 for r in hard if r["ok"])
    cant = [r for r in RESULTS if r["ok"] is None]
    print(f"RESULTS: {passed}/{len(hard)} checks pass"
          + (f"  ({len(cant)} CANNOT-TEST)" if cant else ""))
    for r in RESULTS:
        if r["ok"] is False:
            print(f"  FAIL: {r['name']} -- {r['detail']}")
    for r in cant:
        print(f"  CANNOT-TEST: {r['name']} -- {r['detail']}")
    print("=" * 70)
    return 0 if (hard and passed == len(hard)) else 1


if __name__ == "__main__":
    sys.exit(main())
