#!/usr/bin/env py -3
"""
DC34 ARG full-playthrough integration test. Plays the entire arc in sequence
via serial — P1 (startgame) -> P2 (hack) -> P3 (dork) -> P4 (captcha) ->
P5 (call) -> Quanta reward — and asserts each stage completes and the reward
applies. Run against a --test build (needs the debug reveal cmds p3pass/p5code).

  py -3 scripts/tests/arg_playthrough.py [--port COM8]

Exit 0 = the whole ARG works end to end. Single serial session, dtr=False (no
DTR resets — avoids the native-CDC lockup from rapid reconnects).
"""
import argparse, sys, time, re

try:
    import serial
    import serial.tools.list_ports as lp
except ImportError:
    print("pyserial required"); sys.exit(2)

def find_port(p):
    if p: return p
    for d in lp.comports():
        if (d.vid in (0x303A, 0x1A86)) or "USB" in (d.description or ""): return d.device
    return None

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--port")
    a = ap.parse_args()
    port = find_port(a.port)
    if not port: print("no badge found"); return 2
    s = serial.Serial(); s.port = port; s.baudrate = 115200
    s.dtr = False; s.rts = False; s.timeout = 0.4; s.write_timeout = 4
    s.open(); time.sleep(0.6); s.reset_input_buffer()
    def sr(c, w=0.45):
        s.write((c + "\n").encode()); s.flush(); time.sleep(w)
        return s.read(12000).decode("utf-8", "replace")
    fails = []
    def check(name, ok, extra=""):
        print(("  PASS " if ok else "  FAIL ") + name + (" " + extra if extra else ""))
        if not ok: fails.append(name)

    print(f"[playthrough] port {port}")
    sr("clipcli reset all"); sr("Y")

    # reveal cmds BEFORE any session (a live session would eat them)
    p3pass = (re.search(r"passphrase=([A-Z]+)", sr("p3pass")) or [None, None])[1]
    p5code = (re.search(r"code=(\d{8})", sr("p5code")) or [None, None])[1]

    # P1
    out = sr("clipcli startgame")
    check("P1 startgame", "0x01" in sr("clipcli marshal --dump"))

    # P2 The Hack — enter --plain, guess candidates (handle re-rolls) until EXACT MATCH
    o = sr("clipcli challenge --plain", 0.6)
    cands = re.findall(r"^\s{3}([A-Z]{8})\s*$", o, re.M)
    won = False
    for _ in range(24):
        for c in cands:
            r = sr(c, 0.35)
            if "EXACT MATCH" in r: won = True; break
        if won: break
    check("P2 hack", won and "0x03" in sr("clipcli marshal --dump"))

    # P3 Dork — canonical walkthrough + the per-badge passphrase.
    # Spine (see arg_p3_dork.h): SOLDER SPOOL starts in the Soldering Annex [12],
    # one EAST past Hardware Hacking Village [7] — grab it there, walk it back to
    # the Gremlin. After the Bouncer's wristband, VIP Mixer [11] drops straight
    # down to the Underqueue [9] -> Queue [13].
    sr("clipcli challenge", 0.6)
    for cmd in ["n","n","e","e","take solder","w","give solder to gremlin","w","n",
                "give sao to mallory","n","use lanyard","d","d","talk to queue"]:
        sr(cmd, 0.4)
    last = sr("say " + (p3pass or "XXXXXX"), 0.5)
    check("P3 dork", "P3 COMPLETE" in last and "0x07" in sr("clipcli marshal --dump"),
          f"(pass {p3pass})")

    # P4 Captcha — the clever XYZZY win
    sr("clipcli challenge", 0.5)
    w = sr("XYZZY", 0.6)
    check("P4 captcha", ("Accepted" in w or "fool" in w) and "0x0F" in sr("clipcli marshal --dump"))

    # P5 The Call — the HMAC unlock + Quanta payoff
    sr("clipcli challenge", 0.5)
    u = sr("clipcli unlock " + (p5code or "00000000"), 1.0)
    dump = sr("clipcli marshal --dump")
    prog = (re.search(r"progress=0x([0-9A-Fa-f]+)", dump) or [None, "?"])[1]
    theme = (re.search(r"theme=(\d)", dump) or [None, "?"])[1]
    check("P5 unlock -> Quanta",
          ("verified" in u or "Quanta" in u) and prog == "1F" and theme == "1",
          f"(progress {prog}, theme {theme})")

    st = sr("clipcli status")
    check("end state = operator/complete", "operator" in st.lower() or "complete" in st.lower())

    s.close()
    print("\n[playthrough] " + ("ALL STAGES PASS — ARG works end to end" if not fails
                                else f"FAILED: {fails}"))
    return 0 if not fails else 1

if __name__ == "__main__":
    sys.exit(main())
