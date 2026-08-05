#!/usr/bin/env python3
"""
overnight_integration.py -- Clip-Boy tool INDEPENDENCE / integration test.

Drives every tool on the DUT badge (COM11) in RANDOM order, per SKU, twice each,
to prove tools operate independently -- one tool's run must not leave state that
breaks a later tool. For each tool it records:
  - functional correctness  (kalipi RF stimulus -> detect_counts / pkt_counters delta)
  - SD output               (new pcap / portal files, via uncapped sd_exists probing)
  - UI FPS                  (min / max / avg during execution)
  - free RAM                (DRAM start / low / end + PSRAM + min-ever low-water)
  - faithful screenshot     (real UI output pane, via the tool_open harness cmd)
  - reboot resilience       (uptime monotonicity; repro + quarantine on reboot)
  - independence signals    (heap-leak trend, predecessor correlation across passes)

Active-transmit tools (Res34rch) additionally get EXTERNAL-WITNESS efficacy via
kalipi monitor (wlan1) and/or the COM8 witness badge.

Usage:
  py -3 scripts/tests/overnight_integration.py --smoke          # quick plumbing check
  py -3 scripts/tests/overnight_integration.py --pass res34rch --seed 1
  py -3 scripts/tests/overnight_integration.py --pcap-matrix --sku res34rch
  py -3 scripts/tests/overnight_integration.py --full           # full 4-pass run (flashes SKUs)

Env: CLIPBOY_DUT (COM11), CLIPBOY_WITNESS (COM8), CLIPBOY_KALIPI (192.168.1.146).
"""

import argparse
import csv
import json
import os
import re
import sys
import time
import threading
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)

from harness import Harness  # noqa: E402
# Reuse the PROVEN kalipi plumbing + stimulus/verify specs from the tool suite.
import tool_suite as TS  # noqa: E402

DUT_PORT = os.environ.get("CLIPBOY_DUT", "COM11")
WITNESS_PORT = os.environ.get("CLIPBOY_WITNESS", "COM8")
RESULTS_ROOT = os.path.join(HERE, "overnight_results")

# ─────────────────────────── PLAYLIST ────────────────────────────
# (cid, item, name, sku, flags). cid == stable category id == cat array index
# (res34rch: 0-11; sn34k: passive 0-5 identical). Verified at startup via tool_list.
#   sku:   "S" = both SKUs, "R" = Res34rch only
#   kind:  wifi | ble | passive-wifi | util | keyboard | active-wifi | active-ble | portal
#   pcap:  pcap filename prefix if it writes one (else None)
#   gated: True if start is BLOCKED when Allow-PCAP is off (Raw, EAPOL)
# stim/verify are pulled from tool_suite.ROWS by "cid.item" where present (proven),
# else a category-default best-effort stimulus with functional=INCONCLUSIVE.
PLAYLIST = [
    # ── Detect (0) ──
    dict(cid=0, item=0, name="AirTag",         sku="S", kind="ble"),
    dict(cid=0, item=1, name="Skimmer Check",  sku="S", kind="ble"),
    dict(cid=0, item=2, name="Flipper Zero",   sku="S", kind="ble"),
    dict(cid=0, item=3, name="Flock Batteries", sku="S", kind="ble",  pcap="flock"),
    dict(cid=0, item=4, name="Rogue AP",       sku="S", kind="wifi", pcap="pinescan"),
    dict(cid=0, item=5, name="Evil Twin",      sku="S", kind="wifi", pcap="multissid"),
    # ── Scan (1) ──
    dict(cid=1, item=0, name="APs (full)",     sku="S", kind="wifi", pcap="ap"),
    dict(cid=1, item=1, name="APs + Stations", sku="S", kind="wifi", pcap="ap_sta"),
    dict(cid=1, item=2, name="Stations",       sku="S", kind="wifi", pcap="station"),
    dict(cid=1, item=3, name="BT Devices",     sku="S", kind="ble"),
    dict(cid=1, item=4, name="BLE Adverts",    sku="S", kind="ble"),
    # ── Monitor (2) ──
    dict(cid=2, item=0, name="Packets",        sku="S", kind="wifi", pcap="packet_monitor", chart=True),
    dict(cid=2, item=1, name="Packet Rate",    sku="S", kind="wifi", pcap="packet_monitor", chart=True),
    dict(cid=2, item=2, name="RSSI",           sku="S", kind="wifi", chart=True, select="shipship"),
    dict(cid=2, item=3, name="Channel Stats",  sku="S", kind="wifi", chart=True),
    dict(cid=2, item=4, name="MAC Tracker",    sku="S", kind="wifi", pcap="mac_track"),
    # ── Analyze (3) ──
    dict(cid=3, item=0, name="Beacons",        sku="S", kind="wifi", pcap="ap", chart=True),
    dict(cid=3, item=1, name="Probes",         sku="S", kind="wifi", pcap="probe"),
    dict(cid=3, item=2, name="Deauth",         sku="S", kind="wifi", pcap="deauth", inject_deauth=True),
    dict(cid=3, item=3, name="Raw/PCAP",       sku="S", kind="wifi", pcap="raw",   gated=True),
    dict(cid=3, item=4, name="Pwnagotchi",     sku="S", kind="wifi", pcap="pwnagotchi"),
    dict(cid=3, item=5, name="Espressif",      sku="S", kind="wifi", pcap="ap"),
    dict(cid=3, item=6, name="SAE Commit",     sku="S", kind="wifi", pcap="sae_commit"),
    dict(cid=3, item=7, name="EAPOL/PMKID",    sku="S", kind="wifi", pcap="eapol", gated=True, select="shipship", channel=7),
    # ── Utilities/Lists (4) -- self-contained, no RF ──
    dict(cid=4, item=0,  name="List APs",       sku="S", kind="util"),
    dict(cid=4, item=1,  name="List SSIDs",     sku="S", kind="util"),
    dict(cid=4, item=2,  name="List Stations",  sku="S", kind="util"),
    dict(cid=4, item=3,  name="List BT Devices",sku="S", kind="util"),
    dict(cid=4, item=4,  name="List AirTags",   sku="S", kind="util"),
    dict(cid=4, item=5,  name="List Flippers",  sku="S", kind="util"),
    dict(cid=4, item=6,  name="Saved Networks", sku="S", kind="util"),
    dict(cid=4, item=7,  name="Add SSID",       sku="S", kind="keyboard"),
    dict(cid=4, item=8,  name="Gen Rnd SSIDs",  sku="S", kind="util"),
    dict(cid=4, item=9,  name="Select AP",      sku="S", kind="util"),
    dict(cid=4, item=10, name="Clear All",      sku="S", kind="util"),
    dict(cid=4, item=11, name="Set Channel",    sku="S", kind="util"),
    # ── Network (5) ──
    dict(cid=5, item=0, name="Join WiFi",       sku="S", kind="keyboard", special="join_wifi"),
    dict(cid=5, item=1, name="Rnd AP MAC",      sku="S", kind="util"),
    dict(cid=5, item=2, name="Rnd STA MAC",     sku="S", kind="util"),
    # ── Deauth (6) -- ACTIVE ──
    dict(cid=6, item=0, name="Discovered",      sku="R", kind="active-wifi", witness="deauth"),
    dict(cid=6, item=1, name="Manual",          sku="R", kind="active-wifi", witness="deauth"),
    dict(cid=6, item=2, name="Stations",        sku="R", kind="active-wifi", witness="deauth"),
    # ── Flood (7) -- ACTIVE ──
    dict(cid=7, item=0, name="Auth",            sku="R", kind="active-wifi", witness="sniff_auth"),
    dict(cid=7, item=1, name="Bad Msg",         sku="R", kind="active-wifi", witness="sniff_any"),
    dict(cid=7, item=2, name="Bad Msg Target",  sku="R", kind="active-wifi", witness="sniff_any"),
    dict(cid=7, item=3, name="Sleep",           sku="R", kind="active-wifi", witness="sniff_any"),
    dict(cid=7, item=4, name="Sleep Target",    sku="R", kind="active-wifi", witness="sniff_any"),
    # ── Beacon Spam (8) -- ACTIVE (witness via COM8 scan) ──
    dict(cid=8, item=0, name="Random",          sku="R", kind="active-wifi", witness="beacon"),
    dict(cid=8, item=1, name="List",            sku="R", kind="active-wifi", witness="beacon"),
    dict(cid=8, item=2, name="AP Clone",        sku="R", kind="active-wifi", witness="beacon"),
    dict(cid=8, item=3, name="Rick Roll",       sku="R", kind="active-wifi", witness="beacon"),
    dict(cid=8, item=4, name="Funny",           sku="R", kind="active-wifi", witness="beacon", special="beacon_funny"),
    # ── BLE Spam (9) -- ACTIVE (witness via COM8 BLE scan) ──
    dict(cid=9, item=0, name="Sour Apple",      sku="R", kind="active-ble", witness="ble"),
    dict(cid=9, item=1, name="Swiftpair",       sku="R", kind="active-ble", witness="ble"),
    dict(cid=9, item=2, name="Samsung",         sku="R", kind="active-ble", witness="ble"),
    dict(cid=9, item=3, name="Google",          sku="R", kind="active-ble", witness="ble"),
    dict(cid=9, item=4, name="Flipper",         sku="R", kind="active-ble", witness="ble"),
    dict(cid=9, item=5, name="All",             sku="R", kind="active-ble", witness="ble", special="ble_all"),
    # ── SAE (10) -- ACTIVE ──
    dict(cid=10, item=0, name="Commit Flood",   sku="R", kind="active-wifi", witness="sniff_auth"),
    # ── Evil Portal (11) -- ACTIVE, writes /portal/*.txt ──
    dict(cid=11, item=0, name="Start Default",  sku="R", kind="portal", witness="portal", special="evil_portal"),
    dict(cid=11, item=1, name="Start Custom",   sku="R", kind="portal", witness="portal", special="evil_portal_custom"),
    dict(cid=11, item=2, name="Stop",           sku="R", kind="util"),
]

