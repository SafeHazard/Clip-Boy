#!/usr/bin/env python3
"""
kalipi_stim.py -- RF stimulus/sensor primitives for the Clip-Boy tool test suite.

Runs ON kalipi (Raspberry Pi / Kali). The host runner (tool_suite.py) scp's this to
/tmp and invokes `python3 /tmp/kalipi_stim.py <primitive> [args...]` over ssh (eth0,
192.168.1.146 -- the MANAGEMENT path). wlan0 is therefore free to reconfigure into
monitor/AP mode without dropping ssh; hci0 (Bluetooth) is an independent radio.

Every primitive prints ONE JSON line to stdout (result) so the runner can parse it.

Topology assumptions (see docs/test-automation-kalipi.md):
  eth0 = ssh management (never touched here)   wlan0 = WiFi stimulus/sensor   hci0 = BLE

Primitives:
  caps                                 self-check: interfaces + tool availability
  wifi-join   <ssid> <pw> [iface]      associate (assoc/auth/EAPOL/data stimulus)
  wifi-leave  [iface]                  disconnect
  wifi-cycle  <ssid> <pw> <n> [iface]  join/leave n times (EAPOL handshake generator)
  wifi-scan   [iface]                  active scan -> {ssids:[...]}  (also EMITS probes)
  ble-name    <name> <secs>            advertise a device name (skimmer HC-05, generic)
  ble-raw     <hex_adv_data> <secs> [hex_scanrsp]   advertise raw AD payload (airtag/flipper/flock)
  flock-real  <secs>                   replay a REAL published Penguin capture (can go RED)
  flock-cid-end <secs>                 mfg AD ending at the company ID -- witnesses a SILENT MISS
  flock-cid-end-control <secs>         same +1 filler byte: the paired positive control
  flock-reversed-cid <secs>            wrong company ID -- proves a genuine flock=0 is reachable
  ble-scan    <secs>                   discover BLE devices/adverts -> {devices:[...]}
  mon-up      [iface] [channel]        wlan0 -> monitor mode on channel
  mon-down    [iface] [ssid] [pw]      restore managed (+ optional reconnect)
  sniff       <secs> <filter> [iface]  tcpdump on monitor iface -> {count:N}
  deauth      <bssid> [client] [count] [iface]   inject deauth (drives badge geiger)

Needs passwordless sudo for iw/ip/nmcli/tcpdump/aireplay-ng/btmgmt (kalipi `data` user has it).
Stdlib only.
"""
import sys, json, subprocess, time, re, shutil

WLAN = "wlan0"   # onboard brcmfmac: station stimulus (join/scan) only -- no monitor
MON = "wlan1"    # A6210 / mt76x2u: dedicated monitor + injection adapter


def sh(cmd, timeout=30, sudo=False):
    """Run a shell command; return (rc, stdout+stderr)."""
    if sudo and not cmd[0].startswith("sudo"):
        cmd = ["sudo"] + cmd
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return r.returncode, (r.stdout + r.stderr)
    except subprocess.TimeoutExpired:
        return 124, "(timeout)"
    except FileNotFoundError:
        return 127, f"(missing: {cmd[0]})"


def out(**kw):
    print(json.dumps(kw))
    return 0 if kw.get("ok", True) else 1


# ─── WiFi station stimulus ──────────────────────────────────────────────────

def _connect(ssid, pw, iface):
    # A half-written NM profile from a prior partial connect fails reactivation with
    # "802-11-wireless-security.key-mgmt: property is missing". Delete it first so the
    # connect always builds a fresh, complete WPA-PSK profile.
    sh(["nmcli", "con", "delete", ssid], sudo=True, timeout=15)
    return sh(["nmcli", "--wait", "14", "dev", "wifi", "connect", ssid,
               "password", pw, "ifname", iface], sudo=True, timeout=30)


def wifi_join(ssid, pw, iface=WLAN):
    sh(["nmcli", "dev", "wifi", "rescan", "ifname", iface], sudo=True, timeout=20)
    time.sleep(2)
    # --wait bounds nmcli; the 4-way handshake completes at association, before DHCP.
    rc, o = _connect(ssid, pw, iface)
    return out(ok=(rc == 0), ssid=ssid, iface=iface, msg=o.strip().splitlines()[-1] if o.strip() else "")


def wifi_leave(iface=WLAN):
    rc, o = sh(["nmcli", "dev", "disconnect", iface], sudo=True, timeout=20)
    return out(ok=True, iface=iface, msg=o.strip()[-80:])


def wifi_cycle(ssid, pw, n, iface=WLAN):
    joins = 0
    for _ in range(int(n)):
        rc, _o = _connect(ssid, pw, iface)
        if rc == 0:
            joins += 1
        time.sleep(2)
        sh(["nmcli", "dev", "disconnect", iface], sudo=True, timeout=20)
        time.sleep(1.5)
    return out(ok=(joins > 0), ssid=ssid, cycles=int(n), joins=joins)


def wifi_state(iface=WLAN):
    # Connection state, for the monitor-free Deauth test (watch kalipi get kicked off).
    rc, o = sh(["nmcli", "-t", "-f", "GENERAL.STATE", "dev", "show", iface])
    state = o.strip().split(":")[-1] if ":" in o else o.strip()
    return out(ok=True, iface=iface, connected=("100" in state), state=state)


def wifi_scan(iface=WLAN):
    # Active scan emits probe requests (stimulus for Analyze Probes) AND returns the
    # visible SSIDs (verify for the badge's Beacon Spam output). `iw scan` returns
    # "Device or resource busy" if wlan0 is mid-association (just after a join row),
    # so retry a few times before giving up.
    rc, o = 1, ""
    for _ in range(4):
        rc, o = sh(["iw", "dev", iface, "scan"], sudo=True, timeout=25)
        if rc == 0:
            break
        time.sleep(2)
    ssids, cur = [], None
    for line in o.splitlines():
        line = line.strip()
        if line.startswith("BSS "):
            cur = line.split()[1].split("(")[0]
        elif line.startswith("SSID:"):
            s = line[5:].strip()
            if s:
                ssids.append(s)
    return out(ok=(rc == 0), iface=iface, count=len(ssids), ssids=ssids[:200])


# ─── BLE stimulus / sensor (hci0, independent of wlan0) ─────────────────────

def _btmgmt(args, timeout=15):
    return sh(["btmgmt"] + args, sudo=True, timeout=timeout)


def ble_name(name, secs):
    # Advertise a friendly name (e.g. 'HC-05' for the skimmer heuristic). btmgmt
    # brings the controller up, sets name, enables connectable advertising.
    _btmgmt(["power", "on"])
    _btmgmt(["le", "on"])
    _btmgmt(["connectable", "on"])
    _btmgmt(["name", name])
    _btmgmt(["advertising", "on"])
    time.sleep(float(secs))
    _btmgmt(["advertising", "off"])
    return out(ok=True, name=name, secs=float(secs))


