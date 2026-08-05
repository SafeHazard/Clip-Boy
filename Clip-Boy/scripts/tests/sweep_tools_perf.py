#!/usr/bin/env py -3
"""sweep_tools_perf.py -- per-tool result + RAM + FPS table for the whole SKU (Wave 6).

Walks EVERY tool in the live `tool_list` (name-based, so a category reorg or an SKU difference
cannot silently shrink coverage), starts it, dwells, and records:

    tool | verdict | DRAM start->end | PSRAM start->end | FPS low/hi/avg | notes

Emits JSON for gen_test_report.py and prints the table.

WHAT MAKES THIS NON-VACUOUS -- every one of these guards exists because its absence produced a
confident wrong answer somewhere in this campaign:

  * CRASH DETECTION IS THE PRIMARY GUARD. `state.uptime_ms` going BACKWARDS between two reads
    means the badge rebooted mid-tool. Without it, a tool that panics and auto-restarts looks
    IDENTICAL to a tool that ran cleanly -- the next command answers fine, because a fresh boot
    answers fine. A sweep without this reports "all tools OK" for a firmware that reboots on six
    of them.
  * FPS POSITIVE CONTROL before the sweep. If the rig cannot read a nonzero FPS while the badge
    is demonstrably idle-healthy, every per-tool number would be 0 and the table would read as a
    uniform performance catastrophe caused by the harness. Prove the observable can read POSITIVE
    first, in the same run.
  * `samples == 0` IS NOT `0 fps`. A dwell too short to close a 1 s window yields no samples;
    reporting that as zero FPS invents a stall. Such rows are marked NO-SAMPLES.
  * AIRPLANE MODE IS ASSERTED OFF. `tool_start` now goes through the UI's launch gate, so with
    airplane on EVERY radio tool is refused -- which would look like 30 broken tools.
  * A REFUSED START IS NOT A FAILURE and not a pass either. Tools that need a selected target
    (TAT_AP / TAT_STA) legitimately decline; they are reported NEEDS-TARGET so the table cannot
    be read as coverage it does not have.
  * POST-STOP `running` IS CHECKED. "The tool started" is not the claim under test; "the tool
    started and could be stopped" is.

  py -3 Clip-Boy/scripts/tests/sweep_tools_perf.py --port COM4
  py -3 Clip-Boy/scripts/tests/sweep_tools_perf.py --port COM5 --dwell 8 --json out.json
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Harness

# One-shot (TAT_IMMEDIATE) tools: they perform an action and finish, so there is no START/STOP
# control and `tool_open` merely navigates to the page. The sweep therefore never fires them --
# which is a COVERAGE GAP worth naming, distinct from a list viewer that has nothing to start.
# Keyed on the sweep's own "<Category>[index]" label, so this DOES carry indices and they can rot
# (they have been wrong three separate times in this codebase). The failure direction is the safe
# one: a drifted label falls through to the generic DID-NOT-START, which is still reported and
# still outside the OK count -- it just under-states the gap. Verified against cat_utilities[] /
# cat_network[] in ui_nav.h on 2026-07-27.
ONESHOT_CATS = {"Utilities/Lists", "Network"}
ONESHOT_TOOLS = {
    "Utilities/Lists[8]",   # Gen Rnd SSIDs
    "Utilities/Lists[10]",  # Clear All
    "Network[1]",           # Rnd AP MAC
    "Network[2]",           # Rnd STA MAC
}

# Tools to exclude from the sweep, keyed (category name, ITEM INDEX) because `tool_list` reports
# per-category item COUNTS, not item names -- so there is no name to match on here, and a lookup
# keyed by name would never fire while looking like it did. Empty by default: nothing is skipped,
# which is the honest state. If you add an entry, give the reason -- the sweep PRINTS the skip
# list, because a cap nobody sees reads as coverage.
SKIP = {
    # ("Utilities", 9): "wipes the AP/STA/SSID stores a later module depends on",
}


def _num(d, *keys, default=0):
    for k in keys:
        if isinstance(d, dict) and k in d:
            try:
                return int(d[k])
            except (TypeError, ValueError):
                pass
    return default


def heap_of(h):
    d = h.heap() or {}
    return (_num(d, "dram", "free", "free_heap"),
            _num(d, "psram", "free_psram", "spiram"),
            _num(d, "min_dram"))


def boot_id(h):
    st = h.state() or {}
    return _num(st, "uptime_ms", default=-1), _num(st, "reset_reason", default=-1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=os.environ.get("CLIPBOY_PORT", "COM4"))
    ap.add_argument("--dwell", type=float, default=6.0,
                    help="seconds to let each tool run (needs >=3 to close FPS windows)")
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    if args.dwell < 3:
        print("sweep: dwell must be >= 3 s or no FPS window closes and every row is NO-SAMPLES")
        return 2

    h = Harness(port=args.port)
    rows, skipped = [], []
    try:
        # ── P1: the harness answers at all ────────────────────────────────────────────
        st = h.state()
        if not st or not st.get("ok", True):
            print(f"sweep: CANNOT TEST -- no usable `state` from {args.port}")
            return 2

        # ── P2: airplane mode must be OFF or every radio tool is refused by the gate ──
        # Parse STRICTLY. An unrecognised response shape must abort, not be read as "off":
        # guessing "off" here would let a gated sweep report 30 REFUSED tools as a firmware
        # finding. A precondition that cannot be evaluated is a CANNOT TEST, not a pass.
        def airplane_on():
            r = h.cmd("cfg_get airplane") or {}
            for k in ("value", "airplane", "val"):
                if k in r:
                    return str(r[k]).strip().lower() in ("1", "true", "on")
            raise RuntimeError(f"cannot read airplane state from cfg_get response: {r!r}")

        try:
            if airplane_on():
                h.cmd("cfg_set airplane false")
                time.sleep(0.5)
                if airplane_on():
                    print("sweep: CANNOT TEST -- airplane mode is on and would not turn off; "
                          "every radio tool would be refused by the launch gate")
                    return 2
                print("sweep: airplane mode was ON -- turned it off for the sweep")
        except RuntimeError as e:
            print(f"sweep: CANNOT TEST -- {e}")
            return 2

        # ── P3: prove FPS can read POSITIVE on this rig, this run, this build ─────────
        h.fps_reset()
        time.sleep(3.5)
        probe = h.fps() or {}
        if _num(probe, "samples") < 2 or _num(probe, "fps_avg") <= 0:
            print(f"sweep: CANNOT TEST -- FPS observable never read positive while idle "
                  f"(samples={_num(probe, 'samples')}, avg={_num(probe, 'fps_avg')}). "
                  f"Every per-tool FPS number would be 0 for a RIG reason, not a firmware one.")
            return 2
        idle_fps = _num(probe, "fps_avg")
        print(f"sweep: idle FPS baseline {idle_fps} (positive control OK)")

        cats = (h.cmd("tool_list") or {}).get("cats", [])
        if not cats:
            print("sweep: CANNOT TEST -- tool_list came back empty")
            return 2

        base_uptime, base_reason = boot_id(h)
        print(f"sweep: {sum(c['items'] for c in cats)} tools across {len(cats)} categories "
              f"on {args.port}; dwell {args.dwell}s; reset_reason={base_reason}\n")

        for ci, cat in enumerate(cats):
            for item in range(cat["items"]):
                label = f"{cat['name']}[{item}]"
                if (cat["name"], item) in SKIP:
                    skipped.append((label, SKIP[(cat["name"], item)]))
                    continue

                d0, p0, _m0 = heap_of(h)
                up0, _r0 = boot_id(h)
                h.fps_reset()

                verdict, note = "OK", ""
                try:
                    # tool_open, NOT tool_start. tool_start launches the tool WITHOUT navigating, so
                    # FPS gets sampled while the badge sits on whatever page it was already on --
                    # measuring the idle Status screen, not the tool's own page with its chart or
                    # output pane rendering. That makes the FPS column answer a question nobody
                    # asked. (Same defect the owner caught in the loss-of-signal screenshots, which
                    # photographed STATS > Status while claiming to evidence a tool.)
                    # RAM is unaffected either way -- the heap is global -- but FPS is per-screen.
                    r = h.cmd(f"tool_open {ci} {item}") or {}
                except Exception as e:                       # noqa: BLE001
                    r = {"ok": False, "error": f"{type(e).__name__}: {e}"}

                if not r.get("ok", False):
                    err = str(r.get("error", ""))[:80]
                    # A refusal is information, not a failure -- but it is NOT coverage.
                    if "target" in err.lower() or "select" in err.lower():
                        verdict = "NEEDS-TARGET"
                    elif "airplane" in err.lower():
                        verdict = "GATED"
                    else:
                        verdict = "REFUSED"
                    note = err
                    rows.append(dict(tool=label, verdict=verdict, note=note,
                                     dram0=d0, dram1=d0, psram0=p0, psram1=p0,
                                     fps_min=None, fps_max=None, fps_avg=None, samples=0))
                    print(f"  {label:<28} {verdict:<13} {note}")
                    continue

                time.sleep(args.dwell)
                f = h.fps() or {}
                ts = h.tool_state() or {}
                d1, p1, _m1 = heap_of(h)

                try:
                    h.tool_stop()
                except Exception:                            # noqa: BLE001
                    pass
                time.sleep(0.6)

                # CRASH CHECK. Do this AFTER the stop so a panic during teardown is caught too.
                up1, r1 = boot_id(h)
                if up1 >= 0 and up0 >= 0 and up1 < up0:
                    verdict = "REBOOTED"
                    note = f"uptime went {up0} -> {up1} ms (reset_reason={r1}) -- the badge " \
                           f"restarted during this tool"
                else:
                    after = h.tool_state() or {}
                    if after.get("running"):
                        verdict = "STOP-FAILED"
                        note = "tool_state.running still true after tool_stop"
                    elif not (r.get("running") or ts.get("running")):
                        # `tool_open` answers ok:true for "I navigated to the page", which is NOT
                        # "the tool is running". th_find_start_btn only matches labels START/STOP,
                        # and several pages have none ("> CONNECT <", "> ENTER SSID <",
                        # "> EXECUTE <"); a PCAP tool also early-returns when "Allow PCAP Saving"
                        # is off, which is the DEFAULT on Sn34k. Those all used to score OK with a
                        # full FPS row, so the table asserted coverage of a tool that never ran.
                        # The FPS numbers are still real -- they are just about the PAGE.
                        # Two different things land here and they must not read alike:
                        #  - a viewer/picker/text page has NOTHING to start, so "did not start" is
                        #    the expected and complete answer; the FPS row is a page measurement.
                        #  - a ONE-SHOT action (Clear All, Gen Rnd SSIDs, Rnd AP/STA MAC) DOES have
                        #    an action, and `tool_open` only navigates -- so the sweep never fires
                        #    it. That is a real coverage gap, and filing it under the same label as
                        #    a list viewer hides it.
                        # The badge does not report its action type over the harness, so classify
                        # by name against the known one-shots. A name that drifts falls back to the
                        # generic verdict, which is the safe direction (reported, not silently OK).
                        if label.split("[")[0].strip() in ONESHOT_CATS and label in ONESHOT_TOOLS:
                            verdict = "ONESHOT-NOT-FIRED"
                            note = ("one-shot action; tool_open only navigates to the page, so this "
                                    "action was NEVER EXERCISED by the sweep -- not covered")
                        else:
                            verdict = "DID-NOT-START"
                            note = (f"no start control exists for this page (open btn={r.get('btn')!r} "
                                    f"running={r.get('running')!r}) -- expected for a viewer/picker/"
                                    f"text page; FPS is for the PAGE, not a tool")
                    elif _num(f, "samples") == 0:
                        verdict = "NO-SAMPLES"
                        note = f"dwell {args.dwell}s closed no 1s FPS window (not 0 fps)"

                row = dict(tool=label, verdict=verdict, note=note,
                           dram0=d0, dram1=d1, psram0=p0, psram1=p1,
                           fps_min=_num(f, "fps_min"), fps_max=_num(f, "fps_max"),
                           fps_avg=_num(f, "fps_avg"), samples=_num(f, "samples"),
                           lib_scanning=bool(ts.get("lib_scanning")),
                           promisc=bool(ts.get("promisc")))
                rows.append(row)
                dd = d1 - d0
                print(f"  {label:<28} {verdict:<13} DRAM {d0}->{d1} ({dd:+d})  "
                      f"FPS {row['fps_min']}/{row['fps_max']}/{row['fps_avg']} "
                      f"n={row['samples']} {note}")

                if verdict == "REBOOTED":
                    # A reboot invalidates every subsequent reading's baseline, and the badge
                    # comes back with default state. Stop rather than emit a table whose later
                    # rows silently describe a different session.
                    print("  !! aborting the sweep: readings after a reboot are not comparable")
                    break
            else:
                continue
            break
    finally:
        h.close()

    ok = [r for r in rows if r["verdict"] == "OK"]
    bad = [r for r in rows if r["verdict"] in ("REBOOTED", "STOP-FAILED", "REFUSED")]
    # Break down EVERY verdict, and state how many rows actually exercised a running tool.
    # "39 tools exercised: 39 OK" was the old headline shape even when rows had landed in
    # verdicts that were in neither bucket (NO-SAMPLES, GATED, and now DID-NOT-START) -- a
    # summary whose counts do not add up to the number of rows is where un-run work hides.
    tally = {}
    for r in rows:
        tally[r["verdict"]] = tally.get(r["verdict"], 0) + 1
    breakdown = ", ".join(f"{n} {v}" for v, n in sorted(tally.items(), key=lambda kv: -kv[1]))
    print(f"\n{len(rows)} tool pages visited -- {breakdown}")
    print(f"  tools actually EXERCISED (ran and stopped cleanly): {len(ok)}/{len(rows)}")
    if sum(tally.values()) != len(rows):                     # arithmetic self-check
        print(f"  !! tally {sum(tally.values())} != rows {len(rows)} -- summary is unreliable")
    for label, why in skipped:
        print(f"  SKIPPED {label}: {why}  <-- NOT covered")
    for r in bad:
        print(f"  {r['verdict']}: {r['tool']} -- {r['note']}")
    # ONESHOT-NOT-FIRED first: it is the one that means "an action exists and we never ran it".
    for want in ("ONESHOT-NOT-FIRED", "DID-NOT-START"):
        for r in rows:
            if r["verdict"] == want:
                print(f"  {want}: {r['tool']} -- {r['note']}")

    if args.json:
        with open(args.json, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(dict(port=args.port, dwell=args.dwell, idle_fps=idle_fps,
                           rows=rows, skipped=skipped), fh, indent=2)
        print(f"\njson -> {args.json}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
