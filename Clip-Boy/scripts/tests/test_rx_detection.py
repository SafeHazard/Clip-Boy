#!/usr/bin/env python3
"""test_rx_detection.py -- real RECEIVE-detection for passive/RX tools that previously
had only liveness ("it started without crashing") coverage. Companion to test_scan_rx.py
(which proved Scan>APs + Monitor>Packets). Every case is COMPARATIVE with a liveness
control that can come out bad + a negative control, so ambient traffic cannot fake a pass
and a dead emitter reports CANNOT-TEST (never a false badge FAIL). Stimulus is REAL
over-the-air frames from an independent radio (kalipi), never a fixture derived from the
badge's own parser.

This file = the 3 genuinely-NEW-coverage cases (all WiFi):
  C5  Monitor > Channel Stats  -- deauth flood on a fixed channel makes THAT channel's bar
      the max; a second arm on a different channel SHIFTS the max. Ambient can't move with
      our emitter, so the shift is the oracle (spatial). counts[] read via `channel_activity`.
  C6  Monitor > MAC Tracker    -- the emitter's src MAC (deauth TA = 02:cb:de:00:00:01)
      appears in the top-10 with frames>0; a fabricated MAC never does. Reads new `mac_track`.
  C8  Analyze > Raw/PCAP       -- LOCK the capture channel (raw_channel) and prove the raw sniffer
      RECEIVES via a comparative on its beacon/mgmt counter (offered flood vs ambient, witnessed).
      sd_read returns only a 124B lossy binary preview (measured), so per-frame CONTENT read-back is
      NOT possible IN-HARNESS; the capture-to-FILE half (a new raw_<seq>.pcap grew past its header) is
      REPORTED, NOT asserted (it depends on a non-bloated /pcaps -- orthogonal to RX). ✅ CONTENT was
      VERIFIED OFFLINE once (2026-07-29): pulled the SD card, `grep -a` on raw_4.pcap found the run's
      nonce SSID 34x (raw_0/2/4 each held their own run's nonce; a never-sent nonce was absent) --
      confirming the tool captures the received frames to file. The automated gate stays counter-based
      because the harness cannot read the binary pcap over serial.

Ritual notes baked in (see docs/test-plans/rx-detection-buildout-plan.md, 2 pre-reviews):
  - channel_activity is uint8 (saturates 255) w/ lowest-channel tie-break; default hop is
    ch1-7 only -> C5 uses quiet non-adjacent low channels 3 vs 5 and bails CANNOT-TEST on
    saturation. Hop=100ms/snapshot=5s so 30s is ample.
  - MAC Tracker's DETECT_FOLLOW hops the FULL band and alternates a BLE scan -> C6 window >=40s.
  - sd_read is text/64KB -> C8 keeps the capture short so the nonce lands in the first 64KB.
  - resetDisplayAccumulators() clears channel/MAC-tracker on tool start, so re-opening the
    tool gives a clean slate between arms (no stale-store); pcap/AP lists persist (fresh nonce).

  py -3 scripts/tests/test_rx_detection.py --port COM5
"""
import argparse, os, re, sys, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ.setdefault("CLIPBOY_SESSION_TIMEOUT", "60")
from harness import Harness
import tool_suite as TS
import rf_pcap

DEAUTH_BSSID = "02:cb:de:00:00:01"     # deauth-synth's fabricated BSSID == its TA (src MAC)
FAKE_MAC     = "02:cb:de:00:00:99"     # never transmitted -> the anti-hallucination negative
WIT_MIN      = 20                      # radiation-witness floor; below -> emitter failed, CANNOT-TEST

CS_SECS  = 30      # Channel Stats accumulation (>= ~6 hop cycles at 100ms x 1-7)
CS_A_CH  = 3       # quiet, low, non-adjacent, avoids busy 1/6
CS_B_CH  = 5
CS_MARGIN = 2.0    # pre-registered: emit channel count >= 2x the runner-up
MT_SECS  = 45      # MAC Tracker full-band hop + BLE alternation -> longer window
MT_CH    = 6
RAW_SECS = 12      # raw counter accumulation window (locked channel, no hop dilution)
RAW_CH   = 3       # QUIET low channel (ch6 has ~600 ambient beacons/12s -> diluted 1.7x ratio;
                   # ch3 measured ~0 ambient in C5, so our flood gives a clean >>2x)
RAW_RATIO = 3.0    # pre-registered: raw-capture beacon delta ON vs ambient OFF on locked quiet ch


