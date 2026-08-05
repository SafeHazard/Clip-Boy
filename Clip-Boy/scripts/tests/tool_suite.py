#!/usr/bin/env python3
"""
tool_suite.py -- table-driven end-to-end test of Clip-Boy tools via kalipi.

Ties the badge serial harness (harness.py) to the kalipi RF stimulus lib
(kalipi_stim.py, run over ssh/eth0) using the coverage matrix in
docs/test-automation-kalipi.md. Per tool: start it on the badge, drive real RF from
kalipi, read the badge's signal back, assert, record PASS/FAIL.

Run on a --test --res34rch badge (all tools present). Category id == tool_categories[]
array index on res34rch, so tool_start takes the stable id directly.

Usage:
  py -3 scripts/tests/tool_suite.py                 # Tier 1 (default)
  py -3 scripts/tests/tool_suite.py --tier 1,2
  py -3 scripts/tests/tool_suite.py --only 3.7,1.0  # specific (cat.item)
  py -3 scripts/tests/tool_suite.py --list          # print the table, run nothing
  py -3 scripts/tests/tool_suite.py --no-deploy     # skip scp of kalipi_stim.py
Env: CLIPBOY_KALIPI (default 192.168.1.146), CLIPBOY_PORT (badge COM).
"""
import sys, os, time, json, argparse, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Harness

KALIPI = os.environ.get("CLIPBOY_KALIPI", "192.168.1.146")
# Second stimulus box (kalipi2b): a Pi 2B on ethernet with a USB CSR BLE dongle and an
# RTL8192CU. Needed because a single adapter cannot produce two SIMULTANEOUS distinct
# advertisers, and the newest-of-N selection paths have only ever run with N=1. Ethernet on
# both keeps the CONTROL channel out of the band under test -- do not "simplify" either to WiFi.
KALIPI2 = os.environ.get("CLIPBOY_KALIPI2", "192.168.1.113")
STIM_LOCAL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "kalipi_stim.py")
STIM_REMOTE = "/tmp/kalipi_stim.py"
SSID, PW = "shipship", "shipship"
# shipship's 2.4GHz channel. The AP does auto-channel-selection and ROAMS constantly
# (7 -> 11 -> 4 -> 2 all observed) -- a stale value makes the channel-locked rows
# (3.2 deauth-inject, 3.7 EAPOL) target an empty channel and false-fail. main() now
# AUTO-DETECTS the live channel from a kalipi scan (scan-ch) at startup and patches the
# rows; this is just the fallback if detection fails. Override: --ship-ch / CLIPBOY_SHIP_CH.
SHIP_CH = 11
MON = "wlan1"   # kalipi's A6210/mt76x2u monitor+injection adapter (wlan0 = station)

# ─── kalipi bridge (ssh over eth0) ──────────────────────────────────────────

def kalipi(*prim, timeout=60, host=None):
    """Run kalipi_stim.py <prim...> on a stimulus box; return parsed JSON (or {'ok':False}).

    host=None -> KALIPI (the primary). Pass host=KALIPI2 for the second advertiser. The 2B is a
    slower board, so give its calls more generous timeouts than the primary's.
    """
    cmd = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=12", f"data@{host or KALIPI}",
           "python3 " + STIM_REMOTE + " " + " ".join(str(a) for a in prim)]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        line = (r.stdout or "").strip().splitlines()
        return json.loads(line[-1]) if line else {"ok": False, "err": r.stderr[-120:]}
    except subprocess.TimeoutExpired:
        return {"ok": False, "err": "ssh-timeout"}
    except json.JSONDecodeError:
        return {"ok": False, "err": "bad-json", "raw": (r.stdout or r.stderr)[-160:]}


def deploy(host=None):
    """Push kalipi_stim.py to a stimulus box. Call once per host before using it."""
    subprocess.run(["scp", "-o", "BatchMode=yes", "-o", "ConnectTimeout=12",
                    STIM_LOCAL, f"data@{host or KALIPI}:{STIM_REMOTE}"],
                   capture_output=True, text=True, timeout=45)


