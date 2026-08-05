#!/bin/bash
# build.sh — Compile and optionally upload Clip-Boy firmware
#
# Usage:
#   ./scripts/build.sh                    # Compile production build
#   ./scripts/build.sh --test             # Compile with TEST_HARNESS
#   ./scripts/build.sh --test --upload    # Compile + upload with TEST_HARNESS
#   ./scripts/build.sh --upload           # Compile + upload production
#   ./scripts/build.sh --rift             # Quantum-rift boot variant
#   ./scripts/build.sh --res34rch             # Res34rch-Boy: include ACTIVE RESEARCH tools
#                                         # (default build = Sn34k-Boy, listen-only)
#   ./scripts/build.sh --postcon          # Post-con: reveal the P5 unlock code on
#                                         # the keypad (winnable without the IVR)
#   ./scripts/build.sh --build-path DIR   # Emit build artifacts (.elf/.bin) to DIR
#   ./scripts/build.sh --port COM5        # Specify upload port

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ACLI="$PROJECT_DIR/tools/arduino-cli.exe"
FQBN="esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PSRAM=opi,PartitionScheme=default"

# ── PINNED ESP32 core version (REQUIRED for reproducibility) ─────────────────────
# The signed releases are built with esp32:esp32@2.0.10 (also what ESP32Marauder
# targets). A different core = a different, non-reproducible binary. The FQBN above
# is unversioned (arduino-cli has no per-invocation version pin), so we ENFORCE the
# version here: the required core must be installed, and it must be the ONLY esp32
# core present (else the unversioned FQBN could silently pick a different one -- which
# is exactly how a parallel build once bumped this machine 2.0.10 -> 2.0.17 and broke
# both the link and byte-reproducibility). Install with:
#   arduino-cli core install esp32:esp32@2.0.10
REQUIRED_CORE="2.0.10"
ESP32_HW_DIR="${LOCALAPPDATA:-$HOME/AppData/Local}/Arduino15/packages/esp32/hardware/esp32"
ESP32_HW_DIR="$(cygpath -u "$ESP32_HW_DIR" 2>/dev/null || echo "$ESP32_HW_DIR")"
if [[ ! -d "$ESP32_HW_DIR/$REQUIRED_CORE" ]]; then
    echo "[BUILD] ERROR: required ESP32 core $REQUIRED_CORE not installed." >&2
    echo "[BUILD]   run: arduino-cli core install esp32:esp32@$REQUIRED_CORE" >&2
    exit 1
fi
_other_cores="$(ls "$ESP32_HW_DIR" 2>/dev/null | grep -v "^$REQUIRED_CORE\$" || true)"
if [[ -n "$_other_cores" ]]; then
    echo "[BUILD] ERROR: other esp32 core(s) present besides $REQUIRED_CORE: $_other_cores" >&2
    echo "[BUILD]   the unversioned FQBN may pick the wrong one -> non-reproducible build." >&2
    echo "[BUILD]   remove them: arduino-cli core install esp32:esp32@$REQUIRED_CORE (replaces)" >&2
    exit 1
fi
# Auto-detect the single .ino in the project root (robust to a sketch rename;
# arduino-cli requires the sketch folder basename == the .ino basename).
SKETCH="$(ls "$PROJECT_DIR"/*.ino 2>/dev/null | head -1)"
[[ -z "$SKETCH" ]] && { echo "[BUILD] no .ino found in $PROJECT_DIR" >&2; exit 1; }
SKETCH_NAME="$(basename "$SKETCH" .ino)"   # e.g. "Clip-Boy" -> bins are $SKETCH_NAME.ino.*

TEST_HARNESS=0
RIFT=0
RES34RCH=0
POSTCON=0
UPLOAD=0
PORT=""
BUILD_PATH=""
RADIO_PCM_TEST=0
# Split-phase build (for the parallel release build, DC34): the shared generated
# artifacts (build_stamp.h, collectibles_csv.h, radio_audio_gen.*, clipboy-littlefs.bin)
# are SKU-INDEPENDENT and all write into PROJECT_DIR, so N concurrent full builds would
# race on them. make_release_bins.sh runs `--gen-only` ONCE, then fans out N
# `--compile-only` variant compiles (each to its own --build-path) in parallel.
#   --gen-only     : produce the shared artifacts + run the pre-build checks, then STOP.
#   --compile-only : skip the gen (reuse the artifacts from a prior --gen-only), just compile.
GEN_ONLY=0
COMPILE_ONLY=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --test)  TEST_HARNESS=1; shift ;;
        --rift)  RIFT=1; shift ;;
        --res34rch)  RES34RCH=1; shift ;;
        --postcon)  POSTCON=1; shift ;;
        --radio-pcm-test) RADIO_PCM_TEST=1; shift ;;
        --upload) UPLOAD=1; shift ;;
        --port)  PORT="$2"; shift 2 ;;
        --build-path) BUILD_PATH="$2"; shift 2 ;;
        --gen-only)     GEN_ONLY=1; shift ;;
        --compile-only) COMPILE_ONLY=1; shift ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# --- Claude Code status-line marker ----------------------------------------------
