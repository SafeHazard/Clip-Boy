#!/usr/bin/env python3
"""check_security_regressions.py — source-level guard for the Jun-2026 security audit fixes.

Fails (exit 1) if any audited fix has been reverted in source. Pairs with the
binary/string checks in check_sku_binaries.py (RELEASE HYGIENE + SKU gate) and the
runtime bounds proof in verify_sniffer_bounds.py. Cheap, no device/build needed —
meant to run in the .bin-generation chain (make_release_bins.sh) and any CI.

Each check cites the audit finding it guards. See memory: security_audit.md.
"""
import os
import re
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
CB = os.path.join(REPO, "libs", "ClipBoy", "src")


def read(*parts):
    p = os.path.join(REPO, *parts)
    with open(p, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


CHECKS = []


def check(finding):
    def deco(fn):
        CHECKS.append((finding, fn.__doc__.strip(), fn))
        return fn
    return deco


@check("C1")
def c1_coll_debug_gated():
    """COLL_DEBUG must NOT be unconditionally #define'd (cheat cmds would ship)."""
    txt = read("ui_collectibles.h")
    # any line that enables it without being commented out
    for line in txt.splitlines():
        if re.match(r"\s*#define\s+COLL_DEBUG\b", line):
            return f"uncommented '#define COLL_DEBUG' in ui_collectibles.h: {line.strip()!r}"
    return None


@check("A1")
def a1_factory_reset_wipes_creds():
    """cfg_factory_reset() must wipe saved WiFi creds (wifi_creds_wipe_all)."""
    txt = read("ui_config.h")
    m = re.search(r"cfg_factory_reset\s*\(void\)\s*\{(.*?)\n\}", txt, re.S)
    if not m:
        return "cfg_factory_reset() not found"
    if "wifi_creds_wipe_all()" not in m.group(1):
        return "cfg_factory_reset() no longer calls wifi_creds_wipe_all()"
    if "p.clear()" not in read("ui_config.h") or "wifi_join" not in read("ui_config.h"):
        return "wifi_creds_wipe_all() must clear() the wifi_creds AND wifi_join namespaces"
    return None


@check("A2")
def a2_savepcap_default_off():
    """SavePCAP must default to false (no auto-write of captures to SD)."""
    txt = read("libs", "ClipBoy", "src", "settings.cpp")
    if 'jsonBuffer["Settings"][2]["value"] = true' in txt:
        return "SavePCAP default is true again (settings.cpp [2].value)"
    if 'jsonBuffer["Settings"][2]["value"] = false' not in txt:
        return "SavePCAP default line not found/changed (verify settings.cpp index)"
    return None


@check("B1")
def b1_extract_manufacturer_bounded():
    """extractManufacturer() must take a frame length (sig_len) to bound its walk."""
    h = read("libs", "ClipBoy", "src", "WiFiScan.h")
    if re.search(r"extractManufacturer\s*\(\s*const\s+uint8_t\s*\*\s*payload\s*\)", h):
        return "extractManufacturer() reverted to no-length signature (unbounded walk)"
    if "extractManufacturer(const uint8_t* payload, int len)" not in h:
        return "extractManufacturer(payload, len) signature not found in WiFiScan.h"
    cpp = read("libs", "ClipBoy", "src", "WiFiScan.cpp")
    if "while (pos < 512)" in cpp:
        return "extractManufacturer still uses the unbounded 'while (pos < 512)' loop"
    return None


@check("B3")
def b3_evilportal_strlcpy_dest_bound():
    """EvilPortal strlcpy must bound on the DESTINATION size, not strlen(src)."""
    txt = read("libs", "ClipBoy", "src", "EvilPortal.cpp")
    # the dangerous pattern: strlcpy(..., ..., strlen(...))
    for m in re.finditer(r"strlcpy\([^;]*?,\s*strlen\(", txt):
        return f"strlcpy bound on strlen(src) (heap overflow) survives: {m.group(0)!r}"
    if "Serial.readString().c_str()" in txt:
        return "EvilPortal uses dangling Serial.readString().c_str() temporary"
    return None


@check("A5")
def a5_join_password_not_logged():
    """The 'join' serial handler must not print the WiFi password in cleartext."""
    txt = read("libs", "ClipBoy", "src", "CommandLine.cpp")
    if 'Password: " + (String)password' in txt:
        return "join handler logs the password in cleartext again (CommandLine.cpp)"
    return None


# Optional hook, swapped in by verify_redfirst.py. Tree-shaped claims ("does a dev .bat
# EXIST anywhere under libs/ClipBoy?") cannot be re-checked by materialising a handful of
# named files -- os.walk over that subset finds nothing and the check reports a blind pass,
# which is why pre-OSS was the one check that stayed VACUOUS after it got a baseline. This
# lets the SAME predicate run against a historical commit's real file list, so there is one
# implementation of the rule rather than a second copy inside the verifier.
LIST_TREE = None


def tracked_files():
    """Repo-relative, forward-slashed paths of candidate source files."""
    if LIST_TREE is not None:
        return LIST_TREE()
    out = []
    for root, _dirs, files in os.walk(REPO):
        if os.sep + "build" in root or os.sep + "release" in root or os.sep + ".git" in root:
            continue
        for fn in files:
            rel = os.path.relpath(os.path.join(root, fn), REPO)
            out.append(rel.replace(os.sep, "/"))
    return out


@check("pre-OSS")
def hygiene_no_dev_identity():
    """No dev-identity paths/usernames or dev .bat helpers in tracked source."""
    bad = []
    for rel in tracked_files():
        if rel.lower().endswith(".bat") and "ClipBoy" in os.path.dirname(rel):
            bad.append(rel)
    # known leaked username from the removed zzzUpdate.bat
    for f in ("libs/ClipBoy/src/CommandLine.cpp", "libs/ClipBoy/src/SDInterface.cpp"):
        try:
            if "TomasOwen" in read(*f.split("/")):
                bad.append(f + " contains 'TomasOwen'")
        except OSError:
            pass
    return ("dev-only artifacts present: " + ", ".join(bad)) if bad else None


# ─────────────────────────────────────────────────────────────────────────────
# 2026-07-24 pre-release audit — guards for fixes that were REVIEW-ONLY, i.e. accepted on a
# code read with no runtime test behind them. Those are exactly the fixes most likely to be
# silently undone by a later edit, because nothing fails when they are: no test goes red, and
# the symptom is either a rare crash, a legal exposure, or a wrong number on screen.
#
# Each check below is written so it would have FAILED on the pre-fix source. That direction
# matters: a check authored by reading the FIXED code only proves the code still says what it
# says today, which is not a regression test.
# ─────────────────────────────────────────────────────────────────────────────

@check("SB4")
def sb4_no_evil_portal_credential_sink():
    """The Evil Portal /evil_portal_<n>.log SD sink (startLog) must stay compile-gated.

    !! SCOPE, narrowed 2026-07-28: this check verifies ONE call, `startLog("evil_portal")`.
    It does NOT establish the class "no credentials reach storage" -- its old docstring
    claimed that and was wrong, which is how the buffer_obj.append() half survived for days
    behind a green gate. The append half is SB4b. Keep the two SEPARATE: verify_redfirst does
    one fn() call per check id, so a compound predicate is satisfied at the baseline by
    whichever half is red there -- folding SB4b in here would give it a permanent free pass.
    """
    # The sink is `startLog("evil_portal")` in WiFiScan.cpp -- NOT in EvilPortal.cpp, which is
    # where the first version of this check looked. It passed on the buggy tree (a vacuous green)
    # and only the red-first verification against ce373a5a caught it. Locating the wrong file is
    # indistinguishable from "the bug is absent" unless you test the check against known-bad
    # input, so this check is pinned to the call itself.
    txt = read("libs", "ClipBoy", "src", "WiFiScan.cpp")
    for line in txt.splitlines():
        if re.match(r"\s*#define\s+CB_EVIL_PORTAL_LOG_SD\s+1", line):
            return f"credential sink re-enabled unconditionally: {line.strip()!r}"
    for m in re.finditer(r'^(.*startLog\(\s*"evil_portal"\s*\).*)$', txt, re.M):
        line_no = txt[:m.start()].count("\n") + 1
        # Walk back for the compile-time guard that must wrap it.
        head = "\n".join(txt.splitlines()[max(0, line_no - 6):line_no - 1])
        if not re.search(r"#if\s+defined\(\s*CB_EVIL_PORTAL_LOG_SD\s*\)|#ifdef\s+CB_EVIL_PORTAL_LOG_SD",
                         head):
            return (f"WiFiScan.cpp:{line_no} calls startLog(\"evil_portal\") with no "
                    f"CB_EVIL_PORTAL_LOG_SD guard -- that writes harvested credentials to "
                    f"/evil_portal_<n>.log while tool_info tells the user they are NOT saved "
                    f"to SD, and nothing in the tree ever reads the file")
    return None


@check("SB4b")
def sb4b_evil_portal_append_is_gated():
    """Every buffer_obj.* call in EvilPortal.cpp must be compile-gated (the credential append).

    THE CLASS, not the line. Anchored on the OBJECT (`buffer_obj.`), so a future
    `buffer_obj.write(...)` or a second append in the credential block is caught too --
    keying on `append(line)` would miss both.

    POSITIVE assertion first, because absence is what a rename or a reshape produces for free:
    the credential formatter must still be PRESENT. If someone reshapes the code so the anchor
    disappears, that is a CANNOT-TEST, not a pass. This is the [[gate_blindness_positive_assertions]]
    shape -- SB4's predecessor asserted only "the bad pattern is gone" and stayed green while
    the append half was live.
    """
    txt = read("libs", "ClipBoy", "src", "EvilPortal.cpp")

    # C1 -- positive anchor. Its absence means the check can no longer see its subject.
    if '"u: %s p: %s' not in txt:
        return ("EvilPortal.cpp no longer contains the credential formatter \"u: %s p: %s\" -- "
                "this check can no longer locate what it is guarding. Re-anchor it; do NOT "
                "assume the sink is gone (an un-run check reads exactly like a passing one)")

    # C2 -- the class: every buffer_obj.* call, guarded count >= 1 and unguarded count == 0.
    #
    # !! The guard test is a SCOPE-AWARE BACKWARD WALK, not a fixed look-back window. A plain
    # "is there a #if CB_EVIL_PORTAL_LOG_SD in the preceding N lines?" test is defeatable three
    # ways, all three PROVEN by running the predicate against mutated copies of this file:
    #   A. the call placed in the guard's #else branch      -> passed while creds reached buffer_obj
    #   B. a 2nd unguarded call just past the #endif        -> passed  (and this is the one that
    #      will happen BY ACCIDENT: the guarded block is 3 lines, so any later "also log to X"
    #      addition in the same if(ready) block lands inside a naive window)
    #   C. an empty #ifdef/#endif followed by the real call -> passed
    # Walking back until the FIRST preprocessor directive settles all three: #else/#elif/#endif
    # means we are outside the guarded region, and only the matching #if counts as guarded.
    # Being scope-aware also lets the window be generous without false positives, which a fixed
    # 8-line window was NOT (a correctly-guarded call 9 lines into the block read as unguarded).
    # !! ANCHOR ON THE OBJECT TOKEN `buffer_obj`, not on `buffer_obj.`. A fresh review defeated the
    # `buffer_obj.` form four more ways, all of which still mention the object at least once:
    #   - a clang-format / manual line wrap: `buffer_obj\n        .append(line);`  <- BY ACCIDENT
    #   - pointer alias:   `Buffer *bp = &buffer_obj; bp->append(line);`
    #   - reference alias: `Buffer &b  =  buffer_obj; b.append(line);`
    #   - inverted guard `#if defined(CB_EVIL_PORTAL_LOG_SD) == 0` scored as GUARDED, because the
    #     old GUARD_RE was unanchored at the end.
    # You cannot reach this object without naming it somewhere, so the rule is simply: EVERY
    # mention of `buffer_obj` in this file must sit inside the guard. That is the class.
    # GUARD_RE is now anchored so `== 0` / `!= 1` tails cannot masquerade as the guard.
    GUARD_RE = re.compile(
        r"^\s*#\s*(if\s+defined\(\s*CB_EVIL_PORTAL_LOG_SD\s*\)(\s*&&\s*CB_EVIL_PORTAL_LOG_SD)?"
        r"|ifdef\s+CB_EVIL_PORTAL_LOG_SD)\s*$")
    ANY_DIRECTIVE = re.compile(r"^\s*#\s*(if|ifdef|ifndef|else|elif|endif)\b")
    # Nested directives INSIDE a correct guard (e.g. `#ifdef HAS_SCREEN`, which sits two lines
    # below) must not read as unguarded -- walk past a balanced nested block rather than stopping
    # at its #endif. Tracked with a depth counter while walking backward.
    lines = txt.splitlines()
    guarded = unguarded = 0
    bad = None
    for i, line in enumerate(lines):
        if "buffer_obj" not in line or line.lstrip().startswith("//"):
            continue
        is_guarded = False
        depth = 0                     # nested #if blocks closed between us and the guard
        for j in range(i - 1, max(-1, i - 60), -1):
            m = ANY_DIRECTIVE.match(lines[j])
            if not m:
                continue
            kind = m.group(1)
            if kind == "endif":
                depth += 1            # a nested block ended here; skip over its opener
                continue
            if kind in ("if", "ifdef", "ifndef"):
                if depth:
                    depth -= 1        # that was the nested opener -- keep walking
                    continue
                is_guarded = bool(GUARD_RE.match(lines[j]))
                break
            # #else / #elif at depth 0 => we are in the WRONG arm of the guard
            if not depth:
                is_guarded = False
                break
        if is_guarded:
            guarded += 1
        else:
            unguarded += 1
            if bad is None:
                bad = i + 1                                # report the FIRST offender
    if unguarded:
        return (f"EvilPortal.cpp:{bad} calls buffer_obj.* with no CB_EVIL_PORTAL_LOG_SD guard "
                f"({unguarded} unguarded). Buffer::append gates only on `writing`, which the "
                f"portal path never sets or clears -- it INHERITS an open capture from an "
                f"earlier tool, so harvested credentials land in /pcaps/<n>.pcap (or LittleFS "
                f"with no SD) while tool_info tells the user they are not written to storage")
    if guarded < 1:
        return ("no guarded buffer_obj.* call found in EvilPortal.cpp -- expected exactly the "
                "credential append. If it was deliberately deleted, retire this check "
                "explicitly rather than letting it pass on an empty set")
    return None


@check("FB13")
def fb13_deauth_total_monotonic():
    """A monotonic deauth counter must survive, or the log silently caps mid-burst."""
    txt = read("libs", "ClipBoy", "src", "WiFiScan.cpp")
    if "deauth_total" not in txt:
        return "deauth_total is gone -- burst counts revert to the 32-line display ceiling"
    return None


@check("FB14")
def fb14_monitor_table_window():
    """The Live Devices table must keep its row window AND the data/child offset."""
    txt = read("ui_nav.h")
    if "mon_row_first" not in txt:
        return ("mon_row_first is gone -- mon_sel is both a data index and a child index, so "
                "head-trimming without the offset desyncs the detail strip from the "
                "highlighted row (a wrong-data bug, not just a cosmetic one)")
    return None


@check("R5")
def r5_tour_modal_selfnull():
    """g_tour_modal must self-null on delete, or the screensaver never fires again."""
    txt = read("ui_nav.h")
    # A dangling g_tour_modal makes radio_screen_busy() true for the rest of the boot, which
    # silently defeats the screensaver AND Dark Charge. The registration is the fix.
    if not re.search(r"cb_selfnull_on_delete[^;]*&g_tour_modal", txt):
        return ("g_tour_modal has no identity-checked self-null registration -- a stale "
                "pointer keeps radio_screen_busy() true forever (screensaver + Dark Charge "
                "silently dead for the rest of the boot)")
    return None


@check("W5-ARG")
def w5_arg_boot_theme_gate():
    """The ARG reward gate must test arg.theme_active, not cfg.theme."""
    txt = read("Clip-Boy.ino")
    m = re.search(r"arg_quanta_earned\(\)[^\n]*", txt)
    if not m:
        return "the boot ARG reward gate is gone"
    line = m.group(0)
    if "theme_active" not in line:
        return (f"boot gate reads {line.strip()!r} -- gating on cfg.theme means the exact state "
                f"a user creates by choosing another theme re-applies the reward on EVERY boot, "
                f"reverting their theme + all 8 LEDs and destroying the Rubber Ducky unlock")
    return None


@check("W5-MAC")
def w5_mac_tracker_modulo():
    """The MAC-tracker hash must use % (size 50 is not a power of two)."""
    txt = read("libs", "ClipBoy", "src", "WiFiScan.cpp")
    if re.search(r"hash_mac\(mac\)\s*&\s*\(\s*mac_history_len_half\s*-\s*1\s*\)", txt):
        return ("reverted to `& (mac_history_len_half - 1)`: on a size of 50 that yields only "
                "{0,1,16,17,32,33,48,49}, so 42 slots are unreachable by any lookup while "
                "evict_and_insert still writes them -- one MAC accumulates duplicate entries "
                "with every frame count stuck at 1")
    return None


@check("W5-SAV")
def w5_backup_write_verify_rename():
    """The SD backup must not truncate the only copy before writing the new one."""
    txt = read("ui_collectibles.h")
    m = re.search(r"static bool coll_export_sd\(void\)\s*\{(.*?)\n\}", txt, re.S)
    if not m:
        return "coll_export_sd() is gone"
    body = m.group(1)
    if "COLL_SAV_TMP_PATH" not in body:
        return ("coll_export_sd no longer stages through COLL_SAV_TMP_PATH -- opening the real "
                "path with \"w\" truncates the user's only badge-bound backup before a byte is "
                "written, so any later failure loses ARG progress unrecoverably")
    # The readback is the part that makes the temp write worth anything.
    if "memcmp" not in body:
        return "the temp file is written but never read back and compared (write() succeeding " \
               "only proves the bytes reached the driver, not that they are readable)"
    return None


@check("W5-SAFE")
def w5_join_path_sanitised():
    """Every join-path SSID render must go through cb_safe()."""
    txt = read("ui_nav.h")
    problems = []
    # 1+2: the Join WiFi SSID row and its updater.
    m = re.search(r"static void wifi_join_update_labels\(void\)\s*\{(.*?)\n\}", txt, re.S)
    if not m or "cb_safe" not in m.group(1):
        problems.append("wifi_join_update_labels renders wifi_join_ssid raw")
    # 3+4: every place a saved credential's SSID is DRAWN.
    #
    # This was two negative patterns, and both were dead. The fix RESHAPED the Saved Networks
    # site so the SSID no longer appears in the snprintf arg list -- it goes through `sname` via
    # `strncpy(sname, cb_safe(...))` -- and `[^;]*` cannot cross the intervening `;`. Deleting
    # `cb_safe(` from that line therefore left all three assertions passing, with site 4 of 4
    # unsanitised, in the check whose docstring is "Every join-path SSID render must go through
    # cb_safe()". Verified by injection 2026-07-27. Red-first cannot catch this class: the
    # pattern DID go red at the baseline, so today's green looks earned.
    #
    # So: enumerate instead of pattern-match a remembered shape. CRITERION -- every line that
    # both reads a saved credential's SSID and passes it to something that renders must also
    # call cb_safe() on that line. Fails CLOSED: a new render helper that is not in RENDERERS
    # trips the "no positive sanitisation" arm below rather than passing silently.
    RENDERERS = ("lv_label_set_text", "lv_checkbox_set_text", "make_label",
                 "lv_dropdown_set_options", "snprintf", "strncpy", "lv_textarea_set_text")
    # Copies into the JOIN buffers must stay RAW and must NOT be sanitised: the badge has to
    # associate with the real SSID bytes, and cb_safe() is a lossy display transform (it replaces
    # unsupported glyphs with squares). The picker deliberately joins by index while showing a
    # sanitised label. This exclusion is load-bearing, not an oversight -- when the enumeration
    # first ran it flagged ui_nav.h:5292 and the correct answer was to leave that line alone.
    # Listed by NAME so it fails CLOSED: a copy into some NEW storage destination is reported
    # rather than quietly excused, which is the direction an exclusion list has to fail in.
    STORAGE_DESTS = ("wifi_join_ssid", "wifi_join_pw")
    sanitised = 0
    for ln, line in enumerate(txt.splitlines(), 1):
        if not re.search(r"wifi_creds\[[^\]]+\]\.ssid", line):
            continue
        if not any(r in line for r in RENDERERS):
            continue                      # a comparison, not a render
        if re.search(r"strncpy\s*\(\s*(?:" + "|".join(STORAGE_DESTS) + r")\b", line):
            continue                      # into the join buffer: must stay raw, see above
        if "cb_safe(" in line:
            sanitised += 1
        else:
            problems.append(f"ui_nav.h:{ln} renders a saved-credential SSID without cb_safe()")
    # POSITIVE assertion. Absence-only checks pass for free after a rename or a reformat, which
    # is exactly how this one died; require the sanitisation to be VISIBLY PRESENT at both known
    # sites (the saved-network picker row and the Saved Networks list).
    if sanitised < 2:
        problems.append(f"only {sanitised} saved-credential SSID render(s) call cb_safe(); "
                        f"expected at least 2 (picker row + Saved Networks list)")
    return ("; ".join(problems) + " -- wifi_join_ssid is air-sourced whenever it came from the "
            "AP picker") if problems else None


def main():
    # Windows consoles default to cp1252, and this runner PRINTS each check's docstring. A single
    # non-cp1252 character anywhere in a printed docstring therefore raises UnicodeEncodeError
    # mid-run and ABORTS the remaining checks. That is not hypothetical: adding a "warning" glyph
    # to SB4's docstring on 2026-07-28 cut this gate from 16 checks to 7 (fail-closed, exit 1, so
    # it could not manufacture a green -- but no release could be cut and 9 checks silently stopped
    # running). It was missed because verify_redfirst.py exercises the same predicates while
    # printing only check IDS, so the red-first evidence looked complete.
    # Reconfigure once here rather than policing every docstring forever.
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass          # fail OPEN: a console we cannot reconfigure must not break the gate
    print("=== security-regression source guard ===")
    fails = []
    for finding, desc, fn in CHECKS:
        try:
            err = fn()
        except Exception as e:  # noqa: BLE001
            err = f"check raised {e!r}"
        if err:
            fails.append((finding, err))
            print(f"  [FAIL] {finding}: {err}")
        else:
            print(f"  [ OK ] {finding}: {desc}")
    print("=" * 60)
    if fails:
        print(f"FAIL: {len(fails)} audited fix(es) reverted in source.")
        return 1
    print(f"PASS: all {len(CHECKS)} audited security fixes still present in source.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
