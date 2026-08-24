#!/usr/bin/env python3
"""radroach_test.py [--port COMn] — end-to-end checks for Radroach Ronin.

Drives the game through the firmware test harness (the radroach_* commands in
test_harness.h), so it needs a --test build but no finger and no ToF daughter
board: the whirlwind gesture runs on `sensor_mock` frames.

Every check freezes the game tick (radroach_freeze) and wipes the field
(radroach_reset) first, because the live 1.2 s spawn timer would otherwise put
roaches on screen between two commands and make every count assertion a race.

    py -3.11 scripts/tests/radroach_test.py
"""
import sys, os, argparse
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import Harness

FAILURES = []

# radro_spawn() launches sprites from BELOW the screen (y = SCREEN_H + r), so a
# freshly spawned roach is at y=258 and no on-screen swipe can reach it. Step the
# physics first to toss them into view, then comb the screen.
RISE_STEPS = 8

# Horizontal swipes 30px apart. The hit radius is 18, so a comb at that pitch
# catches a roach at any y, whatever its random toss height turned out to be.
COMB_Y = range(20, 240, 30)


def check(name, cond, detail=""):
    if cond:
        print(f"  PASS  {name}")
    else:
        print(f"  FAIL  {name}  {detail}")
        FAILURES.append(name)


def resync(h, tries=8):
    """Drain late replies until the stream answers the command we just sent."""
    for _ in range(tries):
        r = h.cmd("ping")
        if r.get("ok") and r.get("cmd") == "ping":
            return True
        h.wait(200)
    return False


def ask(h, command, tries=3):
    """Send a command and return ITS reply, recovering from a shifted stream.

    Any harness command that blocks longer than the host's 5 s timeout answers
    after the host gave up, which leaves every later reply one command behind.
    radroach_open does exactly that on a cold VL53L5CX -- begin() uploads ~84KB
    of sensor firmware and takes seconds. The skew then shows up somewhere
    unrelated, either as a bogus timeout or, worse, as an "ok" reply carrying the
    PREVIOUS command's numbers.

    So: always verify the echoed cmd, and resync-and-retry rather than failing a
    run over a protocol artifact. A genuine hang still fails the run, because
    resync gives up too.
    """
    last = None
    for _ in range(tries):
        last = h.cmd(command)
        if last.get("ok") and last.get("cmd") == command.split()[0]:
            return last
        resync(h)
    raise RuntimeError(f"{command!r} unusable after {tries} tries: {last}")


def st(h):
    """Current game state dict."""
    return ask(h, "radroach_state")


# An all-far mocked frame: no zone is inside RADRO_HAND_MAX_MM, so the gesture
# detector sees nvalid < 2 and can never fire.
QUIET_FRAME = ",".join(["2000"] * 64)


def start(h, reset=True, quiet=True):
    """Open a run and wait until the session is answering in step.

    The first open after boot uploads ~84KB of VL53L5CX firmware and blocks the
    main loop for seconds, so its own reply can arrive after the host has given
    up on it. Drain until radroach_state answers as itself.
    """
    # quiet: pin the sensor to a mocked empty frame FIRST. Otherwise these runs
    # claim the real VL53L5CX, and anything moving in front of the badge -- a
    # hand, someone walking past -- is a valid whirlwind gesture. It fired
    # mid-test and cleared a screen the test had just populated, which showed up
    # as unrelated checks failing at random.
    if quiet:
        h.cmd(f"sensor_mock {QUIET_FRAME}")
    h.cmd("radroach_open")          # may answer late; ask() below absorbs that
    for _ in range(12):
        try:
            if st(h).get("open"):
                break
        except RuntimeError:
            pass
        h.wait(500)
    else:
        raise RuntimeError("game did not come up")
    if reset:
        h.cmd("radroach_freeze 1")
        h.cmd("radroach_reset")
    return st(h)


def toss(h, steps=RISE_STEPS):
    """Advance physics so spawned roaches rise into cuttable space."""
    h.cmd(f"radroach_step {steps}")


def cut_everything(h):
    for y in COMB_Y:
        h.cmd(f"radroach_cut 0 {y} 320 {y}")


def mock_hand(h, col):
    """Park a mocked hand in the left ('L') or right ('R') sensor columns."""
    row = [150, 150, 2000, 2000] if col == "L" else [2000, 2000, 150, 150]
    h.cmd("sensor_mock " + ",".join(str(v) for v in row * 4 + [2000] * 48))


