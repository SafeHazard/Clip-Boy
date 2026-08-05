#!/usr/bin/env python3
"""test_detect_oracles.py -- move the weak Detect/Analyze oracles from detect_emitters.py's
thin `delta>=1` to real DISCRIMINATION + IDENTITY oracles, gated so a dead rig -> CANNOT-TEST.

Covers (both pre-reviews' reconciled verdict, docs/test-plans/detect-oracle-strengthen-plan.md):
  TIER 1  STRONG discrimination (the genuine wins):
    - Rogue AP   (Detect item 4, detect_counts.esp)       OUI 00:13:37
    - Evil Twin  (Detect item 5, detect_counts.multissid) 1 BSSID / >=3 SSIDs
    - Pwnagotchi (Analyze item 4, detect_counts.pwn)      DE:AD:BE:EF:DE:AD + JSON
    - Flock      (Detect item 3, detect_counts.flock + flock_serial) -- IDENTITY (exact serial)
    - Espressif  (Analyze item 5, h.text() FILTERED "ESP devices" log) -- the OUI FILTER
  TIER 2  DOWNGRADE, labeled (both reviewers: +1 delta is swamped by ambient in a lab full of
    Apple/Flipper gear -- NOT ambient-discriminating):
    - AirTag  (Detect item 0, detect_counts.airtag)   liveness + signature-shape only
    - Flipper (Detect item 2, detect_counts.flipper)  liveness + signature-shape only

=== LIVENESS DESIGN (why, and its honest limit) ===
The plan called for an external kali witness to prove "the emitter radiated" so a flat/zero counter
means "none sent", not "DUT deaf". The kali .11 witness is a live-image laptop and was DOWN when this
was built. Rather than ship a btmon/tshark grep never seen to match (kali down) -- which would violate
"prove the observable can read positive before trusting its zero" -- liveness is established from the
DUT and the emitter, DIFFERENTLY for positives and negatives:

  Phase 1 (POSITIVES) ARE genuinely self-witnessed: run the DUT in signature S's mode and emit S; the
    DUT reporting S proves S radiated AND was received by THIS badge, this run. ⚠ But only Flock and
    Espressif are IDENTITY-attributed (exact serial / random nonce SSID a stale buffer can't fabricate);
    rogueap/eviltwin/pwnagotchi positives are a bare counter delta on a KNOWN signature, so they prove
    "this signature is PRESENT in the environment", NOT necessarily "our emitter produced it" (ambient
    DEF CON traffic can supply it). Labeled as such; airtag/flipper likewise (TIER 2).
  Phase 2 (NEGATIVES) CANNOT be self-witnessed the same way: a single-radio badge is in ONE
    currentScanMode at a time, so WHILE in X's mode it structurally cannot read Y's counter to confirm
    Y radiated in-window. So each negative carries its OWN same-run liveness, adjacent in time:
      (a) a CONTROL arm -- run Y in Y's mode, emit Y, confirm Y's counter rises THIS RUN (mode-
          independent proof the emitter+path are live NOW, not relying on Phase 1 at a different time);
      (b) sent>0 on the X-mode arm's emit (wifi_beacon returns frames-pushed) -- the emitter radiated
          DURING the discrimination window itself.
    Then, in X's mode, emit Y and assert X's counter stays flat. Discrimination is sound because BLE
    detectors dispatch through ONE live NimBLEScanCallbacks (WiFiScan.cpp:1126) as a single if/else on
    currentScanMode and each WiFi detector owns a per-mode promiscuous callback -> Y can never bump X's
    counter unless that exclusivity REGRESSES, which is what this catches.
    ⚠ HONEST RESIDUAL: the control + sent>0 do NOT close a TRANSIENT in-window radiation failure (frames
    pushed to the monitor iface but not actually emitted by the antenna during the X-window specifically)
    -- only an INDEPENDENT receiver can. That residual is why the external kali witness stays the right
    hardening (the T3 BLE-advert rate test also REQUIRES it); it is a documented gap, NOT a closed one.

kalipi's emit result (ok + sent count) is the emitter-side liveness signal: a Phase-2 arm whose emitter
reports it never sent -> CANNOT-TEST, never a false discrimination PASS.

Rig: DUT = COM5 res34rch --test (Detect/Analyze are SKU-identical passive code, so this is honest for
Sn34k too). Emitter = kalipi .146 (ble-raw / flock-real / wifi-beacon, nonce SSID via "<kind>:<ssid>").

  py -3 scripts/tests/test_detect_oracles.py --port COM5
"""
import argparse, os, sys, time, json, random, string

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ.setdefault("CLIPBOY_SESSION_TIMEOUT", "40")
from harness import Harness
import tool_suite as TS

