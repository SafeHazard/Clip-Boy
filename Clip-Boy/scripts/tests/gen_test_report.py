#!/usr/bin/env py -3
"""gen_test_report.py -- render a self-contained HTML report from a test-run JSON.

Owner requirement (2026-07-25): "results written in html and screenshots used as evidence to
support assertions of correct behavior (esp on our new tests in #57)". The point is that a claim
of correct behaviour is only credible with the pixels attached, so every row that has a
screenshot embeds it inline.

SELF-CONTAINED by design: the BMPs are converted to PNG and inlined as data: URIs, so the single
.html can be mailed, archived, or opened from anywhere without a sibling images folder. The
badge's screenshot command emits RGB565 -> the harness writes a BMP -> this converts to PNG with
pure stdlib (zlib), the same approach grab_screenshot.py uses. No third-party imaging deps: this
has to run on the same machine that flashes badges, with nothing to install.

    py -3 scripts/tests/gen_test_report.py shots/signal_loss/signal_loss_results.json \
        -o docs/test-reports/signal-loss-<date>.html

Also accepts the teardown-suite's text output via --suite-log, so one report can carry both the
regression suite and the loss-of-signal battery.
"""
import argparse
import base64
import datetime
import html
import io
import json
import os
import re
import struct
import sys
import zlib

VERDICT_STYLE = {
    "PASS":              ("#0a4", "PASS"),
    "LOSS-NOT-DETECTED": ("#c70", "LOSS NOT DETECTED"),
    "CANNOT-TEST":       ("#888", "CANNOT TEST"),
    "INFO":              ("#468", "INFO"),
    "FAIL":              ("#c22", "FAIL"),
    "warn":              ("#c70", "WARN"),
}


def _png_chunk(tag, data):
    c = tag + data
    return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)


