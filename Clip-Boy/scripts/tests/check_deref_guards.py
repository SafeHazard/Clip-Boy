#!/usr/bin/env py -3
"""check_deref_guards.py -- enumeration gate for the FB9 defect class.

FB9: `LinkedList<T>::get(i)` returns a value-initialised `T()` when its node lookup misses,
and the node cache (`isCached`/`lastNodeGot`) is SHARED ACROSS TASKS with no synchronisation.
So a torn read hands back an `AccessPoint` whose `stations` member is nullptr. `LinkedList::add`
is virtual, which makes `ap.stations->add(...)` a vtable load from address 0 -- LoadProhibited on
the WiFi task, presenting as a random reboot mid-scan.

WHY THIS EXISTS AS A GATE and not as a note: the class was "fixed" three times and each time an
unvisited sibling turned up -- guards added at 2 sites in clearAPs() while the same shape one
function over went unguarded, then two more that were reachable only from the serial CLI. The
rule earned from that is: terminate by ENUMERATION, not by running out of attention. That means
naming the mechanical criterion for the complete candidate set and running it every build, rather
than trusting that a past sweep was exhaustive.

CRITERION (the complete candidate set): every textual dereference `<expr>.stations->` in the
vendored ClipBoy sources. Sites that hoist the inner list into a local pointer first
(`LinkedList<uint16_t>* st = ap.stations; if (st) ...`) do not match, and do not need to -- the
hoist itself is the guard, and `st` is checked before use.

POLICY: fail-closed. A site counts as guarded only if this script can POSITIVELY prove it, either
from the same line (ternary / `&&`) or from an enclosing `if (!x.stations) return|continue|break;`
within LOOKBACK lines. Anything it cannot prove must be listed in ALLOWLIST with a reason. A
conservative gate that demands an explicit entry is safe; a clever gate that guesses "probably
fine" fails in the PASSING direction, which is the dangerous one for a gate.

Stale ALLOWLIST entries are also a failure -- an allowlist that silently keeps passing for code
that no longer exists is how a real site sneaks in behind a stale exemption.

  py -3 Clip-Boy/scripts/tests/check_deref_guards.py            # gate
  py -3 Clip-Boy/scripts/tests/check_deref_guards.py --verbose  # show every site + verdict
  py -3 Clip-Boy/scripts/tests/check_deref_guards.py --selftest # prove it can read POSITIVE
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.normpath(os.path.join(HERE, "..", "..", "libs", "ClipBoy", "src"))

SCAN_FILES = ["WiFiScan.cpp", "CommandLine.cpp", "ClipBoyMarauder.cpp", "EvilPortal.cpp"]

# How far back to look for an enclosing guard. Deliberately small: a guard 30 lines up is
# probably in a different block, and accepting it would be exactly the kind of guess that makes
# a gate lie.
LOOKBACK = 14

# Sites that are NOT guarded but are known-safe, each with the reason it was cleared.
# `snippet` must still appear on the cited line or the entry is STALE and the gate fails.
ALLOWLIST = [
    # (file, snippet, reason)
    # Currently EMPTY, and that is the goal state: every real site is provably guarded, and the
    # two entries this list started with turned out to be comment text rather than code (a
    # cross-reference note and a commented-out upstream line), so the right fix was to stop
    # counting comments as derefs -- not to grant them exemptions.
]

# Match ANY `.stations` access, then work out the base ourselves. An earlier version required
# the base to be a BARE IDENTIFIER:
#     re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\s*\.\s*stations\s*->")
# which cannot match `access_points->get(i).stations->add(sta)` -- the character before `.stations`
# is `)`. That is *the exact form FB9 had before the fix*, and it is still in the tree as the
# commented-out original at WiFiScan.cpp:8967. So the gate billed as "the enumeration gate for
# the FB9 class" was blind to the shape of the bug it was written for, and an injected site in
# that form scored `OK -- every site is guarded`. It also missed `aps[i].stations->`,
# `delete access_points->get(i).stations;` (the wild-free half of the class, which contains no
# `->` at all), and every non-identifier base.
#
# The criterion is now: every `.stations` access in the vendored sources, whatever the base.
_DOT_STATIONS = re.compile(r"\.\s*stations\s*(->|;)")


def _base_before(line, dot_pos):
    """The expression `.stations` hangs off, or None if it is a TEMPORARY.

    A temporary (`get(i).stations`, `f().stations`, `aps[i].stations`) is the dangerous case and
    is UNGUARDABLE BY CONSTRUCTION: there is no name to test, and `LinkedList::get()` returns a
    value-initialised copy whose `stations` is nullptr on a node-cache miss. So "no simple base"
    is not an inconclusive reading -- it is a finding.
    """
    j = dot_pos - 1
    while j >= 0 and line[j].isspace():
        j -= 1
    if j < 0:
        return None
    if line[j] in ")]":
        return None
    k = j
    while k >= 0 and (line[k].isalnum() or line[k] == "_"):
        k -= 1
    ident = line[k + 1:j + 1]
    return ident or None


def _same_line_guarded(line, base, kind="->"):
    """A ternary or short-circuit that tests <base>.stations before dereferencing it."""
    guard = re.escape(base) + r"\s*\.\s*stations\s*(?:\?|&&|!=\s*(?:NULL|nullptr|0))"
    if not re.search(guard, line):
        return False
    # The guard must come BEFORE the dereference on the line, or it guards nothing.
    g = re.search(guard, line)
    suffix = r"->" if kind == "->" else r";"
    d = re.search(re.escape(base) + r"\s*\.\s*stations\s*" + suffix, line)
    return bool(g and d and g.start() < d.start())


_BAIL_RE = re.compile(r"^\s*(?:return|continue|break|goto)\b")


def _lookback_guarded(lines, idx, base):
    """An early-out guard above the deref: `if (!x.stations)` whose body bails out.

    Accepts the one-line form, the two-line form, and the BRACED form whose bail is separated
    from the `if` by a comment block -- which is how both real sniffer guards are written (a
    9-line comment explaining the torn read sits between `if (!ap.stations) {` and `return;`).
    An earlier version of this function only looked one line past the `if` and reported both
    real guards as UNGUARDED. It failed in the SAFE direction, but a gate that cries wolf on
    correct code gets its findings dismissed, which is its own failure mode.
    """
    bail_inline = re.compile(
        r"if\s*\(\s*!\s*" + re.escape(base) + r"\s*\.\s*stations\s*\)\s*"
        r"(?:\{)?\s*(?:return|continue|break|goto)")
    open_if = re.compile(
        r"if\s*\(\s*!\s*" + re.escape(base) + r"\s*\.\s*stations\s*\)\s*\{?\s*$")
    for j in range(idx - 1, max(-1, idx - 1 - LOOKBACK), -1):
        if bail_inline.search(lines[j]):
            return True
        if open_if.search(lines[j].rstrip()):
            # Walk forward past comments/blank lines to the first real statement. If that
            # statement bails, the guard is real regardless of how much prose sits between.
            for k in range(j + 1, min(len(lines), j + 1 + LOOKBACK)):
                s = lines[k].strip()
                if not s or s.startswith("//") or s.startswith("/*") or s.startswith("*"):
                    continue
                return bool(_BAIL_RE.match(lines[k]))
    return False


def _strip_comments(text):
    """Blank out // and /* */ comment CONTENT, preserving line structure and offsets.

    Without this, prose that merely NAMES a deref registers as one: a cross-reference comment
    in ClipBoyMarauder.cpp and a commented-out upstream line both showed up as UNGUARDED
    findings. Dead code and commented code read as coverage in both directions -- here they
    read as defects, and an allowlist entry to excuse them would have been an exemption for
    something that does not execute.
    """
    out, i, n = [], 0, len(text)
    in_line = in_block = in_str = in_chr = False
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if in_line:
            if c == "\n":
                in_line = False
                out.append(c)
            else:
                out.append(" ")
        elif in_block:
            if c == "*" and nxt == "/":
                in_block = False
                out.append("  ")
                i += 2
                continue
            out.append("\n" if c == "\n" else " ")
        elif in_str or in_chr:
            out.append(c)
            if c == "\\" and nxt:
                out.append(nxt)
                i += 2
                continue
            if (in_str and c == '"') or (in_chr and c == "'"):
                in_str = in_chr = False
        else:
            if c == "/" and nxt == "/":
                in_line = True
                out.append("  ")
                i += 2
                continue
            if c == "/" and nxt == "*":
                in_block = True
                out.append("  ")
                i += 2
                continue
            if c == '"':
                in_str = True
            elif c == "'":
                in_chr = True
            out.append(c)
        i += 1
    return "".join(out)


def scan_text(name, text):
    """Return [(line_no, base, line, verdict)] for every .stations-> deref in `text`."""
    lines = _strip_comments(text).splitlines()
    raw = text.splitlines()          # report the real line, match on the stripped one
    out = []
    for i, line in enumerate(lines):
        for m in _DOT_STATIONS.finditer(line):
            kind = m.group(1)
            # `.stations;` is only a free when it is being deleted. A bare `x.stations;`
            # statement elsewhere is a no-op, not a member access worth gating.
            if kind == ";" and not re.search(r"\bdelete\b", line):
                continue
            base = _base_before(line, m.start())
            if base is None:
                # Unguardable by construction -- see _base_before(). Always a finding.
                verdict = "UNGUARDED (temporary)"
                base = "<temporary>"
            elif _same_line_guarded(line, base, kind):
                verdict = "guarded (same line)"
            elif _lookback_guarded(lines, i, base):
                verdict = "guarded (early-out above)"
            else:
                verdict = "UNGUARDED"
            shown = raw[i].strip() if i < len(raw) else line.strip()
            out.append((i + 1, base, shown, verdict))
    return out


def selftest():
    """Prove the gate can read BOTH ways on the same run. A checker that only ever says
    "clean" reports success forever -- the failure mode that made an earlier teardown test
    vacuous. Known-bad must FAIL and known-good must PASS, or the gate is not trustworthy."""
    bad = "void f() {\n  for (int i=0;i<n;i++) {\n    x += ap.stations->size();\n  }\n}\n"
    good_tern = "void f() {\n  int n = (ap.stations ? ap.stations->size() : 0);\n}\n"
    good_bail = ("void f() {\n  AccessPoint ap = l->get(x);\n"
                 "  if (!ap.stations) continue;\n  int n = ap.stations->size();\n}\n")
    # The shape both real sniffer guards use: a braced if whose bail sits below a comment block.
    good_braced = ("void f() {\n  if (!ap.stations) {\n    // a long explanation of the torn\n"
                   "    // read, several lines of it\n    return;\n  }\n"
                   "  ap.stations->add(1);\n}\n")
    # Comment text that merely NAMES a deref must not be counted as one.
    comment_only = "void f() {\n  //     ap.stations->add() at ~:8829).\n}\n"
    # ── the two shapes an identifier-only base was BLIND to ──────────────────────────────
    # This is FB9's actual pre-fix form. It must be a finding, and it can never be "guarded":
    # the value is a temporary, so there is no name to test.
    temp_call = "void f() {\n  access_points->get(i).stations->add(sta);\n}\n"
    # The wild-free half of the same class. Contains no `->` at all, so the old pattern could
    # not see it even in principle.
    temp_delete = "void f() {\n  delete access_points->get(i).stations;\n}\n"
    # Subscript base -- same hazard, different syntax.
    temp_index = "void f() {\n  x += aps[i].stations->size();\n}\n"
    # A guarded delete through a named local IS fine, so the delete shape must not be a
    # blanket finding -- otherwise the gate cries wolf on the correct fix.
    good_delete = ("void f() {\n  AccessPoint ap = l->get(x);\n"
                   "  if (!ap.stations) return;\n  delete ap.stations;\n}\n")
    cases = [("known-bad (unguarded)", bad, "UNGUARDED"),
             ("known-good (ternary)", good_tern, "guarded (same line)"),
             ("known-good (early-out)", good_bail, "guarded (early-out above)"),
             ("known-good (braced + comment block)", good_braced, "guarded (early-out above)"),
             ("comment-only mention -> no site at all", comment_only, None),
             ("FB9's real pre-fix form: get(i).stations->", temp_call, "UNGUARDED (temporary)"),
             ("wild free: delete get(i).stations;", temp_delete, "UNGUARDED (temporary)"),
             ("subscript base: aps[i].stations->", temp_index, "UNGUARDED (temporary)"),
             ("guarded delete through a named local", good_delete,
              "guarded (early-out above)")]
    ok = True
    for label, text, want in cases:
        hits = scan_text("selftest", text)
        got = [h[3] for h in hits]
        if want is None:
            passed = not hits
            print(f"  [{'ok' if passed else 'FAIL'}] {label}: want no sites, got {got!r}")
            ok = ok and passed
            continue
        # The ternary case contains TWO textual matches (guard + deref) only if the regex is
        # loose; assert on the DEREF verdict, which is the last hit on the line.
        passed = bool(got) and got[-1] == want
        print(f"  [{'ok' if passed else 'FAIL'}] {label}: want {want!r}, got {got!r}")
        ok = ok and passed
    print("selftest:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    if not os.path.isdir(SRC):
        print(f"check_deref_guards: FAIL -- cannot find {SRC}")
        return 2   # a gate that cannot find its input must not report clean

    unguarded, total, allow_used = [], 0, set()
    for name in SCAN_FILES:
        path = os.path.join(SRC, name)
        if not os.path.isfile(path):
            print(f"check_deref_guards: FAIL -- missing source {name}")
            return 2
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
        for line_no, base, line, verdict in scan_text(name, text):
            total += 1
            if verdict == "UNGUARDED":
                excused = None
                for a_name, a_snip, a_reason in ALLOWLIST:
                    if a_name == name and a_snip in line:
                        excused = (a_name, a_snip, a_reason)
                        break
                if excused:
                    allow_used.add(excused[:2])
                    verdict = f"allowlisted ({excused[2]})"
                else:
                    unguarded.append((name, line_no, base, line))
            if args.verbose:
                print(f"  {name}:{line_no}  [{verdict}]  {line[:96]}")

    stale = [(n, s) for (n, s, _r) in ALLOWLIST if (n, s) not in allow_used]

    print(f"check_deref_guards: {total} `.stations->` deref site(s) considered "
          f"in {len(SCAN_FILES)} file(s)")
    for name, line_no, base, line in unguarded:
        print(f"  UNGUARDED  {name}:{line_no}  `{base}.stations->`  {line[:96]}")
    for name, snip in stale:
        print(f"  STALE ALLOWLIST  {name}: {snip!r} no longer matches any site -- "
              f"remove it (a stale exemption is how a real site slips through)")

    if unguarded or stale:
        print("check_deref_guards: FAIL")
        return 1
    print("check_deref_guards: OK -- every site is guarded or explicitly cleared")
    return 0


if __name__ == "__main__":
    sys.exit(main())