# ── Pre-registered thresholds (set BEFORE any run; a result cannot be rationalised after) ──
WIFI_SECS   = 22     # WiFi smart-detectors need several sightings across the 1/6/11 hop set
BLE_SECS    = 12     # BLE raw adverts
POS_MIN     = 1      # a POSITIVE arm must move its counter by at least this (discrimination = pos>=1 AND neg==0)
NEG_MAX     = 0      # a NEGATIVE arm's counter must not move at all (exclusive currentScanMode dispatch)
FLOCK_SERIAL = "TN72023022000771"   # ground truth: kalipi flock-real is a REAL capture (non-circular)

# signature -> (kalipi emit, DUT category NAME + item index, detect_counts field, is-BLE, notes)
SIGS = {
    "airtag":     dict(kal=("ble-raw", "1eff4c001219" + "00" * 25), cat="Detect",  item=0, ctr="airtag",    ble=True),
    "flipper":    dict(kal=("ble-raw", "06ff8230123456"),           cat="Detect",  item=2, ctr="flipper",   ble=True),
    "flock":      dict(kal=("flock-real",),                         cat="Detect",  item=3, ctr="flock",     ble=True),
    "rogueap":    dict(kal=("wifi-beacon", "rogueap"),              cat="Detect",  item=4, ctr="esp"),
    # eviltwin needs >=3 DISTINCT SSIDs captured (vs >=1 for the others) -> the marginal one under the
    # 1/3 channel-hop duty (passed 2/3 early runs, flaked once at 22s). Give it a longer emit window so
    # >=3 of its 4 SSIDs reliably land; a genuine multissid=0 after this still FAILs (safe direction).
    # eviltwin needs >=3 DISTINCT SSIDs captured (vs >=1 for the others) -> the marginal one. It gets a
    # longer 40s window; ch6-only injection (global, see emit()) supplies the ~3x on-channel frames so
    # >=3 of its 4 SSIDs reliably land despite kalipi's variable emission. A genuine multissid=0 after
    # this still FAILs (safe dir). Proven: sent 672 (vs ~276 hopping), green x2.
    "eviltwin":   dict(kal=("wifi-beacon", "eviltwin"),             cat="Detect",  item=5, ctr="multissid", secs=40),
    "pwnagotchi": dict(kal=("wifi-beacon", "pwnagotchi"),           cat="Analyze", item=4, ctr="pwn"),
}
# discrimination pairs: (X under test, Y emitted as the non-matching negative). Y's positive must pass.
NEG_PAIRS = [("rogueap", "eviltwin"), ("eviltwin", "rogueap"), ("pwnagotchi", "rogueap"), ("flock", "flipper")]
DOWNGRADED = {"airtag", "flipper"}     # positive-only, labeled low-SNR (no discrimination claim)


def nonce(prefix):
    return prefix + "".join(random.choice(string.ascii_uppercase + string.digits) for _ in range(6))