def _ble_raw_emit(hex_adv, secs, hex_scanrsp=None):
    """Advertise a raw AD payload for `secs`, then tear the instance down.
    Returns (rc, msg, clr_ok) and prints NOTHING -- the module contract is ONE JSON line per
    primitive, so emission and reporting are separated to keep wrappers from double-printing.

    clr_ok answers "is the advertising slot definitely clean?", because _btmgmt swallows failures
    and a stale instance would attribute one fixture's detection to another.
    ⚠ MEASURED btmgmt semantics on kalipi2b 2026-07-27 -- do NOT "tighten" this back to rc == 0:
        rc 0   -> "Instance removed: 0"                     a leftover WAS present and is now gone
        rc 1   -> "failed with status 0x0d (Invalid Parameters)"   NOTHING to remove: already clean
        rc 124 -> sh() timeout                              UNKNOWN: neither confirmed
    So rc 1 is the COMMON GOOD case, and only a timeout (or a missing binary, 127) is disqualifying.
    An earlier version treated any nonzero rc as contamination and produced a FALSE CANNOT-TEST on
    all four new arms -- a clean run reported as a rig failure."""
    hex_adv = hex_adv.replace(":", "").replace(" ", "")
    _btmgmt(["power", "on"])
    _btmgmt(["le", "on"])
    clr_rc, _clr = _btmgmt(["clr-adv"])
    args = ["add-adv", "-d", hex_adv]
    if hex_scanrsp:
        args += ["-s", hex_scanrsp.replace(":", "").replace(" ", "")]
    args.append("1")
    rc, o = _btmgmt(args)
    time.sleep(float(secs))
    _btmgmt(["rm-adv", "1"])
    return rc, o.strip()[-80:], clr_rc in (0, 1)


def ble_raw(hex_adv, secs, hex_scanrsp=None):
    # Advertise a raw AD payload via btmgmt add-adv (instance 1). hex_adv = the full
    # AD structure(s) as hex (e.g. Apple FindMy / Flipper / Flock manufacturer data).
    #
    # hex_scanrsp (optional) adds a SCAN RESPONSE. This is not cosmetic: the badge scans ACTIVE and
    # NimBLE only delivers onResult for a SCANNABLE legacy adv type once a SCAN_RSP report arrives
    # (NimBLEScan.cpp:143/146). Which adv type BlueZ picks for a -d-only instance is not something
    # we control, so an advert with no scan response has a silent-failure mode that is
    # indistinguishable from "the badge did not detect it". Supplying one removes that ambiguity.
    # ⚠ NimBLE APPENDS scan-response bytes to getPayload() (NimBLEAdvertisedDevice.cpp:79-82), so a
    # consumer must treat the badge's Payload: dump as a PREFIX match, never an equality check.
    rc, msg, clr_ok = _ble_raw_emit(hex_adv, secs, hex_scanrsp)
    return out(ok=(rc == 0), bytes=len(hex_adv.replace(":", "").replace(" ", "")) // 2,
               secs=float(secs), msg=msg, clr_ok=clr_ok)


# ── BLE FAST-advertising (raw HCI ~20ms) for the BLE-advert RATE test (T3) ──────────────────
# btmgmt add-adv CANNOT set the advertising interval (default ~7-10/s). Raw HCI LE Set Advertising
# Parameters at min/max = 0x0020 (20ms) with a CONNECTABLE ADV_IND (type 0x00 -- a NON-connectable
# 0x03 is REJECTED at 20ms with HCI status 0x12 on kalipi's UART controller) reaches ~34 adverts/s
# witnessed on the kali CSR (2.1x the ~16/s ambient), measured 2026-07-30. Autonomous once enabled:
# the controller radiates every ~20ms with NO per-frame count, so the RECEIVER (badge bt_frames /
# kali witness) is what measures the rate. Payload = flags + mfg (company 0xFFFF + "T3RATE") so a
# witness can grep our source address (kalipi hci0) or the mfg bytes.
_FASTADV_PARAMS = ["20", "00", "20", "00", "00", "00", "00", "00", "00", "00", "00", "00", "00", "07", "00"]
_FASTADV_DATA = ["0d", "02", "01", "06", "09", "ff", "ff", "ff", "54", "33", "52", "41", "54", "45"] + ["00"] * 18


def _hci(*words):
    return sh(["hcitool", "-i", "hci0", "cmd"] + list(words), sudo=True)


def _hci_ok(rc, o):
    # hcitool exits 0 even on an HCI error; the real status is the LAST byte of the Command
    # Complete event (e.g. "01 06 20 00" -> 00 OK, "...12" -> Invalid Params). Parse it.
    return rc == 0 and (o.strip().split() or [""])[-1].lower() == "00"


def ble_fastadv_on():
    """Start ~20ms BLE advertising (T3 FLOOD window). Idempotent; pair with ble-fastadv-off.
    ok is true only if all three HCI commands returned status 00 -- but the AUTHORITATIVE proof
    the flood radiated is the receiver (kali witness / badge bt_frames), not this ok."""
    sh(["hciconfig", "hci0", "up"], sudo=True)
    r1, o1 = _hci("0x08", "0x0006", *_FASTADV_PARAMS)
    r2, o2 = _hci("0x08", "0x0008", *_FASTADV_DATA)
    r3, o3 = _hci("0x08", "0x000a", "01")
    return out(ok=(_hci_ok(r1, o1) and _hci_ok(r2, o2) and _hci_ok(r3, o3)),
               kind="ble-fastadv-on", addr="2C:CF:67:CF:29:28",
               statuses=[o1.strip().split()[-1:], o2.strip().split()[-1:], o3.strip().split()[-1:]])


def ble_fastadv_off():
    """Stop the fast advertising (T3 THROTTLE window). ALSO clr-adv to remove any leftover btmgmt
    add-adv instance from an earlier suite (the flock/detect emitters share hci0) -- otherwise a stale
    advertiser keeps pumping the DUT's bt_frames through the baseline and every window. Safe-direction
    (never a false PASS) but a real flake source, so clear it. The HCI disable stops OUR legacy set;
    clr-adv stops btmgmt's."""
    rc, o = _hci("0x08", "0x000a", "00")
    _btmgmt(["clr-adv"])
    return out(ok=_hci_ok(rc, o), kind="ble-fastadv-off")