# Signal "build/flash in progress" for the WHOLE run so the status line stays lit the
# entire time (its process-scan alone flickers between arduino-cli phases). Best-effort +
# self-cleaning via the EXIT trap; the status line also ignores it if it goes stale.
CB_BUILD_MARKER="$HOME/.claude/state/clipboy-build"
_cbv="sn34k"; [[ $RES34RCH -eq 1 ]] && _cbv="res34rch"
[[ $TEST_HARNESS -eq 1 ]] && _cbv="${_cbv}+test"
[[ $RIFT -eq 1 ]] && _cbv="${_cbv}+rift"
[[ $POSTCON -eq 1 ]] && _cbv="${_cbv}+postcon"
_cba="build"; [[ $UPLOAD -eq 1 ]] && _cba="build+flash"
mkdir -p "$(dirname "$CB_BUILD_MARKER")" 2>/dev/null
echo "${_cba} ${_cbv}" > "$CB_BUILD_MARKER" 2>/dev/null

# 2-tone completion beep on exit -- rising = success, low descending = failure -- so a
# background build/flash is audible without watching. Best-effort (needs powershell + a
# beeper); silence with CLIPBOY_NO_BEEP=1.
_cb_done() {
    local rc=$?
    rm -f "$CB_BUILD_MARKER" 2>/dev/null
    if [ "${CLIPBOY_NO_BEEP:-0}" != "1" ]; then
        if [ "$rc" -eq 0 ]; then
            powershell -NoProfile -c "[console]::beep(988,140);[console]::beep(1319,220)" >/dev/null 2>&1
        else
            powershell -NoProfile -c "[console]::beep(440,180);[console]::beep(330,320)" >/dev/null 2>&1
        fi
    fi
}
trap _cb_done EXIT
# ---------------------------------------------------------------------------------

EXTRA_FLAGS=()
EXTRA_FLAGS+=(--build-property "build.partitions=partitions")
# Must match app0 size in partitions.csv (9MB = 0x900000) -- the FQBN's
# PartitionScheme menu otherwise caps the size check at its own default (4MB).
EXTRA_FLAGS+=(--build-property "upload.maximum_size=9437184")

# Accumulate -D defines across variants so flags compose (e.g. --test --rift).
# Applied to BOTH C and C++ so the gated .c assets (rift_*.c) see the define.
DEFS=""
if [[ $TEST_HARNESS -eq 1 ]]; then
    DEFS+=" -DTEST_HARNESS -DCOLL_DEBUG -DCLIPBOY_DEBUG"
    echo "[BUILD] Mode: TEST_HARNESS + COLL_DEBUG + CLIPBOY_DEBUG"
fi
if [[ $RIFT -eq 1 ]]; then
    DEFS+=" -DBADGE_QUANTUM_RIFT"
    echo "[BUILD] Variant: QUANTUM RIFT boot"
fi
if [[ $RES34RCH -eq 1 ]]; then
    DEFS+=" -DCLIPBOY_RES34RCH"
    echo "[BUILD] SKU: Res34rch-Boy (ACTIVE RESEARCH tools included)"
else
    echo "[BUILD] SKU: Sn34k-Boy (listen-only; active tools compile-excluded)"
fi
if [[ $POSTCON -eq 1 ]]; then
    DEFS+=" -DCLIPBOY_POSTCON"
    echo "[BUILD] Variant: POST-CON (P5 unlock code revealed on keypad; for AFTER DEFCON 34)"
fi
if [[ $RADIO_PCM_TEST -eq 1 ]]; then
    DEFS+=" -DRADIO_PCM_TEST"
    echo "[BUILD] Spike: RADIO_PCM_TEST (double-tap CLIP-BOY 3000 plays radio_test_pcm.c)"
