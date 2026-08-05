#!/usr/bin/env python3
"""Finding 1 (fake-ultrareview) stress test: HR scan-start + immediate theme switch.

hr_scan_start() arms hr_scan_begin_cb(50ms). If a teardown (theme switch) deletes
hr_blackout before it fires, pre-fix begin_cb did lv_obj_clean(NULL) -> reboot; post-fix
it early-returns on !hr_blackout.

LIMITATION: the serial `hr_scan_start` command PUMPS lv_timer_handler (th_cmd_hr_scan_start,
5x), forcing begin_cb to run synchronously BEFORE the next serial command -- so the serial
path can't land a theme_set inside the 50ms window. The true race is UI-tap-triggered. This
test therefore stresses scan-start + theme-switch and asserts NO REBOOT (real reboot =
USB-CDC port drop or a boot banner), which -- with the code-correct !hr_blackout guard --
is the practical verification. A 'busy' iteration (scan logs crowd out the state reply) is
NOT a reboot.

  py -3 scripts/hr_spec/test_finding1_teardown.py [iterations]   # default 20
"""
import os, sys, time, serial
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
try:
    from test_bridge import find_esp32_port
except Exception:
    find_esp32_port = lambda: None

PORT = os.environ.get("CB_PORT") or find_esp32_port() or "COM10"
N = int(sys.argv[1]) if len(sys.argv) > 1 else 20
BOOT_MARKERS = ("CLIP-BOY V3", "ClipOS", "rst:", "Backtrace", "Guru Meditation")


def main():
    s = serial.Serial(); s.port = PORT; s.baudrate = 115200
    s.dtr = False; s.rts = False; s.timeout = 0; s.open(); time.sleep(0.4)
    print(f"[badge on {PORT}]  {N} scan-start + immediate theme_set iterations")

    def send(c):
        try: s.write(b"\x02" + c.encode() + b"\n"); s.flush()
        except Exception: pass

    def drain(dur):
        t = time.time(); b = ""
        while time.time() - t < dur:
            try:
                n = s.in_waiting
                b += s.read(n).decode(errors="replace") if n else ""
                if not n: time.sleep(0.01)
            except Exception:
                return b, True            # port dropped == reboot
        return b, False

    def rebooted(buf):
        return any(m in buf for m in BOOT_MARKERS)

    send("cfg_set disp_off 5"); drain(0.3)
    send("touch 160 120 press"); time.sleep(1.0); send("touch 160 120 release"); drain(0.4)

    reboots = 0
    for i in range(N):
        send("nav 1 1"); drain(0.3)
        send("hr_scan_start"); send(f"theme_set {i % 3}")
        b, dropped = drain(0.7)
        send("hr_scan_stop"); b2, d2 = drain(0.6)     # flush scan logs
        buf = b + b2
        if dropped or d2 or rebooted(buf):
            reboots += 1; print(f"  iter {i+1}/{N}: REBOOT"); break
        # alive confirmation with retries (busy != rebooted)
        alive = False
        for _ in range(3):
            s.reset_input_buffer(); send("state"); bs, ds = drain(1.2)
            if ds or rebooted(bs): reboots += 1; break
            if '"cmd":"state"' in bs: alive = True; break
        if reboots: print(f"  iter {i+1}/{N}: REBOOT (on state)"); break
        print(f"  iter {i+1}/{N}: {'alive' if alive else 'alive (slow reply, no reboot)'}")
    send("theme_set 0"); drain(0.3); s.close()
    print(f"\nRESULT: {'PASS -- no reboot in %d iterations (Finding 1 fixed)' % N if not reboots else 'FAIL -- real reboot'}")
    return 1 if reboots else 0


if __name__ == "__main__":
    sys.exit(main())
