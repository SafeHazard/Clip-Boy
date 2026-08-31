#!/bin/bash
# stage_source.sh -- produce a clean, buildable source snapshot of Clip-Boy.
# One command, idempotent. TWO editions:
#
#   (default)  PUBLIC  -> GPLv3 public repo (SafeHazard/Clip-Boy). The KS-exclusive
#                        rift content is absent: a VANILLA rift_private.h (generic
#                        placeholder colorways) + two BLANK boot images are generated,
#                        so a public --rift build shows plain black scenes.
#   --ks       KS      -> private Clip-Boy-KS repo (backers, read-only). Same tree as
#                        public PLUS the REAL rift_private.h colorways + boot art, so
#                        backers build the --rift badge exactly as-built by us.
#
# Repo layout (post subdir move): the firmware lives in the `Clip-Boy/` subdir; the
# repo root holds the web landing pages + README. The staged tree mirrors that:
#   <OUTDIR>/Clip-Boy/...   firmware (arduino sketch folder == Clip-Boy.ino)
#   <OUTDIR>/index.html, postcon/, README.md, LICENSE
#
# Usage:
#   bash scripts/stage_source.sh [--ks] [REF] [VERSION] [OUTDIR]
#     REF      git ref to snapshot            (default: HEAD)
#     VERSION  build-stamp string to pin       (default: read release/flash-spec.json)
#     OUTDIR   output staging dir              (default: clip-boy-src-stage | clip-boy-ks-stage)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FW_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"                 # the Clip-Boy/ firmware dir
REPO_ROOT="$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel)"
FW="$(basename "$FW_DIR")"                              # "Clip-Boy" (== sketch basename)

# --ks flag (position-independent)
KS=0; ARGS=()
for a in "$@"; do
    if [[ "$a" == "--ks" ]]; then KS=1; else ARGS+=("$a"); fi
done
set -- "${ARGS[@]:-}"

REF="${1:-HEAD}"
VERSION="${2:-}"
if [[ "$KS" -eq 1 ]]; then
    OUTDIR="${3:-C:/Users/data/OneDrive/esp/clip-boy-ks-stage}"
else
    OUTDIR="${3:-C:/Users/data/OneDrive/esp/clip-boy-src-stage}"
fi

SAFEHAZARD="C:/Users/data/OneDrive/esp/Clip-Boy"        # public clone (READ-ONLY: web pages)

# ---- resolve VERSION default from the shipped flash-spec -------------------------
if [[ -z "$VERSION" ]]; then
    SPEC="$FW_DIR/release/flash-spec.json"
    [[ -f "$SPEC" ]] && VERSION="$(sed -n 's/.*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$SPEC" | head -1)"
fi
[[ -z "$VERSION" ]] && { echo "ERROR: no VERSION (pass it, or ensure release/flash-spec.json exists)" >&2; exit 1; }

case "$OUTDIR" in ""|"/"|"C:/"|"C:\\") echo "ERROR: refusing to wipe unsafe OUTDIR '$OUTDIR'" >&2; exit 1 ;; esac

ED=$([[ "$KS" -eq 1 ]] && echo "KS (backer)" || echo "PUBLIC")
echo "=== stage_source ================================================"
echo " edition  : $ED"
echo " repo     : $REPO_ROOT   (firmware subdir: $FW/)"
echo " ref      : $REF"
echo " version  : $VERSION"
echo " outdir   : $OUTDIR"
echo "================================================================="

# ---- 1. archive tracked files (root-relative -> OUTDIR/<FW>/... + root meta) -----
# .gitignore already excludes secrets, build artifacts, and the real rift content.
rm -rf "$OUTDIR"; mkdir -p "$OUTDIR"
git -C "$REPO_ROOT" archive "$REF" | tar -x -C "$OUTDIR"
echo "[stage] extracted tracked files from $REF"

# ---- 2. overlay working-tree changes (dry-run faithfulness; clean tree = no-op) --
overlaid=0
while IFS= read -r rel; do
    [[ -z "$rel" ]] && continue
    if [[ -f "$REPO_ROOT/$rel" ]]; then
        mkdir -p "$OUTDIR/$(dirname "$rel")"; cp "$REPO_ROOT/$rel" "$OUTDIR/$rel"
        echo "[stage] overlaid: $rel"; overlaid=$((overlaid+1))
    fi
done < <(git -C "$REPO_ROOT" diff --name-only HEAD)
[[ "$overlaid" -eq 0 ]] && echo "[stage] no working-tree overlays (clean tree)"

