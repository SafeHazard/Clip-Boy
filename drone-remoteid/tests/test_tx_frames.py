#!/usr/bin/env python3
"""Round-trip tests: what standalone-dronesim transmits, the Clip-Boy tool parses.

test_ie_walk.py checks the receiver against frames built from the published
802.11 and ASTM F3411 layouts. That proves the receiver reads the standard.
It does not prove the transmitter writes it.

This mirrors the three frame builders in standalone-dronesim/rid_tx.cpp
(build_beacon, build_nan, ble_advertise) and feeds their output through the
receiver mirrors in rid_mirror.py. A byte the transmitter puts in the wrong
place shows up here as a failed round trip, on a laptop, instead of as a
receiver that stays stubbornly empty with hardware powered up on the bench.

Run:  python drone-remoteid/tests/test_tx_frames.py
"""

import os
import re
import sys

from rid_mirror import (ODID_OUI, ODID_VENDOR_TYPE, NAN_DA, WFA_OUI, ODID_SVC_ID,
                        rid_sniff_beacon, rid_sniff_nan_action)

HERE = os.path.dirname(os.path.abspath(__file__))
RID_TX = os.path.join(HERE, os.pardir, "standalone-dronesim", "rid_tx.cpp")

AP_SSID = b"dronesim"
RATES = bytes([0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24])
MAC = bytes([0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE])
ODID_APP_CODE = 0x0D
ODID_MESSAGE_SIZE = 25

failures = []


def check(name, cond, detail=""):
    if cond:
        print("  ok   %s" % name)
    else:
        print("  FAIL %s %s" % (name, detail))
        failures.append(name)


# ---------------------------------------------------------------------------
# Mirrors of the transmitter. Keep these line-for-line with rid_tx.cpp.
# ---------------------------------------------------------------------------
def build_beacon(mac, channel, odid, counter):
    out = bytearray()
    out += bytes([0x80, 0x00])          # type/subtype: beacon
    out += bytes([0x00, 0x00])          # duration
    out += b"\xff" * 6                  # DA: broadcast
    out += mac                          # SA
    out += mac                          # BSSID
    out += bytes([0x00, 0x00])          # seq/frag
    out += b"\x00" * 8                  # timestamp
    out += bytes([0x64, 0x00])          # beacon interval
    out += bytes([0x01, 0x00])          # capability

    out += bytes([0x00, len(AP_SSID)]) + AP_SSID
    out += bytes([0x01, len(RATES)]) + RATES
    out += bytes([0x03, 0x01, channel])

    out += bytes([221, 5 + len(odid)])
    out += ODID_OUI
    out += bytes([ODID_APP_CODE, counter])
    out += odid
    return bytes(out)


def build_nan(mac, odid, counter):
    out = bytearray()
    out += bytes([0xD0, 0x00])          # type/subtype: action
    out += bytes([0x00, 0x00])          # duration
    out += NAN_DA                       # DA
    out += mac                          # SA
    out += NAN_DA                       # BSSID
    out += bytes([0x00, 0x00])          # seq/frag

    out += bytes([0x04, 0x09])
    out += WFA_OUI
    out += bytes([0x13])

    attr_len = 10 + 1 + len(odid)
    out += bytes([0x03, attr_len & 0xFF, attr_len >> 8])
    out += ODID_SVC_ID
    out += bytes([0x01, 0x00, 0x10, 1 + len(odid)])
    out += bytes([counter])
    out += odid
    return bytes(out)


def build_ble_ad(message, counter):
    out = bytearray()
    out.append(1 + 2 + 2 + ODID_MESSAGE_SIZE)
    out.append(0x16)                    # service data, 16 bit UUID
    out += bytes([0xFA, 0xFF])          # 0xFFFA little endian
    out.append(ODID_APP_CODE)
    out.append(counter)
    out += message
    return bytes(out)