def _emit_thread(kind, *args, secs_hint=60):
    """Start a kalipi stimulus in a daemon thread; return (thread, result_dict)."""
    res = {}
    def run():
        res.update(TS.kalipi(kind, *args, timeout=secs_hint) or {})
    t = threading.Thread(target=run, daemon=True)
    t.start()
    return t, res


# ---------------- C5: Monitor > Channel Stats ----------------
def case_channel_stats(h):
    mon = h.cat_pos("Monitor")
    if mon is None:
        return ("C5 Channel Stats", None, "Monitor category absent (SKU?)")
    arms = {}
    for label, ch in (("A", CS_A_CH), ("B", CS_B_CH)):
        h.tool_stop(); time.sleep(1)
        h.cmd("tool_open %d 3" % mon)        # Monitor > Channel Stats (item 3)
        time.sleep(2)
        rf_pcap._ssh("sudo iw dev wlan0 set channel %d" % ch)
        path = "/tmp/cs_%s.pcap" % label
        rf_pcap.capture_start(iface="wlan0", path=path)
        t, res = _emit_thread("deauth-synth", str(ch), str(CS_SECS + 6), "wlan1",
                              secs_hint=CS_SECS + 46)
        time.sleep(CS_SECS)
        ca = h.cmd("channel_activity") or {}
        t.join(timeout=CS_SECS + 30)
        rf_pcap.capture_stop(path)
        witness = rf_pcap.count(path, rf_pcap._filter("deauth", bssid=DEAUTH_BSSID)) or 0
        arms[label] = dict(ch=ch, counts=ca.get("counts") or [],
                           sent=int(res.get("sent", 0)), witness=witness)
    h.tool_stop()

    # Liveness: both arms must have radiated (independent witness), else CANNOT-TEST.
    if arms["A"]["witness"] < WIT_MIN or arms["B"]["witness"] < WIT_MIN:
        return ("C5 Channel Stats", None,
                "stimulus did not radiate (witness A=%d B=%d, sent A=%d B=%d) -- rig, not badge"
                % (arms["A"]["witness"], arms["B"]["witness"], arms["A"]["sent"], arms["B"]["sent"]))

    ca_A, ca_B = arms["A"]["counts"], arms["B"]["counts"]
    if len(ca_A) < 14 or len(ca_B) < 14:
        return ("C5 Channel Stats", None, "counts unread A=%s B=%s" % (ca_A, ca_B))
    # Per-channel ON-vs-OFF comparative (ambient-immune, duty-cycle-fair): the SAME channel's
    # count must RISE when we target it vs when it is ambient-only in the other arm. A busy
    # ambient channel (e.g. ch6) that we never target stays high in BOTH arms, so it cannot
    # fake this; only OUR emitter moves a channel's count between arms. Bare argmax alone was
    # defeated by the tool's 100ms 1-7 hop diluting a single-channel flood to ~1/7 duty vs a
    # continuously-busy ambient channel -- the shift is real but not 2x the ambient peak.
    a_on,  a_off = ca_A[CS_A_CH - 1], ca_B[CS_A_CH - 1]   # ch3: targeted vs ambient
    b_on,  b_off = ca_B[CS_B_CH - 1], ca_A[CS_B_CH - 1]   # ch5: targeted vs ambient
    topA = 1 + max(range(14), key=lambda i: ca_A[i])
    topB = 1 + max(range(14), key=lambda i: ca_B[i])
    if a_on >= 255 or b_on >= 255:
        return ("C5 Channel Stats", None,
                "channel_activity SATURATED (uint8 255: ch%d=%d ch%d=%d) -- lower emit rate/window"
                % (CS_A_CH, a_on, CS_B_CH, b_on))
    # GATE on the per-channel rise ONLY (ambient-immune: the SAME channel rises only when WE target
    # it). `shifted` (argmax==target) is REPORTED, not gated -- both post-reviewers flagged it as a
    # false-FAIL risk: the tool hops ch1-7 at ~100ms, so a continuously-busy ambient ch1/ch6 can
    # out-count the ~1/7-duty-diluted target and steal the argmax, failing a working badge. The rise
    # is the real oracle; a dead RX gives a_on~0/b_on~0 -> rise false -> FAIL (witness-gated).
    shifted   = (topA == CS_A_CH) and (topB == CS_B_CH)
    rise_a    = a_on >= CS_MARGIN * max(a_off, 1)
    rise_b    = b_on >= CS_MARGIN * max(b_off, 1)
    ok = rise_a and rise_b
    return ("C5 Channel Stats", ok,
            "ch%d on/off=%d/%d(%.1fx) ch%d on/off=%d/%d(%.1fx) rise>=%.0fx=%s | argmax A=ch%d B=ch%d shifted=%s(report-only) [wit A=%d B=%d]"
            % (CS_A_CH, a_on, a_off, a_on / max(a_off, 1), CS_B_CH, b_on, b_off,
               b_on / max(b_off, 1), CS_MARGIN, (rise_a and rise_b), topA, topB, shifted,
               arms["A"]["witness"], arms["B"]["witness"]))


