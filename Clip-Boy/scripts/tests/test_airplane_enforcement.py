#!/usr/bin/env py -3
"""test_airplane_enforcement.py -- is Airplane Mode enforced COMPLETELY, and only when enabled?

Owner's question (2026-07-26): "what tests do we have in place to ensure that airplane mode (when
enabled) is being enforced properly & completely across the UI side of things, and is not (when
disabled)?" The honest answer at the time was TWO tests, both one-directional: F7 (Geiger) and F7b
(scripted tool_start). Neither covered the inline Scan buttons, the teardown direction, or the
disabled direction explicitly. This module is that coverage.

BOTH DIRECTIONS, and the second one is not optional:
  ENFORCED  -- with airplane ON, each radio path must refuse and the DRIVER must stay quiet
               (tool_state.promisc / lib_scanning, not the UI's own bookkeeping).
  NOT OVER-ENFORCED -- with airplane OFF, the same path must WORK. Without this half, a test suite
               passes just as happily when a tool is simply broken: "refused" and "dead" are
               indistinguishable from the ON side alone. Every ON assertion below is paired.

The path list comes from a mechanical trace of every radio-start call site (2026-07-26), not from
memory. Paths deliberately NOT covered here, with reasons:
  - Marauder serial CLI (`cli scanap`) -- MEASURED as ungated and DOCUMENTED as such by owner
    decision; gating it means editing the vendored parser. Asserted here as a KNOWN-UNGATED
    expectation so that if it ever starts refusing, we find out deliberately rather than by
    surprise.
  - Boot bringing the radio up (Clip-Boy.ino:232) -- inherent; airplane is a policy gate, not a
    hardware power-down. Not testable as a refusal.

    CLIPBOY_PORT=COM4 py -3 Clip-Boy/scripts/tests/test_airplane_enforcement.py
    py -3 Clip-Boy/scripts/tests/test_airplane_enforcement.py --port COM5
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Harness

RESULTS = []
# Set in main(); lets record()/cannot() auto-capture without threading a handle through every case.
CURRENT_H = None


def _a(x):
    return str(x).encode("ascii", "replace").decode("ascii")


def _shot(name):
    """Capture failure evidence from the INDEPENDENT channel (pixels), not the object tree that
    produced the reading we are doubting. Never raises -- see Harness.snap_on_failure."""
    if CURRENT_H is None:
        return
    p = CURRENT_H.snap_on_failure(name)
    if p:
        RESULTS[-1]["shot"] = p
        print(f"        (screenshot: {os.path.basename(p)})")


def record(name, ok, detail, blocker=True):
    RESULTS.append({"name": name, "ok": ok, "detail": detail, "blocker": blocker})
    tag = "PASS" if ok else ("FAIL" if blocker else "warn")
    print(_a(f"  [{tag}] {name}: {detail}"))
    if not ok:
        _shot(name)


def cannot(name, detail):
    RESULTS.append({"name": name, "ok": None, "detail": detail, "blocker": False})
    print(_a(f"  [CANNOT-TEST] {name}: {detail}"))
    # CANNOT-TEST especially: it means a PRECONDITION did not hold, and the pixels usually say why
    # in one glance (wrong page, a modal in the way, the screensaver, a dimmed control).
    _shot(name)


def radio(h):
    """The DRIVER's view -- (lib_scanning, promisc). The UI's cb_op_running is not evidence:
    a CLI- or inline-started scan never sets it, which is how two of these defects hid."""
    t = h.tool_state() or {}
    return bool(t.get("lib_scanning")), bool(t.get("promisc"))


def air(h, on):
    h.cmd(f"cfg_set airplane {'true' if on else 'false'}")
    time.sleep(1.5)
    v = (h.cmd("cfg_get airplane") or {}).get("value")
    return str(v).strip().lower() in ("1", "true", "on")


def quiet(h, label):
    """Return the badge to a known-quiet radio state and confirm it. Every case starts here so a
    leftover scan from the previous case cannot be mistaken for this case's failure."""
    try:
        h.cli("stopscan")
    except Exception:
        pass
    try:
        h.cmd("tool_stop")
    except Exception:
        pass
    time.sleep(2)
    ls, pr = radio(h)
    if ls or pr:
        # Airplane ON is the biggest hammer we have (it now tears down unconditionally).
        air(h, True); time.sleep(2); air(h, False); time.sleep(1)
        ls, pr = radio(h)
    return not (ls or pr)


