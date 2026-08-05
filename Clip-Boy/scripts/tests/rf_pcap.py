#!/usr/bin/env python3
"""rf_pcap.py -- capture 802.11 on the kali witness and count frames by FIELD.

This is the ONE genuinely new layer in the RF harness plan. Everything else the plan
needs already exists: `tool_suite.kalipi()` is the ssh transport, `kalipi_stim.mon_up`
brings a monitor iface up, `kalipi_stim.ble_raw` emits BLE. Nothing in scripts/tests/
parses a pcap today -- grepped `tshark|rdpcap|pcapng`, zero hits; every existing witness
counts tcpdump's stderr summary line, which cannot answer a field-level question.

WHY FIELD-LEVEL MATTERS: a counter says "mgmt frames went up". It cannot say "the frame
carried the ESSID we seeded", which is the only way to prove the transmission received is
the one we sent rather than a neighbour's lookalike (owner instruction, 2026-07-28).

--------------------------------------------------------------------------------------
THE TWO-ARM ORACLE, and why one arm was not enough
--------------------------------------------------------------------------------------
An earlier design used ONE arm: emit a decoy, assert `ours == 0 and decoy >= N`. A
reviewer caught that `ours == 0` is UNFALSIFIABLE there -- nothing in that arm proves the
`ours` filter is capable of returning non-zero. A typo'd field name, a wrong address
column, a hex-vs-string SSID mismatch all yield 0 forever and the arm goes green.

  arm 1  (emit decoy only):   ours == 0        decoy >= N
  arm 2  (emit ours only):    ours >= N        decoy == 0

Both filters come from ONE template with only the nonce substituted (`_filter()`), so
they cannot diverge. Every filter is then demonstrated reading POSITIVE and NEGATIVE on
the same radio, same build, same session.

`generic` (same subtype, any address) is reported as a DIAGNOSTIC and never asserted: it
is a strict superset of `decoy`, so it cannot go red while the others are green, and for
beacons it is satisfied by ambient traffic with a dead injector.
"""
import os, re, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

KALI_HOST = os.environ.get("CLIPBOY_KALI", "192.168.1.11")
KALI_USER = os.environ.get("CLIPBOY_KALI_USER", "kali")
DEBUG = os.environ.get("RF_DEBUG", "") not in ("", "0")

# Ordered list of decision-point markers this module can emit. The PLAN's forecast is
# diffed against the actual sequence (owner's ritual step 4b). Names are stable.
MARKERS = []


def dbg(marker, detail=""):
    """Record a decision-point marker. Host-side only -- never Serial, never firmware."""
    MARKERS.append(marker)
    if DEBUG:
        print("    [dbg] %-22s %s" % (marker, detail), flush=True)


def _ssh(cmd, timeout=60):
    r = subprocess.run(
        ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=12",
         "%s@%s" % (KALI_USER, KALI_HOST), cmd],
        capture_output=True, text=True, timeout=timeout,
        # utf-8 explicitly: the default on Windows is cp1252, and these scripts carry
        # non-ASCII (warning glyphs). A cp1252 encode error on the stdin pipe does not
        # raise here -- it kills the writer thread and ssh then HANGS to the timeout,
        # which presents as a dead witness rather than an encoding bug.
        encoding="utf-8", errors="replace")
    return r.returncode, (r.stdout or "") + (r.stderr or "")


def preflight(chan=6):
    """Run the provisioner and HONOUR ITS EXIT CODE. 0 = OK, ANY non-zero = distrust.

    Not `== 3`: FATAL branches exit 1, verify failures exit 3, and a future check may
    pick another code. The contract is binary.
    """
    dbg("preflight.start", "ch%d" % chan)
    script = os.path.join(HERE, "kali_witness_setup.sh")
    with open(script, "rb") as f:
        body = f.read().decode("utf-8", "replace")
    # BINARY stdin, and LF-normalised. TWO separate CRLF hazards, and fixing only the
    # first is not enough -- that cost a run:
    #   1. the working copy is CRLF (git autocrlf), so the file bytes carry \r; and
    #   2. `text=True` opens the child's stdin in TEXT mode, which translates every \n
    #      BACK to \r\n on write. Stripping \r upstream cannot survive that.
    # The remote sees `set: -\r: invalid option` and `syntax error near $'do\r'`, which
    # reads as a broken provisioner rather than a newline bug. Same LF/CRLF class as the
    # release-signing gotcha. Send bytes; decode the replies ourselves.
    body = body.replace("\r\n", "\n").encode("utf-8")
    r = subprocess.run(
        ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=12",
         "%s@%s" % (KALI_USER, KALI_HOST), "bash -s -- %d" % chan],
        input=body, capture_output=True, timeout=180)
    out = (r.stdout or b"").decode("utf-8", "replace")
    err = (r.stderr or b"").decode("utf-8", "replace")
    ok = (r.returncode == 0)
    dbg("preflight.ok" if ok else "preflight.FAILED", "rc=%d" % r.returncode)
    if not ok:
        sys.stderr.write(out + err)
    return ok


# ─── capture: must BRACKET the emit, and the bracket is VERIFIED not assumed ──────────

