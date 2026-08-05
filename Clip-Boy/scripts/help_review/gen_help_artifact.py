#!/usr/bin/env python3
"""Generate the Clip-Boy Help-review artifact (self-contained HTML).

Extracts the live Help hierarchy directly from ui_nav.h (no transcription drift),
merges in a review's metadata + expert-panel findings + adjudication from a
sidecar JSON, and emits a single timestamped .html (embedded CSS+JS, collapsible)
the dev team can read.

This is the engine behind the `help-review` skill. The skill: extract -> spawn
expert agents -> write findings JSON -> run this -> artifact.

Usage:
    py -3 scripts/help_review/gen_help_artifact.py <findings.json> <out.html>

findings.json schema (all fields optional except where noted):
{
  "reviewed_utc": "2026-06-19 22:53Z",       # required
  "firmware_commit": "ab1482a",
  "reviewer": "Claude Opus 4.8 + 3-agent panel",
  "verdict": "Ship-safe after fixes ...",
  "panels": [                                  # raw expert outputs
    {"name": "Safety / red-team", "body_md": "..."},
    {"name": "Accuracy vs code",  "body_md": "..."},
    {"name": "Completeness",      "body_md": "..."}
  ],
  "adjudication": [                            # one row per finding
    {"area":"Accuracy","location":"LEDs > Animations","severity":"WRONG",
     "finding":"...","disposition":"FIXED","note":"commit abc123"}
  ]
}
"""
import hashlib
import html
import json
import re
import sys
import os

UI_NAV = os.path.join(os.path.dirname(__file__), "..", "..", "ui_nav.h")
STAMP = os.path.join(os.path.dirname(__file__), "..", "..", "docs",
                     "help-review", "last-reviewed.json")


def readme_known_issues():
    """The README's "## Known issues" section, or a sentinel when it cannot be read.

    WHY THIS IS PART OF THE HELP CORPUS: it is user-facing text making CHECKABLE CLAIMS ABOUT THE
    FIRMWARE, which is the same category as the on-badge Help -- and it drifts the same way. Proven
    2026-07-26: a fix made Monitor > RSSI distinguish "gone" from "steady", which silently
    invalidated the README section stating the two were indistinguishable. The on-badge text was
    reviewed by this system; the README was not in its corpus, so that drift would have survived a
    full /help-review. Folding it into the hash is what makes the review one-and-done: edit the
    firmware, the section's claims go stale, the staleness gate warns, the review re-reads it.

    Returns a SENTINEL rather than "" when the file or section is missing, so "absent" and "present
    but empty" hash differently -- otherwise deleting the section would leave the stamp matching and
    the gate would report clean about text that no longer exists.
    """
    # The reviewed README lives at the REPO ROOT. stage_source.sh REWRITES that root README.md to
    # the public landing page (which has no "## Known issues" section) -- so when the strict
    # release gate runs check_help.py from a STAGING tree, the HERE-relative path reads the public
    # README, the section is missing, and corpus_sha can never match the stamp -> a spurious
    # staleness FAIL. release_from_staging.sh exports CLIPBOY_SOURCE_REPO=<real repo>; use it so the
    # gate hashes the SAME README the review stamped. Standalone runs (env unset) resolve the real
    # repo via the HERE-relative path, unchanged.
    _src = os.environ.get("CLIPBOY_SOURCE_REPO")
    if _src:
        path = os.path.normpath(os.path.join(_src, "README.md"))
    else:
        path = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                             "..", "..", "..", "README.md"))
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            txt = f.read()
    except OSError:
        return "\x02README-MISSING"
    # ALL matching sections, not the first. The README currently has TWO "## Known issues"
    # headings (a short one and a detailed one), and taking only the first silently excluded the
    # detailed section -- i.e. exactly the text this integration exists to cover. Caught by
    # checking that the extracted text contained the "###" subsections it should have; it did not.
    # Concatenating is robust however many sections exist, and needs no decision about which is
    # canonical.
    secs = re.findall(r"^##\s+Known issues\s*$(.*?)(?=^##\s|\Z)", txt, re.M | re.S)
    if not secs:
        return "\x02README-KNOWN-ISSUES-SECTION-MISSING"
    return "\n".join(secs)


def corpus_sha(cats):
    """Stable hash of the live Help corpus: on-badge Help + the README's Known issues section.

    Both check_help.py (which compares) and this generator (which stamps) call THIS function, so
    they cannot disagree about what the corpus is -- a split definition would leave the gate
    warning forever or, worse, passing forever.
    """
    h = hashlib.sha256()
    for c in cats:
        h.update(c["name"].encode("utf-8"))
        for it in c["items"]:
            h.update(b"\x00")
            h.update(it["title"].encode("utf-8"))
            h.update(b"\x01")
            h.update(it["text"].encode("utf-8"))
    h.update(b"\x03README-KNOWN-ISSUES\x03")
    h.update(readme_known_issues().encode("utf-8"))
    return h.hexdigest()

STR_LIT = re.compile(r'"((?:[^"\\]|\\.)*)"')


