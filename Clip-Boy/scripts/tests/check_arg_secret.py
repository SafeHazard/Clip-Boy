#!/usr/bin/env python3
"""Release gate: the shipped bins MUST carry the REAL ARG finale key.

Why this exists (regression that actually shipped, Jul 2026): the public/KS source
staging trees intentionally contain the all-zero placeholder `secret.h` (the HMAC key
must never land in the GPLv3 source). `release_from_staging.sh` builds the shipped
bins FROM those trees -- so the bins inherited the placeholder, and every badge
verified P5 phone-unlock codes against a zero key while the live IVR used the real
one. Result: the ARG finale was dead on every shipped badge, and nothing caught it.

The key is compiled in as a 64-char hex STRING (arg_unlock.h -> arg_hexdecode), so it
is directly greppable in the .bin. This gate asserts, for every release app image:
    - the all-zero 64-hex placeholder is ABSENT, and
    - the expected real key is PRESENT.

The expected key is read at gate time from the local gitignored secret.h (or
CLIPBOY_ARG_SECRET_HEX). It is NEVER printed -- only a short SHA-256 fingerprint.

Usage:
    py -3 check_arg_secret.py [--release-dir DIR] [--secret-hex HEX]

Exit 0 = all release bins carry the real key.
"""
import argparse
import glob
import hashlib
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# scripts/tests/ -> scripts/ -> Clip-Boy/  (the firmware dir; holds secret.h + release/)
FW = os.path.abspath(os.path.join(HERE, "..", ".."))
ZERO = "0" * 64

# The shipped application images are checked by GLOB, not a fixed list:
# make_release_bins.sh is invoked per-staging with a VARIANT SUBSET (public tree
# builds sn34k+res34rch, KS tree builds the -rift pair), so demanding all four
# would false-fail every partial build. We verify every app image that is
# actually present, and fail if there are none.
APP_GLOB = "clipboy-*-app.bin"


def load_secret(explicit=None):
    """Real 64-hex key from --secret-hex, $CLIPBOY_ARG_SECRET_HEX, or secret.h."""
    for src, val in (("--secret-hex", explicit),
                     ("$CLIPBOY_ARG_SECRET_HEX", os.environ.get("CLIPBOY_ARG_SECRET_HEX"))):
        if val:
            if not re.fullmatch(r"[0-9a-fA-F]{64}", val):
                sys.exit(f"FAIL: {src} is not 64 hex chars")
            return val, src
    path = os.path.join(FW, "secret.h")
    if not os.path.isfile(path):
        sys.exit("FAIL: no real secret available (no --secret-hex, no "
                 "$CLIPBOY_ARG_SECRET_HEX, no Clip-Boy/secret.h). Cannot verify "
                 "the shipped key -- refusing to pass.")
    m = re.search(r'ARG_HMAC_SECRET_HEX\s+"([0-9a-fA-F]{64})"',
                  open(path, encoding="utf-8", errors="replace").read())
    if not m:
        sys.exit(f"FAIL: {path} has no 64-hex ARG_HMAC_SECRET_HEX")
    return m.group(1), "secret.h"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--release-dir", default=os.path.join(FW, "release"))
    ap.add_argument("--secret-hex")
    args = ap.parse_args()

    secret, src = load_secret(args.secret_hex)
    if secret == ZERO:
        sys.exit("FAIL: the configured ARG key IS the all-zero placeholder. "
                 "Inject the real key (CLIPBOY_ARG_SECRET_HEX) before cutting a release.")
    fp = hashlib.sha256(bytes.fromhex(secret)).hexdigest()[:8]
    print(f"[arg-secret] expected key from {src}, fingerprint {fp} (value never printed)")

    want = secret.lower().encode()
    zero = ZERO.encode()
    failures, checked = [], 0

    for path in sorted(glob.glob(os.path.join(args.release_dir, APP_GLOB))):
        name = os.path.basename(path)
        blob = open(path, "rb").read()
        low = blob.lower()
        has_real = want in low
        has_zero = zero in blob
        checked += 1
        status = "OK" if (has_real and not has_zero) else "FAIL"
        print(f"  {status:4s} {name:32s} real-key:{'present' if has_real else 'ABSENT':8s} "
              f"zero-key:{'PRESENT' if has_zero else 'absent'}")
        if not has_real:
            failures.append(f"{name}: real ARG key ABSENT -- P5 phone unlock would be dead")
        if has_zero:
            failures.append(f"{name}: all-zero placeholder key PRESENT in a release image")

    if checked == 0:
        sys.exit(f"FAIL: no {APP_GLOB} found in {args.release_dir}")
    if failures:
        print("\n[arg-secret] FAILED:")
        for f in failures:
            print(f"  - {f}")
        return 1
    print(f"[arg-secret] PASS - all {checked} release images carry the real ARG key")
    return 0


if __name__ == "__main__":
    sys.exit(main())