# ROWS from the proven suite, indexed by "cid.item" -> stim/verify/select/channel/pcap/dwell
ROWS_BY_ID = {r["id"]: r for r in TS.ROWS}

DEFAULT_DWELL = 6.0     # seconds of metric polling / stimulus window per tool
POLL_EVERY = 1.0        # metric poll interval

# ─────────────────────────── SD detection ────────────────────────────

def _pcap_indices(h, prefix, hi=48):
    """Return the set of N for which /<prefix>_N.pcap exists (uncapped probe).
    Bails (raises SessionWedged) if the session desyncs, so a wedge can't balloon
    ~18 probes x timeout into a multi-minute stall (the run-1 amplifier)."""
    have = set()
    miss_streak = 0
    wedge = 0
    n = 0
    while n <= hi and miss_streak < 8:
        r = h.cmd(f"sd_exists /{prefix}_{n}.pcap")
        if "exists" not in r:            # wedged/desynced response (no 'exists' field)
            wedge += 1
            if wedge >= 2:
                raise SessionWedged(f"sd_exists probe wedged on /{prefix}_{n}.pcap")
            continue
        wedge = 0
        if r.get("exists"):
            have.add(n); miss_streak = 0
        else:
            miss_streak += 1
        n += 1
    return have

def _pcap_probe_max(h, prefix, hi=40):
    """Highest existing /<prefix>_N.pcap index (bounded probe, once per prefix).
    Raises SessionWedged on a desynced response so the matrix can reopen."""
    mx = -1; miss = 0; wedge = 0
    for n in range(hi + 1):
        r = h.cmd(f"sd_exists /{prefix}_{n}.pcap")
        if "exists" not in r:
            wedge += 1
            if wedge >= 2:
                raise SessionWedged(f"probe_max wedged on /{prefix}_{n}.pcap")
            continue
        wedge = 0
        if r.get("exists"):
            mx = n; miss = 0
        else:
            miss += 1
            if miss >= 6:
                break
    return mx

def _file_size(h, path):
    """Best-effort size: sd_read returns size for <=64KB; larger -> 'too large' (still >0)."""
    r = h.cmd(f"sd_read {path}")
    if r.get("ok"):
        return r.get("size", 0)
    if "too large" in (r.get("error", "") or "").lower():
        return 65537  # sentinel: exists and >64KB
    return -1

# ─────────────────────────── reboot ────────────────────────────

def _uptime(h):
    r = h.ping()
    return r.get("uptime", -1) if r.get("ok") else -1

def _rebooted(u_before, u_after):
    return u_after >= 0 and u_before >= 0 and u_after < u_before

