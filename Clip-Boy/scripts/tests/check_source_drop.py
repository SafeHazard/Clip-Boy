#!/usr/bin/env py -3
"""check_source_drop.py -- gate: nothing internal may reach the published source drop.

`stage_source.sh` builds the PUBLIC (GPLv3) and KS backer trees with
`git archive <ref> | tar -x`, i.e. EVERY TRACKED FILE, then deletes a short TRIM list.
That is a denylist, so anything new and internal is published by default -- the failure
mode this gate exists to prevent (audit 2026-07-24 FB3).

Three independent layers must all hold; this script checks the outermost one (what
`git archive` actually emits) plus the presence of the other two:
  1. `.gitattributes` export-ignore   -- git archive honors it natively
  2. `stage_source.sh` TRIM           -- explicit rm after extraction
  3. `stage_source.sh` leak gate      -- FAILS the stage if a forbidden path survives

Run:  py -3 Clip-Boy/scripts/tests/check_source_drop.py [--ref HEAD]
Exit: 0 clean, 1 something internal would be published.
"""
import argparse
import fnmatch
import io
import os
import subprocess
import sys
import tarfile

HERE = os.path.dirname(os.path.abspath(__file__))
# This gate checks a property of the REAL source repo at HEAD -- what `git archive HEAD`
# (which stage_source.sh runs to build the published trees) would emit. When make_release_bins.sh
# runs it from a STAGING tree (release_from_staging.sh: `cd $PUB_STAGE/Clip-Boy && make_release_bins`),
# the HERE-relative path lands on the staging OUTPUT dir, which is NOT a git repo -> `git ls-tree HEAD`
# exits 128 and the gate cannot run. release_from_staging.sh exports CLIPBOY_SOURCE_REPO=<real repo>
# so the staged copy still checks the real history. Standalone runs (env unset, from the real repo)
# fall back to the HERE-relative path, unchanged.
REPO = os.environ.get("CLIPBOY_SOURCE_REPO") or os.path.abspath(os.path.join(HERE, "..", "..", ".."))
FW = "Clip-Boy"

# Paths that must NEVER appear in a published source tree, with WHY (printed on failure
# so a future maintainer doesn't "helpfully" delete an entry they don't understand).
FORBIDDEN = [
    (f"{FW}/scripts/production/*",
     "devcon.exe is Microsoft WDK (redistribution is not ours to grant) and "
     "flash_inventory.json carries KS per-tier backer quantities"),
    (f"{FW}/docs/arg-*",
     "full ARG walkthrough -- arg-p3-map.txt literally opens '=== P3 \"DORK\" - FULL MAP ==='; "
     "check_sku_binaries.py already denylists '- FULL MAP' from the BINARIES"),
    (f"{FW}/docs/audit/*",
     "pre-release audit = a vulnerability roadmap with file:line for every open defect"),
    (f"{FW}/docs/security-review-*",
     "security review = vulnerability list plus the accepted-risk rationale"),
    (f"{FW}/docs/superpowers/*",
     "internal plans/specs (schedules, unshipped design intent)"),
    (f"{FW}/secret.h",
     "ARG HMAC key -- must never enter published source (gitignored; belt-and-braces)"),
    ("for_counsel.md",
     "privileged legal correspondence"),
    ("TODO.md",
     "root-level working notes (queues, audit summaries, half-finished plans)"),
    ("TODO.overnight.md",
     "root-level overnight queue -- carries audit findings and open decisions"),
    (f"{FW}/rift_private.h",
     "KS-exclusive Overseer/Space Badge content"),
]

# Layer-2/3 presence checks: substrings that must appear in stage_source.sh.
REQUIRED_IN_STAGE = [
    "$FW/scripts/production",
    "$FW/docs/arg-",          # trimmed by the glob loop + named in the leak gate
    "$FW/docs/audit",
    "$FW/docs/superpowers",
    "$FW/docs/security-review-",
]


def tracked_at(ref):
    """All tracked paths at `ref` (cheap: names only, no blobs)."""
    out = subprocess.run(["git", "-C", REPO, "ls-tree", "-r", "--name-only", ref],
                         capture_output=True, check=True, text=True).stdout
    return [ln.strip() for ln in out.splitlines() if ln.strip()]


def survives_archive(ref, paths):
    """Of `paths`, which does the REAL `git archive <ref>` still emit?

    ⚠ Do NOT model this with `git check-attr`: check-attr reads the WORKING-TREE
    .gitattributes, while `git archive` reads .gitattributes **from the tree being
    archived**. An uncommitted export-ignore therefore looks active to check-attr while a
    real archive still publishes the file -- a model that fails PASS-shaped, i.e. exactly
    the wrong direction for a leak gate (it cost us a false PASS once; verified
    empirically). Corollary: committing the .gitattributes change IS part of the fix.

    We archive ONLY the already-identified suspect paths, so the tarball is a few hundred
    KB instead of >100 MB (this repo has a 23 MB coll_images.c plus embedded assets, on
    OneDrive). A gate slow enough to be skipped protects nothing.
    """
    if not paths:
        return []
    blob = subprocess.run(["git", "-C", REPO, "archive", ref, "--"] + paths,
                          capture_output=True, check=True).stdout
    # Parse with stdlib tarfile, NOT a `tar` subprocess: on Git-Bash/Windows `tar -t`
    # waits on its default device unless given `-f -`, and mixing a bytes `input` with
    # text=True is its own trap -- both manifest as an unexplained hang, not an error.
    with tarfile.open(fileobj=io.BytesIO(blob), mode="r|*") as tf:
        return sorted(m.name for m in tf if m.isfile())