def _unescape(s):
    return (s.replace('\\"', '"').replace('\\\\', '\\')
             .replace('\\n', ' ').replace('\\t', ' ').strip())


def parse_help(src):
    """Return ordered list of {name, items:[{title,text,res34rch}]}."""
    # 1) category order from the help_categories[] block.
    cat_block = re.search(r'help_categories\[\]\s*=\s*\{(.*?)\};', src, re.S)
    order = []  # (display_name, array_name)
    if cat_block:
        for m in re.finditer(r'HELP_CAT\(\s*"((?:[^"\\]|\\.)*)"\s*,\s*(\w+)\s*\)',
                             cat_block.group(1)):
            order.append((_unescape(m.group(1)), m.group(2)))

    # 2) each HelpItem array body.
    arrays = {}
    for m in re.finditer(r'static const HelpItem (\w+)\[\]\s*=\s*\{(.*?)\n\};',
                         src, re.S):
        arrays[m.group(1)] = m.group(2)

    cats = []
    for disp, arr in order:
        body = arrays.get(arr, "")
        items = []
        res_depth = 0           # inside #ifdef CLIPBOY_RES34RCH
        # Walk line-buffered so we can track #ifdef regions, accumulating
        # brace-delimited items that may span multiple lines.
        buf = ""
        for line in body.splitlines():
            stripped = line.strip()
            if stripped.startswith('#if') and 'CLIPBOY_RES34RCH' in stripped:
                res_depth += 1
                continue
            if stripped.startswith('#endif'):
                if res_depth > 0:
                    res_depth -= 1
                continue
            if stripped.startswith('#'):
                continue
            buf += " " + line
            # An item closes at "}," or "}" at depth — detect a complete {...}.
            while True:
                o = buf.find('{')
                if o < 0:
                    break
                c = buf.find('}', o)
                if c < 0:
                    break
                chunk = buf[o + 1:c]
                buf = buf[c + 1:]
                lits = STR_LIT.findall(chunk)
                if not lits:
                    continue
                title = _unescape(lits[0])
                text = _unescape(" ".join(
                    _unescape(x) for x in lits[1:]))
                items.append({"title": title, "text": text,
                              "res34rch": res_depth > 0})
        cats.append({"name": disp, "items": items})
    return cats


SEV_CLASS = {"BLOCK": "sev-block", "WRONG": "sev-block", "REVIEW": "sev-rev",
             "IMPRECISE": "sev-rev", "NIT": "sev-nit", "NOTED": "sev-nit"}
DISP_CLASS = {"FIXED": "d-fixed", "DEFERRED": "d-def", "NOTED": "d-note",
              "WONTFIX": "d-note"}


def md_lite(s):
    """Tiny markdown -> HTML: escape, then **bold**, `code`, and newlines."""
    s = html.escape(s)
    s = re.sub(r'\*\*(.+?)\*\*', r'<strong>\1</strong>', s)
    s = re.sub(r'`([^`]+)`', r'<code>\1</code>', s)
    return s.replace('\n', '<br>')


def render(findings, cats):
    meta = findings
    panels = meta.get("panels", [])
    adj = meta.get("adjudication", [])

    def esc(x):
        return html.escape(str(x))

    rows = ""
    for a in adj:
        sev = a.get("severity", "")
        disp = a.get("disposition", "")
        rows += (
            f'<tr><td>{esc(a.get("area",""))}</td>'
            f'<td>{esc(a.get("location",""))}</td>'
            f'<td><span class="pill {SEV_CLASS.get(sev,"sev-nit")}">{esc(sev)}</span></td>'
            f'<td>{md_lite(a.get("finding",""))}</td>'
            f'<td><span class="pill {DISP_CLASS.get(disp,"d-note")}">{esc(disp)}</span> '
            f'{md_lite(a.get("note",""))}</td></tr>\n')

    panels_html = ""
    for p in panels:
        panels_html += (
            f'<details class="panel"><summary>{esc(p.get("name","Panel"))}</summary>'
            f'<div class="panelbody">{md_lite(p.get("body_md",""))}</div></details>\n')

    cats_html = ""
    total_items = 0
    for c in cats:
        items = ""
        for it in c["items"]:
            total_items += 1
            tag = ('<span class="pill r34">Res34rch only</span>'
                   if it["res34rch"] else "")
            items += (f'<div class="hi"><div class="hi-t">{esc(it["title"])} {tag}</div>'
                      f'<div class="hi-x">{esc(it["text"])}</div></div>\n')
        cats_html += (
            f'<details class="cat"><summary>{esc(c["name"])} '
            f'<span class="count">{len(c["items"])}</span></summary>{items}</details>\n')

    return f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Clip-Boy Help Review &mdash; {esc(meta.get('reviewed_utc',''))}</title>
