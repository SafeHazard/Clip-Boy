#!/usr/bin/env py -3
"""verify_redfirst.py -- prove each source-level regression check can actually SEE its bug.

A check authored by reading the FIXED code proves only that the code still says what it says
today. That is not a regression test, and it is not a hypothetical failure: the SB4 check in
check_security_regressions.py was written against the wrong FILE (the Evil Portal credential
sink is `startLog("evil_portal")` in WiFiScan.cpp, not in EvilPortal.cpp), so it reported a
confident green on a tree where the sink was live. Nothing else would have caught that, because
a check that looks in the wrong place is indistinguishable from a bug that is absent.

So: for every check registered with a BASELINE below, materialise that baseline commit's copies
of the files the check reads, run the check against them, and require it to go RED. Then confirm
it is GREEN on the working tree. Both directions, same run.

  py -3 Clip-Boy/scripts/tests/verify_redfirst.py
  py -3 Clip-Boy/scripts/tests/verify_redfirst.py --verbose

Add an entry to BASELINES whenever you add a check to check_security_regressions.py. A check
with no entry is REPORTED AS UNVERIFIED rather than silently skipped -- an un-run check reads
exactly like a passing one otherwise.
"""
import argparse
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
# The BASELINE side materialises old commits from git history, so it needs the REAL repo. When
# make_release_bins.sh runs this from a STAGING tree (no .git), the HERE-relative path lands on the
# staging output dir and every `git show`/`ls-tree`/`rev-list` fails -> every check scores
# CANNOT-TEST and the gate FAILs. release_from_staging.sh exports CLIPBOY_SOURCE_REPO=<real repo>.
# (The HEAD side reads C.REPO -- the imported check module's own REPO, i.e. the staging tree = HEAD
# source -- which is exactly right and is left untouched.) Standalone runs fall back unchanged.
REPO_ROOT = os.environ.get("CLIPBOY_SOURCE_REPO") or os.path.normpath(os.path.join(HERE, "..", "..", ".."))
PREFIX = "Clip-Boy/"        # firmware lives in a subdir of the repo

# finding -> commit at which that bug was PRESENT.
# ce373a5a is the 2026-07-24 pre-release audit baseline (see docs/audit/).
BASELINES = {
    "SB4":     "ce373a5a",
    # SB4b: the append half. Confirmed by blob inspection that it was NEVER guarded
    # at ce373a5a, so this goes red there cleanly -- it is not riding SB4's red.
    "SB4b":    "ce373a5a",
    "FB13":    "ce373a5a",
    "FB14":    "ce373a5a",
    "R5":      "ce373a5a",
    "W5-ARG":  "ce373a5a",
    "W5-MAC":  "ce373a5a",
    "W5-SAV":  "ce373a5a",
    "W5-SAFE": "ce373a5a",
    # Jun-2026 security audit. These predate the firmware's move into Clip-Boy/, so their
    # blobs live at the REPO ROOT -- see resolve_prefix(). `0c4bf4e6` was the P0 batch
    # (release-debug gate + factory-reset cred wipe + pre-OSS cleanup) and `a349f18a` the
    # P1/P2 batch (sniffer bounds + capture/portal/log hygiene), so each baseline is that
    # fix commit's PARENT -- the last tree on which the bug was live.
    "C1":      "0c4bf4e6^",
    "A1":      "0c4bf4e6^",
    "pre-OSS": "0c4bf4e6^",
    "A2":      "a349f18a^",
    "B1":      "a349f18a^",
    "B3":      "a349f18a^",
    "A5":      "a349f18a^",
}

# Files any check might read. Cheap to over-materialise; a missing one is a CANNOT-TEST.
FILES = [
    "ui_nav.h", "ui_collectibles.h", "ui_config.h", "Clip-Boy.ino",
    "libs/ClipBoy/src/EvilPortal.cpp", "libs/ClipBoy/src/WiFiScan.cpp",
    "libs/ClipBoy/src/WiFiScan.h", "libs/ClipBoy/src/settings.cpp",
    "libs/ClipBoy/src/CommandLine.cpp", "libs/ClipBoy/src/SDInterface.cpp",
]


