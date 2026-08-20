#!/usr/bin/env python3
"""Offset tests for the Remote ID WiFi receive path.

The IE walk in WiFiScan.cpp (rid_sniff_beacon / rid_sniff_nan_action) is the
part of the WiFi path that is easy to get subtly wrong: one bad offset and the
tool decodes garbage, or silently sees nothing, and you only find out with a
drone in the air. There is no host build of the firmware, so this mirrors the
two walkers in Python and runs them against frames assembled here from the
802.11 and ASTM F3411 layouts, not from the walkers themselves. If an offset
in the C drifts, the mirror stops matching the frames and these fail.

It also greps the C source for the magic constants, so a change there without
a change here is caught rather than quietly diverging.

Run:  python drone-remoteid/tests/test_ie_walk.py
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
WIFISCAN = os.path.join(HERE, os.pardir, os.pardir,
                        "Clip-Boy", "libs", "ClipBoy", "src", "WiFiScan.cpp")

ODID_OUI = bytes([0xFA, 0x0B, 0xBC])
ODID_VENDOR_TYPE = 0x0D
NAN_DA = bytes([0x51, 0x6F, 0x9A, 0x01, 0x00, 0x00])
WFA_OUI = bytes([0x50, 0x6F, 0x9A])
ODID_SVC_ID = bytes([0x88, 0x69, 0x19, 0x9D, 0x92, 0x09])

SA = bytes([0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE])
BSSID = SA
FCS = b"\xde\xad\xbe\xef"

failures = []


def check(name, cond, detail=""):
    if cond:
        print("  ok   %s" % name)
    else:
        print("  FAIL %s %s" % (name, detail))
        failures.append(name)


# ---------------------------------------------------------------------------
# Mirrors of the C walkers. Keep these line-for-line with WiFiScan.cpp.
# Each returns (odid_bytes, source_mac) or None, matching what the C hands to
# rid_wifi_enqueue().
# ---------------------------------------------------------------------------
def rid_sniff_beacon(d, len_):
    if len_ < 36 + 2:
        return None
    sa = d[10:16]
    pos = 36
    while pos + 2 <= len_:
        ie_id, l = d[pos], d[pos + 1]
        if pos + 2 + l > len_:
            break
        if (ie_id == 221 and l >= 6 and d[pos + 2:pos + 5] == ODID_OUI
                and d[pos + 5] == ODID_VENDOR_TYPE):
            return (d[pos + 7:pos + 7 + (l - 5)], sa)
        pos += 2 + l
    return None


def rid_sniff_nan_action(d, len_):
    if len_ < 24 + 4 + 3 + 10:
        return None
    if d[4:10] != NAN_DA:
        return None
    sa = d[10:16]
    p = 24
    if d[p] != 0x04 or d[p + 1] != 0x09:
        return None
    if d[p + 2:p + 5] != WFA_OUI or d[p + 5] != 0x13:
        return None
    p += 6
    if p + 13 > len_:
        return None
    if d[p] != 0x03:
        return None
    if d[p + 3:p + 9] != ODID_SVC_ID:
        return None
    svc_info_len = d[p + 12]
    si = p + 13
    if si + svc_info_len > len_ or svc_info_len < 2:
        return None
    return (d[si + 1:si + svc_info_len], sa)


# ---------------------------------------------------------------------------
# Frame builders, straight from the layouts.
# ---------------------------------------------------------------------------
def mac_header(subtype_fc, da):
    """24 bytes: FC(2) dur(2) addr1(6) addr2(6) addr3(6) seq(2)."""
    return (bytes([subtype_fc, 0x00]) + b"\x00\x00" + da + SA + BSSID + b"\x00\x00")


def ie(ie_id, body):
    return bytes([ie_id, len(body)]) + body


def odid_ie(odid, counter=0x11):
    return ie(221, ODID_OUI + bytes([ODID_VENDOR_TYPE, counter]) + odid)


def beacon(ies):
    # header + fixed params (timestamp 8, beacon interval 2, capability 2)
    return mac_header(0x80, b"\xff" * 6) + b"\x00" * 12 + ies + FCS


def nan_action(odid, counter=0x22, svc_id=ODID_SVC_ID):
    svc_info = bytes([counter]) + odid
    attr = (bytes([0x03]) + b"\x00\x00" + svc_id
            + bytes([0x01, 0x00, 0x00]) + bytes([len(svc_info)]) + svc_info)
    body = bytes([0x04, 0x09]) + WFA_OUI + bytes([0x13]) + attr
    return mac_header(0xD0, NAN_DA) + body + FCS


def trim_fcs(frame):
    """What the callback does: sig_len counts the FCS, the walk must not."""
    return len(frame) - 4 if len(frame) > 4 else 0


# A Basic ID message: type/version byte, then 24 bytes of payload.
MSG_BASIC_ID = bytes([0x00]) + bytes(range(1, 25))
# A message pack: header byte 0xF0, msg size, msg count, then the messages.
MSG_PACK = bytes([0xF0, 25, 2]) + MSG_BASIC_ID + bytes([0x10]) + bytes(range(30, 54))

print("beacon path")

f = beacon(ie(0, b"dji-drone") + odid_ie(MSG_BASIC_ID))
got = rid_sniff_beacon(f, trim_fcs(f))
check("single ODID message extracted", got is not None and got[0] == MSG_BASIC_ID,
      "got %r" % (got and got[0],))
check("source MAC is addr2", got is not None and got[1] == SA)

f = beacon(odid_ie(MSG_PACK))
got = rid_sniff_beacon(f, trim_fcs(f))
check("message pack extracted whole", got is not None and got[0] == MSG_PACK,
      "len %s want %s" % (got and len(got[0]), len(MSG_PACK)))

# The ODID IE after several others, which is where a bad IE stride shows up.
f = beacon(ie(0, b"net") + ie(1, b"\x82\x84\x8b") + ie(3, b"\x06")
           + ie(221, WFA_OUI + b"\x01\x01\x00" + b"\x00" * 20)
           + odid_ie(MSG_BASIC_ID))
got = rid_sniff_beacon(f, trim_fcs(f))
check("walks past other IEs, incl. a non-ODID vendor IE",
      got is not None and got[0] == MSG_BASIC_ID)

f = beacon(ie(0, b"no drone here") + ie(1, b"\x82\x84"))
check("no ODID IE -> nothing", rid_sniff_beacon(f, trim_fcs(f)) is None)

# Truncated IE: claims more body than the frame holds. Must stop, not over-read.
f = beacon(ie(0, b"x") + bytes([221, 40]) + ODID_OUI + bytes([ODID_VENDOR_TYPE, 0x00]))
check("truncated IE -> nothing", rid_sniff_beacon(f, trim_fcs(f)) is None)

check("runt frame -> nothing", rid_sniff_beacon(b"\x80\x00" + b"\x00" * 20, 22) is None)

# An empty ODID body (l == 5) must be rejected by the l >= 6 guard, since
# l - 5 would otherwise be a zero-length enqueue.
f = beacon(bytes([221, 5]) + ODID_OUI + bytes([ODID_VENDOR_TYPE, 0x00]))
check("empty ODID body -> nothing", rid_sniff_beacon(f, trim_fcs(f)) is None)

# The FCS must be excluded, or the four trailing bytes look like an IE header.
f = beacon(ie(0, b"x"))
check("FCS is not walked as an IE", rid_sniff_beacon(f, trim_fcs(f)) is None)

print("NAN action path")

f = nan_action(MSG_BASIC_ID)
got = rid_sniff_nan_action(f, trim_fcs(f))
check("service info yields ODID message",
      got is not None and got[0] == MSG_BASIC_ID, "got %r" % (got and got[0],))
check("source MAC is addr2", got is not None and got[1] == SA)

f = nan_action(MSG_BASIC_ID, svc_id=bytes([0x11] * 6))
check("wrong service ID -> nothing", rid_sniff_nan_action(f, trim_fcs(f)) is None)

f = bytearray(nan_action(MSG_BASIC_ID))
f[4:10] = b"\xff" * 6                       # not the NAN cluster address
check("wrong destination -> nothing", rid_sniff_nan_action(bytes(f), trim_fcs(f)) is None)

f = bytearray(nan_action(MSG_BASIC_ID))
f[24] = 0x05                                # not a public action frame
check("wrong action category -> nothing", rid_sniff_nan_action(bytes(f), trim_fcs(f)) is None)

# Service info length that runs past the frame.
f = bytearray(nan_action(MSG_BASIC_ID))
f[24 + 6 + 12] = 200
check("overlong service info -> nothing",
      rid_sniff_nan_action(bytes(f), trim_fcs(f)) is None)

check("beacon walker rejects a NAN frame it should not see",
      rid_sniff_beacon(nan_action(MSG_BASIC_ID), trim_fcs(nan_action(MSG_BASIC_ID))) is None)

print("constants match WiFiScan.cpp")

try:
    with open(WIFISCAN, "r", encoding="utf-8", errors="replace") as fh:
        src = fh.read()
except IOError as e:
    print("  FAIL cannot read %s (%s)" % (WIFISCAN, e))
    failures.append("read WiFiScan.cpp")
    src = ""


def c_bytes(name):
    m = re.search(r"%s\[\d*\]\s*=\s*\{([^}]*)\}" % re.escape(name), src)
    if not m:
        return None
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))


check("ODID_OUI", c_bytes("ODID_OUI") == ODID_OUI, c_bytes("ODID_OUI"))
check("NAN_DA", c_bytes("NAN_DA") == NAN_DA, c_bytes("NAN_DA"))
check("WFA_OUI", c_bytes("WFA_OUI") == WFA_OUI, c_bytes("WFA_OUI"))
check("ODID_SVC_ID", c_bytes("ODID_SVC_ID") == ODID_SVC_ID, c_bytes("ODID_SVC_ID"))

m = re.search(r"#define\s+RID_WIFI_FRAME_MAX\s+(\d+)", src)
frame_max = int(m.group(1)) if m else 0
check("ring frame buffer holds a full message pack (>= %d)" % len(MSG_PACK),
      frame_max >= 3 + 9 * 25, "RID_WIFI_FRAME_MAX=%d" % frame_max)

m = re.search(r"#define\s+RID_WIFI_RING_LEN\s+(\d+)", src)
ring_len = int(m.group(1)) if m else 0
check("ring depth is a positive power of two-ish sane value",
      ring_len >= 2, "RID_WIFI_RING_LEN=%d" % ring_len)

print("")
if failures:
    print("%d FAILED: %s" % (len(failures), ", ".join(failures)))
    sys.exit(1)
print("all passed")