def test_opens_and_exits(h):
    print("[1] the game opens clean and exits")
    s = start(h, quiet=False)       # real sensor here, to cover the claim path
    check("open", s.get("open") is True, s)
    check("real sensor claimed", s.get("claimed") is True, s)
    check("full lives", s.get("lives") == 3, s)
    check("zero score", s.get("score") == 0, s)
    check("empty meter", s.get("charge") == 0, s)
    # Every lv_button_class widget gets a tap sound from the global input-device
    # hook in ui_nav.h. On the play surface that click leaves a driver-owned clip
    # in the single audio slot, so the next kill eats a 30 ms stall retiring it.
    # radro_flat_btn() uses plain lv_obj to dodge the hook; this catches anyone
    # converting it back to lv_btn_create.
    check("play surface has no button-class widgets", s.get("btns") == 0, s)

    # Leave sprites alive across the exit: deleting radro_scr frees them all, and
    # the radro_roach[] pointers used to survive as dangles. A closed game that
    # still reports live roaches is reading freed memory.
    h.cmd("radroach_freeze 1")
    h.cmd("radroach_spawn 6 roach")
    check("6 sprites alive before exit", st(h).get("roaches") == 6)
    h.cmd("radroach_exit")
    s = st(h)
    check("exit closes the screen", s.get("open") is False, s)
    check("no dangling sprites after exit", s.get("roaches") == 0, s)


def test_cuts_score_and_charge(h):
    print("[2] finger cuts score, and 5 of them light the katana")
    start(h)
    h.cmd("radroach_spawn 4 roach")
    check("4 roaches airborne", st(h).get("roaches") == 4)
    toss(h)
    cut_everything(h)
    s = st(h)
    check("all 4 scored", s.get("score") == 4, s)
    check("screen cleared", s.get("roaches") == 0, s)
    check("meter tracks cuts", s.get("charge") == 4, s)
    check("not lit yet", s.get("charged") is False, s)
    check("no lives lost", s.get("lives") == 3, s)

    h.cmd("radroach_spawn 1 roach")
    toss(h)
    cut_everything(h)
    s = st(h)
    check("5th cut lights the katana", s.get("charged") is True, s)
    check("meter pinned at 5", s.get("charge") == 5, s)
    h.cmd("radroach_exit")


def test_whirlwind_spares_barrels(h):
    print("[3] the whirlwind clears the screen and eats barrels harmlessly")
    start(h)
    h.cmd("radroach_charge 5")
    check("katana lit", st(h).get("charged") is True)
    h.cmd("radroach_spawn 3 roach")
    h.cmd("radroach_spawn 2 barrel")
    check("5 sprites airborne", st(h).get("roaches") == 5)

    r = ask(h, "radroach_whirlwind")
    check("whirlwind fired", r.get("fired") is True, r)
    s = st(h)
    check("whole screen cleared", s.get("roaches") == 0, s)
    check("only roaches scored", s.get("score") == 3, s)
    check("barrels did NOT end the run", s.get("lives") == 3, s)
    check("still alive", s.get("over") is False, s)
    check("charge spent", s.get("charged") is False, s)
    check("meter reset", s.get("charge") == 0, s)
    h.cmd("radroach_exit")


def test_whirlwind_is_not_wasted(h):
    print("[4] the whirlwind refuses to fire unlit, or on an empty screen")
    start(h)
    r = ask(h, "radroach_whirlwind")
    check("unlit katana does nothing", r.get("fired") is False, r)

    h.cmd("radroach_charge 5")
    r = ask(h, "radroach_whirlwind")          # lit, but nothing on screen
    check("empty screen does not fire", r.get("fired") is False, r)
    check("charge survives", st(h).get("charged") is True)
    h.cmd("radroach_exit")


def test_cutting_a_barrel_ends_the_run(h):
    print("[5] cutting a rad-barrel with the finger still ends the run")
    start(h)
    h.cmd("radroach_spawn 1 barrel")
    toss(h)
    cut_everything(h)
    s = st(h)
    check("lives zeroed by the barrel", s.get("lives") <= 0, s)
    # The HUD used to keep showing 3 while lives was already 0, so the run ended
    # with no visible reason. The counter must agree with reality.
    check("death cause recorded as barrel", s.get("by_barrel") is True, s)
    h.cmd("radroach_step 1")        # one frame for the tick to raise game-over
    s = st(h)
    check("game over", s.get("over") is True, s)

    # The old build put a CLICKED handler on the WHOLE screen the moment the last
    # life went, so lifting the finger that was still mid-swipe exited instantly
    # and you never saw the score. Losing must leave you on a card, not back in
    # the SAO list.
    check("game-over card is shown", s.get("go_card") is True, s)
    check("game-over card is click-free too", s.get("btns") == 0, s)
    check("losing does NOT close the screen", s.get("open") is True, s)
    claimed_before = s.get("claimed")

    r = ask(h, "radroach_restart")   # what the PLAY AGAIN button does
    check("play again keeps the screen", r.get("open") is True, r)
    check("play again clears the card", r.get("go_card") is False, r)
    check("play again resets lives", r.get("lives") == 3, r)
    check("play again resets score", r.get("score") == 0, r)
    check("play again clears the field", r.get("roaches") == 0, r)
    check("play again is not over", r.get("over") is False, r)
    # Restarting must NOT re-claim the sensor or re-decode the SFX; it reuses the
    # screen it is already on.
    check("play again keeps the sensor as-is", r.get("claimed") == claimed_before, r)
    h.cmd("radroach_exit")


