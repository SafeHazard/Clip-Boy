#!/usr/bin/env python3
"""detect_emitters.py -- make the badge's device detectors FIRE using synthetic RF
from kalipi, so you can confirm Detect/Analyze tools work without a real Flipper /
Flock / skimmer / AirTag / Pwnagotchi / Pineapple nearby.

Each emitter reproduces the EXACT signature the badge matches (see comments):
  BLE (btmgmt add-adv / name)  -> AirTag, Skimmer, Flipper, Flock
  802.11 beacon (scapy/wlan1)  -> Rogue-AP, Evil-Twin, Pwnagotchi, Espressif

Usage (run from repo root; badge = COM11 DUT, kalipi = 192.168.1.146):
  py -3 scripts/tests/detect_emitters.py --list
  py -3 scripts/tests/detect_emitters.py --all              # drive badge + verify each fires
  py -3 scripts/tests/detect_emitters.py --emit flipper     # start the tool + emit; watch the badge
  py -3 scripts/tests/detect_emitters.py --emit flock 30    # emit flock for 30s

Env: CLIPBOY_DUT (COM11), CLIPBOY_KALIPI (192.168.1.146). Deploys kalipi_stim.py first.
"""
import argparse, os, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ.setdefault("CLIPBOY_SESSION_TIMEOUT", "20")
from harness import Harness
import tool_suite as TS

DUT = os.environ.get("CLIPBOY_DUT", "COM11")

# detector -> badge tool (cid,item), detect_counts field to watch, kalipi primitive+args, blurb.
# kal args: BLE emitters use ble-raw/ble-name; WiFi use wifi-beacon <kind> <secs> <chan>.
EMITTERS = {
    "airtag":     dict(tool=(0, 0), counter="airtag",    kal=("ble-raw", "1eff4c001219" + "00" * 25),
                       desc="Apple FindMy / AirTag  (mfg 0x004C, type 0x12/0x19)"),
    "skimmer":    dict(tool=(0, 1), counter="bt",        kal=("ble-name", "HC-05"),
                       desc="BT card-skimmer heuristic (advertises name 'HC-05')"),
    "flipper":    dict(tool=(0, 2), counter="flipper",   kal=("ble-raw", "06ff8230123456"),
                       desc="Flipper Zero  (mfg company 0x3082 = White)"),
    "flock":      dict(tool=(0, 3), counter="flock",     kal=("ble-raw", "04ffc80900"),
                       desc="Flock BATTERY pack (mfg XUNTONG 0x09C8, no name) -- NOT the camera; solar cameras are BLE-silent"),
    "rogueap":    dict(tool=(0, 4), counter="esp",       kal=("wifi-beacon", "rogueap"),
                       desc="Rogue AP / WiFi Pineapple  (beacon OUI 00:13:37)",
                       note="detect_counts.esp actually holds the pinescan/rogue count"),
    "eviltwin":   dict(tool=(0, 5), counter="multissid", kal=("wifi-beacon", "eviltwin"),
                       desc="Evil Twin / MultiSSID  (one BSSID, 4 distinct SSIDs)"),
    "pwnagotchi": dict(tool=(3, 4), counter="pwn",       kal=("wifi-beacon", "pwnagotchi"),
                       desc="Pwnagotchi  (beacon DE:AD:BE:EF:DE:AD + name/pwnd_tot JSON)"),
    "espressif":  dict(tool=(3, 5), counter="ap",        kal=("wifi-beacon", "espressif"),
                       desc="Espressif device  (beacon BSSID OUI 24:0A:C4)",
                       note="no dedicated counter -> verified via AP-list (detect_counts.ap) delta"),
}
WIFI = {"rogueap", "eviltwin", "pwnagotchi", "espressif"}


def fire(name, secs):
    """Emit one signature via kalipi for `secs` (blocking). Returns kalipi result str."""
    e = EMITTERS[name]; prim = e["kal"][0]
    if prim == "ble-raw":
        return TS.kalipi("ble-raw", e["kal"][1], str(secs), timeout=secs + 10)
    if prim == "ble-name":
        return TS.kalipi("ble-name", e["kal"][1], str(secs), timeout=secs + 10)
    if prim == "wifi-beacon":
        return TS.kalipi("wifi-beacon", e["kal"][1], str(secs), "6", timeout=secs + 15)
    return {"ok": False, "error": "bad prim"}


def verify_one(h, name, secs=8):
    e = EMITTERS[name]; cid, item = e["tool"]; ctr = e["counter"]
    if name in WIFI:
        secs = 22   # WiFi smart-detectors need several sightings across the hop set
                    # (pinescan/rogue-AP in particular needs the most overlap time)
    h.cmd("tool_stop"); time.sleep(0.3)
    h.cmd(f"tool_open {cid} {item}"); time.sleep(1.0)
    base = h.detect_counts().get(ctr, 0)
    r = fire(name, secs)
    time.sleep(1.0)
    final = h.detect_counts().get(ctr, 0)
    h.tool_stop()
    d = final - base
    ok = d >= 1
    kal_ok = isinstance(r, dict) and r.get("ok")
    print(f"  {name:11s} tool {cid}.{item}  detect_counts.{ctr}: {base}->{final} (+{d})  "
          f"kalipi={'ok' if kal_ok else r}  -> {'FIRED' if ok else 'no-fire'}"
          + (f"   [{e['note']}]" if e.get('note') else ""))
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--all", action="store_true", help="drive badge + verify every detector fires")
    ap.add_argument("--emit", metavar="NAME", help="start that detector's tool + emit; watch the badge")
    ap.add_argument("secs", nargs="?", type=int, default=15, help="emit duration for --emit (default 15)")
    ap.add_argument("--no-deploy", action="store_true")
    args = ap.parse_args()

    if args.list:
        for k, e in EMITTERS.items():
            print(f"  {k:11s} tool {e['tool'][0]}.{e['tool'][1]:<2d} {e['desc']}")
        return 0

    if not args.no_deploy:
        try: TS.deploy()
        except Exception as ex: print("deploy warn:", ex)

    if args.emit:
        name = args.emit
        if name not in EMITTERS:
            print("unknown:", name, "\n(try --list)"); return 2
        e = EMITTERS[name]; cid, item = e["tool"]
        h = Harness(port=DUT, skip_boot=True)
        try:
            if name in WIFI: h.cmd("raw_channel 6")
            h.cmd(f"tool_open {cid} {item}")
            if name in WIFI: h.cmd("raw_channel 6")
            print(f"badge running {name} tool ({cid}.{item}); emitting {args.secs}s -- watch the badge")
            print("  ", fire(name, args.secs))
            print("  detect_counts.%s = %s" % (e["counter"], h.detect_counts().get(e["counter"])))
            h.tool_stop()
        finally:
            h.close()
            try: TS.kalipi_restore()
            except Exception: pass
        return 0

    # default / --all: verify every detector
    caps = TS.kalipi("caps")
    print("kalipi caps:", caps.get("ok"), "monitor_capable=", caps.get("monitor_capable"))
    h = Harness(port=DUT, skip_boot=True)
    passed = 0
    try:
        print(f"=== detector emitter verification ({len(EMITTERS)}) ===")
        for name in EMITTERS:
            try:
                if verify_one(h, name): passed += 1
            except Exception as ex:
                print(f"  {name:11s} ERROR: {ex}")
        print(f"\n{passed}/{len(EMITTERS)} detectors fired")
    finally:
        h.close()
        try: TS.kalipi_restore()
        except Exception: pass
    return 0 if passed == len(EMITTERS) else 1


if __name__ == "__main__":
    sys.exit(main())