def emit(sig_or_kind, secs, ssid=None):
    """Fire one signature via kalipi (blocking `secs`). Returns the kalipi result dict."""
    kal = SIGS[sig_or_kind]["kal"] if sig_or_kind in SIGS else sig_or_kind  # key, or a raw tuple (nonce arms)
    prim = kal[0]
    if prim == "ble-raw":
        return TS.kalipi("ble-raw", kal[1], str(secs), timeout=secs + 15) or {}
    if prim == "flock-real":
        return TS.kalipi("flock-real", str(secs), timeout=secs + 30) or {}
    if prim == "wifi-beacon":
        kind = kal[1] + (":" + ssid if ssid else "")
        # hop="0" PINS the injector to ch6. This test pins the DUT to ch6 (open_tool raw_channel 6) for
        # EVERY WiFi arm, so ch6-only injection gives ~3x on-channel frames vs a 1/6/11 hop -- needed
        # because kalipi's scapy emission volume varies run-to-run (measured sent 0..672) and the
        # high-threshold detectors flake when under-stimulated: multissid needs >=3 distinct SSIDs, and
        # the Espressif-nonce positive needs the beacon CAPTURED INTO the AP list (a run7 CANNOT-TEST
        # when it hopped away). Only affects this test; other wifi_beacon callers keep the 1/6/11 hop.
        return TS.kalipi("wifi-beacon", kind, str(secs), "6", "0", timeout=secs + 25) or {}
    return {"ok": False, "err": "bad prim"}


def open_tool(h, cat_id, item, wifi):
    h.tool_stop(); time.sleep(0.4)
    h.cmd("tool_open %d %d" % (cat_id, item)); time.sleep(1.0)
    if wifi:
        # Pin the DUT to the injector's ch6 (it hops 1/6/11). ⚠ ASSUMES raw_channel HOLDS under
        # sniffPinescan/sniffMultiSSID/sniffPwnagotchi -- if a detect scan re-hops and ignores the
        # pin (CLAUDE.md: "a lock that does nothing leaves the radio off-target"), the WiFi arms read
        # on the wrong channel. Held empirically here (positives fired), but not independently proven.
        h.cmd("raw_channel 6")
        time.sleep(0.3)


def dc(h, field):
    return (h.detect_counts() or {}).get(field, 0)


def _secs(sig):
    """Per-signature emit window: a SIGS `secs` override, else the BLE/WiFi default."""
    e = SIGS[sig]
    return e.get("secs") or (BLE_SECS if e.get("ble") else WIFI_SECS)


def positive_arm(h, cat_id, sig):
    """Run the DUT in `sig`'s mode, emit `sig`. Returns (delta, kalipi_ok, flock_serial_or_None, sent).
    `sent` (wifi_beacon frames-pushed, None for BLE) lets the caller tell "emitter radiated nothing"
    (CANNOT-TEST, rig) apart from "DUT didn't detect a real emission" (FAIL) -- the same dead-emit
    class the post-review flagged for the negative arm applies to the positive."""
    e = SIGS[sig]; wifi = not e.get("ble")
    open_tool(h, cat_id, e["item"], wifi)
    base = dc(h, e["ctr"])
    r = emit(sig, _secs(sig))
    time.sleep(1.0)
    final = dc(h, e["ctr"])
    ser = (h.detect_counts() or {}).get("flock_serial") if sig == "flock" else None
    h.tool_stop()
    return final - base, bool(isinstance(r, dict) and r.get("ok")), ser, _sent(r)


def _sent(r):
    """Frames the injector reported pushing (wifi_beacon returns `sent`; BLE emitters don't -> None)."""
    return r.get("sent") if isinstance(r, dict) else None


