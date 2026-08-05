#!/usr/bin/env py -3
"""test_signal_loss.py -- what does each target-dependent tool do when its target VANISHES?

Owner-designed (2026-07-25). The premise: no amount of source reading establishes this, because
every finding in this area has the same shape -- the tool does not fall over, it keeps asserting
success. So the assertion here is deliberately NOT "did it crash" but "what does the UI CLAIM
versus what is true".

Rig:
  kalipi (data@192.168.1.146, eth0 = management) injects a beacon on wlan1/mon for N seconds via
  kalipi_stim.py. That gives a target that EXISTS and then STOPS -- the loss event -- without
  needing to physically move hardware. The badge is COM4 (Sn34k) or COM5 (Res34rch).

Each case captures a screenshot at every phase into shots/signal_loss/, and every assertion is
recorded with the evidence, so `gen_signal_loss_report.py` can render an HTML report where a
"correct behaviour" claim is backed by a picture rather than by my say-so.

  CLIPBOY_PORT=COM4 py -3 scripts/tests/test_signal_loss.py
  CLIPBOY_PORT=COM5 py -3 scripts/tests/test_signal_loss.py --sku res34rch

Known-going-in (owner-confirmed on hardware with a phone hotspot, and traced in source):
  Monitor > RSSI flat-lines at the last-seen value. ap.rssi is written ONLY when a frame from
  that BSSID arrives (WiFiScan.cpp `if (!found) return;`), and nothing ages it -- so a flat trace
  is indistinguishable from a steady signal. Compounded by SIG_STREN never binding to the
  selected AP's channel (it uses the inherited set_channel). These tests EXPECT that today; they
  exist to (a) prove it on the rig, (b) capture the evidence, and (c) go green if it is ever fixed.
"""
import argparse
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Harness

KALIPI = os.environ.get("CLIPBOY_KALIPI", "192.168.1.146")
STIM_REMOTE = "/tmp/kalipi_stim.py"
STIM_LOCAL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "kalipi_stim.py")
SHOT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "..", "..", "shots", "signal_loss")
TARGET_SSID = "Pineapple"      # what kalipi_stim's "rogueap" beacon advertises
TARGET_CHAN = "6"

RESULTS = []


def _a(x):
    """ASCII-safe console output; badge labels are not cp1252-safe."""
    return str(x).encode("ascii", "replace").decode("ascii")


def record(case, phase, verdict, detail, shot=None, evidence=None):
    """verdict: PASS | LOSS-NOT-DETECTED | CANNOT-TEST | INFO.

    LOSS-NOT-DETECTED is deliberately its own verdict rather than FAIL: for most of these tools
    "the reading is retained" is CURRENT DOCUMENTED BEHAVIOUR (see README Known issues), not a
    regression. Calling it FAIL would make the suite permanently red and train everyone to
    ignore it. It is a finding to be looked at, with a picture attached.
    """
    RESULTS.append({"case": case, "phase": phase, "verdict": verdict, "detail": detail,
                    "shot": shot, "evidence": evidence or {}})
    print(_a(f"  [{verdict}] {case} / {phase}: {detail}"))


def kali(*args, timeout=90):
    """Run a kalipi_stim primitive over ssh. Returns its parsed JSON, or an error dict."""
    cmd = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=12", f"data@{KALIPI}",
           "sudo", "python3", STIM_REMOTE] + [str(a) for a in args]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return {"ok": False, "err": "ssh-timeout"}
    try:
        return json.loads(p.stdout.strip().splitlines()[-1])
    except Exception:
        return {"ok": False, "err": "unparseable", "raw": (p.stdout + p.stderr)[:300]}


def kali_bg(*args):
    """Fire a primitive WITHOUT waiting -- for a beacon that must run while we drive the badge."""
    cmd = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=12", f"data@{KALIPI}",
           "sudo", "python3", STIM_REMOTE] + [str(a) for a in args]
    return subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