def kalipi_restore():
    """Leave wlan0 associated to shipship + BLE idle after a run."""
    kalipi("mon-down", "wlan0", SSID, PW, timeout=45)


# ─── stimulus helpers (return a callable that fires the RF) ──────────────────

def st_none():        return lambda: {"ok": True}
def st_join():        return lambda: kalipi("wifi-join", SSID, PW, timeout=45)
def st_cycle(n=3):    return lambda: kalipi("wifi-cycle", SSID, PW, n, timeout=45 + 12 * n)
def st_scan():        return lambda: kalipi("wifi-scan", timeout=35)
def st_ble_name(nm):  return lambda: kalipi("ble-name", nm, 6, timeout=20)
def st_ble_raw(hx):   return lambda: kalipi("ble-raw", hx, 6, timeout=20)


# ─── verify: (source, key, mode, threshold) ─────────────────────────────────
# source: 'pkt' (pkt_counters) | 'det' (detect_counts) | 'kwifi'/'kble' (kalipi delta)
# mode:   'abs' (final >= thr) | 'delta' (final-base >= thr)

def read_src(h, src):
    if src == "pkt":  return h.pkt_counters()
    if src == "det":  return h.detect_counts()
    return {}


# ─── the coverage table ─────────────────────────────────────────────────────
# id = "cat.item"; sku S=Sn34k(passive) R=Res34rch(active); tier; name;
# stim() = RF to fire; verify = (source,key,mode,thr); notes/confidence.
# 'select' = ssid to select as target AP before start (RSSI/EAPOL).
# 'pcap'   = enable Allow PCAP before start.