def negative_arm(h, cat_id_x, x_sig, cat_id_y, y_sig):
    """SAME-RUN discrimination negative with adjacent liveness (no external witness needed for the
    control, but see the docstring's HONEST RESIDUAL on transient in-window radiation):
      CONTROL: run Y in Y's OWN mode, emit Y -> Y's counter must rise THIS RUN (proves the emitter+path
        are live now; NOT importing Phase 1 at a different time).
      NEGATIVE: run X in X's mode, emit Y -> X's counter must stay flat.
    ⚠ Assumes the WiFi smart-counters are EDGE-triggered on fresh frames, not recomputed from the
    persistent AP list (which would let stale SSIDs from an earlier arm bump multissid). Held
    empirically (0 FAIL) but load-bearing for the WiFi negatives.
    Returns (x_delta, y_control_delta, ctrl_ok, neg_ok, sent_ctrl, sent_neg)."""
    ey = SIGS[y_sig]; wifi_y = not ey.get("ble"); secs_y = _secs(y_sig)
    ex = SIGS[x_sig]; wifi_x = not ex.get("ble")
    # CONTROL: Y in Y's mode -- prove Y radiates + is received THIS RUN.
    open_tool(h, cat_id_y, ey["item"], wifi_y)
    yb = dc(h, ey["ctr"])
    rc = emit(y_sig, secs_y)
    time.sleep(1.0)
    y_ctrl = dc(h, ey["ctr"]) - yb
    h.tool_stop()
    # NEGATIVE: X in X's mode, emit Y -> X must ignore it.
    open_tool(h, cat_id_x, ex["item"], wifi_x)
    xb = dc(h, ex["ctr"])
    rn = emit(y_sig, secs_y)
    time.sleep(1.0)
    x_delta = dc(h, ex["ctr"]) - xb
    h.tool_stop()
    return (x_delta, y_ctrl,
            bool(isinstance(rc, dict) and rc.get("ok")),
            bool(isinstance(rn, dict) and rn.get("ok")),
            _sent(rc), _sent(rn))


def esp_devices_text(h):
    """The FILTERED 'ESP devices:' log label from the active Espressif page (cb_poll_espressif,
    ui_nav.h:4278, emits ONE label 'ESP devices:\\n<ssid> [OUI] chN' or '(none yet)'), or ''.
    Scopes the match to the FILTER'S OWN output -- a future widget echoing a raw scanned SSID
    elsewhere on the page cannot then fake an ESP-list hit (post-review NIT)."""
    # ⚠ the harness field is "texts" (plural, test_harness.h:587 `"texts":[...]`), NOT "text".
    # (Reading "text" here silently returned '' -> false-negative on the Espressif positive; caught
    # by red-first when the scoped match went red where the whole-blob json.dumps search had passed.)
    for t in ((h.text() or {}).get("texts") or []):
        if isinstance(t, str) and "ESP devices" in t:
            return t
    return ""


