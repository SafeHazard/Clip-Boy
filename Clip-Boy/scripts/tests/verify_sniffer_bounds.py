#!/usr/bin/env python3
"""verify_sniffer_bounds.py — bounds proof for the Marauder sniffer parsers (B1/B2).

No host C++ compiler is available and Windows cannot RF-inject malformed 802.11
management frames, so this models the EXACT OLD vs NEW pointer-walk logic of the
fixed parsers and fuzzes them. Every payload[i] access is instrumented: an access
at index >= the declared frame length (rx_ctrl.sig_len) is an out-of-bounds read.

It proves two things over a fuzz battery of crafted/malicious frames:
  1. the OLD logic reads PAST the frame (the bug the audit found, B1 + B2), and
  2. the NEW (shipped) logic NEVER does, while still parsing valid frames correctly.

This is a spec/regression for the C++ in libs/ClipBoy/src/WiFiScan.cpp
(extractManufacturer + the three SSID-length reads). Mirror any change there here.
"""
import os
import random
import re
import sys


class Frame:
    """A captured frame: `data` bytes, `decl` = declared length (sig_len)."""
    def __init__(self, data, decl):
        self.data = bytearray(data)
        self.decl = decl          # the length the parser is told (rx_ctrl.sig_len)
        self.max_oob = -1         # highest index read at-or-past decl (-1 = none)

    def rd(self, i):
        if i >= self.decl or i < 0:
            self.max_oob = max(self.max_oob, i)
            # simulate reading adjacent memory (the over-read); value is arbitrary
            return self.data[i] if 0 <= i < len(self.data) else 0
        return self.data[i]


# ── B1: extractManufacturer — WPS manufacturer IE parser ──────────────────────
def extract_manuf_OLD(f):
    pos = 36
    while pos < 512:                         # NO frame-length bound (the bug)
        tagNumber = f.rd(pos); tagLength = f.rd(pos + 1)
        if tagNumber == 0xdd and tagLength >= 4:
            oui = (f.rd(pos + 2), f.rd(pos + 3), f.rd(pos + 4))
            if oui == (0x00, 0x50, 0xF2):
                wpsPos = pos + 6; end = pos + 2 + tagLength
                while wpsPos + 4 <= end:
                    t = (f.rd(wpsPos) << 8) | f.rd(wpsPos + 1)
                    ln = (f.rd(wpsPos + 2) << 8) | f.rd(wpsPos + 3)
                    if t == 0x1021:
                        cl = min(ln, 64)
                        return bytes(f.rd(wpsPos + 4 + k) for k in range(cl))
                    wpsPos += 4 + ln
        pos += 2 + tagLength
    return b""


def extract_manuf_NEW(f):
    L = f.decl; pos = 36
    while pos + 2 <= L:                       # bounded against frame length
        tagNumber = f.rd(pos); tagLength = f.rd(pos + 1)
        if pos + 2 + tagLength > L:           # IE body must fit in frame
            break
        if tagNumber == 0xdd and tagLength >= 4:
            oui = (f.rd(pos + 2), f.rd(pos + 3), f.rd(pos + 4))
            if oui == (0x00, 0x50, 0xF2):
                wpsPos = pos + 6; end = pos + 2 + tagLength   # end <= L
                while wpsPos + 4 <= end:
                    t = (f.rd(wpsPos) << 8) | f.rd(wpsPos + 1)
                    ln = (f.rd(wpsPos + 2) << 8) | f.rd(wpsPos + 3)
                    if wpsPos + 4 + ln > end:  # sub-TLV body must fit in the IE
                        break
                    if t == 0x1021:
                        cl = min(ln, 64)
                        return bytes(f.rd(wpsPos + 4 + k) for k in range(cl))
                    wpsPos += 4 + ln
        pos += 2 + tagLength
    return b""