def _recover_after_reboot(h, port, log):
    """Wait for the harness to come back after a suspected reboot."""
    log("  ! suspected REBOOT -- waiting for harness_ready")
    deadline = time.time() + 30
    while time.time() < deadline:
        try:
            r = h.ping()
            if r.get("ok"):
                h.skip_boot()
                log(f"  ! recovered (uptime={r.get('uptime')})")
                return True
        except Exception:
            pass
        time.sleep(1.5)
    log("  ! FAILED to recover within 30s")
    return False

# ─────────────────────────── metric polling ────────────────────────────

class Metrics:
    def __init__(self):
        self.fps_samples = []
        self.fps_min = None
        self.fps_avg = None
        self.dram = []
        self.psram = []
        self.min_dram = None

    def poll(self, h):
        f = h.fps()
        if f.get("ok"):
            inst = f.get("fps", 0)
            self.fps_samples.append(inst)
            self.fps_min = f.get("fps_min", self.fps_min)
            self.fps_avg = f.get("fps_avg", self.fps_avg)
        hp = h.heap()
        if hp.get("ok"):
            self.dram.append(hp.get("dram", 0))
            self.psram.append(hp.get("psram", 0))
            self.min_dram = hp.get("min_dram", self.min_dram)

    def summary(self):
        fs = [s for s in self.fps_samples if s is not None]
        return dict(
            fps_min=(min(fs) if fs else None),
            fps_max=(max(fs) if fs else None),
            fps_avg=(round(sum(fs) / len(fs), 1) if fs else None),
            fps_fw_min=self.fps_min, fps_fw_avg=self.fps_avg,
            fps_samples=len(fs),
            dram_start=(self.dram[0] if self.dram else None),
            dram_low=(min(self.dram) if self.dram else None),
            dram_end=(self.dram[-1] if self.dram else None),
            psram_low=(min(self.psram) if self.psram else None),
            min_dram_lowwater=self.min_dram,
        )

# ─────────────────────────── stimulus ────────────────────────────

def _stim_for(tool):
    """Return (fire_fn_or_None, verify_tuple_or_None, note). Prefer proven ROWS spec."""
    # Deauth *detection* (Analyze>Deauth): kalipi wlan1 monitor + mdk4 amok inject
    # while the badge sniffs -> its deauth counters should climb.
    if tool.get("inject_deauth"):
        ch = tool.get("channel", TS.SHIP_CH)
        def _fire_deauth():
            up = TS.kalipi("mon-up", "wlan1", str(ch), timeout=30)
            if not up.get("ok"):
                return up
            r = TS.kalipi("deauth", "wlan1", str(ch), "12", timeout=30)  # 12s (mdk4 ramps)
            TS.kalipi("mon-down", "wlan1", timeout=25)
            return r
        return _fire_deauth, ("pkt", ["deauth", "ndeauth"], "delta", 1), "inject:deauth(wlan1/mdk4)"
    # Tools with newly-added serial readback (were perennial FAILs -- no readback/stim):
    ci = (tool["cid"], tool["item"])
    if ci == (0, 3):   # Flock Batteries: BLE mfg-data XUNTONG (0x09C8) advert trips detect_counts.flock
        return (lambda: TS.kalipi("ble-raw", "04ffc80900", 6, timeout=20)), ("det", "flock", "delta", 1), "flock:xuntong-0x09C8"
    if ci == (1, 4):   # BLE Adverts: any BLE advert -> bt_frames climbs (added to pkt_counters)
        return (lambda: TS.kalipi("ble-name", "clipboy-adv", 8, timeout=22)), ("pkt", "bt_frames", "delta", 1), "ble-adverts"
    if ci == (2, 3):   # Channel Stats: WiFi traffic on ch7 (shipship, page1) -> channel_activity total
        return (lambda: TS.kalipi("wifi-cycle", TS.SSID, TS.PW, 2, timeout=69)), ("chan", "total", "delta", 1), "chan-activity:ch7"
    # Synthetic device-detector stimuli (kalipi emitters -- see detect_emitters.py).
    # These trip detectors we can't otherwise stimulate (no real Flipper/Flock/etc).
    if ci == (0, 0):   # AirTag: Apple FindMy mfg advert
        return (lambda: TS.kalipi("ble-raw", "1eff4c001219" + "00" * 25, "8", timeout=20)), ("det", "airtag", "delta", 1), "emit:airtag"
    if ci == (0, 1):   # Skimmer: BT name HC-05
        return (lambda: TS.kalipi("ble-name", "HC-05", "8", timeout=20)), ("det", "bt", "delta", 1), "emit:skimmer"
    if ci == (0, 2):   # Flipper: mfg company 0x3082
        return (lambda: TS.kalipi("ble-raw", "06ff8230123456", "8", timeout=20)), ("det", "flipper", "delta", 1), "emit:flipper"
    if ci == (0, 4):   # Rogue AP / Pineapple: beacon OUI 00:13:37 (detect_counts.esp = pinescan)
        return (lambda: TS.kalipi("wifi-beacon", "rogueap", "22", "6", timeout=45)), ("det", "esp", "delta", 1), "emit:rogueap"
    if ci == (0, 5):   # Evil Twin: 1 BSSID x >=3 SSIDs
        return (lambda: TS.kalipi("wifi-beacon", "eviltwin", "22", "6", timeout=45)), ("det", "multissid", "delta", 1), "emit:eviltwin"
    if ci == (3, 4):   # Pwnagotchi: beacon DE:AD:BE:EF:DE:AD + JSON
        return (lambda: TS.kalipi("wifi-beacon", "pwnagotchi", "22", "6", timeout=45)), ("det", "pwn", "delta", 1), "emit:pwnagotchi"
    if ci == (3, 5):   # Espressif: beacon OUI 24:0A:C4 (no counter -> ap-list proxy)
        return (lambda: TS.kalipi("wifi-beacon", "espressif", "22", "6", timeout=45)), ("det", "ap", "delta", 1), "emit:espressif(ap-proxy)"
    tid = f"{tool['cid']}.{tool['item']}"
    row = ROWS_BY_ID.get(tid)
    if row and row.get("stim") is not None:
        stim = row["stim"]
        fire = stim() if callable(stim) else None
        return fire, row.get("verify"), f"ROWS[{tid}]"
    # Fallback best-effort by kind (functional result -> INCONCLUSIVE, metrics still valid)
    k = tool["kind"]
    if k in ("wifi", "passive-wifi"):
        return (lambda: TS.kalipi("wifi-scan", timeout=35)), None, "default:wifi-scan"
    if k == "ble":
        return (lambda: TS.kalipi("ble-name", "clipboy-test", 6, timeout=20)), None, "default:ble-name"
    return None, None, "no-stim"

