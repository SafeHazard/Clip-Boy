#!/usr/bin/env python3
"""test_sae_sniff_rx.py -- does Analyze > SAE Commit SNIFF actually RECEIVE SAE commit frames?

Closes the census's ONE truly-untested SHIPPING (Sn34k) tool: Analyze > SAE Commit sniff
(cat Analyze item 6 = cb.sniffSAE). It is a passive detector present in BOTH SKUs. Two-badge
test (kalipi has NO SAE injector, verified):
  EMITTER = a Res34rch badge running SAE Commit Flood (cat SAE item 0) -- proven to radiate
           auth subtype 0x0b alg==3 by test_sae_witness.py.
  DUT     = the Sn34k badge running the SAE sniff; its pkt_counters.sae must climb.

⚠ Two mechanism facts drive the design (verified in WiFiScan.cpp, 2026-07-29):
  1. `saeAttackLoop` (:7992) transmits ONLY for a SELECTED AP, on that AP's channel, with a
     RANDOM source MAC (:7999 generateRandomMac). => the EMITTER must ap_scan/select shipship
     (ch11) first, or nothing radiates.
  2. `getSAEACT` (:8198 filterActive): if the DUT has an AP SELECTED, it counts only SAE frames
     whose SOURCE MAC == a selected BSSID. The flood's src MAC is RANDOM, so a DUT with any AP
     selected would count ZERO => false FAIL. => the DUT must sniff with NO AP selected. We
     REBOOT the DUT to guarantee that deterministically (a reboot clears any persisted selection;
     the DUT has no ap_list reader to assert it otherwise).

ORACLE: comparative -- DUT `sae` delta ON (emitter flooding) vs OFF (emitter idle). Ambient SAE
~0 (WPA3 auth only, not periodic), so ON>>OFF. LIVENESS: an independent kali witness counts
alg==3 frames on ch11; on_witness < MIN => the emitter didn't radiate => CANNOT-TEST, never FAIL
(the exact "sent != received; prove the stimulus landed" rule). NEGATIVE: OFF arm witness must be
~0 (proves "0 received" means "none sent", not "DUT deaf").

  py -3 scripts/tests/test_sae_sniff_rx.py --dut COM4 --emit COM5

DUT defaults to COM4 (the Sn34k build -- the SKU this gap is about); emitter to COM5 (Res34rch).
"""
import argparse, os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ.setdefault("CLIPBOY_SESSION_TIMEOUT", "45")
from harness import Harness
import tool_suite as TS
import rf_pcap

SSID, CH, BSSID = "shipship", 11, "38:2c:4a:69:1b:e0"
SAE_FILTER = "wlan.fc.type_subtype==0x0b && wlan.fixed.auth.alg==3"
PCAP = "/tmp/sae_sniff.pcap"
WIN = 45                 # DUT SAE sniff HOPS 1-14 (~250ms/ch); ~7% duty on ch11, so widen the window
MIN_WITNESS = 5          # SAE is EC-crypto-heavy -> low rate (matches test_sae_witness MIN_SAE)
MIN_DUT = 2              # DUT-received SAE commits: heavily diluted by the ~7% ch11 hop duty, so the
                         # DUT catches ~1/14 of what the continuous witness sees. The SIGNAL is
                         # on>=MIN_DUT WITH a strict-zero OFF arm (ambient SAE ~0), not a big ratio.
                         # Pre-registered conservative; validate red-first and adjust w/ evidence.


def _cap_start():
    rf_pcap._ssh("sudo iw dev wlan0 set channel %d" % CH)
    rf_pcap._ssh("sudo rm -f %s" % PCAP)
    rf_pcap._ssh("sudo sh -c 'nohup tcpdump -i wlan0 -w %s -U </dev/null >/dev/null 2>&1 & "
                 "echo $! > %s.pid'" % (PCAP, PCAP))
    time.sleep(1)


def _cap_stop():
    rf_pcap._ssh("sudo sh -c 'kill -TERM $(cat %s.pid) 2>/dev/null'; sleep 1" % PCAP)


def _wcount(dfilter):
    rc, out = rf_pcap._ssh("tshark -r %s -Y '%s' 2>/dev/null | wc -l" % (PCAP, dfilter), timeout=120)
    try:
        return int(out.strip().split()[-1])
    except Exception:
        return None


