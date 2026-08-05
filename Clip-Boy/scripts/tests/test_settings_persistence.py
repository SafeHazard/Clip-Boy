#!/usr/bin/env python3
"""
test_settings_persistence.py — verify NVS-backed settings survive reboot.

For each scalar config key:
  1. Read the original value
  2. Choose a different value within valid range
  3. Write it via cfg_set
  4. Verify the in-memory read via cfg_get
  5. Reboot the badge
  6. Re-read — must equal the value written before reboot
  7. Restore the original (don't leave the device in a weird state)
"""

import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Harness


# (key, type, alternate_value_for_testing)
# Int keys pick a value different from the default but within clamp range.
# Bool keys flip whatever is currently set.
SCALAR_KEYS = [
    ("theme",      "int",  1),     # 0=Mojave, 1=RibbitCity, 2=Flashbang
    ("brightness", "int",  50),    # 10-100
    ("volume",     "int",  30),    # 0-100
    ("disp_off",   "int",  5),     # 0-5 (5=Never, safe during test)
    ("airplane",   "bool", None),  # flip
    ("sound",      "bool", None),  # flip
    ("ss_style",   "int",  1),     # 0-1
    ("ss_leds",    "bool", None),  # flip
    ("crt_scan",   "bool", None),  # flip
    ("crt_flick",  "bool", None),  # flip
]


def read_all(h):
    out = {}
    for key, *_ in SCALAR_KEYS:
        r = h.cfg_get(key)
        if not r.get("ok"):
            raise RuntimeError(f"cfg_get {key} failed: {r}")
        out[key] = r["value"]
    return out


def pick_alternate(key, kind, alt, current):
    """Return a value guaranteed to differ from current."""
    if kind == "bool":
        return not current
    # int
    if alt != current:
        return alt
    # fallback: pick something else safe
    if key == "theme":      return 2 if current != 2 else 0
    if key == "brightness": return 70 if current != 70 else 40
    if key == "volume":     return 50 if current != 50 else 20
    if key == "disp_off":   return 4 if current != 4 else 3
    if key == "ss_style":   return 0 if current != 0 else 1
    return alt


def main():
    print("=" * 60)
    print("TEST: Settings Persistence (NVS)")
    print("=" * 60)

    h = Harness()
    failures = []

    # 1. Snapshot current values
    original = read_all(h)
    print("\nOriginal values:")
    for k, v in original.items():
        print(f"  {k:12s} = {v}")

    # 2. Build the test plan — a different value for each key
    plan = {}
    for key, kind, alt in SCALAR_KEYS:
        plan[key] = pick_alternate(key, kind, alt, original[key])

    # 3. Write new values + verify in-memory
    print("\nWriting new values:")
    for key, new_val in plan.items():
        r = h.cfg_set(key, new_val)
        if not r.get("ok"):
            failures.append(f"cfg_set {key}={new_val} failed: {r.get('error', '?')}")
            continue
        verify = h.cfg_get(key)
        if not verify.get("ok") or verify["value"] != new_val:
            failures.append(f"{key}: wrote {new_val}, in-mem read {verify.get('value')}")
        else:
            print(f"  {key:12s} = {new_val} (in-mem OK)")

    if failures:
        print("\nIn-memory write/read failures before reboot:")
        for f in failures:
            print(f"  - {f}")

    # 4. Reboot
    print("\nRebooting...")
    h.reboot_and_wait(timeout=12)

    # 5. Read back after reboot
    print("\nPost-reboot values:")
    post = read_all(h)
    persist_failures = []
    for key, want in plan.items():
        got = post[key]
        if got != want:
            persist_failures.append(f"{key}: wrote {want}, after reboot {got}")
        print(f"  {key:12s} = {got}  (expected {want}) {'OK' if got == want else 'FAIL'}")

    # 6. Restore originals
    print("\nRestoring original values...")
    for key, orig in original.items():
        h.cfg_set(key, orig)
    h.reboot_and_wait(timeout=12)
    restored = read_all(h)
    restore_failures = []
    for key, orig in original.items():
        if restored[key] != orig:
            restore_failures.append(f"{key}: couldn't restore (have {restored[key]}, wanted {orig})")

    h.close()

    # 7. Report
    total_fails = len(failures) + len(persist_failures) + len(restore_failures)
    total_tests = len(SCALAR_KEYS) * 2 + len(SCALAR_KEYS)  # in-mem + post-reboot + restore
    passed = total_tests - total_fails

    print(f"\n{'=' * 60}")
    print(f"RESULTS: {passed}/{total_tests} assertions passed, {total_fails} failed")
    if persist_failures:
        print("\nPersistence failures (values NOT preserved across reboot):")
        for f in persist_failures:
            print(f"  - {f}")
    if restore_failures:
        print("\nRestore failures (device left in a non-original state!):")
        for f in restore_failures:
            print(f"  - {f}")
    print("=" * 60)
    return 0 if total_fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
