#!/usr/bin/env python3
"""
gen_hr_fixtures.py — Generate mock VL53L5CX data for HR code testing.

Converts an HR code ID into realistic 8x8 distance arrays that the
HRCode4x4 + HRScanEngine pipeline will decode (or fail to decode).

Usage:
    py -3 scripts/tests/gen_hr_fixtures.py --all       # Generate all fixtures
    py -3 scripts/tests/gen_hr_fixtures.py --id 42      # Generate one valid fixture
"""

import json
import os
import sys
import math
import random
import argparse

FIXTURE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures")

# ─── HR Code Encoding ─────────────────────────────────────────────────────

def crc4(id_val):
    """CRC4 nibble-fold: (id ^ (id >> 4)) & 0x0F"""
    return (id_val ^ (id_val >> 4)) & 0x0F

def id_to_grid(id_val):
    """Convert an HR code ID (0-255) to a 4x4 bit grid.

    Layout:
      [0,0]=1  [0,1]=1  [0,2]=b0  [0,3]=b1    (orientation + bits 0-1)
      [1,0]=1  [1,1]=0  [1,2]=b2  [1,3]=b3    (orientation + bits 2-3)
      [2,0]=b4 [2,1]=b5 [2,2]=b6  [2,3]=b7    (bits 4-7)
      [3,0]=c0 [3,1]=c1 [3,2]=c2  [3,3]=c3    (CRC4)
    """
    grid = [[0]*4 for _ in range(4)]

    # Orientation corner
    grid[0][0] = 1; grid[0][1] = 1
    grid[1][0] = 1; grid[1][1] = 0

    # ID bits (LSB first)
    bits = [(id_val >> i) & 1 for i in range(8)]
    grid[0][2] = bits[0]; grid[0][3] = bits[1]
    grid[1][2] = bits[2]; grid[1][3] = bits[3]
    grid[2][0] = bits[4]; grid[2][1] = bits[5]
    grid[2][2] = bits[6]; grid[2][3] = bits[7]

    # CRC4
    crc = crc4(id_val)
    crc_bits = [(crc >> i) & 1 for i in range(4)]
    grid[3][0] = crc_bits[0]; grid[3][1] = crc_bits[1]
    grid[3][2] = crc_bits[2]; grid[3][3] = crc_bits[3]

    return grid

def rotate_grid_cw(grid):
    """Rotate a 4x4 grid 90 degrees clockwise."""
    n = len(grid)
    return [[grid[n-1-c][r] for c in range(n)] for r in range(n)]

def mirror_grid_h(grid):
    """Horizontally mirror a 4x4 grid."""
    return [[grid[r][3-c] for c in range(4)] for r in range(4)]

# ─── VL53L5CX Mock Data Generation ────────────────────────────────────────