# ─── Flock BATTERY-PACK emulation (this is the accessory, NOT the camera) ────────────────────────────────────────────────────────────────
# Built from the badge's OWN matcher (WiFiScan.cpp): manufacturer-specific AD with the Xuntong
# company ID FF C8 09, matched when the name is a Penguin shape OR absent, with a serial lifted
# as "TN" + digits from the vendor bytes.
#
# ⚠ THIS IS CIRCULAR AND THAT LIMIT IS REAL. Emulating from our own matcher means a green test
# only ever proves we detect what we BELIEVE Flock looks like. It cannot detect that the real
# product changed. Do not cite these kinds as evidence the detector works against real hardware.
#
# ⚠ THE FILLER BYTES BELOW MASK TWO SEPARATE PARSER DEFECTS. Both constants end in 00, and that is
# not incidental -- it is what makes the current parser look correct:
#   1. `adEnd = adStart + adLen` is one short, so the LAST byte of a manufacturer AD is never
#      examined -> without the filler, FLOCK_MFG_FULL's final serial digit is DROPPED.
#   2. `for (i = 1; i + 3 < len; i++)` is one too strict, so a mfg AD ending AT the company ID is
#      never matched at all -> without the filler, FLOCK_MFG_BARE is NOT DETECTED. A silent miss.
# Keep the fillers (these kinds are the known-good emitters), and use the flock-real /
# flock-cid-end kinds further down to witness the defects. Neither of the two kinds here CAN fail.
FLOCK_MFG_FULL = "10ffc809544e3132333435363738393000"   # Xuntong + "TN1234567890" + filler
FLOCK_MFG_BARE = "04ffc80900"                            # Xuntong only, no serial + filler
FLOCK_NAME_10D = "0b0931323334353637383930"              # complete local name "1234567890"


def flock_full(secs):
    """Xuntong mfg data + TN serial + a 10-digit penguin name -- exercises the penguin arm AND
    the serial extraction, neither of which the bare emitter ever reached."""
    _btmgmt(["power", "on"]); _btmgmt(["le", "on"]); _btmgmt(["clr-adv"])
    rc, o = _btmgmt(["add-adv", "-d", FLOCK_MFG_FULL, "-s", FLOCK_NAME_10D, "1"])
    time.sleep(float(secs))
    _btmgmt(["rm-adv", "1"])
    return out(ok=(rc == 0), kind="flock-full", secs=float(secs), msg=o.strip()[-80:])


def flock_named(name, secs):
    """Same mfg data, but an arbitrary name -- for the 'Penguin-NNNNNNNNNN' and 'FS Ext Battery'
    arms, and for negative cases (a name that matches NEITHER shape must NOT be detected)."""
    nb = name.encode("utf-8")[:29]
    ad = "%02x09%s" % (len(nb) + 1, nb.hex())
    _btmgmt(["power", "on"]); _btmgmt(["le", "on"]); _btmgmt(["clr-adv"])
    rc, o = _btmgmt(["add-adv", "-d", FLOCK_MFG_FULL, "-s", ad, "1"])
    time.sleep(float(secs))
    _btmgmt(["rm-adv", "1"])
    return out(ok=(rc == 0), kind="flock-named", name=name, secs=float(secs), msg=o.strip()[-80:])


def flock_rotate(count, dwell):
    """Advertise as `count` DISTINCT addresses in turn -- the only way to reach the drop-new cap
    and the only way to test whether MAC-keyed dedup leaks under BLE address rotation.

    btmgmt already advertises from a non-resolvable private address rather than the adapter's BD
    address, so each power-cycle of the advertising instance yields a fresh address without
    setting one explicitly. Verified on kalipi2b: the badge logged 36:72:0E:56:14:FD, not the
    adapter's 00:1A:7D:DA:71:13.
    """
    n = int(count); d = float(dwell)
    seen = 0
    for _ in range(n):
        _btmgmt(["power", "off"]); _btmgmt(["power", "on"]); _btmgmt(["le", "on"])
        _btmgmt(["clr-adv"])
        rc, _o = _btmgmt(["add-adv", "-d", FLOCK_MFG_FULL, "-s", FLOCK_NAME_10D, "1"])
        if rc == 0:
            seen += 1
        time.sleep(d)
        _btmgmt(["rm-adv", "1"])
    return out(ok=(seen == n), kind="flock-rotate", requested=n, advertised=seen, dwell=d)


# ─── Flock fixtures that can go RED (the three above cannot) ─────────────────────────────────────
# The kinds above are honest about being circular, but NONE OF THEM CAN FAIL: every one is shaped so
# the current parser succeeds, so they can only ever confirm today's behaviour. These four exist to
# be the RED half of a red/green pair. Each carries a `source=` field, because "where did this
# expected value come from?" is the question that decides whether a green means anything.
#
#   source="capture:..."  -> bytes observed from real hardware by a third party. Can disagree with us.
#   source="derived:..."  -> bytes we constructed from our own matcher. Cannot disagree with us.

# A real Penguin battery SCAN_RSP published at ryanohoro.com (see that writeup for the capture).
# 30 bytes: 1d ff c809 | d8a0d89f4a5e (its own MAC, echoed) | 2030502a | "TN72023022000771".
# TRUTH = TN + 14 digits = 16 chars. The badge yields 15 -- adEnd = adStart + adLen is one short, so
# payLoad[29] (the final '1') is never read. That is what this fixture exists to demonstrate.
FLOCK_REAL_CAPTURE = "1dffc809d8a0d89f4a5e2030502a544e3732303233303232303030373731"
FLOCK_REAL_SERIAL  = "TN72023022000771"   # 16 chars -- what a CORRECT parser must produce

# ⚠⚠ THE MFG AD MUST BE THE LAST THING IN THE PAYLOAD THE PARSER SEES, AND A SCAN RESPONSE BREAKS
# THAT. NimBLE APPENDS scan-response bytes to getPayload() (NimBLEAdvertisedDevice.cpp:79-82), so
# passing these two payloads via -s-plus-name made `len` 12 bytes longer and the `i + 3 < len` guard
# stopped biting -- the badge detected flock-cid-end and the arm reported "D2 is FIXED" on an
# UNPATCHED parser. A textbook false green, caused by the fix for a different risk.
# So the penguin name goes in the ADV DATA *BEFORE* the manufacturer AD: the name test still passes
# via the penguin arm, no scan response is needed, and the mfg AD remains last.
#   name AD (12 B) + 03ffc809  = 16 B, mfg len byte at 12, 0xFF at 13 -> `i + 3 < 16` stops at i=12,
#                                so i=13 is NEVER TRIED -> NOT DETECTED.
#   name AD (12 B) + 04ffc80900 = 17 B -> `i + 3 < 17` reaches i=13 -> DETECTED.
# One filler byte, opposite outcomes, same run. That is the whole witness.
# ⚠ Do NOT add a scan response to these two kinds. Do NOT reorder the name after the mfg AD.

