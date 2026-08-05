"""Embed the canonical collectibles CSV as a PROGMEM C string header.

data/collectibles.csv is the SINGLE SOURCE OF TRUTH for collectible data.
This script reads it and generates collectibles_csv.h (the entire CSV as a
null-terminated PROGMEM byte array) -- the compiled-in built-in catalog
(overridable at runtime by an SD/LittleFS collectibles.csv).

collectibles_csv.h is a DERIVED build artifact: it is git-ignored and
regenerated on every `scripts/build.sh` run, so it can never drift from the
CSV. Do NOT hand-edit it; edit data/collectibles.csv instead.

Usage:
  py -3 scripts/embed_csv.py        # (build.sh runs this automatically)

Input:  data/collectibles.csv
Output: collectibles_csv.h          (PROGMEM byte array in project root)
"""
import sys, os

INPUT  = os.path.join(os.path.dirname(__file__), "..", "data", "collectibles.csv")
OUTPUT = os.path.join(os.path.dirname(__file__), "..", "collectibles_csv.h")

# Editors on Windows often save "smart" punctuation (cp1252) -- ellipsis, curly
# quotes, en/em dashes. The Pip-Boy font is ASCII-only, so those wouldn't render
# anyway. Read tolerantly (utf-8, fall back to cp1252) and FLATTEN to ASCII so
# the build never breaks on an editor's encoding and the on-device text is clean.
SMART = {
    "‘": "'", "’": "'", "‚": "'", "‛": "'",   # single quotes
    "“": '"', "”": '"', "„": '"', "‟": '"',   # double quotes
    "–": "-", "—": "-", "―": "-", "−": "-",   # dashes/minus
    "…": "...", " ": " ", "­": "", "•": "*",  # ellipsis/nbsp/bullet
    "°": " deg", "™": "(TM)", "®": "(R)", "©": "(C)",
}

with open(INPUT, "rb") as f:
    rawb = f.read()
try:
    raw = rawb.decode("utf-8")
except UnicodeDecodeError:
    raw = rawb.decode("cp1252")          # Windows editor fallback
for k, v in SMART.items():
    raw = raw.replace(k, v)
# Any remaining non-ASCII -> closest ASCII (strip accents), else drop.
import unicodedata
raw = "".join(
    c if ord(c) < 128
    else (unicodedata.normalize("NFKD", c).encode("ascii", "ignore").decode("ascii"))
    for c in raw
)
# Normalize line endings to LF. CRITICAL for byte-reproducibility (DC34-122):
# the working tree checks out CRLF on Windows, and without this the baked-in
# PROGMEM bytes (and the self-healed source) would differ from a clean-clone/
# Linux LF build -> divergent app_elf_sha256 -> divergent signed SHA256SUMS.
raw = raw.replace("\r\n", "\n").replace("\r", "\n")

# Self-heal the canonical source: if the CSV carried any smart/non-ASCII bytes,
# write the flattened ASCII back so data/collectibles.csv stays "dumb"-ASCII.
# Idempotent -- a clean (already-ASCII) file produces identical bytes, so no
# write happens and reproducible/clean-tree builds are never disturbed.
ascii_bytes = raw.encode("ascii")
if ascii_bytes != rawb:
    with open(INPUT, "wb") as f:
        f.write(ascii_bytes)
    print(f"  normalized {os.path.basename(INPUT)} -> ASCII (smart glyphs flattened)")

with open(OUTPUT, "w", encoding="utf-8", newline="\n") as f:
    f.write("#pragma once\n")
    f.write("// Auto-generated from data/collectibles.csv by scripts/embed_csv.py\n")
    f.write("// DO NOT EDIT. Edit data/collectibles.csv; build.sh regenerates this.\n\n")
    f.write("#include <pgmspace.h>\n\n")
    f.write("static const char collectibles_csv_data[] PROGMEM = {\n")

    # Write as byte array to avoid escaping headaches (now guaranteed ASCII)
    data = raw.encode("ascii")
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        f.write("    " + ",".join(f"0x{b:02x}" for b in chunk) + ",\n")
    f.write("    0x00  // null terminator\n")
    f.write("};\n")
    f.write(f"static const size_t collectibles_csv_len = {len(data)};\n")

print(f"OK: {len(data)} bytes -> {OUTPUT}")