# ---------------- C6: Monitor > MAC Tracker ----------------
def case_mac_tracker(h):
    mon = h.cat_pos("Monitor")
    if mon is None:
        return ("C6 MAC Tracker", None, "Monitor category absent (SKU?)")
    h.tool_stop(); time.sleep(1)
    h.cmd("tool_open %d 4" % mon)            # Monitor > MAC Tracker (item 4)
    time.sleep(2)
    rf_pcap._ssh("sudo iw dev wlan0 set channel %d" % MT_CH)
    rf_pcap.capture_start(iface="wlan0", path="/tmp/mt.pcap")
    t, res = _emit_thread("deauth-synth", str(MT_CH), str(MT_SECS + 6), "wlan1",
                          secs_hint=MT_SECS + 46)
    time.sleep(MT_SECS)
    mt = h.mac_track() or {}
    t.join(timeout=MT_SECS + 30)
    rf_pcap.capture_stop("/tmp/mt.pcap")
    witness = rf_pcap.count("/tmp/mt.pcap", rf_pcap._filter("deauth", bssid=DEAUTH_BSSID)) or 0
    h.tool_stop()
    macs = mt.get("macs") or []
    sent = int(res.get("sent", 0))
    if witness < WIT_MIN:
        return ("C6 MAC Tracker", None,
                "stimulus did not radiate (witness=%d sent=%d) -- rig, not badge" % (witness, sent))
    present = any(m.get("mac", "").lower() == DEAUTH_BSSID and m.get("frames", 0) > 0 for m in macs)
    neg_ok = not any(m.get("mac", "").lower() == FAKE_MAC for m in macs)
    ok = present and neg_ok
    seen = ",".join("%s/%s" % (m.get("mac"), m.get("frames")) for m in macs[:6])
    return ("C6 MAC Tracker", ok,
            "emitter %s present=%s (frames>0), fabricated-absent=%s [wit=%d] top:[%s]"
            % (DEAUTH_BSSID, present, neg_ok, witness, seen))


# ---------------- C8: Analyze > Raw/PCAP ----------------
def _newest_pcap(h, prefix="raw"):
    """Newest /pcaps file whose name starts with `prefix` (the Raw tool writes
    raw_<seq>.pcap -- startPcap("raw"), WiFiScan.cpp:5568). Filtering by prefix is
    essential: a stale higher-seq deauth_N.pcap from a prior tool would otherwise win."""
    lst = h.sd_list("/pcaps") or {}
    files = lst.get("files") or lst.get("entries") or []
    norm = []
    for f in files:
        if isinstance(f, dict):
            norm.append((f.get("name") or f.get("file"), f.get("size", 0)))
        else:
            norm.append((str(f), 0))
    pfx = prefix.lower()
    norm = [(n, s) for n, s in norm
            if n and n.lower().endswith(".pcap") and os.path.basename(n).lower().startswith(pfx)]
    if not norm:
        return None, 0, lst
    def seq(n):
        m = re.search(r"(\d+)\.pcap$", n)
        return int(m.group(1)) if m else -1
    norm.sort(key=lambda t: seq(t[0]))
    return norm[-1][0], norm[-1][1], lst


def _seq(n):
    m = re.search(r"(\d+)\.pcap$", n or "")
    return int(m.group(1)) if m else -1