# Manufacturer AD that ENDS AT THE COMPANY ID (len 3 = type + 2 company bytes), and nothing after it.
# The scan loop is `for (i = 1; i + 3 < len; i++)` but its body only reads up to payLoad[i+2], so a
# match at i = len-3 is never attempted -> this advert is NOT DETECTED AT ALL. A silent miss, which
# is strictly worse than a truncated serial. FLOCK_MFG_BARE below is byte-identical PLUS one filler
# byte, and that single byte is the difference between "detected" and "invisible" -- so it is the
# paired positive control, and it must be emitted in the SAME RUN or a 0 proves nothing.
FLOCK_MFG_ENDS_AT_CID = "03ffc809"

# Company ID REVERSED (09 C8 instead of C8 09); every other byte identical to FLOCK_MFG_FULL. Proves
# the matcher discriminates on the ID rather than firing on any manufacturer AD -- and, more
# importantly, proves a genuine flock=0 is REACHABLE, without which a 0 cannot be told from a dead rig.
FLOCK_MFG_REVERSED_CID = "10ff09c8544e3132333435363738393000"


def flock_real(secs):
    """Replay a REAL published Penguin capture. The only Flock fixture whose expected value does not
    come from our own matcher, and therefore the only one that can catch a parser defect.
    ⚠ Deviation stated on purpose: the capture was a SCAN_RSP PDU; we emit the bytes as ADV data and
    supply the penguin name as the scan response. The predicted serial is unaffected, because adEnd
    is derived from the AD length byte, not from the payload length."""
    rc, msg, clr_ok = _ble_raw_emit(FLOCK_REAL_CAPTURE, secs, FLOCK_NAME_10D)
    return out(ok=(rc == 0), kind="flock-real", source="capture:ryanohoro.com",
               expect_serial=FLOCK_REAL_SERIAL, adv_bytes=len(FLOCK_REAL_CAPTURE) // 2,
               secs=float(secs), msg=msg, clr_ok=clr_ok)


def flock_cid_end(secs):
    """Mfg AD ending exactly at the company ID -- the witness for the SILENT MISS. Expect flock=0 on
    an unpatched badge. MUST be paired with flock-cid-end-control in the same run.
    Name is inlined into the ADV data ahead of the mfg AD; NO scan response (see the block above)."""
    rc, msg, clr_ok = _ble_raw_emit(FLOCK_NAME_10D + FLOCK_MFG_ENDS_AT_CID, secs)
    return out(ok=(rc == 0), kind="flock-cid-end", source="derived:our-own-matcher",
               expect_detect=False, note="unpatched badge must NOT detect this",
               secs=float(secs), msg=msg, clr_ok=clr_ok)


def flock_cid_end_control(secs):
    """FLOCK_MFG_BARE = flock-cid-end plus ONE filler byte. Positive control: if this is not detected
    either, the rig is dead and flock-cid-end's 0 means nothing. Same layout, no scan response."""
    rc, msg, clr_ok = _ble_raw_emit(FLOCK_NAME_10D + FLOCK_MFG_BARE, secs)
    return out(ok=(rc == 0), kind="flock-cid-end-control", source="derived:our-own-matcher",
               expect_detect=True, secs=float(secs), msg=msg, clr_ok=clr_ok)


def flock_reversed_cid(secs):
    """Negative: valid manufacturer AD, WRONG company ID. Must not be detected."""
    rc, msg, clr_ok = _ble_raw_emit(FLOCK_MFG_REVERSED_CID, secs, FLOCK_NAME_10D)
    return out(ok=(rc == 0), kind="flock-reversed-cid", source="derived:our-own-matcher",
               expect_detect=False, secs=float(secs), msg=msg, clr_ok=clr_ok)


def ble_scan(secs):
    # Discover advertising devices (verify for the badge's BLE Spam output). btmgmt
    # find runs a timed LE scan; parse dev_found lines for MAC + name.
    rc, o = _btmgmt(["find", "-l"], timeout=int(float(secs)) + 10)
    devs = []
    mac = None
    for line in o.splitlines():
        m = re.search(r"dev_found:\s+([0-9A-Fa-f:]{17})", line)
        if m:
            mac = m.group(1)
            devs.append({"mac": mac, "name": None})
        nm = re.search(r"name\s+(.+)$", line)
        if nm and devs:
            devs[-1]["name"] = nm.group(1).strip()
    # de-dup by mac
    seen, uniq = set(), []
    for d in devs:
        if d["mac"] not in seen:
            seen.add(d["mac"]); uniq.append(d)
    return out(ok=True, count=len(uniq), devices=uniq[:200])


# ─── Monitor mode + sniff + injection (wlan0) ───────────────────────────────

def mon_up(iface=MON, channel="7"):
    # wlan1 = A6210/mt76x2u dedicated monitor adapter. Release from NM (never a global
    # 'airmon-ng check kill' -- that kills NetworkManager, which may carry eth0/ssh),
    # then set monitor + channel. wlan0 (station) is untouched, so join/scan stimulus
    # and monitor sniffing can run at the SAME time.
    sh(["nmcli", "dev", "disconnect", iface], sudo=True, timeout=15)
    sh(["nmcli", "dev", "set", iface, "managed", "no"], sudo=True)
    sh(["pkill", "-f", f"wpa_supplicant.*{iface}"], sudo=True)  # targeted; leaves eth0 alone
    time.sleep(1)
    sh(["ip", "link", "set", iface, "down"], sudo=True)
    rc, o = sh(["iw", "dev", iface, "set", "type", "monitor"], sudo=True)
    sh(["ip", "link", "set", iface, "up"], sudo=True)
    sh(["iw", "dev", iface, "set", "channel", str(channel)], sudo=True)
    rc2, o2 = sh(["iw", "dev", iface, "info"], sudo=True)
    ok = "type monitor" in o2
    return out(ok=ok, iface=iface, channel=str(channel),
               settype_rc=rc, settype_err=o.strip()[-90:], info=o2.strip()[-120:])


def mon_down(iface=MON, ssid=None, pw=None):
    sh(["ip", "link", "set", iface, "down"], sudo=True)
    sh(["iw", "dev", iface, "set", "type", "managed"], sudo=True)
    sh(["ip", "link", "set", iface, "up"], sudo=True)
    sh(["nmcli", "dev", "set", iface, "managed", "yes"], sudo=True)
    time.sleep(1)
    msg = "managed"
    if ssid and pw:
        # a rescan is required after the radio was toggled or the AP won't be "found"
        sh(["nmcli", "dev", "wifi", "rescan", "ifname", iface], sudo=True, timeout=20)
        time.sleep(4)
        rc, o = _connect(ssid, pw, iface)   # _connect deletes the stale profile first
        msg = o.strip().splitlines()[-1] if o.strip() else "reconnect"
    return out(ok=True, iface=iface, msg=msg)


