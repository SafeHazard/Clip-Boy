#!/usr/bin/env python3
"""gen_help_site.py -- build a phone-friendly static Help page from the on-device
Help corpus so it can be hosted (SafeHazard/Clip-Boy) and reached via a QR code.

Parses the `help_*[]` HelpItem arrays + `help_categories[]` order, AND the credits
lists from `show_credits`, straight out of ui_nav.h -- SINGLE SOURCE OF TRUTH, no
hand transcription -- and renders one self-contained (inline CSS+JS, no external
fetches), responsive, searchable page. A Legal section links to the canonical
counsel-approved docs in the repo rather than duplicating them (no drift).

Items inside a `#ifdef CLIPBOY_RES34RCH` block are tagged "Res34rch-only".

Usage:
  py -3 scripts/gen_help_site.py [OUT_HTML]
    OUT_HTML defaults to ../Clip-Boy/help/index.html (the SafeHazard public clone).
"""
import re, sys, os, subprocess, html, datetime

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
UI_NAV = os.path.join(ROOT, "ui_nav.h")
TOOL_INFO = os.path.join(ROOT, "tool_info.h")
DEFAULT_OUT = os.path.normpath(os.path.join(ROOT, "..", "Clip-Boy", "help", "index.html"))
REPO = "https://github.com/SafeHazard/Clip-Boy"       # public mirror (renders .md)
BLOB = REPO + "/blob/main"
RAW = "https://raw.githubusercontent.com/SafeHazard/Clip-Boy/main/help"  # absolute so
# images resolve on htmlpreview + Pages + any browser (relative img/ breaks on htmlpreview)

# A C string literal: quotes with \-escapes.
CSTR = r'"((?:[^"\\]|\\.)*)"'
ITEM_RE = re.compile(r'\{\s*' + CSTR + r'\s*,\s*' + CSTR + r'\s*\}', re.S)
ARRAY_RE = re.compile(r'static\s+const\s+HelpItem\s+(\w+)\s*\[\]\s*=\s*\{(.*?)\n\};', re.S)
CAT_RE = re.compile(r'HELP_CAT\(\s*' + CSTR + r'\s*,\s*(\w+)\s*\)')
STRLIST_RE = re.compile(r'static\s+const\s+char\s*\*\s*(\w+)\s*\[\]\s*=\s*\{(.*?)\};', re.S)


def unescape(s):
    return s.replace('\\"', '"').replace('\\\\', '\\')


def prettify(s):
    return s.replace(' -- ', ' — ')   # badge font is ASCII; web can use a real em-dash


def strings_in(body):
    return [prettify(unescape(m.group(1))) for m in re.finditer(CSTR, body)]


def parse_arrays(src):
    """arr_name -> list of (title, text, res34rch_only)."""
    arrays = {}
    for m in ARRAY_RE.finditer(src):
        name, body = m.group(1), m.group(2)
        res_spans, depth, start = [], 0, None
        for mm in re.finditer(r'#\s*(ifdef\s+CLIPBOY_RES34RCH|if|ifdef|ifndef|endif)\b', body):
            tok = mm.group(1)
            if tok.startswith('ifdef CLIPBOY_RES34RCH'):
                if depth == 0:
                    start = mm.start()
                depth = 1
            elif tok.startswith(('if', 'ifdef', 'ifndef')):
                if depth:
                    depth += 1
            elif tok == 'endif' and depth:
                depth -= 1
                if depth == 0 and start is not None:
                    res_spans.append((start, mm.end()))
                    start = None
        items = []
        for im in ITEM_RE.finditer(body):
            res_only = any(a <= im.start() < b for a, b in res_spans)
            items.append((prettify(unescape(im.group(1))),
                          prettify(unescape(im.group(2))), res_only))
        arrays[name] = items
    return arrays


def parse_categories(src):
    m = re.search(r'static\s+const\s+HelpCategory\s+help_categories\s*\[\]\s*=\s*\{(.*?)\n\};', src, re.S)
    block = m.group(1) if m else src
    return [(unescape(cm.group(1)), cm.group(2)) for cm in CAT_RE.finditer(block)]