# How many concurrent beacon injectors to run. ONE is not enough and the arithmetic is the
# reason: wifi-beacon emits ~3.4 frames/s (measured: "sent 41" over 12 s), while the badge's AP
# scan HOPS all 14 channels and therefore dwells on our channel roughly 1/14 of the time. That
# gives well under one expected sighting per scan -- which is exactly why the first two runs of
# this battery reported "could not select 'Pineapple'" and I initially mis-read it as a test bug.
# Several injectors in parallel raise the aggregate rate enough that a hopping scan cannot miss
# it. (The badge's own channel cannot be pinned from the harness: raw_channel only affects RAW
# capture, and the scan channel is only settable through the Utilities > Set Channel dropdown.)
BEACON_FANOUT = 5


def kali_beacon_fanout(secs, chan):
    """Start BEACON_FANOUT concurrent injectors; returns the list of processes to terminate."""
    return [kali_bg("wifi-beacon", "rogueap", str(secs), str(chan))
            for _ in range(BEACON_FANOUT)]


def kill_all(procs):
    for p in procs or []:
        try:
            p.terminate()
        except Exception:
            pass


def deploy_stim():
    subprocess.run(["scp", "-o", "BatchMode=yes", STIM_LOCAL,
                    f"data@{KALIPI}:{STIM_REMOTE}"],
                   capture_output=True, timeout=60)


def shot(h, name):
    """Screenshot into shots/signal_loss/<name>.bmp. Returns the basename or None.

    Screenshots are the whole point of this suite: an assertion that a gauge "still reads live"
    is only credible with the pixels attached.

    Uses the harness's OWN screenshot() -- which drives the same `screenshot` command and
    verifies the BMP magic -- rather than re-implementing the RGB565 read. grab_screenshot.py
    cannot be used here because it opens its own serial connection, and this session already
    holds the port (opening a second one is how the native-CDC wedge happens).
    """
    os.makedirs(SHOT_DIR, exist_ok=True)
    path = os.path.join(SHOT_DIR, f"{name}.bmp")
    try:
        h.screenshot(path)
        return os.path.basename(path) if os.path.exists(path) else None
    except Exception as e:
        print(_a(f"    (screenshot {name} failed: {type(e).__name__}: {e})"))
        return None


