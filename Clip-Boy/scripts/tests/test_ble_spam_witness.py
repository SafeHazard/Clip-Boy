#!/usr/bin/env python3
"""test_ble_spam_witness.py -- does the badge's BLE Spam actually RADIATE, per type?

TIER A: BLE Spam is 5 of the 7 tool entries with no proof of any kind. It is Bluetooth,
so nothing on the proven esp_wifi_80211_tx(WIFI_IF_AP) path says anything about it
(ui_nav.h:2229-2236). Res34rch-only -> COM5.

THE ORACLE, after a diagnostic that killed the first version (2026-07-29):
  v1 counted TOTAL adverts, ON vs OFF, ratio >= 5x. It FAILED all five even though the
  badge was plainly radiating -- ambient BLE (~200 adverts/15s, real phones everywhere)
  dilutes any total-count ratio to ~2-3x. I did NOT lower the threshold; I found a better
  discriminator.

  Per BLE Spam type, the badge floods ONE distinguishing advertising-data key from a
  NON-RESOLVABLE address. Measured:
     Sour Apple -> mfg:Apple            89 vs 9 ambient   (10x)
     Swiftpair  -> mfg:Microsoft       433 vs ~0
     Samsung    -> mfg:Samsung         412 vs 33
     Google     -> svc:Google 0xfe2c   361 vs ~0   (Service Data, NOT a company)
     Flipper    -> mfg:not assigned    366 vs ~0
  Real phones advertise the same continuity from RESOLVABLE-private addresses, so the
  non-resolvable filter (in ble_adv_signature.py) removes the ambient dilution.

  The test DISCOVERS the spiking key per item -- it does not hardcode the map. That is
  what caught Google being service-data rather than a company; a hardcoded map would have
  scored it silent. Pass = the top-spiking key clears BOTH pre-registered gates.

  py -3 scripts/tests/test_ble_spam_witness.py --port COM5
"""
import argparse, os, sys, time
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ.setdefault("CLIPBOY_SESSION_TIMEOUT", "30")
from harness import Harness
import rf_pcap

WINDOW = 10                 # btmon capture seconds per arm
MIN_ON = 40                 # pre-registered BEFORE the loop: spam floods, real keys don't
MIN_RATIO = 4.0             # top key must beat its own ambient baseline by this
SNOOP = "/tmp/ble_spam.snoop"
SIG_REMOTE = "/tmp/ble_adv_signature.py"

ITEMS = [(0, "! Sour Apple"), (1, "! Swiftpair"), (2, "! Samsung"),
         (3, "! Google"), (4, "! Flipper")]

# Documentation only -- what we EXPECT each item to spike, from the 2026-07-29 diagnostic.
# The test discovers the actual key; a mismatch is reported, not failed (the badge's spam
# library could legitimately change what it spoofs).
EXPECTED = {0: "mfg:Apple, Inc./type15",   # Nearby Action -- ambient Apple barely uses it
            1: "mfg:Microsoft",
            2: "mfg:Samsung Electronics Co. Ltd.", 3: "svc:Google (0xfe2c)",
            4: "mfg:not assigned"}

BLE_IDX = None


def resolve_ble_idx():
    """CSR dongle index by USB id 0a12:0001, never a hardcoded hciN (they renumber)."""
    rc, out = rf_pcap._ssh(
        "for H in $(ls /sys/class/bluetooth/ 2>/dev/null | grep '^hci'); do "
        "D=$(readlink -f /sys/class/bluetooth/$H/device); "
        "V=$(cat $D/../idVendor 2>/dev/null); P=$(cat $D/../idProduct 2>/dev/null); "
        "[ \"$V:$P\" = \"0a12:0001\" ] && echo ${H#hci}; done")
    for line in out.split():
        if line.strip().isdigit():
            return line.strip()
    return "0"


def reset_csr():
    """down/up cycle. MEASURED: the CSR dongle goes deaf every ~10-15 discovery cycles --
    btmon captures 0 while the adapter still reports UP RUNNING. A multi-capture test
    wedges MID-RUN, not just at boot, so this runs before EVERY capture, not once."""
    rf_pcap._ssh("sudo hciconfig hci%s down; sleep 1; sudo hciconfig hci%s up; sleep 2; "
                 "sudo btmgmt --index %s power on >/dev/null 2>&1; "
                 "sudo btmgmt --index %s le on >/dev/null 2>&1"
                 % (BLE_IDX, BLE_IDX, BLE_IDX, BLE_IDX), timeout=30)


