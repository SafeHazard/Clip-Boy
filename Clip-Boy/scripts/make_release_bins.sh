#!/bin/bash
# make_release_bins.sh - Build all SKU x boot variants and emit .bin images.
#
# 8-variant matrix: {sn34k, res34rch} x {stock, rift} x {full, app}.
#   sn34k     = Sn34k-Boy (default, listen-only)
#   res34rch  = Res34rch-Boy (active research)
#   *-rift    = Quantum-Rift boot screen (bakes in LOCAL rift art - see note)
# Per variant:
#   release/clipboy-<variant>-full.bin  merged bootloader+partitions+boot_app0+
#                                       app @ 0x0 - fresh/factory flash.
#   release/clipboy-<variant>-app.bin   app only @ 0x10000 - NVS-preserving
#                                       update (keeps collectibles + settings).
# Shared boot chain (identical across ALL variants - only `app` carries the SKU):
#   release/clipboy-bootloader.bin   @ 0x0      release/clipboy-boot_app0.bin @ 0xe000
#   release/clipboy-partitions.bin   @ 0x8000   (app @ 0x10000 is per-variant)
# These let the web flasher flash the boot chain + per-variant app directly,
# without slicing the merged full.bin. A `cmp` guard below FAILS the build if a
# variant's bootloader/partitions ever diverge (would break the shared-set assumption).
#
# Usage:
#   bash scripts/make_release_bins.sh                       # all 8
#   bash scripts/make_release_bins.sh sn34k res34rch-rift   # specific variants

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
# Sketch basename (arduino names its outputs "<SK>.ino.*"); robust to a rename.
SK="$(basename "$(ls "$PROJECT_DIR"/*.ino 2>/dev/null | head -1)" .ino)"
OUT="$PROJECT_DIR/release"
mkdir -p "$OUT"

# Toolchain (esptool 4.5.1) + boot_app0 from the ESP32 core (not vendored).
A15="$HOME/AppData/Local/Arduino15/packages/esp32"
ESPTOOL=$(ls "$A15/tools/esptool_py/"*/esptool.exe 2>/dev/null | head -1)
BOOT_APP0="$A15/hardware/esp32/2.0.10/tools/partitions/boot_app0.bin"
# Flash params for the Waveshare ESP32-S3 (from boards.txt: esp32s3.build.*)
CHIP=esp32s3
FLASH_MODE=dio
FLASH_FREQ=80m
FLASH_SIZE=16MB

if [[ -z "$ESPTOOL" || ! -f "$ESPTOOL" ]]; then echo "esptool not found"; exit 1; fi
if [[ ! -f "$BOOT_APP0" ]]; then echo "boot_app0.bin not found: $BOOT_APP0"; exit 1; fi