def capture_start(iface="wlan0", path="/tmp/rf.pcap"):
    """Start tcpdump detached. Returns the wall-clock start time on the WITNESS.

    `ssh host 'tcpdump &'` does NOT return -- a bare `&` inherits stdout/stderr, so the
    "background" launch blocks for the whole capture and the emit never overlaps it.
    nohup + </dev/null + redirect is what actually detaches.
    """
    dbg("capture.start", path)
    _ssh("sudo rm -f %s %s.pid" % (path, path))
    rc, out = _ssh(
        "sudo sh -c 'nohup tcpdump -i %s -w %s -U </dev/null >/tmp/rf_cap.log 2>&1 & "
        "echo $! > %s.pid'; sleep 1; date +%%s.%%N" % (iface, path, path))
    t0 = None
    for line in out.strip().splitlines():
        if re.fullmatch(r"\d+\.\d+", line.strip()):
            t0 = float(line.strip())
    if not _capture_running(path):
        dbg("capture.start.FAILED", "tcpdump not running")
        return None
    return t0


def _capture_running(path="/tmp/rf.pcap"):
    rc, out = _ssh("sudo sh -c 'kill -0 $(cat %s.pid) 2>/dev/null && echo RUNNING'" % path)
    return "RUNNING" in out


def capture_stop(path="/tmp/rf.pcap"):
    """SIGTERM, never SIGKILL -- a killed tcpdump truncates the final pcap block."""
    dbg("capture.stop")
    _ssh("sudo sh -c 'kill -TERM $(cat %s.pid) 2>/dev/null'; sleep 1" % path)
    rc, out = _ssh("date +%s.%N")
    t1 = None
    for line in out.strip().splitlines():
        if re.fullmatch(r"\d+\.\d+", line.strip()):
            t1 = float(line.strip())
    return t1


def bracket_ok(path, min_span):
    """Assert the capture was LISTENING THROUGHOUT its window -- clock-independent.

    ⚠ v1 compared the pcap timestamps (kali's clock) against emit_start/emit_end measured
    on the ORCHESTRATOR's clock (this Windows box). Those are two unsynchronised clocks:
    measured skew was 1.9 s, enough to fail the bracket on a perfectly good capture and
    report CANNOT-TEST. Cross-machine timestamp comparison without NTP is the bug.

    The capture ALREADY brackets the emit by CODE ORDERING -- capture_start confirms
    tcpdump is running BEFORE the emit begins, capture_stop is AFTER it ends, so
    capture-window ⊇ emit-window by construction, no clock needed. What remains to prove
    is that the capture did not DIE early: that it heard frames across most of its own
    lifetime. That uses ONLY the pcap's internal (single-clock) timestamps.

    Returns (ok, span_seconds). `min_span` is the lower bound the caller expects the
    capture to have run (typically ~0.6 * the intended window).
    """
    rc, out = _ssh("tshark -r %s -T fields -e frame.time_epoch 2>/dev/null" % path, timeout=120)
    ts = [float(x) for x in out.split() if re.fullmatch(r"\d+\.\d+", x)]
    if not ts:
        dbg("bracket.NO_FRAMES")
        return False, 0.0
    span = max(ts) - min(ts)
    ok = span >= min_span
    dbg("bracket.ok" if ok else "bracket.SHORT",
        "span=%.1fs (need >=%.1fs)" % (span, min_span))
    return ok, span


# ─── counting: ONE filter template, nonce substituted ────────────────────────────────

def _filter(kind, bssid=None, ssid=None):
    """Build a display filter. ONE template per kind so `ours` and `decoy` cannot diverge.

    badfcs: iwlwifi is ASSUMED to pass corrupt frames up (unmeasured -- confirm by
    counting badfcs in a real capture). The filter is free either way.
    retry: a unicast deauth to a fabricated DA is never ACKed, so the injector retries to
    the limit; counting retries inflates any captured-vs-sent ratio.
    """
    base = "radiotap.flags.badfcs == 0"
    if kind == "deauth":
        f = "%s && wlan.fc.type_subtype == 12" % base
        if bssid:
            # BOTH: for a deauth tshark's wlan.bssid is addr3. If the emitter put the
            # nonce in addr2 only, a bssid-only filter reads 0 -- indistinguishable from
            # a dead radio.
            f += " && wlan.ta == %s && wlan.bssid == %s" % (bssid, bssid)
        return f
    if kind == "beacon":
        f = "%s && wlan.fc.type_subtype == 8" % base
        if ssid:
            f += ' && wlan.ssid == "%s"' % ssid
        return f
    raise ValueError("unknown kind %r" % kind)


def count(path, display_filter, timeout=180):
    """Count frames matching a tshark DISPLAY FILTER (field comparison, not substring)."""
    rc, out = _ssh("tshark -r %s -Y '%s' 2>/dev/null | wc -l" % (path, display_filter),
                   timeout=timeout)
    n = None
    for line in out.strip().splitlines():
        if re.fullmatch(r"\d+", line.strip()):
            n = int(line.strip())
    dbg("count", "%d  <- %s" % (n if n is not None else -1, display_filter[:60]))
    return n


def badfcs_census(path):
    """Diagnostic for an ASSUMED claim: does this radio actually surface bad-FCS frames?"""
    good = count(path, "radiotap.flags.badfcs == 0")
    bad = count(path, "radiotap.flags.badfcs == 1")
    dbg("badfcs.census", "good=%s bad=%s" % (good, bad))
    return good, bad
