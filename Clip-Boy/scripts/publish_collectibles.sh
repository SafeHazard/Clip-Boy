#!/usr/bin/env bash
# publish_collectibles.sh -- one command to (re)publish collectible art after
# editing images/hires_src/<id>.png.
#
# STALENESS GATE: only regenerates when some hires_src png is newer than the
# firmware artifact (coll_images.c). If nothing is newer, it does nothing (use
# --force to rebuild anyway). This is the same gate build.sh warns on.
#
# When stale, it regenerates BOTH derived outputs for exactly the changed IDs:
#   - firmware PROGMEM A8  -> coll_images.c            (build_collectible_images.py)
#   - web RGB 512x512 sq   -> ../clip-boy_images/img/  (build_web_images.py; uniform,
#                             downscale-only + square-pad; re-source small originals in
#                             images/hires_src to raise the floor, images/hires_orig = retired)
# It does NOT commit ui_test (you commit coll_images.c + build deliberately).
# Pass --push to also commit + push clip-boy_images (Netlify auto-deploys).
#
# Usage: scripts/publish_collectibles.sh [--force] [--push]
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HIRES="$PROJECT_DIR/images/hires_src"
COLL_C="$PROJECT_DIR/coll_images.c"
WEB_REPO="$PROJECT_DIR/../clip-boy_images"

FORCE=0; PUSH=0
for a in "$@"; do
    case "$a" in
        --force) FORCE=1 ;;
        --push)  PUSH=1 ;;
        *) echo "unknown arg: $a  (use --force / --push)"; exit 2 ;;
    esac
done

# --- staleness gate ---------------------------------------------------------
# Stale IDs = hires_src/<id>.png newer than coll_images.c. If coll_images.c is
# missing, everything is stale (first run).
if [ -f "$COLL_C" ]; then
    NEWER_ARG=(-newer "$COLL_C")
else
    NEWER_ARG=()   # no artifact yet -> everything is stale
fi
# `|| true` so a no-match grep doesn't trip set -e / pipefail (empty = up to date).
STALE_IDS=$(find "$HIRES" -maxdepth 1 -name '*.png' "${NEWER_ARG[@]}" -printf '%f\n' 2>/dev/null \
            | sed 's/\.png$//' | grep -E '^[0-9]+$' | sort -n | paste -sd, - || true)

if [ -z "$STALE_IDS" ] && [ $FORCE -eq 0 ]; then
    echo "[publish] collectible art is up to date (no hires_src image newer than coll_images.c)."
    echo "[publish] nothing to do. Use --force to rebuild anyway."
    exit 0
fi

if [ $FORCE -eq 1 ] && [ -z "$STALE_IDS" ]; then
    echo "[publish] --force: rebuilding ALL collectibles."
    WEB_IDS=""     # empty -> build_web_images.py rebuilds all
else
    echo "[publish] stale IDs (hires_src newer than coll_images.c): $STALE_IDS"
    WEB_IDS="$STALE_IDS"
fi

# --- firmware PROGMEM A8 (coll_images.c) ------------------------------------
echo "[publish] regenerating firmware A8 -> coll_images.c ..."
py -3 "$PROJECT_DIR/scripts/build_collectible_images.py"

# --- web RGB 200x200 -> clip-boy_images/img ---------------------------------
echo "[publish] regenerating web RGB 200x200 -> clip-boy_images/img ..."
py -3 "$PROJECT_DIR/scripts/build_web_images.py" ${WEB_IDS:+--ids "$WEB_IDS"}

# --- optional web publish ---------------------------------------------------
if [ $PUSH -eq 1 ] && [ -d "$WEB_REPO/.git" ]; then
    echo "[publish] committing + pushing clip-boy_images (Netlify auto-deploys)..."
    git -C "$WEB_REPO" add img _redirects
    if git -C "$WEB_REPO" diff --cached --quiet; then
        echo "[publish] clip-boy_images: nothing changed to push."
    else
        git -C "$WEB_REPO" commit -q -m "Update collectible images (${STALE_IDS:-all})"
        git -C "$WEB_REPO" push origin main
        echo "[publish] clip-boy_images pushed."
    fi
else
    echo "[publish] web images written to clip-boy_images/img (NOT pushed)."
    echo "[publish]   review, then re-run with --push, or: git -C $WEB_REPO add img && commit && push"
fi

echo "[publish] DONE. Review images/review/<id>.png, commit coll_images.c, then build.sh to bake it into firmware."