fi
# ARG finale secret (DC34). The published source trees carry the all-zero
# placeholder on purpose; the SHIPPED bins must hold the REAL key or the P5 phone
# unlock can never validate (badge checks zeros, live IVR uses the real secret --
# this shipped broken once). Injected at COMPILE time so it never lands in the
# staged/published tree. Set CLIPBOY_ARG_SECRET_HEX to the 64-hex key.
# NEVER echo the value.
if [[ -n "${CLIPBOY_ARG_SECRET_HEX:-}" ]]; then
    if [[ ! "$CLIPBOY_ARG_SECRET_HEX" =~ ^[0-9a-fA-F]{64}$ ]]; then
        echo "[BUILD] ERROR: CLIPBOY_ARG_SECRET_HEX must be exactly 64 hex chars" >&2
        exit 1
    fi
    DEFS+=" -DARG_HMAC_SECRET_HEX_RAW=$CLIPBOY_ARG_SECRET_HEX"
    echo "[BUILD] ARG secret: REAL key injected at compile time"
else
    echo "[BUILD] ARG secret: not injected (tree default: real secret.h if present, else all-zero placeholder)"
fi

if [[ -n "$BUILD_PATH" ]]; then
    EXTRA_FLAGS+=(--build-path "$BUILD_PATH")
fi
[[ -z "$DEFS" ]] && echo "[BUILD] Mode: PRODUCTION"

# Reproducibility (DC34-122): strip the absolute build/source path so the ELF
# -- and the app_elf_sha256 the SDK bakes into the image -- is identical across
# machines (without this, a different PROJECT_DIR yields a different image even
# from the same commit). Use the Windows path form the toolchain actually
# embeds (cygpath -m -> C:/...); the MSYS /c/... form both fails to match and
# gets mangled by Git Bash arg-conversion. Applied to EVERY build.
WINPROJ="$(cygpath -m "$PROJECT_DIR" 2>/dev/null || echo "$PROJECT_DIR")"
REPRO_FLAGS="-ffile-prefix-map=${WINPROJ}=."
ALL_DEFS="$REPRO_FLAGS"
[[ -n "$DEFS" ]] && ALL_DEFS="$REPRO_FLAGS ${DEFS# }"

# Ad-hoc extra defines, for test + FAULT-INJECTION builds only. Example: rebuild the SB3
# defect so its regression test can be proven to go RED before the fix is trusted:
#   CB_EXTRA_DEFS='-DCB_THEREMIN_DUCK_CLICK=0 -DCB_SB3_FAULT_INJECT' bash scripts/build.sh --test
# Deliberately loud: a silently-injected define would make a broken build look like a normal
# one, and release_from_staging.sh never sets this.
if [[ -n "${CB_EXTRA_DEFS:-}" ]]; then
    ALL_DEFS="$ALL_DEFS ${CB_EXTRA_DEFS}"
    echo "[BUILD] *** CB_EXTRA_DEFS ACTIVE: ${CB_EXTRA_DEFS} -- NOT a shippable build ***"
fi
EXTRA_FLAGS+=(--build-property "compiler.cpp.extra_flags=$ALL_DEFS")
EXTRA_FLAGS+=(--build-property "compiler.c.extra_flags=$ALL_DEFS")

# Linker flags the ClipBoy (vendored Marauder) integration REQUIRES. These were
# historically hand-patched into the shared ESP32 core's platform.txt, which is
# fragile: an arduino-core reinstall or a *parallel* build of another sketch
# resets platform.txt and silently drops them, breaking the link
# (undefined `__real_esp_wifi_*` / "multiple definition" of ClipBoy headers). A
# fresh public clone (GPLv3 source conveyance) has no patched core either. Inject
# them here via the recipe's `{compiler.c.elf.extra_flags}` slot so the build is
# SELF-CONTAINED and survives a stock core:
#  - --wrap=esp_wifi_*: ClipBoy's RunSetup shims the low-level esp_wifi_* calls
#    (see ClipBoyMarauder.cpp / clipboy_wifi_bug fix + active_tx_apsta_fix). Each
#    wrapped symbol needs its own --wrap so calls route to __wrap_ and __real_ is
#    provided. The on-demand APSTA switch bypasses the shim via __real_esp_wifi_set_mode.
#  - --allow-multiple-definition: ui_test.ino pulls ClipBoy headers (utils.h/
#    lang_var.h) that define real (non-inline) symbols, which also land in
#    ClipBoy's own .cpp objects. Identical defs from the same header -> picking the
#    first is safe; this is inherent to the single-sketch vendoring.
CB_WRAPS="esp_wifi_init esp_wifi_set_country esp_wifi_set_mode esp_wifi_set_ps"
CB_WRAPS+=" esp_wifi_set_storage esp_wifi_start esp_wifi_deinit esp_wifi_restore"
CB_WRAPS+=" esp_wifi_stop esp_netif_deinit"
CB_ELF_FLAGS="-Wl,--allow-multiple-definition"
for _w in $CB_WRAPS; do CB_ELF_FLAGS+=" -Wl,--wrap=$_w"; done
EXTRA_FLAGS+=(--build-property "compiler.c.elf.extra_flags=$CB_ELF_FLAGS")

