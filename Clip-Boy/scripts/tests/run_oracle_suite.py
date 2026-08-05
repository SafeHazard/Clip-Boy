#!/usr/bin/env python3
"""run_oracle_suite.py -- the ORACLE-grade release gate for Clip-Boy.

Unlike run_all.py (the liveness gauntlet: "every tool starts without crashing"), this runs the
tests that actually PROVE behaviour -- comparative/identity oracles with controls -- across a
2x2 badge fleet (2 sn34k + 2 res34rch) and emits a signed coverage MANIFEST the owner acks
before a release.

DESIGN (owner-approved 2026-07-30):
- BUILD FROM STAGING. The bins under test are built from the SAME staged source a release is
  built from (stage_source.sh -> build.sh --test), so "what we tested" == "what we can release"
  modulo the TEST_HARNESS/COLL_DEBUG flags. (The shipping bins are non-test and cannot be driven
  by the STX harness; their integrity is covered by the T0 source/binary gates. This split is
  inherent and is stated in the manifest, not papered over.)
- TIERS:
    T0  host-only    -- source/binary gates, no badge. HARD gate (all must PASS).
    T1  single-badge -- no RF witness. HARD gate. SERIALIZED across badges (host-load contention).
    T2  RF-witness   -- needs kali/kalipi/shipship. SERIALIZED on the shared rig.
                        Gate = all-PASS OR owner-acked CANNOT-TEST (rig down != silent pass).
- WITNESS-NOT-READY CHECK (decision 1): the kali witness is a live image (re-setup per boot).
  If its self-check fails, ALL T2 tests are recorded CANNOT-TEST -- never a false FAIL, never a
  silent pass. The manifest names exactly what was skipped-for-rig.
- 2x2 (decision 3): each test runs on BOTH badges of its SKU; a test PASSES the gate only if both
  units agree. Disagreement -> flagged as a UNIT issue, not auto-ship-blocking.
- rc convention (matches the oracle tests): 0=PASS, 2=CANNOT-TEST, other=FAIL.

USAGE:
    py -3 scripts/tests/run_oracle_suite.py --dry-run          # print the plan, no hardware
    py -3 scripts/tests/run_oracle_suite.py --ports COM4,COM5,COM6,COM7
    py -3 scripts/tests/run_oracle_suite.py --build --ports COM4,COM5,COM6,COM7
      --build   : stage HEAD + build sn34k/res34rch --test from staging, flash per SKU
      (no --build: run against whatever is already flashed on each port)

Increment 1: architecture + registry + staging-build + SKU auto-detect + dry-run. Full 2x2
hardware execution is validated incrementally as each registered test is confirmed on the fleet.
"""
import argparse, concurrent.futures as cf, json, os, shutil, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
FW_DIR = os.path.abspath(os.path.join(HERE, "..", ".."))          # the Clip-Boy/ dir
sys.path.insert(0, HERE)

def _find_git_bash():
    """subprocess.run(['bash', ...]) on Windows lets CreateProcess resolve the bare name, and the
    Windows 'bash.exe' App-Execution-Alias (WindowsApps / System32) points at WSL bash -- uname
    Linux, drives at /mnt/c (NOT /c), and NO arduino-cli/esptool/vendored toolchain. That bash
    cannot run our staging build; it just 127s on every path form. So resolve the MSYS/Git bash
    explicitly and hand subprocess its FULL path, bypassing the alias. (Git bash accepts both
    'C:\\..' and 'C:/..' path args, so path form is secondary -- the launcher identity is the bug.)"""
    cands = [r"C:\Program Files\Git\usr\bin\bash.exe", r"C:\Program Files\Git\bin\bash.exe"]
    w = shutil.which("bash")
    if w and "system32" not in w.lower() and "windowsapps" not in w.lower():
        cands.insert(0, w)
    for c in cands:
        if c and os.path.exists(c):
            return c
    return "bash"  # last resort -- may still hit WSL, but we tried and the failure is loud (127)

BASH = _find_git_bash()