def resolve_prefix(commit):
    """Return the path prefix under which `commit` stores the firmware, or None.

    The firmware was moved from the repo root into `Clip-Boy/` partway through the project,
    so a single hard-coded PREFIX silently fails to materialise every pre-move baseline --
    and a failed materialisation used to be *indistinguishable from a check going red*
    (see main()). Probe a file every tree has, and pick the prefix that actually resolves.
    """
    for prefix in (PREFIX, ""):
        probe = subprocess.run(["git", "-C", REPO_ROOT, "show", f"{commit}:{prefix}ui_nav.h"],
                               capture_output=True)
        if probe.returncode == 0:
            return prefix
    return None


def tree_listing(commit):
    """Every path in `commit`, repo-relative to the firmware root, forward-slashed.

    Lets checks whose evidence is a file's EXISTENCE (rather than its contents) be
    red-first verified with the same predicate the live run uses.
    """
    prefix = resolve_prefix(commit)
    if prefix is None:
        return []
    out = subprocess.run(["git", "-C", REPO_ROOT, "ls-tree", "-r", "--name-only", commit],
                         capture_output=True, text=True)
    if out.returncode != 0:
        return []
    paths = []
    for line in out.stdout.splitlines():
        line = line.strip().replace("\\", "/")
        if not line:
            continue
        if prefix:
            if not line.startswith(prefix):
                continue
            line = line[len(prefix):]
        paths.append(line)
    return paths


def materialise(commit):
    """Write `commit`'s copy of every FILES entry into a temp tree; return (dir, missing)."""
    tmp = tempfile.mkdtemp(prefix=f"redfirst_{commit.replace('^', '_p')}_")
    prefix = resolve_prefix(commit)
    if prefix is None:
        return tmp, list(FILES)          # nothing resolvable -> every check CANNOT TEST
    missing = []
    for rel in FILES:
        blob = subprocess.run(["git", "-C", REPO_ROOT, "show", f"{commit}:{prefix}{rel}"],
                              capture_output=True)
        if blob.returncode != 0:
            missing.append(rel)
            continue
        dst = os.path.join(tmp, rel.replace("/", os.sep))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(blob.stdout)
    return tmp, missing


def classify(old, new, raised):
    """(baseline result, HEAD result, exception) -> verdict.

    `raised` dominates: a check that threw did not RUN, and must never be scored as though
    it had SEEN its bug. That was a real defect here -- the exception text was assigned to
    `old`, and a truthy `old` is exactly the signal for "went red", so every baseline whose
    files failed to materialise scored a confident, meaningless "ok".
    """
    if raised:
        return "CANNOT TEST"
    if bool(old) and not new:
        return "ok"
    if not bool(old):
        return "VACUOUS"       # cannot see its own bug
    return "RED ON HEAD"       # the fix is missing, or the check is wrong