ROWS = [
    # ── Tier 1: WiFi station-stimulus (pkt_counters / detect_counts) ────────
    {"id":"1.0","sku":"S","tier":1,"name":"Scan APs","stim":st_scan,
     "verify":("det","ap","abs",1),"dwell":5},
    # Station detection is window-sensitive (the tool hops channels; a brief join is
    # easy to miss) -> dwell so the badge sweeps ch7 several times while kalipi stays
    # associated and emitting frames. sta list persists across tool starts -> abs.
    {"id":"1.1","sku":"S","tier":1,"name":"APs + Stations","stim":st_join,
     "verify":("det","sta","abs",1),"dwell":7},
    {"id":"1.2","sku":"S","tier":1,"name":"Stations only","stim":st_join,
     "verify":("det","sta","abs",1),"dwell":7},
    {"id":"2.0","sku":"S","tier":1,"name":"Monitor frames","stim":st_scan,
     "verify":("pkt",["beacon","nbeacon","mgmt"],"delta",1)},
    {"id":"2.1","sku":"S","tier":1,"name":"Packet rate","stim":st_scan,
     "verify":("pkt","mgmt","delta",1)},
    # Channel histogram feeds only the per-channel activity array (getChannelActivity),
    # which isn't exposed over serial yet -> park until a channel-activity readback exists.
    {"id":"2.3","sku":"S","tier":11,"name":"Channel histogram","stim":st_scan,
     "verify":("pkt",["beacon","nbeacon","mgmt"],"delta",1),"conf":"needs channel-activity hook"},
    {"id":"2.4","sku":"S","tier":1,"name":"MAC tracker","stim":st_scan,
     "verify":("pkt","mgmt","delta",1)},
    # Analyze Beacons feeds neither *Frames nor num* -> it captures beacons into the
    # AP list, so assert the AP list is populated.
    {"id":"3.0","sku":"S","tier":1,"name":"Analyze Beacons","stim":st_scan,
     "verify":("det","ap","abs",1),"dwell":3},
    # Analyze Probes needs DIRECTED probe-requests (SSID-bearing) to fill the probe-SSID
    # list; kalipi's broadcast scan emits null-SSID probes -> needs a directed-probe primitive.
    {"id":"3.1","sku":"S","tier":11,"name":"Analyze Probes","stim":st_scan,
     "verify":("det","probe","delta",1),"conf":"needs directed-probe stimulus"},
    {"id":"3.3","sku":"S","tier":1,"name":"Raw / PCAP","stim":lambda:st_cycle(2),
     "verify":("pkt",["data","beacon","mgmt"],"delta",1),"pcap":True,"channel":SHIP_CH},
    {"id":"3.7","sku":"S","tier":1,"name":"EAPOL / PMKID","stim":lambda:st_cycle(3),
     "verify":("pkt","eapol","delta",1),"select":SSID,"channel":SHIP_CH,"pcap":True},
    # FINDING (DC34 follow-up): Beacon Spam tool_start BLOCKS the main loop ~7-10s
    # (SSID gen / transmit setup) -> exceeds the 5s bridge timeout (and would freeze
    # the UI / risk the WDT in normal use). The tool DOES start (kalipi sees the SSIDs).
    # Parked until the badge start is made non-blocking OR a longer per-tool timeout lands.
    {"id":"8.4","sku":"R","tier":11,"name":"Beacon Spam (funny)","stim":st_none,
     "verify":("kwifi",None,"delta",3),"conf":"badge: Beacon Spam start blocks ~7-10s > 5s timeout"},
    # ── Tier 1: BLE stimulus (detect_counts) ────────────────────────────────
    {"id":"1.3","sku":"S","tier":1,"name":"BT/BLE scan","stim":lambda:st_ble_name("clipboy-test"),
     "verify":("det","bt","delta",1)},
    # BLE advert rate is a packets/sec counter with no list/serial readback -> parked
    # until a rate accessor is exposed (the tool itself works; just not asserted here).
    {"id":"1.4","sku":"S","tier":11,"name":"BLE advert rate","stim":lambda:st_ble_name("clipboy-test"),
     "verify":("det","bt","delta",1),"conf":"needs a rate readback hook"},
    {"id":"0.1","sku":"S","tier":1,"name":"Skimmer detect","stim":lambda:st_ble_name("HC-05"),
     "verify":("det","bt","delta",1),"conf":"medium: bt-count proxy; refine to skimmer flag"},
    # AirTag/Flipper: adv bytes derived from the badge's detector (WiFiScan.cpp).
    # AirTag match = payload contains 1E FF 4C 00  OR  4C 00 12 19 (Apple FindMy).
    {"id":"0.0","sku":"S","tier":1,"name":"AirTag detect","stim":lambda:st_ble_raw("1eff4c001219"+"00"*25),
     "verify":("det","airtag","abs",1),"conf":"adv Apple FindMy 1EFF4C00..; refine on hw"},
    # Flipper match = payload contains an 8X 30 mfg-data pair (82 30 = white).
    {"id":"0.2","sku":"S","tier":1,"name":"Flipper detect","stim":lambda:st_ble_raw("06ff8230123456"),
     "verify":("det","flipper","abs",1),"conf":"adv mfg pair 82 30; refine on hw"},
    # Flock signature not yet extracted (service-UUID/name based) -> park until read.
    {"id":"0.3","sku":"S","tier":11,"name":"Flock detect","stim":st_none,
     "verify":("det","flock","abs",1),"conf":"TODO: grep WiFiScan flock match criteria"},
    # ── Tier 1: active tools verified by kalipi SCAN (no monitor mode) ───────
    # BLE Spam via ambient device-count delta is too noisy (rotating MACs + churn).
    # Park until kalipi ble-scan matches the spam's Apple/MS mfg-data specifically.
    {"id":"9.0","sku":"R","tier":11,"name":"BLE Spam (Sour Apple)","stim":st_none,
     "verify":("kble",None,"delta",1),"conf":"needs mfg-data-specific BLE match"},
    # ── Tier 2: monitor-mode via wlan1 (A6210/mt76x2u) ──────────────────────
    # Deauth DETECT: wlan1 INJECTS deauth (mdk4) -> the badge's detector counts them.
    # PROVEN: badge deauth counter +196. (12s window -- mdk4's amok rate ramps slowly.)
    {"id":"3.2","sku":"S","tier":2,"name":"Deauth detect + Geiger","stim":None,
     "verify":("inject",["deauth","ndeauth"],"delta",1),"channel":SHIP_CH,
     "conf":"wlan1 injects deauth; badge deauth counter climbs (+196 verified)"},
    # FINDING (parked): the badge's Deauth/Flood TRANSMIT was NOT observable via wlan1
    # monitor sniff -- 0 deauth frames even hopping 1/6/11/7, vs 196 counted the other way.
    # Root cause TBD: sparse/ineffective badge TX (worth a firmware look) OR a channel/
    # station subtlety. Resolve via the badge-to-badge axis (badge #2 as detector).
    {"id":"6.0","sku":"R","tier":11,"name":"Deauth (discovered)","stim":st_none,
     "verify":("sniff","deauth","abs",1),"prescan":True,"channel":SHIP_CH,
     "conf":"badge deauth TX not sniffable (0 frames); use badge-to-badge"},
    {"id":"7.0","sku":"R","tier":11,"name":"Flood (auth)","stim":st_none,
     "verify":("sniff","auth","abs",1),"prescan":True,"select":SSID,"channel":SHIP_CH,
     "conf":"badge auth-flood TX not sniffable; use badge-to-badge"},
    # SAE flood needs a WPA3-SAE AP as target (shipship is WPA2) -> parked (Tier-3 hostapd).
    {"id":"10.0","sku":"R","tier":11,"name":"SAE flood","stim":st_none,
     "verify":("sniff","auth","abs",1),"conf":"needs a WPA3-SAE target AP"},
]