def on_air(frame):
    """The receiver sees the frame plus a hardware FCS, and trims it back off."""
    withfcs = frame + b"\xde\xad\xbe\xef"
    return withfcs, len(withfcs) - 4


def pack(n_messages):
    """An ODID message pack: 3 byte header then n 25 byte messages."""
    body = b"".join(bytes([i]) + bytes(range(1, ODID_MESSAGE_SIZE))
                    for i in range(n_messages))
    return bytes([0xF0, ODID_MESSAGE_SIZE, n_messages]) + body


# ---------------------------------------------------------------------------
print("beacon path, transmitter to receiver")

PACK5 = pack(5)                     # what the sim actually sends
frame, ln = on_air(build_beacon(MAC, 6, PACK5, 0x11))
got = rid_sniff_beacon(frame, ln)
check("receiver finds the ODID IE past SSID, rates and DS param", got is not None)
if got:
    check("payload survives the round trip", got[0] == PACK5,
          "%d bytes back, %d sent" % (len(got[0]), len(PACK5)))
    check("source MAC is the drone", got[1] == MAC, got[1].hex())

# The IE list must be exactly consumed. A wrong length byte anywhere leaves the
# walk mid-IE or past the end, which is the classic way to lose the vendor IE.
def ie_list_is_exact(frame_, len_):
    pos = 36
    while pos + 2 <= len_:
        pos += 2 + frame_[pos + 1]
    return pos == len_

check("IE lengths add up exactly to the end of the frame",
      ie_list_is_exact(frame, ln), "walk ended at a different offset")

# Locate the vendor IE and verify its declared length directly.
pos = 36
vendor_at = None
while pos + 2 <= ln:
    if frame[pos] == 221:
        vendor_at = pos
        break
    pos += 2 + frame[pos + 1]
check("vendor IE is present", vendor_at is not None)
if vendor_at is not None:
    check("vendor IE length byte is 5 + payload",
          frame[vendor_at + 1] == 5 + len(PACK5),
          "len=%d payload=%d" % (frame[vendor_at + 1], len(PACK5)))

# A full nine message pack is the largest the standard allows. The IE length is
# a single byte, so this is the case that would overflow it if it were going to.
PACK9 = pack(9)
check("a full 9 message pack still fits an 8 bit IE length",
      5 + len(PACK9) <= 255, "would be %d" % (5 + len(PACK9)))
frame9, ln9 = on_air(build_beacon(MAC, 6, PACK9, 0x22))
got9 = rid_sniff_beacon(frame9, ln9)
check("9 message pack round trips", got9 is not None and got9[0] == PACK9)

# The smallest legal payload the receiver will accept is one byte, since it
# requires the IE length to be at least 6.
frame1, ln1 = on_air(build_beacon(MAC, 6, b"\x42", 0x33))
got1 = rid_sniff_beacon(frame1, ln1)
check("a single byte payload is still accepted", got1 is not None and got1[0] == b"\x42")

print("")
print("NAN action path, transmitter to receiver")

nframe, nln = on_air(build_nan(MAC, PACK5, 0x44))
ngot = rid_sniff_nan_action(nframe, nln)
check("receiver accepts the NAN action frame", ngot is not None)
if ngot:
    check("payload survives the round trip", ngot[0] == PACK5,
          "%d bytes back, %d sent" % (len(ngot[0]), len(PACK5)))
    check("source MAC is the drone", ngot[1] == MAC, ngot[1].hex())

check("service info length fits a byte with a full pack",
      1 + len(PACK9) <= 255, "would be %d" % (1 + len(PACK9)))
n9, n9ln = on_air(build_nan(MAC, PACK9, 0x55))
check("9 message pack round trips over NAN",
      rid_sniff_nan_action(n9, n9ln) == (PACK9, MAC))

# The attribute length field must describe the rest of the attribute, or a
# stricter parser than ours drops the frame even though we would accept it.
attr_len = nframe[31] | (nframe[32] << 8)
check("NAN attribute length describes the remainder of the attribute",
      attr_len == 10 + 1 + len(PACK5), "attr_len=%d" % attr_len)

