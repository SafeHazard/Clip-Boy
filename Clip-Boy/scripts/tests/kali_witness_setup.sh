#!/bin/bash
# kali_witness_setup.sh -- provision the Kali laptop as the RF WITNESS box.
#
# Run this after EVERY boot: the box runs a Kali LIVE image, so nothing here
# survives a reboot.
#
#   ssh kali@192.168.1.11 'bash -s' < scripts/tests/kali_witness_setup.sh
#
# WHAT THIS BOX IS (measured 2026-07-28, not inferred from part numbers):
#   WiFi  Intel AX211 CNVi [8086:7a70], iwlwifi, wlan0
#         -> monitor RX WORKS: link-type IEEE802_11_RADIO, ambient beacons
#            captured with no stimulus, 112 frames / 0 dropped in 15 s on ch6.
#         -> INJECTION IS NOT SUPPORTED (iwlwifi). Inject from kalipi wlan1
#            (mt76x2u); this box only listens.
#   BT    Intel AX211 Bluetooth, hci0 -> BLE advert RX works.
#   NIC   "Killer" here is the E3000 2.5GbE ETHERNET, not the wireless. The
#         wireless is Intel. Do not plan around a Killer wifi chipset.
#
# This is the first box in the fleet that can actually capture. kalipi2b
# (RTL8192CU) and kalipi2w (brcmfmac) BOTH report `type monitor`, set a channel
# without error, and capture ZERO packets -- not even ambient beacons. That is
# why the verify step below is mandatory and not decoration: a witness that
# cannot read POSITIVE turns every negative result into a false green.
#
# ############################################################################
# # HOST PREP THIS SCRIPT CANNOT DO FOR ITSELF -- REDO AFTER EVERY REBOOT.
# #
# # The box boots a Kali LIVE image with NO persistence (verified: / is an
# # overlay whose upperdir is tmpfs, and /proc/cmdline carries no `persistence`).
# # So the two manual changes below are ERASED on every power cycle, and until
# # they are back, THIS SCRIPT CANNOT RUN AT ALL -- it needs key auth to get in
# # and passwordless sudo to configure the radios. Chicken-and-egg: do these
# # first, from the console or over a password login.
# #
# #   1. Passwordless sudo -- appended to the END of /etc/sudoers (via visudo):
# #        kali ALL=(ALL:ALL) NOPASSWD:ALL
# #      Must be last; a later line would override it. `sudo -n true` verifies.
# #
# #   2. Our SSH public key -> /home/kali/.ssh/authorized_keys
# #      (dir 0700, file 0600, both owned by kali:kali, or sshd ignores it).
# #
# # Both were set up by the owner 2026-07-28. Console login for the box is in
# # per-machine memory (`kali_rf_witness`), NOT here -- this file is published
# # to the public mirror, so no credentials in it, ever.
# #
# # If this rig becomes standing rather than throwaway, building a PERSISTENT
# # Kali USB retires this whole block. The boot medium is the PNY [154b:00ad].
# ############################################################################
set -u
CH="${1:-6}"

# --- WiFi monitor ---------------------------------------------------------
# `iw set type monitor` fails EBUSY (-16) while the interface is admin-UP.
# Bring it DOWN first. Do NOT use `airmon-ng check kill` here: on a box where
# NetworkManager IS running it also drops eth0, which is the link we manage
# this machine over. (On this live image NM is not running at all.)
sudo rfkill unblock wifi
sudo nmcli device set wlan0 managed no 2>/dev/null   # no-op if NM is absent
sudo ip link set wlan0 down
sudo iw dev wlan0 set type monitor || { echo "FATAL: monitor type refused"; exit 1; }
sudo ip link set wlan0 up
sudo iw dev wlan0 set channel "$CH" || { echo "FATAL: channel $CH refused"; exit 1; }

# --- BLE ------------------------------------------------------------------
# The adapter comes up `powered br/edr` with NO low-energy. btmgmt find then
# fails with `status 0x0b (Rejected)`, which reads like a broken adapter and is
# not -- `le on` is simply required. bluetoothd may be inactive; that is fine,
# btmgmt speaks the mgmt socket directly.
#
# WHICH adapter: prefer the USB CSR dongle over the built-in AX211. Not because
# the AX211 fails -- it works -- but because AX211 wifi and bluetooth are one
# CNVi combo module sharing antennas under coexistence arbitration. Witnessing
# a wifi tool and a BLE tool CONCURRENTLY on it makes the two witnesses steal
# airtime from each other, and that failure is SILENT: fewer frames, no error,
# which reads as "the badge transmitted less". A separate radio decouples them.
# Measured on this rig: CSR 171 adverts / 12 s vs AX211 9, and both saw the
# same device addresses -- independent cross-validation, not just a bigger number.
#
# Selected by USB VID:PID, never by hciN -- indices renumber across reboot and
# replug, and "witness on hci1" silently becomes the wrong radio.
BLE_IDX=""
for H in $(ls /sys/class/bluetooth/ 2>/dev/null | grep '^hci'); do
  DEVP=$(readlink -f "/sys/class/bluetooth/$H/device" 2>/dev/null)
  VEND=$(cat "$DEVP/../idVendor" 2>/dev/null); PROD=$(cat "$DEVP/../idProduct" 2>/dev/null)
  [ "$VEND:$PROD" = "0a12:0001" ] && BLE_IDX="${H#hci}"   # CSR dongle wins