def selftest():
    """Prove the verdict logic reads all four ways, including the one that used to lie."""
    cases = [
        ("saw its bug at the baseline, clean on HEAD", ("found the bug", None, None), "ok"),
        ("blind at the baseline (the VACUOUS case)",   (None, None, None),            "VACUOUS"),
        ("still failing on HEAD",                      ("found", "found", None),      "RED ON HEAD"),
        ("threw -- must NOT read as 'ok'",             (None, None, "FileNotFoundError: x"),
         "CANNOT TEST"),
        ("threw WITH a truthy old (the old bug)",      ("exc text", None, "OSError: y"),
         "CANNOT TEST"),
    ]
    bad = 0
    print("=== selftest: verdict classification ===")
    for label, args_, want in cases:
        got = classify(*args_)
        ok = got == want
        bad += 0 if ok else 1
        print(f"  [{'ok' if ok else 'FAIL':^6}] {label}: {got}" + ("" if ok else f" (want {want})"))
    # End-to-end: a real check whose file is ABSENT at the baseline must report CANNOT TEST,
    # not "ok". This is the exact shape that used to be blessed, so prove it on real code
    # rather than trusting classify() in isolation.
    rev = subprocess.run(["git", "-C", REPO_ROOT, "rev-list", "--max-parents=0", "HEAD"],
                         capture_output=True, text=True)
    root_commit = rev.stdout.split()[0] if rev.returncode == 0 and rev.stdout.split() else None
    if root_commit:
        sys.path.insert(0, HERE)
        import check_security_regressions as C   # noqa: E402
        live_repo, tmp = C.REPO, materialise(root_commit)[0]
        probe = next((f for n, _d, f in C.CHECKS if n == "A2"), None)   # reads settings.cpp
        if probe is None:
            print("  [ FAIL ] selftest probe check 'A2' is gone -- update this selftest")
            bad += 1
        else:
            C.REPO = tmp
            raised = None
            try:
                old = probe()
            except Exception as e:                  # noqa: BLE001
                old, raised = None, f"{e.__class__.__name__}: {e}"
            C.REPO = live_repo
            got = classify(old, None, raised)
            ok = got == "CANNOT TEST"
            bad += 0 if ok else 1
            print(f"  [{'ok' if ok else 'FAIL':^6}] a check whose baseline file is absent "
                  f"reports: {got}" + ("" if ok else "  <-- would be silently blessed"))
    print("=" * 72)
    print("selftest: PASS" if not bad else f"selftest: FAIL ({bad})")
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--selftest", action="store_true",
                    help="prove the verdict logic itself, then exit")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    sys.path.insert(0, HERE)
    import check_security_regressions as C   # noqa: E402

    live_repo = C.REPO
    trees = {}
    for commit in sorted(set(BASELINES.values())):
        tmp, missing = materialise(commit)
        trees[commit] = tmp
        if missing and args.verbose:
            print(f"  note: {commit} lacks {', '.join(missing)} (checks reading them "
                  f"will report CANNOT TEST)")

    rows, unverified = [], []
    for finding, desc, fn in C.CHECKS:
        commit = BASELINES.get(finding)
        if not commit:
            unverified.append((finding, desc))
            continue
        C.REPO = trees[commit]
        C.LIST_TREE = lambda c=commit: tree_listing(c)
        raised = None
        try:
            old = fn()
        except Exception as e:                      # noqa: BLE001
            old, raised = None, f"{e.__class__.__name__}: {e}"
        C.REPO = live_repo
        C.LIST_TREE = None
        try:
            new = fn()
        except Exception as e:                      # noqa: BLE001
            new, raised = None, f"on HEAD -- {e.__class__.__name__}: {e}"
        rows.append((finding, commit, old, new, raised))

    print("=== red-first verification (check must FAIL on the buggy tree, PASS on HEAD) ===")
    bad = 0
    for finding, commit, old, new, raised in rows:
        verdict = classify(old, new, raised)
        if verdict != "ok":
            bad += 1
        print(f"  [{verdict:^11}] {finding} vs {commit}")
        if args.verbose or verdict != "ok":
            if raised:
                print(f"                 raised    : {raised[:150]}")
            print(f"                 buggy tree: {str(old)[:150] if old else 'PASSED (blind)'}")
            print(f"                 HEAD      : {str(new)[:150] if new else 'passed'}")

    for finding, desc in unverified:
        print(f"  [ UNVERIFIED ] {finding}: no BASELINES entry -- {desc[:80]}")

    print("=" * 72)
    print(f"{len(rows)} verified, {bad} problem(s), {len(unverified)} without a baseline")
    if bad or unverified:
        # A check with no baseline has never been shown to detect anything, and an un-run check
        # is indistinguishable from a passing one -- so it must FAIL the gate, not merely print
        # a line. This used to exit 0 with 7 checks unverified.
        print("verify_redfirst: FAIL")
        return 1
    print("verify_redfirst: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