# ── Case 1: Monitor > RSSI ───────────────────────────────────────────────────
def case_rssi(h):
    """Select the kalipi beacon as the RSSI target, confirm a live reading, then stop the
    beacon and see whether the trace/readout distinguishes 'gone' from 'steady'."""
    case = "Monitor > RSSI"
    print(f"\n-- {case}: target vanishes mid-monitor --")
    mon = h.cat_pos("Monitor")
    sp = h.cat_pos("Scan")
    if mon is None or sp is None:
        record(case, "setup", "CANNOT-TEST", "Monitor/Scan category not found")
        return

    # Beacon for 150 s: long enough to scan, select, start, and observe before it dies.
    # mon_up FIRST. wifi_beacon injects on wlan1, which must be in MONITOR mode -- without this
    # the beacon is silently not transmitted and the badge simply never sees the target. My first
    # run of this battery reported "could not select 'Pineapple'" for exactly that reason: the
    # test was fine, the stimulus never happened. Assert it came up rather than assuming.
    mu = kali("mon-up", "wlan1", TARGET_CHAN)
    if not mu.get("ok"):
        record(case, "setup", "CANNOT-TEST",
               f"kalipi monitor mode did not come up: {_a(mu)}")
        return
    bg = kali_beacon_fanout(150, TARGET_CHAN)
    time.sleep(4)
    try:
        h.tool_stop(); h.wait(600)
        h.cmd(f"tool_open {sp} 0")           # Scan > APs (full) to populate the list
        # tool_open here too, for the same reason as the other two cases: this case's
        # rssi_01_scanned shot is supposed to evidence a scan in progress, and with tool_start
        # it evidenced the Status page instead.
        h.wait(12000)
        aps = (h.cmd("detect_counts") or {}).get("ap")
        h.tool_stop(); h.wait(1000)
        if not aps:
            record(case, "acquire", "CANNOT-TEST",
                   "no APs seen at all -- kalipi beacon not reaching the badge", shot=shot(h, "rssi_00_noaps"))
            return
        record(case, "acquire", "INFO", f"{aps} APs in the list after a 12 s scan",
               shot=shot(h, "rssi_01_scanned"), evidence={"ap_count": aps})

        # Open RSSI and select the target by name.
        h.cmd(f"tool_open {mon} 2")
        h.wait(1500)
        # tap_text_scrolled: the AP list overflows the content pane, and the target is often
        # below the fold (it was at y=218, inside the division bar, on the run that produced
        # this fix). tap_text refuses to tap an off-screen widget -- correctly, since those
        # pixels belong to the division bar and the tap would silently navigate -- so without
        # scrolling this case could only ever report cannot-test.
        if not h.tap_text_scrolled(TARGET_SSID, settle_ms=1200):
            record(case, "select", "CANNOT-TEST",
                   f"could not find/select '{TARGET_SSID}' in the AP list "
                   f"({getattr(h, 'last_tap_error', None)})", shot=shot(h, "rssi_02_noselect"))
            return
        record(case, "select", "INFO", f"selected '{TARGET_SSID}'",
               shot=shot(h, "rssi_03_selected"))

        # EARLY shot, 4 s in: catches the trace RISING from its 0 dBm start to the live value.
        # Owner's point (2026-07-26) -- a report that only shows a settled reading cannot
        # demonstrate the graph is live at all; the transition is the part that proves it moves.
        h.wait(4000)
        early_shot = shot(h, "rssi_03b_first_samples")
        record(case, "first-samples", "INFO",
               "4 s after start -- the trace should be climbing from 0 dBm toward the live "
               "value; this is the evidence that the graph updates at all",
               shot=early_shot)

        # Let it monitor while the target is ALIVE, and capture the settled reading.
        h.wait(9000)
        live_shot = shot(h, "rssi_04_target_alive")
        st_live = h.tool_state() or {}
        record(case, "monitoring", "INFO",
               f"monitoring with the target alive (running={st_live.get('running')}, "
               f"name={st_live.get('name')!r})", shot=live_shot)

        # Kill the target.
        kill_all(bg)
        kali("mon-down")                     # drop the injector interface for certainty
        record(case, "target-killed", "INFO", "kalipi beacon stopped and mon iface downed")

        # Two post-loss shots, 12 s and 25 s. One reading cannot tell a flat trace from a slow
        # decay; two spaced samples can, and 12 s is inside the window the owner asked about
        # ("what happens 10-20 s later on the graph").
        time.sleep(12)
        mid_shot = shot(h, "rssi_05a_gone_12s")
        record(case, "after-loss-12s", "INFO",
               "12 s after the target stopped -- compare against the 25 s shot: if the trace is "
               "identical in both, it is frozen rather than decaying",
               shot=mid_shot)
        time.sleep(13)
        gone_shot = shot(h, "rssi_05_target_gone")
        st_gone = h.tool_state() or {}
        still_claims = bool(st_gone.get("running"))
        # The honest question: does ANYTHING on screen distinguish "gone" from "steady"?
        if still_claims:
            record(case, "after-loss", "LOSS-NOT-DETECTED",
                   "the tool still reports running 25 s after the target stopped transmitting; "
                   "compare the two screenshots -- if the trace/readout is unchanged then a flat "
                   "line means either a steady signal or a dead target, and the UI does not say "
                   "which. This is current documented behaviour, not a regression.",
                   shot=gone_shot,
                   evidence={"alive_shot": live_shot, "gone_shot": gone_shot,
                             "running_after_loss": still_claims})
        else:
            record(case, "after-loss", "PASS",
                   "the tool stopped or flagged the loss on its own", shot=gone_shot)
    finally:
        kill_all(bg)
        try:
            h.tool_stop()
        except Exception:
            pass