SKUS=("$@")
[[ ${#SKUS[@]} -eq 0 ]] && SKUS=(sn34k res34rch sn34k-rift res34rch-rift)

# ── Security gate (source-level): fail BEFORE building if any audited fix
# regressed (security_audit.md) or weaponization terminology slipped in. The
# binary SKU + release-hygiene gate runs after the builds below. Keeps bad
# firmware out of release/.
# check_source_drop.py guards the PUBLISHED SOURCE tree rather than the bins: stage_source.sh
# archives every tracked file and trims a denylist, so anything new and internal (a fresh
# docs/audit report, a re-added devcon.exe, another arg-* writeup) publishes by default
# (audit 2026-07-24 FB3).
# check_deref_guards.py is the ENUMERATION gate for the FB9 class (a torn LinkedList read hands
# back an AccessPoint whose `stations` is nullptr; add() is virtual, so the deref is a vtable load
# from address 0). That class was "finished" three times and each time a sibling turned up
# unvisited -- so the criterion ("every `.stations->` deref") is now machine-checked every
# release instead of depending on someone remembering to re-run a grep.
# verify_redfirst.py is the gate ON the gates: it requires every check in
# check_security_regressions.py to go RED at a commit where its bug was live and GREEN on HEAD.
# It is here because it is the only mechanism that catches a check looking in the WRONG PLACE --
# which has happened twice for real (SB4 was written against EvilPortal.cpp while the credential
# sink lived in WiFiScan.cpp, and W5-SAFE's pattern outlived the code it described). A check that
# cannot see its own bug is indistinguishable from a bug that is absent, and hand-running the
# verifier is exactly the habit that let both survive.
# The two --selftest runs prove those gates can still read BOTH ways. A checker that only ever
# says "clean" reports success forever.
for chk in check_terminology.py check_security_regressions.py verify_sniffer_bounds.py \
           check_source_drop.py check_deref_guards.py verify_redfirst.py \
           "check_deref_guards.py --selftest" "verify_redfirst.py --selftest"; do
    # shellcheck disable=SC2086 -- $chk intentionally carries an optional flag
    if out=$(py -3 "$SCRIPT_DIR/tests/"$chk 2>&1); then
        echo "[GATE] ok: $chk"
    else
        echo "[GATE] FAIL: $chk -- aborting .bin generation"; echo "$out" | tail -10; exit 1
    fi
done

# Help corpus must be re-reviewed for release: --strict promotes "Help changed
# since last /help-review" from WARN to FAIL (a normal build only warns).
if out=$(py -3 "$SCRIPT_DIR/help_review/check_help.py" --strict 2>&1); then
    echo "[GATE] ok: help-review (strict)"
else
    echo "[GATE] FAIL: help corpus changed since last review -- run /help-review"; echo "$out" | tail -6; exit 1
fi

# Collectibles CSV must already be pure ASCII. embed_csv.py self-heals smart
# punctuation on dev builds by rewriting the source -- but a release must NOT
# silently mutate a tracked file mid-build (would flip the version to -dirty and
# break reproducibility). Fail fast instead; the fix is "build once, commit".
# NOTE: $PROJECT_DIR is the MSYS form (/c/Users/...) which Windows Python's
# open() cannot resolve (it reads it as C:\c\Users\... -> FileNotFoundError).
# Convert to a Windows path via cygpath, and do NOT swallow stderr -- a masked
# error here once misreported a path bug as "non-ASCII" and blocked a release.
CSV_ASCII_CHK="$PROJECT_DIR/data/collectibles.csv"
command -v cygpath >/dev/null 2>&1 && CSV_ASCII_CHK="$(cygpath -m "$CSV_ASCII_CHK")"
if ! py -3 -c "open(r'$CSV_ASCII_CHK','r',encoding='ascii').read()"; then
    echo "[GATE] FAIL: data/collectibles.csv not readable as pure ASCII -- if it has non-ASCII bytes, run 'bash scripts/build.sh' to normalize, then commit, then re-release"; exit 1
fi
echo "[GATE] ok: collectibles.csv is ASCII"
echo "[GATE] ok: collectibles CSV is pure ASCII"

# Shared boot chain (same for every variant). boot_app0 is the core's file verbatim;
# bootloader/partitions are emitted from the first build + cmp-guarded against the rest.
SH_BL="$OUT/clipboy-bootloader.bin"
SH_PT="$OUT/clipboy-partitions.bin"
SH_BA0="$OUT/clipboy-boot_app0.bin"
cp "$BOOT_APP0" "$SH_BA0"

flag_for() {
    case "$1" in
        sn34k)         echo "" ;;
        res34rch)      echo "--res34rch" ;;
        sn34k-rift)    echo "--rift" ;;
        res34rch-rift) echo "--res34rch --rift" ;;
        *) echo "unknown variant '$1' (sn34k|res34rch|sn34k-rift|res34rch-rift)" >&2; return 1 ;;
    esac
}

# ── Shared gen ONCE (SKU-independent: build_stamp.h, collectibles_csv.h,
# radio_audio_gen.*, clipboy-littlefs.bin). Doing it here (not per-variant) is what
# lets the compiles run in parallel without racing on these PROJECT_DIR files. ──
echo "==================================================================="
echo "[gen] shared artifacts (stamp + generated headers + littlefs image)..."
bash "$SCRIPT_DIR/build.sh" --gen-only >/dev/null || { echo "[gen] FAILED (re-run 'bash scripts/build.sh --gen-only' to see why)"; exit 1; }

# ── Fan out the 4 variant COMPILES in PARALLEL. Each writes its own --build-path
# (build/<SKU>) and only READS the shared gen artifacts, so there's no cross-build
# write race. On a many-core box this collapses ~4x sequential compiles into ~1
# (each variant is bottlenecked on the single big ui_nav.h translation unit; running
# them concurrently keeps the other cores busy). CLIPBOY_NO_BEEP so we don't get 4 beeps. ──
echo "==================================================================="
# Concurrency cap. Default 0 = all variants at once (fastest on an idle many-core
# box). But 4 concurrent arduino-cli each spawn their own internally-threaded
# compile, and with the repo under OneDrive the build/<SKU> dirs get sync-touched
# mid-write -> arduino-cli occasionally dies with a bare "Error during build: exit
# status 1" (no compiler error), a DIFFERENT variant each run. Set REL_JOBS=2 (or 1)
# to batch the compiles and trade a little wall-clock for a reliable release build.
REL_JOBS="${REL_JOBS:-0}"
echo "[build] compiling ${#SKUS[@]} variants (REL_JOBS=$REL_JOBS; 0=all-at-once)..."
declare -A CPID CRES
launch_one() {   # $1 = SKU
    local SKU="$1" FLAG BP
    FLAG="$(flag_for "$SKU")" || return 1
    BP="$PROJECT_DIR/build/$SKU"
    mkdir -p "$BP"   # the compile.log redirect below needs the dir to exist first
                     # (a fresh checkout has no build/<SKU> yet)
    CLIPBOY_NO_BEEP=1 bash "$SCRIPT_DIR/build.sh" --compile-only $FLAG --build-path "$BP" > "$BP/compile.log" 2>&1 &
    CPID[$SKU]=$!
}
collect_one() {  # $1 = SKU  (blocks on its compile, records result)
    local SKU="$1"
    if wait "${CPID[$SKU]}"; then CRES[$SKU]=0; else CRES[$SKU]=1; fi
}
inflight=()
for SKU in "${SKUS[@]}"; do
    launch_one "$SKU" || exit 1
    inflight+=("$SKU")
    # If a cap is set and we've hit it, drain the current batch before launching more.
    if [[ "$REL_JOBS" -gt 0 && "${#inflight[@]}" -ge "$REL_JOBS" ]]; then
        for S in "${inflight[@]}"; do collect_one "$S"; done
        inflight=()
    fi