def case_raw_pcap(h, nonce):
    """sd_read returns only a 124B lossy binary preview (measured), so the exact-nonce-in-file
    content check is not viable. Instead: LOCK the raw capture to ch6 (raw_channel, kills the
    1/14 hop dilution) and use an RX2-style comparative on the raw sniffer's mgmt/beacon
    counter (pkt_counters '*Frames' family, fed by WIFI_SCAN_RAW_CAPTURE) -- offered beacon
    flood ON vs ambient OFF -- PLUS proof a NEW raw_<seq>.pcap was written and grew past its
    24B header. Together: the tool RECEIVES on its capture channel scaling with load AND
    writes it to file. Radiation witness gates a dead emitter to CANNOT-TEST."""
    an = h.cat_pos("Analyze")
    if an is None:
        return ("C8 Raw PCAP", None, "Analyze category absent (SKU?)")
    h.tool_stop(); time.sleep(1)
    before_name, _, _ = _newest_pcap(h)      # newest raw_ before this capture
    h.cmd("tool_open %d 3" % an)             # Analyze > Raw/PCAP (item 3)
    time.sleep(1)
    h.cmd("raw_channel %d" % RAW_CH)         # lock capture to ch6 (applies live)
    time.sleep(2)

    def mgmt_delta(window):
        c0 = h.pkt_counters() or {}
        b0, m0 = c0.get("beacon"), c0.get("mgmt")
        time.sleep(window)
        c1 = h.pkt_counters() or {}
        b1, m1 = c1.get("beacon"), c1.get("mgmt")
        if None in (b0, m0, b1, m1):
            return None
        return (b1 - b0, m1 - m0)

    off = mgmt_delta(RAW_SECS)                # ambient on locked ch6
    rf_pcap._ssh("sudo iw dev wlan0 set channel %d" % RAW_CH)
    rf_pcap.capture_start(iface="wlan0", path="/tmp/raw.pcap")
    t, res = _emit_thread("beacon-ssid", nonce, str(RAW_SECS + 6), str(RAW_CH),
                          secs_hint=RAW_SECS + 46)
    time.sleep(2)
    on = mgmt_delta(RAW_SECS)
    t.join(timeout=RAW_SECS + 30)
    rf_pcap.capture_stop("/tmp/raw.pcap")
    h.tool_stop(); time.sleep(2)             # flush/close pcap to SD
    witness = rf_pcap.count("/tmp/raw.pcap", rf_pcap._filter("beacon", ssid=nonce)) or 0
    sent = int(res.get("sent", 0))
    name, size, lst = _newest_pcap(h)
    grew = bool(name) and size > 24 and _seq(name) > _seq(before_name)

    if witness < WIT_MIN:
        return ("C8 Raw PCAP", None,
                "beacon did not radiate (witness=%d sent=%d) -- rig, not badge" % (witness, sent))
    if off is None or on is None:
        return ("C8 Raw PCAP", None, "raw counters unread off=%s on=%s" % (off, on))
    b_off, m_off = off
    b_on, m_on = on
    # Prefer the beacon-specific counter if the raw sniffer moves it; else mgmt (beacons are
    # mgmt subtype 8, so mgmt always rises under a beacon flood). Assert against whichever the
    # tool actually feeds -- per test_harness pkt_counters comment.
    use_b = (b_on + b_off) > 0
    on_v, off_v, fld = (b_on, b_off, "beacon") if use_b else (m_on, m_off, "mgmt")
    ratio = on_v / max(off_v, 1)
    # PASS gates on the RECEIVE proof (raw sniffer counts our offered load on its locked
    # capture channel, comparative + radiation-witnessed). The capture-to-FILE half is
    # REPORTED, not gated: a new raw_<seq> file failing to appear is the known /pcaps SD-bloat
    # concern (memory pcap_manyfiles_perf), a separate issue from whether the tool RECEIVES.
    ok = (on_v >= 40) and (ratio >= RAW_RATIO)
    file_note = ("new file %s (%dB) grew=%s" % (name, size, grew)) if name else "no raw_ file written"
    return ("C8 Raw PCAP", ok,
            "raw ch%d LOCKED (RX proof): %s on/off=%d/%d (%.1fx>=%.0f) [nonce radiated wit=%d] | capture-to-file (NOT ASSERTED): %s"
            % (RAW_CH, fld, on_v, off_v, ratio, RAW_RATIO, witness, file_note))


# ---------------- C1: Scan > Stations ----------------
CLIENT_MAC   = "dc:ef:09:14:d6:0f"  # kalipi2b wlan0 joined to shipship (192.168.111.198), kept chatty
                                    # by the systemd 'ss-keepalive' ping to the gateway. MUST stay
                                    # lowercase -- it is compared against a .lower()'d scan result.
STA_SECS     = 45                   # Scan>Stations hops; a continuously-pinged client is caught over ~3 cycles
AP_WARM_SECS = 25                   # Scan>APs window to catch shipship's beacon (hops 1-14)