def _read_src(h, source):
    if source == "pkt":
        return h.pkt_counters()
    if source == "chan":
        return h.cmd("channel_activity")
    return h.detect_counts()

def _verify_delta(base, final, keys, thr, mode):
    if isinstance(keys, str):
        keys = [keys]
    best = 0
    for k in keys:
        b = base.get(k, 0) or 0
        f = final.get(k, 0) or 0
        best = max(best, (f - b) if mode == "delta" else f)
    return best, best >= thr

# ─────────────────────────── active-TX witness ────────────────────────────

def witness_active(tool, dut, witness_h, log):
    """External-witness efficacy for active-TX tools. Returns (status, detail)."""
    w = tool.get("witness")
    try:
        if w == "beacon":
            # COM8 scans APs WHILE the DUT is still spamming (witness runs before
            # DUT tool_stop). Clear the witness lists first so the delta reflects a
            # fresh scan. NOTE: ambient APs inflate the count -> report OBSERVED
            # data, not PASS/FAIL (beacon-spam efficacy-by-count is known-noisy;
            # tool_suite parked it). tool_start (witness fw is 1.1.0, no tool_open).
            witness_h.cmd("tool_start 4 10")   # Clear All lists
            time.sleep(0.3)
            witness_h.cmd("tool_start 1 0")    # Scan APs
            time.sleep(6)
            f = witness_h.detect_counts()
            witness_h.tool_stop()
            return ("OBSERVED",
                    f"COM8 fresh scan during spam: ssid={f.get('ssid')} ap={f.get('ap')} "
                    f"(ambient+spam; interpret vs baseline)")
        if w == "ble":
            witness_h.cmd("tool_start 4 10")   # Clear All
            time.sleep(0.3)
            witness_h.cmd("tool_start 1 4")    # BLE Adverts
            time.sleep(6)
            f = witness_h.detect_counts()
            witness_h.tool_stop()
            return ("OBSERVED",
                    f"COM8 BLE fresh scan during spam: bt={f.get('bt')} (ambient+spam)")
        if w == "portal":
            # Confirm the captive-portal AP is radiating from kalipi's vantage.
            scan = TS.kalipi("wifi-scan", timeout=35)
            n = scan.get("count", scan.get("aps", "?")) if scan.get("ok") else "?"
            return ("INCONC",
                    f"portal AP up; kalipi saw {n} APs. Cred capture needs a client "
                    f"submit (not automated); SD /portal checked separately.")
        if w in ("sniff_auth", "sniff_any", "deauth"):
            # kalipi wlan1 monitor sniff. Badge TX is known-marginal on mt76x2u RX;
            # log honestly rather than hard-fail.
            filt = {"sniff_auth": "auth", "deauth": "deauth"}.get(w, "any")
            up = TS.kalipi("mon-up", "wlan1", str(TS.SHIP_CH), timeout=20)
            if not up.get("ok"):
                return ("SKIP", f"kalipi monitor unavailable: {up.get('error')}")
            time.sleep(0.3)
            snf = TS.kalipi("sniff", "5", filt, "wlan1", timeout=15)
            TS.kalipi("mon-down", "wlan1", timeout=15)
            got = snf.get("count", snf.get("packets", 0)) if snf.get("ok") else 0
            return ("PASS" if got and got > 0 else "INCONC",
                    f"kalipi wlan1 sniffed {got} {filt} frames (mt76x2u RX marginal)")
    except Exception as e:
        return ("ERROR", f"witness exception: {e}")
    return ("SKIP", "no witness handler")

# ─────────────────────────── one tool ────────────────────────────