# ── 1. Geiger (F7's surface, re-asserted here so this module stands alone) ───────────────
def case_geiger(h):
    name = "geiger"
    if not quiet(h, name):
        cannot(name, "could not reach a quiet radio state first")
        return
    air(h, True)
    r_on = h.cmd("geiger_start") or {}
    time.sleep(2)
    ls_on, pr_on = radio(h)
    refused = (not r_on.get("ok", False)) or ("airplane" in str(r_on.get("error", "")).lower())
    h.cmd("geiger_stop"); air(h, False); time.sleep(1)
    # PAIRED HALF: it must actually work with airplane off, or "refused" proves nothing.
    r_off = h.cmd("geiger_start") or {}
    time.sleep(3)
    ls_off, pr_off = radio(h)
    h.cmd("geiger_stop"); time.sleep(1)
    works_off = r_off.get("ok", False) and pr_off
    if not works_off:
        cannot(name, f"the Geiger does not start even with airplane OFF "
                     f"(resp={_a(r_off)}, promisc={pr_off}) -- the ON result below would be "
                     f"meaningless")
        return
    if refused and not pr_on:
        record(name, True, f"refused under airplane (promisc stayed {pr_on}); works with it off "
                           f"(promisc {pr_off})")
    else:
        record(name, False, f"started under airplane: refused={refused} promisc={pr_on} "
                            f"lib_scanning={ls_on}")


# ── 2. Scripted tool start (F7b's surface) ────────────────────────────────────────────────
def case_tool_start(h):
    name = "tool_start"
    sp = h.cat_pos("Scan")
    if sp is None:
        cannot(name, "Scan category not found")
        return
    if not quiet(h, name):
        cannot(name, "could not reach a quiet radio state first")
        return
    air(h, True)
    r_on = h.cmd(f"tool_start {sp} 0") or {}
    time.sleep(3)
    ls_on, pr_on = radio(h)
    refused = not r_on.get("ok", False)
    h.cmd("tool_stop"); air(h, False); time.sleep(1)
    r_off = h.cmd(f"tool_start {sp} 0") or {}
    time.sleep(4)
    ls_off, pr_off = radio(h)
    h.cmd("tool_stop"); time.sleep(1)
    if not (r_off.get("ok", False) and (ls_off or pr_off)):
        cannot(name, f"Scan[0] does not start with airplane OFF (resp={_a(r_off)}, "
                     f"lib={ls_off} promisc={pr_off})")
        return
    if refused and not (ls_on or pr_on):
        record(name, True, f"refused under airplane (lib={ls_on} promisc={pr_on}); works off "
                           f"(lib={ls_off} promisc={pr_off})")
    else:
        record(name, False, f"started under airplane: refused={refused} lib={ls_on} "
                            f"promisc={pr_on}")


