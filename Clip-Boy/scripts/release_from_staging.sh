#!/bin/bash
# release_from_staging.sh -- Build the release from the EXACT staging trees that get
# published, so the published bins provably came from the published source.
#
#   clip-boy-src-stage (PUBLIC) -> sn34k + res34rch        -> public web_flash /firmware
#   clip-boy-ks-stage  (KS)     -> sn34k-rift + res34rch-rift -> gated web_flash /rift-staging
#
# The shared boot chain (bootloader/partitions/boot_app0) + littlefs content image are
# built by BOTH stagings and MUST be byte-identical -- that cross-check is the proof the
# two trees agree on the common foundation. We ship one copy. App bins differ by variant.
#
# Output: Clip-Boy/release/{clipboy-*.bin, flash-spec.json, SHA256SUMS} (all 4 variants,
# one unified manifest). Then sign with sign_release_paste.sh (autonomous) + commit/push;
# web_flash routes non-rift -> firmware/, rift -> gated rift-staging/.
#
# Usage: bash scripts/release_from_staging.sh
#   REL_JOBS (default 2) = per-staging compile concurrency (OneDrive-safe).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FW_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"                 # dev Clip-Boy/
REPO_ROOT="$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel)"
# check_source_drop.py (run inside each staging tree by make_release_bins.sh) inspects the REAL
# repo's `git archive HEAD`; the staging output dir has no .git, so point the gate at the real repo.
# Inherited by the `( cd $STAGE && make_release_bins.sh )` subshells and its py subprocess.
export CLIPBOY_SOURCE_REPO="$REPO_ROOT"
PUB_STAGE="C:/Users/data/OneDrive/esp/clip-boy-src-stage"
KS_STAGE="C:/Users/data/OneDrive/esp/clip-boy-ks-stage"
OUT="$FW_DIR/release"
export REL_JOBS="${REL_JOBS:-2}"

# 0a) ARG finale key. The staging trees deliberately carry the all-zero placeholder
# (the key must never land in the published GPLv3 source), but the SHIPPED bins must
# hold the REAL key or the P5 phone unlock can never validate -- the badge would check
# zeros while the live IVR uses the real secret. This shipped broken once (Jul 2026).
# Read it from the DEV tree's gitignored secret.h and hand it to the compiles via the
# env var build.sh consumes, so it lands ONLY in the binaries, never in the staged tree.
# NEVER echo the value.
if [[ -z "${CLIPBOY_ARG_SECRET_HEX:-}" ]]; then
    CLIPBOY_ARG_SECRET_HEX="$(sed -nE 's/.*ARG_HMAC_SECRET_HEX[[:space:]]+"([0-9a-fA-F]{64})".*/\1/p' "$FW_DIR/secret.h" 2>/dev/null || true)"
fi
if [[ ! "${CLIPBOY_ARG_SECRET_HEX:-}" =~ ^[0-9a-fA-F]{64}$ ]]; then
    echo "ERROR: no real ARG key. Need $FW_DIR/secret.h (64-hex ARG_HMAC_SECRET_HEX) or CLIPBOY_ARG_SECRET_HEX." >&2
    echo "       Refusing to cut a release whose P5 phone unlock would be dead." >&2
    exit 1
fi
if [[ "$CLIPBOY_ARG_SECRET_HEX" == "$(printf '0%.0s' {1..64})" ]]; then
    echo "ERROR: the ARG key is the all-zero placeholder -- inject the real key." >&2; exit 1
fi
export CLIPBOY_ARG_SECRET_HEX
echo "[rel] ARG key: real key loaded (fingerprint $(printf '%s' "$CLIPBOY_ARG_SECRET_HEX" | py -3 -c 'import sys,hashlib;print(hashlib.sha256(bytes.fromhex(sys.stdin.read().strip())).hexdigest()[:8])'))"

# 0) clean tree -> the version stamp is a real commit, not -dirty
git -C "$REPO_ROOT" diff --quiet HEAD -- . || { echo "ERROR: working tree dirty -- commit first (build_stamp maps to HEAD)"; exit 1; }
VERSION="$(git -C "$REPO_ROOT" show -s --format=%cd --date=format:%Y-%m-%d HEAD)+$(git -C "$REPO_ROOT" rev-parse --short HEAD)"
echo "=== release_from_staging  version=$VERSION  REL_JOBS=$REL_JOBS ==="

# 1) regenerate BOTH stagings pinned to this version (public = vanilla rift; KS = real)
echo "--- staging (public + KS) ---"
bash "$SCRIPT_DIR/stage_source.sh"      HEAD "$VERSION" "$PUB_STAGE"  >/dev/null
bash "$SCRIPT_DIR/stage_source.sh" --ks HEAD "$VERSION" "$KS_STAGE"   >/dev/null
echo "[rel] staged: $PUB_STAGE (public), $KS_STAGE (KS)"