# ── B2: beacon SSID copy (payload[37] = SSID length) ──────────────────────────
def beacon_ssid_OLD(f):
    out = bytearray()
    if f.rd(37) > 0:                          # length byte read with no bound
        for i in range(f.rd(37)):
            out.append(f.rd(i + 38))
    return bytes(out)


def beacon_ssid_NEW(f):
    L = f.decl
    sl = f.rd(37) if L > 37 else 0
    if 38 + sl > L:
        sl = (L - 38) if L > 38 else 0
    return bytes(f.rd(i + 38) for i in range(sl))


# ── B4: PROBE-REQUEST SSID copy (payload[25] = SSID length) -- the 4fda7354 class ──
# Four textual copies of this loop existed, ALL unguarded, all live, all reachable from PASSIVE
# tools in the DEFAULT Sn34k build: WiFiScan.cpp beaconSnifferCallback (Detect > Flock Batteries),
# deauthSnifferCallback (Analyze > Deauth) and beaconListSnifferCallback x2. A guard already
# existed on a FIFTH copy (the WIFI_SCAN_PROBE branch) with a comment explaining exactly this
# hazard -- it had simply never been applied to the others. Modelled here because the shipped
# clamps were closed by ENUMERATION and compile only; nothing exercised them at runtime.
#
# ⚠ HONEST LIMIT, same as the pairs above: this is a host-side MODEL of the C, not the compiled
# firmware, so the model and the C can drift. What makes it worth having anyway is that it is a
# DIFFERENTIAL over one shared corpus -- OLD and NEW disagree -- so it cannot be satisfied by a
# tautology the way a single-sided reimplementation could. Keep it in step with the C by hand.
def probe_ssid_OLD(f):
    out = bytearray()
    for i in range(f.rd(25)):                 # length byte itself read with NO bound
        out.append(f.rd(26 + i))
    return bytes(out)


def probe_ssid_NEW(f):
    L = f.decl
    sl = f.rd(25) if L > 25 else 0
    if 26 + sl > L:
        sl = (L - 26) if L > 26 else 0
    return bytes(f.rd(26 + i) for i in range(sl))


def valid_probe_req(ssid=b"MyNet"):
    """A well-formed probe request: SSID IE at 24, length byte at 25, body from 26."""
    b = bytearray(24)
    b[0] = 0x40                               # mgmt / probe request
    b += bytes([0x00, len(ssid)]) + ssid      # tag 0 (SSID), len, body
    # payload[25] must BE the ssid length for the parser's fixed offsets to line up
    b[25] = len(ssid)
    return Frame(b, len(b))


# ── frame crafters ────────────────────────────────────────────────────────────

# ── B3: getSecurityType -- uint16_t underflow on a short mgmt frame (audit FB4) ────
# WiFiScan.cpp:6635  uint8_t getSecurityType(const uint8_t* beacon, uint16_t len)
#                    uint16_t ies_len = len - 36;
# With len < 36 that subtraction WRAPS: len=24 -> 65524. Every subsequent bound
# (`while (i + 2 <= ies_len)` and `if (i + 2 + tag_len > ies_len) break;`) is then
# measured against 65524, so no guard can fire and the IE walk runs up to ~64 KB past
# the frame -- inside the WiFi RX callback, on the DEFAULT SKU, for any beacon-subtype
# frame with sig_len <= 39. It is the only narrow-typed `len` parameter in the file,
# which is why the earlier bounds sweep (B1/B2, eleven lines above the call) missed it.
# The function also reads frame[34]/frame[35] for the WEP capability bits, which is
# itself past the end of a 24-byte frame.
SEC_UNKNOWN, SEC_OPEN, SEC_WEP, SEC_WPA2 = 0xFF, 0, 1, 3