done
[ -z "$BLE_IDX" ] && BLE_IDX=0 && echo "  [note] CSR dongle absent -- falling back to built-in BT (hci0)."
echo "  [note] BLE witness = hci$BLE_IDX"
sudo rfkill unblock bluetooth
# ⚠ ALWAYS down/up FIRST. MEASURED 2026-07-29: the CSR dongle GOES DEAF after repeated
# discovery sessions -- `btmgmt find` drops from ~166 adverts to 0 while the adapter still
# reports `UP RUNNING`, `powered br/edr le`, and the USB device is still enumerated. Every
# status read says healthy; only a scan reveals it. A down/up cycle restores it (0 -> 132
# adverts, measured back-to-back).
#
# This is why the reset is unconditional rather than triggered by a failed check: an
# unattended run that wedges mid-suite would otherwise report every subsequent BLE tool as
# transmitting nothing, and those zeros are indistinguishable from a real badge defect.
sudo hciconfig "hci$BLE_IDX" down 2>/dev/null
sleep 1
sudo hciconfig "hci$BLE_IDX" up 2>/dev/null
sleep 2
sudo btmgmt --index "$BLE_IDX" power on >/dev/null 2>&1
sudo btmgmt --index "$BLE_IDX" le on    >/dev/null 2>&1

# --- VERIFY: prove both observables can read POSITIVE, right now ----------
# Never skip. An observable that is always quiet reports success forever.
#
# ⚠ THE EXIT CODE IS THE PRODUCT. This block printed `[FAIL]` from inside `||`
# clauses and then fell through to `ip -br addr show eth0`, so the script's
# status was `ip`'s -- i.e. 0. A DEAF WITNESS PREFLIGHTED GREEN, in the one
# script whose entire job is to stop that. Caught by adversarial review, then
# reproduced. Every check now records into FAILED and the script exits 3.
# Callers MUST branch on the exit code, not on the text.
#
# Prove the red path on demand -- an untested failure path is indistinguishable
# from one that cannot fire:
#   WITNESS_FORCE_FAIL=wifi bash kali_witness_setup.sh   -> expect exit 3
#   WITNESS_FORCE_FAIL=ble  bash kali_witness_setup.sh   -> expect exit 3
FAILED=0
FORCE="${WITNESS_FORCE_FAIL:-}"

# tshark is what the harness parses captures with, and it was never checked --
# on a no-persistence live image "installed yesterday" is not evidence.
if command -v tshark >/dev/null 2>&1; then
  echo "  [OK]   tshark present ($(tshark --version 2>/dev/null | head -1))"
else
  echo "  [FAIL] tshark MISSING -- capture parsing is impossible"; FAILED=1
fi

echo "=== wifi witness: ambient capture on ch$CH (no stimulus) ==="
# ⚠ BPF beacon filter + TIME bound, NOT `-c 15`. MEASURED 2026-07-29: `-c N` stops after
# N frames of ANY type, and control/data/ACK frames on a busy channel can fill the quota
# BEFORE a single beacon arrives -- the check reported "0 ambient beacons" (FAIL) on a
# radio that a time-bounded capture proved healthy (92 beacons / 823 frames / 10 s, same
# radio, same channel, same instant). A false-negative control -- it fails the whole
# preflight, so it would have blocked every downstream test on a working witness. The
# beacon BPF makes every captured frame a beacon, so nothing races the quota; the
# link-type banner still prints to stderr regardless of filter.
WIFI=$(sudo timeout 8 tcpdump -i wlan0 -e -n type mgt subtype beacon 2>&1)
[ "$FORCE" = "wifi" ] && WIFI="(forced-fail: simulated deaf radio)"
if echo "$WIFI" | grep -q "IEEE802_11_RADIO"; then
  echo "  [OK]   link-type IEEE802_11_RADIO"