def grid_to_distance_8x8(grid_4x4, base_depth=250, separation=80,
                          roi_x=0, roi_y=0, roi_size=8,
                          tilt_x=0.0, tilt_y=0.0, noise=0.0):
    """Convert a 4x4 bit grid to 8x8 VL53L5CX distance_mm values.

    The pattern fills the entire 8x8 grid by default (roi_size=8 at 0,0).
    Each 4x4 cell maps to a 2x2 block of sensor pixels, giving clean
    bilinear sampling boundaries.

    Args:
        grid_4x4: 4x4 list of 0/1 bits
        base_depth: center depth in mm
        separation: depth difference between near(1) and far(0) in mm
        roi_x, roi_y: top-left corner of the ROI in the 8x8 grid
        roi_size: size of the ROI (4-8 pixels)
        tilt_x: depth gradient per column (mm/pixel), simulates X-axis tilt
        tilt_y: depth gradient per row (mm/pixel), simulates Y-axis tilt
        noise: random noise amplitude (mm)

    Returns:
        list of 64 int16 values (row-major, with column flip applied)
    """
    raw_8x8 = [[base_depth] * 8 for _ in range(8)]

    # Map 4x4 grid onto the ROI region
    cell_size = roi_size / 4.0
    for r in range(8):
        for c in range(8):
            # Is this pixel inside the ROI?
            pr = r - roi_y
            pc = c - roi_x
            if 0 <= pr < roi_size and 0 <= pc < roi_size:
                # Which 4x4 cell does this map to?
                cell_r = min(int(pr / cell_size), 3)
                cell_c = min(int(pc / cell_size), 3)
                bit = grid_4x4[cell_r][cell_c]

                # With oneIsNear=true: near distance → bit 1
                # bit=1 → physically CLOSER to sensor (LOWER distance value)
                # bit=0 → physically FARTHER from sensor (HIGHER distance value)
                depth = base_depth + (-separation/2 if bit else separation/2)

                # Apply tilt gradients
                depth += tilt_x * (c - 3.5)
                depth += tilt_y * (r - 3.5)

                # Apply noise
                if noise > 0:
                    depth += random.uniform(-noise, noise)

                raw_8x8[r][c] = max(10, int(depth))

    # Apply column flip (reverse of engine's oriented[r][c] = raw[r*8 + (7-c)])
    distance_mm = [0] * 64
    for r in range(8):
        for c in range(8):
            raw_col = 7 - c
            idx = raw_col + r * 8
            distance_mm[idx] = raw_8x8[r][c]

    return distance_mm

def make_fixture(name, description, distance_mm, expect_lock, expect_id=None,
                 frames=10, category="valid", target_status_override=None,
                 nb_target_override=None):
    """Create a fixture dict."""
    # Default: all zones valid
    target_status = target_status_override or [5] * 64
    nb_target = nb_target_override or [1] * 64

    fixture = {
        "name": name,
        "description": description,
        "distance_mm": distance_mm,
        "target_status": target_status,
        "nb_target_detected": nb_target,
        "expect_lock": expect_lock,
        "frames_to_send": frames,
        "category": category,
    }
    if expect_id is not None:
        fixture["expect_id"] = expect_id
    return fixture

def save_fixture(fixture):
    os.makedirs(FIXTURE_DIR, exist_ok=True)
    path = os.path.join(FIXTURE_DIR, f"{fixture['name']}.json")
    with open(path, "w") as f:
        json.dump(fixture, f, indent=2)
    print(f"  {fixture['name']}.json ({fixture['category']})")

# ─── Valid Code Fixtures ───────────────────────────────────────────────────

def gen_valid_canonical(id_val, suffix=""):
    grid = id_to_grid(id_val)
    # Full 8x8 grid, 80mm separation. Each 4x4 cell maps to 2x2 sensor pixels.
    dist = grid_to_distance_8x8(grid)
    name = f"valid_id{id_val}_canonical{suffix}"
    return make_fixture(name, f"Valid HR code ID {id_val}, canonical orientation, full grid",
                        dist, True, id_val, frames=20, category="valid")