PASS, FAIL, CANT = "PASS", "FAIL", "CANNOT-TEST"

# Settle between the serial tests. Each test subprocess opens/closes its own bridge session, and
# reboot-heavy tests (teardown-paths ~18 reboots, settings-persist) cycle the native USB-CDC many
# times. Those cycles accumulate and eventually WEDGE the ESP32-S3 CDC (a reconnect then hangs ->
# a 600s test timeout). Confirmed 2026-07-31: every test passes fresh-solo; a long full run wedges
# partway. A short settle spaces the GLOBAL reconnect cadence (only one test runs at a time here),
# giving the host USB controller + the badge CDC room to breathe between cycles.
INTER_TEST_SETTLE_S = 4

# ── Test registry ──────────────────────────────────────────────────────────────────────────
# tier: T0|T1|T2 ; skus: which SKUs the test applies to ; witness: rig deps it needs (for T2);
# sensor: needs the VL53 sensor. cmd is argv relative to the tests dir (--port appended for badge
# tests). host tests run once (no badge). Keep this list the single source of coverage truth.
class T:
    def __init__(self, name, cmd, tier, skus=("sn34k", "res34rch"), witness=(), sensor=False,
                 note=""):
        self.name, self.cmd, self.tier = name, cmd, tier
        self.skus, self.witness, self.sensor, self.note = skus, witness, sensor, note

REGISTRY = [
    # ── T0 host/source/binary gates (run once, no badge). Some need the built release bins. ──
    T("terminology",     ["check_terminology.py"],            "T0"),
    T("deref-guards",    ["check_deref_guards.py"],           "T0"),
    T("security-regress",["check_security_regressions.py"],   "T0"),
    T("redfirst",        ["verify_redfirst.py"],              "T0"),
    T("source-drop",     ["check_source_drop.py"],            "T0"),
    # ── The 4 BINARY-ARTIFACT gates (sku-binaries / ip-scrub / arg-secret / littlefs) were DROPPED
    #    here (owner decision 2026-07-30). They are STATIC checks of the RELEASE .bin files (string
    #    denylists, hash/symbol scans) -- NOT harness tests -- and this suite builds only --test
    #    bins (no release bins), so here they inspected the stale DEV tree: littlefs FALSE-FAILed on
    #    a missing artifact, and sku/ip/arg "passed" against whatever stale dev bins happened to
    #    exist. They are owned by `make_release_bins.sh` at RELEASE time, against the real ship bins.
    #    The oracle keeps only the 5 SOURCE gates above (valid: staged tree == git archive of HEAD).

    # ── T1 single-badge, no external RF witness. SERIALIZED across badges (see the T1 exec block
    #    in main() for why -- host-load harness read-timeout under concurrency). ──
    T("navigation",      ["test_navigation.py"],              "T1"),
    T("continuity",      ["test_continuity.py"],              "T1"),
    T("settings-persist",["test_settings_persistence.py"],    "T1"),
    T("screensaver",     ["test_screensaver.py"],             "T1"),
    T("screenshot",      ["test_screenshot.py"],              "T1"),
    T("sd-card",         ["test_sd_card.py"],                 "T1"),
    T("teardown-paths",  ["test_teardown_paths.py"],          "T1"),
    T("tool-gauntlet",   ["test_tool_gauntlet.py"],           "T1", note="liveness, not oracle"),
    T("info-screens",    ["test_info_screens.py"],            "T1"),
    T("leds-theme",      ["test_leds_theme.py"],              "T1"),
    # hr-scanning + theremin DROPPED (owner 2026-07-30): don't run sensor tests in the oracle. They
    #    need a physically-presented HR tag / a hand over the LiDAR, which the fleet rig can't
    #    supply, so they FAIL where they'd have to be CANNOT-TEST. LiDAR presence is covered by the
    #    badges booting; sensor BEHAVIOR is validated manually.
    T("airplane",        ["test_airplane_enforcement.py"],    "T1"),

    # ── T2 RF-witness (kali/kalipi/shipship). SERIALIZED on the rig. ──
    #    Passive/RX oracles -- run on the SHIPPING sn34k SKU (SKU-identical passive path).
    T("detect-oracles",  ["test_detect_oracles.py"],          "T2", skus=("sn34k",), witness=("kalipi",)),
    T("scan-rx",         ["test_scan_rx.py"],                 "T2", skus=("sn34k",), witness=("kalipi","kali")),
    T("rx-detection",    ["test_rx_detection.py"],            "T2", skus=("sn34k",), witness=("kalipi","kali")),
    T("skimmer-id",      ["test_skimmer_identity_rx.py"],     "T2", skus=("sn34k",), witness=("kalipi","kali-csr")),
    T("sae-sniff-rx",    ["test_sae_sniff_rx.py"],            "T2", skus=("sn34k",), witness=("kali","res34rch-emitter")),
    T("join-wifi",       ["test_join_wifi.py"],               "T2", skus=("sn34k",), witness=("shipship",)),
    T("ble-advert-rate", ["test_ble_advert_rate.py"],         "T2", skus=("sn34k",), witness=("kalipi","kali-csr")),
    T("deauth-channel",  ["test_deauth_channel.py"],          "T2", skus=("res34rch",), witness=("kalipi",)),
    #    Active-TX oracles -- res34rch only.
    T("sae-witness",     ["test_sae_witness.py"],             "T2", skus=("res34rch",), witness=("kali","shipship")),
    T("ble-spam-witness",["test_ble_spam_witness.py"],        "T2", skus=("res34rch",), witness=("kali-csr",)),
    T("evil-portal",     ["test_evil_portal_witness.py"],     "T2", skus=("res34rch",), witness=("kali","shipship")),
    T("flood-beacon",    ["test_flood_beacon_witness.py"],    "T2", skus=("res34rch",), witness=("kali","shipship")),
]