# tcpdump filter shortcuts for 802.11 frame subtypes on a monitor iface.
SNIFF_FILTERS = {
    "deauth": "type mgt subtype deauth",
    "auth":   "type mgt subtype auth",
    "beacon": "type mgt subtype beacon",
    "probereq": "type mgt subtype probe-req",
    "data":   "type data",
    "any":    "",
}


def sniff(secs, filt, iface=MON):
    expr = SNIFF_FILTERS.get(filt, filt)   # named or raw pcap expr
    cmd = ["timeout", str(int(float(secs))), "tcpdump", "-i", iface, "-nn", "-c", "5000"]
    if expr:
        cmd += expr.split()
    rc, o = sh(cmd, sudo=True, timeout=int(float(secs)) + 8)
    # tcpdump prints "<N> packets captured" on stderr at exit.
    m = re.search(r"(\d+) packets captured", o)
    count = int(m.group(1)) if m else sum(1 for ln in o.splitlines() if " > " in ln)
    return out(ok=True, iface=iface, filter=filt, secs=float(secs), count=count)


def sniff_hop(secs, filt, iface=MON):
    # Sniff while hopping 1/6/11/7 -- catches a channel-spread transmitter (the badge's
    # deauth cycles its discovered APs' channels) that a fixed-channel sniff would miss.
    expr = SNIFF_FILTERS.get(filt, filt)
    s = int(float(secs))
    hop = f"for i in $(seq 1 {s * 5}); do for c in 1 6 11 7; do iw dev {iface} set channel $c 2>/dev/null; sleep 0.2; done; done"
    td = f"timeout {s} tcpdump -i {iface} -nn -c 5000 {expr}"
    script = f"({hop}) & HOP=$!; {td} 2>&1; kill $HOP 2>/dev/null; wait 2>/dev/null"
    rc, o = sh(["bash", "-c", script], sudo=True, timeout=s + 12)
    m = re.search(r"(\d+) packets captured", o)
    cnt = int(m.group(1)) if m else sum(1 for ln in o.splitlines() if " > " in ln)
    return out(ok=True, iface=iface, filter=filt, secs=s, count=cnt)


def deauth(iface=MON, chan="7", secs="6"):
    # mdk4 deauth-amok on a fixed channel: reliably TRANSMITS deauth frames (blasts
    # every AP/client it sees on that channel) that the badge's deauth detector counts.
    # (aireplay --deauth against a fake BSSID doesn't inject on mt76x2u -- no frames go out.)
    s = int(secs) if str(secs).isdigit() else 6
    rc, o = sh(["timeout", str(s), "mdk4", iface, "d", "-c", str(chan), "-s", "200"],
               sudo=True, timeout=s + 8)
    # mdk4 reprints "Packets sent: N" as a PROGRESS line while it runs, so re.search() returns
    # the FIRST match -- an early count, often 1 -- not the final total. Measured: a run that
    # actually sent 5345 reported sent=1. That still satisfied a `sent > 0` control, so it never
    # failed loudly; it just quietly under-reported the stimulus in every log that quotes it.
    # Take the LAST match.
    hits = re.findall(r"Packets sent:\s+(\d+)", o)
    return out(ok=True, iface=iface, chan=str(chan), secs=s,
               sent=int(hits[-1]) if hits else 0, msg=o.strip()[-100:])


# ─── Self-check ─────────────────────────────────────────────────────────────

def caps():
    tools = {t: bool(shutil.which(t)) for t in
             ("iw", "nmcli", "btmgmt", "tcpdump", "aireplay-ng", "airmon-ng")}
    rc, links = sh(["ip", "-br", "addr"])
    rc2, iwd = sh(["iw", "dev"], sudo=True)
    monitor = "* monitor" in (sh(["iw", "list"], sudo=True)[1])
    return out(ok=True, tools=tools, monitor_capable=monitor,
               links=[l.split()[0] for l in links.splitlines() if l.strip()],
               wlan_present=(WLAN in iwd))


