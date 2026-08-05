#!/usr/bin/env python3
"""embed_audio.py -- split audio/*.mp3 between PROGMEM and littlefs + captions.

Scans a folder NON-recursively for .mp3, reads each file's ID3v2 COMM comment as
the on-screen caption, then splits clips by a PROGMEM budget (the ESP32-S3 DROM
wall -- see docs/radio-audio-pipeline.md):
  * Small clips (fit the budget, smallest-first, `1-1` always kept)  -> embedded
    as PROGMEM byte arrays.
  * The rest (big flavor beds) -> STAGED into <lfs_dir>/radio/<name>.mp3 for the
    littlefs image, and recorded in the manifest with a `path` instead of `data`.

Emits radio_audio_gen.cpp/.h: `RadioClip{ name, data, len, caption, path }` in
g_radio_clips[]. data==NULL means "stream from littlefs at path" (the player skips
it gracefully if the file is missing -- #noReboots). The .mp3 files are the
committed source; the .cpp + staged copies are git-ignored build artifacts.

Usage: py -3 embed_audio.py <audio_dir> <out_base> <progmem_budget_bytes> [lfs_dir]
  lfs_dir omitted -> over-budget clips are SKIPPED (no littlefs), old behavior.
"""
import os, sys, struct, glob

def _synchsafe(b):
    return (b[0] << 21) | (b[1] << 14) | (b[2] << 7) | b[3]

def _decode(enc, raw, skip_desc):
    if enc in (1, 2):
        if skip_desc:
            i = 0
            while i + 1 < len(raw):
                if raw[i] == 0 and raw[i+1] == 0:
                    raw = raw[i+2:]; break
                i += 2
            else:
                raw = b""
        return raw.decode('utf-16' if enc == 1 else 'utf-16-be', 'replace')
    codec = 'utf-8' if enc == 3 else 'latin-1'
    if skip_desc:
        parts = raw.split(b'\x00', 1)
        raw = parts[1] if len(parts) > 1 else b""
    return raw.decode(codec, 'replace')

def read_id3_comment(data):
    if data[:3] != b'ID3':
        return ""
    ver = data[3]
    size = _synchsafe(data[6:10])
    pos, end = 10, 10 + size
    while pos + 10 <= end:
        fid = data[pos:pos+4]
        if fid == b'\x00\x00\x00\x00':
            break
        fsize = _synchsafe(data[pos+4:pos+8]) if ver >= 4 else struct.unpack('>I', data[pos+4:pos+8])[0]
        fdata = data[pos+10:pos+10+fsize]
        if fid == b'COMM' and len(fdata) >= 4:
            txt = _decode(fdata[0], fdata[4:], skip_desc=True).strip()
            if txt:
                return txt
        pos += 10 + fsize
    return ""

_SMART = {'‘': "'", '’': "'", '“': '"', '”': '"',
          '–': '-', '—': '-', '…': '...', ' ': ' '}
def to_ascii(s):
    for k, v in _SMART.items():
        s = s.replace(k, v)
    return s.encode('ascii', 'ignore').decode('ascii')

def c_escape(s):
    return (s.replace('\\', '\\\\').replace('"', '\\"')
             .replace('\r', '').replace('\n', '\\n').replace('\t', ' '))

def sym(name):
    return "a_" + "".join(c if c.isalnum() else "_" for c in name)