# ─── Shared-gen phase (SKU-independent; skipped by --compile-only) ───────────────
# Everything below writes into PROJECT_DIR (build_stamp.h, collectibles_csv.h,
# radio_audio_gen.*, clipboy-littlefs.bin) + runs the pre-build checks. It is
# byte-identical across the 4 variants, so the parallel release build runs it ONCE
# (via --gen-only) and the per-variant --compile-only builds reuse the results.
if [[ $COMPILE_ONLY -eq 0 ]]; then

# Deterministic build stamp (DC34-122): write build_stamp.h with a stamp
# derived from the commit (date + short sha, + "-dirty" for an uncommitted
# tree) instead of the wall-clock __DATE__/__TIME__. This makes a clean-tree
# build byte-reproducible from its commit -- the keyless backstop to the
# minisign signature (see SECURITY.md). The header is git-ignored; ui_nav.h
# falls back to __DATE__/__TIME__ when it's absent (ad-hoc IDE builds).
#
# Stamp source precedence (DC34, GPLv3 source-conveyance): a .git-less public
# source snapshot has no commit to derive from, so allow an EXPLICIT pin:
#   1. env CB_BUILD_STAMP  -- one-off override (CI / scripted release)
#   2. a VERSION file at the repo root  -- how stage_source.sh pins the snapshot
#      so a source build reproduces the signed release byte-for-byte
#   3. the git-derived stamp (unchanged) -- the normal dev/release path
# A normal ui_test checkout has neither the env nor a VERSION file, so this is a
# no-op for dev builds (same git-derived stamp as before).
if [[ -n "$CB_BUILD_STAMP" ]]; then
    STAMP="$CB_BUILD_STAMP"
elif [[ -f "$PROJECT_DIR/VERSION" ]]; then
    STAMP="$(tr -d ' \t\r\n' < "$PROJECT_DIR/VERSION")"
else
    BUILD_DATE="$(git -C "$PROJECT_DIR" show -s --format=%cd --date=format:%Y-%m-%d HEAD 2>/dev/null)"
    BUILD_SHA="$(git -C "$PROJECT_DIR" rev-parse --short HEAD 2>/dev/null)"
    if [[ -n "$BUILD_DATE" && -n "$BUILD_SHA" ]]; then
        STAMP="${BUILD_DATE}+${BUILD_SHA}"
        git -C "$PROJECT_DIR" diff --quiet HEAD -- . 2>/dev/null || STAMP="${STAMP}-dirty"
    else
        STAMP="unknown"
    fi
fi
printf '#define CB_BUILD_STAMP "%s"\n' "$STAMP" > "$PROJECT_DIR/build_stamp.h"
echo "[BUILD] stamp: $STAMP"

# ARG secret for a clean/public source clone. The real secret.h is private +
# gitignored; arg_unlock.h falls back to #include "secret.h.example". But
# arduino-cli only stages recognized code extensions (.h/.c/.cpp/.ino/...) into
# its build sketch dir -- a ".example" file is NOT copied, so that include would
# fail at compile. Materialize a real secret.h (a .h, which DOES stage) from the
# tracked placeholder when none is present, so a public tree compiles out of the
# box. The placeholder is an all-zero key: the badge builds + runs, only the
# phone-based ARG finale won't match the live IVR. A real (owner's) secret.h is
# never touched. secret.h stays gitignored, so this never lands in the tree.
if [[ ! -f "$PROJECT_DIR/secret.h" && -f "$PROJECT_DIR/secret.h.example" ]]; then
    cp "$PROJECT_DIR/secret.h.example" "$PROJECT_DIR/secret.h"
    echo "[BUILD] no secret.h -> materialized placeholder from secret.h.example (ARG phone finale won't match live system)"