def _beacon_py(kind, secs, chan, iface, hop=True):
    """Build a self-contained scapy script (run as root via sudo) that floods the
    kind's synthetic beacon(s) on `iface` for `secs`. Returns None for unknown kind."""
    bodies = {
        # Pwnagotchi: beacon w/ source MAC DE:AD:BE:EF:DE:AD + a name/pwnd_tot JSON
        # (raw byte-scanned, so a 221 vendor IE is fine).
        "pwnagotchi":
            'mac="de:ad:be:ef:de:ad"\n'
            'j=b\'{"name":"pwnpal","pwnd_tot":42,"version":"1.5.5","uptime":9,"policy":{"deauth":true}}\'\n'
            'frames=[RadioTap()/Dot11(type=0,subtype=8,addr1="ff:ff:ff:ff:ff:ff",addr2=mac,addr3=mac)'
            '/Dot11Beacon(cap="ESS")/Dot11Elt(ID=0,info="pwnagotchi")/Dot11Elt(ID=221,info=j)]\n',
        # Pwnagotchi with a DIFFERENT NAME PER FRAME -- 150 synthetic distinct devices from one
        # injector. The badge dedups the pwnagotchi list by NAME (they all share the MAC
        # de:ad:be:ef:de:ad, so the MAC carries no identity), which means varying the JSON `name`
        # is enough to simulate a crowded room.
        # WHY IT EXISTS: the dedup/eviction work needed the list DRIVEN PAST ITS CAP, and the
        # worst-case cost question ("O(N) walk with a two-String copy per entry visited -- does it
        # lag at 100 entries?") cannot be answered with one device in the list. The owner asked
        # whether kalipi could just spam signatures; for the WiFi side the answer is yes, and this
        # is it. (For the BT list the equivalent needs LE random-address rotation per advert, which
        # is far slower -- prefer a CB_BT_LIST_CAP-lowered test build there.)
        "pwnagotchi-many":
            'mac="de:ad:be:ef:de:ad"\n'
            'frames=[]\n'
            'for _i in range(150):\n'
            '    _j=(\'{"name":"pwn%03d","pwnd_tot":%d,"version":"1.5.5","uptime":9,'
            '"policy":{"deauth":true}}\' % (_i, _i)).encode()\n'
            '    frames.append(RadioTap()/Dot11(type=0,subtype=8,addr1="ff:ff:ff:ff:ff:ff",'
            'addr2=mac,addr3=mac)/Dot11Beacon(cap="ESS")/Dot11Elt(ID=0,info="pwnagotchi")'
            '/Dot11Elt(ID=221,info=_j))\n',
        # Two DISJOINT pwnagotchi name ranges, for testing list EVICTION decisively.
        # Why not one flood of many names: with a big burst the badge captures only a fraction of
        # the frames, and if it consistently catches the same part of the burst it keeps seeing the
        # same names -- which are already in the list, so dedup updates them in place and membership
        # legitimately does not change. "Membership frozen" then cannot distinguish working dedup
        # from broken eviction. Measured exactly that: names pinned at pwn144-149 while beacons
        # climbed 117 -> 412.
        # With two disjoint ranges the test is capture-timing independent: fill the list from range
        # A, stop, then send ONLY range B. Every B name is necessarily absent from the list, so a
        # working evictor must replace the A names. If the list still shows A, eviction is broken.
        "pwnagotchi-rangeA":
            'mac="de:ad:be:ef:de:ad"\n'
            'frames=[]\n'
            'for _i in range(0, 6):\n'
            '    _j=(\'{"name":"pwnA%02d","pwnd_tot":1,"version":"1.5.5","uptime":9,'
            '"policy":{"deauth":true}}\' % _i).encode()\n'
            '    frames.append(RadioTap()/Dot11(type=0,subtype=8,addr1="ff:ff:ff:ff:ff:ff",'
            'addr2=mac,addr3=mac)/Dot11Beacon(cap="ESS")/Dot11Elt(ID=0,info="pwnagotchi")'
            '/Dot11Elt(ID=221,info=_j))\n',
        "pwnagotchi-rangeB":
            'mac="de:ad:be:ef:de:ad"\n'
            'frames=[]\n'
            'for _i in range(0, 6):\n'
            '    _j=(\'{"name":"pwnB%02d","pwnd_tot":2,"version":"1.5.5","uptime":9,'
            '"policy":{"deauth":true}}\' % _i).encode()\n'
            '    frames.append(RadioTap()/Dot11(type=0,subtype=8,addr1="ff:ff:ff:ff:ff:ff",'
            'addr2=mac,addr3=mac)/Dot11Beacon(cap="ESS")/Dot11Elt(ID=0,info="pwnagotchi")'
            '/Dot11Elt(ID=221,info=_j))\n',
        # A SINGLE distinctive pwnagotchi name, hammered. The decisive eviction stimulus.
        # Multi-name floods proved ambiguous: the badge captures only a fraction of a burst, and the
        # `beacon` counter includes ambient traffic, so "a name never appeared" could not be
        # distinguished from "its frame was never captured". One name repeated for the whole window
        # makes capture near-certain, so if the list is at its cap and this name still never
        # appears, the evictor genuinely is not running.
        "pwnagotchi-one":
            'mac="de:ad:be:ef:de:ad"\n'
            'j=b\'{"name":"pwnZZZ","pwnd_tot":7,"version":"1.5.5","uptime":9,'
            '"policy":{"deauth":true}}\'\n'
            'frames=[RadioTap()/Dot11(type=0,subtype=8,addr1="ff:ff:ff:ff:ff:ff",addr2=mac,addr3=mac)'
            '/Dot11Beacon(cap="ESS")/Dot11Elt(ID=0,info="pwnagotchi")/Dot11Elt(ID=221,info=j)]\n',
        # Espressif: beacon whose BSSID OUI is Espressif (24:0A:C4).
        "espressif":
            'mac="24:0a:c4:13:37:01"\n'
            'frames=[RadioTap()/Dot11(type=0,subtype=8,addr1="ff:ff:ff:ff:ff:ff",addr2=mac,addr3=mac)'
            '/Dot11Beacon(cap="ESS")/Dot11Elt(ID=0,info=SSID)/Dot11Elt(ID=3,info=bytes([CH]))]\n',
        # Rogue AP / Pineapple: beacon w/ SUSPICIOUS_ALWAYS OUI 00:13:37 (Pineapple MK7).
        "rogueap":
            'mac="00:13:37:aa:bb:cc"\n'
            'frames=[RadioTap()/Dot11(type=0,subtype=8,addr1="ff:ff:ff:ff:ff:ff",addr2=mac,addr3=mac)'
            '/Dot11Beacon(cap="ESS")/Dot11Elt(ID=0,info=SSID)/Dot11Elt(ID=3,info=bytes([CH]))]\n',
        # Evil Twin / MultiSSID: one BSSID advertising >=3 distinct SSIDs.
        "eviltwin":
            'mac="de:ad:00:be:ef:01"\n'
            'frames=[RadioTap()/Dot11(type=0,subtype=8,addr1="ff:ff:ff:ff:ff:ff",addr2=mac,addr3=mac)'
            '/Dot11Beacon(cap="ESS")/Dot11Elt(ID=0,info=s) '
            'for s in ["CoffeeShop","CoffeeShop_Guest","Free_WiFi","Airport_WiFi"]]\n',
    }
    # A NONCE SSID may be appended as "<kind>:<ssid>" (espressif / rogueap) to defeat
    # STALE AP-LIST HITS: the scanned AP list persists across tool switches by design, so a
    # fixed SSID left by a prior run reads as a fresh detection (FIX-4, both pre-reviews).
    # Split it off; default to the template's built-in name. Injected as SSID for the two
    # templates that reference it; harmlessly unused by the others.
    base, _, nonce = kind.partition(":")
    if base not in bodies:
        return None
    ssid = nonce or {"espressif": "esp-synth", "rogueap": "Pineapple"}.get(base, "")
    # hop=True: cycle the injector across 1/6/11 so a channel-HOPPING badge overlaps often.
    # hop=False: PIN the injector to `chan` -- for a badge the caller has pinned to one channel
    # (raw_channel), no-hop gives ~3x the on-channel frames, which the high-threshold detectors
    # (multissid needs >=3 distinct SSIDs) need under variable emission. A fixed-channel injector
    # only lands ~1/3 of the time against a hopping badge, so no-hop is ONLY for a pinned DUT.
    chans = "[1,6,11]" if hop else "[%d]" % chan
    return ("from scapy.all import RadioTap,Dot11,Dot11Beacon,Dot11Elt,sendp\n"
            "import time,os\n"
            "SSID=%r\n" % ssid +
            "CH=%d\n" % chan
            + bodies[base] +
            "CHANS=%s; t0=time.time(); n=0; i=0\n" % chans +
            "while time.time()-t0 < %f:\n" % secs +
            "    os.system('iw dev %s set channel %%d 2>/dev/null' %% CHANS[i%%len(CHANS)]); i+=1\n" % iface +
            "    be=time.time()+0.6\n"
            "    while time.time()<be:\n"
            "        for f in frames:\n"
            "            sendp(f, iface='%s', verbose=False); n+=1\n" % iface +
            "        time.sleep(0.08)\n"
            "print('sent', n)\n")