# ── Case 2: the AP list's own freshness ──────────────────────────────────────
def case_ap_list_staleness(h):
    """An AP that has gone away stays in the list at its first-contact signal. There is no
    timestamp field on AccessPoint at all, so the list is an 'ever seen' record presented as
    live devices. This case measures how long a dead AP persists."""
    case = "AP list freshness"
    print(f"\n-- {case}: does a departed AP leave the list? --")
    sp = h.cat_pos("Scan")
    if sp is None:
        record(case, "setup", "CANNOT-TEST", "Scan category not found")
        return
    mu = kali("mon-up", "wlan1", TARGET_CHAN)   # see the note in case_rssi
    if not mu.get("ok"):
        record(case, "setup", "CANNOT-TEST", f"kalipi monitor mode did not come up: {_a(mu)}")
        return
    bg = kali_beacon_fanout(90, TARGET_CHAN)
    time.sleep(3)
    try:
        h.tool_stop(); h.wait(500)
        h.cmd("tool_start %s 10" % h.cat_pos("Utilities/Lists"))   # Clear All first
        h.wait(1500)
        # tool_open, not tool_start: tool_start launches the tool WITHOUT navigating, so
        # the badge stayed on STATS > Status and every screenshot in this case showed the
        # Status page with only the status-bar task label changing -- no output pane, no
        # device list, no evidence. tool_open lands on ITEMS > Tools and opens the tool's
        # own page, which is also the path a user actually takes.
        h.cmd(f"tool_open {sp} 0")
        h.wait(14000)
        n_with = (h.cmd("detect_counts") or {}).get("ap") or 0
        s1 = shot(h, "aplist_01_target_present")
        record(case, "present", "INFO", f"{n_with} APs while the target is beaconing", shot=s1)

        kill_all(bg)
        kali("mon-down")
        time.sleep(30)                       # a generous window for any aging to happen
        n_after = (h.cmd("detect_counts") or {}).get("ap") or 0
        s2 = shot(h, "aplist_02_target_gone")
        if n_after >= n_with:
            record(case, "after-loss", "LOSS-NOT-DETECTED",
                   f"AP count did not drop 30 s after the target stopped ({n_with} -> {n_after}). "
                   f"AccessPoint carries no last-seen field, so entries cannot age out and the "
                   f"list is a cumulative 'ever seen' record. Documented in README Known issues.",
                   shot=s2, evidence={"before": n_with, "after": n_after,
                                      "present_shot": s1, "gone_shot": s2})
        else:
            record(case, "after-loss", "PASS",
                   f"AP count dropped {n_with} -> {n_after} after the target left", shot=s2)
    finally:
        kill_all(bg)
        try:
            h.tool_stop()
        except Exception:
            pass


