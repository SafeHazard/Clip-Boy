#!/usr/bin/env python3
"""test_join_wifi.py -- does the badge's Join WiFi (Utilities 5,0) actually ASSOCIATE + get a DHCP
lease, or merely "run"? A comparative oracle on the badge's own join result -- NO external rig, NO
firmware change (TIER 1).

WHY THIS IS THE RIGHT OBSERVABLE (both pre-reviews, 2026-07-30): `th_cmd_wifijoin` returns
`joined:true` ONLY after `WiFiScan::joinWiFi` reaches `WiFi.status()==WL_CONNECTED` and pulls a DHCP
lease (WiFiScan.cpp:2118/2147); it returns false on the 20-try (~10s) timeout. So `joined` already
means associated+authenticated+DHCP -- not "the tool ran". And `th_cmd_wifijoin` (test_harness.h:1214)
is an HONEST PROXY for the UI tool: it runs the identical precondition dance (stop op / geiger
force-stop / clear promiscuous rx cb / audio suspend) and the SAME `cb.joinWiFi(ssid,pw)` core
(verified against ui_nav.h:5602). So this gates the exact regression the project fears most: the
documented WiFi-init / APSTA / promiscuous-clear order breaking -> joinWiFi returns false or crashes
Core 0 (CLAUDE.md "Critical Integration Notes").

ORACLE (comparative; the control CAN come out bad):
  POSITIVE: `wifijoin shipship shipship` -> joined:true  (real, joinable test AP).
  NEGATIVE/control: `wifijoin <fresh-nonce> badpass` -> joined:false  (a never-existent SSID; a
    joinWiFi that ALWAYS returned true would make this arm wrongly true -> FAIL, so the negative is
    the built-in falsifier).
  Disambiguation:
    - both true  -> wifijoin is broken/always-true -> FAIL.
    - both false -> shipship not joinable right now (down / out of range) -> CANNOT-TEST (rig), not FAIL.
    - airplane refusal ("blocked by airplane mode") we could not clear -> CANNOT-TEST.

⚠ Must send STX-framed (h.cmd does; a RAW `wifijoin shipship shipship` falls through to the plaintext
handler in Clip-Boy.ino which takes "shipship shipship" as ONE ssid + empty pw -> false FAIL). The
blocking join needs CLIPBOY_SESSION_TIMEOUT>=30 (nonce arm burns the full ~10s timeout).

HONEST RESIDUAL: TIER 1 proves "joined the AP named shipship + got DHCP", not "on the expected
192.168.111.0/24 subnet" (that needs localIP/gateway added to the wifijoin JSON = a TEST_HARNESS
firmware change -> Tier 2 follow-up). joinWiFi joins BY SSID, so joined:true is specifically shipship.

  py -3 scripts/tests/test_join_wifi.py --port COM5
"""
import argparse, os, sys, time, random, string

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ.setdefault("CLIPBOY_SESSION_TIMEOUT", "35")   # cb.joinWiFi blocks ~10s (20 tries x 500ms)
from harness import Harness

SSID = "shipship"       # owner test AP (SSID==pass), BSSID 38:2c:4a:69:1b:e0, ch11
PASS = "shipship"


def _joined(resp):
    """True/False from a wifijoin reply, or None if it errored / unreadable."""
    if not isinstance(resp, dict):
        return None
    if not resp.get("ok"):
        return None            # e.g. "blocked by airplane mode" -> None -> CANNOT-TEST
    return bool(resp.get("joined"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=os.environ.get("CLIPBOY_PORT", "COM5"))
    a = ap.parse_args()
    print("JOIN WIFI oracle -- DUT %s, target %s (badge self-reported join; no external rig)\n" % (a.port, SSID))

    # ⚠ The negative SSID MUST be a fresh nonce that is NOT shipship (nor any currently/previously
    # connected SSID). joinWiFi's fast path (WiFiScan.cpp:2087) returns joined:true for
    # `ssid == connected_network && WL_CONNECTED` WITHOUT re-checking the password -- so reusing
    # shipship with a bad password would FALSELY report joined:true and make the control vacuous.
    # A random nonce cannot be a real/known AP. Do NOT "simplify" this to a wrong-password-on-shipship.
    nonce = "nojoin_" + "".join(random.choice(string.ascii_lowercase + string.digits) for _ in range(8))
    assert nonce != SSID and not SSID.startswith(nonce), "negative SSID must not be shipship (WiFiScan.cpp:2087 fast-path)"
    h = Harness(port=a.port)
    airplane_was = None
    pos = neg = None
    try:
        # Precondition: airplane OFF (wifijoin refuses under airplane). Save + restore.
        st = h.cfg_get("airplane") or {}
        airplane_was = st.get("value")
        h.cfg_set("airplane", False); time.sleep(1.0)

        # NEGATIVE control FIRST (fresh nonce SSID can never exist -> must be joined:false).
        neg_resp = h.cmd("wifijoin %s badpass" % nonce)
        neg = _joined(neg_resp)
        print("NEGATIVE wifijoin %s -> %s" % (nonce, neg_resp))
        time.sleep(1.0)

        # POSITIVE (real joinable AP -> must be joined:true).
        pos_resp = h.cmd("wifijoin %s %s" % (SSID, PASS))
        pos = _joined(pos_resp)
        print("POSITIVE wifijoin %s -> %s" % (SSID, pos_resp))
    finally:
        # Leave the radio off shipship + restore airplane so a following test isn't perturbed.
        try:
            if airplane_was is not None:
                h.cfg_set("airplane", bool(airplane_was))
        except Exception:
            pass
        try: h.close()
        except Exception: pass

    # Verdicts (pre-registered).
    if pos is None:
        print("\nCANNOT-TEST: positive wifijoin errored/unreadable (airplane not cleared?) -- rig, not badge")
        return 2
    if neg is None:
        print("\nCANNOT-TEST: negative wifijoin errored/unreadable -- rig")
        return 2
    if pos is False and neg is False:
        print("\nCANNOT-TEST: shipship not joinable now (both arms false) -- AP down/out of range, not a badge fault")
        return 2
    if pos is True and neg is True:
        print("\nFAIL: wifijoin returned joined:true for a NEVER-EXISTENT SSID -- the join result is not real")
        return 1
    ok = (pos is True and neg is False)
    print("\nVERDICT: %s" % (
        "PASS -- Join WiFi ASSOCIATES + gets DHCP on shipship (joined:true), and correctly reports "
        "joined:false for a nonexistent SSID (control)." if ok else
        "FAIL -- pos joined=%s (want True), neg joined=%s (want False)" % (pos, neg)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