# ── Staging build (the "build what we can release" path) ─────────────────────────────────────
def _bp(p):
    """Forward-slash a path for MSYS/Git Bash. When a Windows backslash path is handed to bash --
    either as an argv element or inside a `bash -c` string -- bash eats the backslashes as escape
    chars ('C:\\Users\\...' -> 'C:Users...'), so the file is 'not found' (exit 127). Git Bash
    accepts 'C:/Users/...' natively, so normalize every bash-facing path through here. NOT needed
    for subprocess cwd= (handled by the OS, not bash) or native Windows exes like esptool."""
    return str(p).replace("\\", "/")

def build_from_staging(stage_dir):
    """stage HEAD -> build sn34k --test AND res34rch --test from the STAGED source. Returns
    {sku: app_bin_path} or raises. Mirrors release_from_staging.sh's stage+provision, but builds
    the --test (harness) variants the oracle tests need instead of the release bins."""
    # tested==releasable: refuse a DIRTY tree, exactly as release_from_staging.sh does. stage_source.sh
    # archives HEAD and then overlays `git diff HEAD` (tracked mods), so a dirty tree would build+test
    # HEAD+uncommitted edits that can never be released -- a green run on source that doesn't exist as
    # a commit. Override with ORACLE_ALLOW_DIRTY=1 for local iteration (accepting tested != releasable).
    dirty = subprocess.run([BASH, "-c", "git -C '%s' diff --quiet HEAD || echo DIRTY" % _bp(FW_DIR)],
                           capture_output=True, text=True).stdout.strip()
    if dirty == "DIRTY" and os.environ.get("ORACLE_ALLOW_DIRTY") != "1":
        raise SystemExit("ERROR: working tree is dirty -- oracle bins must build from a COMMITTED tree "
                         "so 'tested == releasable' holds. Commit first, or set ORACLE_ALLOW_DIRTY=1 "
                         "to override (the tested source will then differ from any release).")
    print("[build] staging HEAD -> %s" % stage_dir)
    subprocess.run([BASH, _bp(os.path.join(FW_DIR, "scripts", "stage_source.sh")),
                    "HEAD", "", _bp(stage_dir)], check=True, cwd=FW_DIR)
    st_fw = os.path.join(stage_dir, "Clip-Boy")
    # provision tools/ exactly like release_from_staging.sh (staging strips them)
    subprocess.run([BASH, "-c", 'cp -r "%s/tools" "%s/tools" 2>/dev/null || true'
                    % (_bp(FW_DIR), _bp(st_fw))], check=False)
    out = {}
    cache = os.path.join(os.environ.get("LOCALAPPDATA", ""), "arduino", "sketches").replace("\\", "/")
    for sku, flag in (("sn34k", []), ("res34rch", ["--res34rch"])):
        print("[build] %s --test from staging..." % sku)
        subprocess.run([BASH, "scripts/build.sh", "--test"] + flag, check=True, cwd=st_fw)
        # ⚠ Both SKUs share the sketch PATH, so build.sh writes to the same build-dir / arduino
        # sketch-cache hash -> the NEXT build overwrites this bin. Find the just-built bin (staged
        # build/ OR the arduino cache) and COPY it to a distinct per-SKU name IMMEDIATELY.
        r = subprocess.run([BASH, "-c",
            "ls -t '%s'/build/*/Clip-Boy.ino.bin '%s'/*/Clip-Boy.ino.bin 2>/dev/null | head -1"
            % (_bp(st_fw), cache)], capture_output=True, text=True)
        src = r.stdout.strip()
        if src:
            dst = os.path.join(st_fw, "oracle-%s-app.bin" % sku)
            subprocess.run([BASH, "-c", "cp '%s' '%s'" % (src, _bp(dst))], check=True)
            out[sku] = dst
        else:
            out[sku] = None
    return out

