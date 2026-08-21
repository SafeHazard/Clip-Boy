"""Python mirrors of the Remote ID WiFi receive path in WiFiScan.cpp.

These are line-for-line copies of rid_sniff_beacon() and rid_sniff_nan_action().
They live in their own module because two test files depend on them:

  test_ie_walk.py   feeds them frames built from the 802.11 and ASTM F3411
                    layouts, checking the receiver reads the offsets correctly.
  test_tx_frames.py feeds them frames built the way the transmitter in
                    standalone-dronesim builds them, checking the two halves
                    of the protocol actually agree.

Keeping one copy means a drift in the C is caught in both places at once,
rather than being fixed in one mirror and left stale in the other.
"""

ODID_OUI = bytes([0xFA, 0x0B, 0xBC])
ODID_VENDOR_TYPE = 0x0D
NAN_DA = bytes([0x51, 0x6F, 0x9A, 0x01, 0x00, 0x00])
WFA_OUI = bytes([0x50, 0x6F, 0x9A])
ODID_SVC_ID = bytes([0x88, 0x69, 0x19, 0x9D, 0x92, 0x09])


def rid_sniff_beacon(d, len_):
    """Returns (odid_bytes, source_mac) or None, as the C hands to the ring."""
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
    """Returns (odid_bytes, source_mac) or None, as the C hands to the ring."""
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