def parse_credits(src):
    """Pull the credit lists straight out of show_credits()."""
    a = src.find("static void show_credits")
    b = src.find("static void show_legal", a if a >= 0 else 0)
    body = src[a:b] if a >= 0 and b > a else src
    lists = {name: strings_in(arr) for name, arr in
             ((m.group(1), m.group(2)) for m in STRLIST_RE.finditer(body))}
    personal = [prettify(unescape(m.group(1)))
                for m in re.finditer(r'info_add_bullet\(\s*cont\s*,\s*' + CSTR, body)]
    groups = []
    if personal:
        groups.append(("Thanks", personal))
    if lists.get("founding_backers"):
        groups.append(("Founding Backers", lists["founding_backers"]))
    if lists.get("tech"):
        groups.append(("Technical Credits", lists["tech"]))
    if lists.get("special"):
        groups.append(("Special Mentions", lists["special"]))
    return groups


def _brace_blocks(body):
    """Yield the inner text of each top-level { ... } block, string-aware."""
    depth = 0; start = None; i = 0; n = len(body); instr = False
    while i < n:
        c = body[i]
        if instr:
            if c == '\\':
                i += 2; continue
            if c == '"':
                instr = False
        elif c == '"':
            instr = True
        elif c == '{':
            if depth == 0:
                start = i + 1
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                yield body[start:i]
        i += 1


def _top_commas(s):
    """Split s on commas that are at brace depth 0 and outside strings."""
    out = []; depth = 0; instr = False; last = 0; i = 0
    while i < len(s):
        c = s[i]
        if instr:
            if c == '\\':
                i += 2; continue
            if c == '"':
                instr = False
        elif c == '"':
            instr = True
        elif c in '{[':
            depth += 1
        elif c in '}]':
            depth -= 1
        elif c == ',' and depth == 0:
            out.append(s[last:i]); last = i + 1
        i += 1
    out.append(s[last:])
    return out


def _field(f):
    f = f.strip()
    if f == 'NULL' or not f:
        return None
    lits = re.findall(CSTR, f)
    return prettify(unescape(''.join(lits))) if lits else None


def parse_tool_info(src):
    """(cat,item) -> {what, requires, effects, avoid}."""
    m = re.search(r'tool_info_table\s*\[\]\s*=\s*\{(.*)\n\};', src, re.S)
    if not m:
        return {}
    info = {}
    for e in _brace_blocks(m.group(1)):
        parts = _top_commas(e)
        if len(parts) < 3:
            continue
        try:
            cat, item = int(parts[0]), int(parts[1])
        except ValueError:
            continue
        # parts[2:] rejoined is the "{ what, requires, effects, avoid }" block
        inner = ",".join(parts[2:])
        bm = re.search(r'\{(.*)\}', inner, re.S)
        fields = _top_commas(bm.group(1)) if bm else []
        vals = [_field(x) for x in fields][:4] + [None] * 4
        info[(cat, item)] = dict(zip(("what", "requires", "effects", "avoid"), vals[:4]))
    return info


def parse_tools(src, tool_info_src):
    """Ordered categories with items joined to tool_info; res_only per gate."""
    info = parse_tool_info(tool_info_src)
    # cat arrays -> [(name, desc)]
    arrays = {}
    for m in re.finditer(r'static\s+const\s+ToolItem\s+(\w+)\s*\[\]\s*=\s*\{(.*?)\n\};', src, re.S):
        items = []
        for e in _brace_blocks(m.group(2)):
            parts = _top_commas(e)
            lits = re.findall(CSTR, parts[0]) if parts else []
            name = prettify(unescape("".join(lits))) if lits else None
            desc = _field(parts[1]) if len(parts) > 1 else None
            if name:
                items.append((name, desc))
        arrays[m.group(1)] = items
    # tool_categories[] order + which are behind CLIPBOY_RES34RCH
    tm = re.search(r'static\s+const\s+ToolCategory\s+tool_categories\s*\[\]\s*=\s*\{(.*?)\n\};', src, re.S)
    block = tm.group(1) if tm else ""
    res_from = block.find("CLIPBOY_RES34RCH")
    cats = []
    for m in re.finditer(r'\{\s*(\d+)\s*,\s*' + CSTR + r'\s*,\s*(\w+)\s*,', block):
        cid, name, arr = int(m.group(1)), unescape(m.group(2)), m.group(3)
        res_only = res_from != -1 and m.start() > res_from
        items = []
        for idx, (nm, desc) in enumerate(arrays.get(arr, [])):
            items.append({"name": nm, "desc": desc, "res_only": res_only,
                          "info": info.get((cid, idx))})
        cats.append({"name": name, "res_only": res_only, "items": items})
    return cats