# ── 3. THE NEW ONE: the inline Scan button on the Select AP page ──────────────────────────
def case_inline_scan(h):
    """Traced 2026-07-26 as the one USER-REACHABLE, TAP-ONLY path that ignored airplane mode.

    Reachable because tool_needs_radio() ALLOWLISTS "Select AP" as a non-radio tool, so
    tool_tap_cb's gate never fires -- and the page then offers a button that starts the radio.
    The fix guards cb_inline_scan_blocked_for(), which also dims the button.
    """
    name = "inline_scan"
    up = h.cat_pos("Utilities/Lists")
    if up is None:
        cannot(name, "Utilities/Lists category not found")
        return
    if not quiet(h, name):
        cannot(name, "could not reach a quiet radio state first")
        return

    # Select AP is item 9 per the audit notes; assert we landed on the right page rather than
    # trusting the index, because an index is exactly what a category reorg rots.
    air(h, True)
    h.cmd(f"tool_open {up} 9")
    time.sleep(1.5)
    labels = [t for t in ((h.cmd("text") or {}).get("texts") or []) if isinstance(t, str)]
    if not any("Select" in t or "Deselect" in t for t in labels):
        cannot(name, f"Utilities[9] is not the Select AP page -- labels {_a(labels[:12])}")
        return
    # DO NOT BLIND-TAP A POSSIBLY-DIMMED CONTROL.
    # When the fix dims the button it loses LV_OBJ_FLAG_CLICKABLE, and `find` then walks UP to the
    # nearest clickable ANCESTOR -- measured: hit_w jumps from 42 px (the button) to 172 px (a
    # container). tap_text taps that ancestor and returns True, so the tap lands on an unintended
    # widget and disturbs the page for everything after it. That is what made this case's
    # airplane-OFF half fail on the first run, and it is a harness sharp edge, not a firmware bug:
    # `find` substituting an ancestor is silent retargeting of exactly the kind the "locating a
    # target is not the same as being able to act on it" rule warns about.
    # So: measure the resolved hit. A hit box far wider than the label means the button itself is
    # NOT clickable, which IS the enforcement signal -- record it and skip the tap.
    hits = h.find_exact("Scan")
    hit_w_on = hits[0].get("hit_w") if hits else None
    dimmed = bool(hits) and hit_w_on is not None and hit_w_on > 100
    tapped_on = False
    if not dimmed:
        tapped_on = h.tap_text("=Scan", settle_ms=2500)
    time.sleep(2)
    ls_on, pr_on = radio(h)

    air(h, False)
    time.sleep(1)
    # PAIRED HALF: with airplane off the same button must start a scan, or the ON result could
    # just mean the button is broken / the label moved.
    h.cmd(f"tool_open {up} 9")
    time.sleep(1.5)
    tapped_off = h.tap_text("=Scan", settle_ms=2500)
    time.sleep(3)
    ls_off, pr_off = radio(h)
    quiet(h, name)

    if not (tapped_off and (ls_off or pr_off)):
        cannot(name, f"the inline Scan button does not start a scan with airplane OFF "
                     f"(tapped={tapped_off} lib={ls_off} promisc={pr_off}) -- cannot conclude "
                     f"anything from the airplane-ON reading")
        return
    if not (ls_on or pr_on):
        how = (f"button DIMMED under airplane (resolved hit_w={hit_w_on}px vs a ~42px button, "
               f"i.e. not clickable in its own right)") if dimmed else \
              (f"button tappable but the handler refused (tapped={tapped_on})")
        record(name, True, f"no radio under airplane -- {how}; starts normally with airplane off "
                           f"(lib={ls_off} promisc={pr_off})")
    else:
        record(name, False, f"INLINE SCAN STARTED THE RADIO UNDER AIRPLANE MODE "
                            f"(lib={ls_on} promisc={pr_on}) -- switch reads engaged while the "
                            f"driver is live")


# ── 4. Teardown direction: engaging airplane must stop something already running ──────────
def case_teardown(h):
    """Today's other fix. airplane_apply() used to route through cb_stop_operation(), which is
    gated on cb_op_running -- so a scan started outside the UI's bookkeeping survived it."""
    name = "teardown"
    if not quiet(h, name):
        cannot(name, "could not reach a quiet radio state first")
        return
    air(h, False)
    h.cli("scanap")                      # deliberately the UNGATED path, to prove the teardown
    time.sleep(4)
    ls_run, pr_run = radio(h)
    if not (ls_run or pr_run):
        cannot(name, "could not get a scan running with airplane off, so there is nothing to "
                     "prove got torn down")
        return
    air(h, True)
    time.sleep(3)
    ls_after, pr_after = radio(h)
    quiet(h, name)
    air(h, False)
    if not ls_after and not pr_after:
        record(name, True, f"engaging airplane stopped a CLI-started scan "
                           f"(lib {ls_run}->{ls_after}, promisc {pr_run}->{pr_after})")
    else:
        record(name, False, f"radio still live after engaging airplane "
                            f"(lib={ls_after} promisc={pr_after})")