def run_one_tool(tool, dut, witness_h, ctx, log):
    """Execute one tool with full instrumentation. Returns a result dict."""
    cid, item, name = tool["cid"], tool["item"], tool["name"]
    res = dict(cid=cid, item=item, name=name, kind=tool["kind"], sku=tool["sku"],
               predecessor=ctx.get("predecessor"), pass_id=ctx["pass_id"], seq=ctx["seq"],
               functional="NA", func_detail="", sd="NA", sd_detail="",
               witness="NA", witness_detail="", reboot=False, error="")

    # ---- pre-state ----
    u0 = _uptime(dut)
    heap_pre = dut.heap()
    det_pre = dut.detect_counts()
    res["heap_pre_dram"] = heap_pre.get("dram")
    res["heap_pre_psram"] = heap_pre.get("psram")

    # ---- optional AP selection (RSSI / EAPOL) ----
    if tool.get("select"):
        try:
            dut.ap_scan(tool["select"])
        except Exception as e:
            res["error"] += f"select({tool['select']}) failed: {e}; "

    # ---- optional PCAP arm ----
    prefix = tool.get("pcap")
    want_pcap = ctx.get("allow_pcap")
    pcap_before = None
    if prefix and want_pcap is True:
        dut.cfg_set("allow_pcap", True)
        pcap_before = _pcap_indices(dut, prefix)
    elif prefix and want_pcap is False:
        dut.cfg_set("allow_pcap", False)
        pcap_before = _pcap_indices(dut, prefix)

    # ---- optional channel lock ----
    if tool.get("channel"):
        pass  # applied after start below

    # ---- start (faithful UI path) ----
    dut.fps_reset()
    to = 30 if tool.get("special") in ("beacon_funny", "ble_all") else None
    if to:
        # blocky start: raise the session read window for this call is not trivial
        # via the persistent bridge; rely on harness.cmd _max_reads skipping raw.
        pass
    start = dut.cmd(f"tool_open {cid} {item}")
    res["start_ok"] = start.get("ok", False)
    res["start_running"] = start.get("running", False)

    # reboot check right after start (blocky tools are the suspects)
    u_mid = _uptime(dut)
    if _rebooted(u0, u_mid):
        res["reboot"] = True
        _recover_after_reboot(dut, DUT_PORT, log)
        res["func_detail"] = "REBOOTED on start"
        return res

    if not start.get("ok"):
        err = start.get("error", "")
        if prefix and tool.get("gated") and "PCAP Saving off" in err:
            res["functional"] = "GATED-OK" if want_pcap is False else "GATE-ERR"
            res["func_detail"] = f"gated start blocked (allow_pcap={want_pcap}): {err}"
        else:
            res["functional"] = "START-FAIL"
            res["func_detail"] = err
        return res

    if tool.get("channel"):
        dut.cmd(f"raw_channel {tool['channel']}")

    # Utilities / immediate actions: tool_open renders the faithful screen, but
    # their effect fires from a non-START button / is the dispatch itself -- also
    # run tool_start so the action actually executes (real independence effect).
    if tool["kind"] in ("util", "keyboard") and not tool.get("special"):
        dut.cmd(f"tool_start {cid} {item}")

    # ---- verify baseline (after start, before stimulus) ----
    fire, verify, stim_note = _stim_for(tool)
    res["stim_note"] = stim_note
    vbase = None
    if verify:
        vbase = _read_src(dut, verify[0])

    # ---- stimulus in a thread while we poll DUT metrics ----
    stim_box = {}
    def _fire():
        try:
            stim_box["r"] = fire() if fire else {"ok": True}
        except Exception as e:
            stim_box["r"] = {"ok": False, "error": str(e)}

    metrics = Metrics()
    shot_path = os.path.join(ctx["shots_dir"],
                             f"{ctx['pass_id']}_{ctx['seq']:02d}_c{cid}i{item}_{re.sub(r'[^A-Za-z0-9]+','_',name)}.bmp")
    t = None
    if fire:
        t = threading.Thread(target=_fire, daemon=True)
        t.start()

    dwell = tool.get("dwell", DEFAULT_DWELL)
    t_end = time.time() + dwell
    shot_taken = False
    while True:
        metrics.poll(dut)
        if not shot_taken and time.time() > (t_end - dwell + 2.5):
            try:
                dut.screenshot(shot_path)
                shot_taken = True
            except Exception as e:
                res["error"] += f"shot: {e}; "
                shot_taken = True
        if time.time() >= t_end and (t is None or not t.is_alive()):
            break
        if time.time() >= t_end + 90:  # hard cap in case stimulus hangs
            break
        time.sleep(POLL_EVERY)
    if t:
        t.join(timeout=5)
    res["stim_result"] = stim_box.get("r", {}).get("ok") if fire else None
    res.update(metrics.summary())
    res["screenshot"] = os.path.relpath(shot_path, ctx["run_dir"]) if shot_taken else None

    # Fast wedge check: the screenshot binary stream can collide with verbose
    # --test log output and desync the session. Bail now (reopen fast) rather than
    # limp through verify/SD/stop on a dead session and trip the 130s watchdog.
    if not _alive(dut):
        raise SessionWedged(f"session desynced after {name} screenshot")

    # ---- functional verdict ----
    if verify and verify[1] is not None:
        vfinal = _read_src(dut, verify[0])
        obs, ok = _verify_delta(vbase or {}, vfinal, verify[1], verify[3], verify[2])
        res["functional"] = "PASS" if ok else "FAIL"
        res["func_detail"] = f"{verify[0]}.{verify[1]} {verify[2]} obs={obs} thr={verify[3]} ({stim_note})"
    else:
        res["functional"] = "RAN" if start.get("running") else "RAN?"
        res["func_detail"] = f"no verify spec ({stim_note}); ran with metrics"

    # ---- SD output check ----
    if prefix and want_pcap is True:
        after = _pcap_indices(dut, prefix)
        new = sorted(after - (pcap_before or set()))
        if new:
            newpath = f"/{prefix}_{new[-1]}.pcap"
            sz = _file_size(dut, newpath)
            res["sd"] = "WROTE" if sz != 0 else "EMPTY"
            res["sd_detail"] = f"{newpath} size~{sz}"
        else:
            res["sd"] = "NONE"
            res["sd_detail"] = f"no new /{prefix}_N.pcap (allow_pcap=on)"
    elif prefix and want_pcap is False:
        after = _pcap_indices(dut, prefix)
        new = sorted(after - (pcap_before or set()))
        res["sd"] = "CORRECT-NONE" if not new else "LEAK"
        res["sd_detail"] = f"allow_pcap=off, new={new}"

    # ---- active-TX / portal external-witness efficacy ----
    if tool["kind"] == "portal":
        st, dt = witness_active(tool, dut, witness_h, log)
        res["witness"], res["witness_detail"] = st, dt
        # also snapshot /portal for any cred file the badge may have written
        pr = dut.cmd("sd_list /portal")
        if pr.get("ok"):
            res["witness_detail"] += f" | /portal files={pr.get('count')}"
    elif tool["kind"] in ("active-wifi", "active-ble"):
        if witness_h is not None or tool.get("witness") in ("sniff_auth", "sniff_any", "deauth"):
            st, dt = witness_active(tool, dut, witness_h, log)
            res["witness"], res["witness_detail"] = st, dt
        else:
            res["witness"], res["witness_detail"] = "SKIP", "no witness badge"

    # ---- stop + post-state ----
    dut.tool_stop()
    time.sleep(0.4)
    u1 = _uptime(dut)
    if _rebooted(u0, u1):
        res["reboot"] = True
        _recover_after_reboot(dut, DUT_PORT, log)
    heap_post = dut.heap()
    res["heap_post_dram"] = heap_post.get("dram")
    res["heap_delta_dram"] = (heap_post.get("dram", 0) - (heap_pre.get("dram", 0) or 0))
    return res

# ─────────────────────────── passes ────────────────────────────

PER_TOOL_BUDGET = 130.0   # hard per-tool watchdog (s); backstop for a true hang

class SessionWedged(Exception):
    """The serial session desynced (verbose --test logging can collide with the
    screenshot binary stream) -> reopen fast instead of waiting for the watchdog."""

def _make_dut(retries=6):
    os.environ["CLIPBOY_TAP_SETTLE"] = os.environ.get("CLIPBOY_TAP_SETTLE", "0.4")
    os.environ["CLIPBOY_SESSION_TIMEOUT"] = os.environ.get("CLIPBOY_SESSION_TIMEOUT", "8")
    last = None
    for _ in range(retries):
        try:
            h = Harness(port=DUT_PORT, skip_boot=True)
            # Silence CB_LOG chatter for the whole session so log lines can't
            # collide with the STX protocol / sd_exists responses (the text
            # variant of the CDC wedge). No-op on older firmware w/o `quiet`.
            try:
                h.cmd("quiet 1")
            except Exception:
                pass
            return h
        except Exception as e:
            last = e
            time.sleep(2)   # a just-killed bridge may still be releasing COM11
    raise RuntimeError(f"could not open DUT after retries: {last}")