# ── Case 3: BLE target vanishing ─────────────────────────────────────────────
def case_ble_loss(h):
    """A BLE advertiser appears then stops. AirTag/Flock entries carry a last_seen the UI never
    reads, so a departed device keeps its last RSSI forever."""
    case = "BT scan freshness"
    print(f"\n-- {case}: does a departed BLE device leave the list? --")
    sp = h.cat_pos("Scan")
    if sp is None:
        record(case, "setup", "CANNOT-TEST", "Scan category not found")
        return
    try:
        h.tool_stop(); h.wait(500)
        kali_bg("ble-name", "CBLOSSTEST", "70")
        time.sleep(3)
        # tool_open, not tool_start: tool_start launches the tool WITHOUT navigating, so
        # the badge stayed on STATS > Status and every screenshot in this case showed the
        # Status page with only the status-bar task label changing -- no output pane, no
        # device list, no evidence. tool_open lands on ITEMS > Tools and opens the tool's
        # own page, which is also the path a user actually takes.
        h.cmd(f"tool_open {sp} 3")           # Scan > BT Devices
        h.wait(16000)
        n_with = (h.cmd("detect_counts") or {}).get("bt") or 0
        s1 = shot(h, "ble_01_advertiser_up")
        record(case, "present", "INFO", f"{n_with} BT devices while the advertiser is up", shot=s1)

        kali("ble-name", "off", "1")          # best-effort stop
        time.sleep(35)
        n_after = (h.cmd("detect_counts") or {}).get("bt") or 0
        s2 = shot(h, "ble_02_advertiser_gone")
        if n_after >= n_with:
            record(case, "after-loss", "LOSS-NOT-DETECTED",
                   f"BT count did not drop 35 s after the advertiser stopped "
                   f"({n_with} -> {n_after}). last_seen exists on these records but no UI reads "
                   f"it, so a departed device keeps its last reading.",
                   shot=s2, evidence={"before": n_with, "after": n_after,
                                      "present_shot": s1, "gone_shot": s2})
        else:
            record(case, "after-loss", "PASS",
                   f"BT count dropped {n_with} -> {n_after}", shot=s2)
    finally:
        try:
            h.tool_stop()
        except Exception:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None)
    ap.add_argument("--sku", default="sn34k", choices=["sn34k", "res34rch"])
    ap.add_argument("--out", default=None, help="write results JSON here (for the HTML report)")
    a = ap.parse_args()

    print("=" * 70)
    print("TEST: target loss-of-signal battery (kalipi target -> target stops)")
    print("=" * 70)
    print(f"kalipi: {KALIPI}   sku: {a.sku}")
    deploy_stim()
    caps = kali("caps")
    print(f"kalipi caps: {_a(caps)}")

    h = Harness(port=a.port)
    # PIN THE DISPLAY TIMEOUT TO "Never" FOR THE WHOLE BATTERY.
    # This suite's entire value is the screenshots -- and every one of them came out as the
    # SCREENSAVER ("Hold to unlock" over the mascot), because each case deliberately waits
    # 25-35 s for the target to disappear and the default timeout is 60 s with SERIAL COMMANDS
    # NOT COUNTING AS USER ACTIVITY. So the evidence attached to every assertion showed the lock
    # screen instead of the tool. Owner caught it reviewing the report; I had already fixed the
    # identical problem in test_teardown_paths.py and failed to sweep for the sibling -- the
    # criterion is "every test that screenshots or idles past the display timeout".
    ss_saved = None
    try:
        ss_saved = (h.cmd("cfg_get disp_off") or {}).get("value")
        h.cmd("cfg_set disp_off 5")            # 5 = Never
        print(f"screensaver: pinned to Never for the run (was {ss_saved})")
    except Exception as e:                      # noqa: BLE001
        print(f"screensaver: WARNING could not pin the display timeout ({e}) -- "
              f"screenshots may capture the lock screen instead of the tool")
    try:
        h.cmd("skip_boot")
        for fn in (case_rssi, case_ap_list_staleness, case_ble_loss):
            try:
                fn(h)
            except Exception as e:
                record(getattr(fn, "__name__", "?"), "exception", "CANNOT-TEST",
                       f"raised: {type(e).__name__}: {_a(e)}")
            try:
                h.reboot_and_wait()
                h.cmd("skip_boot")
            except Exception:
                pass
    finally:
        try:
            kali("mon-down")
        except Exception:
            pass
        # Put the user's screensaver setting back -- a suite that leaves the display timeout
        # disabled has changed the device it was measuring.
        try:
            if ss_saved is not None:
                h.cmd(f"cfg_set disp_off {int(ss_saved)}")
                print(f"screensaver: restored to {ss_saved}")
        except Exception as e:                   # noqa: BLE001
            print(f"screensaver: WARNING could not restore ({e}) -- "
                  f"set DATA > Settings > Screensaver by hand")
        h.close()

    print("\n" + "=" * 70)
    n_loss = sum(1 for r in RESULTS if r["verdict"] == "LOSS-NOT-DETECTED")
    n_cant = sum(1 for r in RESULTS if r["verdict"] == "CANNOT-TEST")
    n_pass = sum(1 for r in RESULTS if r["verdict"] == "PASS")
    print(f"RESULTS: {n_pass} detected-loss, {n_loss} loss-NOT-detected, {n_cant} cannot-test")
    print("=" * 70)

    out = a.out or os.path.join(SHOT_DIR, "signal_loss_results.json")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8") as f:
        json.dump({"sku": a.sku, "kalipi": KALIPI, "port": a.port or os.environ.get("CLIPBOY_PORT"),
                   "results": RESULTS}, f, indent=2)
    print(f"results JSON -> {out}")
    # Exit 0 even with LOSS-NOT-DETECTED: those are findings to read, not build breaks.
    return 1 if n_cant and not (n_pass + n_loss) else 0


if __name__ == "__main__":
    sys.exit(main())