def espressif_arms(h, an_cat):
    """Shape C: the OUI FILTER. POSITIVE = a nonce Espressif SSID appears in the FILTERED
    'ESP devices' label AND in the unfiltered ap_list (proves RX). NEGATIVE = a nonce rogueap
    SSID (OUI 00:13:37) appears in ap_list (RX) but is ABSENT from the ESP-devices label (filter
    rejects). Both emits' ok is captured so a dead rig -> CANNOT-TEST, not a FAIL blaming the DUT.
    Returns dict of observations."""
    espn = nonce("ESP"); rogn = nonce("RG")
    o = {}
    # POSITIVE
    open_tool(h, an_cat, 5, wifi=True)
    rp = emit(("wifi-beacon", "espressif"), WIFI_SECS, ssid=espn)
    time.sleep(0.5)
    o["pos_emit_ok"]    = bool(isinstance(rp, dict) and rp.get("ok"))
    o["pos_in_esptext"] = espn in esp_devices_text(h)
    o["pos_in_aplist"]  = espn in json.dumps(h.ap_list(espn) or {})
    h.tool_stop()
    # NEGATIVE: a rogueap-OUI beacon is a real AP (in ap_list) but must NOT pass the ESP filter.
    open_tool(h, an_cat, 5, wifi=True)
    r = emit(("wifi-beacon", "rogueap"), WIFI_SECS, ssid=rogn)
    time.sleep(0.5)
    o["neg_in_aplist"]  = rogn in json.dumps(h.ap_list(rogn) or {})   # proves RX (non-vacuous)
    o["neg_in_esptext"] = rogn in esp_devices_text(h)                 # must be False (filter worked)
    o["neg_emit_ok"]    = bool(isinstance(r, dict) and r.get("ok"))
    h.tool_stop()
    o["nonces"] = (espn, rogn)
    return o


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=os.environ.get("CLIPBOY_PORT", "COM5"))
    a = ap.parse_args()
    print("DETECT ORACLES -- DUT %s, emitter kalipi (DUT-self-witnessed; kali external witness optional)\n" % a.port)
    TS.deploy()

    h = Harness(port=a.port)
    results = {}   # name -> (verdict, detail)
    try:
        det = h.cat_pos("Detect"); an = h.cat_pos("Analyze")
        if det is None or an is None:
            print("CANNOT TEST: DUT lacks Detect/Analyze categories (det=%s an=%s)." % (det, an)); return 2

        # ── Phase 1: POSITIVES (self-witness) ──
        pos_ok = {}
        print("== Phase 1: positives (does each emitter radiate + each detector fire?) ==")
        for sig in ["rogueap", "eviltwin", "pwnagotchi", "flock", "airtag", "flipper"]:
            cat_id = det if SIGS[sig]["cat"] == "Detect" else an
            delta, kok, ser, sent = positive_arm(h, cat_id, sig)
            wifi = not SIGS[sig].get("ble")
            if not kok:
                v = "CANNOT-TEST"; d = "kalipi emit failed (rig, not DUT)"
            elif wifi and sent == 0:
                # Injector reported ok but pushed ZERO frames -> a flat counter is a dead-rig artifact,
                # NOT a detector miss. Distinguishes "emitter silent" from "DUT deaf" (the eviltwin FAIL
                # investigation: is multissid+0 a real miss or did scapy send nothing?).
                v = "CANNOT-TEST"; d = "%s: injector radiated 0 frames (sent=0) -- rig, not DUT" % SIGS[sig]["ctr"]
            elif sig == "flock":
                # IDENTITY proof = the detector parsed OUR real-capture device's serial. Assert the
                # parsed serial is a correct non-empty PREFIX of the true 16-char serial (>=15 chars,
                # so it is unmistakably the real serial, not garbage). Ambient-immune (no other device
                # carries serial TN720230220007..). This TOLERATES the KNOWN, separately-tracked flock
                # serial OFF-BY-ONE truncation (see flock_serial_offbyone / test_flock_serial.py:
                # adEnd = adStart + adLen is one short, dropping the last serial byte) WITHOUT
                # certifying it -- a FIXED parser emitting the full 16 chars still prefix-matches. It
                # is NOT this oracle's job to catch that truncation (that is test_flock_serial.py's);
                # this oracle proves the DETECTOR received + identified the specific device.
                ident = bool(ser) and FLOCK_SERIAL.startswith(ser) and len(ser) >= 15
                v = "PASS" if (delta >= POS_MIN and ident) else "FAIL"
                d = "flock +%d, serial=%r (prefix-of %r, ident=%s)" % (delta, ser, FLOCK_SERIAL, ident)
                if ident and len(ser) < 16:
                    d += "  [!] serial TRUNCATED to %d chars -- known off-by-one, test_flock_serial.py" % len(ser)
            else:
                v = "PASS" if delta >= POS_MIN else "FAIL"
                d = "%s +%d (want >=%d, sent=%s)" % (SIGS[sig]["ctr"], delta, POS_MIN, sent)
                # Bare counter delta on a KNOWN signature -> proves the signature is PRESENT in the
                # environment, NOT that OUR emitter produced it (ambient DEF CON traffic can supply it).
                # Only Flock/Espressif are identity-attributed. airtag/flipper are additionally low-SNR.
                d += ("  [DOWNGRADED: liveness+shape; positive may be AMBIENT, not our emit]"
                      if sig in DOWNGRADED else
                      "  [signature-present; may be AMBIENT, not identity-attributed to our emit]")
            pos_ok[sig] = (v == "PASS")
            results["pos:" + sig] = (v, d)
            print("  %-11s %-11s %s" % (sig, v, d))

        # ── Phase 2: NEGATIVES (discrimination) -- only where Y's positive established radiation ──
        print("\n== Phase 2: negatives (does X's mode IGNORE a proven-radiating Y?) ==")
        for x_sig, y_sig in NEG_PAIRS:
            cat_x = det if SIGS[x_sig]["cat"] == "Detect" else an
            cat_y = det if SIGS[y_sig]["cat"] == "Detect" else an
            if not pos_ok.get(x_sig):
                # A dead X-mode makes "X stayed +0 under Y" trivially true -> require X's counter be
                # PROVEN able to move (its own positive passed) or the negative is vacuous.
                v = "CANNOT-TEST"; d = "X=%s positive did not pass -> its counter not proven movable, negative vacuous" % x_sig
            elif not pos_ok.get(y_sig):
                v = "CANNOT-TEST"; d = "Y=%s positive did not pass -> radiation not pre-established" % y_sig
            else:
                xd, yctrl, ctrl_ok, neg_ok, sent_c, sent_n = negative_arm(h, cat_x, x_sig, cat_y, y_sig)
                wifi_y = not SIGS[y_sig].get("ble")
                if not (ctrl_ok and neg_ok):
                    v = "CANNOT-TEST"; d = "%s emit failed (ctrl_ok=%s neg_ok=%s) -- rig" % (y_sig, ctrl_ok, neg_ok)
                elif yctrl < POS_MIN:
                    # Same-run control: Y not received in its OWN mode this run -> emitter/path dead
                    # NOW; a flat X is then a dead-rig artifact, not discrimination.
                    v = "CANNOT-TEST"; d = "same-run control: %s NOT received in its own mode (+%d) -- emitter/path dead this run" % (y_sig, yctrl)
                elif wifi_y and (sent_n == 0 or sent_c == 0):
                    v = "CANNOT-TEST"; d = "injector radiated 0 frames (sent ctrl=%s neg=%s) -- rig, not DUT" % (sent_c, sent_n)
                else:
                    v = "PASS" if xd <= NEG_MAX else "FAIL"
                    d = "%s +%d under %s [Y ctrl +%d same-run, sent=%s] (want <=%d)" % (
                        SIGS[x_sig]["ctr"], xd, y_sig, yctrl, sent_n, NEG_MAX)
            results["neg:%s/%s" % (x_sig, y_sig)] = (v, d)
            print("  %-11s vs %-11s %-11s %s" % (x_sig, y_sig, v, d))

        # ── Espressif OUI filter (Shape C) ──
        print("\n== Espressif OUI filter (h.text filtered list) ==")
        o = espressif_arms(h, an)
        pos_c = o["pos_in_esptext"] and o["pos_in_aplist"]
        neg_c = o["neg_in_aplist"] and (not o["neg_in_esptext"])
        if not (o["pos_emit_ok"] and o["neg_emit_ok"]):
            v = "CANNOT-TEST"; d = "emit failed (pos_ok=%s neg_ok=%s) -- rig, not DUT" % (o["pos_emit_ok"], o["neg_emit_ok"])
        elif not o["neg_in_aplist"]:
            v = "CANNOT-TEST"; d = "negative SSID never reached ap_list -> RX not established, filter-reject is vacuous"
        elif not o["pos_in_aplist"]:
            v = "CANNOT-TEST"; d = "positive nonce never reached ap_list -> RX not established this run, filter-accept unprovable"
        else:
            v = "PASS" if (pos_c and neg_c) else "FAIL"
            d = ("pos: nonce in ESPtext=%s ap_list=%s | neg: in ap_list=%s in ESPtext=%s (want F)"
                 % (o["pos_in_esptext"], o["pos_in_aplist"], o["neg_in_aplist"], o["neg_in_esptext"]))
        results["espressif"] = (v, d)
        print("  espressif   %-11s %s" % (v, d))
    finally:
        try: h.tool_stop(); h.close()
        except Exception: pass

    # ── Verdict ──
    fails = [k for k, (v, _) in results.items() if v == "FAIL"]
    cants = [k for k, (v, _) in results.items() if v == "CANNOT-TEST"]
    passes = [k for k, (v, _) in results.items() if v == "PASS"]
    print("\n%d PASS, %d FAIL, %d CANNOT-TEST" % (len(passes), len(fails), len(cants)))
    if fails:
        print("FAIL:", ", ".join(fails)); return 1
    if not passes:
        print("CANNOT-TEST: no arm produced a positive result (dead rig?)"); return 2
    print("PASS (with %d cannot-test arms deferred)" % len(cants) if cants else "PASS -- all arms green")
    return 0


if __name__ == "__main__":
    sys.exit(main())
