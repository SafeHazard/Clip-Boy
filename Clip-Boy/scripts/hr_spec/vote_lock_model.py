"""Window-plurality-with-margin HR lock model (Phase A reference).

The executable spec for HRScanEngine's anchor-mode commit. The C++ port MUST
match the integer decisions here. See docs/superpowers/specs/2026-07-06-hr-scan-
first-scan-reliability-design.md sec 4.
"""
WEIGHT_CLEAN = 2   # a clean (SECDED syndrome==0) frame
WEIGHT_CORR = 1    # a corrected (syn!=0 but Ok) frame


def decide(votes, floor=12, margin_x10=20):
    """Stateless commit test over a tallied weighted-vote dict.

    Lock the plurality winner iff: top >= floor AND top*10 >= margin_x10*second.
    Clean-ness is NOT gated here -- it lives in the vote WEIGHT (clean=+2,
    corrected=+1), so a corrected-only-but-correct tag (the common case at ~2
    sensor-zones/cell, which NEVER reads clean) still locks. The margin + pose
    gates + whitelist are the safety. Returns the id or None.
    """
    if not votes:
        return None
    ordered = sorted(votes.items(), key=lambda kv: (-kv[1], kv[0]))
    top_id, top_v = ordered[0]
    second_v = ordered[1][1] if len(ordered) > 1 else 0
    if top_v < floor:
        return None
    if top_v * 10 < margin_x10 * second_v:
        return None
    return top_id


class VoteLock:
    def __init__(self, window=24, floor=12, margin_x10=20):
        self.window = window
        self.floor = floor
        self.margin_x10 = margin_x10
        self.votes = {}          # id -> weighted votes in-window
        self.ring = [None] * window  # (id, weight) or None
        self.pos = 0
        self.locked = None

    def feed(self, id, clean, pose_ok):
        if self.locked is not None:
            return self.locked
        if not pose_ok:
            return None                      # pose-failed frames don't vote (and don't age)
        w = WEIGHT_CLEAN if clean else WEIGHT_CORR
        old = self.ring[self.pos]            # age out the slot we overwrite
        if old is not None:
            oid, ow = old
            self.votes[oid] = max(0, self.votes.get(oid, 0) - ow)
        self.ring[self.pos] = (id, w)
        self.pos = (self.pos + 1) % self.window
        self.votes[id] = self.votes.get(id, 0) + w
        d = decide(self.votes, self.floor, self.margin_x10)
        if d is not None:
            self.locked = d
        return self.locked

    def top2(self):
        ordered = sorted(self.votes.items(), key=lambda kv: (-kv[1], kv[0]))
        top = ordered[0] if ordered else (-1, 0)
        second = ordered[1] if len(ordered) > 1 else (-1, 0)
        return top, second
