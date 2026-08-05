#include "HRCode4x4.h"

namespace HRCode4x4 {

static constexpr uint8_t N = 4;

// Orientation pattern in canonical orientation (top-left 2x2):
// 1 1
// 1 0
static constexpr uint8_t ORIENT_POS[4][2] = {
  {0,0}, {0,1}, {1,0}, {1,1}
};
static constexpr uint8_t ORIENT_VAL[4] = {1, 1, 1, 0};

// ID bits (LSB->MSB) cells:
static constexpr uint8_t ID_POS[8][2] = {
  {0,2}, {0,3},
  {1,2}, {1,3},
  {2,0}, {2,1}, {2,2}, {2,3}
};

// CRC4 bits (LSB->MSB) cells:
static constexpr uint8_t CRC_POS[4][2] = {
  {3,0}, {3,1}, {3,2}, {3,3}
};

uint8_t crc4(uint8_t id) {
  return (uint8_t)((id ^ (id >> 4)) & 0x0F);
}

static inline bool isBit01(uint8_t b) {
  return (b == 0 || b == 1);
}

// Rotate clockwise: out[r][c] = in[N-1-c][r]
static void rotate90cw(const uint8_t inG[4][4], uint8_t outG[4][4]) {
  for (uint8_t r = 0; r < N; r++) {
    for (uint8_t c = 0; c < N; c++) {
      outG[r][c] = inG[N - 1 - c][r];
    }
  }
}

static bool orientMatches(const uint8_t g[4][4]) {
  for (uint8_t i = 0; i < 4; i++) {
    uint8_t r = ORIENT_POS[i][0];
    uint8_t c = ORIENT_POS[i][1];
    if (g[r][c] != ORIENT_VAL[i]) return false;
  }
  return true;
}

static void decodeFields(const uint8_t g[4][4], uint8_t &idOut, uint8_t &crcOut) {
  uint8_t id = 0;
  for (uint8_t i = 0; i < 8; i++) {
    uint8_t r = ID_POS[i][0];
    uint8_t c = ID_POS[i][1];
    id |= (uint8_t)((g[r][c] & 1) << i);
  }

  uint8_t crc = 0;
  for (uint8_t i = 0; i < 4; i++) {
    uint8_t r = CRC_POS[i][0];
    uint8_t c = CRC_POS[i][1];
    crc |= (uint8_t)((g[r][c] & 1) << i);
  }

  idOut = id;
  crcOut = crc;
}

DecodeResult decodeGrid(const uint8_t grid4x4[4][4]) {
  DecodeResult res{};
  res.status = DecodeStatus::InvalidInput;
  res.id = 0;
  res.crc_read = 0;
  res.crc_calc = 0;
  res.rotation = 0;

  if (!grid4x4) return res;

  // Validate input bits are 0/1
  for (uint8_t r = 0; r < N; r++) {
    for (uint8_t c = 0; c < N; c++) {
      if (!isBit01(grid4x4[r][c])) return res;
    }
  }

  // Try 0..3 rotations
  uint8_t g0[4][4];
  uint8_t g1[4][4];

  // Copy into g0
  for (uint8_t r = 0; r < N; r++) {
    for (uint8_t c = 0; c < N; c++) {
      g0[r][c] = grid4x4[r][c];
    }
  }

  const uint8_t *cur = &g0[0][0];
  (void)cur; // silence unused warning in some cores

  uint8_t curG[4][4];
  for (uint8_t r = 0; r < N; r++) for (uint8_t c = 0; c < N; c++) curG[r][c] = g0[r][c];

  for (uint8_t rot = 0; rot < 4; rot++) {
    if (orientMatches(curG)) {
      uint8_t id, crcRead;
      decodeFields(curG, id, crcRead);
      uint8_t crcCalc = crc4(id);

      res.id = id;
      res.crc_read = crcRead;
      res.crc_calc = crcCalc;
      res.rotation = rot;

      if (crcRead == crcCalc) {
        res.status = DecodeStatus::Ok;
      } else {
        res.status = DecodeStatus::OrientationMatchedBadCrc;
      }
      return res;
    }

    // rotate for next attempt
    rotate90cw(curG, g1);
    for (uint8_t r = 0; r < N; r++) for (uint8_t c = 0; c < N; c++) curG[r][c] = g1[r][c];
  }

  res.status = DecodeStatus::OrientationNotFound;
  return res;
}

DecodeResult decode16(const uint8_t bits16[16]) {
  DecodeResult res{};
  res.status = DecodeStatus::InvalidInput;
  res.id = 0;
  res.crc_read = 0;
  res.crc_calc = 0;
  res.rotation = 0;

  if (!bits16) return res;

  uint8_t g[4][4];
  for (uint8_t i = 0; i < 16; i++) {
    if (!isBit01(bits16[i])) return res;
    g[i / 4][i % 4] = bits16[i];
  }

  return decodeGrid(g);
}

} // namespace HRCode4x4