# ── Fast flash (validated 2026-07-30: stub -z @0x10000, 921600 -> ~64s for the 7MB app on these
#    Waveshare boards, hash-verified, badge boots. Same fast/ROM-fallback shape as production
#    flash_core.py). App-only is enough for a --test reflash: boot/part/littlefs don't change. ──
ESPTOOL = os.path.join(os.environ.get("LOCALAPPDATA", ""), "Arduino15", "packages", "esp32",
                       "tools", "esptool_py", "4.5.1", "esptool.exe")

def fast_flash(port, appbin, timeout=300):
    """Fast stub flash of the app at 0x10000; ROM --no-stub fallback if the stub write doesn't
    hash-verify (the recycled-board wedge path). Returns (ok, note)."""
    def _run(cmd):
        try:
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            return False, "timeout"
        return ("Hash of data verified" in p.stdout and p.returncode == 0), ""
    ok, _ = _run([ESPTOOL, "--chip", "esp32s3", "--port", port, "--baud", "921600",
                  "write_flash", "-z", "0x10000", appbin])
    if ok:
        return True, "stub"
    ok, _ = _run([ESPTOOL, "--chip", "esp32s3", "--port", port, "--baud", "460800",
                  "--no-stub", "write_flash", "0x10000", appbin])
    return ok, ("rom-fallback" if ok else "FAILED (reflash / BOOT+RESET needed)")

# ── Badge SKU auto-detect (re-assert identity per the multi-badge rule) ──────────────────────
def detect_sku(port):
    """Open the harness and read tool_list -> 'res34rch' if the active cats are present else
    'sn34k'. Never trust a fixed port==SKU mapping (reconnect-by-autodetect bit us before)."""
    from harness import Harness, HarnessTimeout
    h = None
    try:
        h = Harness(port=port)
        return "res34rch" if h.cat_pos("Deauth") is not None else "sn34k"
    except Exception as e:
        return "?(%s)" % type(e).__name__
    finally:
        try:
            if h: h.close()
        except Exception:
            pass

# ── Witness-not-ready self-check (decision 1) ────────────────────────────────────────────────
def witness_ready():
    """True if the kali RF witness answers a preflight. A live-image witness that was not
    re-set-up this boot fails here -> all T2 become CANNOT-TEST, never a false FAIL."""
    try:
        import rf_pcap
        return bool(rf_pcap.preflight(chan=6))
    except Exception:
        return False