# ---- 3. trim internal-only paths (never published, either edition) --------------
TRIM=(
    CLAUDE.md
    .claude
    "$FW/CLAUDE.md"
    "$FW/docs/prs"
    "$FW/docs/rename-clipboy-checklist.md"
    "$FW/docs/web_flash-rename-handoff.md"
    "$FW/eez"
    "$FW/collectibles"
    "$FW/images/hires_src"
    "$FW/images/hires_orig"
    "$FW/TODO.md"
    "$FW/TODO.overnight.md"
    TODO.md                    # root-level working notes -- the $FW/ entries above do
    TODO.overnight.md          # NOT cover these (different directory)
    # Never publish (audit 2026-07-24 FB3). These are ALSO export-ignore'd in
    # Clip-Boy/.gitattributes so `git archive` never emits them; kept here so an older
    # git, a different archive path, or a dropped attribute still cannot publish them.
    "$FW/scripts/production"   # devcon.exe = MS WDK (not ours to redistribute);
                               # flash_inventory.json = KS per-tier backer quantities
    "$FW/docs/audit"           # vulnerability roadmap with file:line per open defect
    "$FW/docs/superpowers"     # internal plans/specs
    # Proprietary Monotype Arial bundled inside vendored LVGL's FreeType DEMO (unused by our
    # build). Publishing the tree would re-distribute a non-free font; strip it. Our real fonts
    # are the generated ui_font_pipboy_*.c glyph headers + Noto Emoji (OFL). (2026-07-31)
    "$FW/libs/lvgl/src/libs/freetype/arial.ttf"
    # drone-remoteid/ has its OWN canonical public repo (SafeHazard/drone-remoteid) --
    # a standalone Remote ID TRANSMITTER toolkit. It stays in this monorepo only for the
    # badge's test tooling (scripts/tests/test_drone_rid.py builds/flashes the dronesim),
    # and is NOT republished with Clip-Boy to avoid two public copies of the same tree.
    # (2026-08-30) To publish it here instead, drop this line + MD_ALLOW its README.
    drone-remoteid
)
for p in "${TRIM[@]}"; do
    [[ -e "$OUTDIR/$p" ]] && { rm -rf "$OUTDIR/$p"; echo "[stage] trimmed: $p"; }
done

# Glob trims (the loop above cannot glob). ARG walkthroughs + security reviews:
# arg-p3-map.txt literally opens '=== P3 "DORK" - FULL MAP ===', and
# check_sku_binaries.py already denylists that string from the BINARIES -- publishing
# it as plaintext would defeat that decision.
for g in "$OUTDIR/$FW/docs/arg-"* "$OUTDIR/$FW/docs/security-review-"*; do
    [[ -e "$g" ]] && { rm -rf "$g"; echo "[stage] trimmed: ${g#"$OUTDIR/"}"; }
done
# ---- 3b. MARKDOWN: default-DENY sweep (owner decision 2026-07-25) ---------------
# Everything above is a denylist, which publishes anything NEW by default -- that is how a
# fresh docs/audit report, a root TODO queue, and the ARG walkthrough all ended up staged.
# Prose is where internal thinking lives, so .md is inverted: every markdown file is dropped
# unless it is explicitly allowed here. Adding a doc to the public drop is now a deliberate
# one-line act rather than an accident of creating a file.
# NOTE: MD_ALLOW must contain everything the REQUIRED check below expects, or staging fails.
MD_ALLOW=(
    "README.md"                      # repo landing page (REQUIRED below)
    "LICENSE.md"                     # GPLv3 text
    "$FW/acceptable_use.md"          # the counsel-approved policy the firmware renders
    "$FW/SECURITY.md"                # release-signature trust anchor + verify instructions
    "$FW/THIRD_PARTY.md"             # vendored-library provenance (license obligation)
    "$FW/AI-DISCLOSURE.md"           # on-device AI disclosure, mirrored in-repo
    "$FW/AI_TRANSPARENCY.md"
    "$FW/hardware/BOM_Clip-Boy.md"   # open-hardware BOM (build-your-own parts list)
)
md_dropped=0; md_kept=0
while IFS= read -r -d '' f; do
    rel="${f#"$OUTDIR/"}"
    allowed=0
    for a in "${MD_ALLOW[@]}"; do [[ "$rel" == "$a" ]] && { allowed=1; break; }; done
    if [[ "$allowed" -eq 1 ]]; then
        md_kept=$((md_kept+1))
    else
        rm -f "$f"; md_dropped=$((md_dropped+1))
        echo "[stage] md-deny: $rel"
    fi
done < <(find "$OUTDIR" -type f -name '*.md' -print0)
echo "[stage] markdown sweep: kept $md_kept allow-listed, dropped $md_dropped"