def sel(rows, tiers, only):
    for r in rows:
        if only:
            if r["id"] in only: yield r
            continue
        if r["tier"] in tiers: yield r


def run_row(h, r):
    cat, item = (int(x) for x in r["id"].split("."))
    src, key, mode, thr = r["verify"]
    if src == "disconnect":
        return run_disconnect(h, r, cat, item, thr)
    # setup
    if r.get("prescan"):                  # populate the badge AP list before a transmit tool
        h.tool_start(1, 0); time.sleep(r.get("prescan_dwell", 5)); h.tool_stop()
    if r.get("pcap"):
        h.cfg_set("allow_pcap", True)
    if r.get("select"):
        h.ap_scan(r["select"])            # scan + select target AP
    start = h.tool_start(cat, item)
    if not start.get("ok"):
        return "SKIP", f"tool_start {cat}.{item}: {start.get('error') or start.get('err','?')}"
    if r.get("channel"):
        h.cmd(f"raw_channel {r['channel']}")
    time.sleep(1.0)

    # baseline for delta modes / kalipi-scan deltas
    base = read_src(h, src) if src in ("pkt", "det") else {}
    kbase = None
    if src == "kwifi": kbase = kalipi("wifi-scan").get("count", 0)
    if src == "kble":  kbase = kalipi("ble-scan", 6).get("count", 0)

    # fire stimulus
    detail = ""
    if src in ("sniff", "inject"):
        obs, detail = run_tier2(h, r, cat, item, src, key)
        h.tool_stop()
        if obs < 0:                       # monitor mode unavailable -> skip, don't fail
            return "SKIP", detail
        return ("PASS" if obs >= thr else "FAIL"), f"{key}={obs} (>= {thr}) {detail}"

    stim = r["stim"]() if callable(r.get("stim")) else st_none()
    sres = stim()
    time.sleep(r.get("dwell", 1.5))   # some tools need dwell to accumulate list/counters

    # read result
    if src in ("pkt", "det"):
        final = read_src(h, src)
        keys = key if isinstance(key, (list, tuple)) else [key]
        # any-of: the tool passes if ANY of its plausible counters moved (the badge
        # feeds *Frames vs num* per tool, so we accept whichever one this tool drives).
        obs = max(final.get(k, 0) - (base.get(k, 0) if mode == "delta" else 0) for k in keys)
        detail = " ".join(f"{k} {(base.get(k,0) if mode=='delta' else 0)}->{final.get(k,0)}" for k in keys)
    elif src == "kwifi":
        obs = kalipi("wifi-scan").get("count", 0) - (kbase or 0)
        detail = f"ssid-count delta {obs}"
    elif src == "kble":
        obs = kalipi("ble-scan", 6).get("count", 0) - (kbase or 0)
        detail = f"ble-count delta {obs}"
    else:
        obs = 0
    h.tool_stop()
    ok = obs >= thr
    return ("PASS" if ok else "FAIL"), f"{detail} (need >= {thr}) [{sres.get('msg','') or sres.get('ok')}]"