# ── Run one test ─────────────────────────────────────────────────────────────────────────────
def _detail(stdout, stderr="", maxlen=280):
    """Extract a MEANINGFUL one-liner from a test's output for the manifest. The suites print a
    '====' separator as their FINAL line, so a naive [-1] records the separator and nothing else
    -- which is exactly why the first real run's manifest had detail-free T1 FAILs (both pre-review
    agents flagged this). Drop blanks + pure separators, prefer lines that NAME the failure, else
    keep the last few meaningful lines. Include stderr so a crash traceback isn't lost."""
    lines = [ln.strip() for ln in ((stdout or "") + "\n" + (stderr or "")).splitlines()]
    lines = [ln for ln in lines if ln and (set(ln) - set("=-_ "))]   # drop blank + pure-separator
    if not lines:
        return ""
    KEY = ("FAIL", "ERROR", "Error", "missing", "CANNOT", "cannot", "assert", "Exception",
           "Traceback", "precondition", "deadline", "timeout", "unreach")
    hits = [ln for ln in lines if any(k in ln for k in KEY)]
    picked = (hits[-3:] if hits else lines[-3:])
    return " | ".join(picked)[:maxlen]

def run_one(spec, port=None, timeout=600):
    argv = [sys.executable, os.path.join(HERE, spec.cmd[0])] + spec.cmd[1:]
    if port:
        argv += ["--port", port]
    env = dict(os.environ, CLIPBOY_PORT=port or "")
    try:
        p = subprocess.run(argv, capture_output=True, text=True, timeout=timeout, env=env, cwd=FW_DIR)
        rc = p.returncode
    except subprocess.TimeoutExpired:
        # A subprocess TIMEOUT on the recycled boards is the native-USB-CDC endurance wedge
        # (hundreds of serial reconnects over a long staged run), NOT a firmware fault -- the
        # same test passes solo on a fresh CDC (teardown-paths: proven 17/17 twice). A timeout
        # means the test did not COMPLETE, i.e. we could NOT determine a verdict -- that is
        # CANNOT-TEST, not "firmware defect detected". CANT is owner-ackable (rc=3) and surfaces
        # a row the owner must review, so a real firmware hang can't silently pass either; it
        # shows up as CANNOT-TEST with "verify solo". An assertion FAILURE still returns FAIL
        # below (rc != 0/2); only a hard timeout is reclassified. Owner-blessed 2026-07-31.
        return CANT, "timeout (native-CDC endurance / host stall, not a firmware fail -- verify solo)"
    if rc == 0:
        return PASS, ""
    if rc == 2:
        return CANT, (_detail(p.stdout, p.stderr) or "cannot-test")
    return FAIL, (_detail(p.stdout, p.stderr) or "fail")