# 2) provision tools/ (gitignored -> not archived; public users install their own)
cp -r "$FW_DIR/tools" "$PUB_STAGE/Clip-Boy/tools"
cp -r "$FW_DIR/tools" "$KS_STAGE/Clip-Boy/tools"

# 3) build public variants from the PUBLIC staging, rift variants from the KS staging
echo "--- build: sn34k res34rch  (from public staging) ---"
( cd "$PUB_STAGE/Clip-Boy" && bash scripts/make_release_bins.sh sn34k res34rch )
echo "--- build: sn34k-rift res34rch-rift  (from KS staging) ---"
( cd "$KS_STAGE/Clip-Boy" && bash scripts/make_release_bins.sh sn34k-rift res34rch-rift )

PUB_REL="$PUB_STAGE/Clip-Boy/release"
KS_REL="$KS_STAGE/Clip-Boy/release"

# 4) INTEGRITY: the DETERMINISTIC shared parts (boot chain) must be byte-identical
# across both staging builds -- that's the proof the two trees agree.
echo "--- verify shared parts across stagings ---"
for f in clipboy-bootloader.bin clipboy-partitions.bin clipboy-boot_app0.bin; do
    [[ -f "$PUB_REL/$f" && -f "$KS_REL/$f" ]] || { echo "ERROR: $f missing from a staging build"; exit 1; }
    cmp -s "$PUB_REL/$f" "$KS_REL/$f" || { echo "ERROR: shared part $f DIFFERS between public & KS staging -- trees disagree, refusing to publish"; exit 1; }
    echo "[rel] identical: $f"
done
# littlefs (radio beds) is content-driven from the SAME tracked audio/, but mklittlefs
# is NOT byte-deterministic (its FS block layout varies ~450B in 6.75MB across runs).
# So verify the two are the same SIZE (a real content change would move it), then ship
# the PUBLIC one (authoritative; the content is rift-independent). Byte-repro of littlefs
# is functional, not bit-exact -- like the app-bin ELF-self-hash caveat.
LP="$PUB_REL/clipboy-littlefs.bin"; LK="$KS_REL/clipboy-littlefs.bin"
if [[ -f "$LP" && -f "$LK" ]]; then
    sp=$(stat -c%s "$LP"); sk=$(stat -c%s "$LK")
    [[ "$sp" == "$sk" ]] || { echo "ERROR: littlefs SIZE differs ($sp vs $sk) -- real content change, refusing to publish"; exit 1; }
    echo "[rel] littlefs: same size ($sp bytes); content-equivalent (mklittlefs layout not bit-deterministic)"
fi

# 5) assemble the unified release/ (shared + littlefs once; app/full from each source)
echo "--- assemble Clip-Boy/release/ ---"
rm -rf "$OUT"; mkdir -p "$OUT"
cp "$PUB_REL"/clipboy-bootloader.bin "$PUB_REL"/clipboy-partitions.bin \
   "$PUB_REL"/clipboy-boot_app0.bin  "$PUB_REL"/clipboy-littlefs.bin   "$OUT/"
cp "$PUB_REL"/clipboy-sn34k-app.bin    "$PUB_REL"/clipboy-sn34k-full.bin    "$OUT/"
cp "$PUB_REL"/clipboy-res34rch-app.bin "$PUB_REL"/clipboy-res34rch-full.bin "$OUT/"
cp "$KS_REL"/clipboy-sn34k-rift-app.bin    "$KS_REL"/clipboy-sn34k-rift-full.bin    "$OUT/"
cp "$KS_REL"/clipboy-res34rch-rift-app.bin "$KS_REL"/clipboy-res34rch-rift-full.bin "$OUT/"

# 6) unified manifest over all 4 variants (single-sourced generator)
py -3 "$SCRIPT_DIR/gen_manifest.py" "$OUT" "$VERSION" sn34k res34rch sn34k-rift res34rch-rift

# 6b) SCRUB the staging build dirs. Compiling the real ARG key in leaves it in the
# intermediates (build.options.json records the -D, and it lands in .o/objs.a). The
# staging trees exist to be PUBLISHED, so key-bearing artifacts must not linger in
# them. `build/` is gitignored at both levels so it would not be committed anyway --
# this is defense in depth, and it keeps a stray `cp -r` publish from leaking the key.
for st in "$PUB_STAGE" "$KS_STAGE"; do
    rm -rf "$st/Clip-Boy/build" "$st/Clip-Boy/build_littlefs"
done
echo "[rel] scrubbed staging build/ dirs (key-bearing compile intermediates)"

echo "==================================================================="
echo "[rel] DONE. $(ls "$OUT"/clipboy-*.bin | wc -l) bins + flash-spec.json + SHA256SUMS in $OUT"
echo "[rel] version $VERSION -- public sn34k/res34rch from public source, rift from KS source"
echo "[rel] NEXT: bash scripts/sign_release_paste.sh   (autonomous)  ->  commit + push  ->  web_flash"
echo "==================================================================="