def run_disconnect(h, r, cat, item, thr):
    """Monitor-FREE active-tool test: kalipi stays associated to shipship, the badge
    deauths it, and we watch kalipi get kicked off. Works on the brcmfmac radio (which
    can't do monitor mode) because it observes the EFFECT, not the frames."""
    kalipi("wifi-join", SSID, PW, timeout=45)
    if not kalipi("wifi-state").get("connected"):
        return "SKIP", "kalipi not associated to shipship (precondition)"
    if r.get("prescan"):
        h.tool_start(1, 0); time.sleep(5); h.tool_stop()   # populate the badge's AP list
    if r.get("select"):
        h.ap_scan(r["select"])
    start = h.tool_start(cat, item)
    if not start.get("ok"):
        kalipi("wifi-join", SSID, PW, timeout=45)
        return "SKIP", f"tool_start {cat}.{item}: {start.get('error') or start.get('err','?')}"
    dropped = False
    for _ in range(8):                    # ~16s window for the deauth to land
        time.sleep(2)
        if not kalipi("wifi-state").get("connected"):
            dropped = True; break
    h.tool_stop()
    kalipi("wifi-join", SSID, PW, timeout=45)   # reconnect for subsequent rows
    if dropped:
        return "PASS", "kalipi DROPPED off shipship (badge deauth landed)"
    # No drop is INCONCLUSIVE, not a badge failure: kalipi's brcmfmac client resists
    # spoofed deauth in firmware (and/or PMF), so we can't confirm the badge's TX this
    # way. A monitor-capable adapter would verify it directly (sniff the deauth frames).
    return "SKIP", "no drop -- brcmfmac client resists deauth; needs monitor adapter to confirm"


