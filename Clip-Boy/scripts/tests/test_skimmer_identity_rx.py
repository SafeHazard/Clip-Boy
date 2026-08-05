#!/usr/bin/env python3
"""test_skimmer_identity_rx.py -- does Detect > Skimmer Check RECEIVE and DISCRIMINATE by name?

Strengthens the weak Sn34k Skimmer oracle (detect_emitters.py used a generic bt-count delta that
ambient BLE also moves) to a real IDENTITY + DISCRIMINATION test:
  ACCEPT arm : kalipi advertises name "HC-05" (a skimmer name) -> it appears in `bt_list` under
               skimmer mode (btScanSkimmers only ADDS names in {HC-03,HC-05,HC-06} -- live match at
               WiFiScan.cpp:1780, addOrUpdateBTDevice into the same bt_devices bt_list reads).
  REJECT arm : kalipi advertises "HC-04" (a NEAR MISS, not in the set) -> it must be ABSENT while a
               witness confirms it actually radiated. This proves the filter DISCRIMINATES
               accept(HC-05)/reject(HC-04), not just "something showed up".

⚠ Skimmer mode gives NO host-readable liveness (it doesn't bump bt_frames or list ambient BLE), so a
dead emitter would read as "HC-05 absent" = badge falsely blamed. => an INDEPENDENT kali CSR BLE
witness (btmon name-grep) must confirm each advert radiated; witness<MIN => CANNOT-TEST, never FAIL.
This is the fix for the pre-review's "the plan's liveness gate can never fire" BLOCK.

Rig: DUT = a badge with bt_list (COM5 --test); emitter = kalipi .146 hci0 (`ble-name`); witness =
kali .11 CSR dongle (0a12:0001). kalipi and kali are co-located (both hear the emit).

  py -3 scripts/tests/test_skimmer_identity_rx.py --port COM5
"""
import argparse, os, sys, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ.setdefault("CLIPBOY_SESSION_TIMEOUT", "45")
from harness import Harness
import tool_suite as TS
import rf_pcap

SK_SECS = 22            # kalipi advertises this long; DUT scans + CSR witnesses inside it
SK_WIN  = 14            # CSR btmgmt-find witness window
MIN_WIT = 3             # advert frames the CSR must see to call it "radiated"
SNOOP   = "/tmp/skim.snoop"


def csr_index():
    """CSR dongle hci index by USB id 0a12:0001 (never a hardcoded hciN -- they renumber)."""
    rc, out = rf_pcap._ssh(
        "for H in $(ls /sys/class/bluetooth/ 2>/dev/null | grep '^hci'); do "
        "D=/sys/class/bluetooth/$H/device; "
        "V=$(cat $D/../idVendor 2>/dev/null); P=$(cat $D/../idProduct 2>/dev/null); "
        "[ \"$V:$P\" = \"0a12:0001\" ] && echo ${H#hci}; done")
    s = (out or "").strip().split()
    return s[-1] if s else None


def csr_reset(idx):
    # The CSR goes deaf every ~10-15 discovery cycles -> reset before EVERY capture (measured).
    rf_pcap._ssh("sudo hciconfig hci%s down; sleep 1; sudo hciconfig hci%s up; sleep 2; "
                 "sudo btmgmt --index %s power on >/dev/null 2>&1" % (idx, idx, idx))


def witness_name(idx, name):
    """Capture btmon on the CSR while btmgmt-find scans; return how many times `name` appears
    in the decoded advert stream (0 if the emitter didn't radiate / CSR wedged)."""
    csr_reset(idx)
    rf_pcap._ssh("sudo rm -f %s" % SNOOP)
    rf_pcap._ssh("sudo sh -c 'nohup btmon -w %s </dev/null >/dev/null 2>&1 & echo $! > %s.pid'"
                 % (SNOOP, SNOOP))
    rf_pcap._ssh("sudo timeout %d btmgmt --index %s find >/dev/null 2>&1" % (SK_WIN, idx))
    rf_pcap._ssh("sudo sh -c 'kill -TERM $(cat %s.pid) 2>/dev/null'; sleep 1" % SNOOP)
    rc, out = rf_pcap._ssh("btmon -r %s 2>/dev/null | grep -c -- '%s'" % (SNOOP, name), timeout=120)
    try:
        return int((out or "0").strip().split()[-1])
    except Exception:
        return 0


def arm(dut, detect_cat, kidx, name):
    """Emit `name` via kalipi ble-name; run DUT Detect>Skimmer; witness on the CSR.
    Returns (present_in_bt_list, witnessed_count, dut_bt_count)."""
    res = {}
    def emit():
        res.update(TS.kalipi("ble-name", name, str(SK_SECS + 6), timeout=SK_SECS + 46) or {})
    t = threading.Thread(target=emit, daemon=True); t.start()
    time.sleep(3)
    dut.tool_stop(); time.sleep(1)
    dut.cmd("tool_open %d 1" % detect_cat)          # Detect > Skimmer Check (item 1)
    wit = witness_name(kidx, name)                  # ~SK_WIN s, concurrent with the DUT scan
    time.sleep(2)
    bl = dut.cmd("bt_list") or {}
    devs = bl.get("devices") or []
    present = any((d.get("name") == name) for d in devs)
    dut.tool_stop()
    t.join(timeout=SK_SECS + 30)
    return present, wit, bl.get("count", 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=os.environ.get("CLIPBOY_PORT", "COM5"))
    a = ap.parse_args()
    print("SKIMMER IDENTITY RX -- DUT %s, emit=kalipi ble-name, witness=kali CSR\n" % a.port)
    TS.deploy()
    kidx = csr_index()
    if not kidx:
        print("CANNOT TEST: kali CSR BLE dongle (0a12:0001) not found."); return 2

    h = Harness(port=a.port)
    try:
        dc = h.cat_pos("Detect")
        if dc is None:
            print("CANNOT TEST: no Detect category."); return 2
        # ACCEPT: HC-05 is a skimmer name -> must be detected.
        acc_present, acc_wit, acc_n = arm(h, dc, kidx, "HC-05")
        print("ACCEPT HC-05: present=%s witnessed=%d (skimmer-list count=%d)" % (acc_present, acc_wit, acc_n))
        # REJECT: HC-04 is a NEAR MISS -> must be absent, but must be witnessed radiating.
        rej_present, rej_wit, rej_n = arm(h, dc, kidx, "HC-04")
        print("REJECT HC-04: present=%s witnessed=%d (skimmer-list count=%d)" % (rej_present, rej_wit, rej_n))
    finally:
        try: h.tool_stop(); h.close()
        except Exception: pass
        try: rf_pcap._ssh("sudo sh -c 'kill -TERM $(cat %s.pid) 2>/dev/null'" % SNOOP)
        except Exception: pass

    # Liveness gate: BOTH adverts must have radiated (else the emitter/CSR failed -> CANNOT-TEST).
    if acc_wit < MIN_WIT or rej_wit < MIN_WIT:
        print("\nCANNOT-TEST: advert(s) did not radiate on the CSR witness "
              "(HC-05=%d HC-04=%d, need >=%d) -- rig, not badge" % (acc_wit, rej_wit, MIN_WIT)); return 2
    ok = acc_present and not rej_present
    print("\nVERDICT: %s" % (
        "PASS -- Skimmer detects HC-05 (identity) and DISCRIMINATES (HC-04 rejected though it radiated)"
        if ok else
        "FAIL -- HC-05 present=%s (want True), HC-04 present=%s (want False) [both witnessed radiating]"
        % (acc_present, rej_present)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
