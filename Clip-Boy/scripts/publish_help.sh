#!/usr/bin/env bash
# publish_help.sh — regenerate the hosted Help page (SafeHazard/Clip-Boy
# help/index.html) from the firmware Help corpus, and in --push mode commit + push it.
#
# gen_help_site.py is DETERMINISTIC — a pure function of (ui_nav.h, tool_info.h, HEAD),
# footer date derived from the commit date, not wall-clock. So this is a safe
# wipe-and-replace: any diff vs the deployed page is a real content change, never a
# timestamp or a hand-edit. (Verify: `py -3 scripts/gen_help_site.py a; py -3 ... b;
# diff a b` is byte-identical.)
#
#   bash scripts/publish_help.sh            # --check: WARN if the deployed page is stale
#   bash scripts/publish_help.sh --push     # regenerate into the clone + commit + push if changed
set -euo pipefail
UT="$(cd "$(dirname "$0")/.." && pwd)"
# SafeHazard/Clip-Boy public clone. UT is the firmware dir (esp/ui_test/Clip-Boy after
# the Clip-Boy/ subdir rename), so the sibling public clone is TWO levels up at
# esp/Clip-Boy (not one -- the old layout had UT = repo root). Override with env.
CB="${CLIPBOY_PUBLIC_CLONE:-$UT/../../Clip-Boy}"
OUT="$CB/help/index.html"
MODE="${1:---check}"

if [ ! -d "$CB/.git" ]; then
    echo "[help-site] SafeHazard clone not at $CB — skipping (nothing to publish/check)"
    exit 0
fi

case "$MODE" in
  --check)
    TMP="$(mktemp)"
    py -3 "$UT/scripts/gen_help_site.py" "$TMP" >/dev/null
    if [ -f "$OUT" ] && diff -q "$OUT" "$TMP" >/dev/null 2>&1; then
        echo "[help-site] OK — deployed help/index.html matches the firmware Help corpus"
    else
        echo "[help-site] WARN: help/index.html is STALE vs the Help corpus."
        echo "[help-site]       run  bash scripts/publish_help.sh --push  to regenerate + deploy."
    fi
    rm -f "$TMP"
    ;;
  --push)
    py -3 "$UT/scripts/gen_help_site.py" "$OUT" >/dev/null   # deterministic wipe-and-replace
    if [ -z "$(git -C "$CB" status --porcelain -- help/index.html)" ]; then
        echo "[help-site] no change — deployed page already current"
        exit 0
    fi
    git -C "$CB" add help/index.html
    git -C "$CB" commit -q -m "help: regenerate help/index.html from firmware corpus"
    git -C "$CB" push origin main
    echo "[help-site] pushed help/index.html -> SafeHazard ($(git -C "$CB" rev-parse --short HEAD))"
    ;;
  *)
    echo "usage: publish_help.sh [--check|--push]"; exit 2 ;;
esac