def _sec_walk(f, ies_len):
    """The shared IE walk. `ies` is at absolute offset 36, so ies[i] == frame[36+i]."""
    hasRSN = False
    i = 0
    while i + 2 <= ies_len:
        tag_id = f.rd(36 + i)
        tag_len = f.rd(36 + i + 1)
        if i + 2 + tag_len > ies_len:
            break
        if tag_id == 48:
            hasRSN = True
            if tag_len >= 20:
                for k in range(14, 20):          # tag_data[14..19]
                    f.rd(36 + i + 2 + k)
        i += 2 + tag_len
    f.rd(34); f.rd(35)                            # capability bits (WEP determination)
    return SEC_WPA2 if hasRSN else SEC_OPEN


def sec_type_OLD(f):
    ies_len = (f.decl - 36) & 0xFFFF              # uint16_t wrap -- the bug
    return _sec_walk(f, ies_len)


def sec_type_NEW(f):
    # Matches the siblings at WiFiScan.cpp:6967/:6993 (`if (len < 38) return ...`).
    # 38 is the true minimum: 36 header + a 2-byte tag header, and it also makes the
    # frame[34]/frame[35] capability read in-bounds.
    if f.decl < 38:
        return SEC_UNKNOWN
    return _sec_walk(f, f.decl - 36)


def valid_wps_probe_resp(manuf=b"AcmeCorp"):
    body = bytearray(36)
    body[0] = 0x50                                            # probe-resp marker
    # SSID IE (tag 0)
    body += bytes([0x00, 4]) + b"home"
    # WPS vendor IE (221): OUI 00:50:F2 + OUI-type 0x04 (the parser skips
    # tag+len+OUI+type = 6 bytes), then one Manufacturer (0x1021) sub-TLV
    sub = bytes([0x10, 0x21, (len(manuf) >> 8) & 0xff, len(manuf) & 0xff]) + manuf
    ie = bytes([0x00, 0x50, 0xF2, 0x04]) + sub
    body += bytes([0xdd, len(ie)]) + ie
    return Frame(body, len(body))


def valid_beacon(ssid=b"MyNet"):
    body = bytearray(38)
    body[0] = 0x80
    body[37] = len(ssid)
    body += ssid
    return Frame(body, 38 + len(ssid))


def malicious_frames():
    out = []
    # 1. WPS IE claims a huge tagLength on a short frame
    b = bytearray(36); b[0] = 0x50
    b += bytes([0xdd, 0xff, 0x00, 0x50, 0xF2, 0x10, 0x21, 0xff, 0xff])  # man TLV len 0xffff
    out.append(("wps-oversize-tlv", Frame(b, len(b))))
    # 2. tag chain that walks pos past the frame (each tag claims max length)
    b = bytearray(36); b[0] = 0x50
    b += bytes([0x07, 0xff]) + bytes(4)
    out.append(("tag-walk-past-end", Frame(b, len(b))))
    # 3. WPS manufacturer TLV body runs past the IE/frame
    b = bytearray(36); b[0] = 0x50
    ie = bytes([0x00, 0x50, 0xF2, 0x10, 0x21, 0x00, 0x40])  # claims 64 manuf bytes, none present
    b += bytes([0xdd, len(ie)]) + ie
    out.append(("wps-tlv-past-ie", Frame(b, len(b))))
    # 4. beacon SSID length 0xff on a 40-byte frame
    b = bytearray(40); b[0] = 0x80; b[37] = 0xff
    out.append(("beacon-ssid-0xff", Frame(b, 40)))
    # 4b. PROBE REQUEST whose SSID length byte lies (the 4fda7354 class). A 40-byte frame
    #     claiming a 200-byte SSID over-reads 186 bytes past the capture; the 26-byte case
    #     has no room for ANY body, and the 20-byte runt cannot even hold payload[25].
    for total, claim in ((40, 200), (40, 0xff), (26, 0xff), (30, 100), (20, 0xff)):
        b = bytearray(total)
        if total: b[0] = 0x40                 # probe request -- what reaches these callbacks
        if total > 25: b[25] = claim
        out.append((f"probe-ssid-claim{claim}-len{total}", Frame(b, total)))
    # 5. truncated frame shorter than the fixed header
    out.append(("runt-frame", Frame(bytearray([0x50] + [0]*20), 21)))
    # 6. bodyless mgmt frames: the getSecurityType underflow (len < 36 wraps ies_len)
    for ln in (0, 10, 21, 24, 30, 35, 36, 37):
        b = bytearray(ln)
        if ln:
            b[0] = 0x80          # beacon subtype -- what reaches getSecurityType
        out.append((f"bodyless-mgmt-len{ln}", Frame(b, ln)))
    # 7-106. random fuzz
    rng = random.Random(1234)
    for n in range(100):
        ln = rng.randint(24, 300)
        b = bytearray(rng.randint(0, 255) for _ in range(ln))
        b[0] = rng.choice([0x50, 0x80, 0x40])
        out.append((f"rand-{n}", Frame(b, ln)))
    return out