print("")
print("BLE advertisement")

msg = bytes(range(ODID_MESSAGE_SIZE))
ad = build_ble_ad(msg, 0x66)
check("advertisement is exactly the 31 byte legacy limit", len(ad) == 31, "%d" % len(ad))
check("length byte covers everything after itself", ad[0] == len(ad) - 1,
      "len byte %d, actual %d" % (ad[0], len(ad) - 1))
check("AD type is service data with a 16 bit UUID", ad[1] == 0x16)
check("UUID is 0xFFFA little endian", ad[2:4] == bytes([0xFA, 0xFF]), ad[2:4].hex())
check("ASTM application code precedes the counter", ad[4] == ODID_APP_CODE)
check("the whole 25 byte message is carried", ad[6:] == msg, "%d bytes" % len(ad[6:]))

print("")
print("constants match rid_tx.cpp")

src = open(RID_TX, encoding="utf-8").read()


def c_bytes(name):
    m = re.search(r"%s\[\d*\]\s*=\s*\{([^}]*)\}" % re.escape(name), src)
    if not m:
        return None
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))


check("ODID_OUI matches the receiver", c_bytes("ODID_OUI") == ODID_OUI, c_bytes("ODID_OUI"))
check("WFA_OUI matches the receiver", c_bytes("WFA_OUI") == WFA_OUI, c_bytes("WFA_OUI"))
check("NAN_CLUSTER matches the receiver NAN_DA",
      c_bytes("NAN_CLUSTER") == NAN_DA, c_bytes("NAN_CLUSTER"))
check("ODID_SVC_ID matches the receiver",
      c_bytes("ODID_SVC_ID") == ODID_SVC_ID, c_bytes("ODID_SVC_ID"))

m = re.search(r"ODID_APP_CODE\s*=\s*0x([0-9A-Fa-f]{2})", src)
check("app code is the ASTM 0x0D",
      m is not None and int(m.group(1), 16) == ODID_VENDOR_TYPE)

m = re.search(r"#define\s+RID_TX_FRAME_MAX\s+(\d+)", src)
frame_max = int(m.group(1)) if m else 0
biggest = len(build_beacon(MAC, 6, PACK9, 0))
check("transmit buffer holds the largest frame we can build (>= %d)" % biggest,
      frame_max >= biggest, "RID_TX_FRAME_MAX=%d" % frame_max)

hdr = open(os.path.join(HERE, os.pardir, "standalone-dronesim", "rid_tx.h"),
           encoding="utf-8").read()
m = re.search(r"#define\s+RID_TX_MSG_COUNT\s+(\d+)", hdr)
msg_count = int(m.group(1)) if m else 0
check("message count is within the 9 the standard allows",
      1 <= msg_count <= 9, "RID_TX_MSG_COUNT=%d" % msg_count)

# The receiver copies into a fixed ring slot. If our pack is larger than that
# slot the tail is silently truncated and the decode fails on real hardware.
WIFISCAN = os.path.join(HERE, os.pardir, os.pardir,
                        "Clip-Boy", "libs", "ClipBoy", "src", "WiFiScan.cpp")
rxsrc = open(WIFISCAN, encoding="utf-8").read()
m = re.search(r"#define\s+RID_WIFI_FRAME_MAX\s+(\d+)", rxsrc)
rx_max = int(m.group(1)) if m else 0
our_pack = 3 + msg_count * ODID_MESSAGE_SIZE
check("receiver ring slot holds our pack without truncating (>= %d)" % our_pack,
      rx_max >= our_pack, "RID_WIFI_FRAME_MAX=%d" % rx_max)

print("")
if failures:
    print("%d FAILED: %s" % (len(failures), ", ".join(failures)))
    sys.exit(1)
print("all passed")
