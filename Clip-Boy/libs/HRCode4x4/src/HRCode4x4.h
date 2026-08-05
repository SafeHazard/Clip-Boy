#pragma once
#include <Arduino.h>

namespace HRCode4x4 {

enum class DecodeStatus : uint8_t {
  Ok = 0,                 // Orientation matched AND CRC matched
  OrientationMatchedBadCrc, // Orientation matched, CRC mismatch
  OrientationNotFound,    // No rotation matches the orientation pattern
  InvalidInput            // Bits not 0/1 or null pointers, etc.
};

struct DecodeResult {
  DecodeStatus status;
  uint8_t id;          // 0..255 (meaningful if orientation matched)
  uint8_t crc_read;    // 0..15
  uint8_t crc_calc;    // 0..15
  uint8_t rotation;    // 0..3 (number of CW 90° rotations applied to input to reach canonical)
};

/// Decode from 16 bits laid out row-major (r0c0..r0c3,r1c0..r3c3).
DecodeResult decode16(const uint8_t bits16[16]);

/// Convenience: decode from 4x4 grid [r][c].
DecodeResult decodeGrid(const uint8_t grid4x4[4][4]);

/// Helper: compute CRC4 = (id ^ (id >> 4)) & 0xF
uint8_t crc4(uint8_t id);

} // namespace HRCode4x4