def check_cpp_has_guard():
    """Assert the REAL C++ carries the guard this file models.

    Without this, the script is only a spec: the NEW model passes while the shipped
    getSecurityType still underflows, so a green run would imply a fix that does not
    exist. Modelling a fix is not the same as having one.
    """
    src = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "..", "..", "libs", "ClipBoy", "src", "WiFiScan.cpp")
    src = os.path.abspath(src)
    if not os.path.isfile(src):
        return [f"cannot find {src}"]
    text = open(src, encoding="utf-8", errors="replace").read()
    # Window must be generous: a well-commented guard pushes the code itself far down the
    # body. A 600-char window reported "still unguarded" for a guard that was present but
    # sat behind ~900 chars of explanation -- the checker was wrong, not the code.
    m = re.search(r"uint8_t\s+WiFiScan::getSecurityType\s*\([^)]*\)\s*\{(.{0,4000})",
                  text, re.S)
    if not m:
        return ["getSecurityType not found in WiFiScan.cpp"]
    body = m.group(1)
    # Either an early length guard, or a signed/wider ies_len that cannot wrap.
    guarded = re.search(r"if\s*\(\s*len\s*<\s*(3[6-9]|4\d)\s*\)", body) is not None
    widened = re.search(r"(int|int32_t|long)\s+ies_len", body) is not None
    if guarded or widened:
        return []
    return ["WiFiScan.cpp getSecurityType still computes ies_len from an unguarded "
            "uint16_t len -- `uint16_t ies_len = len - 36;` underflows to 65524 for "
            "len<36 (audit FB4). Add `if (len < 38) return WIFI_SECURITY_UNKNOWN;` "
            "or widen ies_len to a signed type."]