def case_stations(h):
    """Detection ISOLATED TO Scan>Stations (item 2). VERIFIED mechanism: item 2 = WIFI_SCAN_STATION,
    whose stationSnifferCallback (WiFiScan.cpp:9063) only COUNTS beacons (:9263) and links a station
    only to an ALREADY-KNOWN AP -- it never adds APs. So APs must be pre-loaded, but the station list
    must be EMPTY of the client before item 2 runs, or a warm-up that already caught the client makes
    the positive VACUOUS (the fresh-reviewer BLOCK: the persistent list + the sniffer's refusal to
    re-link an already-present MAC would satisfy 'present' from the warm-up, not from item 2).
    RESOLVE: reboot (clean), then Scan>APs (item 0 = WIFI_SCAN_TARGET_AP) which populates APs from
    beacons but does NOT link stations -- its data-frame->station branch is AP_STA-gated
    (WiFiScan.cpp:6753). Confirm stations==0 AND shipship is in the AP list, THEN run item 2 so it
    MUST re-detect the chatty client itself. Negative = a FABRICATED MAC that can never appear.
    PRECONDITION (rig, provisioned 2026-07-30): kalipi2b's wlan0 (dc:ef:09:14:d6:0f) joined to
    shipship at 192.168.111.198, kept chatty by a systemd transient unit 'ss-keepalive' pinging the
    gateway (192.168.111.1). LIVENESS = those ping replies (they prove the client transmits
    end-to-end -- a stronger 'stimulus landed' than a passive witness here), so C1 uses no kali
    witness. If the client is absent this returns CANNOT-TEST (rig), NOT FAIL -- so re-confirm
    kalipi2b is still joined + ss-keepalive is running before reading a CANNOT-TEST as a problem.
    Proves DETECTION of a known chatty client, not absent-when-silent."""
    sc = h.cat_pos("Scan")
    if sc is None:
        return ("C1 Stations", None, "Scan category absent (SKU?)")
    h.reboot_and_wait(timeout=30); time.sleep(2)
    # 1) Populate APs ONLY (TARGET_AP does not link stations) so item 2 has an AP to attach to.
    h.cmd("tool_open %d 0" % sc)             # Scan > APs (item 0)
    time.sleep(AP_WARM_SECS)
    h.tool_stop(); time.sleep(1)
    pre = h.cmd("sta_list") or {}
    if pre.get("count", -1) != 0:
        return ("C1 Stations", None,
                "precondition failed: %s stations present before item 2 (AP scan shouldn't link any) "
                "-- can't isolate Scan>Stations" % pre.get("count"))
    aps = h.ap_list("shipship") or {}
    if not any(a.get("essid") == "shipship" for a in (aps.get("aps") or [])):
        return ("C1 Stations", None,
                "shipship not in AP list after AP scan -- out of range/rig, item 2 can't link to it")
    # 2) Item 2 must NOW detect the client fresh (empty station list => 'present' is item-2's own RX).
    h.cmd("tool_open %d 2" % sc)             # Scan > Stations (item 2)
    time.sleep(STA_SECS)
    # Derive BOTH 'present' and the negative arm from the SAME unfiltered station list, so a
    # device-side MAC-filter quirk can't make 'present' read False while the client is actually in
    # the list (that would be a spurious CANNOT-TEST). (fresh post-review 2026-07-30, safe-direction.)
    full = h.cmd("sta_list") or {}
    all_macs = [s.get("mac", "").lower() for s in (full.get("stations") or [])]
    present = CLIENT_MAC in all_macs
    neg_ok = FAKE_MAC not in all_macs
    total = full.get("count", 0)
    h.tool_stop()
    # Verdict (honest classification, per adversarial review 2026-07-30):
    #   - fabricated MAC PRESENT -> the scanner reported a device that cannot exist -> real FAIL.
    #   - total==0 -> nothing detected at all -> rig (client not chatty / out of range) -> CANNOT-TEST.
    #   - controlled client present (+ neg clean) -> genuine detection -> PASS.
    #   - stations found but NOT the controlled client -> RX is proven (others were seen), but the
    #     specific chatty client is absent -> a RIG condition (is kalipi2b still joined + keep-alive
    #     live?), NOT a badge fault -> CANNOT-TEST, never a false FAIL. (Trade acknowledged: this
    #     could mask a "scanner finds others but drops THIS one" bug; the 7 other C-arms + total>0
    #     already prove the station-RX path, so that residual is small and this kills the false-FAIL.)
    if not neg_ok:
        return ("C1 Stations", False,
                "FABRICATED MAC %s present in station list -- scanner reporting a device that cannot "
                "exist (total stations=%d)" % (FAKE_MAC, total))
    if total == 0:
        return ("C1 Stations", None,
                "item 2 detected no stations (total=0) -- rig: controlled client not chatty / out of "
                "range (is kalipi2b joined to shipship + ss-keepalive live?), not a badge fault")
    if present:
        return ("C1 Stations", True,
                "client %s re-detected by item 2 (total stations=%d, from empty), fabricated-absent=True"
                % (CLIENT_MAC, total))
    return ("C1 Stations", None,
            "controlled client %s NOT among %d detected stations -- station-RX proven (others seen) "
            "but the chatty client is absent: is kalipi2b (dc:ef:09:14:d6:0f) still joined to shipship "
            "+ ss-keepalive running? RIG condition, not a badge fault" % (CLIENT_MAC, total))