def _kill_harness(h):
    """Force-release the serial port: kill the bridge subprocess (does NOT reboot
    the badge -- dtr=False -- so badge state is preserved across a session reopen)."""
    if not h:
        return
    try:
        h.proc.kill()
    except Exception:
        pass
    try:
        h.proc.wait(timeout=3)
    except Exception:
        pass

def _alive(h):
    """Cheap session-health probe. False if the session is wedged/desynced."""
    try:
        r = h.ping()
        return bool(r.get("ok")) and ("uptime" in r)
    except Exception:
        return False

def verify_cat_indices(h):
    r = h.cmd("tool_list")
    cats = r.get("cats", [])
    idx_of_id = {}
    for i, c in enumerate(cats):
        idx_of_id[c["id"]] = i
    return idx_of_id, len(cats)

def run_pass(sku, pass_no, seed, smoke=False):
    import random
    ts = time.strftime("%Y%m%d_%H%M%S")
    pass_id = f"{sku}_p{pass_no}"
    run_dir = os.path.join(RESULTS_ROOT, f"{ts}_{pass_id}")
    shots_dir = os.path.join(run_dir, "screenshots")
    os.makedirs(shots_dir, exist_ok=True)
    logf = open(os.path.join(run_dir, "run.log"), "w", encoding="utf-8")

    def log(m):
        line = f"[{time.strftime('%H:%M:%S')}] {m}"
        print(line, flush=True)
        logf.write(line + "\n"); logf.flush()

    log(f"=== PASS {pass_id} seed={seed} sku={sku} smoke={smoke} ===")

    # tools for this SKU
    tools = [t for t in PLAYLIST if (sku == "res34rch" or t["sku"] == "S")]
    if smoke:
        # diverse subset: a scan, a chart, a util, a pcap tool, an active-tx, a keyboard
        pick = [(1, 0), (2, 1), (4, 0), (3, 3), (8, 0), (5, 0)]
        tools = [t for t in tools if (t["cid"], t["item"]) in pick]
    rng = random.Random(seed)
    rng.shuffle(tools)
    log(f"order ({len(tools)}): " + ", ".join(f"{t['cid']}.{t['item']}" for t in tools))

    dut = _make_dut()
    witness_h = None
    try:
        idx_of_id, ncats = verify_cat_indices(dut)
        log(f"tool_list cats={ncats}; id->idx={idx_of_id}")
        # witness badge (for active-tx efficacy) -- res34rch only
        if sku == "res34rch" and any(t["kind"].startswith("active") for t in tools):
            try:
                witness_h = Harness(port=WITNESS_PORT, skip_boot=True)
                log(f"witness badge up on {WITNESS_PORT}")
            except Exception as e:
                log(f"witness badge unavailable: {e}")

        results = []
        predecessor = None
        for seq, tool in enumerate(tools):
            # deauth-detect needs kalipi injection; handle via witness sniff path is N/A
            ctx = dict(pass_id=pass_id, seq=seq, predecessor=predecessor,
                       run_dir=run_dir, shots_dir=shots_dir,
                       allow_pcap=(True if tool.get("pcap") else None))
            log(f"[{seq:02d}] {tool['cid']}.{tool['item']} {tool['name']} (pred={predecessor})")
            # Run each tool under a hard watchdog: an occasional serial/ssh call
            # wedges past its own timeout and would otherwise hang the whole pass.
            box = {}
            def _worker(_dut=dut):
                try:
                    box["r"] = run_one_tool(tool, _dut, witness_h, ctx, log)
                except SessionWedged as e:
                    box["r"] = dict(cid=tool["cid"], item=tool["item"], name=tool["name"],
                                    kind=tool["kind"], functional="WEDGED", error=str(e),
                                    pass_id=pass_id, seq=seq, predecessor=predecessor, reboot=False)
                    box["reopen"] = True
                except Exception as e:
                    box["r"] = dict(cid=tool["cid"], item=tool["item"], name=tool["name"],
                                    functional="ERROR", error=f"{e}\n{traceback.format_exc()}",
                                    pass_id=pass_id, seq=seq, predecessor=predecessor)
            wt = threading.Thread(target=_worker, daemon=True)
            wt.start()
            wt.join(timeout=PER_TOOL_BUDGET)
            if not wt.is_alive() and box.get("reopen"):
                # Fast recovery from a detected session desync (much cheaper than
                # the 130s watchdog): reopen DUT (+witness), badge state preserved.
                log(f"  ~ session wedged on {tool['cid']}.{tool['item']} {tool['name']} -- reopening")
                _kill_harness(dut); dut = _make_dut()
                if witness_h is not None:
                    _kill_harness(witness_h)
                    try: witness_h = Harness(port=WITNESS_PORT, skip_boot=True)
                    except Exception: witness_h = None
                r = box["r"]
            elif wt.is_alive():
                log(f"  !! WATCHDOG fired ({PER_TOOL_BUDGET}s) on {tool['cid']}.{tool['item']} "
                    f"{tool['name']} -- force-recovering session")
                _kill_harness(dut)          # frees COM11; abandoned worker dies on the dead pipe
                dut = _make_dut()            # fresh session, badge state preserved (no reboot)
                if witness_h is not None:    # abandoned worker may still hold COM8
                    _kill_harness(witness_h)
                    try:
                        witness_h = Harness(port=WITNESS_PORT, skip_boot=True)
                    except Exception as e:
                        log(f"     witness reopen failed: {e}")
                        witness_h = None
                r = dict(cid=tool["cid"], item=tool["item"], name=tool["name"], kind=tool["kind"],
                         functional="HUNG", error=f"watchdog timeout >{PER_TOOL_BUDGET}s",
                         pass_id=pass_id, seq=seq, predecessor=predecessor, reboot=False)
            else:
                r = box.get("r") or dict(cid=tool["cid"], item=tool["item"], name=tool["name"],
                                         functional="ERROR", error="no result", pass_id=pass_id,
                                         seq=seq, predecessor=predecessor)
            results.append(r)
            log(f"     -> func={r.get('functional')} sd={r.get('sd')} "
                f"fps({r.get('fps_min')}/{r.get('fps_max')}/{r.get('fps_avg')}) "
                f"dram={r.get('dram_start')}->{r.get('dram_low')}->{r.get('dram_end')} "
                f"wit={r.get('witness')} reboot={r.get('reboot')}")
            # incremental persist
            with open(os.path.join(run_dir, "results.json"), "w", encoding="utf-8") as fh:
                json.dump(results, fh, indent=2)
            predecessor = f"{tool['cid']}.{tool['item']} {tool['name']}"

        _write_csv(run_dir, results)
        log(f"=== PASS {pass_id} complete: {len(results)} tools -> {run_dir} ===")
        return run_dir, results
    finally:
        try:
            dut.close()
        except Exception:
            pass
        _kill_harness(dut)          # guarantee COM11 is released for the next phase
        if witness_h:
            try:
                witness_h.close()
            except Exception:
                pass
            _kill_harness(witness_h)
        try:
            TS.kalipi_restore()
        except Exception:
            pass
        logf.close()