def wifi_beacon(kind, secs="6", chan="6", hop="1", iface=MON):
    """Inject synthetic 802.11 beacons that trip the badge's WiFi detectors, so they
    fire without the real device. kind: pwnagotchi | espressif | rogueap | eviltwin.
    Monitor on `iface`(wlan1)/`chan`, then flood via scapy (as root/sudo) for `secs`.
    The badge is pinned to the same channel by the caller for reliable capture.
    hop="0" PINS the injector to `chan` (no 1/6/11 cycling) -- use ONLY when the DUT is
    itself pinned to that channel, to give ~3x on-channel frames (e.g. eviltwin/multissid)."""
    mon_up(iface, str(chan))
    script = _beacon_py(kind, float(secs), int(chan), iface, hop=(str(hop) != "0"))
    if script is None:
        return out(ok=False, error="unknown kind: %s" % kind)
    rc, o = sh(["python3", "-c", script], sudo=True, timeout=float(secs) + 25)
    sent = 0
    for tok in o.split():
        if tok.isdigit():
            sent = int(tok)
    return out(ok=(rc == 0), kind=kind, chan=int(chan), sent=sent, rc=rc, msg=o.strip()[-100:])


def wifi_probeflood(names="30", secs="20", chan="6", iface=MON):
    """Inject DIRECTED probe requests carrying `names` distinct SSIDs (CBPRB00, CBPRB01, ...).

    Why this exists: nothing in this file could previously emit a probe request with a
    non-empty SSID. `wifi-scan` runs `iw dev wlan0 scan`, which is self-paced (~0.66
    sweeps/s) and emits mostly WILDCARD probes -- zero-length SSID -- which the badge
    discards before recording (`if (probe_req_essid.length() > 0)`). `wifi-beacon` emits
    BEACONS (subtype 8), which never reach the probe path at all. So the badge's
    Analyze > Probes list had no stimulus, and its list cap had no way to be exercised.

    Frame layout is chosen to match what the badge actually parses, not what scapy finds
    convenient: the firmware reads the SSID LENGTH from payload[25] and the SSID from
    payload[26..]. A management header is 24 bytes (FC 2 + dur 2 + addr1/2/3 18 + seq 2),
    so the first information element lands at 24 (ID), 25 (len), 26 (data). Putting the
    SSID element FIRST is therefore mandatory -- a probe request with any other IE ahead
    of it parses as garbage on the badge, and a fixture that did that would exercise the
    wrong bytes while looking correct.

    Each SSID gets its own source MAC so the flood also resembles many distinct clients
    rather than one shouting device.

    NOT a real capture: this is a synthesised frame, so it certifies the badge's handling
    of well-formed directed probes only. It says nothing about malformed ones -- that is
    verify_sniffer_bounds.py's job.
    """
    n_names = max(1, int(names))
    mon_up(iface, str(chan))
    # ⚠ Build this as a LIST of parts, formatting each explicitly. Do NOT go back to one big
    # implicitly-concatenated literal with a trailing `% secs`: `%` binds tighter than `+`, so a
    # trailing format operator silently consumes every `%` in the adjacent literals above it. That
    # is not hypothetical -- the first version of this function did exactly that and the `%02x` in
    # the MAC line was eaten by `% float(secs)`, failing with "an integer is required, not float".
    # It failed loudly here; the same shape inside a frame body would have emitted wrong bytes.
    parts = [
        "from scapy.all import RadioTap,Dot11,Dot11ProbeReq,Dot11Elt,sendp",
        "import time,os",
        "N=%d" % n_names,
        "IFACE=%r" % str(iface),
        "DUR=%f" % float(secs),
        "frames=[]",
        "for i in range(N):",
        "    mac='02:cb:{:02x}:{:02x}:50:52'.format(i & 0xff, (i >> 8) & 0xff)",
        "    s='CBPRB{:02d}'.format(i)",
        "    frames.append(RadioTap()/Dot11(type=0,subtype=4,addr1='ff:ff:ff:ff:ff:ff',"
        "addr2=mac,addr3='ff:ff:ff:ff:ff:ff')/Dot11ProbeReq()"
        "/Dot11Elt(ID=0,info=s)/Dot11Elt(ID=1,info=b'\\x02\\x04\\x0b\\x16'))",
        # Hop 1/6/11 like the beacon injector: the badge channel-hops 1 s across 14 channels,
        # so a fixed-channel injector overlaps it only ~1/14 of the time.
        "CHANS=[1,6,11]; t0=time.time(); n=0; i=0",
        "while time.time()-t0 < DUR:",
        "    os.system('iw dev {} set channel {} 2>/dev/null'.format(IFACE, CHANS[i%3])); i+=1",
        "    be=time.time()+0.6",
        "    while time.time()<be:",
        "        for f in frames:",
        "            sendp(f, iface=IFACE, verbose=False); n+=1",
        "        time.sleep(0.02)",
        "print('sent', n)",
    ]
    script = "\n".join(parts) + "\n"
    rc, o = sh(["python3", "-c", script], sudo=True, timeout=float(secs) + 30)
    sent = 0
    for tok in o.split():
        if tok.isdigit():
            sent = int(tok)
    # `sent` is the STIMULUS-LANDED control: a run that emitted 0 frames must never be
    # read as "the badge saw nothing", which is what a bare badge-side count would say.
    return out(ok=(rc == 0 and sent > 0), names=n_names, chan=int(chan),
               sent=sent, rc=rc, msg=o.strip()[-100:])


def deauth_synth(chan="1", secs="20", iface=MON, bssid="02:cb:de:00:00:01"):
    """Inject deauth frames at a FABRICATED BSSID on a fixed channel.

    Why this exists alongside `deauth`: mdk4's amok mode blasts the APs and clients it
    FINDS on the channel, so on a channel with no APs in range it sends NOTHING -- measured,
    `sent=0` with empty output on ch1 while ch6/ch7 produced thousands. That makes it
    unusable for a channel-steering test, whose whole point is choosing the channel freely.

    It is also better behaviour: mdk4 amok deauths REAL neighbouring networks. This uses
    locally-administered 02: addresses that belong to nobody, the same discipline
    deauth_badge2badge.py already follows, so nothing real is disrupted.

    reason=7 (class-3 frame from nonassociated STA) is the ordinary "you are not associated
    with us" deauth, which is what the badge's detector counts.
    """
    mon_up(iface, str(chan))
    parts = [
        "from scapy.all import RadioTap,Dot11,Dot11Deauth,sendp",
        "import time",
        "IFACE=%r" % str(iface),
        "DUR=%f" % float(secs),
        # Fabricated, locally-administered (02:) -- not a real BSSID anywhere.
        # PARAMETERISED (2026-07-28) so a caller can emit a per-run NONCE and prove the
        # frames received are the ones IT sent, rather than a neighbour's lookalike.
        # addr2 (TA) and addr3 (BSSID) are set to the SAME value on purpose: for a deauth
        # tshark's `wlan.bssid` is addr3, so a witness filtering only one of the two can
        # read 0 against a perfectly good capture -- indistinguishable from a dead radio.
        "bssid=%r" % str(bssid),
        "sta='02:cb:de:00:00:02'",
        "f=RadioTap()/Dot11(type=0,subtype=12,addr1=sta,addr2=bssid,addr3=bssid)/Dot11Deauth(reason=7)",
        "t0=time.time(); n=0",
        "while time.time()-t0 < DUR:",
        "    sendp(f, iface=IFACE, count=20, inter=0.002, verbose=False); n+=20",
        "print('sent', n)",
    ]
    rc, o = sh(["python3", "-c", "\n".join(parts) + "\n"], sudo=True, timeout=float(secs) + 30)
    # LABELLED match, not "last numeric token anywhere in the output". The unlabelled scan is
    # the same class as the mdk4 `sent` bug fixed alongside this one: any scapy warning that
    # happens to end in a number would silently become the stimulus count.
    m = re.search(r"\bsent (\d+)", o)
    sent = int(m.group(1)) if m else 0
    # Emitter-side count only. On this box (mt76x2u) injection is independently demonstrated;
    # do NOT reuse this control on an adapter whose TX has not been proven -- kalipi2b reports
    # frames sent while radiating nothing.
    return out(ok=(rc == 0 and sent > 0), chan=int(chan), secs=float(secs),
               sent=sent, rc=rc, msg=o.strip()[-120:])