# ── Orchestration ────────────────────────────────────────────────────────────────────────────
def plan(fleet):
    """fleet: {port: sku}. Yields (spec, [ports]) -- the 2x2 both-must-agree assignment."""
    for spec in REGISTRY:
        if spec.tier == "T0":
            yield spec, [None]                       # host, run once
        else:
            ports = [p for p, s in fleet.items() if s in spec.skus]
            if ports:
                yield spec, ports

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ports", default="", help="comma list, e.g. COM4,COM5,COM6,COM7")
    ap.add_argument("--assign", default="", help="intended layout, e.g. "
                    "COM4:sn34k,COM5:res34rch,COM6:sn34k,COM7:res34rch. With --build, flashes "
                    "the intended SKU per port (handles a non-responsive badge auto-detect can't "
                    "classify); auto-detect then VERIFIES each port matches.")
    ap.add_argument("--build", action="store_true", help="stage+build --test bins, flash per SKU")
    ap.add_argument("--dry-run", action="store_true", help="print the plan, touch no hardware")
    ap.add_argument("--tests", default="", help="comma list of test NAMES to run this pass (a STAGE). "
                    "Omitted = all. Lets a long run be split into stages with a physical un/replug "
                    "between them, so each stage starts on a fresh native-CDC state and no single "
                    "stage accumulates enough serial reconnects to wedge it. Merge the per-stage "
                    "--out manifests with oracle_merge.py for the final gate verdict.")
    ap.add_argument("--out", default=os.path.join(FW_DIR, "docs", "test-reports",
                    "oracle-manifest.json"))
    a = ap.parse_args()
    ports = [p.strip() for p in a.ports.split(",") if p.strip()]
    tset = {t.strip() for t in a.tests.split(",") if t.strip()} or None
    if tset:
        unknown = tset - {t.name for t in REGISTRY}
        if unknown:
            print("ERROR: unknown --tests names: %s" % ", ".join(sorted(unknown))); return 2
        print("STAGE: running only %d of %d tests: %s" % (len(tset), len(REGISTRY), ",".join(sorted(tset))))

    if a.dry_run:
        # Fake a 2x2 fleet so the plan is inspectable without hardware.
        fleet = {(ports[i] if i < len(ports) else "COM?%d" % i):
                 ("sn34k" if i < 2 else "res34rch") for i in range(4)}
        print("DRY RUN -- assumed fleet: %s\n" % fleet)
        wit = "(unknown, dry-run)"
    else:
        assigned = {}
        for kv in a.assign.split(","):
            if ":" in kv:
                p, s = kv.split(":", 1); assigned[p.strip()] = s.strip()
        if assigned:
            fleet = dict(assigned)                    # intended layout drives --build flashing
            if not ports:
                ports = list(assigned)
            print("Intended fleet (from --assign): %s" % fleet)
        else:
            if not ports:
                print("ERROR: --ports or --assign required (or use --dry-run)."); return 2
            print("Detecting SKU on %s ..." % ports)
            fleet = {p: detect_sku(p) for p in ports}
            print("  detected fleet: %s" % fleet)
        wit = witness_ready()
        print("  witness ready: %s" % wit)

    by_tier = {"T0": [], "T1": [], "T2": []}
    for spec, tports in plan(fleet):
        if tset and spec.name not in tset:
            continue                              # STAGE filter: skip tests not in this pass
        by_tier[spec.tier].append((spec, tports))
    for tier in ("T0", "T1", "T2"):
        print("\n== %s (%d tests) ==" % (tier, len(by_tier[tier])))
        for spec, tports in by_tier[tier]:
            tgt = "host" if spec.tier == "T0" else ",".join(tports)
            extra = (" witness=%s" % ",".join(spec.witness)) if spec.witness else ""
            extra += (" [%s]" % spec.note) if spec.note else ""
            print("   %-18s %-24s %s%s" % (spec.name, tgt, spec.skus, extra))

    if a.dry_run:
        print("\n[dry-run] %d tests total. No hardware touched." % sum(len(v) for v in by_tier.values()))
        return 0

    # ── LIVE EXECUTION ──
    if a.build:
        bins = build_from_staging(os.path.join(os.path.dirname(FW_DIR), "clip-boy-src-stage"))
        print("[build] staged --test bins: %s" % bins)
        # flash all ports in PARALLEL via the validated fast stub method (~64s/app, hash-verified,
        # ROM fallback). Same fast path production uses.
        print("[flash] fast parallel (stub -z @0x10000, 921600) + ROM fallback ...")
        with cf.ThreadPoolExecutor(max_workers=len(fleet) or 1) as ex:
            ff = {ex.submit(fast_flash, p, bins[sku]): (p, sku)
                  for p, sku in fleet.items() if bins.get(sku)}
            for fut in cf.as_completed(ff):
                (p, sku) = ff[fut]; ok, note = fut.result()
                print("   %s <- %s (%s): %s [%s]"
                      % (p, os.path.basename(bins[sku]), sku, "OK" if ok else "FAIL", note))
        time.sleep(10)   # post-flash CDC settle before opening harness sessions
        # VERIFY the flash took the intended SKU (re-assert identity; a still-timing-out port is a
        # flash failure, e.g. a CDC-wedged badge that needed a physical BOOT+RESET).
        print("[verify] re-detecting SKU post-flash...")
        for p, want in list(fleet.items()):
            time.sleep(2); got = detect_sku(p)
            ok = (got == want)
            print("   %s: intended=%s detected=%s %s" % (p, want, got, "OK" if ok else "!! MISMATCH"))
            if not ok:
                print("   ^ %s did not come up as %s -- excluding from the run "
                      "(reflash / physical BOOT+RESET needed)." % (p, want))
                fleet.pop(p, None)
        # REBUILD the tier->port plan from the POST-EXCLUSION fleet. by_tier was captured before
        # flashing (to print the plan), so a captured port list still holds excluded/dead badges;
        # without this rebuild a dropped badge is STILL run and FAILs every case, polluting the
        # manifest and dragging its SKU's 2x2 verdict to FAIL. (Bit us on the first real run: COM4
        # flash-failed, was "excluded", yet FAILed all of T1. A SKU with 0 survivors simply yields
        # no rows -- the console exclusion line is the record; the manifest 'fleet' shows survivors.)
        by_tier = {"T0": [], "T1": [], "T2": []}
        for spec, tports in plan(fleet):
            if tset and spec.name not in tset:
                continue                          # STAGE filter (see the main by_tier loop)
            by_tier[spec.tier].append((spec, tports))

    results = {}   # (name, port|host) -> (verdict, detail)

    # T0 once
    for spec, _ in by_tier["T0"]:
        v, d = run_one(spec)
        results[(spec.name, "host")] = (v, d)
        print("  [T0] %-18s %s %s" % (spec.name, v, d))

    # T1 SERIALIZED across badges (REAL FINDING 2026-07-30, corrected after adversarial review).
    # The earlier 4-way parallel run (ThreadPoolExecutor over the fleet) produced badly flaky,
    # MIXED per-badge failures. The dominant coupling is NOT cross-badge USB-CDC re-enumeration
    # (each badge is its own COM port; a reboot re-enumerates only THAT badge) -- it is
    # HOST-LOAD-INDUCED HARNESS READ-TIMEOUT: the per-test bridge starves under 4x concurrent
    # load and trips its read deadline ("bridge read deadline 30s"). Two secondary couplings:
    # shared RF airspace (only the radio-touching tests -- tool-gauntlet, airplane), and shared
    # HOST OUTPUT FILES (screenshot wrote fixed paths -- fixed separately by per-port namespacing).
    # T2 has always been serialized and its 2x2 results are perfectly clean, so serialize T1 the
    # same way: single-badge is ALSO the real ship condition, so serial is the more honest oracle.
    # ⚠ Serialize is SYMPTOM-TREATMENT and does NOT make every T1 test green: genuinely-real fails
    # stay RED (e.g. info-screens asserts a since-scrubbed string -- a stale TEST, tracked to
    # /help-review). ⚠ TARGET DESIGN (follow-up, NOT done): capped-concurrency=2 pairing one
    # sn34k + one res34rch -- half the wall-clock AND it preserves the gate's 2x2 independence
    # (running two SAME-SKU units at once lets a shared transient correlate them into FALSE
    # agreement); plus harden the too-tight bridge read deadline so the tests are concurrency-safe
    # on their own merits rather than needing exclusive hardware.
    for spec, tports in by_tier["T1"]:
        for p in tports:
            v, d = run_one(spec, p)
            results[(spec.name, p)] = (v, d)
            print("  [T1] %-18s %-6s %s %s" % (spec.name, p, v, d))
            time.sleep(INTER_TEST_SETTLE_S)   # let the CDC settle between serial reconnects

    # T2 SERIALIZED on the shared witness rig
    for spec, tports in by_tier["T2"]:
        for p in tports:
            if not wit:
                v, d = CANT, "witness not ready (rig self-check failed)"
            else:
                v, d = run_one(spec, p)
            results[(spec.name, p)] = (v, d)
            print("  [T2] %-18s %-6s %s %s" % (spec.name, p, v, d))
            time.sleep(INTER_TEST_SETTLE_S)   # let the CDC settle between serial reconnects

    # ── Manifest + gate verdict ──
    write_manifest(a.out, fleet, wit, results, by_tier)
    return gate_verdict(results, by_tier, fleet)

