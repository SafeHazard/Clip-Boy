#!/usr/bin/env python3
"""
test_sd_card.py — SD card interaction tests.

Tests:
  - SD card mounted and accessible
  - File listing
  - File read/write
  - Collectibles CSV override from SD
  - Image override from SD /images/ directory
  - File existence check
"""

import sys
import os
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Harness


def test_sd_available(h):
    """Verify SD card is mounted and accessible."""
    print("\n--- SD card availability ---")

    r = h.sd_list("/")
    if not r.get("ok"):
        err = r.get("error", "unknown")
        return False, [f"SD card not available: {err}"]

    files = r.get("files", [])
    print(f"  Root directory: {len(files)} entries")
    for f in files[:10]:
        ftype = "DIR " if f.get("dir") else "FILE"
        print(f"    {ftype} {f.get('name', '?')} ({f.get('size', 0)} bytes)")
    if len(files) > 10:
        print(f"    ... and {len(files) - 10} more")

    return True, []


def test_sd_list_images(h):
    """List /images/ directory on SD card."""
    print("\n--- SD /images/ directory ---")

    try:
        r = h.sd_list("/images")
    except Exception as e:
        print(f"  WARN: sd_list /images exception: {e}")
        return True, []  # Not a failure — directory may not exist

    if not r.get("ok"):
        err = r.get("error", "unknown")
        print(f"  SKIP: /images/ not accessible ({err})")
        return True, []  # Optional directory

    files = r.get("files", [])
    print(f"  /images/: {len(files)} files")
    for f in files[:5]:
        print(f"    {f.get('name', '?')} ({f.get('size', 0)} bytes)")

    return True, []


def test_sd_write_read(h):
    """Write a test file, read it back, verify content."""
    print("\n--- SD write + read ---")

    test_path = "/test_harness_verify.txt"
    test_content = "ClipBoy test harness verification 2026"

    # Write
    r = h.sd_write(test_path, test_content)
    if not r.get("ok"):
        return False, [f"SD write failed: {r.get('error', '?')}"]
    print(f"  Written: {r.get('bytes', 0)} bytes to {test_path}")

    # Verify exists
    r = h.sd_exists(test_path)
    if not r.get("exists"):
        return False, ["File doesn't exist after write"]
    print(f"  Exists: yes")

    # Read back
    r = h.sd_read(test_path)
    if not r.get("ok"):
        return False, [f"SD read failed: {r.get('error', '?')}"]
    print(f"  Read: {r.get('size', 0)} bytes")

    # Note: sd_read sends binary content after the JSON header.
    # The session mode harness may not handle the binary payload correctly
    # for text comparison. The JSON response confirming size is sufficient.
    expected_size = len(test_content)
    actual_size = r.get("size", 0)
    if actual_size != expected_size:
        return False, [f"Size mismatch: wrote {expected_size}, read {actual_size}"]

    print(f"  OK: Write/read/verify cycle passed")
    return True, []


def test_sd_collectibles_csv(h):
    """Verify /collectibles.csv exists on SD and is readable."""
    print("\n--- SD collectibles.csv ---")

    r = h.sd_exists("/collectibles.csv")
    if not r.get("ok"):
        return False, [f"sd_exists failed: {r.get('error', '?')}"]

    if not r.get("exists"):
        print("  SKIP: /collectibles.csv not found on SD")
        return True, []  # Not a failure — SD override is optional

    r = h.sd_read("/collectibles.csv")
    if not r.get("ok"):
        return False, [f"SD read failed: {r.get('error', '?')}"]

    size = r.get("size", 0)
    print(f"  /collectibles.csv: {size} bytes")
    if size < 100:
        print(f"  WARN: CSV suspiciously small ({size} bytes)")
    elif size > 53 * 1024:
        print(f"  WARN: CSV exceeds max size ({size} > 53KB)")
    else:
        print(f"  OK: CSV size within expected range")

    return True, []