def run_tier2(h, r, cat, item, src, key):
    """Tier-2 via the A6210/mt76x2u monitor adapter (wlan1). The badge tool is already
    started by run_row. sniff: the badge TRANSMITS, wlan1 hears the frames. inject:
    wlan1 injects deauth frames, the badge's DETECTOR counts them."""
    ch = r.get("channel", SHIP_CH)
    mu = kalipi("mon-up", MON, ch, timeout=45)
    if not mu.get("ok"):
        kalipi("mon-down", MON, timeout=30)
        return -1, "monitor unavailable on wlan1: " + str(mu.get("settype_err") or mu.get("err"))
    try:
        if src == "sniff":
            res = kalipi("sniff", 7, key, MON, timeout=22)
            return res.get("count", 0), f"wlan1 sniffed {res.get('count')} {key} frames (badge TX)"
        else:  # inject deauth (mdk4 on ch) -> badge detector counts
            keys = key if isinstance(key, (list, tuple)) else [key]
            # STATE THE MODE THE TEST NEEDS -- do not inherit it.
            #
            # The badge's deauth channel policy defaults to 1/6/11 and PERSISTS to NVS, so a
            # row that floods `ch` and simply hopes the badge is listening is inheriting
            # whatever a human (or a previous row) last left. SHIP_CH is auto-detected from
            # the live AP and that AP roams -- 7 -> 11 -> 4 have all been observed -- so on any
            # night it lands outside {1,6,11} this row would read ~ambient and look like a
            # firmware regression when the badge is working perfectly.
            #
            # Ask for hop-all explicitly. Failure to set it is CANNOT-TEST, not a quiet
            # fallback: a run that could not configure the DUT has not tested anything.
            dm = h.cmd("deauth_channel 0")
            if not (dm and dm.get("ok")):
                return 0, "CANNOT TEST: could not set deauth_channel 0 (hop all) on the badge"
            b = max(h.pkt_counters().get(k, 0) for k in keys)
            dj = kalipi("deauth", MON, ch, "12", timeout=30)   # 12s deauth-amok (rate ramps)
            time.sleep(1)
            obs = max(h.pkt_counters().get(k, 0) for k in keys) - b
            return obs, f"badge {'/'.join(keys)} +{obs} (mdk4 sent {dj.get('sent','?')} on ch{ch})"
    finally:
        kalipi("mon-down", MON, timeout=30)
        # th_cmd_deauth_channel PERSISTS to NVS, so a run that set hop-all would otherwise
        # strand the badge on 0 instead of the shipping default 200 -- and every later
        # manual check would silently be on the wrong policy. Restore unconditionally:
        # this block also runs on the exception path, which is exactly when a badge would
        # otherwise be left mis-set.
        try:
            h.cmd("deauth_channel 200")
        except Exception:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tier", default="1")
    ap.add_argument("--only", default="")
    ap.add_argument("--sku", default="res34rch")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--no-deploy", action="store_true")
    ap.add_argument("--ship-ch", default=os.environ.get("CLIPBOY_SHIP_CH"),
                    help="override shipship's 2.4GHz channel (else auto-detected from a kalipi scan)")
    a = ap.parse_args()
    global SHIP_CH
    tiers = {int(t) for t in a.tier.split(",") if t.strip()}
    only = {s.strip() for s in a.only.split(",") if s.strip()}
    rows = list(sel(ROWS, tiers, only))

    if a.list:
        for r in rows:
            print(f"  {r['id']:>5} T{r['tier']} [{r['sku']}] {r['name']:<22} "
                  f"verify={r['verify']} {r.get('conf','')}")
        print(f"\n{len(rows)} rows.")
        return 0

    if not a.no_deploy:
        print(f"[deploy] scp kalipi_stim.py -> {KALIPI}")
        deploy()
    caps = kalipi("caps")
    print(f"[kalipi] {caps.get('links')} monitor={caps.get('monitor_capable')}")

    # Resolve shipship's LIVE 2.4GHz channel: --ship-ch/env override, else auto-detect
    # via a kalipi scan. The AP roams (7->11->4 seen), and a stale SHIP_CH makes the
    # channel-locked rows (3.2 deauth-inject, 3.7 EAPOL) target an empty channel and
    # false-fail. Patch the selected rows so run_tier2/run_row use the real channel.
    eff_ch = int(a.ship_ch) if a.ship_ch else None
    if eff_ch:
        print(f"[ship-ch] override: ch{eff_ch}")
    else:
        sc = kalipi("scan-ch", SSID)
        if sc.get("ok") and sc.get("channel"):
            eff_ch = int(sc["channel"]); print(f"[ship-ch] auto-detected {SSID} on ch{eff_ch}")
        else:
            print(f"[ship-ch] WARN: auto-detect failed ({sc.get('err')}); using default ch{SHIP_CH}")
    if eff_ch and eff_ch != SHIP_CH:
        old = SHIP_CH; SHIP_CH = eff_ch
        for r in rows:
            if r.get("channel") == old:
                r["channel"] = eff_ch
        print(f"[ship-ch] SHIP_CH {old} -> {eff_ch} (patched {sum(1 for r in rows if r.get('channel')==eff_ch)} channel-locked rows)")

    print(f"=== tool_suite: {len(rows)} tools (tiers {sorted(tiers)}, sku {a.sku}) ===")
    h = Harness(port=os.environ.get("CLIPBOY_PORT"))   # pin badge #1 when 2 are attached
    results = []
    try:
        for r in rows:
            if r["sku"] == "R" and a.sku != "res34rch":
                results.append((r["id"], r["name"], "SKIP", "Res34rch-only tool"))
                continue
            print(f"  -> {r['id']:>5} {r['name']} ...", flush=True)
            try:
                status, detail = run_row(h, r)
            except Exception as e:
                status, detail = "ERROR", f"{type(e).__name__}: {e}"
            h.tool_stop()
            results.append((r["id"], r["name"], status, detail))
            print(f"     {status}: {detail}")
    finally:
        h.tool_stop()
        h.close()
        kalipi_restore()

    print("\n=== RESULTS ===")
    npass = sum(1 for _ in results if _[2] == "PASS")
    for rid, name, status, detail in results:
        print(f"  [{status:5}] {rid:>5} {name:<22} {detail}")
    print(f"\n{npass}/{len(results)} PASS")
    return 0 if npass == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
