#!/usr/bin/env py -3
"""Unlock-feature test: the two reward-gated UI features that the tool/ARG
suites don't reach.

  #3 Flying-Clippy screensaver style (cfg.ss_style==2) — ARG reward. Gated by
     arg_quanta_earned(); screensaver_activate() (ui_nav.h:8169) resets ss_style
     2->0 if the ARG isn't complete. We drive the guard deterministically: set
     ss_style=2, force a 15s idle (disp_off=0) so the screensaver fires, then
     read ss_style back. NOT-earned -> reset to 0 (gate holds); earned -> stays 2.

  #4 Custom UI theme (THEME_CUSTOM=6) — 100%-collectibles reward. theme_set is
     the test hook that applies it directly (the coll_all_found() gate lives in
     the settings dropdown, theme_changed_cb ui_nav.h:7803). We verify it applies
     + renders and that the completionist overlays (reveal + hue picker) exist.

  py -3 scripts/tests/test_unlock_features.py [--port COM11]

--test build (TEST_HARNESS + COLL_DEBUG). Restores ARG/coll/cfg on exit.
"""
import argparse, sys, time, re
import serial
import serial.tools.list_ports as lp

STX = b"\x02"

def find_port(p):
    if p: return p
    for d in lp.comports():
        if (d.vid in (0x303A, 0x1A86)) or "USB" in (d.description or ""): return d.device
    return None

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--port"); a = ap.parse_args()
    port = find_port(a.port)
    if not port: print("no badge found"); return 2
    s = serial.Serial(); s.port = port; s.baudrate = 115200
    s.dtr = False; s.rts = False; s.timeout = 0.2; s.write_timeout = 4
    s.open(); time.sleep(0.6); s.reset_input_buffer()

    def read_until(sentinel, idle=0.25, cap=6.0):
        buf = bytearray(); t0 = time.time(); last = time.time()
        while time.time() - t0 < cap:
            chunk = s.read(4096)
            if chunk: buf += chunk; last = time.time()
            else:
                if sentinel in buf and time.time() - last >= idle: break
                if time.time() - last >= cap * 0.5: break
        return buf.decode("utf-8", "replace")

    def plain(c, cap=6.0):
        s.write((c + "\n").encode()); s.flush(); return read_until(b"> ", cap=cap)
    def th(c, cap=4.0):
        s.write(STX + c.encode() + b"\n"); s.flush(); return read_until(STX, cap=cap)

    def cfg_get(key):
        for _ in range(3):
            m = re.search(r'"value":(-?\d+)', th(f"cfg_get {key}"))
            if m: return int(m.group(1))
            time.sleep(0.2)
        return None
    def state_ss():
        for _ in range(3):
            m = re.search(r'"screensaver":(true|false)', th("state"))
            if m: return m.group(1) == "true"
            time.sleep(0.2)
        return None
    def arg_progress():
        m = re.search(r"progress=0x([0-9A-Fa-f]+)", plain("clipcli marshal --dump"))
        return int(m.group(1), 16) if m else None
    def arg_reset_verified():
        # `clipcli reset all` needs a Y confirm; the native CDC can drop it, leaving
        # ARG still earned from a prior playthrough. Retry until progress actually 0.
        for _ in range(4):
            plain("clipcli reset all"); plain("Y")
            if arg_progress() == 0: return True
            time.sleep(0.3)
        return False

    fails = []
    def check(name, ok, extra=""):
        print(("  PASS " if ok else "  FAIL ") + name + (" " + extra if extra else ""))
        if not ok: fails.append(name)

    print(f"[unlock-features] port {port}")
    orig_disp = cfg_get("disp_off")
    orig_theme = cfg_get("theme")

    def wake():
        # dismiss the screensaver (tap-and-hold ~2s) only if it's up, then reset the
        # idle timer with a tap on the inert status bar (avoids hitting a control).
        if state_ss():
            th("touch 160 120 press"); time.sleep(2.4); th("touch 160 120 release"); time.sleep(1.0)
        th("touch 160 5 tap"); time.sleep(0.5)

    # ══ #3 Flying-Clippy screensaver — ARG gate ══════════════════════════════
    print("\n-- #3 Flying-Clippy screensaver (ss_style=2), ARG-gated --")
    # Case A: ARG NOT earned -> the guard must knock ss_style back to 0.
    check("A: ARG reset to not-earned (progress 0)", arg_reset_verified(),
          f"(progress {arg_progress()})")
    wake()   # dismiss any active screensaver so the idle guard runs on a FRESH activation
    th("cfg_set ss_style 2"); th("cfg_set disp_off 0")
    got2 = cfg_get("ss_style")
    check("A: ss_style accepted as 2 (pre-idle)", got2 == 2, f"(={got2})")
    print("     forcing 15s idle for screensaver...")
    time.sleep(18.0)
    ss_active = state_ss(); ss_val = cfg_get("ss_style")
    check("A: screensaver fired on idle", ss_active is True, f"(screensaver={ss_active})")
    check("A: NOT-earned guard reset ss_style 2->0", ss_val == 0, f"(ss_style={ss_val})")
    wake()
    check("A: woke from screensaver", state_ss() is False, f"(screensaver={state_ss()})")

    # Case B: grant the ARG (marshal 1..5) -> ss_style=2 must survive the idle guard.
    for n in range(1, 6):
        plain(f"clipcli marshal {n} complete")
    prog = re.search(r"progress=0x([0-9A-Fa-f]+)", plain("clipcli marshal --dump"))
    check("B: ARG granted (progress 0x1F)", bool(prog) and prog.group(1).upper() == "1F",
          f"(progress {prog.group(1) if prog else '?'})")
    wake()   # fresh activation so the guard evaluates the now-earned state
    th("cfg_set ss_style 2"); th("cfg_set disp_off 0")
    print("     forcing 15s idle for screensaver...")
    time.sleep(18.0)
    ss_active = state_ss(); ss_val = cfg_get("ss_style")
    check("B: screensaver fired on idle", ss_active is True, f"(screensaver={ss_active})")
    check("B: earned -> Flying-Clippy ss_style stays 2", ss_val == 2, f"(ss_style={ss_val})")
    wake()
    check("B: woke from screensaver", state_ss() is False, f"(screensaver={state_ss()})")

    # ══ #4 Custom UI theme — 100%-collectibles reward ════════════════════════
    print("\n-- #4 Custom UI theme (THEME_CUSTOM=6), 100%-collectibles reward --")
    plain("cli-none"); th("cfg_set disp_off 5")   # 'Never' so it can't nap mid-test
    plain("coll add all", cap=12.0)
    ch = re.search(r'"changed":(true|false)', th("theme_set 6"))
    check("theme_set 6 applied at 100% (changed)", bool(ch) and ch.group(1) == "true",
          f"(changed {ch.group(1) if ch else '?'})")
    tval = cfg_get("theme")
    check("cfg.theme == 6 (Custom active)", tval == 6, f"(theme {tval})")
    r1 = re.search(r'"ok":(true|false)', th("info_show reveal"))
    check("100% custom-reveal overlay renders", bool(r1) and r1.group(1) == "true")
    time.sleep(0.6)
    th("theme_set 6")   # rebuild back to the theme screen after the overlay
    r2 = re.search(r'"ok":(true|false)', th("info_show huepicker"))
    check("completionist hue-picker overlay renders", bool(r2) and r2.group(1) == "true")
    time.sleep(0.6)

    # ══ restore ══════════════════════════════════════════════════════════════
    print("\n-- restore --")
    th(f"theme_set {orig_theme if orig_theme is not None else 0}")
    th("cfg_set ss_style 0")
    th(f"cfg_set disp_off {orig_disp if orig_disp is not None else 3}")
    arg_reset_verified()
    plain("coll reset")
    rt = cfg_get("theme"); rs = cfg_get("ss_style")
    check("restore: theme + ss_style reset", rt == (orig_theme or 0) and rs == 0,
          f"(theme {rt}, ss_style {rs})")

    s.close()
    print("\n[unlock-features] " + ("ALL PASS" if not fails else f"FAILED: {fails}"))
    return 0 if not fails else 1

if __name__ == "__main__":
    sys.exit(main())