def test_missed_roach_costs_a_life(h):
    print("[6] a roach that falls off the bottom costs a life")
    start(h)
    h.cmd("radroach_spawn 1 roach")
    # A toss peaks around frame 21-31 and is back below the screen by ~63.
    h.cmd("radroach_step 90")
    s = st(h)
    check("roach is gone", s.get("roaches") == 0, s)
    check("one life lost", s.get("lives") == 2, s)
    check("nothing scored", s.get("score") == 0, s)
    check("a miss is NOT blamed on a barrel", s.get("by_barrel") is False, s)

    # A rad-barrel falling off is NOT a miss -- you were right to let it go.
    h.cmd("radroach_spawn 1 barrel")
    h.cmd("radroach_step 90")
    s = st(h)
    check("barrel fell away", s.get("roaches") == 0, s)
    check("dropped barrel is free", s.get("lives") == 2, s)
    h.cmd("radroach_exit")


def test_hand_wave_triggers_whirlwind(h):
    print("[7] a mocked hand wave fires the whirlwind (no ToF board needed)")
    mock_hand(h, "L")               # mock BEFORE opening, so the claim is skipped
    start(h, quiet=False)           # keep OUR frame; quiet=True would erase the hand
    s = st(h)
    check("harness frames in use", s.get("mocked") is True, s)
    check("real sensor NOT claimed", s.get("claimed") is False, s)
    check("whirlwind available", s.get("sensor_ok") is True, s)

    h.cmd("radroach_charge 5")
    h.cmd("radroach_spawn 4 roach")
    h.wait(300)                     # let the 60 Hz poll settle on the left hand
    check("still lit before the wave", st(h).get("charged") is True)

    mock_hand(h, "R")               # the wave: centroid jumps ~2 columns
    h.wait(400)
    s = st(h)
    check("wave fired the whirlwind", s.get("charged") is False, s)
    check("screen cleared by the wave", s.get("roaches") == 0, s)
    check("wave scored the roaches", s.get("score") == 4, s)
    h.cmd("radroach_exit")
    h.cmd("sensor_real")


def test_spawning_survives_the_ramp(h):
    print("[8] roaches keep spawning at every score")
    # spawn_every = 1200 - score*20, floored at 350. As uint32_t that wrapped at
    # score 61 (1200-1220) to ~4.29e9, and the game stopped spawning for the rest
    # of the run. Walk the tier boundary and well past it.
    # This case forces absurd scores and lets roaches fall, so it WILL hit game
    # over and write a bogus high score to NVS -- it ate a real 46 the first time
    # it ran. Bracket the whole thing and put the player's best back.
    real_best = ask(h, "radroach_hiscore").get("hiscore")
    start(h)
    for score in (0, 43, 60, 61, 100, 500):
        ask(h, f"radroach_score {score}")
        ask(h, "radroach_reset")            # clears the field, keeps us frozen
        ask(h, f"radroach_score {score}")   # reset zeroes the score, re-apply
        r = ask(h, "radroach_step 120 spawn")
        check(f"still spawning at score {score}", r.get("roaches", 0) > 0, r)
    h.cmd("radroach_exit")
    ask(h, f"radroach_hiscore {real_best}")
    check("player's high score survived the ramp test",
          ask(h, "radroach_hiscore").get("hiscore") == real_best)


def test_hiscore_persists(h):
    print("[9] high score round-trips through NVS")
    before = ask(h, "radroach_hiscore").get("hiscore")
    h.cmd("radroach_hiscore 1234")
    check("write took", ask(h, "radroach_hiscore").get("hiscore") == 1234)
    h.cmd("radroach_hiscore clear")
    check("clear took", ask(h, "radroach_hiscore").get("hiscore") == 0)
    if before:
        h.cmd(f"radroach_hiscore {before}")   # put the player's best back
        print(f"  ..    restored previous best {before}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None)
    args = ap.parse_args()

    h = Harness(port=args.port)
    try:
        for t in (test_opens_and_exits,
                  test_cuts_score_and_charge,
                  test_whirlwind_spares_barrels,
                  test_whirlwind_is_not_wasted,
                  test_cutting_a_barrel_ends_the_run,
                  test_missed_roach_costs_a_life,
                  test_hand_wave_triggers_whirlwind,
                  test_spawning_survives_the_ramp,
                  test_hiscore_persists):
            t(h)
    finally:
        h.cmd("radroach_exit")
        h.cmd("sensor_real")
        h.close()

    print()
    if FAILURES:
        print(f"RESULT: {len(FAILURES)} FAILED -> {', '.join(FAILURES)}")
        sys.exit(1)
    print("RESULT: all checks passed")


if __name__ == "__main__":
    main()