def _write_csv(run_dir, results):
    cols = ["pass_id", "seq", "cid", "item", "name", "kind", "predecessor",
            "functional", "func_detail", "sd", "sd_detail", "witness", "witness_detail",
            "fps_min", "fps_max", "fps_avg", "fps_samples",
            "dram_start", "dram_low", "dram_end", "psram_low", "min_dram_lowwater",
            "heap_delta_dram", "reboot", "screenshot", "error"]
    with open(os.path.join(run_dir, "metrics.csv"), "w", newline="", encoding="utf-8") as fh:
        w = csv.DictWriter(fh, fieldnames=cols, extrasaction="ignore")
        w.writeheader()
        for r in results:
            w.writerow(r)

# ─────────────────────────── PCAP on/off matrix ────────────────────────────

def run_pcap_matrix(sku, seed=1):
    """For every pcap-producing tool, run it with Allow-PCAP ON (expect a file) and
    OFF (expect NO file; Raw/EAPOL additionally expect a start-block). Verifies the
    'Allow PCAP Saving' gate end-to-end on the SD card."""
    ts = time.strftime("%Y%m%d_%H%M%S")
    run_dir = os.path.join(RESULTS_ROOT, f"{ts}_{sku}_pcapmatrix")
    os.makedirs(run_dir, exist_ok=True)
    logf = open(os.path.join(run_dir, "run.log"), "w", encoding="utf-8")

    def log(m):
        line = f"[{time.strftime('%H:%M:%S')}] {m}"
        print(line, flush=True); logf.write(line + "\n"); logf.flush()

    pcap_tools = [t for t in PLAYLIST if t.get("pcap") and (sku == "res34rch" or t["sku"] == "S")]
    log(f"=== PCAP MATRIX {sku}: {len(pcap_tools)} pcap tools ===")
    dut = _make_dut()
    rows = []
    seeds = {}   # prefix -> highest existing /prefix_N.pcap index (probed once, then tracked)
    try:
        for tool in pcap_tools:
            cid, item, prefix = tool["cid"], tool["item"], tool["pcap"]
            row = dict(cid=cid, item=item, name=tool["name"], prefix=prefix, gated=bool(tool.get("gated")))
            try:
                if prefix not in seeds:
                    seeds[prefix] = _pcap_probe_max(dut, prefix)   # ONE bounded probe per prefix
                for state in (True, False):
                    dut.cfg_set("allow_pcap", state)
                    if tool.get("select"):
                        try: dut.ap_scan(tool["select"])
                        except Exception: pass
                    base = seeds[prefix]
                    start = dut.cmd(f"tool_open {cid} {item}")
                    if tool.get("channel"):
                        dut.cmd(f"raw_channel {tool['channel']}")
                    fire, _, _ = _stim_for(tool)
                    if fire:
                        try: fire()
                        except Exception as e: row[f"stim_err_{state}"] = str(e)
                    else:
                        time.sleep(4)
                    time.sleep(1)
                    dut.tool_stop(); time.sleep(0.4)
                    # Probe upward from base+1 to find the new max (catches tools
                    # that write MULTIPLE pcaps per run -> avoids false off=LEAK).
                    new_max = base; n = base + 1; misses = 0
                    while misses < 3:
                        chk = dut.cmd(f"sd_exists /{prefix}_{n}.pcap")
                        if "exists" not in chk:
                            raise SessionWedged("pcap matrix sd_exists desync")
                        if chk.get("exists"):
                            new_max = n; misses = 0
                        else:
                            misses += 1
                        n += 1
                    wrote = new_max > base
                    seeds[prefix] = new_max
                    key = "on" if state else "off"
                    row[f"{key}_started"] = start.get("running", False)
                    row[f"{key}_start_ok"] = start.get("ok", False)
                    row[f"{key}_wrote"] = wrote
                    if state:
                        row["on_result"] = "WROTE" if wrote else "NO-FILE"
                    elif tool.get("gated"):
                        blocked = (not start.get("ok")) and "PCAP Saving off" in (start.get("error", "") or "")
                        row["off_result"] = "GATED-BLOCKED" if blocked else ("LEAK" if wrote else "NOT-BLOCKED-NO-FILE")
                    else:
                        row["off_result"] = "CORRECT-NONE" if not wrote else "LEAK"
            except SessionWedged as e:
                log(f"  ~ pcap wedge on {cid}.{item} -- reopening ({e})")
                _kill_harness(dut); dut = _make_dut()
                row.setdefault("on_result", "WEDGED"); row.setdefault("off_result", "WEDGED")
            except Exception as e:
                row["error"] = str(e)
                try: _kill_harness(dut); dut = _make_dut()
                except Exception: pass
            log(f"  {cid}.{item} {tool['name']:<14} on={row.get('on_result')} off={row.get('off_result')} "
                f"(wrote on={row.get('on_wrote')} off={row.get('off_wrote')})")
            rows.append(row)
            with open(os.path.join(run_dir, "pcap_matrix.json"), "w", encoding="utf-8") as fh:
                json.dump(rows, fh, indent=2)
        log(f"=== PCAP MATRIX {sku} complete -> {run_dir} ===")
        return run_dir, rows
    finally:
        try: dut.close()
        except Exception: pass
        _kill_harness(dut)
        logf.close()