def bmp_to_png_b64(path):
    """Read a 16bpp or 24bpp BMP and return base64 PNG. Returns None if unreadable.

    Deliberately tolerant: a missing or truncated screenshot must degrade to "no image" in the
    report, never crash the generator. A report that fails to build because one capture went
    wrong is worse than a report with one gap clearly marked.
    """
    try:
        with open(path, "rb") as f:
            d = f.read()
        if d[:2] != b"BM":
            return None
        pix_off = struct.unpack_from("<I", d, 10)[0]
        w = struct.unpack_from("<i", d, 18)[0]
        h = struct.unpack_from("<i", d, 22)[0]
        bpp = struct.unpack_from("<H", d, 28)[0]
        flip = h > 0            # positive height = bottom-up rows
        h = abs(h)
        row_bytes = ((w * bpp // 8) + 3) & ~3
        rows = []
        for y in range(h):
            src_y = (h - 1 - y) if flip else y
            off = pix_off + src_y * row_bytes
            raw = bytearray(b"\x00")            # PNG filter byte 0 per scanline
            if bpp == 16:
                for x in range(w):
                    v = struct.unpack_from("<H", d, off + x * 2)[0]
                    # RGB565 -> RGB888, replicating high bits into the low ones so full-scale
                    # values stay full-scale (a plain shift would cap white at 0xF8).
                    r = ((v >> 11) & 0x1F); g = ((v >> 5) & 0x3F); b = v & 0x1F
                    raw += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
            elif bpp == 24:
                for x in range(w):
                    b, g, r = d[off + x * 3], d[off + x * 3 + 1], d[off + x * 3 + 2]
                    raw += bytes((r, g, b))
            else:
                return None
            rows.append(bytes(raw))
        ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
        png = (b"\x89PNG\r\n\x1a\n" + _png_chunk(b"IHDR", ihdr)
               + _png_chunk(b"IDAT", zlib.compress(b"".join(rows), 9))
               + _png_chunk(b"IEND", b""))
        return base64.b64encode(png).decode("ascii")
    except Exception:
        return None


def parse_suite_log(text):
    """Pull [PASS]/[FAIL]/[warn] lines out of a teardown-suite run so both suites can share one
    report. Format: '  [PASS] NAME: detail'."""
    rows = []
    for m in re.finditer(r"^\s+\[(PASS|FAIL|warn)\]\s+([^:]+):\s*(.*)$", text, re.M):
        rows.append({"case": m.group(2).strip(), "phase": "regression",
                     "verdict": m.group(1), "detail": m.group(3).strip(),
                     "shot": None, "evidence": {}})
    return rows


CSS = """
:root{--bg:#101214;--fg:#e8e6e3;--dim:#9aa0a6;--line:#2a2e33;--card:#171a1d}
@media (prefers-color-scheme:light){:root{--bg:#fbfbfa;--fg:#1a1c1e;--dim:#5f6368;--line:#dcdfe3;--card:#fff}}
:root[data-theme="dark"]{--bg:#101214;--fg:#e8e6e3;--dim:#9aa0a6;--line:#2a2e33;--card:#171a1d}
:root[data-theme="light"]{--bg:#fbfbfa;--fg:#1a1c1e;--dim:#5f6368;--line:#dcdfe3;--card:#fff}
*{box-sizing:border-box}
body{margin:0;padding:2rem 1.25rem 4rem;background:var(--bg);color:var(--fg);
 font:15px/1.55 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;max-width:1100px;margin-inline:auto}
h1{font-size:1.5rem;margin:0 0 .25rem} h2{font-size:1.1rem;margin:2.5rem 0 .75rem;
 padding-bottom:.35rem;border-bottom:1px solid var(--line)}
.meta{color:var(--dim);font-size:.85rem;margin-bottom:1.5rem}
.meta code{background:var(--card);padding:.1rem .35rem;border-radius:3px}
.tallies{display:flex;flex-wrap:wrap;gap:.5rem;margin:1rem 0 2rem}
.tally{background:var(--card);border:1px solid var(--line);border-radius:6px;padding:.5rem .8rem;font-size:.85rem}
.tally b{font-size:1.15rem;display:block}
.case{background:var(--card);border:1px solid var(--line);border-radius:8px;margin:0 0 1rem;overflow:hidden}
.case>summary{cursor:pointer;padding:.7rem .9rem;font-weight:600;display:flex;gap:.6rem;align-items:center}
.case>summary::-webkit-details-marker{display:none}
.body{padding:0 .9rem .9rem}
.row{display:flex;gap:1rem;padding:.7rem 0;border-top:1px solid var(--line);align-items:flex-start;flex-wrap:wrap}
.row .txt{flex:1 1 320px;min-width:0}
.pill{font-size:.7rem;font-weight:700;letter-spacing:.04em;color:#fff;padding:.15rem .45rem;border-radius:3px;white-space:nowrap}
.phase{color:var(--dim);font-size:.78rem;text-transform:uppercase;letter-spacing:.05em}
.detail{margin:.3rem 0 0} .ev{color:var(--dim);font-size:.8rem;margin-top:.35rem;font-family:ui-monospace,Menlo,Consolas,monospace}
figure{margin:0;flex:0 0 auto} figure img{display:block;width:320px;max-width:100%;height:auto;
 image-rendering:pixelated;border:1px solid var(--line);border-radius:4px;background:#000}
figcaption{color:var(--dim);font-size:.72rem;margin-top:.25rem;font-family:ui-monospace,Menlo,Consolas,monospace}
.pair{display:flex;gap:.75rem;flex-wrap:wrap}
.noimg{color:var(--dim);font-size:.8rem;font-style:italic}
.note{background:var(--card);border-left:3px solid #c70;padding:.7rem .9rem;margin:1rem 0;border-radius:0 6px 6px 0}
table{border-collapse:collapse;width:100%;font-size:.87rem} th,td{text-align:left;padding:.4rem .6rem;border-bottom:1px solid var(--line)}
th{color:var(--dim);font-weight:600;font-size:.78rem;text-transform:uppercase;letter-spacing:.04em}
.scroll{overflow-x:auto}
"""


def render(data, suite_rows, title):
    results = list(data.get("results", [])) + list(suite_rows)
    by_case = {}
    for r in results:
        by_case.setdefault(r["case"], []).append(r)

    counts = {}
    for r in results:
        counts[r["verdict"]] = counts.get(r["verdict"], 0) + 1

    out = [f"<title>{html.escape(title)}</title>", f"<style>{CSS}</style>",
           f"<h1>{html.escape(title)}</h1>"]
    stamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
    out.append('<div class="meta">Generated {} &middot; SKU <code>{}</code> &middot; '
               'badge <code>{}</code> &middot; kalipi <code>{}</code></div>'.format(
                   stamp, html.escape(str(data.get("sku", "?"))),
                   html.escape(str(data.get("port", "?"))),
                   html.escape(str(data.get("kalipi", "-")))))

    out.append('<div class="tallies">')
    for v, n in sorted(counts.items(), key=lambda kv: -kv[1]):
        colour, label = VERDICT_STYLE.get(v, ("#666", v))
        out.append(f'<div class="tally" style="border-left:3px solid {colour}">'
                   f'<b>{n}</b>{html.escape(label)}</div>')
    out.append("</div>")

    if counts.get("LOSS-NOT-DETECTED"):
        out.append('<div class="note"><b>On "LOSS NOT DETECTED".</b> This is a finding, not a '
                   'build break. For these tools a retained reading is current documented '
                   'behaviour &mdash; see <i>Known issues</i> in README.md. It is reported '
                   'separately from FAIL so the suite does not sit permanently red, which is the '
                   'fastest way to make a test result get ignored. Compare the paired screenshots: '
                   'if nothing on screen changes when the target stops transmitting, then a steady '
                   'reading and a dead target look identical to the operator.</div>')

    for case, rows in by_case.items():
        worst = "PASS"
        for r in rows:
            if r["verdict"] in ("FAIL", "LOSS-NOT-DETECTED", "CANNOT-TEST", "warn"):
                worst = r["verdict"]
                break
        colour, label = VERDICT_STYLE.get(worst, ("#666", worst))
        out.append('<details class="case" open><summary>'
                   f'<span class="pill" style="background:{colour}">{html.escape(label)}</span>'
                   f'{html.escape(case)}</summary><div class="body">')
        for r in rows:
            c, lab = VERDICT_STYLE.get(r["verdict"], ("#666", r["verdict"]))
            out.append('<div class="row"><div class="txt">')
            out.append(f'<span class="pill" style="background:{c}">{html.escape(lab)}</span> '
                       f'<span class="phase">{html.escape(r.get("phase",""))}</span>')
            out.append(f'<p class="detail">{html.escape(r.get("detail",""))}</p>')
            ev = {k: v for k, v in (r.get("evidence") or {}).items()
                  if not str(k).endswith("_shot")}
            if ev:
                out.append('<div class="ev">' + html.escape(
                    "  ".join(f"{k}={v}" for k, v in ev.items())) + "</div>")
            out.append("</div>")
            # Paired before/after images where the evidence names both, else the single shot.
            evd = r.get("evidence") or {}
            pair = [evd.get("alive_shot") or evd.get("present_shot"),
                    evd.get("gone_shot")]
            imgs = [p for p in pair if p] or ([r["shot"]] if r.get("shot") else [])
            if imgs:
                out.append('<div class="pair">')
                for name in imgs:
                    b64 = bmp_to_png_b64(os.path.join(data["_shotdir"], name))
                    if b64:
                        out.append(f'<figure><img alt="{html.escape(name)}" '
                                   f'src="data:image/png;base64,{b64}">'
                                   f'<figcaption>{html.escape(name)}</figcaption></figure>')
                    else:
                        out.append(f'<div class="noimg">[{html.escape(name)} unreadable]</div>')
                out.append("</div>")
            out.append("</div>")
        out.append("</div></details>")
    return "\n".join(out)


def _referenced_shots(data):
    """Every screenshot basename the JSON references, in the same places render() looks."""
    names = []
    for r in data.get("results", []) or []:
        evd = r.get("evidence") or {}
        for k in ("alive_shot", "present_shot", "gone_shot"):
            if evd.get(k):
                names.append(evd[k])
        if r.get("shot"):
            names.append(r["shot"])
    seen, out = set(), []
    for n in names:
        if n not in seen:
            seen.add(n)
            out.append(n)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("results_json", nargs="?", help="signal-loss results JSON")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--suite-log", help="text output of test_teardown_paths.py to fold in")
    ap.add_argument("--title", default="Clip-Boy test report")
    ap.add_argument("--shots-dir", help="where the referenced .bmp screenshots live "
                                        "(default: next to the JSON, then <repo>/shots/signal_loss)")
    ap.add_argument("--allow-missing-shots", action="store_true",
                    help="emit the report anyway when referenced screenshots cannot be found")
    a = ap.parse_args()

    data = {"results": [], "_shotdir": "."}
    if a.results_json:
        with open(a.results_json, encoding="utf-8") as f:
            data = json.load(f)
        # RESOLVE THE SHOTS DIRECTORY, do not assume it. This used to be hard-wired to the JSON's
        # own directory, so writing the results JSON anywhere other than the shots folder produced
        # a report with ZERO images -- while still printing "self-contained", because the image
        # loader is deliberately tolerant of a missing file. The evidence is the entire point of
        # this report, so silently dropping all of it is the worst possible failure mode.
        here = os.path.dirname(os.path.abspath(__file__))
        candidates = [a.shots_dir] if a.shots_dir else []
        candidates += [os.path.dirname(os.path.abspath(a.results_json)),
                       os.path.normpath(os.path.join(here, "..", "..", "shots", "signal_loss")),
                       os.path.normpath(os.path.join(here, "..", "..", "shots"))]
        named = _referenced_shots(data)
        data["_shotdir"] = candidates[0] if candidates else "."
        for c in candidates:
            if c and named and any(os.path.isfile(os.path.join(c, n)) for n in named):
                data["_shotdir"] = c
                break
        missing = [n for n in named if not os.path.isfile(os.path.join(data["_shotdir"], n))]
        print(f"shots: {len(named) - len(missing)}/{len(named)} found in {data['_shotdir']}")
        if missing:
            print("MISSING screenshots (the report's evidence would be incomplete):")
            for n in missing[:12]:
                print(f"  {n}")
            if not a.allow_missing_shots:
                print("refusing to emit an evidence report with missing evidence; "
                      "pass --shots-dir, or --allow-missing-shots to override")
                return 2

    suite_rows = []
    if a.suite_log:
        with open(a.suite_log, encoding="utf-8", errors="replace") as f:
            suite_rows = parse_suite_log(f.read())

    if not data.get("results") and not suite_rows:
        print("nothing to report (no results JSON rows and no suite log rows)")
        return 1

    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with io.open(a.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(render(data, suite_rows, a.title))
    kb = os.path.getsize(a.out) // 1024
    # Count what actually landed in the HTML rather than claiming "self-contained" on faith --
    # the previous version printed that while embedding zero images.
    with io.open(a.out, encoding="utf-8") as f:
        n_img = f.read().count("data:image/png;base64")
    print(f"report -> {a.out}  ({kb} KB, {n_img} image(s) inlined)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