def _sku_verdict(results, spec, tports):
    """2x2: PASS only if BOTH units of the SKU agree PASS. Any FAIL -> FAIL. Any CANNOT-TEST
    (and no FAIL) -> CANNOT-TEST. Disagreement (one PASS one FAIL) -> FAIL + unit flag."""
    vs = [results.get((spec.name, p), (CANT, "not run"))[0] for p in tports]
    if FAIL in vs:
        return FAIL, ("unit-disagree" if PASS in vs else "")
    if CANT in vs:
        return CANT, ""
    return PASS, ""

def write_manifest(path, fleet, wit, results, by_tier):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    rows = []
    for tier in ("T0", "T1", "T2"):
        for spec, tports in by_tier[tier]:
            if tier == "T0":
                v, d = results.get((spec.name, "host"), (CANT, "not run"))
                rows.append(dict(tier=tier, name=spec.name, sku="host", verdict=v, detail=d))
            else:
                v, flag = _sku_verdict(results, spec, tports)
                rows.append(dict(tier=tier, name=spec.name, sku="/".join(spec.skus),
                                 verdict=v, flag=flag,
                                 per_badge={p: results.get((spec.name, p), (CANT, ""))[0]
                                            for p in tports}))
    manifest = dict(fleet=fleet, witness_ready=wit, rows=rows,
                    summary={v: sum(1 for r in rows if r["verdict"] == v) for v in (PASS, FAIL, CANT)})
    with open(path, "w") as f:
        json.dump(manifest, f, indent=2)
    print("\n[manifest] %s" % path)
    print("  summary: %s" % manifest["summary"])

