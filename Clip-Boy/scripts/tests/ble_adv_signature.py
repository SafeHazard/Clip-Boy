#!/usr/bin/env python3
"""ble_adv_signature.py -- parse a btmon .snoop into {distinguishing-key: count}.

Deployed to the kali witness and run there (it needs `sudo btmon -r`). Prints one
"KEY\tCOUNT" line per distinguishing advertising-data key, counting ONLY adverts from
NON-RESOLVABLE addresses.

WHY THIS SHAPE (measured 2026-07-29, all five BLE Spam types on a real badge):
  * Each spam type floods ONE distinguishing key, and it is NOT always a manufacturer
    company -- Google Fast Pair spam floods Service Data UUID 0xfe2c, Flipper floods the
    "not assigned" company 0x8801. A company-only parser would score Google/Flipper as
    silent. So the key is Company OR Service-Data UUID.
  * The NON-RESOLVABLE address filter is what makes it ambient-immune. Real modern phones
    advertise Apple/Google continuity from RESOLVABLE-private addresses; the ESP32 spam
    uses public/static ones. Without the filter, Sour Apple reads 187 vs 129 ambient Apple
    (1.45x -- FAILS); with it, 89 vs 9 (10x). The other four are naturally low-ambient, so
    the filter is essential for Apple and harmless for the rest.

The caller diffs ON vs OFF and finds the key with the biggest spike -- it does NOT hardcode
which key each item should hit. That discovery is deliberate: it is what caught Google
being service-data rather than a company, which a hardcoded map would have gotten wrong.
"""
import re, subprocess, sys
from collections import Counter

txt = subprocess.run(["btmon", "-r", sys.argv[1]], capture_output=True, text=True).stdout
blocks, cur = [], []
for line in txt.splitlines():
    if "LE Advertising Report" in line:
        if cur:
            blocks.append("\n".join(cur))
        cur = [line]
    elif cur:
        cur.append(line)
if cur:
    blocks.append("\n".join(cur))

keys = Counter()
for b in blocks:
    at = re.search(r"Address type: (\w+)", b)
    sub = re.search(r"\((Resolvable|Static|Non-Resolvable)\)", b)
    # non-resolvable = Public type, OR a Static / Non-Resolvable random. A Resolvable
    # private address is a real privacy-preserving device and is EXCLUDED.
    nonres = (at and at.group(1) == "Public") or (sub and sub.group(1) in ("Static", "Non-Resolvable"))
    if not nonres:
        continue
    # Continuity TYPE, when btmon decodes one (Apple-specific: "Type: <name> (N)").
    # Appending it to the key is what makes SOUR APPLE separable from ambient Apple:
    # the spam floods ONE continuity type (Nearby Action, 15) that real devices barely
    # use, while ambient Apple spreads across Nearby Info (16) / Handoff (12). MEASURED
    # 2026-07-29: Type-15 was 85 ON vs 0 OFF (with 122 ambient Apple present), where
    # plain "mfg:Apple" was 156 vs 40 = 3.9x and FLAPPED against the 4x gate. Non-Apple
    # companies have no Type line, so their keys are unchanged. The caller DISCOVERS the
    # spiking key, so this only sharpens Apple; it does not perturb the others.
    tm = re.search(r"\n\s+Type: [^(\n]*\((\d+)\)", b)
    tsuf = "/type%s" % tm.group(1) if tm else ""
    for co in re.findall(r"Company: ([A-Za-z][^(\n]*?)\s*(?:\(|$)", b):
        keys["mfg:" + co.strip() + tsuf] += 1
    for uu in re.findall(r"Service Data: ([^\n]+?\(0x[0-9a-f]+\))", b):
        keys["svc:" + uu.strip()] += 1

for key, n in keys.most_common():
    print("%s\t%d" % (key, n))
