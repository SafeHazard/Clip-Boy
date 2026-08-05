#!/usr/bin/env python3
"""
run_all.py — Test orchestrator for Clip-Boy badge.

Runs all tests using a SINGLE serial session to avoid DTR resets.
Optionally compiles and flashes first.

Usage:
    py -3 scripts/tests/run_all.py                          # Run tests only
    py -3 scripts/tests/run_all.py --build                   # Build + test
    py -3 scripts/tests/run_all.py --build --upload          # Build + upload + test
    py -3 scripts/tests/run_all.py --build --upload --port COM5
"""

import subprocess
import sys
import os
import time
import argparse
from datetime import datetime

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.abspath(os.path.join(SCRIPT_DIR, "..", ".."))
BUILD_SCRIPT = os.path.join(PROJECT_DIR, "scripts", "build.sh")

sys.path.insert(0, SCRIPT_DIR)
from harness import Harness

# Import test modules
import test_navigation
import test_continuity
import test_tool_sequence
import test_hr_scanning
import test_settings
import test_screensaver
import test_wifi_tools
import test_sd_card
import test_sd_pcap
import test_screenshot
import test_tool_gauntlet
import test_manual_entry


def run_build(upload=False, port=None):
    """Compile and optionally upload firmware with TEST_HARNESS."""
    cmd = ["bash", BUILD_SCRIPT, "--test"]
    if upload:
        cmd.append("--upload")
        if port:
            cmd += ["--port", port]

    print(f"[ORCHESTRATOR] Building: {' '.join(cmd)}")
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    print(result.stdout[-500:] if len(result.stdout) > 500 else result.stdout)
    if result.returncode != 0:
        print(f"[ORCHESTRATOR] Build FAILED")
        if result.stderr:
            print(result.stderr[-500:])
        return False
    return True


def main():
    parser = argparse.ArgumentParser(description="Clip-Boy test orchestrator")
    parser.add_argument("--build", action="store_true", help="Compile before testing")
    parser.add_argument("--upload", action="store_true", help="Upload after compile")
    parser.add_argument("--port", help="Serial port")
    parser.add_argument("--skip", nargs="*", default=[], help="Test names to skip")
    args = parser.parse_args()

    print(f"[ORCHESTRATOR] Clip-Boy Test Suite — {datetime.now().isoformat()}")
    print(f"[ORCHESTRATOR] Project: {PROJECT_DIR}")

    # Build phase
    if args.build:
        if not run_build(upload=args.upload, port=args.port):
            sys.exit(1)
        if args.upload:
            print("[ORCHESTRATOR] Waiting 12s for device boot after flash...")
            time.sleep(12)

    # All tests use the main() function from their modules.
    # Each test creates its own Harness (which opens one serial session).
    # Since we can't share a single Harness across modules easily,
    # we run tests sequentially — each gets a fresh boot from DTR.
    # The _ensure_ready mechanism handles the boot wait.

    results = {}
    total_passed = 0
    total_failed = 0

    # Host-side security/regression checks (no device) — run first so a
    # source-level revert of an audited fix fails fast before the device suite.
    # check_sku_binaries needs built bins, so it's added only when present.
    print(f"\n{'#' * 60}\n[ORCHESTRATOR] Host security checks\n{'#' * 60}")
    host_checks = ["check_terminology.py", "check_security_regressions.py",
                   "verify_sniffer_bounds.py"]
    if os.path.isdir(os.path.join(PROJECT_DIR, "build", "sn34k")) and \
       os.path.isdir(os.path.join(PROJECT_DIR, "build", "res34rch")):
        host_checks.append("check_sku_binaries.py")
    for hc in host_checks:
        name = "host:" + hc.replace("check_", "").replace("verify_", "").replace(".py", "")
        if hc in args.skip or name in args.skip:
            results[name] = "SKIP"
            continue
        rc = subprocess.run([sys.executable, os.path.join(SCRIPT_DIR, hc)]).returncode
        print(f"[ORCHESTRATOR] {hc}: {'PASS' if rc == 0 else 'FAIL'}")
        results[name] = "PASS" if rc == 0 else "FAIL"
        total_passed += 1 if rc == 0 else 0
        total_failed += 0 if rc == 0 else 1

    tests = [
        ("navigation", test_navigation.main),
        ("continuity", test_continuity.main),
        ("tool_sequence", test_tool_sequence.main),
        ("hr_scanning", test_hr_scanning.main),
        ("settings", test_settings.main),
        ("screensaver", test_screensaver.main),
        ("wifi_tools", test_wifi_tools.main),
        ("sd_card", test_sd_card.main),
        ("sd_pcap", test_sd_pcap.main),
        ("screenshot", test_screenshot.main),
        ("tool_gauntlet", test_tool_gauntlet.main),
        ("manual_entry", test_manual_entry.main),
    ]

    for test_name, test_fn in tests:
        if test_name in args.skip:
            print(f"\n[ORCHESTRATOR] SKIP: {test_name}")
            results[test_name] = "SKIP"
            continue

        print(f"\n{'#' * 60}")
        print(f"[ORCHESTRATOR] Running: {test_name}")
        print(f"{'#' * 60}")

        try:
            rc = test_fn()
            if rc == 0:
                results[test_name] = "PASS"
                total_passed += 1
            else:
                results[test_name] = "FAIL"
                total_failed += 1
        except Exception as e:
            print(f"[ORCHESTRATOR] CRASH: {test_name} — {e}")
            results[test_name] = f"CRASH: {e}"
            total_failed += 1

    # Summary
    print(f"\n{'#' * 60}")
    print(f"TEST SUITE RESULTS — {datetime.now().isoformat()}")
    print(f"{'#' * 60}")
    for test_name, status in results.items():
        icon = "PASS" if status == "PASS" else ("SKIP" if status == "SKIP" else "FAIL")
        print(f"  [{icon}] {test_name}: {status}")
    print(f"\nTotal: {total_passed} passed, {total_failed} failed")
    print(f"{'#' * 60}")

    sys.exit(0 if total_failed == 0 else 1)


if __name__ == "__main__":
    main()