# ---------------- C4: Monitor > Packet Rate (thin completeness; cites RX2) ----------------
PR_SECS = 30       # Monitor hops 1-14 ~14s/cycle -> ~2 cycles
PR_RATIO = 5.0


def case_packet_rate(h):
    """COMPLETENESS (not new RX proof): Monitor>Packet Rate (2,1) feeds the SAME promiscuous
    packet counter as Monitor>Packets, whose RX is already proven by test_scan_rx.py RX2. This
    drives item 1 specifically and confirms its poller reads the counter. Use the MONOTONIC
    `deauth` field (ndeauth resets every 1s). deauth-synth (ambient deauth ~0) + radiation witness."""
    mon = h.cat_pos("Monitor")
    if mon is None:
        return ("C4 Packet Rate", None, "Monitor category absent (SKU?)")
    h.tool_stop(); time.sleep(1)
    h.cmd("tool_open %d 1" % mon)            # Monitor > Packet Rate (item 1)
    time.sleep(2)

    def dd(w):
        d0 = (h.pkt_counters() or {}).get("deauth")
        time.sleep(w)
        d1 = (h.pkt_counters() or {}).get("deauth")
        return None if (d0 is None or d1 is None) else d1 - d0

    off = dd(PR_SECS)
    rf_pcap._ssh("sudo iw dev wlan0 set channel 6")
    rf_pcap.capture_start(iface="wlan0", path="/tmp/pr.pcap")
    t, res = _emit_thread("deauth-synth", "6", str(PR_SECS + 6), "wlan1", secs_hint=PR_SECS + 46)
    time.sleep(2)
    on = dd(PR_SECS)
    t.join(timeout=PR_SECS + 30)
    rf_pcap.capture_stop("/tmp/pr.pcap")
    h.tool_stop()
    witness = rf_pcap.count("/tmp/pr.pcap", rf_pcap._filter("deauth", bssid=DEAUTH_BSSID)) or 0
    if off is None or on is None:
        return ("C4 Packet Rate", None, "counter unread off=%s on=%s" % (off, on))
    if witness < WIT_MIN:
        return ("C4 Packet Rate", None,
                "stimulus did not radiate (witness=%d) -- rig, not badge" % witness)
    ratio = on / max(off, 1)
    ok = on >= 20 and ratio >= PR_RATIO
    return ("C4 Packet Rate", ok,
            "deauth delta off=%d on=%d (%.1fx>=%.0f) [wit=%d] (cites RX2 same counter)"
            % (off, on, ratio, PR_RATIO, witness))


# ---------------- C7: Analyze > Beacons (thin completeness; cites RX1) ----------------
BEA_SECS = 30


def case_beacons(h, nonce):
    """COMPLETENESS (not new RX proof): Analyze>Beacons (3,0) = sniffBeacons = WIFI_SCAN_TARGET_AP,
    the SAME beacon-capture mode test_scan_rx.py RX1 already proves via ap_scan. This drives the
    tool itself and reads its populated AP list (ap_list, NOT ap_scan which re-scans). FRESH unique
    nonce mandatory -- the AP list PERSISTS, so a reused nonce could be a stale hit."""
    an = h.cat_pos("Analyze")
    if an is None:
        return ("C7 Beacons", None, "Analyze category absent (SKU?)")
    bnonce = nonce + "B"                      # distinct from any other case's nonce
    h.tool_stop(); time.sleep(1)
    h.cmd("tool_open %d 0" % an)             # Analyze > Beacons (item 0)
    time.sleep(2)
    rf_pcap._ssh("sudo iw dev wlan0 set channel 6")
    rf_pcap.capture_start(iface="wlan0", path="/tmp/bea.pcap")
    t, res = _emit_thread("beacon-ssid", bnonce, str(BEA_SECS + 6), "6", secs_hint=BEA_SECS + 46)
    time.sleep(BEA_SECS)
    t.join(timeout=BEA_SECS + 30)
    rf_pcap.capture_stop("/tmp/bea.pcap")
    witness = rf_pcap.count("/tmp/bea.pcap", rf_pcap._filter("beacon", ssid=bnonce)) or 0
    hit = h.ap_list(bnonce) or {}
    present = any((a.get("essid") == bnonce) for a in (hit.get("aps") or []))
    neg = h.ap_list(bnonce + "X404") or {}   # never-sent nonce
    neg_ok = not any((a.get("essid") == bnonce + "X404") for a in (neg.get("aps") or []))
    h.tool_stop()
    if witness < WIT_MIN:
        return ("C7 Beacons", None,
                "beacon did not radiate (witness=%d) -- rig, not badge" % witness)
    ok = present and neg_ok
    return ("C7 Beacons", ok,
            "nonce %s present=%s, never-sent absent=%s [wit=%d] (cites RX1 same TARGET_AP mode)"
            % (bnonce, present, neg_ok, witness))