def beacon_ssid(ssid, secs="20", chan="6", iface=MON):
    """Beacon ONE chosen SSID from a fabricated BSSID -- an RX nonce for the badge's
    Scan/Analyze tools. The point is the reverse of the TX tests: the badge should
    RECEIVE the exact unique value we transmit. `ap_scan <ssid>` on the badge returns
    selected>=0 only if it heard THIS beacon; a never-sent SSID returns -1 (the negative
    control that proves the scan does not hallucinate).

    Fabricated locally-administered (02:) BSSID, and a DS-parameter IE carrying `chan` so
    the SSID is unambiguously on the channel we claim. Blocking for `secs` -- the caller
    runs it in a thread so the badge's ~16 s AP-scan sweep overlaps the emission.
    """
    mon_up(iface, str(chan))
    ch = int(chan)
    # NB: build the scapy script with NO %-format on any line that contains literal scapy
    # code -- an earlier version put "%02x" (literal) next to a "% ch" and Python tried to
    # fill all five specs from one value ("not enough arguments for format string"). Pass
    # the runtime values as their own assigned vars instead.
    parts = [
        "from scapy.all import RadioTap,Dot11,Dot11Beacon,Dot11Elt,sendp",
        "import time",
        "IFACE=" + repr(str(iface)),
        "DUR=" + repr(float(secs)),
        "SSID=" + repr(str(ssid)),
        "CH=" + repr(ch),
        # fabricated locally-administered BSSID, last byte = channel so distinct per channel
        "bssid='02:cb:de:00:00:%02x' % CH",
        "f=(RadioTap()/Dot11(type=0,subtype=8,addr1='ff:ff:ff:ff:ff:ff',addr2=bssid,addr3=bssid)"
        "/Dot11Beacon(cap='ESS')/Dot11Elt(ID=0,info=SSID)/Dot11Elt(ID=3,info=chr(CH)))",
        "t0=time.time(); n=0",
        "while time.time()-t0 < DUR:",
        "    sendp(f, iface=IFACE, count=20, inter=0.05, verbose=False); n+=20",
        "print('sent', n)",
    ]
    rc, o = sh(["python3", "-c", "\n".join(parts) + "\n"], sudo=True, timeout=float(secs) + 30)
    m = re.search(r"\bsent (\d+)", o)
    sent = int(m.group(1)) if m else 0
    return out(ok=(rc == 0 and sent > 0), ssid=str(ssid), chan=ch, secs=float(secs),
               sent=sent, rc=rc, msg=o.strip()[-120:])


def scan_ch(ssid, iface=WLAN):
    # Live 2.4GHz channel of `ssid`. The AP does auto-channel-selection and roams
    # (7 -> 11 -> 4 observed), so tool_suite auto-detects instead of hardcoding
    # SHIP_CH -- a stale channel makes the deauth-inject (3.2) + EAPOL (3.7) rows
    # target an empty channel and false-fail. Returns {channel:N} (2.4GHz, <=14).
    sh(["nmcli", "dev", "wifi", "rescan", "ifname", iface], sudo=True, timeout=20)
    time.sleep(2)
    rc, o = sh(["nmcli", "-t", "-f", "SSID,CHAN", "dev", "wifi", "list", "ifname", iface],
               sudo=True, timeout=15)
    for line in o.splitlines():
        # nmcli -t escapes ':' within fields as '\:'; SSID,CHAN -> "<ssid>:<chan>".
        m = re.match(r"^(.*?):(\d+)$", line.strip())
        if not m:
            continue
        name = m.group(1).replace("\\:", ":")
        chan = int(m.group(2))
        if name == ssid and 1 <= chan <= 14:      # exact SSID, 2.4GHz band only
            return out(ok=True, ssid=ssid, channel=chan, iface=iface)
    return out(ok=False, ssid=ssid, err="ssid not found on 2.4GHz", iface=iface)


DISPATCH = {
    "caps": caps,
    "wifi-join": wifi_join, "wifi-leave": wifi_leave, "wifi-cycle": wifi_cycle,
    "wifi-scan": wifi_scan, "wifi-state": wifi_state, "scan-ch": scan_ch,
    "ble-name": ble_name, "ble-raw": ble_raw, "ble-scan": ble_scan,
    "ble-fastadv-on": ble_fastadv_on, "ble-fastadv-off": ble_fastadv_off,
    "flock-full": flock_full, "flock-named": flock_named, "flock-rotate": flock_rotate,
    "flock-real": flock_real, "flock-cid-end": flock_cid_end,
    "flock-cid-end-control": flock_cid_end_control, "flock-reversed-cid": flock_reversed_cid,
    "mon-up": mon_up, "mon-down": mon_down, "sniff": sniff, "sniff-hop": sniff_hop,
    "deauth": deauth, "wifi-beacon": wifi_beacon,
    "wifi-probeflood": wifi_probeflood,
    "deauth-synth": deauth_synth,
    "beacon-ssid": beacon_ssid,
}


def main(argv):
    if not argv or argv[0] not in DISPATCH:
        print(json.dumps({"ok": False, "err": "usage",
                          "primitives": sorted(DISPATCH)}))
        return 2
    try:
        return DISPATCH[argv[0]](*argv[1:]) or 0
    except TypeError as e:
        return out(ok=False, err=f"bad args for {argv[0]}: {e}")
    except Exception as e:
        return out(ok=False, err=f"{type(e).__name__}: {e}")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