fi

# Regenerate the collectibles PROGMEM header from the canonical CSV. data/
# collectibles.csv is the SINGLE SOURCE OF TRUTH; collectibles_csv.h is a derived
# build artifact (git-ignored), regenerated every build so the shipped data can
# never be stale or hand-divergent. REQUIRED (the sketch includes the header).
py -3 "$PROJECT_DIR/scripts/embed_csv.py" \
    || { echo "[BUILD] embed_csv FAILED (need py-3 + data/collectibles.csv)"; exit 1; }

# Collectible ART staleness gate (WARN only). coll_images.c is NOT regenerated
# by the build (it's a ~23MB derived file), so freshly-edited images/hires_src
# art would otherwise ship stale. If any hires_src png is newer than coll_images.c,
# warn and point at the one-command refresh. (embed_csv above keeps the catalog
# TEXT fresh every build; this covers the ART.)
if [ -f "$PROJECT_DIR/coll_images.c" ]; then
    stale_art=$(find "$PROJECT_DIR/images/hires_src" -maxdepth 1 -name '*.png' \
                    -newer "$PROJECT_DIR/coll_images.c" 2>/dev/null | wc -l)
    if [ "$stale_art" -gt 0 ]; then
        echo "[BUILD] WARN: $stale_art hires_src image(s) newer than coll_images.c -> collectible ART is STALE."
        echo "[BUILD]       run  scripts/publish_collectibles.sh  to refresh firmware A8 + web RGB before shipping."
    fi
fi

# Hosted Help page staleness gate (WARN only). The public SafeHazard help/index.html
# is generated from the firmware Help corpus (ui_nav.h + tool_info.h) by the
# deterministic gen_help_site.py. If the corpus changed since the page was deployed,
# warn + point at the one-command refresh. Skips silently if the SafeHazard clone
# isn't present (fresh clone / CI).
bash "$PROJECT_DIR/scripts/publish_help.sh" --check 2>/dev/null || true

# Post-con companion page staleness gate (WARN only; PRIVATE artifact). postcon/index.html
# is generated from data/collectibles.csv + the HR encoder by the deterministic
# gen_postcon_site.py. If the catalog / encoder changed since it was generated, warn.
# (No auto-publish: it stays private in this repo; the SafeHazard push is the post-con
# manual step with the real ARG key.)
if [ -f "$PROJECT_DIR/postcon/index.html" ]; then
    _pc_tmp="$(mktemp)"
    if py -3 "$PROJECT_DIR/scripts/gen_postcon_site.py" "$_pc_tmp" >/dev/null 2>&1 \
       && ! diff -q "$PROJECT_DIR/postcon/index.html" "$_pc_tmp" >/dev/null 2>&1; then
        echo "[BUILD] WARN: postcon/index.html is STALE vs data/collectibles.csv + the HR encoder."
        echo "[BUILD]       run  py -3 scripts/gen_postcon_site.py  to refresh (private; SafeHazard publish is post-con)."
    fi
    rm -f "$_pc_tmp"
fi

# Radio audio: embed every audio/*.mp3 as PROGMEM + its ID3 caption into
# radio_audio_gen.cpp (git-ignored, regenerated each build). Budget-checks the
# total against the app0 audio headroom and FAILS the build if it won't fit.
# Budget = how much audio stays PROGMEM. Kept intentionally SMALL (~290KB) so ONLY
# the pinned ARG breadcrumb (1-1, ~268KB) rides PROGMEM and ALL other radio/Summon
# clips route to littlefs -- this shrinks the app image ~900KB+ (faster/more-reliable
# flash + app0 headroom). littlefs holds ~5.4MB of ~6.75MB. (Ceiling is still the
# ESP32-S3 DROM rodata window, but we're well under it now.) See docs/radio-audio-pipeline.md.
LFS_STAGE="$PROJECT_DIR/build_littlefs"
py -3 "$PROJECT_DIR/scripts/embed_audio.py" "$PROJECT_DIR/audio" "$PROJECT_DIR/radio_audio_gen" 290000 "$LFS_STAGE" \
    || { echo "[BUILD] embed_audio FAILED (bad mp3 / no audio dir)"; exit 1; }