# Keep only the help-review STAMP (last-reviewed.json -- the strict build gate reads
# it); drop the verbose per-review findings JSON + HTML artifacts.
if [[ -d "$OUTDIR/$FW/docs/help-review" ]]; then
    find "$OUTDIR/$FW/docs/help-review" -type f ! -name 'last-reviewed.json' -delete 2>/dev/null
    echo "[stage] help-review: kept last-reviewed.json, dropped findings/html"
fi

# ---- 4. rift content: KS = real (copied in); PUBLIC = vanilla + blank boot -------
if [[ "$KS" -eq 1 ]]; then
    ks_n=0
    for f in rift_private.h rift_art_present.h rift_clippy_img.c rift_loading_img.c; do
        [[ -f "$FW_DIR/$f" ]] && { cp "$FW_DIR/$f" "$OUTDIR/$FW/$f"; ks_n=$((ks_n+1)); echo "[stage:ks] +$FW/$f"; }
    done
    if [[ -d "$FW_DIR/images/rift" ]]; then
        mkdir -p "$OUTDIR/$FW/images/rift"; cp -r "$FW_DIR/images/rift/." "$OUTDIR/$FW/images/rift/" 2>/dev/null \
            && { ks_n=$((ks_n+1)); echo "[stage:ks] +$FW/images/rift/"; }
    fi
    echo "[stage:ks] KS edition -- $ks_n real rift asset(s) included"
    # Q3 (owner 2026-07-30): missing KS art is now a HARD FAIL, not a WARN. A KS stage with no
    # real rift assets would silently ship BLACK boot screens (the placeholders) as a "KS" build.
    # KS-only blast radius: our KS builds have the art present, so this never fires for them.
    if [[ "$ks_n" -eq 0 ]]; then
        echo "[stage:ks] ERROR: no real rift assets in the working tree -- refusing to stage a KS edition with placeholder art." >&2
        exit 1
    fi
else
    # vanilla rift_private.h: generic placeholder colorways (real ones are KS-only).
    cat > "$OUTDIR/$FW/rift_private.h" <<'RIFT_EOF'
#pragma once
// VANILLA rift colorways (PUBLIC edition). The real Overseer/Space Badge palettes
// are KS-backer-exclusive and NOT in the public source; these generic placeholders
// keep the --rift theme slots valid. Format matches the consuming sites in
// ui_theme.h / ui_nav.h. (See the gate in ui_theme.h.)
#define RIFT_THEME_OVERSEER \
    "Rift Blue", 0x05080F, 0x4A90D8, 0x7AB4E8, 0x2A4870, 0x4A90D8, 0x03060C, 0x244468, 0x40506A, true
#define RIFT_THEME_SPACE_BADGE \
    "Rift Red", 0x0C0000, 0xD85448, 0xE87868, 0x603434, 0xD85448, 0x0C0606, 0x603434, 0x50403A, true
#define RIFT_LED_OV_R  40,120,200, 80,160,240,120,200
#define RIFT_LED_OV_G  80,140,200, 80,140,200, 80,140
#define RIFT_LED_OV_B 200,240,255,200,240,255,200,240
#define RIFT_LED_SB_R 200, 80,160,240,120,200, 80,160
#define RIFT_LED_SB_G  60,100,140, 60,100,140, 60,100
#define RIFT_LED_SB_B  80,120,180, 80,120,180, 80,120
RIFT_EOF
    echo "[stage] wrote vanilla $FW/rift_private.h"
    py -3 "$SCRIPT_DIR/gen_blank_rift.py" "$OUTDIR/$FW" \
        && echo "[stage] PUBLIC edition -- vanilla rift colorways + BLANK boot images"
fi

# ---- 5. pin the build stamp (build.sh reads <sketchdir>/VERSION) -----------------
printf '%s\n' "$VERSION" > "$OUTDIR/$FW/VERSION"
echo "[stage] wrote $FW/VERSION -> $VERSION"

# ---- 6. web landing pages (root) ------------------------------------------------
if [[ -f "$SAFEHAZARD/index.html" ]]; then
    sed -E 's@[[:space:]]*<span class="soon">[^<]*after DEF CON[^<]*</span>@@g' \
        "$SAFEHAZARD/index.html" > "$OUTDIR/index.html"
    echo "[stage] copied index.html (removed 'after DEF CON 34' pill)"
else
    echo "[stage] WARN: $SAFEHAZARD/index.html not found -- skipping landing page"