def git(*args, default=""):
    try:
        return subprocess.check_output(["git", "-C", ROOT, *args],
                                       stderr=subprocess.DEVNULL).decode().strip()
    except Exception:
        return default


def esc(s):
    return html.escape(s, quote=True)


def render(cats, arrays, credit_groups, tools):
    commit = git("rev-parse", "--short", "HEAD", default="unknown")
    # Derive the "generated" date from the COMMIT date (not wall-clock now()), so the
    # whole page is a pure, byte-reproducible function of (ui_nav.h, tool_info.h, HEAD).
    # That makes the build-chain "regenerate + push" safe as a wipe-and-replace: any
    # diff vs what's deployed is a real content change, never a timestamp or hand-edit.
    built = git("show", "-s", "--format=%cs", "HEAD", default="unknown")

    # ---- build a unified section list: help categories + Credits + Legal ----
    sections = []  # (id, title, inner_html)

    # Photographic scan aids -- the one thing the 320x240 screen can't show.
    scan_guide = (
        '        <li class="item">\n'
        '          <div class="q">Scan, Visually</div>\n'
        f'          <figure class="fig"><img src="{RAW}/img/scan-distance.jpg" loading="lazy" alt="Trilancer strap spanning the 3-inch gap between sensor and tag"><figcaption>Distance: the "Trilancer" wrist strap is exactly 3"/8 cm -- the ideal gap between the sensor and the tag.</figcaption></figure>\n'
        f'          <figure class="fig"><img src="{RAW}/img/scan-parallel.jpg" loading="lazy" alt="Sensor face and tag face on a red line marked Parallel"><figcaption>Parallel: keep the sensor face flat to the tag\'s face. You can scan at any angle -- what matters is the two faces staying parallel, not level to the ground.</figcaption></figure>\n'
        f'          <figure class="fig"><img src="{RAW}/img/scan-alignment.png" loading="lazy" alt="On-screen scan legend aligned to the tag corners"><figcaption>Alignment: line the three on-screen corner markers up with the tag\'s three corners (Top Left, Top Right, Bottom Right).</figcaption></figure>\n'
        '        </li>')

    for name, arr in cats:
        items = arrays.get(arr, [])
        if not items:
            continue
        rows = [scan_guide] if name == "Collectibles" else []
        for title, text, res_only in items:
            m = re.match(r'^--\s*(.*?)\s*--$', title)
            if m:
                rows.append(f'        <li class="divider">{esc(m.group(1))}</li>')
                continue
            tag = '<span class="tag">Res34rch-only</span>' if res_only else ''
            rows.append('        <li class="item">\n'
                        f'          <div class="q">{esc(title)}{tag}</div>\n'
                        f'          <div class="a">{esc(text)}</div>\n'
                        '        </li>')
        sid = "cat-" + re.sub(r'[^a-z0-9]+', '-', name.lower()).strip('-')
        sections.append((sid, name, "\n".join(rows)))

    # Tool Reference (from tool_categories[] + tool_info_table[])
    if tools:
        rows = ['        <li class="item"><div class="a">Each tool also has an on-badge '
                '"More Info" panel; this mirrors it. Tools under the Res34rch-only categories '
                'are absent from the listen-only Sn34k-Boy build. For the serial command line, see the '
                '<a href="https://github.com/justcallmekoko/ESP32Marauder/wiki/cli">ESP32 Marauder CLI wiki</a>.</div></li>']
        for cat in tools:
            dtag = " (Res34rch-only)" if cat["res_only"] else ""
            rows.append(f'        <li class="divider">{esc(cat["name"])}{dtag}</li>')
            for it in cat["items"]:
                tag = '<span class="tag">Res34rch-only</span>' if it["res_only"] else ''
                a = []
                if it["desc"]:
                    a.append(f'          <div class="a">{esc(it["desc"])}</div>')
                info = it["info"] or {}
                for lbl, key in (("What", "what"), ("Setup", "requires"),
                                 ("You'll see", "effects"), ("Use responsibly", "avoid")):
                    if info.get(key):
                        a.append(f'          <div class="tif"><b>{lbl}:</b> {esc(info[key])}</div>')
                rows.append('        <li class="item">\n'
                            f'          <div class="q">{esc(it["name"])}{tag}</div>\n'
                            + "\n".join(a) + '\n        </li>')
        sections.append(("sec-tools", "Tool Reference", "\n".join(rows)))

    # Credits (from show_credits)
    if credit_groups:
        rows = []
        for gname, names in credit_groups:
            rows.append(f'        <li class="divider">{esc(gname)}</li>')
            for n in names:
                rows.append(f'        <li class="item"><div class="a">{esc(n)}</div></li>')
        sections.append(("sec-credits", "Credits", "\n".join(rows)))

    # Legal & Licensing (summary + links to canonical docs -- no duplication)
    legal_rows = f"""        <li class="item"><div class="q">The short version</div>
          <div class="a">Use these tools only on hardware you own or are explicitly authorized to test. You are responsible for how you use the badge.</div></li>
        <li class="item"><div class="q">Passive isn't automatically harmless</div>
          <div class="a">Capturing handshakes or packets can be interception under wiretap law even without transmitting. Know the rules where you are.</div></li>
        <li class="item"><div class="q">Parody &amp; no affiliation</div>
          <div class="a">Collectibles and SAOs are fan-made parody. Properties belong to their respective owners; Clip-Boy is not affiliated with or endorsed by any of them.</div></li>
        <li class="item"><div class="q">Radios &amp; RF</div>
          <div class="a">This device complies with FCC Part 15. Users outside the US are responsible for local RF regulations. The "Whether Radio" is simulated -- pre-recorded audio, no receiver.</div></li>
        <li class="item"><div class="q">AI disclosure</div>
          <div class="a">Some art and the SegFault-Tec FM audio are AI-generated under commercial licenses; synthetic voices are original, never clones of real people. Details in the on-device Legal screen and the repo.</div></li>
        <li class="item"><div class="q">License &amp; source</div>
          <div class="a">Clip-Boy firmware is open source under GPLv3. Read it, fork it, contribute back.</div></li>
        <li class="divider">Full documents</li>
        <li class="item"><div class="a"><a href="{BLOB}/acceptable_use.md">Acceptable Use notice</a> &middot; <a href="{BLOB}/SECURITY.md">Security policy</a> &middot; <a href="{BLOB}/LICENSE">License (GPLv3)</a> &middot; <a href="{REPO}">Source code</a></div></li>"""
    sections.append(("sec-legal", "Legal &amp; Licensing", legal_rows))

    toc = "\n".join(f'      <a class="chip" href="#{sid}">{title}</a>'
                    for sid, title, _ in sections)
    details = "\n".join(
        f'    <details class="cat" id="{sid}"{" open" if i == 0 else ""}>\n'
        f'      <summary>{title}</summary>\n      <ul>\n{inner}\n      </ul>\n    </details>'
        for i, (sid, title, inner) in enumerate(sections))

    return f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="color-scheme" content="dark light">