def main():
    if len(sys.argv) not in (4, 5):
        print("usage: embed_audio.py <audio_dir> <out_base> <progmem_budget_bytes> [lfs_dir]", file=sys.stderr)
        return 2
    audio_dir, base, budget = sys.argv[1], sys.argv[2], int(sys.argv[3])
    lfs_dir = sys.argv[4] if len(sys.argv) == 5 else None
    files = sorted(glob.glob(os.path.join(audio_dir, "*.mp3")))
    if not files:
        print("embed_audio: no .mp3 files in " + audio_dir, file=sys.stderr)
        return 2

    allc = []   # (name, symbol, bytes, caption)
    for f in files:
        data = open(f, "rb").read()
        name = os.path.splitext(os.path.basename(f))[0]
        cap = c_escape(to_ascii(read_id3_comment(data)))
        allc.append((name, sym(name), data, cap))

    # Route clips PROGMEM vs littlefs:
    #  * [music] beds (declared via the ID3 caption "[music]") ALWAYS stream from
    #    littlefs -- they're non-crucial and degrade gracefully if the image is
    #    absent, so keeping them off the fixed DROM/PROGMEM window frees that budget
    #    for speech + collectible art (which have no such fallback).
    #  * 1-1 (ARG breadcrumb) + Summon_the_Data (collectible-75) ALWAYS stay in
    #    PROGMEM so they work with zero littlefs dependency (pinned to the front).
    #  * everything else (speech) contends for PROGMEM smallest-first, up to budget.
    def prio(c):
        # 1-1 (ARG breadcrumb) + Summon_the_Data are pinned to PROGMEM so they play
        # with zero littlefs dependency. NOTE (owner decision, 2026-07-12):
        # Summon_the_Data is NOT strictly required for the ARG -- if we approach the
        # app-size limit or hit flashing issues, it's fine to drop it from this pin so
        # it rides littlefs like the other beds (its play path falls back gracefully).
        return (0 if c[0] in ("1-1", "Summon_the_Data") else 1, len(c[2]))
    prog, lfs, total = [], [], 0
    for c in sorted(allc, key=prio):
        pinned = c[0] in ("1-1", "Summon_the_Data")
        music  = c[3].strip().lower() == "[music]"
        if pinned or (not music and total + len(c[2]) <= budget):
            prog.append(c); total += len(c[2])
        else:
            lfs.append(c)

    # Stage the littlefs clips (or, with no lfs_dir, they're simply skipped).
    if lfs and lfs_dir:
        radio_dir = os.path.join(lfs_dir, "radio")
        os.makedirs(radio_dir, exist_ok=True)
        for fn in os.listdir(radio_dir):        # clean stale clips first
            if fn.endswith(".mp3"):
                os.remove(os.path.join(radio_dir, fn))
        for name, _s, data, _cap in lfs:
            with open(os.path.join(radio_dir, name + ".mp3"), "wb") as f:
                f.write(data)
    elif lfs and not lfs_dir:
        print("embed_audio: WARNING -- %d clip(s) over the %.2f MB PROGMEM budget and NO lfs_dir "
              "given -> SKIPPED:" % (len(lfs), budget / 1048576.0), file=sys.stderr)
        for n, _s, d, _c in sorted(lfs, key=lambda c: -len(c[2])):
            print("    SKIP %-20s %7.1f KB" % (n, len(d) / 1024.0), file=sys.stderr)

    # Manifest: PROGMEM clips carry data=<sym>, path=NULL; littlefs clips carry
    # data=NULL, path="/radio/<name>.mp3". Sorted by name for a stable order.
    manifest = [(n, s, len(d), c, None) for n, s, d, c in prog]
    if lfs_dir:
        manifest += [(n, None, len(d), c, "/radio/%s.mp3" % n) for n, s, d, c in lfs]
    manifest.sort(key=lambda m: m[0])

    with open(base + ".cpp", "w", encoding="utf-8", newline="\n") as f:
        f.write('// Generated by scripts/embed_audio.py from %s. Do not edit.\n' % audio_dir)
        f.write('#include "%s.h"\n\n' % os.path.basename(base))
        for name, s, data, _cap in prog:
            f.write("static const uint8_t %s[] = {\n" % s)
            for i in range(0, len(data), 20):
                f.write("  " + ",".join("0x%02x" % b for b in data[i:i+20]) + ",\n")
            f.write("};\n")
        f.write("\nextern const RadioClip g_radio_clips[] = {\n")
        for name, s, ln, cap, path in manifest:
            data_expr = s if s else "nullptr"
            path_expr = '"%s"' % path if path else "nullptr"
            f.write('  { "%s", %s, %du, "%s", %s },\n' % (name, data_expr, ln, cap, path_expr))
        f.write("};\n")
        f.write("extern const int g_radio_clip_count = %d;\n" % len(manifest))

    with open(base + ".h", "w", encoding="utf-8", newline="\n") as f:
        f.write("#pragma once\n#include <stdint.h>\n")
        f.write("// Generated by scripts/embed_audio.py. Do not edit.\n")
        f.write("// data==nullptr -> stream from littlefs at `path` (skipped if missing).\n")
        f.write("struct RadioClip { const char *name; const uint8_t *data; uint32_t len; "
                "const char *caption; const char *path; };\n")
        f.write("extern const RadioClip g_radio_clips[];\n")
        f.write("extern const int g_radio_clip_count;\n")

    print("embed_audio: %d clips -> %d PROGMEM (%.2f MB / %.2f MB budget) + %d littlefs"
          % (len(manifest), len(prog), total / 1048576.0, budget / 1048576.0,
             len(manifest) - len(prog)))
    return 0

if __name__ == "__main__":
    sys.exit(main())
