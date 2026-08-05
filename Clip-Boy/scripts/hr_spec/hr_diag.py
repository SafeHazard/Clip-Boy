#!/usr/bin/env py -3
"""All-in-one anchor-scan diagnostic (DC34-155), reboot-free.

Opens the port with dtr=False/rts=False so it does NOT reset the badge (unlike
the bridge, which toggles DTR on open and reboots — clearing anchor mode). In
one session it: disables the screensaver, wakes it, enables anchor mode, starts
a scan, then dumps the decode intermediates every ~1s for 10s while you hold the
tag. Prints the last couple of frames that reached the sampling stage.

    py -3 scripts/hr_spec/hr_diag.py [COM10]

Present the anchor tag (bump-side, flat, centered) when it says "PRESENT". If the
harness can't start the scan, just tap Scan on the badge during the capture
window and it'll still catch the frames.
"""
import sys, time
import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM10"
s = serial.Serial()
s.port = port; s.baudrate = 115200
s.dtr = False; s.rts = False          # no reset on open
s.timeout = 0.3
s.open(); time.sleep(0.5); s.reset_input_buffer()

def send(c, w=0.3):
    s.write(b"\x02" + c.encode() + b"\n"); s.flush(); time.sleep(w)
    return s.read(30000).decode("utf-8", "replace")

def scanning():
    return '"hr_scanning":true' in send("state", 0.2)

send("cfg_set disp_off 5", 0.3)                       # screensaver -> Never
send("touch 160 120 press", 0.1); time.sleep(2.6); send("touch 160 120 release", 0.6)  # wake saver
print("anchor:", "on" if '"anchor":true' in send("hr_anchor 1", 0.3) else "??")

started = False
for _ in range(4):
    send("nav 1 1", 0.4); send("hr_scan_start", 0.6)
    for _ in range(8):
        if scanning(): started = True; break
        time.sleep(0.4)
    if started: break
    send("hr_scan_stop", 0.6); time.sleep(1.4)
print(f"scan {'started via harness' if started else 'NOT auto-started -> tap Scan on the badge now'}")

print(">>> PRESENT THE ANCHOR TAG NOW (bump-side, flat, centered). Capturing ~25s...")
import re
dumps = []
t0 = time.time()
locked = False
while time.time() - t0 < 25.0:
    d = send("hr_debug", 0.35)                     # tighter poll -> catch the lock transition
    b = d.find("HR_DEBUG_BEGIN"); e = d.find("HR_DEBUG_END")
    if b < 0 or e <= b:
        continue
    frame = d[b:e + 12]
    dumps.append(frame)
    # Live one-liner so lockRun_ accumulation is visible (was the blind spot:
    # the modal nav tears the scan down right after lock, freezing the dump).
    md = re.search(r"decode: id=(-?\d+) status=(\w+)", frame)
    ms = re.search(r"syn=(-?\d+)\s+ovl=(-?\d+)\s+rotation=(-?\d+)", frame)
    mr = re.search(r"run=(\d+)/(\d+) lockedId=(-?\d+)", frame)
    if md and mr:
        syn = ms.group(1) if ms else "?"; ovl = ms.group(2) if ms else "?"
        rot = ms.group(3) if ms else "?"
        print(f"  id={md.group(1):>3} {md.group(2):<8} syn={syn} ovl={ovl} rot={rot:>3}  "
              f"run={mr.group(1)}/{mr.group(2)} locked={mr.group(3)}")
        if int(mr.group(3)) >= 0:
            print(f"  *** LOCKED id {mr.group(3)} ***")
            locked = True; break
send("hr_scan_stop", 0.4)
s.close()

good = [d for d in dumps if "4x4 bits" in d]
if good:
    print(f"\n===== captured {len(dumps)} frames; showing last {min(3,len(good))} =====")
    for d in good[-3:]:
        print(d); print("-" * 60)
else:
    print(f"\nno decode frames captured ({len(dumps)} dumps). Was the tag in view / scan active?")
    if dumps: print(dumps[-1])