fi
if [[ -f "$SAFEHAZARD/postcon/index.html" ]]; then
    mkdir -p "$OUTDIR/postcon"; cp "$SAFEHAZARD/postcon/index.html" "$OUTDIR/postcon/index.html"
    echo "[stage] copied postcon/index.html"
fi

# ---- 7. public README (root) ----------------------------------------------------
cat > "$OUTDIR/README.md" <<'README_EOF'
# Clip-Boy

Clip-Boy is an open-source DEF CON electronic badge with a digital wasteland
survivor soul, running on an ESP32-S3 with an LVGL touch UI.

> **Want one?** https://brycebadges.com  -  Docs & help: https://safehazard.github.io/Clip-Boy

## Repository layout
- **`Clip-Boy/`** - the firmware (Arduino sketch `Clip-Boy.ino` + sources, vendored
  `Clip-Boy/libs/`, build scripts in `Clip-Boy/scripts/`).
- **`Clip-Boy/hardware/`** - open-hardware package to build your own badge: bill of
  materials (`BOM_*.md`/`.csv`), PCB source (`Clip-Boy-PCBs.eprj2`, EasyEDA) + fab-ready
  Gerbers for both boards, the enclosure (`Enclosure-*.step`), and the OmniTag HR-code
  master (`omnitag.3mf`).
- **`Clip-Boy/tags/`** + **`Clip-Boy/stands/`** - ready-to-print `.3mf` models: one
  collectible HR-code tag per ID, plus display stands (regenerate any time with
  `Clip-Boy/scripts/build_all_tags.py`). Print assets only - not needed to compile.
- `index.html`, `postcon/` - the project's web pages. `README.md`, `LICENSE`.

## License
**GPLv3** (`LICENSE`, repo root) - forced by linking the GPL `audio-tools` library.
First-party code is also offered under MIT for audio-tools-free builds. Third-party
components: `Clip-Boy/THIRD_PARTY.md`. AI disclosure: `Clip-Boy/AI-DISCLOSURE.md`.
Acceptable use: `Clip-Boy/acceptable_use.md`.