else
  echo "  [FAIL] wrong link-type -- this is the kalipi2w failure mode"; FAILED=1
fi
BC=$(echo "$WIFI" | grep -c "Beacon")
if [ "$BC" -gt 0 ]; then
  echo "  [OK]   $BC ambient beacons -- the radio really hears the room"
else
  echo "  [FAIL] 0 ambient beacons -- CANNOT WITNESS, do not trust negatives"; FAILED=1
fi

echo "=== ble witness: ambient adverts on hci$BLE_IDX (no stimulus) ==="
BLEN=$(sudo timeout 12 btmgmt --index "$BLE_IDX" find 2>&1 | grep -c "dev_found")
[ "$FORCE" = "ble" ] && BLEN=0
if [ "$BLEN" -gt 0 ]; then
  echo "  [OK]   $BLEN BLE adverts seen (btmgmt: liveness only)"
else
  echo "  [FAIL] 0 adverts -- check 'btmgmt le on' (0x0b Rejected = LE off)"; FAILED=1
fi

# btmon is the instrument the HARNESS actually parses -- btmgmt above only proves the
# radio is alive. Validating btmgmt while the tests read btmon is the gate-blindness
# shape: a preflight blessing a tool it will not use. btmgmt reports one merged record
# per DEVICE and DISCARDS manufacturer data; btmon gives one HCI LE Advertising Report
# per advert with the full AD payload, which is where the BLE nonce lives.
echo "=== ble PARSE path: btmon LE Advertising Report records ==="
if ! command -v btmon >/dev/null 2>&1; then
  echo "  [FAIL] btmon MISSING -- BLE advert parsing is impossible"; FAILED=1
else
  # RETRY on empty. The cheap CSR dongle wedges so often it can go deaf between the
  # reset above and this capture -- even WITHIN one preflight. A single empty capture is
  # a recoverable wedge, not a dead witness, so reset + retry twice before failing. The
  # FAIL still fires if it stays deaf (fail-closed preserved); WITNESS_FORCE_FAIL=btmon
  # forces the dead path for the red-first check.
  ADV=0
  for _try in 1 2 3; do
    [ "$FORCE" = "btmon" ] && break
    sudo rm -f /tmp/_pf.snoop
    sudo sh -c "nohup timeout 8 btmon -w /tmp/_pf.snoop >/dev/null 2>&1 &"
    sleep 1
    sudo timeout 6 btmgmt --index "$BLE_IDX" find >/dev/null 2>&1   # stir the air
    while pgrep btmon >/dev/null 2>&1; do sleep 1; done
    ADV=$(sudo btmon -r /tmp/_pf.snoop 2>/dev/null | grep -c "LE Advertising Report")
    [ "$ADV" -gt 0 ] && break
    # deaf -- reset the CSR and try again
    sudo hciconfig "hci$BLE_IDX" down 2>/dev/null; sleep 1
    sudo hciconfig "hci$BLE_IDX" up 2>/dev/null; sleep 2
    sudo btmgmt --index "$BLE_IDX" le on >/dev/null 2>&1
  done
  [ "$FORCE" = "btmon" ] && ADV=0
  if [ "$ADV" -gt 0 ]; then
    echo "  [OK]   $ADV LE Advertising Reports parsed out of a btmon capture"
  else
    echo "  [FAIL] btmon captured/parsed 0 adverts after 3 tries -- the BLE oracle is blind"; FAILED=1
  fi
fi

# A GENUINE detection-path red, not a stubbed value. WITNESS_FORCE_FAIL injects
# downstream of the observable, so it proves the REPORTING path only -- which was the
# real defect, but is not the same as proving detection works. eth0 is a real interface
# with a real capture that is EN10MB and carries no beacons: the literal kalipi2w
# failure mode, reproduced on demand.
if [ "${WITNESS_SELFTEST_ETH0:-}" = "1" ]; then
  echo "=== SELFTEST: same check against eth0 (must NOT look like a wifi witness) ==="
  E=$(sudo timeout 8 tcpdump -i eth0 -c 10 -e -n 2>&1)
  echo "$E" | grep -q "IEEE802_11_RADIO" \
    && echo "  [SELFTEST-FAIL] eth0 reported IEEE802_11_RADIO -- the check cannot discriminate" \
    || echo "  [SELFTEST-OK]   eth0 correctly does NOT report IEEE802_11_RADIO"
fi

echo "=== mgmt link still up? ==="
ip -br addr show eth0 || true

if [ "$FAILED" -ne 0 ]; then
  echo "PREFLIGHT FAILED -- the witness cannot be trusted. Do NOT interpret zeros."
  exit 3
fi
echo "PREFLIGHT OK"
exit 0