<title>Clip-Boy Help</title>
<style>
  :root {{
    --bg:#0b0d0a; --panel:#12160f; --panel2:#171c12; --line:#2c3a22;
    --amber:#ffb642; --amber-dim:#c98f34; --green:#7ee081; --text:#e8dcc0;
    --muted:#9a9070; --tagbg:#3a2a12; --tagfg:#ffcf7a; --hh:124px;
  }}
  * {{ box-sizing:border-box; }}
  html {{ -webkit-text-size-adjust:100%; scroll-behavior:smooth; }}
  body {{
    margin:0; background:var(--bg); color:var(--text);
    font:16px/1.55 ui-monospace,"Cascadia Mono",Consolas,"DejaVu Sans Mono",monospace;
    padding-bottom:env(safe-area-inset-bottom);
  }}
  a {{ color:var(--amber); }}
  header {{
    position:sticky; top:0; z-index:5; background:linear-gradient(#0b0d0aee,#0b0d0acc);
    backdrop-filter:blur(4px); border-bottom:1px solid var(--line);
    padding:14px 16px 10px; padding-top:calc(14px + env(safe-area-inset-top));
  }}
  h1 {{ margin:0; font-size:20px; letter-spacing:1px; color:var(--amber);
        text-shadow:0 0 8px #ffb64233; }}
  .sub {{ margin:2px 0 10px; color:var(--muted); font-size:12.5px; }}
  #q {{
    width:100%; padding:11px 12px; font:inherit; color:var(--text);
    background:var(--panel); border:1px solid var(--line); border-radius:8px;
  }}
  #q::placeholder {{ color:var(--muted); }}
  #q:focus {{ outline:none; border-color:var(--amber-dim); box-shadow:0 0 0 2px #ffb64222; }}
  .toc {{ display:flex; flex-wrap:wrap; gap:7px; padding:12px 16px 4px; }}
  .chip {{
    font-size:12.5px; text-decoration:none; color:var(--amber-dim);
    border:1px solid var(--line); border-radius:999px; padding:5px 11px; white-space:nowrap;
  }}
  .chip:active {{ background:var(--panel2); }}
  main {{ max-width:820px; margin:0 auto; padding:6px 12px 40px; }}
  details.cat {{
    background:var(--panel); border:1px solid var(--line); border-radius:10px;
    margin:10px 0; overflow:hidden; scroll-margin-top:var(--hh);
  }}
  summary {{
    cursor:pointer; list-style:none; padding:14px 16px; font-size:16.5px;
    color:var(--amber); font-weight:600; letter-spacing:.4px;
    display:flex; align-items:center; gap:10px; user-select:none;
  }}
  summary::-webkit-details-marker {{ display:none; }}
  summary::before {{ content:"\\25B8"; color:var(--amber-dim); transition:transform .15s; }}
  details[open] > summary::before {{ transform:rotate(90deg); }}
  ul {{ list-style:none; margin:0; padding:0 12px 8px; }}
  li.item {{ padding:11px 4px; border-top:1px solid #ffffff0d; }}
  .q {{ color:var(--green); font-weight:600; margin-bottom:3px; }}
  .a {{ color:var(--text); }}
  .tif {{ color:var(--text); margin-top:4px; font-size:14px; }}
  .tif b {{ color:var(--amber-dim); font-weight:600; }}
  .fig {{ margin:10px 0 14px; }}
  .fig img {{ width:100%; max-width:520px; display:block; border:1px solid var(--line); border-radius:8px; }}
  figcaption {{ color:var(--muted); font-size:13px; margin-top:5px; max-width:520px; }}
  li.divider {{
    margin:12px 4px 2px; padding:5px 8px; color:var(--amber-dim); font-size:12px;
    letter-spacing:2px; text-transform:uppercase; border-left:2px solid var(--amber-dim);
    background:#ffb6420a;
  }}
  .tag {{
    font-size:10.5px; letter-spacing:.5px; color:var(--tagfg); background:var(--tagbg);
    border:1px solid #6a4a1a; border-radius:5px; padding:1px 6px; margin-left:8px;
    vertical-align:middle; white-space:nowrap;
  }}
  #noresults {{ display:none; color:var(--muted); text-align:center; padding:30px 10px; }}
  footer {{
    max-width:820px; margin:0 auto; padding:18px 16px 40px; color:var(--muted);
    font-size:12px; border-top:1px solid var(--line);
  }}
  @media (max-width:480px) {{ body {{ font-size:15px; }} h1 {{ font-size:18px; }} }}
</style>
</head>
<body>
<header>
  <h1>CLIP-BOY &middot; HELP</h1>
  <div class="sub">The same in-badge Help, easier to read on a phone. Tap a section, or search.</div>
  <input id="q" type="search" inputmode="search" autocomplete="off"
         placeholder="Search help (e.g. scan, deauth, battery)&hellip;" aria-label="Search help">
</header>
<nav class="toc">
{toc}
</nav>
<main>
{details}
  <div id="noresults">No help topics match that search.</div>
</main>
<footer>
  Generated from the Clip-Boy firmware Help corpus (commit <code>{commit}</code>, {built}).
  Source: <a href="{REPO}">github.com/SafeHazard/Clip-Boy</a>.
  This mirrors what's on the badge under DATA &middot; Settings &middot; Help.
</footer>
<script>
(function(){{
  var q = document.getElementById('q');
  var cats = Array.prototype.slice.call(document.querySelectorAll('details.cat'));
  var none = document.getElementById('noresults');
  var savedOpen = null;
  // Match the anchor-scroll offset to the ACTUAL sticky-header height (varies with
  // font size / viewport / a wrapped title) so a pill lands its section just below
  // the header instead of under it. scroll-margin-top: var(--hh) does the rest.
  var header = document.querySelector('header');
  function setHH(){{ document.documentElement.style.setProperty('--hh', (header.offsetHeight + 10) + 'px'); }}
  setHH(); window.addEventListener('resize', setHH);                 // pre-search open state, restored on clear

  function searching(){{ return q.value.trim() !== ''; }}
  // Accordion: opening one section closes the others (skipped during search).
  cats.forEach(function(c){{
    c.addEventListener('toggle', function(){{
      if(!c.open || searching()) return;
      cats.forEach(function(o){{ if(o !== c) o.open = false; }});
    }});
  }});
  // TOC pills: open the target section, then let the browser's NATIVE #hash jump
  // scroll to it -- crucially NO preventDefault. Native anchor scrolling works even
  // where programmatic JS scroll is blocked (sandboxed preview iframes); calling
  // preventDefault + a JS scroll was the "click and nothing happens" bug.
  // scroll-margin-top on details.cat offsets the sticky header for the landing.
  document.querySelectorAll('.chip').forEach(function(a){{
    a.addEventListener('click', function(){{
      var c = document.getElementById(a.getAttribute('href').slice(1));
      if(!c) return;
      cats.forEach(function(o){{ if(o !== c) o.open = false; }});
      c.open = true;
    }});
  }});

  function run(){{
    var term = q.value.trim().toLowerCase();
    if(!term){{
      if(savedOpen){{ cats.forEach(function(c,i){{ c.open = savedOpen[i]; }}); savedOpen = null; }}
      cats.forEach(function(c){{
        c.style.display = '';
        c.querySelectorAll('li.item,li.divider').forEach(function(li){{ li.style.display=''; }});
      }});
      none.style.display = 'none';
      return;
    }}
    if(!savedOpen){{ savedOpen = cats.map(function(c){{ return c.open; }}); }}
    var anyCat = false;
    cats.forEach(function(c){{
      var anyItem = false;
      c.querySelectorAll('li.item').forEach(function(li){{
        var hit = li.textContent.toLowerCase().indexOf(term) !== -1;
        li.style.display = hit ? '' : 'none';
        if(hit) anyItem = true;
      }});
      c.querySelectorAll('li.divider').forEach(function(d){{ d.style.display = anyItem ? '' : 'none'; }});
      c.style.display = anyItem ? '' : 'none';
      c.open = anyItem;
      if(anyItem) anyCat = true;
    }});
    none.style.display = anyCat ? 'none' : '';
  }}
  q.addEventListener('input', run);
}})();
</script>
</body>
</html>
"""


def main():
    out = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_OUT
    with open(UI_NAV, encoding="utf-8", errors="replace") as f:
        src = f.read()
    with open(TOOL_INFO, encoding="utf-8", errors="replace") as f:
        tool_info_src = f.read()
    arrays = parse_arrays(src)
    cats = parse_categories(src)
    credit_groups = parse_credits(src)
    tools = parse_tools(src, tool_info_src)
    n_items = sum(len(arrays.get(a, [])) for _, a in cats)
    n_credits = sum(len(g) for _, g in credit_groups)
    n_tools = sum(len(c["items"]) for c in tools)
    doc = render(cats, arrays, credit_groups, tools)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(doc)
    print(f"gen_help_site: {len(cats)} help cats / {n_items} items + "
          f"{n_tools} tools / {len(tools)} cats + {len(credit_groups)} credit groups / "
          f"{n_credits} names + Legal -> {out}")


if __name__ == "__main__":
    main()