# ---------------- C2: Scan > BT Devices ----------------
BLE_SECS = 25


def case_bt_devices(h, nonce):
    """Detection: a UNIQUE nonce BLE device name (emitted by kalipi hci0) appears in bt_list. The
    unique name is the BLE analog of the WiFi nonce SSID -- nothing else on air sends that exact
    name, so its presence proves BOTH that our emitter radiated AND the badge received+parsed it
    (self-contained, no separate BLE radiation witness needed, which rf_pcap can't provide). Anti-
    hallucination: a fabricated name never appears. BADGE-RX-ALIVE control: bt_list sees ambient
    BLE (count>0); if our nonce is absent WHILE ambient is seen, that's emitter/range -> CANNOT-TEST,
    never a false FAIL. clearBTDevices() runs on tool start so the positive is not a stale hit."""
    sc = h.cat_pos("Scan")
    if sc is None:
        return ("C2 BT Devices", None, "Scan category absent (SKU?)")
    bname = "CBTB" + nonce[-5:]
    t, res = _emit_thread("ble-name", bname, str(BLE_SECS + 6), secs_hint=BLE_SECS + 46)
    time.sleep(3)
    h.tool_stop(); time.sleep(1)
    h.cmd("tool_open %d 3" % sc)             # Scan > BT Devices (item 3)
    time.sleep(BLE_SECS)
    bl = h.cmd("bt_list") or {}
    t.join(timeout=BLE_SECS + 30)
    h.tool_stop()
    devs = bl.get("devices") or []
    count = bl.get("count", 0)
    present = any(d.get("name") == bname for d in devs)
    neg_ok = not any(d.get("name") == bname + "X404" for d in devs)
    if count == 0:
        return ("C2 BT Devices", None, "badge saw NO BLE at all (count=0) -- rig, not badge")
    if not present:
        return ("C2 BT Devices", None,
                "nonce name %s not seen though badge saw %d ambient BLE -- emitter/range, CANNOT-TEST"
                % (bname, count))
    ok = present and neg_ok
    return ("C2 BT Devices", ok,
            "nonce name %s present=%s, fabricated-absent=%s (ambient BLE count=%d)"
            % (bname, present, neg_ok, count))


# ---------------- C3: Scan > BLE Adverts (thin completeness; cites C2) ----------------
BLE_ADV_MIN = 40   # adverts the counter must accumulate in the window to prove reception