def signature(secs):
    """Reset, capture btmon WHILE scanning, parse -> {key: count} (non-resolvable only).

    btmon is a passive HCI TAP, not a scanner: `btmgmt find` for the window is what makes
    the controller listen. Retries ONCE on an empty capture (the CSR wedge), so a transient
    deafness reports as a retry rather than as 'the badge sent nothing'.
    """
    for attempt in (1, 2):
        reset_csr()
        rf_pcap._ssh("sudo rm -f %s" % SNOOP)
        rf_pcap._ssh("sudo sh -c 'nohup btmon -w %s </dev/null >/dev/null 2>&1 & echo $! > %s.pid'"
                     % (SNOOP, SNOOP))
        time.sleep(1)
        rf_pcap._ssh("sudo timeout %d btmgmt --index %s find >/dev/null 2>&1"
                     % (secs, BLE_IDX), timeout=secs + 30)
        rf_pcap._ssh("sudo sh -c 'kill -TERM $(cat %s.pid) 2>/dev/null'; sleep 1" % SNOOP)
        rc, out = rf_pcap._ssh("sudo python3 %s %s" % (SIG_REMOTE, SNOOP), timeout=120)
        sig = {}
        total = 0
        for line in out.splitlines():
            if "\t" in line:
                k, n = line.rsplit("\t", 1)
                if n.strip().isdigit():
                    sig[k] = int(n)
                    total += int(n)
        if total > 0:
            return sig
        rf_pcap.dbg("ble.capture.empty.retry", "attempt %d" % attempt)
    return {}


def main():
    global BLE_IDX
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=os.environ.get("CLIPBOY_PORT", "COM5"))
    a = ap.parse_args()

    BLE_IDX = resolve_ble_idx()
    print("BLE SPAM WITNESS -- badge %s (res34rch) vs kali CSR hci%s" % (a.port, BLE_IDX))
    print("per-item discriminator: top non-resolvable-address key, ON vs OFF")
    print("pre-registered gates: ON >= %d AND ON/OFF >= %.1fx\n" % (MIN_ON, MIN_RATIO))

    # Push the parser to the witness (idempotent).
    import subprocess
    subprocess.run(["scp", "-q", "-o", "BatchMode=yes",
                    os.path.join(HERE, "ble_adv_signature.py"),
                    "%s@%s:%s" % (rf_pcap.KALI_USER, rf_pcap.KALI_HOST, SIG_REMOTE)],
                   timeout=40)

    if not rf_pcap.preflight(chan=6):
        print("CANNOT TEST: witness preflight failed -- zeros would be meaningless.")
        return 2

    h = Harness(port=a.port)
    results = []
    try:
        if h.cat_pos("BLE Spam") is None:
            print("CANNOT TEST: %s is not a Res34rch build (no BLE Spam)." % a.port)
            return 2

        # OFF baseline FIRST -- the control, before the readings it validates. One capture,
        # reused for every item's per-key ambient floor.
        h.tool_stop(); time.sleep(2)
        off = signature(WINDOW)
        if not off:
            print("CANNOT TEST: witness saw ZERO adverts even after reset -- not listening.")
            return 2
        off_total = sum(off.values())
        print("baseline (idle): %d non-resolvable adverts across %d keys\n"
              % (off_total, len(off)))

        for item, name in ITEMS:
            h.cmd("tool_open 9 %d" % item)
            time.sleep(3)                       # BLE spam has a slow start
            on = signature(WINDOW)
            h.tool_stop(); time.sleep(1)
            if not on:
                results.append((name, None, "witness went deaf even after retry"))
                print("  %-14s CANNOT-TEST" % name)
                continue
            # Rank keys by ON count; the discriminator is the top key that clears MIN_ON,
            # scored against its OWN ambient baseline.
            best = None
            for key, on_n in sorted(on.items(), key=lambda kv: -kv[1]):
                if on_n < MIN_ON:
                    break
                ratio = on_n / max(off.get(key, 0), 1)
                if best is None or ratio > best[2]:
                    best = (key, on_n, ratio)
            if best is None:
                results.append((name, False, "no key reached ON>=%d" % MIN_ON))
                print("  %-14s FAIL   no key floods (top=%s)"
                      % (name, max(on.items(), key=lambda kv: kv[1]) if on else "-"))
                continue
            key, on_n, ratio = best
            ok = (on_n >= MIN_ON) and (ratio >= MIN_RATIO)
            exp = EXPECTED.get(item, "?")
            match = "" if key == exp else "  (expected %s)" % exp
            d = "key=%s on=%d off=%d ratio=%.0fx%s" % (key, on_n, off.get(key, 0), ratio, match)
            results.append((name, ok, d))
            print("  %-14s %-5s %s" % (name, "PASS" if ok else "FAIL", d))
    finally:
        try:
            h.tool_stop(); h.close()
        except Exception:
            pass

    print("\n===== SUMMARY (tier A: BLE Spam) =====")
    fails = cannot = 0
    for name, ok, d in results:
        tag = "PASS" if ok else ("CANNOT-TEST" if ok is None else "FAIL")
        if ok is None:
            cannot += 1
        elif not ok:
            fails += 1
        print("  %-14s %-12s %s" % (name, tag, d))
    if cannot:
        print("\nVERDICT: %d CANNOT-TEST (rig), %d FAIL, %d PASS"
              % (cannot, fails, len(results) - fails - cannot))
        return 2
    print("\nVERDICT: %s" % ("ALL PASS -- BLE Spam radiates, per type"
                             if fails == 0 else "%d of %d FAILED" % (fails, len(results))))
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