<style>
 :root{{--amber:#ffb000;--bg:#0c0c0c;--panel:#161616;--line:#2a2a2a;--dim:#9a9a9a;}}
 *{{box-sizing:border-box}}
 body{{font:14px/1.5 -apple-system,Segoe UI,Roboto,sans-serif;margin:0;background:var(--bg);color:#e8e8e8}}
 header{{padding:20px 24px;border-bottom:2px solid var(--amber);background:#000}}
 h1{{margin:0 0 4px;font-size:20px;color:var(--amber)}}
 .meta{{color:var(--dim);font-size:13px}}
 .meta b{{color:#e8e8e8}}
 main{{max-width:1100px;margin:0 auto;padding:20px 24px 60px}}
 h2{{color:var(--amber);font-size:16px;margin:28px 0 10px;border-bottom:1px solid var(--line);padding-bottom:6px}}
 .verdict{{background:var(--panel);border-left:3px solid var(--amber);padding:12px 14px;border-radius:4px}}
 table{{width:100%;border-collapse:collapse;font-size:13px}}
 th,td{{text-align:left;padding:7px 9px;border-bottom:1px solid var(--line);vertical-align:top}}
 th{{color:var(--amber);font-weight:600;position:sticky;top:0;background:#000}}
 .pill{{display:inline-block;padding:1px 7px;border-radius:10px;font-size:11px;font-weight:600;white-space:nowrap}}
 .sev-block{{background:#4a1414;color:#ff8080}} .sev-rev{{background:#4a3a14;color:#ffd080}} .sev-nit{{background:#1f3a1f;color:#9fdf9f}}
 .d-fixed{{background:#16331c;color:#7fe89a}} .d-def{{background:#33301a;color:#e8d27f}} .d-note{{background:#23262e;color:#a9b6d6}}
 .r34{{background:#2a1a3a;color:#caa6ff}}
 details{{background:var(--panel);border:1px solid var(--line);border-radius:5px;margin:8px 0}}
 summary{{cursor:pointer;padding:9px 12px;font-weight:600;color:var(--amber);user-select:none}}
 .panel summary{{color:#cfe0ff}}
 .panelbody{{padding:4px 14px 14px;color:#d6d6d6;font-size:13px}}
 .cat .count{{background:#000;color:var(--dim);border:1px solid var(--line);border-radius:10px;padding:0 7px;font-size:11px;margin-left:6px}}
 .hi{{padding:8px 14px;border-top:1px dashed var(--line)}}
 .hi-t{{color:#fff;font-weight:600}} .hi-x{{color:#c4c4c4;margin-top:2px}}
 code{{background:#000;border:1px solid var(--line);border-radius:3px;padding:0 4px;color:var(--amber);font-size:12px}}
 .controls{{margin:10px 0}} button{{background:#000;color:var(--amber);border:1px solid var(--amber);border-radius:4px;padding:5px 10px;cursor:pointer;font-size:12px}}
</style></head><body>
<header>
 <h1>Clip-Boy &mdash; Help System Review</h1>
 <div class="meta">Reviewed <b>{esc(meta.get('reviewed_utc',''))}</b> &middot;
  firmware <b>{esc(meta.get('firmware_commit','?'))}</b> &middot;
  reviewer <b>{esc(meta.get('reviewer','?'))}</b> &middot;
  <b>{len(cats)}</b> categories / <b>{total_items}</b> entries</div>
</header>
<main>
 <h2>Verdict</h2>
 <div class="verdict">{md_lite(meta.get('verdict',''))}</div>

 <h2>Adjudication &mdash; {len(adj)} findings</h2>
 <table><thead><tr><th>Area</th><th>Location</th><th>Sev</th><th>Finding</th><th>Disposition</th></tr></thead>
 <tbody>{rows}</tbody></table>

 <h2>Expert panel (raw inputs)</h2>
 {panels_html}

 <h2>Full Help menu (live, extracted from ui_nav.h)</h2>
 <div class="controls"><button onclick="document.querySelectorAll('.cat').forEach(d=>d.open=true)">Expand all</button>
 <button onclick="document.querySelectorAll('.cat').forEach(d=>d.open=false)">Collapse all</button></div>
 {cats_html}
</main>
<script>
 // Persist nothing; pure static. Expand the first category for orientation.
 var first=document.querySelector('.cat'); if(first) first.open=true;
</script>
</body></html>"""


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    findings = json.load(open(sys.argv[1], encoding="utf-8"))
    src = open(UI_NAV, encoding="utf-8", errors="replace").read()
    cats = parse_help(src)
    out = render(findings, cats)
    with open(sys.argv[2], "w", encoding="utf-8") as f:
        f.write(out)
    n = sum(len(c["items"]) for c in cats)
    # Stamp the corpus hash so the pre-build CI check can tell when Help has
    # drifted from the last reviewed state and a re-review is due.
    sha = corpus_sha(cats)
    os.makedirs(os.path.dirname(STAMP), exist_ok=True)
    with open(STAMP, "w", encoding="utf-8") as f:
        json.dump({"corpus_sha": sha,
                   "reviewed_utc": findings.get("reviewed_utc", ""),
                   "artifact": os.path.basename(sys.argv[2]),
                   "firmware_commit": findings.get("firmware_commit", "")},
                  f, indent=2)
    print(f"Wrote {sys.argv[2]} ({len(cats)} categories, {n} entries)")
    print(f"Stamped {os.path.basename(STAMP)} corpus_sha={sha[:12]}")


if __name__ == "__main__":
    main()