# ── 5. wifijoin serial command (production build, gated 2026-07-26) ───────────────────────
def case_wifijoin(h):
    name = "wifijoin"
    if not quiet(h, name):
        cannot(name, "could not reach a quiet radio state first")
        return
    air(h, True)
    # ⚠ THIS CASE CANNOT REACH THE CODE IT NAMES, and used to report PASS anyway.
    # The comment here used to say "Not h.cli(): send it as a raw CLI line so the real handler
    # runs" -- and then called h.cli() on the very next line, doing the opposite of its own
    # reasoning. The `wifijoin` airplane gate lives in Clip-Boy.ino's PLAIN-TEXT branch, which is
    # reached only by a line with NO STX prefix; h.cli() goes to th_cmd_cli, which forwards to the
    # MARAUDER parser, and every harness path prefixes STX. So the Marauder parser simply rejects
    # the command, no radio comes up, and "radio stayed down" was recorded as the gate working.
    # It would read identically with the gate deleted -- a textbook false green.
    # Reaching it needs a plain-line session verb in the bridge; until that exists this reports
    # cannot-test. A refusal that was never issued is not evidence of a refusal.
    r = h.cli("wifijoin __cb_airplane_probe__") or {}
    time.sleep(3)
    ls, pr = radio(h)
    air(h, False)
    quiet(h, name)
    if ls or pr:
        # Still worth failing on: something brought the radio up under airplane, whatever route
        # the command took. This direction is sound even though the pass direction is not.
        record(name, False, f"wifijoin brought the radio up under airplane (lib={ls} promisc={pr})")
    else:
        cannot(name, "the .ino plain-text `wifijoin` gate is UNREACHABLE from the harness "
                     f"(every path prefixes STX; h.cli() -> Marauder parser, via={r.get('via')!r}). "
                     "The radio stayed down, but it would have stayed down with the gate deleted, "
                     "so this proves nothing. Needs a plain-line session verb in test_bridge.")


# ── 6. Marauder CLI: KNOWN-UNGATED by owner decision -- assert the expectation ────────────
def case_cli_known_ungated(h):
    """Documented, not fixed (gating it means editing the vendored parser). Asserted so that a
    future change of behaviour surfaces deliberately instead of silently."""
    name = "cli_known_ungated"
    if not quiet(h, name):
        cannot(name, "could not reach a quiet radio state first")
        return
    air(h, True)
    h.cli("scanap")
    time.sleep(4)
    ls, pr = radio(h)
    quiet(h, name)
    air(h, False)
    if ls or pr:
        record(name, True, f"as DOCUMENTED: the Marauder CLI still starts the radio under "
                           f"airplane (lib={ls} promisc={pr}). Known issue, owner decision.",
               blocker=False)
    else:
        record(name, True, "the CLI now REFUSES under airplane -- behaviour changed for the "
                           "better; update the known-issues note and this expectation",
               blocker=False)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None)
    a = ap.parse_args()
    os.environ.setdefault("CLIPBOY_SESSION_TIMEOUT", "30")
    print("=" * 70)
    print("TEST: Airplane Mode enforcement -- both directions")
    print("=" * 70)
    h = Harness(port=a.port)
    global CURRENT_H
    CURRENT_H = h                  # enables auto-capture in record()/cannot()
    ss = None
    try:
        ss = (h.cmd("cfg_get disp_off") or {}).get("value")
        h.cmd("cfg_set disp_off 5")     # this module idles past the display timeout
        for fn in (case_geiger, case_tool_start, case_inline_scan,
                   case_teardown, case_wifijoin, case_cli_known_ungated):
            try:
                fn(h)
            except Exception as e:                       # noqa: BLE001
                record(getattr(fn, "__name__", "?"), False,
                       f"raised: {type(e).__name__}: {_a(e)}")
    finally:
        try:
            h.cmd("cfg_set airplane false")
            if ss is not None:
                h.cmd(f"cfg_set disp_off {int(ss)}")
        except Exception:
            pass
        h.close()

    print("\n" + "=" * 70)
    hard = [r for r in RESULTS if r["ok"] is not None and r["blocker"]]
    passed = sum(1 for r in hard if r["ok"])
    cant = [r for r in RESULTS if r["ok"] is None]
    print(f"RESULTS: {passed}/{len(hard)} blocking checks pass"
          + (f"  ({len(cant)} CANNOT-TEST)" if cant else ""))
    for r in RESULTS:
        if r["ok"] is False:
            print(_a(f"  {'FAIL' if r['blocker'] else 'warn'}: {r['name']} -- {r['detail']}"))
    for r in cant:
        print(_a(f"  CANNOT-TEST: {r['name']} -- {r['detail']}"))
    print("=" * 70)
    return 0 if (hard and passed == len(hard)) else 1


if __name__ == "__main__":
    sys.exit(main())