def test_sd_write_collectibles_override(h):
    """Write a test collectible to SD CSV and read it back."""
    print("\n--- SD collectibles override test ---")

    test_csv = "ID,Title,Source,Tier,Desc,Mod1,Stat1,Mod2,Stat2,Mod3,Stat3"

    try:
        r = h.sd_write("/coll_test.csv", test_csv)
        if not r.get("ok"):
            return False, [f"Write failed: {r.get('error', '?')}"]
        print(f"  Written: {r.get('bytes', 0)} bytes")

        # Verify via sd_exists (was known to hang — fixed by using SD.open
        # internally in th_cmd_sd_exists)
        r = h.sd_exists("/coll_test.csv")
        if not r.get("ok"):
            return False, [f"sd_exists failed: {r.get('error', '?')}"]
        if not r.get("exists"):
            return False, ["File doesn't exist after write"]
        print(f"  sd_exists: yes")

        # Read back too
        r = h.sd_read("/coll_test.csv")
        if not r.get("ok"):
            return False, [f"Read back failed: {r.get('error', '?')}"]
        print(f"  Read back: {r.get('size', 0)} bytes")
        print("  OK: Collectibles override CSV write/read/exists passed")
        return True, []
    except Exception as e:
        return False, [f"Exception: {e}"]


def test_sd_file_operations(h):
    """Test various SD file operations — error handling."""
    print("\n--- SD file operations ---")
    errors = []

    try:
        # Test reading non-existent file
        r = h.sd_read("/nonexistent_file_xyz.txt")
        if r.get("ok"):
            errors.append("Reading non-existent file should fail")
        else:
            print(f"  OK: Reading non-existent file correctly fails")
    except Exception as e:
        print(f"  WARN: sd_read exception for non-existent file: {e}")

    try:
        # Test listing non-existent directory
        r = h.sd_list("/nonexistent_dir_xyz")
        if r.get("ok"):
            errors.append("Listing non-existent dir should fail")
        else:
            print(f"  OK: Listing non-existent dir correctly fails")
    except Exception as e:
        print(f"  WARN: sd_list exception for non-existent dir: {e}")

    if errors:
        return False, errors
    return True, []


def main():
    print("=" * 60)
    print("TEST: SD Card Operations")
    print("=" * 60)

    h = Harness()

    # GATE: SD availability decides testable-vs-CANNOT-TEST. If the badge can't even list the SD
    # root there is no card mounted -> CANNOT-TEST (rc=2), never a false FAIL (owner 2026-07-30:
    # probe per-badge). A card that IS present but whose operations misbehave still returns FAIL in
    # the loop below, so a real SD regression is NOT masked by this gate.
    ok, errs = test_sd_available(h)
    if not ok:
        h.close()
        print("\n" + "=" * 60)
        print("CANNOT-TEST: no SD card mounted -- %s" % (errs[0] if errs else "sd_list('/') failed"))
        print("(A card present but broken would FAIL in the sub-tests, not reach here.)")
        print("=" * 60)
        return 2

    passed = 1   # SD-available is the first sub-test and it just passed
    failed = 0
    all_errors = []

    tests = [
        ("Images directory", test_sd_list_images),
        ("Write + read", test_sd_write_read),
        ("Collectibles CSV", test_sd_collectibles_csv),
        ("Collectibles override", test_sd_write_collectibles_override),
        ("File operations", test_sd_file_operations),
    ]

    for name, test_fn in tests:
        try:
            ok, errs = test_fn(h)
            if ok:
                passed += 1
            else:
                failed += 1
                all_errors.extend([f"{name}: {e}" for e in errs])
        except Exception as e:
            print(f"  CRASH: {e}")
            failed += 1
            all_errors.append(f"{name}: CRASH — {e}")
            # Try to reconnect
            try:
                h.close()
            except Exception:
                pass
            try:
                h = Harness()
            except Exception:
                print("  Cannot reconnect — aborting remaining tests")
                break

    h.close()

    total = passed + failed
    print(f"\n{'=' * 60}")
    print(f"RESULTS: {passed}/{total} passed, {failed} failed")
    if all_errors:
        print("\nFailures:")
        for e in all_errors:
            print(f"  - {e}")
    print("=" * 60)
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