def gen_valid_rotated(id_val, rotation):
    grid = id_to_grid(id_val)
    for _ in range(rotation // 90):
        grid = rotate_grid_cw(grid)
    dist = grid_to_distance_8x8(grid)
    name = f"valid_id{id_val}_rot{rotation}"
    return make_fixture(name, f"Valid HR code ID {id_val}, rotated {rotation} CW",
                        dist, True, id_val, frames=20, category="valid")

def gen_valid_tilted(id_val, tilt_x, tilt_y, axis_name):
    grid = id_to_grid(id_val)
    dist = grid_to_distance_8x8(grid, tilt_x=tilt_x, tilt_y=tilt_y)
    name = f"valid_id{id_val}_tilted_{axis_name}"
    return make_fixture(name, f"Valid HR code ID {id_val}, tilted on {axis_name} axis",
                        dist, True, id_val, frames=20, category="valid")

def gen_valid_depth(id_val, depth, depth_name):
    grid = id_to_grid(id_val)
    dist = grid_to_distance_8x8(grid, base_depth=depth)
    name = f"valid_id{id_val}_{depth_name}"
    return make_fixture(name, f"Valid HR code ID {id_val}, at {depth}mm ({depth_name})",
                        dist, True, id_val, frames=20, category="valid")

def gen_valid_offset(id_val, roi_x, roi_y):
    grid = id_to_grid(id_val)
    dist = grid_to_distance_8x8(grid, roi_x=roi_x, roi_y=roi_y, roi_size=6)
    name = f"valid_id{id_val}_offset_{roi_x}_{roi_y}"
    return make_fixture(name, f"Valid HR code ID {id_val}, ROI offset to ({roi_x},{roi_y}) size 6",
                        dist, True, id_val, frames=20, category="valid")

def gen_valid_mirrored(id_val):
    grid = id_to_grid(id_val)
    grid = mirror_grid_h(grid)
    dist = grid_to_distance_8x8(grid)
    name = f"valid_id{id_val}_mirrored"
    return make_fixture(name, f"Valid HR code ID {id_val}, horizontally mirrored",
                        dist, True, id_val, frames=20, category="valid")

# ─── Invalid Code Fixtures ────────────────────────────────────────────────

def gen_invalid_bad_crc(id_val=42):
    grid = id_to_grid(id_val)
    # Flip one CRC bit
    grid[3][0] = 1 - grid[3][0]
    dist = grid_to_distance_8x8(grid, base_depth=250, separation=50)
    return make_fixture("invalid_bad_crc", "Correct orientation but CRC mismatch",
                        dist, False, category="invalid")

def gen_invalid_no_orientation():
    # Random grid with no valid orientation corner
    grid = [[0]*4 for _ in range(4)]
    grid[0][0] = 0; grid[0][1] = 0; grid[1][0] = 0; grid[1][1] = 0
    for r in range(4):
        for c in range(4):
            if r >= 2 or c >= 2:
                grid[r][c] = random.randint(0, 1)
    dist = grid_to_distance_8x8(grid, base_depth=250, separation=50)
    return make_fixture("invalid_no_orientation", "Random pattern, no valid orientation corner",
                        dist, False, category="invalid")

def gen_invalid_too_far():
    dist = [2500] * 64  # All zones very far
    status = [0] * 64   # No valid targets
    nb = [0] * 64
    return make_fixture("invalid_too_far", "All zones > 2000mm, no valid targets",
                        dist, False, category="invalid",
                        target_status_override=status, nb_target_override=nb)

def gen_invalid_too_close():
    dist = [15] * 64    # All zones saturated close
    status = [2] * 64   # Status 2 = sigma failure (not 5 or 9)
    nb = [1] * 64
    return make_fixture("invalid_too_close", "All zones < 20mm, saturated sensor",
                        dist, False, category="invalid",
                        target_status_override=status, nb_target_override=nb)

def gen_invalid_low_separation():
    grid = id_to_grid(42)
    dist = grid_to_distance_8x8(grid, base_depth=250, separation=3)  # Only 3mm
    return make_fixture("invalid_low_separation",
                        "Near/far difference only 3mm (below 4.5mm threshold)",
                        dist, False, category="invalid")

def gen_invalid_partial_coverage():
    grid = id_to_grid(42)
    dist = grid_to_distance_8x8(grid, base_depth=250, separation=50)
    # Set most zones to invalid
    status = [0] * 64
    nb = [0] * 64
    # Only 8 valid zones (need 12 minimum)
    for i in range(8):
        status[i] = 5
        nb[i] = 1
    return make_fixture("invalid_partial_coverage",
                        "Only 8 of 64 zones valid (below 12 minimum)",
                        dist, False, category="invalid",
                        target_status_override=status, nb_target_override=nb)

def gen_invalid_flat_surface():
    dist = [250] * 64  # Uniform depth
    return make_fixture("invalid_all_same_depth", "Flat surface, no pattern (250mm everywhere)",
                        dist, False, category="invalid")

def gen_invalid_noisy():
    dist = [250 + random.randint(-80, 80) for _ in range(64)]
    return make_fixture("invalid_noisy", "Random noise +/-80mm on every zone",
                        dist, False, category="invalid")

def gen_invalid_blacklisted(id_val):
    grid = id_to_grid(id_val)
    dist = grid_to_distance_8x8(grid, base_depth=250, separation=50)
    return make_fixture(f"invalid_blacklisted_id{id_val}",
                        f"Valid pattern for blacklisted ID {id_val} — ambiguous across rotations",
                        dist, False, category="invalid")

# ─── Timeout Fixtures ─────────────────────────────────────────────────────

def gen_timeout_empty():
    dist = [0] * 64
    status = [0] * 64
    nb = [0] * 64
    return make_fixture("timeout_empty", "No target at all (all target_status=0)",
                        dist, False, frames=200, category="timeout",
                        target_status_override=status, nb_target_override=nb)

def gen_timeout_flickering():
    """Generate two alternating frame sets — valid code then noise.
    The test script should alternate between these."""
    grid = id_to_grid(42)
    valid_dist = grid_to_distance_8x8(grid, base_depth=250, separation=50)
    noise_dist = [250 + random.randint(-60, 60) for _ in range(64)]
    return make_fixture("timeout_flickering",
                        "Alternating valid (ID=42) and noise frames — never gets 8 consecutive. "
                        "Test script should use distance_mm for odd frames and alt_distance_mm for even.",
                        valid_dist, False, frames=200, category="timeout"), noise_dist

def gen_timeout_wrong_id_sequence():
    """Two valid codes that alternate — neither locks."""
    grid1 = id_to_grid(1)
    grid2 = id_to_grid(2)
    dist1 = grid_to_distance_8x8(grid1, base_depth=250, separation=50)
    dist2 = grid_to_distance_8x8(grid2, base_depth=250, separation=50)
    fixture = make_fixture("timeout_wrong_id_sequence",
                           "Alternating valid ID=1 (5 frames) then ID=2 (5 frames) — never locks. "
                           "Test sends distance_mm for 5 frames, alt_distance_mm for 5, repeating.",
                           dist1, False, frames=200, category="timeout")
    fixture["alt_distance_mm"] = dist2
    return fixture, None

# ─── 3D Rotation Edge Cases ───────────────────────────────────────────────

def gen_rot3d_pitch(id_val, degrees, name):
    """Simulate pitch (tilt toward/away from sensor) as Y-axis depth gradient."""
    gradient = math.tan(math.radians(degrees)) * 30
    grid = id_to_grid(id_val)
    dist = grid_to_distance_8x8(grid, tilt_y=gradient)
    return make_fixture(name, f"Code tilted {degrees} deg pitch (Y gradient {gradient:.1f}mm/px)",
                        dist, degrees < 60, id_val if degrees < 60 else None,
                        frames=12, category="rotation")

def gen_rot3d_roll(id_val, degrees, name):
    """Simulate roll as X-axis depth gradient."""
    gradient = math.tan(math.radians(degrees)) * 30
    grid = id_to_grid(id_val)
    dist = grid_to_distance_8x8(grid, tilt_x=gradient)
    return make_fixture(name, f"Code rolled {degrees} deg (X gradient {gradient:.1f}mm/px)",
                        dist, degrees < 60, id_val if degrees < 60 else None,
                        frames=12, category="rotation")

def gen_rot3d_compound(id_val):
    """Realistic hand-held: 20 pitch + 15 roll + 90 yaw."""
    grid = id_to_grid(id_val)
    grid = rotate_grid_cw(grid)  # 90° yaw
    pitch_grad = math.tan(math.radians(20)) * 30
    roll_grad = math.tan(math.radians(15)) * 30
    dist = grid_to_distance_8x8(grid, tilt_x=roll_grad, tilt_y=pitch_grad)
    return make_fixture("rot3d_compound",
                        "Compound rotation: 20 pitch + 15 roll + 90 yaw — realistic hand-held",
                        dist, True, id_val, frames=12, category="rotation")

# ─── Main ──────────────────────────────────────────────────────────────────

def generate_all():
    print("Generating HR code test fixtures...")
    os.makedirs(FIXTURE_DIR, exist_ok=True)

    random.seed(42)  # Reproducible

    # All 95 valid collectible IDs
    ALL_IDS = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 13, 14, 15, 16, 17, 18, 19,
               20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35,
               36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51,
               52, 53, 54, 55, 56, 57, 58, 60, 61, 62, 63, 64, 65, 66, 67, 68,
               69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 83, 84, 85,
               86, 87, 88, 89, 90, 92, 93, 95, 96, 97, 98, 99, 100]

    # Valid codes — one for each collectible ID
    print(f"\n--- Valid codes ({len(ALL_IDS)} IDs) ---")
    for id_val in ALL_IDS:
        save_fixture(gen_valid_canonical(id_val))

    # Rotation variants (sample)
    print("\n--- Rotation variants ---")
    for rot in [90, 180, 270]:
        save_fixture(gen_valid_rotated(42, rot))

    # Tilt variants
    print("\n--- Tilt variants ---")
    save_fixture(gen_valid_tilted(42, tilt_x=8.0, tilt_y=0, axis_name="x"))
    save_fixture(gen_valid_tilted(42, tilt_x=0, tilt_y=8.0, axis_name="y"))
    save_fixture(gen_valid_tilted(42, tilt_x=6.0, tilt_y=6.0, axis_name="xy"))

    # Depth variants
    print("\n--- Depth variants ---")
    save_fixture(gen_valid_depth(42, 400, "far"))
    save_fixture(gen_valid_depth(42, 80, "near"))
    save_fixture(gen_valid_offset(42, 1, 1))
    save_fixture(gen_valid_mirrored(42))

    # Invalid codes
    print("\n--- Invalid codes ---")
    save_fixture(gen_invalid_bad_crc())
    save_fixture(gen_invalid_no_orientation())
    save_fixture(gen_invalid_too_far())
    save_fixture(gen_invalid_too_close())
    save_fixture(gen_invalid_low_separation())
    save_fixture(gen_invalid_partial_coverage())
    save_fixture(gen_invalid_flat_surface())
    save_fixture(gen_invalid_noisy())
    for bl_id in [11, 82]:
        save_fixture(gen_invalid_blacklisted(bl_id))

    # Timeout scenarios
    print("\n--- Timeout scenarios ---")
    save_fixture(gen_timeout_empty())
    flicker_fixture, flicker_alt = gen_timeout_flickering()
    if flicker_alt:
        flicker_fixture["alt_distance_mm"] = flicker_alt
    save_fixture(flicker_fixture)
    seq_fixture, _ = gen_timeout_wrong_id_sequence()
    save_fixture(seq_fixture)

    # 3D rotation edge cases
    print("\n--- 3D rotation edge cases ---")
    save_fixture(gen_rot3d_pitch(42, 45, "rot3d_pitch_45"))
    save_fixture(gen_rot3d_roll(42, 30, "rot3d_roll_30"))
    save_fixture(gen_rot3d_compound(42))
    save_fixture(gen_rot3d_pitch(42, 80, "rot3d_extreme_pitch"))

    print(f"\nDone. Fixtures saved to {FIXTURE_DIR}/")

def generate_single(id_val):
    print(f"Generating fixture for ID {id_val}...")
    save_fixture(gen_valid_canonical(id_val))

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Generate HR code test fixtures")
    parser.add_argument("--all", action="store_true", help="Generate all fixtures")
    parser.add_argument("--id", type=int, help="Generate single fixture for this ID")
    args = parser.parse_args()

    if args.all:
        generate_all()
    elif args.id is not None:
        generate_single(args.id)
    else:
        parser.print_help()