def check_md_allowlist(tracked):
    """Markdown is DEFAULT-DENY in stage_source.sh (owner decision 2026-07-25).

    A denylist publishes anything new by default -- that is how a fresh docs/audit report, a
    root TODO queue and the ARG walkthrough all ended up staged. Prose is where internal
    thinking lives, so .md inverts the rule: dropped unless explicitly allowed.

    This verifies the mechanism is still present and prints the exact publish set, because
    the danger with an allowlist is the opposite one -- quietly dropping a doc that SHOULD
    ship (a license or provenance file) and only noticing after the release.
    """
    stage = os.path.join(REPO, FW, "scripts", "stage_source.sh")
    if not os.path.isfile(stage):
        return ["stage_source.sh not found"], []
    text = open(stage, encoding="utf-8", errors="replace").read()
    problems = []
    if "md-deny" not in text or "MD_ALLOW=(" not in text:
        problems.append("stage_source.sh lost its default-deny markdown sweep (MD_ALLOW / md-deny)")
        return problems, []
    # Terminate on a line that is just ")" -- NOT on the first ")" character, which lands
    # inside a trailing comment like "# repo landing page (REQUIRED below)" and silently
    # truncates the allowlist to its first entry. That made this gate report LICENSE.md and
    # acceptable_use.md as "dropped" when staging publishes them correctly: a parser bug in
    # the checker, not in the thing checked.
    tail = text.split("MD_ALLOW=(", 1)[1]
    lines = []
    for ln in tail.splitlines():
        if ln.strip() == ")":
            break
        lines.append(ln)
    block = "\n".join(lines)
    allow = []
    for line in block.splitlines():
        line = line.split("#", 1)[0].strip()
        if line.startswith('"') and line.endswith('"'):
            allow.append(line.strip('"').replace("$FW", FW))
    md_tracked = [t for t in tracked if t.lower().endswith(".md")]
    published = sorted(t for t in md_tracked if t in allow)
    dropped = [t for t in md_tracked if t not in allow]
    # An allowlist entry that no longer exists is a silent hole in the published tree.
    missing = [a for a in allow if a not in md_tracked]
    for m in missing:
        problems.append(f"MD_ALLOW lists '{m}' but it is not tracked -- stale entry or moved file")
    if "README.md" not in allow:
        problems.append("MD_ALLOW must include README.md (stage_source's REQUIRED check needs it)")
    return problems, (published, dropped)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref", default="HEAD")
    args = ap.parse_args()

    print(f"[source-drop] ref={args.ref}  repo={REPO}")
    tracked = tracked_at(args.ref)
    suspects = sorted({p for p in tracked
                       for pat, _ in FORBIDDEN if fnmatch.fnmatch(p, pat)})
    print(f"[source-drop] {len(tracked)} tracked at {args.ref}; "
          f"{len(suspects)} match a forbidden pattern")
    entries = survives_archive(args.ref, suspects)
    print(f"[source-drop] real `git archive` still emits {len(entries)} of them")

    failures = []
    for pattern, why in FORBIDDEN:
        hits = [e for e in entries if fnmatch.fnmatch(e, pattern)]
        if hits:
            failures.append((pattern, why, hits))
            print(f"  LEAK  {pattern}  ({len(hits)} file(s))")
            for h in hits[:6]:
                print(f"          {h}")
            if len(hits) > 6:
                print(f"          ... +{len(hits) - 6} more")
            print(f"        why: {why}")
        else:
            print(f"  ok    {pattern}")

    md_problems, md_info = check_md_allowlist(tracked)
    if md_info:
        published, dropped = md_info
        print("")
        print(f"[source-drop] markdown is DEFAULT-DENY: {len(published)} published, "
              f"{len(dropped)} dropped")
        for x in published:
            print(f"  publish  {x}")
    for x in md_problems:
        print(f"  MD-PROBLEM  {x}")

    # Layers 2+3: the trim/leak-gate entries must exist, so a future `git archive`
    # behaviour change (or an older git without export-ignore) still cannot publish.
    stage = os.path.join(REPO, FW, "scripts", "stage_source.sh")
    missing_layers = []
    if os.path.isfile(stage):
        text = open(stage, encoding="utf-8", errors="replace").read()
        for needle in REQUIRED_IN_STAGE:
            if needle not in text:
                missing_layers.append(needle)
        if missing_layers:
            print("\n[source-drop] stage_source.sh is missing TRIM/leak-gate entries:")
            for m in missing_layers:
                print(f"  MISS  {m}")
    else:
        missing_layers.append("stage_source.sh not found")

    print()
    missing_layers += md_problems
    if failures or missing_layers:
        print(f"[source-drop] FAILED  ({len(failures)} leaking pattern(s), "
              f"{len(missing_layers)} missing layer(s))")
        return 1
    print("[source-drop] PASSED  nothing internal would be published")
    return 0


if __name__ == "__main__":
    sys.exit(main())