done
for S in "${inflight[@]}"; do collect_one "$S"; done   # drain the remainder
CFAIL=0
for SKU in "${SKUS[@]}"; do
    if [[ "${CRES[$SKU]}" -eq 0 ]]; then
        echo "[$SKU] compiled OK"
    else
        echo "[$SKU] COMPILE FAILED -- tail of build/$SKU/compile.log:"; tail -20 "$PROJECT_DIR/build/$SKU/compile.log"; CFAIL=1
    fi
done
[[ $CFAIL -eq 1 ]] && { echo "[build] one or more variant compiles FAILED"; exit 1; }

# ── Sequential post-processing (fast: boot-chain cmp-guard + app copy + merged full). ──
for SKU in "${SKUS[@]}"; do
    BP="$PROJECT_DIR/build/$SKU"
    BL="$BP/$SK.ino.bootloader.bin"
    PT="$BP/$SK.ino.partitions.bin"
    APP="$BP/$SK.ino.bin"
    for f in "$BL" "$PT" "$APP"; do
        [[ -f "$f" ]] || { echo "missing $f"; exit 1; }
    done

    # Shared boot chain: emit on the first variant, verify identical on the rest.
    if [[ ! -f "$SH_BL" ]]; then
        cp "$BL" "$SH_BL"; cp "$PT" "$SH_PT"
        echo "[$SKU] -> shared release/clipboy-{bootloader,partitions,boot_app0}.bin"
    else
        cmp -s "$BL" "$SH_BL" || { echo "ERROR: $SKU bootloader differs from shared set"; exit 1; }
        cmp -s "$PT" "$SH_PT" || { echo "ERROR: $SKU partitions differ from shared set"; exit 1; }
    fi

    # App-only (NVS-preserving update)
    cp "$APP" "$OUT/clipboy-$SKU-app.bin"

    # Merged full image (fresh/factory flash at 0x0)
    "$ESPTOOL" --chip "$CHIP" merge_bin -o "$OUT/clipboy-$SKU-full.bin" \
        --flash_mode "$FLASH_MODE" --flash_freq "$FLASH_FREQ" --flash_size "$FLASH_SIZE" \
        0x0 "$BL" 0x8000 "$PT" 0xe000 "$BOOT_APP0" 0x10000 "$APP" >/dev/null
    echo "[$SKU] -> release/clipboy-$SKU-full.bin  +  release/clipboy-$SKU-app.bin"
done

# ── Shared littlefs CONTENT image (radio beds; byte-identical across variants,
# built by build.sh's mklittlefs step into $PROJECT_DIR/clipboy-littlefs.bin).
# Copy into release/ and GATE it before it can be signed (DC34-139). The gate is
# the BUILD-TIME backstop for content-safety; firmware runtime stays graceful
# (missing clip -> skipped, never a reboot). If no image was built, drop any stale
# copy so the flash-spec omits the littlefs part (web_flash treats that as no-content).
SH_LFS="$OUT/clipboy-littlefs.bin"
LFS_PART_SIZE=6750208   # littlefs partition size (partitions.csv, 0x670000)
if [[ -f "$PROJECT_DIR/clipboy-littlefs.bin" ]]; then
    cp "$PROJECT_DIR/clipboy-littlefs.bin" "$SH_LFS"
    if out=$(py -3 "$SCRIPT_DIR/tests/check_littlefs.py" "$SH_LFS" "$LFS_PART_SIZE" 2>&1); then
        echo "[GATE] ok: littlefs content image -- $out"
    else
        echo "[GATE] FAIL: littlefs content check -- DO NOT publish"; echo "$out" | tail -12; exit 1
    fi