## Credits
The **drone Remote-ID detector** (ASTM F3411 / Open Drone ID) was contributed by
**[zenrandom](https://github.com/zenrandom)**; the companion transmitter/simulator
toolkit lives at **[SafeHazard/drone-remoteid](https://github.com/SafeHazard/drone-remoteid)**.
Full on-device credits are under **Settings > Credits**; third-party components in
`Clip-Boy/THIRD_PARTY.md`.

## Build setup
Versions are pinned for reproducibility:
- **Git Bash** (Windows) to run the build scripts: https://git-scm.com/download/win
  (macOS/Linux already have bash). **Python 3.9+**: https://www.python.org/downloads/
- **`arduino-cli`** (1.4.x).
- **ESP32 core `esp32:esp32@2.0.10`** - REQUIRED, exact version (what the signed
  releases are built with; a different core = a different, non-reproducible binary):
  ```bash
  arduino-cli core install esp32:esp32@2.0.10
  ```
  `build.sh` enforces this and injects the ClipBoy linker flags itself, so **no
  `platform.txt` hand-patching** is needed (unlike a stock Marauder setup).
- Third-party libraries are **vendored** in `Clip-Boy/libs/` - nothing to fetch.

## Build
```bash
cd Clip-Boy
bash scripts/build.sh              # Sn34k-Boy (default, listen-only)
bash scripts/build.sh --res34rch   # Res34rch-Boy (active research tools)
```
Flags compose: `--test`, `--rift` (alt boot screen), `--upload --port COMx`.

## Reproducibility
Builds are deterministic (stamp derived from the commit or a pinned `VERSION`, and
the build path is stripped from the image). From the exact release commit (with the
real `secret.h` + the release `VERSION`), the owner reproduces the signed release
byte-for-byte; a public builder gets a **functionally identical** badge (a fixed ~64
bytes of metadata differ: the ELF self-hash + the esptool image hash). The sketch is
`Clip-Boy.ino` here; the official signed releases are built from the same name.

## The ARG secret
The badge's ARG finale derives phone-unlock codes from a private HMAC key that is
**not** in this repo: `arg_unlock.h` compiles an all-zero placeholder by default, so
the firmware builds and runs (everything works except matching the live phone/IVR).
Define your own 32-byte key to run your own finale.

## Fonts (verify licensing for YOUR use)
The UI typeface (**monofonto**) and tag typeface (**Trek**) are **not included**;
the pre-generated glyph headers (`ui_font_pipboy_*.c`) ARE, so the firmware builds
without them. You only need the `.otf` files to *regenerate* fonts or build the tags.
We don't host or vouch for these fonts - check the license for your use:
monofonto https://www.dafont.com/monofonto.font  -  Trek https://font.download/font/trek

## Legal
Clip-Boy is a fan-made parody project - **not affiliated with, endorsed by, or
sponsored by** any property it references; all such properties remain (C) their
respective owners. Use the badge's tools only on networks/devices you own or are
authorized to test - see `Clip-Boy/acceptable_use.md`.
README_EOF
echo "[stage] wrote README.md"

# KS edition: append a backer note so the PRIVATE repo's README reflects what it is.
# (The public README above must NOT advertise the exclusive assets.)
if [[ "$KS" -eq 1 ]]; then
    cat >> "$OUTDIR/README.md" <<'KSNOTE_EOF'

## Kickstarter backer edition
This is the **KS backer** source tree - identical to the public `SafeHazard/Clip-Boy`
source **plus** the Kickstarter-exclusive **Quantum-Rift** boot art and the
**Overseer / Space Badge** rift colorways (`Clip-Boy/rift_private.h`,
`Clip-Boy/images/rift/`, `Clip-Boy/rift_*_img.c`), so backers can build the `--rift`
badge exactly as shipped. Please keep these exclusive assets within the backer
community - the public build ships vanilla placeholder colorways and blank boot art.
KSNOTE_EOF
    echo "[stage:ks] appended KS backer note to README.md"
fi

# ---- 8. required-file verification (firmware under <FW>/) ------------------------
echo "================================================================="
echo "[verify] required-file check:"
REQUIRED=(
    "$FW/Clip-Boy.ino"
    "$FW/scripts/build.sh"
    "$FW/coll_images.c"
    "$FW/data/collectibles.csv"
    "$FW/data/png_to_a8.py"
    "$FW/scripts/build_tag_plates.py"
    "$FW/scripts/build_all_tags.py"
    "$FW/scripts/build_qr_codes.py"
    "$FW/secret.h.example"
    "$FW/rift_private.h"
    "README.md"
)
missing=0
for f in "${REQUIRED[@]}"; do
    if [[ -e "$OUTDIR/$f" ]]; then echo "  ok   $f"; else echo "  MISS $f"; missing=$((missing+1)); fi
done
nfonts=$(ls "$OUTDIR/$FW"/ui_font_pipboy_*.c 2>/dev/null | wc -l)
[[ "$nfonts" -ge 1 ]] && echo "  ok   $FW/ui_font_pipboy_*.c ($nfonts)" || { echo "  MISS $FW/ui_font_pipboy_*.c"; missing=$((missing+1)); }
# secrets must NEVER be staged
for bad in "$FW/secret.h" for_counsel.md; do
    [[ -e "$OUTDIR/$bad" ]] && { echo "  !! LEAK: $bad present in staged tree"; missing=$((missing+1)); }
done
# Q1 (owner 2026-07-30): KS-exclusive rift ART must never appear in the PUBLIC edition. The other
# 4 KS assets are gitignored AND overwritten/blanked; images/rift/ had only the gitignore layer.
# Public-only: the KS edition legitimately copies it in (above), so this fires only for KS==0.
[[ "$KS" -eq 0 && -d "$OUTDIR/$FW/images/rift" ]] && { echo "  !! LEAK: $FW/images/rift/ present in PUBLIC staged tree"; missing=$((missing+1)); }
# ...nor anything internal (audit 2026-07-24 FB3). Fail the stage rather than publish:
# a re-added devcon.exe / a new docs/audit report / a fresh arg-* writeup must break the
# release, not ride along. Globs are quoted-then-expanded via compgen so a no-match is
# clean under `set -e`.
for pat in "$FW/scripts/production" "$FW/docs/audit" "$FW/docs/superpowers" \
           "$FW/docs/arg-*" "$FW/docs/security-review-*"; do
    while IFS= read -r hit; do
        [[ -n "$hit" ]] || continue
        echo "  !! LEAK: ${hit#"$OUTDIR/"} present in staged tree (internal-only)"
        missing=$((missing+1))
    done < <(compgen -G "$OUTDIR/$pat" 2>/dev/null || true)
done

echo "================================================================="
echo "[summary] edition     : $ED"
echo "[summary] staged tree : $OUTDIR"
echo "[summary] files       : $(find "$OUTDIR" -type f | wc -l)   size: $(du -sh "$OUTDIR" | cut -f1)"
echo "[summary] version pin : $VERSION"
if [[ "$missing" -gt 0 ]]; then echo "[summary] CHECK: FAILED ($missing)" >&2; exit 1; fi
echo "[summary] CHECK: PASSED"
echo "================================================================="