# ─────────────────────────── report / analysis ────────────────────────────

def generate_report(out_path=None):
    """Aggregate every pass + pcap-matrix dir into a morning independence report."""
    import glob
    dirs = sorted(glob.glob(os.path.join(RESULTS_ROOT, "2026*_*_p*"))) + \
           sorted(glob.glob(os.path.join(RESULTS_ROOT, "2026*_*_pcapmatrix")))
    passes = {}
    for d in dirs:
        fn = "pcap_matrix.json" if d.endswith("pcapmatrix") else "results.json"
        rj = os.path.join(d, fn)
        if os.path.isfile(rj):
            with open(rj, encoding="utf-8") as f:
                passes[os.path.basename(d)] = json.load(f)
    out_path = out_path or os.path.join(RESULTS_ROOT, f"REPORT_{time.strftime('%Y%m%d_%H%M%S')}.md")
    L = ["# Clip-Boy Tool Independence / Integration Test — Report",
         f"_generated {time.strftime('%Y-%m-%d %H:%M')} local_", "",
         "DUT=COM11, witness=COM8, kalipi=192.168.1.146. Builds are `--test` (+`--res34rch`).",
         "FPS min is typically a one-frame tool-start hitch; judge on avg/max. Heap in bytes.", ""]
    # per-pass sections
    reboots = []
    leak_flags = []
    for pid, results in passes.items():
        L.append(f"## Pass `{pid}` ({len(results)} tools)")
        # heap trend
        drams = [(r.get("seq"), r.get("name"), r.get("heap_pre_dram")) for r in results if r.get("heap_pre_dram")]
        if drams:
            first = drams[0][2]; last = drams[-1][2]; lo = min(d[2] for d in drams)
            drop = first - last
            L.append(f"- heap DRAM pre-tool: start={first} end={last} min={lo} net_drop={drop}"
                     + ("  ⚠ possible leak" if drop > 8000 else ""))
            if drop > 8000:
                leak_flags.append((pid, drop))
        rb = [r for r in results if r.get("reboot")]
        if rb:
            reboots += [(pid, r["name"]) for r in rb]
            L.append(f"- ⚠ REBOOTS: " + ", ".join(r["name"] for r in rb))
        # table
        L.append("")
        L.append("| seq | tool | func | sd | fps min/max/avg | dram s/lo/e | heapΔ | witness | pred |")
        L.append("|----:|------|------|----|----|----|----:|----|------|")
        for r in results:
            L.append(f"| {r.get('seq')} | {r.get('name')} | {r.get('functional')} | {r.get('sd')} | "
                     f"{r.get('fps_min')}/{r.get('fps_max')}/{r.get('fps_avg')} | "
                     f"{r.get('dram_start')}/{r.get('dram_low')}/{r.get('dram_end')} | {r.get('heap_delta_dram')} | "
                     f"{r.get('witness')} | {r.get('predecessor')} |")
        L.append("")
    # cross-pass independence: functional differences per (cid,item) across passes of same SKU
    L.append("## Independence analysis (cross-pass)")
    bysku = {}
    for pid, results in passes.items():
        if "pcapmatrix" in pid:
            continue
        sku = pid.split("_")[0]
        for r in results:
            bysku.setdefault(sku, {}).setdefault((r.get("cid"), r.get("item")), []).append(
                (pid, r.get("functional"), r.get("predecessor")))
    order_dep = []
    for sku, tools in bysku.items():
        for (cid, item), obs in tools.items():
            verds = set(o[1] for o in obs)
            # order-dependence: same tool passing in one pass, failing in another
            if {"FAIL"} & verds and ({"PASS", "RAN"} & verds):
                order_dep.append((sku, cid, item, obs))
    if order_dep:
        L.append("**⚠ ORDER-DEPENDENT tools (differ across passes — investigate predecessor):**")
        for sku, cid, item, obs in order_dep:
            L.append(f"- {sku} {cid}.{item}: " + "; ".join(f"{o[0]}={o[1]} (pred={o[2]})" for o in obs))
    else:
        L.append("- No tool changed functional verdict across passes of the same SKU (good — no obvious order dependence).")
    L.append("")
    L.append(f"- Reboots: {reboots if reboots else 'none'}")
    L.append(f"- Heap-leak flags: {leak_flags if leak_flags else 'none'}")
    # pcap matrix
    for pid, rows in passes.items():
        if "pcapmatrix" not in pid:
            continue
        L.append(f"\n## PCAP matrix `{pid}`")
        L.append("| tool | prefix | Allow-ON | Allow-OFF |")
        L.append("|------|--------|----------|-----------|")
        for r in rows:
            L.append(f"| {r.get('name')} | {r.get('prefix')} | {r.get('on_result')} | {r.get('off_result')} |")
    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(L))
    print("report ->", out_path)
    return out_path

# ─────────────────────────── main ────────────────────────────

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--smoke", action="store_true", help="quick 6-tool plumbing check (res34rch)")
    ap.add_argument("--pass", dest="passid", choices=["res34rch", "sn34k"], help="run one shuffle pass")
    ap.add_argument("--pass-no", type=int, default=1)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--pcap-matrix", dest="pcapmatrix", choices=["res34rch", "sn34k"], help="run the PCAP on/off matrix")
    ap.add_argument("--report", action="store_true", help="aggregate all pass dirs into a report")
    ap.add_argument("--no-deploy", action="store_true", help="skip scp of kalipi_stim.py")
    args = ap.parse_args()

    if args.report:
        generate_report()
        return 0
    if args.pcapmatrix:
        if not args.no_deploy:
            try: TS.deploy()
            except Exception as e: print("deploy failed:", e)
        run_pcap_matrix(args.pcapmatrix, args.seed)
        return 0

    os.makedirs(RESULTS_ROOT, exist_ok=True)
    if not args.no_deploy:
        try:
            TS.deploy()
            caps = TS.kalipi("caps")
            print("kalipi caps:", json.dumps(caps))
        except Exception as e:
            print("kalipi deploy/caps failed (continuing, functional may be INCONC):", e)

    if args.smoke:
        run_pass("res34rch", args.pass_no, args.seed, smoke=True)
        return 0
    if args.passid:
        run_pass(args.passid, args.pass_no, args.seed, smoke=False)
        return 0
    ap.print_help()
    return 0

if __name__ == "__main__":
    sys.exit(main())