def main():
    fails = []
    fails += check_cpp_has_guard()

    # Valid frames still parse correctly under the NEW logic.
    f = valid_wps_probe_resp(b"AcmeCorp")
    man = extract_manuf_NEW(f)
    if man != b"AcmeCorp" or f.max_oob >= 0:
        fails.append(f"valid WPS: man={man!r} oob={f.max_oob}")
    f = valid_beacon(b"MyNet")
    ss = beacon_ssid_NEW(f)
    if ss != b"MyNet" or f.max_oob >= 0:
        fails.append(f"valid beacon: ssid={ss!r} oob={f.max_oob}")

    f = valid_probe_req(b"MyNet")
    ps = probe_ssid_NEW(f)
    if ps != b"MyNet" or f.max_oob >= 0:
        fails.append(f"valid probe req: ssid={ps!r} oob={f.max_oob} (NEW clamp broke a GOOD frame)")

    f = valid_beacon(b"MyNet")
    if sec_type_NEW(f) == SEC_UNKNOWN or f.max_oob >= 0:
        fails.append(f"valid beacon rejected/over-read by NEW getSecurityType: oob={f.max_oob}")

    # Fuzz: NEW must never OOB; demonstrate OLD does on the crafted cases.
    old_oob_demo = 0
    new_oob = 0
    old_sec_oob = []
    old_probe_oob = []
    for name, fr in malicious_frames():
        # NEW (fresh frame each parser so max_oob is per-run)
        fn = Frame(bytes(fr.data), fr.decl); extract_manuf_NEW(fn)
        fb = Frame(bytes(fr.data), fr.decl); beacon_ssid_NEW(fb)
        fp = Frame(bytes(fr.data), fr.decl); probe_ssid_NEW(fp)
        if fp.max_oob >= 0:
            fails.append(f"NEW probe_ssid OOB on {name}: idx {fp.max_oob} >= len {fp.decl}")
            new_oob += 1
        if fn.max_oob >= 0:
            fails.append(f"NEW extractManufacturer OOB on {name}: idx {fn.max_oob} >= len {fn.decl}")
            new_oob += 1
        if fb.max_oob >= 0:
            fails.append(f"NEW beacon_ssid OOB on {name}: idx {fb.max_oob} >= len {fb.decl}")
            new_oob += 1
        # OLD (for the demonstration that the bug was real)
        fs = Frame(bytes(fr.data), fr.decl); sec_type_NEW(fs)
        if fs.max_oob >= 0:
            fails.append(f"NEW getSecurityType OOB on {name}: idx {fs.max_oob} >= len {fs.decl}")
            new_oob += 1
        # OLD (for the demonstration that the bug was real)
        fo = Frame(bytes(fr.data), fr.decl); extract_manuf_OLD(fo)
        fbo = Frame(bytes(fr.data), fr.decl); beacon_ssid_OLD(fbo)
        fpo = Frame(bytes(fr.data), fr.decl); probe_ssid_OLD(fpo)
        if fpo.max_oob >= 0:
            old_probe_oob.append((name, fpo.max_oob, fpo.decl))
        fso = Frame(bytes(fr.data), fr.decl); sec_type_OLD(fso)
        if fso.max_oob >= 0:
            old_sec_oob.append((name, fso.max_oob, fso.decl))
        if fo.max_oob >= 0 or fbo.max_oob >= 0 or fso.max_oob >= 0 or fpo.max_oob >= 0:
            old_oob_demo += 1

    print(f"Fuzz battery: {len(malicious_frames())} frames")
    print(f"  OLD logic over-read past frame on {old_oob_demo} frame(s)  (the B1/B2 bug)")
    print(f"  NEW logic over-read past frame on {new_oob} frame(s)")
    if old_sec_oob:
        worst = max(old_sec_oob, key=lambda t: t[1])
        print(f"  OLD getSecurityType over-read on {len(old_sec_oob)} frame(s); "
              f"worst: {worst[0]} read idx {worst[1]} on a {worst[2]}-byte frame "
              f"(uint16 underflow -> ies_len 65524)")
    else:
        fails.append("OLD getSecurityType never over-read -- FB4 not exercised")
    if old_probe_oob:
        w = max(old_probe_oob, key=lambda t: t[1])
        print(f"  OLD probe-SSID over-read on {len(old_probe_oob)} frame(s); "
              f"worst: {w[0]} read idx {w[1]} on a {w[2]}-byte frame "
              f"({w[1] - w[2] + 1} bytes past the capture)")
    else:
        fails.append("OLD probe_ssid never over-read -- the 4fda7354 class is NOT exercised, so a "
                     "green here would prove nothing about those 4 clamps")
    if old_oob_demo == 0:
        fails.append("OLD logic never over-read — the fuzz battery does not exercise the bug")

    print("=" * 56)
    if fails:
        print(f"FAIL: {len(fails)} issue(s):")
        for x in fails:
            print("  - " + x)
        return 1
    print("PASS: fixed parsers never read past the frame length; OLD did;")
    print("      valid frames still parse correctly. Bounds fix verified.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