else
    rm -f "$SH_LFS"
    echo "[GATE] NOTE: no clipboy-littlefs.bin built -- flash-spec will omit the littlefs part"
fi

# ── Binary gate: SKU separation (active tools absent from Sn34k) + release
# hygiene (no debug/cheat strings) on the built artifacts. Needs both default
# SKUs built; skipped for partial variant lists.
if [[ -d "$PROJECT_DIR/build/sn34k" && -d "$PROJECT_DIR/build/res34rch" ]]; then
    if out=$(py -3 "$SCRIPT_DIR/tests/check_sku_binaries.py" 2>&1); then
        echo "[GATE] ok: SKU gate + release hygiene verified on built binaries"
    else
        echo "[GATE] FAIL: SKU/hygiene check -- DO NOT publish these bins"; echo "$out" | tail -14; exit 1
    fi
else
    echo "[GATE] NOTE: skipped binary SKU check (need both sn34k & res34rch builds)"
fi

# ── ARG finale key gate: the shipped bins MUST carry the REAL HMAC key, not the
# all-zero placeholder the published source trees ship. This regression ACTUALLY
# SHIPPED (Jul 2026): bins built from the staging trees inherited the placeholder,
# so every badge verified P5 phone codes against zeros while the live IVR used the
# real key -- the ARG finale was dead on every badge and no gate caught it.
if out=$(py -3 "$SCRIPT_DIR/tests/check_arg_secret.py" 2>&1); then
    echo "[GATE] ok: ARG finale key -- real key present in all release images"
else
    echo "[GATE] FAIL: ARG secret check -- P5 phone unlock would be DEAD; DO NOT publish"
    echo "$out" | tail -16; exit 1
fi

# ── IP / trademark gate: no denylisted third-party named marks in the shipped
# user-visible strings (CLAUDE.md "IP/parody posture" -- keep references, scrub
# named marks + verbatim copyright). Scans every built .bin's strings.
if out=$(py -3 "$SCRIPT_DIR/tests/check_ip.py" 2>&1); then
    echo "[GATE] ok: IP/trademark gate -- no denylisted marks in shipped strings"
else
    echo "[GATE] FAIL: IP/trademark check -- DO NOT publish these bins"; echo "$out" | tail -16; exit 1
fi

# ── Firmware build id for the flash-spec (DC34-121): commit date + short sha,
# plus "-dirty" when tracked files differ from HEAD. web_flash stamps this into
# its esp-web-tools manifests' `version` and the on-site build banner. Falls
# back to "unknown" when git isn't available.
if FW_SHA="$(git -C "$PROJECT_DIR" rev-parse --short HEAD 2>/dev/null)"; then
    FW_DATE="$(git -C "$PROJECT_DIR" show -s --format=%cd --date=format:%Y-%m-%d HEAD 2>/dev/null)"
    FW_VERSION="${FW_DATE}+${FW_SHA}"
    git -C "$PROJECT_DIR" diff --quiet HEAD -- . 2>/dev/null || FW_VERSION="${FW_VERSION}-dirty"
else
    FW_VERSION="unknown"
fi
export FW_VERSION
echo "[GATE] flash-spec version: $FW_VERSION"

# ── flash-spec.json + SHA256SUMS: build-derived flash recipe web_flash consumes
# (DC34-114/122). Single-sourced in gen_manifest.py so the build-from-staging
# orchestrator (release_from_staging.sh) emits byte-identical manifests.
py -3 "$SCRIPT_DIR/gen_manifest.py" "$OUT" "$FW_VERSION" "${SKUS[@]}"

cat <<EOF

Done. Images in: $OUT

Flash commands (replace COMx; esptool = $ESPTOOL):

  Fresh / factory flash (wipes NVS - collectibles + settings reset):
    esptool --chip $CHIP -p COMx write_flash --erase-all 0x0 release/clipboy-<variant>-full.bin

  NVS-preserving update (keeps collectibles + settings; flashes app only):
    esptool --chip $CHIP -p COMx write_flash 0x10000 release/clipboy-<variant>-app.bin

  Part-wise fresh flash (web flasher path - shared boot chain + per-variant app,
  no slicing of the merged full.bin):
    esptool --chip $CHIP -p COMx write_flash \\
      0x0     release/clipboy-bootloader.bin \\
      0x8000  release/clipboy-partitions.bin \\
      0xe000  release/clipboy-boot_app0.bin \\
      0x10000 release/clipboy-<variant>-app.bin

  <variant> = sn34k | res34rch | sn34k-rift | res34rch-rift

NOTE: the *-rift variants bake in the LOCAL (uncommitted) Quantum-Rift boot art
(images/rift/ + rift_art_present.h) and are NOT reproducible from a clean clone -
build them on the machine that has the art. The stock variants are reproducible.
EOF
