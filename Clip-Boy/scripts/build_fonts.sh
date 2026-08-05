#!/usr/bin/env bash
# Regenerate the LVGL .c font files from their source OTF/TTF.
#
# This is NOT part of scripts/build.sh -- the generated ui_font_*.c files are
# committed build artifacts. Run this only when changing glyph COVERAGE.
# Requires Node on PATH (npx fetches lv_font_conv on first run).
#
# Fonts:
#   * monofonto = the Pip-Boy UI typeface (Larabie). EXTERNAL, not committed
#     (font EULA); point MONOFONTO_OTF at it. The original ui_font_pipboy_*.c
#     were generated from it at --range 32-127 (ASCII only), which is why any
#     non-ASCII SSID/device name rendered as a "tofu" box. We widen size 14 to
#     0x20-0x2EFF -- every one of the ~902 glyphs the font actually has:
#     Latin-1 + Latin Extended-A, Greek, Cyrillic, general punctuation, arrows,
#     box-drawing, block elements, geometric shapes (incl. the division dots
#     U+25CB/U+25CF) and misc symbols -- so those names now render natively in
#     the same typeface. (16/18/20 stay ASCII; only size 14 shows untrusted
#     names. Re-run with --size 16 etc. if that changes.)
#   * Noto Emoji (OFL-1.1, vendored at assets/fonts/) = a curated 24-emoji
#     subset compiled as ui_font_emoji_14 and wired as the pip-boy-14 FALLBACK
#     (baked in via --lv-fallback), so common emoji draw a monochrome
#     silhouette instead of a box. Everything outside this set is replaced at
#     display time by cb_safe() in ui_nav.h with a middle dot.
#
# After running, rebuild the firmware (scripts/build.sh) to embed the fonts.
set -euo pipefail
cd "$(dirname "$0")/.."

OTF="${MONOFONTO_OTF:-../documents/monofonto rg.otf}"
EMOJI_TTF="assets/fonts/NotoEmoji-Regular.ttf"
LFC=(npx -y lv_font_conv)

if [ ! -f "$OTF" ]; then
    echo "ERROR: monofonto OTF not found at: $OTF" >&2
    echo "       set MONOFONTO_OTF=/path/to/monofonto.otf" >&2
    exit 1
fi
if [ ! -f "$EMOJI_TTF" ]; then
    echo "ERROR: $EMOJI_TTF missing. It is a static instance of Google's" >&2
    echo "       NotoEmoji[wght].ttf (OFL). Re-create with:" >&2
    echo "       py -3 -m fontTools.varLib.instancer 'NotoEmoji[wght].ttf' wght=400 -o $EMOJI_TTF" >&2
    exit 1
fi

# 24 curated emoji: signal, dish, globe, lock, unlock, key, laptop, robot,
# skull, skull&bones, ghost, alien, devil, fire, volt, rocket, star, heart,
# sparkles, poo, party, cat, unicorn, thumbs-up.
EMOJI="0x1F4F6,0x1F4E1,0x1F310,0x1F512,0x1F513,0x1F511,0x1F4BB,0x1F916,0x1F480,0x2620,0x1F47B,0x1F47D,0x1F608,0x1F525,0x26A1,0x1F680,0x2B50,0x2764,0x2728,0x1F4A9,0x1F389,0x1F431,0x1F984,0x1F44D"

echo "[fonts] emoji subset -> ui_font_emoji_14.c"
"${LFC[@]}" --bpp 4 --size 14 --no-compress --font "$EMOJI_TTF" \
    --range "$EMOJI" --format lvgl -o ui_font_emoji_14.c

# Sizes 14/16/18 carry the extended range + emoji fallback because the tool
# "terminal" log can render names at any of them (Settings > Terminal Text =
# Small/Medium/Large). 20 stays ASCII -- only big headers use it, never names.
# The emoji fallback is the single size-14 font reused at all sizes (a 14px
# emoji inside a 16/18px line is fine).
for SZ in 14 16 18; do
    echo "[fonts] extended pip-boy $SZ (+emoji fallback) -> ui_font_pipboy_$SZ.c"
    "${LFC[@]}" --bpp 4 --size "$SZ" --no-compress --font "$OTF" \
        --range 0x20-0x2EFF --lv-fallback ui_font_emoji_14 \
        --format lvgl -o "ui_font_pipboy_$SZ.c"
done

# Newer lv_font_conv drops the __has_include auto-detect that makes "lvgl.h"
# resolve under the Arduino include layout (it emits a bare
# `#include "lvgl/lvgl.h"` that doesn't exist here). Restore it so the files
# build like the older committed fonts.
normalize_include() {
    local f="$1"
    if ! grep -q '__has_include("lvgl.h")' "$f"; then
        perl -0pi -e 's{#ifdef LV_LVGL_H_INCLUDE_SIMPLE\n#include "lvgl\.h"}{#ifdef __has_include\n    #if __has_include("lvgl.h")\n        #ifndef LV_LVGL_H_INCLUDE_SIMPLE\n            #define LV_LVGL_H_INCLUDE_SIMPLE\n        #endif\n    #endif\n#endif\n\n#ifdef LV_LVGL_H_INCLUDE_SIMPLE\n#include "lvgl.h"}' "$f"
    fi
}
normalize_include ui_font_emoji_14.c
normalize_include ui_font_pipboy_14.c
normalize_include ui_font_pipboy_16.c
normalize_include ui_font_pipboy_18.c

echo "[fonts] done. Rebuild firmware (scripts/build.sh) to embed."
