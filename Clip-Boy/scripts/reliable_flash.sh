#!/usr/bin/env bash
# reliable_flash.sh -- app-only flash for the ESP32-S3 Clip-Boy over native USB-CDC.
#
# The stub loader (arduino-cli's default + `esptool write_flash` without --no-stub)
# WEDGES mid-write on the big ~6.7MB app over native USB-CDC ("The chip stopped
# responding" at a non-deterministic %). The ROM loader (--no-stub) does NOT wedge
# -- it flashed the full app first try, hash-verified. So use it. App-only: the
# bootloader/partitions are unchanged across --test builds (same partition scheme),
# so we only need to (re)write 0x10000. Run build.sh (compile) first, then this.
#
# Usage: bash scripts/reliable_flash.sh [PORT] [BIN]
#   PORT defaults to COM11. BIN defaults to the newest compiled <sketch>.ino.bin.
set -euo pipefail
PROJ_SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # the Clip-Boy/ sketch dir
PORT="${1:-COM11}"
ESPTOOL="$LOCALAPPDATA/Arduino15/packages/esp32/tools/esptool_py/4.5.1/esptool.exe"
BIN="${2:-}"
if [[ -z "$BIN" ]]; then
  BIN=$(find "$LOCALAPPDATA/arduino/sketches" -name "*.ino.bin" -printf "%T@ %p\n" \
        | sort -rn | head -1 | cut -d' ' -f2-)
fi
[[ -f "$BIN" ]] || { echo "no app bin found ($BIN)"; exit 1; }

# STALENESS GATE. This script picks the NEWEST .ino.bin, which after a FAILED compile is the
# previous successful build -- so it happily flashed a stale image and reported "[flash] done".
# The badge then runs code that does not match the tree, and the next test result is about
# firmware nobody wrote. Bit us 2026-07-25: a compile error left the prior binary on the badge.
# Refuse if any first-party source is newer than the binary we are about to write.
#
# ⚠ The criterion is "every source this build compiles", and the original flat glob was
# NARROWER THAN THAT: `"$PROJ_SRC"/*.h` is non-recursive, so it was BLIND to libs/, where
# the vendored ClipBoy sources live. That is not a hypothetical gap -- the 2026-07-27/28
# session edited WiFiScan.cpp and EvilPortal.cpp repeatedly, and a failed compile in
# either would have flashed the previous binary and printed "[flash] done", making every
# hardware reading that night about firmware nobody wrote. A gate narrower than its class
# reads as full coverage forever. Use find; do not go back to a glob.
#
# Scope note: libs/ is INCLUDED deliberately (we patch vendored sources in-tree, and
# build.sh compiles them with --libraries libs). Non-source trees are pruned so an
# unrelated artifact cannot block a flash.
STALE=""
while IFS= read -r f; do
  [[ -f "$f" ]] || continue
  [[ "$f" -nt "$BIN" ]] && STALE="$STALE
  ${f#$PROJ_SRC/}"
done < <(find "$PROJ_SRC" \
           \( -name .git -o -name build -o -name release -o -name docs \
              -o -name test-reports -o -name '*.d' \) -prune -o \
           -type f \( -name '*.h' -o -name '*.hpp' -o -name '*.ino' \
                      -o -name '*.c' -o -name '*.cpp' \) -print)
if [[ -n "$STALE" ]]; then
  echo "[flash] REFUSING: these sources are NEWER than $BIN --$STALE"
  echo "[flash] The last compile probably FAILED. Re-run scripts/build.sh and check for errors."
  echo "[flash] (Override with: FLASH_ALLOW_STALE=1 bash scripts/reliable_flash.sh ...)"
  [[ "${FLASH_ALLOW_STALE:-0}" == "1" ]] || exit 2
  echo "[flash] FLASH_ALLOW_STALE=1 set -- proceeding with a KNOWN-STALE image."
fi

echo "[flash] $BIN"
echo "[flash] -> $PORT via ROM loader (--no-stub)"
"$ESPTOOL" --chip esp32s3 --port "$PORT" --baud 460800 --before default_reset --after hard_reset --no-stub \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB 0x10000 "$BIN"

# POST-FLASH SETTLE. esptool toggles DTR/RTS as it closes; a serial session opened immediately
# afterwards is a "rapid reconnect that toggles DTR", which is the documented way to WEDGE the
# ESP32-S3 native USB-CDC ("Write timeout" on every command -- not a crash, but it needs an
# esptool flash_id round-trip to clear). Bit us twice on 2026-07-25 by flashing and then running
# the harness in the same command line; earlier runs only survived because something slow
# happened to sit in between. The badge also needs this long to finish booting + POST before it
# answers harness commands at all, so the wait is not wasted.
SETTLE="${FLASH_SETTLE_SEC:-10}"
echo "[flash] settling ${SETTLE}s before any serial session (native-CDC reconnect guard)"
sleep "$SETTLE"
echo "[flash] done"