def gate_verdict(results, by_tier, fleet):
    """Gate policy (owner decision 2026-07-31): a real FAIL is a hard fail; an honest CANNOT-TEST --
    where a FIXTURE is absent (no SD card, a down RF witness, no controlled client) -- is
    OWNER-ACKABLE (rc=3 = 'needs ack'), NOT a hard fail. This is honest coverage accounting, not a
    loophole: every CANNOT-TEST is named in the manifest and the owner reviews+acks before release.
      - T0 host/source gates: STRICT -- any non-PASS (incl. a check that could not RUN) hard-fails
        (a host gate has no legitimate 'fixture absent' reason; an exception there is a real problem).
      - T1 + T2 badge tiers: a SKU verdict of FAIL hard-fails; a SKU verdict of CANNOT-TEST -> ack.
    rc: 1=FAIL, 3=NEEDS-ACK, 0=PASS. (Previously T1 CANNOT-TEST hard-failed, conflating 'no SD card
    in this rig' with 'the firmware is broken' -- wrong for a fixture-dependent test like sd-card.)"""
    hard_fail = False
    needs_ack = False
    # T0 host gates: strict.
    for spec, _ in by_tier["T0"]:
        if results.get((spec.name, "host"), (CANT,))[0] != PASS:
            hard_fail = True
    # T1 + T2 badge tiers: FAIL -> hard; CANNOT-TEST -> owner-ack.
    for tier in ("T1", "T2"):
        for spec, tports in by_tier[tier]:
            v = _sku_verdict(results, spec, tports)[0]
            if v == FAIL:
                hard_fail = True
            elif v == CANT:
                needs_ack = True
    if hard_fail:
        print("\nGATE: FAIL (a T0 gate did not PASS, or a T1/T2 oracle FAILed)")
        return 1
    if needs_ack:
        print("\nGATE: NEEDS OWNER ACK -- every real test PASSed, but some T1/T2 were CANNOT-TEST "
              "(a fixture was absent: e.g. no SD card on this badge, a down RF witness). Review the "
              "manifest's CANNOT-TEST rows and ack before release.")
        return 3
    print("\nGATE: PASS -- all tiers green.")
    return 0

if __name__ == "__main__":
    sys.exit(main())