def _sae_delta(h, window):
    c0 = (h.pkt_counters() or {}).get("sae")
    time.sleep(window)
    c1 = (h.pkt_counters() or {}).get("sae")
    return None if (c0 is None or c1 is None) else c1 - c0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dut", default="COM4", help="Sn34k badge running the SAE sniff")
    ap.add_argument("--emit", default="COM5", help="Res34rch badge running the SAE flood")
    # The oracle runner (run_oracle_suite.run_one) appends '--port <DUT>' to EVERY test; this
    # two-badge test used only --dut/--emit, so argparse exited rc=2 -> the runner read that as
    # CANNOT-TEST and the test NEVER RAN (a false "fixture absent"). Accept --port as an alias
    # for --dut (the DUT is the Sn34k sniffer the runner assigns). SUPPRESS so a standalone
    # invocation without --port keeps --dut's default instead of clobbering it to None.
    ap.add_argument("--port", dest="dut", default=argparse.SUPPRESS,
                    help="alias for --dut (the oracle runner passes --port)")
    a = ap.parse_args()
    print("SAE-SNIFF RX -- DUT %s (sniff), EMIT %s (flood), target %s ch%d\n" % (a.dut, a.emit, SSID, CH))
    TS.deploy()
    if not rf_pcap.preflight(chan=CH):
        print("CANNOT TEST: kali witness preflight failed -- radiation control unavailable.")
        return 2

    # EXPLICIT ports on BOTH sessions; never set CLIPBOY_PORT (single global -> both would
    # fall through to the same badge). Separate bridge subprocesses, no collision.
    dut = Harness(port=a.dut)
    emit = Harness(port=a.emit)
    try:
        if emit.cat_pos("SAE") is None:
            print("CANNOT TEST: emitter %s is not a Res34rch build (no SAE flood tool)." % a.emit)
            return 2
        # EMITTER: select shipship (ch11) so saeCommitFlood has a target + channel.
        sel = emit.ap_scan(SSID)
        if sel.get("selected", -1) < 0:
            print("CANNOT TEST: emitter could not find/select '%s' (ap out of range?)." % SSID)
            return 2
        # DUT: reboot => guaranteed NO AP selected => getSAEACT filter off => counts all SAE.
        dut.reboot_and_wait(timeout=30); time.sleep(2)
        an = dut.cat_pos("Analyze")
        if an is None:
            print("CANNOT TEST: DUT has no Analyze category."); return 2
        dut.cmd("tool_open %d 6" % an)          # Analyze > SAE Commit sniff
        time.sleep(2)

        # OFF arm: emitter idle. DUT delta should be ~0; witness must be ~0 (proves "none sent").
        _cap_start()
        off = _sae_delta(dut, WIN)
        _cap_stop()
        off_wit = _wcount(SAE_FILTER)
        off_tot = _wcount("")                    # liveness: witness captured SOMETHING

        # ON arm: emitter floods SAE on ch11 (random src MAC). Witness confirms radiation.
        sae = emit.cat_pos("SAE")
        emit.cmd("tool_open %d 0" % sae)         # SAE Commit Flood
        time.sleep(2)
        _cap_start()
        on = _sae_delta(dut, WIN)
        _cap_stop()
        on_wit = _wcount(SAE_FILTER)
    finally:
        # tool_stop() MUST be in finally: an exception between _cap_start and here (e.g. an ssh
        # timeout in _wcount/_sae_delta) would otherwise leave the Res34rch emitter FLOODING SAE
        # indefinitely -- close() quits the serial session but does NOT stop the running tool.
        # That is an active-TX spectrum-contamination hazard for every later test, not just a leak.
        try: emit.tool_stop()
        except Exception: pass
        try: dut.tool_stop()
        except Exception: pass
        try: dut.close()
        except Exception: pass
        try: emit.close()
        except Exception: pass
        try: rf_pcap._ssh("sudo sh -c 'kill -TERM $(cat %s.pid) 2>/dev/null'" % PCAP)
        except Exception: pass

    print("OFF: DUT sae delta=%s, witnessed alg3=%s (total frames=%s)" % (off, off_wit, off_tot))
    print("ON : DUT sae delta=%s, witnessed alg3=%s" % (on, on_wit))

    if off is None or on is None:
        print("\nCANNOT-TEST: DUT sae counter unread (off=%s on=%s)" % (off, on)); return 2
    if on_wit is None or on_wit < MIN_WITNESS:
        print("\nCANNOT-TEST: SAE flood did NOT radiate (kali witnessed %s alg3 frames) -- rig, not DUT"
              % on_wit); return 2
    if off_wit and off_wit > 0:
        print("\nCANNOT-TEST: OFF arm saw %d alg3 frames on air (ambient SAE?) -- negative control dirty"
              % off_wit); return 2
    if not off_tot:
        # The OFF-arm witness captured NOTHING -> the capture silently failed, so off_wit==0 is
        # vacuous (not "clean air"). Assert the observable actually read, don't just print it.
        print("\nCANNOT-TEST: OFF-arm witness captured 0 total frames -- capture failed, "
              "negative control unverifiable"); return 2
    # Signal = ON>=MIN_DUT with a STRICT-ZERO OFF arm (ambient SAE ~0). The DUT is hop-diluted so a
    # large ratio isn't expected; a clean zero OFF is what makes even a few ON frames decisive.
    ok = on >= MIN_DUT and off == 0
    print("\nVERDICT: %s" % (
        "PASS -- Sn34k SAE sniff RECEIVES (DUT sae ON=%d OFF=%d; kali-witnessed %d radiated)"
        % (on, off, on_wit) if ok else
        "FAIL -- DUT sae ON=%d OFF=%d (need ON>=%d, OFF==0) though %d SAE radiated"
        % (on, off, MIN_DUT, on_wit)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