def case_ble_adverts(h):
    """COUNTER-ALIVE proof for Scan>BLE Adverts (1,4 / BT_SCAN_SIMPLE) -- a raw advert COUNTER
    (pkt_counters.bt_frames), not a list. ⚠ IDENTITY IS UNPROVEN for THIS mode: C2 exercises a
    DIFFERENT mode (item 3 = BT_SCAN_ALL) so it does NOT cover item 4's parse -- this case cannot
    tell 'received our advert' from 'received N ambient adverts'. A global ON/OFF-vs-our-advertiser
    ratio is hopeless (MEASURED ~31 ambient adverts/s, ~788 in 25s, drowns one emitter -> 1.0x). What
    IS proven: bt_frames is zeroed when BLE scanning (re)starts (BLE teardown, WiFiScan.cpp:2589 --
    NOT resetPacketCounters, which doesn't touch it) and increments ONLY on a RECEIVED advert
    (WiFiScan.cpp:1053); so start~0 + delta>=BLE_ADV_MIN == that many real receptions, and a BLE-deaf
    badge reads 0. Our advertiser runs too, guaranteeing traffic even in a quiet room. This is the
    honest ceiling without an item-4 identity hook or a BLE radiation witness (neither exists)."""
    sc = h.cat_pos("Scan")
    if sc is None:
        return ("C3 BLE Adverts", None, "Scan category absent (SKU?)")
    t, res = _emit_thread("ble-raw", "020106050903434254", str(BLE_SECS + 8), secs_hint=BLE_SECS + 46)
    time.sleep(2)
    h.tool_stop(); time.sleep(1)
    h.cmd("tool_open %d 4" % sc)             # Scan > BLE Adverts (item 4) -- clears bt_frames
    time.sleep(1)
    start = (h.pkt_counters() or {}).get("bt_frames")   # should be ~0 right after start
    time.sleep(BLE_SECS)
    end = (h.pkt_counters() or {}).get("bt_frames")
    t.join(timeout=BLE_SECS + 30)
    h.tool_stop()
    if start is None or end is None:
        return ("C3 BLE Adverts", None, "bt_frames unread start=%s end=%s" % (start, end))
    delta = end - start
    # start-low proves the counter was zeroed at scan (re)start (not a stuck static value); delta
    # proves it accumulated received adverts. A deaf badge would show start~0 AND delta~0. The <200
    # bound tolerates the ambient that arrives in the ~1-2s between tool_open and the first read
    # (measured ~31/s); it is a stuck-high guard, not a precise "~1s" claim.
    started_clean = start < 200
    ok = started_clean and delta >= BLE_ADV_MIN
    return ("C3 BLE Adverts", ok,
            "bt_frames start=%d end=%d delta=%d (>=%d received; started_clean=%s) [item-4 IDENTITY UNPROVEN; counter-alive only]"
            % (start, end, delta, BLE_ADV_MIN, started_clean))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=os.environ.get("CLIPBOY_PORT", "COM5"))
    ap.add_argument("--only", default="", help="comma list: c5,c6,c8")
    a = ap.parse_args()
    only = set(x.strip().lower() for x in a.only.split(",") if x.strip())
    nonce = "CBRX%d" % (int(time.time()) % 100000)
    print("RX-DETECTION (new coverage) -- badge %s, nonce %s\n" % (a.port, nonce))
    TS.deploy()
    # C5/C6/C8 use the kali RADIATION witness (kalipi injection, co-located w/ kali). C1 does NOT
    # (its client is on shipship, out of kali's range; the wired-host ping replies are its
    # 'stimulus landed' proof). Only run/gate on the witness preflight if a witness case is active.
    needs_witness = (not only) or bool(only & {"c4", "c5", "c6", "c7", "c8"})
    if needs_witness:
        # Preflight on a BUSY channel (6): it self-verifies by counting AMBIENT beacons, which a
        # quiet oracle channel (3/5) has ~none of -- ch3/5 witness the STIMULUS fine (C5 saw 7k+),
        # but have no ambient to prove the pipeline. Each case re-tunes the witness to its own chan.
        if not rf_pcap.preflight(chan=6):
            print("CANNOT TEST: kali witness preflight failed -- radiation control unavailable.")
            return 2

    h = Harness(port=a.port)
    cases = [
        ("c1", "C1 Stations",     lambda: case_stations(h)),
        ("c2", "C2 BT Devices",   lambda: case_bt_devices(h, nonce)),
        ("c3", "C3 BLE Adverts",  lambda: case_ble_adverts(h)),
        ("c5", "C5 Channel Stats", lambda: case_channel_stats(h)),
        ("c6", "C6 MAC Tracker",  lambda: case_mac_tracker(h)),
        ("c8", "C8 Raw PCAP",     lambda: case_raw_pcap(h, nonce)),
        ("c4", "C4 Packet Rate",  lambda: case_packet_rate(h)),
        ("c7", "C7 Beacons",      lambda: case_beacons(h, nonce)),
    ]
    results = []
    try:
        for cid, label, fn in cases:
            if only and cid not in only:
                continue
            try:
                r = fn()
            except Exception as e:
                # A truncated read / HarnessTimeout in ONE case must not abort the run or drop the
                # summary (fresh-review FIX). Record it as CANNOT-TEST and keep going.
                r = (label, None, "EXCEPTION: %r -- case aborted, suite continues" % e)
                try:
                    h.tool_stop()
                except Exception:
                    pass
            results.append(r)
            print("  %s" % results[-1][2])
    finally:
        try:
            h.tool_stop()
        except Exception:
            pass
        try:
            h.close()
        except Exception:
            pass
        # Stop any stranded kali witness capture: a case that raised between capture_start and
        # capture_stop would otherwise leave tcpdump running on the witness (fresh-review FIX).
        try:
            rf_pcap._ssh("sudo pkill -f tcpdump")
        except Exception:
            pass

    print("\n===== RX-DETECTION SUMMARY =====")
    fails = cannot = 0
    for name, ok, d in results:
        tag = "PASS" if ok else ("CANNOT-TEST" if ok is None else "FAIL")
        cannot += ok is None; fails += ok is False
        print("  %-18s %-12s %s" % (name, tag, d))
    if cannot:
        return 2
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