# Stage the Evil Portal EXAMPLE templates into littlefs for ALL SKUs. They are
# harmless demos (no capture form) -- inert text on their own. Res34rch reads them
# as the built-in Evil Portal fallback when no SD card is present (SD stays primary);
# Sn34k has the Evil Portal tool compile-excluded so it never reads them, they just
# ride along. Staging UNCONDITIONALLY keeps the shared release littlefs byte-identical
# regardless of which variant built last (was gated on RES34RCH -> build-order-fragile).
if [[ -d "$PROJECT_DIR/assets/littlefs_res34rch" ]]; then
    cp -r "$PROJECT_DIR/assets/littlefs_res34rch/." "$LFS_STAGE/" \
        && echo "[BUILD] staged littlefs examples (Evil Portal templates, all SKUs)"
fi
# Build the littlefs image (big radio beds) with mklittlefs, sized to the littlefs
# partition (0x670000). Flashed separately to 0x980000 (arduino-cli upload writes
# only the app). If mklittlefs is absent the app still builds -- the player skips
# missing littlefs clips gracefully (#noReboots).
MKLFS=$(ls "$HOME/AppData/Local/Arduino15/packages/esp32/tools/mklittlefs/"*/mklittlefs.exe 2>/dev/null | head -1)
if [[ -n "$MKLFS" && -d "$LFS_STAGE" ]]; then
    "$MKLFS" -c "$LFS_STAGE" -s 6750208 -p 256 -b 4096 "$PROJECT_DIR/clipboy-littlefs.bin" >/dev/null \
        && echo "[BUILD] littlefs image -> clipboy-littlefs.bin ($(stat -c%s "$PROJECT_DIR/clipboy-littlefs.bin" 2>/dev/null) B) flash @ 0x980000" \
        || echo "[BUILD] WARN: mklittlefs failed (big radio beds won't ship)"
else
    echo "[BUILD] (mklittlefs not found -- skipping littlefs image; big beds absent)"
fi

# Pre-build Help guard (deterministic; no LLM). Denylist + stale tutorial menu
# paths FAIL the build; a changed Help corpus WARNs that a full /help-review
# (expert panel) is due. Release gating can pass --strict to promote staleness
# to a failure. Skipped if the py launcher isn't present (non-Windows CI).
if command -v py >/dev/null 2>&1; then
    py -3 "$PROJECT_DIR/scripts/help_review/check_help.py" \
        || { echo "[BUILD] Help check FAILED (see above)"; exit 1; }
fi

    # --gen-only: shared artifacts + checks are done -- stop before the compile so the
    # caller can fan out the per-variant --compile-only builds in parallel.
    if [[ $GEN_ONLY -eq 1 ]]; then
        echo "[BUILD] gen-only done (stamp + generated headers + littlefs image)"
        exit 0
    fi
fi   # end shared-gen phase (COMPILE_ONLY guard)

echo "[BUILD] Compiling..."
# Vendored libraries live in ./libs (see scripts/vendor_libs.sh) so a fresh
# clone builds with no external library fetches.
EXTRA_FLAGS+=(--libraries "$PROJECT_DIR/libs")

"$ACLI" compile --fqbn "$FQBN" "${EXTRA_FLAGS[@]}" "$SKETCH"
RC=$?
if [[ $RC -ne 0 ]]; then
    echo "[BUILD] Compile FAILED (exit code $RC)"
    exit $RC
fi
echo "[BUILD] Compile OK"

if [[ $UPLOAD -eq 1 ]]; then
    if [[ -z "$PORT" ]]; then
        # Auto-detect port
        PORT=$(py -3 "$PROJECT_DIR/scripts/test_bridge.py" port 2>/dev/null | py -3 -c "import sys,json; print(json.load(sys.stdin).get('port',''))" 2>/dev/null)
        if [[ -z "$PORT" ]]; then
            echo "[BUILD] No ESP32 port detected. Use --port COMx"
            exit 1
        fi
    fi
    echo "[BUILD] Uploading to $PORT..."
    "$ACLI" upload --fqbn "$FQBN" -p "$PORT" "$SKETCH"
    RC=$?
    if [[ $RC -ne 0 ]]; then
        echo "[BUILD] Upload FAILED (exit code $RC)"
        exit $RC
    fi
    echo "[BUILD] Upload OK"
fi
