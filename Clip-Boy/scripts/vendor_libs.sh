#!/bin/bash
# vendor_libs.sh -- copy the third-party Arduino libraries Clip-Boy depends on
# into ./libs (trimmed) so a fresh clone builds without fetching anything.
#
# Each library is copied, then trimmed to what actually compiles: its src/
# directory, root source/headers, library.properties, and its license/notice.
# examples/docs/tests/demos/vectors/.git etc. are dropped (often 90%+ of size).
#
# Core libraries (WiFi, FS, SD, SPI, LittleFS, Preferences, Wire, ...) are part
# of the ESP32 board package and are NOT vendored. The ESP32 core + arduino-cli
# still need to be installed -- see README.
#
# Usage:  bash scripts/vendor_libs.sh [SOURCE_LIBRARIES_DIR]
#   SOURCE_LIBRARIES_DIR defaults to ../libraries

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC="${1:-$PROJECT_DIR/../libraries}"
DEST="$PROJECT_DIR/libs"

# Third-party libraries the sketch actually links (from arduino-cli's
# "Used library" report). Core/platform libs are excluded.
LIBS=(
  LovyanInit-Waveshare
  lvgl
  LovyanGFX
  ClipBoy
  ClipBoyTheremin
  HRCode4x4
  HRScanGuidance
  ArduinoJson
  NimBLE-Arduino
  ESP32Ping
  ESPAsyncWebServer
  AsyncTCP
  Adafruit_NeoPixel
  SparkFun_VL53L5CX_Arduino_Library
  LinkedList
  audio-tools
  minimp3
  lz4-1.10.0
)

trim() {  # keep src/ + root sources + manifest + license; drop everything else
  local d="$1"
  find "$d" -mindepth 1 -maxdepth 1 -print0 | while IFS= read -r -d '' item; do
    base="$(basename "$item")"
    if [ -d "$item" ]; then
      [ "$base" = "src" ] && continue
      rm -rf "$item"
    else
      case "$base" in
        library.properties|library.json|keywords.txt) ;;
        LICENSE*|License*|license*|COPYING*|COPYRIGHT*|NOTICE*|*.txt) ;;
        *.h|*.hpp|*.c|*.cpp|*.cc|*.cxx) ;;
        *) rm -f "$item" ;;
      esac
    fi
  done
}

echo "Vendoring from: $SRC"
echo "Into:           $DEST"

# ── CLOBBER GUARD (added 2026-07-28) ────────────────────────────────────────
# This script's FIRST destructive act is `rm -rf "$DEST"` -- it wipes ALL of libs/
# before copying anything, so a per-library check further down would be far too late.
#
# Why it matters: libs/ is no longer a pristine mirror of $SRC. Clip-Boy carries dozens
# of local patches in the VENDORED tree only (heap-leak + UAF fixes in EvilPortal, the
# SSID over-read clamps, the FB9/FB10 caps, the probe-SSID cap, the deauth channel
# selector...). Meanwhile ../libraries/ClipBoy is stale -- it does not even contain
# setRawChannel. So re-vendoring would silently roll the firmware back to upstream and
# every subsequent test would describe code nobody wrote.
#
# Fail CLOSED and name what would be lost, rather than warn and proceed: an operator who
# skims a warning still loses the patches, and the loss is invisible in a diff of $SRC.
PATCHED=$(grep -rl "Clip-Boy local patch" "$DEST" 2>/dev/null | wc -l | tr -d ' ')
if [ "${PATCHED:-0}" -gt 0 ] && [ "${VENDOR_ALLOW_CLOBBER:-0}" != "1" ]; then
  echo
  echo "  !! REFUSING: $DEST holds $PATCHED file(s) carrying 'Clip-Boy local patch'."
  echo "     This script does 'rm -rf $DEST' first, so those local fixes would be LOST."
  echo "     $SRC is the UPSTREAM working copy and is known to be STALE."
  echo
  grep -rl "Clip-Boy local patch" "$DEST" 2>/dev/null | sed "s|^|       |"
  echo
  echo "     If you genuinely mean to discard them:  VENDOR_ALLOW_CLOBBER=1 $0"
  echo "     To re-vendor safely, reconcile $SRC with $DEST FIRST."
  exit 2
fi
[ "${VENDOR_ALLOW_CLOBBER:-0}" = "1" ] && [ "${PATCHED:-0}" -gt 0 ] && \
  echo "  ** VENDOR_ALLOW_CLOBBER=1 -- discarding $PATCHED locally-patched file(s)."

rm -rf "$DEST"
mkdir -p "$DEST"

for lib in "${LIBS[@]}"; do
  if [ ! -d "$SRC/$lib" ]; then
    echo "  !! MISSING: $lib  (not in $SRC)"; continue
  fi
  cp -r "$SRC/$lib" "$DEST/$lib"
  rm -rf "$DEST/$lib/.git"
  trim "$DEST/$lib"
  # LovyanGFX bundles ~117MB of embedded CJK glyph data (efont/IPA) we never use
  # (Clip-Boy renders text via LVGL). lgfx_fonts.cpp still #includes the headers,
  # so keep the tiny .h declarations and drop only the huge .c data -- the unused
  # font objects are dropped by the linker.
  if [ "$lib" = "LovyanGFX" ]; then
    rm -f "$DEST/$lib"/src/lgfx/Fonts/efont/*.c "$DEST/$lib"/src/lgfx/Fonts/IPA/*.c
  fi
  sz=$(du -sm "$DEST/$lib" 2>/dev/null | cut -f1)
  echo "  vendored $lib  (${sz} MB)"
done

# LVGL reads lv_conf.h from the folder ABOVE the lvgl/ dir (relative include
# ../../lv_conf.h). Place our configured copy at libs/lv_conf.h.
if [ -f "$SRC/lv_conf.h" ]; then
  cp "$SRC/lv_conf.h" "$DEST/lv_conf.h"
  echo "  vendored lv_conf.h"
else
  echo "  !! MISSING: lv_conf.h"
fi

echo "Total vendored size: $(du -sm "$DEST" | cut -f1) MB"
