#!/usr/bin/env python3
"""Hardware verification for the PR#6 Drone Remote-ID fix.

Covers the two review findings that could only be settled on hardware:
  B1 (bounds guard): drone_ingest_odid() must REJECT a pack whose declared length
      is shorter than its own MsgPackSize implies (the over-read case), while
      still decoding a valid pack. Driven synthetically via `drone_feed` -- no
      emitter needed, fully deterministic.
  B2 (stack): the ~920 B ODID decode must run on the loopTask, not the small
      nimble_host BLE task. Read nimble_host stack high-water via `drone_stack`.
  Functional: with the emitter live (standalone-dronesim), the Drone ID tool must
      show a contact -- proves the redesigned extract->ring->service->decode path
      works end-to-end on REAL adverts.

RED/GREEN: run with --expect fixed on the fixed build (guard rejects) and
--expect baseline on a pre-fix build with the harness cherry-picked (guard absent
-> the short-declared pack decodes = the bug reproduced). The valid-pack controls
run in BOTH arms, so a rejecting result can never be a dead rig.

  Badge on --port (default COM16). Emitter (standalone-dronesim) on its own board;
  pass --emitter once it is powered + broadcasting to run the functional + loaded-B2
  checks (the badge's Drone ID tool is started here).

Pack fixtures are SPEC-derived (ASTM F3411 message-pack layout: [0xF<v>][25][N] +
N*25-byte messages), NOT read back from our own parser.
"""
import sys, os, time, argparse
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Harness

PROTO = 2

def build_pack(msg_types):
    # ODID message pack: byte0 = (PACKED<<4)|proto, byte1 = SingleMessageSize (25),
    # byte2 = MsgPackSize (N), then N * 25-byte messages (byte0 = (type<<4)|proto).
    hdr = bytes([(0xF << 4) | PROTO, 25, len(msg_types)])
    body = b"".join(bytes([(t << 4) | PROTO]) + b"\x00" * 24 for t in msg_types)
    return (hdr + body).hex()

# A valid MULTI-message pack. Kept to 3 messages (BASIC + LOC + SYS, all within
# checkPackContent limits) so the whole `drone_feed <len> <hex>` line fits under the
# serial harness's command-length limit (a 9-message/228 B pack truncates on the wire).
# 3 + 3*25 = 78 B. This is the buffer the over-read case declares only 28 B of.
PACKM = build_pack([0, 1, 4])
# valid single BASIC_ID message  ->  3 + 25 = 28 B.
PACK1 = build_pack([0])

RESULTS = []
def check(name, cond, detail=""):
    RESULTS.append(cond)
    print(f"[{'PASS' if cond else 'FAIL'}] {name}   {detail}")

def feed(h, declared, hexstr):
    h.cmd("drone_clear")
    r = h.cmd(f"drone_feed {declared} {hexstr}")
    assert isinstance(r, dict) and r.get("ok"), f"drone_feed failed: {r}"
    return r

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM16")
    ap.add_argument("--expect", choices=["fixed", "baseline"], default="fixed")
    ap.add_argument("--emitter", action="store_true",
                    help="standalone-dronesim is powered + broadcasting; run functional + loaded-B2")
    a = ap.parse_args()

    h = Harness(port=a.port)
    p = h.cmd("ping")
    check("ping / harness alive", isinstance(p, dict) and p.get("ok"), p.get("version", ""))

    nm = len(PACKM) // 2   # 78
    # --- B1 controls (both arms): valid packs must decode -> ret=1, count=1 ---
    r = feed(h, nm, PACKM)
    check("B1 control: valid 3-msg pack (len=78) decodes",
          r.get("ret") == 1 and r.get("count") == 1, str(r))
    r = feed(h, 28, PACK1)
    check("B1 control: valid 1-msg pack (len=28) decodes",
          r.get("ret") == 1 and r.get("count") == 1, str(r))

    # --- B1 over-read case: 78-byte buffer, declare only 28, MsgPackSize says 3 ---
    # Fixed: guard rejects (28 < 3+3*25=78). Baseline: reads all 3 messages past the
    # 28 declared bytes -> decodes -> a contact from an advert that "sent" only 28 B.
    r = feed(h, 28, PACKM)
    if a.expect == "fixed":
        check("B1 FIX: short-declared oversized pack REJECTED (ret=0,count=0)",
              r.get("ret") == 0 and r.get("count") == 0, str(r))
    else:
        check("B1 RED (baseline): short-declared pack DECODED = bug reproduced (ret=1)",
              r.get("ret") == 1, str(r))

    # base len<3 guard
    r = feed(h, 2, PACKM)
    check("B1: len<3 rejected", r.get("ret") == 0, str(r))

    # --- B2 stack (informational; compare across fixed vs baseline runs) ---
    s = h.cmd("drone_stack")
    found = isinstance(s, dict) and s.get("nimble_host") == 1
    check("B2: nimble_host task located", found, f"hw_free={s.get('hw_bytes')}B (min ever)")
    print(f"    >>> nimble_host stack high-water ({a.expect} build): {s.get('hw_bytes')} bytes free")

    # --- Functional + loaded B2 (needs the emitter) ---
    if a.emitter:
        # Start Detect > Drone ID so the BLE + WiFi Remote-ID scan runs.
        pos = h.cat_pos("Detect")
        assert pos is not None, "Detect category not found"
        h.tool_start(pos, 6)                      # cat_detect[] item 6 = Drone ID
        print("    Drone ID tool started; listening 20 s for the emitter...")
        best = 0
        for _ in range(20):
            time.sleep(1)
            c = h.cmd("drone_count")
            best = max(best, c.get("count", 0) if isinstance(c, dict) else 0)
            if best > 0:
                break
        check("FUNCTIONAL: emitter contact seen by Drone ID (count>0)", best > 0, f"count={best}")
        s2 = h.cmd("drone_stack")
        print(f"    >>> nimble_host stack high-water AFTER live BLE adverts: {s2.get('hw_bytes')} bytes free")
        check("B2 loaded: nimble_host still has healthy headroom (>512 B)",
              isinstance(s2, dict) and (s2.get("hw_bytes") or 0) > 512, f"hw_free={s2.get('hw_bytes')}B")
        h.tool_stop()

    h.close()
    nfail = sum(1 for c in RESULTS if not c)
    print(f"\n{'='*46}\n{len(RESULTS)-nfail}/{len(RESULTS)} PASSED  ({a.expect} build)")
    sys.exit(1 if nfail else 0)

if __name__ == "__main__":
    main()
