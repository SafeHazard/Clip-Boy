#include "HRScanEngine.h"

#include <math.h>

namespace HRScan {

void Engine::begin(Profile p) {
  setProfile(p);
  clearLockInternal();
  histWrite_ = 0;
  histCount_ = 0;
}

void Engine::setProfile(Profile p) {
  profile_ = p;
  if (p == Profile::Sensitive10mm) {
    minSeparationMm_ = 4.5f;
    lockRequired_ = 10;   // DC34-155: 12->10. A marginal anchor read (one cell
                          // consistently off -> syn=1, so extra flicker = a
                          // double-error rejected frame) peaks ~11 and oscillates;
                          // 10 lets it lock within the scan window. decode_v3
                          // (correct-rotation pick) + consistent-id are the
                          // wrong-lock defenses, not a high count.
  } else {
    minSeparationMm_ = 8.0f;
    lockRequired_ = 8;
  }
  clearLockInternal();
}

Profile Engine::profile() const { return profile_; }

void Engine::setExpectedId(int idOrMinusOne) {
  expectedId_ = idOrMinusOne;
  useExpectedIdPrior_ = (idOrMinusOne >= 0);
}

void Engine::clearLock() { clearLockInternal(); }

int Engine::bitCount8(uint8_t v) {
  int c = 0;
  for (uint8_t i = 0; i < 8; i++) c += (v >> i) & 1;
  return c;
}

int16_t Engine::medianSmall(int16_t *vals, uint8_t n) {
  for (uint8_t i = 1; i < n; i++) {
    int16_t key = vals[i];
    int8_t j = (int8_t)i - 1;
    while (j >= 0 && vals[j] > key) {
      vals[j + 1] = vals[j];
      j--;
    }
    vals[j + 1] = key;
  }
  return vals[n / 2];
}

bool Engine::zoneValid(uint8_t nbTargets, uint8_t targetStatus) {
  if (nbTargets == 0) return false;
  return (targetStatus == 5 || targetStatus == 9);
}

void Engine::orientFrame(const int16_t distanceMm64[64],
                         const uint8_t nbTargetDetected64[64],
                         const uint8_t targetStatus64[64],
                         int16_t outMm[8][8], bool outValid[8][8]) {
  for (uint8_t r = 0; r < 8; r++) {
    for (uint8_t c = 0; c < 8; c++) {
      const uint8_t rawCol = 7 - c;
      const uint8_t idx = rawCol + (r * 8);
      outMm[r][c] = distanceMm64[idx];
      outValid[r][c] = zoneValid(nbTargetDetected64[idx], targetStatus64[idx]);
    }
  }
}

void Engine::pushHistory(const int16_t mm8[8][8], const bool valid8[8][8]) {
  for (uint8_t r = 0; r < 8; r++) {
    for (uint8_t c = 0; c < 8; c++) {
      histMm_[histWrite_][r][c] = mm8[r][c];
      histValid_[histWrite_][r][c] = valid8[r][c];
    }
  }
  histWrite_ = (uint8_t)((histWrite_ + 1) % kFilterWindow);
  if (histCount_ < kFilterWindow) histCount_++;
}

void Engine::median8x8(int16_t outMm[8][8], bool outValid[8][8]) const {
  for (uint8_t r = 0; r < 8; r++) {
    for (uint8_t c = 0; c < 8; c++) {
      int16_t samples[kFilterWindow];
      uint8_t n = 0;
      for (uint8_t k = 0; k < histCount_; k++) {
        if (histValid_[k][r][c] && histMm_[k][r][c] > 0) samples[n++] = histMm_[k][r][c];
      }
      if (n == 0) {
        outValid[r][c] = false;
        outMm[r][c] = 0;
      } else {
        outValid[r][c] = true;
        outMm[r][c] = medianSmall(samples, n);
      }
    }
  }
}

bool Engine::sampleBilinear8x8(const int16_t mm8[8][8], const bool valid8[8][8],
                               float y, float x, uint16_t &outMm) {
  if (x < 0.0f) x = 0.0f;
  if (x > 7.0f) x = 7.0f;
  if (y < 0.0f) y = 0.0f;
  if (y > 7.0f) y = 7.0f;

  const int x0 = (int)floorf(x);
  const int y0 = (int)floorf(y);
  const int x1 = (x0 < 7) ? x0 + 1 : x0;
  const int y1 = (y0 < 7) ? y0 + 1 : y0;
  const float fx = x - (float)x0;
  const float fy = y - (float)y0;

  struct Tap { int r; int c; float w; };
  Tap t[4] = {
    {y0, x0, (1.0f - fx) * (1.0f - fy)},
    {y0, x1, fx * (1.0f - fy)},
    {y1, x0, (1.0f - fx) * fy},
    {y1, x1, fx * fy},
  };

  float sumW = 0.0f;
  float sumV = 0.0f;
  for (uint8_t i = 0; i < 4; i++) {
    if (valid8[t[i].r][t[i].c] && mm8[t[i].r][t[i].c] > 0) {
      sumW += t[i].w;
      sumV += t[i].w * (float)mm8[t[i].r][t[i].c];
    }
  }
  if (sumW <= 0.0f) return false;
  outMm = (uint16_t)(sumV / sumW);
  return true;
}

uint8_t Engine::sampleRoi4x4(const int16_t mm8[8][8], const bool valid8[8][8],
                             uint8_t roiX, uint8_t roiY, uint8_t roiSize,
                             uint16_t depth16[16], bool valid16[16]) {
  uint8_t validCount = 0;
  for (uint8_t r = 0; r < 4; r++) {
    for (uint8_t c = 0; c < 4; c++) {
      const float srcY = (float)roiY + (((float)r + 0.5f) * (float)roiSize / 4.0f) - 0.5f;
      const float srcX = (float)roiX + (((float)c + 0.5f) * (float)roiSize / 4.0f) - 0.5f;
      const uint8_t i = (r * 4) + c;
      uint16_t mm = 0;
      if (sampleBilinear8x8(mm8, valid8, srcY, srcX, mm)) {
        depth16[i] = mm;
        valid16[i] = true;
        validCount++;
      } else {
        depth16[i] = 0xFFFF;
        valid16[i] = false;
      }
    }
  }
  return validCount;
}

bool Engine::fitPlane4x4(const uint16_t depth16[16], const bool valid16[16],
                         float &a, float &b, float &c0) {
  float sx2 = 0.0f, sy2 = 0.0f, sxy = 0.0f, sx = 0.0f, sy = 0.0f;
  float sxz = 0.0f, syz = 0.0f, sz = 0.0f, n = 0.0f;

  for (uint8_t r = 0; r < 4; r++) {
    for (uint8_t col = 0; col < 4; col++) {
      const uint8_t i = (r * 4) + col;
      if (!valid16[i] || depth16[i] == 0xFFFF) continue;
      const float x = (float)col;
      const float y = (float)r;
      const float z = (float)depth16[i];
      sx2 += x * x;
      sy2 += y * y;
      sxy += x * y;
      sx += x;
      sy += y;
      sxz += x * z;
      syz += y * z;
      sz += z;
      n += 1.0f;
    }
  }
  if (n < 8.0f) return false;

  const float m11 = sx2, m12 = sxy, m13 = sx;
  const float m21 = sxy, m22 = sy2, m23 = sy;
  const float m31 = sx, m32 = sy, m33 = n;
  const float b1 = sxz, b2 = syz, b3 = sz;

  const float det =
      m11 * (m22 * m33 - m23 * m32) -
      m12 * (m21 * m33 - m23 * m31) +
      m13 * (m21 * m32 - m22 * m31);
  if (fabsf(det) < 1e-6f) return false;

  const float detA =
      b1 * (m22 * m33 - m23 * m32) -
      m12 * (b2 * m33 - m23 * b3) +
      m13 * (b2 * m32 - m22 * b3);
  const float detB =
      m11 * (b2 * m33 - m23 * b3) -
      b1 * (m21 * m33 - m23 * m31) +
      m13 * (m21 * b3 - b2 * m31);
  const float detC =
      m11 * (m22 * b3 - b2 * m32) -
      m12 * (m21 * b3 - b2 * m31) +
      b1 * (m21 * m32 - m22 * m31);

  a = detA / det;
  b = detB / det;
  c0 = detC / det;
  return true;
}

void Engine::detrend4x4(const uint16_t depth16[16], const bool valid16[16],
                        int16_t residual16[16], bool residualValid16[16]) {
  float a = 0.0f, b = 0.0f, c0 = 0.0f;
  const bool ok = fitPlane4x4(depth16, valid16, a, b, c0);
  for (uint8_t r = 0; r < 4; r++) {
    for (uint8_t col = 0; col < 4; col++) {
      const uint8_t i = (r * 4) + col;
      if (!valid16[i] || depth16[i] == 0xFFFF) {
        residualValid16[i] = false;
        residual16[i] = 0;
        continue;
      }
      residualValid16[i] = true;
      if (!ok) {
        residual16[i] = (int16_t)depth16[i];
      } else {
        const float plane = a * (float)col + b * (float)r + c0;
        residual16[i] = (int16_t)lrintf((float)depth16[i] - plane);
      }
    }
  }
}

void Engine::splitAdaptiveSigned(const int16_t vals16[16], const bool valid16[16],
                                 int &threshold, float &sepMm) {
  threshold = 0;
  sepMm = 0.0f;

  int16_t minV = 32767;
  int16_t maxV = -32768;
  uint8_t n = 0;
  for (uint8_t i = 0; i < 16; i++) {
    if (!valid16[i]) continue;
    if (vals16[i] < minV) minV = vals16[i];
    if (vals16[i] > maxV) maxV = vals16[i];
    n++;
  }
  if (n < 8 || maxV <= minV + 2) return;

  float c1 = (float)minV;
  float c2 = (float)maxV;
  for (uint8_t iter = 0; iter < 12; iter++) {
    float s1 = 0.0f, s2 = 0.0f;
    uint8_t n1 = 0, n2 = 0;
    for (uint8_t i = 0; i < 16; i++) {
      if (!valid16[i]) continue;
      const float x = (float)vals16[i];
      if (fabsf(x - c1) <= fabsf(x - c2)) {
        s1 += x;
        n1++;
      } else {
        s2 += x;
        n2++;
      }
    }
    if (n1 == 0 || n2 == 0) return;
    c1 = s1 / (float)n1;
    c2 = s2 / (float)n2;
  }
  if (c1 > c2) {
    const float t = c1;
    c1 = c2;
    c2 = t;
  }
  threshold = (int)lrintf((c1 + c2) * 0.5f);
  sepMm = fabsf(c2 - c1);
}

void Engine::splitAdaptiveUnsigned(const uint16_t vals16[16], const bool valid16[16],
                                   int &threshold, float &sepMm) {
  threshold = -1;
  sepMm = 0.0f;

  uint16_t samples[16];
  uint8_t n = 0;
  for (uint8_t i = 0; i < 16; i++) {
    if (valid16[i] && vals16[i] != 0xFFFF) samples[n++] = vals16[i];
  }
  if (n < 8) return;

  uint16_t minV = 0xFFFF;
  uint16_t maxV = 0;
  for (uint8_t i = 0; i < n; i++) {
    if (samples[i] < minV) minV = samples[i];
    if (samples[i] > maxV) maxV = samples[i];
  }
  if (maxV <= minV + 20) return;

  float c1 = (float)minV;
  float c2 = (float)maxV;
  for (uint8_t iter = 0; iter < 12; iter++) {
    float s1 = 0.0f, s2 = 0.0f;
    uint8_t n1 = 0, n2 = 0;
    for (uint8_t i = 0; i < n; i++) {
      const float x = (float)samples[i];
      if (fabsf(x - c1) <= fabsf(x - c2)) {
        s1 += x;
        n1++;
      } else {
        s2 += x;
        n2++;
      }
    }
    if (n1 == 0 || n2 == 0) return;
    c1 = s1 / (float)n1;
    c2 = s2 / (float)n2;
  }
  if (c1 > c2) {
    const float t = c1;
    c1 = c2;
    c2 = t;
  }

  threshold = (int)lrintf((c1 + c2) * 0.5f);
  sepMm = fabsf(c2 - c1);
}

void Engine::bitsFromResidual(const int16_t residual16[16], const bool valid16[16],
                              int threshold, bool oneIsNear, uint8_t bits16[16]) {
  for (uint8_t i = 0; i < 16; i++) {
    if (!valid16[i]) {
      bits16[i] = 0;
      continue;
    }
    const bool near = (residual16[i] <= threshold);
    bits16[i] = oneIsNear ? (near ? 1 : 0) : (near ? 0 : 1);
  }
}

void Engine::bitsFromDepth(const uint16_t depth16[16], const bool valid16[16],
                           int threshold, bool oneIsNear, uint8_t bits16[16]) {
  for (uint8_t i = 0; i < 16; i++) {
    if (!valid16[i] || depth16[i] == 0xFFFF) {
      bits16[i] = 0;
      continue;
    }
    const bool near = ((int)depth16[i] <= threshold);
    bits16[i] = oneIsNear ? (near ? 1 : 0) : (near ? 0 : 1);
  }
}

void Engine::bitsToGrid(const uint8_t bits16[16], uint8_t g[4][4]) {
  for (uint8_t i = 0; i < 16; i++) g[i / 4][i % 4] = bits16[i];
}

void Engine::mirrorGridHorizontal(const uint8_t inG[4][4], uint8_t outG[4][4]) {
  for (uint8_t r = 0; r < 4; r++) {
    for (uint8_t c = 0; c < 4; c++) outG[r][c] = inG[r][3 - c];
  }
}

HRCode4x4::DecodeResult Engine::decodeWithMirrorFallback(const uint8_t bits16[16],
                                                          bool &usedMirror) {
  uint8_t g[4][4];
  bitsToGrid(bits16, g);
  uint8_t gm[4][4];
  mirrorGridHorizontal(g, gm);

  auto out = HRCode4x4::decodeGrid(g);
  auto mirrored = HRCode4x4::decodeGrid(gm);
  usedMirror = false;

  // Prefer Ok over BadCrc, regardless of mirror state. The legacy short-
  // circuit (return on first BadCrc, skip mirror) was masking real id=N
  // decodes from X-mirrored tags whose non-mirrored 270°-rotated bits
  // happened to fake-match orient with bad CRC.
  if (out.status == HRCode4x4::DecodeStatus::Ok) return out;
  if (mirrored.status == HRCode4x4::DecodeStatus::Ok) {
    usedMirror = true;
    return mirrored;
  }
  if (out.status == HRCode4x4::DecodeStatus::OrientationMatchedBadCrc) return out;
  if (mirrored.status == HRCode4x4::DecodeStatus::OrientationMatchedBadCrc) {
    usedMirror = true;
    return mirrored;
  }
  return out;
}

// DC34-155: decode the anchor layout "A3-D13" from a 4x4 bit grid (1=near).
// Anchors at (0,0),(0,3),(3,0). The 12 data cells row-major minus anchors AND
// minus the (3,3) rotation guard carry a SECDED(12,7) codeword (extended
// Hamming(11,7) + overall parity) over a 7-bit ID. SECDED corrects any single
// cell flip (the ~1-cell/frame VL53L5CX flicker) so a steady read stops
// oscillating and locks; it detects (rejects) any double flip so a corrupted
// read never unlocks the wrong ID. (3,3) is a rotation guard that STAYS 0 in
// every valid tag; rotation is disambiguated by picking the CLEAN-decoding
// rotation (see the loop below) rather than by hard-gating on the guard, since
// the sensor can misread the guard NEAR (adjacent data bumps bleed in). Must
// match build_anchor_grid() in hm_codegen.py. Codeword bit order = the 12
// non-guard cells row-major: cw[0..10] = Hamming positions 1..11, cw[11] = overall.
HRCode4x4::DecodeResult Engine::decodeAnchorBits(const uint8_t bits16[16], int expectedRot) {
  static const uint8_t DR[12] = {0,0,1,1,1,1,2,2,2,2,3,3};  // 12 non-guard cells,
  static const uint8_t DC[12] = {1,2,0,1,2,3,0,1,2,3,1,2};  // row-major (excl (3,3))
  static const uint8_t PPOS[4] = {1, 2, 4, 8};              // parity positions
  static const uint8_t DPOS[7] = {3, 5, 6, 7, 9, 10, 11};   // 7 data positions in 1..11
  HRCode4x4::DecodeResult res{};
  res.status = HRCode4x4::DecodeStatus::OrientationNotFound;
  res.id = 0; res.crc_read = 0; res.crc_calc = 0; res.rotation = 0;
  uint8_t base[4][4];
  for (uint8_t r = 0; r < 4; r++)
    for (uint8_t c = 0; c < 4; c++) base[r][c] = (uint8_t)(bits16[r * 4 + c] & 1);
  // ROTATION is fixed by the POSE, not guessed from the guard cell. The pose
  // sampling delivers the grid in the bbox frame at an unknown rotation. We do
  // NOT try the mirror (a mirror+rotation can re-align the 3 anchors while
  // scrambling the data into ANOTHER valid ID, e.g. 34<->64 -> wrong unlock).
  //
  // The guard cell (3,3) CANNOT disambiguate rotation: on many ids its neighbours
  // are data bumps (e.g. id-16 at (2,2)+(3,2)) that bleed into the guard corner,
  // so the sensor reads it NEAR. A guard-cell tiebreak (guard-far / lowest-index)
  // therefore mis-picked a WRONG-rotation CLEAN codeword (id-16 -> a clean id-2 at
  // 90/180, which then locked). Instead the caller passes `expectedRot`: the
  // localization's WEAKEST bbox corner IS the tag's guard, which fixes the true
  // de-rotation from the pose. With expectedRot>=0 we decode ONLY that rotation
  // (no cross-rotation ambiguity possible). expectedRot<0 (defensive) falls back
  // to a 4-rotation search that accepts a clean read ONLY if its id is UNIQUE
  // across rotations -- never a guessed tiebreak.
  uint8_t order[4]; uint8_t nOrder;
  if (expectedRot >= 0 && expectedRot < 4) { order[0] = (uint8_t)expectedRot; nOrder = 1; }
  else { order[0] = 0; order[1] = 1; order[2] = 2; order[3] = 3; nOrder = 4; }

  bool gatePassed = false;
  bool haveClean = false; uint8_t clRot = 0; int clId = -1; bool clAmbig = false;  // CLEAN read(s)
  bool haveOk = false; uint8_t okId = 0, okSyn = 0, okC = 0, okRot = 0;            // first CORRECTED
  for (uint8_t oi = 0; oi < nOrder; oi++) {
    const uint8_t t = order[oi];
    uint8_t g[4][4];
    for (uint8_t r = 0; r < 4; r++)
      for (uint8_t c = 0; c < 4; c++) g[r][c] = base[r][c];
    for (uint8_t k = 0; k < t; k++) {               // rotate 90 CW: out[r][c]=in[3-c][r]
      uint8_t tmp[4][4];
      for (uint8_t r = 0; r < 4; r++)
        for (uint8_t c = 0; c < 4; c++) tmp[r][c] = g[3 - c][r];
      for (uint8_t r = 0; r < 4; r++)
        for (uint8_t c = 0; c < 4; c++) g[r][c] = tmp[r][c];
    }
    if (!(g[0][0] && g[0][3] && g[3][0])) continue;   // 3-corner anchor gate
    gatePassed = true;

    // Read + SECDED-decode the 12-bit codeword. h[1..11]=cw[0..10]; overall=cw[11].
    uint8_t h[12];
    for (uint8_t i = 0; i < 11; i++) h[i + 1] = g[DR[i]][DC[i]];
    const uint8_t overall = g[DR[11]][DC[11]];
    uint8_t syn = 0;
    for (uint8_t pi = 0; pi < 4; pi++) {
      const uint8_t p = PPOS[pi];
      uint8_t s = 0;
      for (uint8_t j = 1; j <= 11; j++) if (j & p) s ^= h[j];
      if (s) syn = (uint8_t)(syn | p);
    }
    uint8_t c = overall;
    for (uint8_t j = 1; j <= 11; j++) c ^= h[j];

    bool good = false;
    if (syn == 0 && c == 0)      good = true;               // clean
    else if (c == 1) { if (syn >= 1 && syn <= 11) h[syn] ^= 1; good = true; }  // corrected
    else                         good = false;              // syn!=0 && c==0 -> double: reject
    if (!good) continue;

    uint8_t id = 0;
    for (uint8_t i = 0; i < 7; i++) id = (uint8_t)(id | (h[DPOS[i]] << i));

    if (syn == 0 && c == 0) {                               // CLEAN decode
      if (!haveClean) { haveClean = true; clId = (int)id; clRot = t; }
      else if ((int)id != clId) clAmbig = true;             // 2 clean ids (only in the 4-rot fallback)
    }
    if (!haveOk) { haveOk = true; okId = id; okSyn = syn; okC = c; okRot = t; }
  }

  if (haveClean && !clAmbig) {                              // the expectedRot clean, or a UNIQUE clean
    res.status = HRCode4x4::DecodeStatus::Ok;
    res.id = (uint8_t)clId; res.crc_read = 0; res.crc_calc = 0; res.rotation = clRot;
    return res;
  }
  if (haveClean && clAmbig) {                               // >1 clean id across rotations -> refuse (no guess)
    res.status = HRCode4x4::DecodeStatus::OrientationMatchedBadCrc;
    res.id = (uint8_t)clId; res.crc_read = 0; res.rotation = clRot;
    return res;
  }
  if (haveOk) {                                             // no clean -> the corrected read (at expectedRot)
    res.status = HRCode4x4::DecodeStatus::Ok;
    res.id = okId; res.crc_read = okSyn; res.crc_calc = okC; res.rotation = okRot;
    return res;
  }
  if (gatePassed)                                           // anchors seen but no valid codeword
    res.status = HRCode4x4::DecodeStatus::OrientationMatchedBadCrc;
  return res;                                               // else NoOrient
}

// Manual-entry decode (feature/manual-entry-grid, Task 1). Reuses the anchor
// SECDED decoder. The user enters the tag in the orientation they SEE it
// (TL/TR/BR corners raised, BL flat), which is X-mirrored vs the decoder's
// coordinate space -- so mirror col c -> 3-c before decoding, after which the
// decoder's 3-corner anchor gate passes. Rotation 0 only (a hand entry has no
// pose ambiguity). Returns the id on Ok (clean OR 1-cell-corrected), else -1.
int Engine::decodeUserGrid(const bool userView[4][4]) {
  uint8_t bits16[16];
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++)
      bits16[r * 4 + c] = userView[r][3 - c] ? 1 : 0;   // X-mirror user -> decoder
  HRCode4x4::DecodeResult res = decodeAnchorBits(bits16, /*expectedRot=*/0);
  return (res.status == HRCode4x4::DecodeStatus::Ok) ? (int)res.id : -1;
}

// Inverse of decodeUserGrid: encode a 7-bit id into the user-view 4x4 the operator
// sees (TL/TR/BR raised, BL flat). Uses the SAME SECDED(12,7) layout the decoder
// reads (DR/DC data cells, PPOS parity, DPOS data positions), then X-mirrors to
// user-view. decodeUserGrid(encodeUserGrid(id)) == id (clean). Used to pre-fill the
// manual grid with a scan's decoded pattern for verify-first confirmation.
void Engine::encodeUserGrid(int id, bool userView[4][4]) {
  static const uint8_t DR[12] = {0,0,1,1,1,1,2,2,2,2,3,3};
  static const uint8_t DC[12] = {1,2,0,1,2,3,0,1,2,3,1,2};
  static const uint8_t PPOS[4] = {1, 2, 4, 8};
  static const uint8_t DPOS[7] = {3, 5, 6, 7, 9, 10, 11};
  uint8_t h[12] = {0};
  for (int i = 0; i < 7; i++) h[DPOS[i]] = (uint8_t)((id >> i) & 1);   // data bits
  for (int pi = 0; pi < 4; pi++) {                                     // parity bits
    uint8_t p = PPOS[pi], s = 0;
    for (int j = 1; j <= 11; j++) if ((j & p) && j != p) s ^= h[j];
    h[p] = s;
  }
  uint8_t overall = 0;
  for (int j = 1; j <= 11; j++) overall ^= h[j];                      // overall parity
  uint8_t g[4][4] = {{0}};
  g[0][0] = g[0][3] = g[3][0] = 1;   // anchors raised
  g[3][3] = 0;                       // guard flat
  for (int i = 0; i < 11; i++) g[DR[i]][DC[i]] = h[i + 1];
  g[DR[11]][DC[11]] = overall;
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++) userView[r][c] = g[r][3 - c] != 0;    // X-mirror decoder -> user
}

int Engine::scoreCandidate(const HRCode4x4::DecodeResult &decoded,
                           uint8_t validCells,
                           float separationMm,
                           bool usedMirror,
                           uint8_t roiX,
                           uint8_t roiY,
                           uint8_t roiSize) const {
  int score = 0;
  if (decoded.status == HRCode4x4::DecodeStatus::Ok) {
    score += 700;
  } else if (decoded.status == HRCode4x4::DecodeStatus::OrientationMatchedBadCrc) {
    score += 60;
  }

  if (profile_ == Profile::Strict15mm && decoded.status != HRCode4x4::DecodeStatus::Ok) {
    score -= 250;
  }

  score += (int)validCells * 6;
  if (separationMm > 0.0f) {
    const float capped = (separationMm > 80.0f) ? 80.0f : separationMm;
    score += (int)(capped * (float)kScoreSepWeight);
  }
  if (separationMm > 120.0f) score -= kScoreBadSepPenalty;

  const float cx = (float)roiX + (float)roiSize * 0.5f;
  const float cy = (float)roiY + (float)roiSize * 0.5f;
  const float dx = cx - 4.0f;
  const float dy = cy - 4.0f;
  const float d2 = dx * dx + dy * dy;
  score -= (int)(d2 * (float)kScoreCenterWeight);

  const int szErr = abs((int)roiSize - (int)kRoiSizeTarget);
  score -= szErr * kScoreSizeWeight;

  if (useExpectedIdPrior_ && expectedId_ >= 0 && decoded.status == HRCode4x4::DecodeStatus::Ok) {
    const int hd = bitCount8((uint8_t)decoded.id ^ (uint8_t)expectedId_);
    score += (8 - hd) * 25;
  }

  if (usedMirror) score -= 5;
  return score;
}

Prompt Engine::buildPrompt(bool ok, bool locked, HRCode4x4::DecodeStatus status) const {
  if (locked) return Prompt::Locked;
  if (status == HRCode4x4::DecodeStatus::OrientationNotFound ||
      status == HRCode4x4::DecodeStatus::InvalidInput) {
    return Prompt::Searching;
  }
  if (!ok) return Prompt::HoldSteady;
  return Prompt::HoldSteady;
}

void Engine::clearLockInternal() {
  lockCandidateId_ = -1;
  lockRun_ = 0;
  lockCleanCount_ = 0;
  trackCandidateId_ = -1;
  trackRun_ = 0;
  lockedId_ = -1;
  badFrames_ = 0;
  // Clear CV vote history too — a stale window from a prior tag would
  // contaminate the next scan's first few frames.
  cvHistWrite_ = 0;
  cvHistCount_ = 0;
  for (uint8_t k = 0; k < kCvVoteHistory; k++) {
    for (uint8_t i = 0; i < 16; i++) {
      cvCellNearHist_[k][i] = 0;
      cvCellTotalHist_[k][i] = 0;
    }
  }
  // DC34-156: reset the plurality-vote window too (a stale window from a prior
  // tag would contaminate the next scan's commit).
  voteRingPos_ = 0;
  for (uint8_t i = 0; i < kVoteWindow; i++) { voteRingId_[i] = 0; voteRingW_[i] = 0; }
  for (int i = 0; i < 128; i++) { voteVotes_[i] = 0; voteClean_[i] = 0; }
}

void Engine::beginCalCapture() {
  clearZoneCal();
  calCapturing_ = true;
  memset(calSum_, 0, sizeof(calSum_));
  memset(calCount_, 0, sizeof(calCount_));
}

int Engine::finishCalCapture(int16_t out[64]) {
  calCapturing_ = false;
  float avg[64]; bool have[64]; int nValid = 0; double sumZ = 0.0;
  for (int i = 0; i < 64; i++) {
    have[i] = (calCount_[i] >= 2);
    avg[i]  = have[i] ? (float)calSum_[i] / (float)calCount_[i] : 0.0f;
    if (have[i]) { nValid++; sumZ += avg[i]; }
  }
  // Record coverage + mean captured distance for the UI (set before the fail
  // return so the modal can report coverage even when a calibration doesn't take).
  lastCalZones_ = nValid;
  lastCalAvgMm_ = nValid ? (int16_t)lrint(sumZ / (double)nValid) : 0;
  if (nValid < 30) return nValid;   // report coverage; hasCal_ stays false (fail)
  // Least-squares plane z = a*r + b*c + d over valid zones (removes absolute
  // distance + any hold tilt); the residual is the sensor's fixed bowl.
  double Srr=0,Scc=0,Src=0,Sr=0,Sc=0,Srz=0,Scz=0,Sz=0; int n=0;
  for (int i = 0; i < 64; i++) {
    if (!have[i]) continue;
    double r = (double)(i / 8), c = (double)(i % 8), z = (double)avg[i];
    Srr+=r*r; Scc+=c*c; Src+=r*c; Sr+=r; Sc+=c; Srz+=r*z; Scz+=c*z; Sz+=z; n++;
  }
  double M[3][4] = {{Srr,Src,Sr,Srz},{Src,Scc,Sc,Scz},{Sr,Sc,(double)n,Sz}};
  for (int col = 0; col < 3; col++) {          // Gaussian elimination w/ partial pivot
    int piv = col;
    for (int r2 = col + 1; r2 < 3; r2++) if (fabs(M[r2][col]) > fabs(M[piv][col])) piv = r2;
    if (fabs(M[piv][col]) < 1e-9) return nValid;   // singular -> fail (hasCal_ stays false)
    for (int k = 0; k < 4; k++) { double t = M[col][k]; M[col][k] = M[piv][k]; M[piv][k] = t; }
    for (int r2 = 0; r2 < 3; r2++) if (r2 != col) {
      double f = M[r2][col] / M[col][col];
      for (int k = col; k < 4; k++) M[r2][k] -= f * M[col][k];
    }
  }
  const double a = M[0][3]/M[0][0], b = M[1][3]/M[1][1], d = M[2][3]/M[2][2];
  for (int i = 0; i < 64; i++) {
    int16_t off = 0;
    if (have[i]) off = (int16_t)lrint((double)avg[i] - (a*(double)(i/8) + b*(double)(i%8) + d));
    zoneCal_[i] = off;
    out[i] = off;
  }
  hasCal_ = true;
  return nValid;
}

Result Engine::processFrame(const int16_t distanceMm64[64],
                            const uint8_t nbTargetDetected64[64],
                            const uint8_t targetStatus64[64],
                            bool oneIsNear) {
  Result out{};
  out.decode.status = HRCode4x4::DecodeStatus::InvalidInput;
  out.lockedId = lockedId_;
  out.runRequired = lockRequired_;

  int16_t mm8[8][8];
  bool valid8[8][8];
  orientFrame(distanceMm64, nbTargetDetected64, targetStatus64, mm8, valid8);
  // Flat-field calibration CAPTURE: accumulate raw (pre-cal) zone depths. Cal is
  // cleared while capturing, so mm8 here is uncorrected.
  if (calCapturing_) {
    for (uint8_t r = 0; r < 8; r++)
      for (uint8_t c = 0; c < 8; c++)
        if (valid8[r][c] && mm8[r][c] > 0) {
          calSum_[r * 8 + c] += mm8[r][c];
          if (calCount_[r * 8 + c] < 60000) calCount_[r * 8 + c]++;
        }
  }
  // Per-sensor flat-field calibration: cancel the fixed zone-geometry distortion
  // (a flat surface reads center-far/edges-near by ~8mm) so the near/far bump
  // threshold is clean. Applied before filtering/clustering so everything
  // downstream sees a flattened field. No-op until a calibration is captured.
  if (hasCal_) {
    for (uint8_t r = 0; r < 8; r++)
      for (uint8_t c = 0; c < 8; c++)
        if (valid8[r][c]) mm8[r][c] = (int16_t)((int)mm8[r][c] - (int)zoneCal_[r * 8 + c]);
  }
  pushHistory(mm8, valid8);

  int16_t filtMm[8][8];
  bool filtValid[8][8];
  median8x8(filtMm, filtValid);

  // CV mode dispatch: alternative pipeline that locates the tag via
  // depth clustering instead of brute-force ROI search.
  if (useCV_) {
    return processFrameCV(filtMm, filtValid, oneIsNear);
  }

  int bestScore = -100000;
  HRCode4x4::DecodeResult bestDecoded{};
  bestDecoded.status = HRCode4x4::DecodeStatus::InvalidInput;
  uint8_t bestValid = 0;
  float bestSep = 0.0f;
  int bestThr = 0;
  bool bestResidualMode = true;
  bool bestMirror = false;
  uint8_t bestX = 0, bestY = 0, bestS = 0;
  int16_t bestResidual[16] = {0};
  bool bestResidualValid[16] = {false};
  uint8_t bestBits[16] = {0};
  uint16_t bestDepth[16] = {0};
  bool bestDepthValid[16] = {false};

  for (uint8_t roiSize = kRoiMin; roiSize <= kRoiMax; roiSize++) {
    const uint8_t maxStart = 8 - roiSize;
    for (uint8_t roiY = 0; roiY <= maxStart; roiY++) {
      for (uint8_t roiX = 0; roiX <= maxStart; roiX++) {
        uint16_t depth[16];
        bool depthValid[16];
        int16_t residual[16];
        bool residualValid[16];
        uint8_t bits[16];

        const uint8_t validCells = sampleRoi4x4(filtMm, filtValid, roiX, roiY, roiSize,
                                                depth, depthValid);

        detrend4x4(depth, depthValid, residual, residualValid);

        int thr = 0;
        float sep = 0.0f;
        splitAdaptiveSigned(residual, residualValid, thr, sep);

        bool residualMode = true;
        if (sep > 0.0f) {
          bitsFromResidual(residual, residualValid, thr, oneIsNear, bits);
        } else {
          int absThr = -1;
          float absSep = 0.0f;
          splitAdaptiveUnsigned(depth, depthValid, absThr, absSep);
          if (absThr < 0) absThr = 250;
          thr = absThr;
          sep = absSep;
          residualMode = false;
          bitsFromDepth(depth, depthValid, thr, oneIsNear, bits);
        }

        bool usedMirror = false;
        auto decoded = decodeWithMirrorFallback(bits, usedMirror);
        const int score = scoreCandidate(decoded, validCells, sep, usedMirror,
                                         roiX, roiY, roiSize);
        if (score > bestScore) {
          bestScore = score;
          bestDecoded = decoded;
          bestValid = validCells;
          bestSep = sep;
          bestThr = thr;
          bestResidualMode = residualMode;
          bestMirror = usedMirror;
          bestX = roiX;
          bestY = roiY;
          bestS = roiSize;
          for (uint8_t i = 0; i < 16; i++) {
            bestResidual[i] = residual[i];
            bestResidualValid[i] = residualValid[i];
            bestBits[i] = bits[i];
            bestDepth[i] = depth[i];
            bestDepthValid[i] = depthValid[i];
          }
        }
      }
    }
  }

  // Snapshot for dumpDebug — populated unconditionally each frame so callers
  // can inspect even on bad/no-decode frames.
  dbgFrameCount_++;
  for (uint8_t r = 0; r < 8; r++) {
    for (uint8_t c = 0; c < 8; c++) {
      dbgRawMm_[r][c]   = mm8[r][c];
      dbgRawValid_[r][c] = valid8[r][c];
      dbgFiltMm_[r][c]  = filtMm[r][c];
      dbgFiltValid_[r][c] = filtValid[r][c];
    }
  }
  for (uint8_t i = 0; i < 16; i++) {
    dbgDepth16_[i]      = bestDepth[i];
    dbgDepth16Valid_[i] = bestDepthValid[i];
    dbgResidual16_[i]   = bestResidual[i];
    dbgResidualValid_[i] = bestResidualValid[i];
    dbgBits16_[i]       = bestBits[i];
  }
  dbgRoiX_ = bestX;
  dbgRoiY_ = bestY;
  dbgRoiSize_ = bestS;
  dbgThreshold_ = bestThr;
  dbgSepMm_ = bestSep;
  dbgResidualMode_ = bestResidualMode;
  dbgUsedMirror_ = bestMirror;
  dbgDecode_ = bestDecoded;

  // Optional whitelist: drop CRC-valid candidates whose IDs aren't in
  // the validator's set, before they can advance the tracker or vote
  // toward a lock. Lets host firmware fail-fast on noise that flipped
  // into a non-collectible ID.
  const bool whitelistOk =
      (bestDecoded.status == HRCode4x4::DecodeStatus::Ok) &&
      (!idValidator_ || idValidator_((int)bestDecoded.id));

  const bool ok =
      whitelistOk &&
      (bestValid >= kMinValidCells) &&
      (bestSep >= minSeparationMm_);

  // Tracking run for UI progress (less strict than lock gate).
  if (whitelistOk) {
    if ((int)bestDecoded.id == trackCandidateId_) {
      if (trackRun_ < 255) trackRun_++;
    } else {
      trackCandidateId_ = (int)bestDecoded.id;
      trackRun_ = 1;
    }
  } else {
    trackCandidateId_ = -1;
    trackRun_ = 0;
  }

  if (ok) {
    badFrames_ = 0;
    if ((int)bestDecoded.id == lockCandidateId_) {
      if (lockRun_ < 255) lockRun_++;
    } else {
      lockCandidateId_ = (int)bestDecoded.id;
      lockRun_ = 1;
    }
    if (lockRun_ >= lockRequired_) {
      lockedId_ = (int)bestDecoded.id;
    }
  } else {
    lockCandidateId_ = -1;
    lockRun_ = 0;
    if (badFrames_ < 255) badFrames_++;
    if (badFrames_ >= kLockClearBadFrames) lockedId_ = -1;
  }

  out.decode = bestDecoded;
  out.ok = ok;
  out.lockedId = lockedId_;
  out.run = lockRun_;
  out.runRequired = lockRequired_;
  if (lockedId_ >= 0) {
    out.progress = 100;
  } else if (lockRequired_ > 0) {
    uint8_t p = (uint8_t)((100UL * trackRun_) / lockRequired_);
    out.progress = (p > 100) ? 100 : p;
  } else {
    out.progress = 0;
  }
  out.validCells = bestValid;
  out.separationMm = bestSep;
  out.threshold = bestThr;
  out.residualMode = bestResidualMode;
  out.usedMirror = bestMirror;
  out.roiX = bestX;
  out.roiY = bestY;
  out.roiSize = bestS;
  out.prompt = buildPrompt(ok, lockedId_ >= 0, bestDecoded.status);

  for (uint8_t i = 0; i < 16; i++) {
    out.cells[i].valid = bestResidualValid[i];
    out.cells[i].bit = bestBits[i];
    if (!bestResidualValid[i]) {
      out.cells[i].confidence = 0;
    } else {
      const int d = abs((int)bestResidual[i] - bestThr);
      const int q = d * 8;
      out.cells[i].confidence = (uint8_t)(q > 255 ? 255 : q);
    }
  }

  return out;
}

void Engine::dumpDebug(Print &out) const {
  out.print(F("=== HR DEBUG frame "));
  out.print(dbgFrameCount_);
  out.print(F(" ("));
  out.print(useCV_ ? F("CV") : F("legacy"));
  out.println(F(") ==="));

  out.println(F("raw 8x8 mm (orientFrame output):"));
  for (uint8_t r = 0; r < 8; r++) {
    out.print(F("  "));
    for (uint8_t c = 0; c < 8; c++) {
      if (dbgRawValid_[r][c]) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%5d ", dbgRawMm_[r][c]);
        out.print(buf);
      } else {
        out.print(F("    . "));
      }
    }
    out.println();
  }

  out.println(F("filt 8x8 mm (median-filtered):"));
  for (uint8_t r = 0; r < 8; r++) {
    out.print(F("  "));
    for (uint8_t c = 0; c < 8; c++) {
      if (dbgFiltValid_[r][c]) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%5d ", dbgFiltMm_[r][c]);
        out.print(buf);
      } else {
        out.print(F("    . "));
      }
    }
    out.println();
  }

  out.print(F("ROI: x=")); out.print(dbgRoiX_);
  out.print(F(" y=")); out.print(dbgRoiY_);
  out.print(F(" size=")); out.println(dbgRoiSize_);

  out.println(F("4x4 sampled depth (mm) at chosen ROI:"));
  for (uint8_t r = 0; r < 4; r++) {
    out.print(F("  "));
    for (uint8_t c = 0; c < 4; c++) {
      uint8_t i = r * 4 + c;
      if (dbgDepth16Valid_[i]) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%5u ", dbgDepth16_[i]);
        out.print(buf);
      } else {
        out.print(F("    . "));
      }
    }
    out.println();
  }

  out.println(F("4x4 residual (depth - plane fit):"));
  for (uint8_t r = 0; r < 4; r++) {
    out.print(F("  "));
    for (uint8_t c = 0; c < 4; c++) {
      uint8_t i = r * 4 + c;
      if (dbgResidualValid_[i]) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%+5d ", dbgResidual16_[i]);
        out.print(buf);
      } else {
        out.print(F("    . "));
      }
    }
    out.println();
  }

  out.println(F("4x4 bits:"));
  for (uint8_t r = 0; r < 4; r++) {
    out.print(F("  "));
    for (uint8_t c = 0; c < 4; c++) {
      out.print(dbgBits16_[r * 4 + c]);
      out.print(' ');
    }
    out.println();
  }

  out.print(F("threshold=")); out.print(dbgThreshold_);
  out.print(F(" sep=")); out.print(dbgSepMm_, 2);
  out.print(F("mm tilt=")); out.print(dbgCvTiltMmPerZone_, 2);
  out.print(F(" mode=")); out.print(dbgResidualMode_ ? F("residual") : F("absolute"));
  out.print(F(" mirror=")); out.println(dbgUsedMirror_ ? F("yes") : F("no"));
  // Corner blob sizes (near-zone count) TL/TR/BL/BR + which was picked as guard.
  // The GUARD is the empty (~0) corner; if the picked guard isn't the empty one,
  // the pose is being mis-solved -> tells us which corner to pin.
  out.print(F("corners TL/TR/BL/BR="));
  out.print(dbgCorner_[0]); out.print('/'); out.print(dbgCorner_[1]); out.print('/');
  out.print(dbgCorner_[2]); out.print('/'); out.print(dbgCorner_[3]);
  out.print(F(" guard=")); out.print(dbgMiss_);
  out.print(F(" weakest=")); out.print(dbgWeakest_);
  out.print(F(" pin=")); out.print((int)fixedGuardCorner_);
  out.print(F(" (0TL 1TR 2BL 3BR)"));
  out.println();

  out.print(F("decode: id=")); out.print(dbgDecode_.id);
  out.print(F(" status="));
  switch (dbgDecode_.status) {
    case HRCode4x4::DecodeStatus::Ok: out.print(F("Ok")); break;
    case HRCode4x4::DecodeStatus::OrientationMatchedBadCrc: out.print(F("BadCRC")); break;
    case HRCode4x4::DecodeStatus::OrientationNotFound: out.print(F("NoOrient")); break;
    case HRCode4x4::DecodeStatus::InvalidInput: out.print(F("InvalidInput")); break;
  }
  // Anchor mode repurposes these two fields: crc_read=SECDED syndrome (0=clean,
  // 1..11=corrected bit position), crc_calc=overall-parity check (1=single-err
  // corrected, 0 with syn!=0=double-err rejected). Legacy mode = CRC read/calc.
  out.print(useAnchor_ ? F(" syn=") : F(" crc_read="));  out.print(dbgDecode_.crc_read);
  out.print(useAnchor_ ? F(" ovl=") : F(" crc_calc=")); out.print(dbgDecode_.crc_calc);
  out.print(F(" rotation=")); out.println(dbgDecode_.rotation * 90);

  out.print(F("lock: candidate=")); out.print(lockCandidateId_);
  out.print(F(" run=")); out.print(lockRun_);
  out.print(F("/")); out.print(lockRequired_);
  out.print(F(" clean=")); out.print(lockCleanCount_);
  out.print(F(" lockedId=")); out.println(lockedId_);

  if (useAnchor_ && voteLockEnabled_) {
    // Engine-side plurality view (analogue of the host 'saw' histogram): top-3
    // weighted vote-getters + the margin ratio. Explains lock vs stall at a glance.
    uint8_t t1i=0,t1v=0,t2i=0,t2v=0,t3i=0,t3v=0;
    for (int i = 0; i < 128; i++) {
      const uint8_t v = voteVotes_[i];
      if (v > t1v) { t3i=t2i;t3v=t2v; t2i=t1i;t2v=t1v; t1i=(uint8_t)i;t1v=v; }
      else if (v > t2v) { t3i=t2i;t3v=t2v; t2i=(uint8_t)i;t2v=v; }
      else if (v > t3v) { t3i=(uint8_t)i;t3v=v; }
    }
    out.print(F("votes: ")); out.print(t1i); out.print('x'); out.print(t1v);
    out.print(';'); out.print(t2i); out.print('x'); out.print(t2v);
    out.print(';'); out.print(t3i); out.print('x'); out.print(t3v);
    out.print(F(" cleanTop=")); out.print(voteClean_[t1i]);
    out.print(F(" margin=")); out.print(t2v ? (int)((10*(int)t1v)/(int)t2v) : 999);
    out.print(F(" floor=")); out.print(kVoteEvidenceFloor);
    out.print(F(" needX10=")); out.println(kVoteMarginX10);
  }

  // CV-mode-specific block.
  if (useCV_) {
    out.println(F("--- CV ---"));
    if (dbgCvReject_) {
      out.print(F("REJECTED: "));
      out.println(dbgCvReject_);
    }
    out.print(F("clusters: near=")); out.print(dbgCvNearMm_);
    out.print(F("mm  far=")); out.print(dbgCvFarMm_);
    out.print(F("mm  sep=")); out.print(dbgCvSepMm_, 2); out.println(F("mm"));
    if (dbgCvBboxValid_) {
      out.print(F("bbox: rows ")); out.print(dbgCvBboxMinR_);
      out.print(F("..")); out.print(dbgCvBboxMaxR_);
      out.print(F(" cols ")); out.print(dbgCvBboxMinC_);
      out.print(F("..")); out.println(dbgCvBboxMaxC_);
    } else {
      out.println(F("bbox: <none>"));
    }
    out.println(F("near-mask 8x8 (N=near, .=far/invalid):"));
    for (uint8_t r = 0; r < 8; r++) {
      out.print(F("  "));
      for (uint8_t c = 0; c < 8; c++) {
        out.print((dbgCvNearMask_[r] & (1u << c)) ? 'N' : '.');
        out.print(' ');
      }
      out.println();
    }
    out.println(F("4x4 cell vote totals (NEAR/total):"));
    for (uint8_t r = 0; r < 4; r++) {
      out.print(F("  "));
      for (uint8_t c = 0; c < 4; c++) {
        const uint8_t i = (uint8_t)(r * 4 + c);
        char buf[10];
        snprintf(buf, sizeof(buf), "%2u/%-2u ", dbgCvCellNear_[i], dbgCvCellTotal_[i]);
        out.print(buf);
      }
      out.println();
    }
  }

  out.println(F("=== end frame ==="));
}

// ----------------------------------------------------------------------
// CV pipeline
// ----------------------------------------------------------------------
//
// Algorithm:
//   1. Collect all valid depth values from the 8x8 filtered grid.
//   2. K-means k=2 (1D) with min/max init, ≤16 iterations, until centroids stable.
//   3. Sanity gate: |near - far| within [kCvSepMinMm, kCvSepMaxMm].
//   4. Classify each zone NEAR or FAR (by which centroid is closer).
//   5. Find bounding box of NEAR zones.
//   6. Sanity gate: bbox dimensions, total NEAR-zone density inside bbox.
//   7. Divide bbox into a 4x4 cell grid (proportional to bbox extent, not FoV).
//   8. For each tag cell, count NEAR vs total zones whose center falls inside
//      the cell. Bit = 1 if NEAR > FAR (strict majority).
//   9. Run decodeWithMirrorFallback to handle 4 rotations + mirror.
//  10. Build the Result struct, including the standard lock-state machinery.

Result Engine::processFrameCV(const int16_t mm8[8][8], const bool valid8[8][8],
                              bool oneIsNear) {
  Result out{};
  out.decode.status = HRCode4x4::DecodeStatus::InvalidInput;
  out.lockedId = lockedId_;
  out.runRequired = lockRequired_;

  // Reset CV debug snapshot for this frame.
  dbgFrameCount_++;
  dbgCvBboxValid_ = false;
  dbgCvReject_ = nullptr;
  dbgCvNearMm_ = 0;
  dbgCvFarMm_ = 0;
  dbgCvSepMm_ = 0.0f;
  for (uint8_t i = 0; i < 8; i++) dbgCvNearMask_[i] = 0;
  for (uint8_t i = 0; i < 16; i++) {
    dbgCvCellNear_[i] = 0;
    dbgCvCellTotal_[i] = 0;
    dbgBits16_[i] = 0;
    dbgDepth16_[i] = 0;
    dbgDepth16Valid_[i] = false;
  }
  // Mirror the raw->dbg snapshot (so dumpDebug can show the same 8x8 view).
  for (uint8_t r = 0; r < 8; r++) {
    for (uint8_t c = 0; c < 8; c++) {
      dbgRawMm_[r][c] = mm8[r][c];   // reuse "raw" slot for the median-filtered view in CV mode
      dbgRawValid_[r][c] = valid8[r][c];
      dbgFiltMm_[r][c] = mm8[r][c];
      dbgFiltValid_[r][c] = valid8[r][c];
    }
  }
  dbgUsedMirror_ = false;
  dbgRoiX_ = 0; dbgRoiY_ = 0; dbgRoiSize_ = 0;
  dbgThreshold_ = 0;
  dbgSepMm_ = 0.0f;
  dbgResidualMode_ = false;

  // 1. Collect valid depths. Reject obvious outliers — single-zone glitches
  //    (e.g. a stray 191mm reading from a distant background object) can
  //    blow up the k-means FAR centroid and kill the separation gate.
  //    Compute median of raw depths, then keep only zones within ±25mm of it.
  int16_t depthsAll[64];
  uint8_t nAll = 0;
  for (uint8_t r = 0; r < 8; r++) {
    for (uint8_t c = 0; c < 8; c++) {
      if (valid8[r][c] && mm8[r][c] > 0) depthsAll[nAll++] = mm8[r][c];
    }
  }
  if (nAll < kCvMinValidZones) {
    dbgCvReject_ = "not enough valid zones";
    out.prompt = Prompt::Searching;
    return out;
  }
  // Cheap median via insertion sort.
  for (uint8_t i = 1; i < nAll; i++) {
    int16_t key = depthsAll[i];
    int8_t j = (int8_t)i - 1;
    while (j >= 0 && depthsAll[j] > key) {
      depthsAll[j + 1] = depthsAll[j];
      j--;
    }
    depthsAll[j + 1] = key;
  }
  const int16_t medMm = depthsAll[nAll / 2];
  // Keep only depths within ±25mm of median; throw out outliers, but track
  // which zones survive so we don't accidentally classify outlier zones as
  // NEAR or FAR later.
  bool valid8inlier[8][8];
  for (uint8_t r = 0; r < 8; r++) {
    for (uint8_t c = 0; c < 8; c++) {
      valid8inlier[r][c] = valid8[r][c] && mm8[r][c] > 0 &&
                           (abs((int)mm8[r][c] - (int)medMm) <= 25);
    }
  }
  int16_t depths[64];
  uint8_t nValid = 0;
  for (uint8_t r = 0; r < 8; r++) {
    for (uint8_t c = 0; c < 8; c++) {
      if (valid8inlier[r][c]) depths[nValid++] = mm8[r][c];
    }
  }
  if (nValid < kCvMinValidZones) {
    dbgCvReject_ = "not enough valid zones";
    out.prompt = Prompt::Searching;
    return out;
  }

  // 2. K-means k=2 with min/max init.
  int16_t minV = depths[0], maxV = depths[0];
  for (uint8_t i = 1; i < nValid; i++) {
    if (depths[i] < minV) minV = depths[i];
    if (depths[i] > maxV) maxV = depths[i];
  }
  float c1 = (float)minV;
  float c2 = (float)maxV;
  for (uint8_t iter = 0; iter < 16; iter++) {
    float s1 = 0.0f, s2 = 0.0f;
    uint16_t n1 = 0, n2 = 0;
    for (uint8_t i = 0; i < nValid; i++) {
      const float v = (float)depths[i];
      if (fabsf(v - c1) <= fabsf(v - c2)) { s1 += v; n1++; }
      else                                  { s2 += v; n2++; }
    }
    if (n1 == 0 || n2 == 0) break;
    const float nc1 = s1 / (float)n1;
    const float nc2 = s2 / (float)n2;
    if (fabsf(nc1 - c1) < 0.5f && fabsf(nc2 - c2) < 0.5f) {
      c1 = nc1; c2 = nc2;
      break;
    }
    c1 = nc1; c2 = nc2;
  }
  if (c1 > c2) { float t = c1; c1 = c2; c2 = t; }
  const int16_t nearMm = (int16_t)lrintf(c1);
  const int16_t farMm  = (int16_t)lrintf(c2);
  const float sepMm    = (float)(farMm - nearMm);
  dbgCvNearMm_ = nearMm;
  dbgCvFarMm_ = farMm;
  dbgCvSepMm_ = sepMm;

  // 3. Separation gate.
  if (sepMm < (float)kCvSepMinMm) {
    dbgCvReject_ = "near/far separation too small (no tag in view?)";
    out.prompt = Prompt::Searching;
    return out;
  }
  if (sepMm > (float)kCvSepMaxMm) {
    dbgCvReject_ = "near/far separation too large (background interference?)";
    out.prompt = Prompt::Searching;
    return out;
  }

  // 4. Classify each zone. Threshold midway between centroids, with a
  //    deadband of ±2mm where the zone is treated as "ambiguous" and
  //    excluded from cell-vote totals. Spillover-photons from adjacent
  //    bumps register weakly above the FAR cluster (right at the threshold)
  //    and pollute neighboring cells if we count them as either NEAR or FAR.
  const float threshold = (c1 + c2) * 0.5f;
  const float kDeadbandMm = 2.0f;
  bool nearGrid[8][8] = {{false}};
  bool ambGrid[8][8]  = {{false}};   // ambiguous: in deadband
  uint8_t nearCount = 0;
  for (uint8_t r = 0; r < 8; r++) {
    for (uint8_t c = 0; c < 8; c++) {
      if (!valid8inlier[r][c]) continue;
      const float v = (float)mm8[r][c];
      if (fabsf(v - threshold) < kDeadbandMm) {
        ambGrid[r][c] = true;
        continue;
      }
      const bool isNear = (v <= threshold);
      nearGrid[r][c] = isNear;
      if (isNear) {
        nearCount++;
        dbgCvNearMask_[r] |= (uint8_t)(1u << c);
      }
    }
  }

  // 4c. Tag-relative TILT: least-squares depth gradient over the NEAR zones.
  // A flat-on tag reads ~constant depth across its zones (gradient ~0); a badge
  // angled to the tag reads a near->far ramp. Steep tilt is what makes the
  // 3-anchor affine mis-sample data cells (perspective the linear fit can't
  // model) -> skew misreads (e.g. hand-held id-16 -> 96). Measured here; the
  // lock FSM gates on it. gr = cov(r,z)/var(r), gc = cov(c,z)/var(c).
  {
    float n = 0, Sr = 0, Sc = 0, Sz = 0, Srz = 0, Scz = 0, Srr = 0, Scc = 0;
    for (uint8_t r = 0; r < 8; r++)
      for (uint8_t c = 0; c < 8; c++)
        if (nearGrid[r][c]) {
          const float z = (float)mm8[r][c];
          n += 1; Sr += r; Sc += c; Sz += z;
          Srz += r * z; Scz += c * z; Srr += (float)r * r; Scc += (float)c * c;
        }
    float tilt = 0.0f, gr = 0.0f, gc = 0.0f;
    if (n >= 4) {
      const float mr = Sr / n, mc = Sc / n, mz = Sz / n;
      const float vr = Srr / n - mr * mr, vc = Scc / n - mc * mc;
      gr = (vr > 0.25f) ? (Srz / n - mr * mz) / vr : 0.0f;
      gc = (vc > 0.25f) ? (Scz / n - mc * mz) / vc : 0.0f;
      tilt = sqrtf(gr * gr + gc * gc);   // mm of depth change per zone step
    }
    dbgCvTiltMmPerZone_ = tilt;
    dbgCvTiltGr_ = gr;   // signed gradient components (row, col) -> tag-relative bubble
    dbgCvTiltGc_ = gc;
  }

  // 5. Bounding box of NEAR zones.
  if (nearCount == 0) {
    dbgCvReject_ = "no NEAR zones";
    out.prompt = Prompt::Searching;
    return out;
  }
  uint8_t bMinR = 8, bMinC = 8, bMaxR = 0, bMaxC = 0;
  for (uint8_t r = 0; r < 8; r++) {
    for (uint8_t c = 0; c < 8; c++) {
      if (!nearGrid[r][c]) continue;
      if (r < bMinR) bMinR = r;
      if (c < bMinC) bMinC = c;
      if (r > bMaxR) bMaxR = r;
      if (c > bMaxC) bMaxC = c;
    }
  }
  dbgCvBboxMinR_ = bMinR;
  dbgCvBboxMinC_ = bMinC;
  dbgCvBboxMaxR_ = bMaxR;
  dbgCvBboxMaxC_ = bMaxC;
  dbgCvBboxValid_ = true;

  // 6. Bbox sanity gates.
  uint8_t bboxH = bMaxR - bMinR + 1;
  uint8_t bboxW = bMaxC - bMinC + 1;
  if (bboxH < kCvBboxMin || bboxW < kCvBboxMin) {
    dbgCvReject_ = "bbox too small";
    out.prompt = Prompt::Searching;
    return out;
  }
  if (bboxH > kCvBboxMax || bboxW > kCvBboxMax) {
    // The near-blob overfills the FoV -> the tag is TOO CLOSE (this is the
    // extreme-close case that returned before the distance-prompt logic, so the
    // user saw no "Back up"). Say it here.
    dbgCvReject_ = "bbox too large";
    out.prompt = Prompt::MoveFarther;   // "Back up"
    return out;
  }

  // 6a. Pad asymmetric bboxes to the tag's expected physical footprint.
  // A 60mm tag at scan distance covers ~6 zones; an asymmetric bit pattern
  // (e.g. canonical 48 with empty bottom 2 rows) shrinks bbox-of-NEAR to
  // half that, which then squashes the 4-cell logical grid into the wrong
  // zone span and breaks decode. Expand the smaller dimension symmetrically
  // around the bbox center so cell sampling spans the actual tag area.
  constexpr uint8_t kCvBboxPadTo = 6;
  auto padDim = [](uint8_t &lo, uint8_t &hi, uint8_t want) {
    int16_t mid = (int16_t)(lo + hi) / 2;
    int16_t newLo = mid - (want - 1) / 2;
    int16_t newHi = newLo + want - 1;
    if (newLo < 0) { newLo = 0; newHi = want - 1; }
    if (newHi > 7) { newHi = 7; newLo = 8 - want; }
    lo = (uint8_t)newLo;
    hi = (uint8_t)newHi;
  };
  if (bboxH < kCvBboxPadTo) {
    padDim(bMinR, bMaxR, kCvBboxPadTo);
    bboxH = bMaxR - bMinR + 1;
  }
  if (bboxW < kCvBboxPadTo) {
    padDim(bMinC, bMaxC, kCvBboxPadTo);
    bboxW = bMaxC - bMinC + 1;
  }
  // Update debug snapshot to reflect the padded bbox.
  dbgCvBboxMinR_ = bMinR;
  dbgCvBboxMinC_ = bMinC;
  dbgCvBboxMaxR_ = bMaxR;
  dbgCvBboxMaxC_ = bMaxC;

  // 7. Map each zone INSIDE the bbox to one of the 4x4 cells, proportionally.
  //    Cell (cr, cc) covers bbox rows [bMinR + cr*bboxH/4, bMinR + (cr+1)*bboxH/4)
  //    and similarly for cols.
  //    For each zone, we pick the cell whose row/col bucket contains the
  //    zone's row/col offset within the bbox. We use ((r - bMinR + 0.5) * 4 / bboxH)
  //    floor to compute the bucket.
  if (!useAnchor_) {
    // Legacy: blind bbox-quarter mapping (fragile to sub-cell misalignment).
    for (uint8_t r = bMinR; r <= bMaxR; r++) {
      for (uint8_t c = bMinC; c <= bMaxC; c++) {
        if (!valid8inlier[r][c]) continue;
        if (ambGrid[r][c]) continue;  // skip deadband zones — they don't vote
        const uint8_t rOff = r - bMinR;
        const uint8_t cOff = c - bMinC;
        uint8_t cr = (uint8_t)(((uint16_t)rOff * 4U) / bboxH);
        uint8_t cc = (uint8_t)(((uint16_t)cOff * 4U) / bboxW);
        if (cr > 3) cr = 3;
        if (cc > 3) cc = 3;
        const uint8_t i = (uint8_t)(cr * 4 + cc);
        dbgCvCellTotal_[i]++;
        if (nearGrid[r][c]) dbgCvCellNear_[i]++;
      }
    }
  } else {
    // DC34-155 anchor spec: ORIENTATION-AGNOSTIC pose. Localize near-pad
    // centroids at all 4 bbox corners, drop the WEAKEST (the tag's absent 4th
    // corner), solve a zone->unit affine from the 3 present pads mapped to their
    // bbox-corner unit positions, and assign each zone to its pose-corrected
    // cell. decodeAnchorBits() then resolves the rotation/mirror. The old
    // fixed-TL/TR/BL version skewed the affine on a mirrored/rotated tag and
    // misread the ID cells -> id 0 / BadCRC.
    // GRADED weighted corner rings (replaces the old binary 30%-bbox windows).
    // Each of the 4 bbox corners scores near-zones by Chebyshev distance from the
    // corner: weight 3 in the core 2x2, 2 in the next ring, 1 in the next, 0
    // beyond. Two wins: (a) an anchor's centroid is pulled to the true corner and
    // shrugs off a stray edge zone; (b) a MID-FIELD data cluster (the id-47
    // "decoy" -- a fake 3rd anchor grabbed from the tag interior) scores ~0 and
    // can't be chosen. Ring weights/extent are tunable.
    const float cornR[4] = {(float)bMinR, (float)bMinR, (float)bMaxR, (float)bMaxR}; // TL TR BL BR
    const float cornC[4] = {(float)bMinC, (float)bMaxC, (float)bMinC, (float)bMaxC};
    static const float CU[4] = {0.125f, 0.875f, 0.125f, 0.875f};   // 0=TL 1=TR 2=BL 3=BR (u=col)
    static const float CV[4] = {0.125f, 0.125f, 0.875f, 0.875f};   // (v=row)
    float cI[4], cJ[4], wsum[4]; int strength[4];
    for (uint8_t k = 0; k < 4; k++) {
      float si = 0.0f, sj = 0.0f, wtot = 0.0f;
      for (uint8_t r = 0; r < 8; r++)
        for (uint8_t c = 0; c < 8; c++) {
          if (!nearGrid[r][c]) continue;
          const float dr = fabsf((float)r - cornR[k]);
          const float dc = fabsf((float)c - cornC[k]);
          const float d  = dr > dc ? dr : dc;                 // Chebyshev distance
          const float w  = d <= 1.0f ? 3.0f : d <= 2.0f ? 2.0f : d <= 3.0f ? 1.0f : 0.0f;
          if (w <= 0.0f) continue;
          si += w * ((float)r + 0.5f); sj += w * ((float)c + 0.5f); wtot += w;
        }
      wsum[k] = wtot;
      strength[k] = (int)lrintf(wtot);
      cI[k] = wtot > 0.0f ? si / wtot : cornR[k] + 0.5f;
      cJ[k] = wtot > 0.0f ? sj / wtot : cornC[k] + 0.5f;
    }
    // Guard corner. FIXED-ORIENTATION tags (fixedGuardCorner_ 0..3) PIN it -- we
    // KNOW which corner is the empty guard by how the tag is presented, so guard-
    // cell bleed (which used to make the guard read "near" and steal the pose)
    // can't fool us. Legacy path (fixedGuardCorner_ < 0) keeps weakest-blob.
    uint8_t weakest = 0;
    for (uint8_t k = 1; k < 4; k++) if (wsum[k] < wsum[weakest]) weakest = k;
    uint8_t miss = weakest;
    if (fixedGuardCorner_ >= 0 && fixedGuardCorner_ <= 3) miss = (uint8_t)fixedGuardCorner_;
    for (uint8_t k = 0; k < 4; k++) dbgCorner_[k] = (uint8_t)strength[k];
    dbgMiss_ = miss;
    dbgWeakest_ = weakest;
    // Guard-clarity (lock-commit gate, NOT a per-frame reject). Pinned: only
    // require the guard not be a near-full anchor (catches a grossly mis-presented
    // tag); SECDED + clean-frame consistency carry correctness. Legacy: strict
    // "guard clearly emptiest".
    {
      float minAnchorW = 1e9f, maxAnchorW = 0.0f;
      for (uint8_t k = 0; k < 4; k++) if (k != miss) {
        if (wsum[k] < minAnchorW) minAnchorW = wsum[k];
        if (wsum[k] > maxAnchorW) maxAnchorW = wsum[k];
      }
      if (fixedGuardCorner_ >= 0)
        cvGuardClear_ = (maxAnchorW > 0.0f) && (wsum[miss] < 0.85f * maxAnchorW);
      else
        cvGuardClear_ = (minAnchorW > 0.0f) && (wsum[miss] * 2.0f < minAnchorW);
    }
    // Fixed orientation resolves the de-rotation directly from the guard corner.
    static const uint8_t kRotForMiss[4] = {2, 1, 3, 0};
    cvExpectedRot_ = (int)kRotForMiss[miss];
    uint8_t P[3]; uint8_t pc = 0;
    for (uint8_t k = 0; k < 4; k++) if (k != miss && wsum[k] > 0.0f && pc < 3) P[pc++] = k;
    if (pc < 3) {
      dbgCvReject_ = "anchors not found"; out.prompt = Prompt::Searching; return out;
    }
    // Overlay: mark near zones inside each present anchor's core+secondary rings
    // so the UI shows tight corner squares (no more mid-field strays).
    for (uint8_t pk = 0; pk < 3; pk++) {
      const uint8_t k = P[pk];
      for (uint8_t r = 0; r < 8; r++)
        for (uint8_t c = 0; c < 8; c++) {
          if (!nearGrid[r][c]) continue;
          const float dr = fabsf((float)r - cornR[k]);
          const float dc = fabsf((float)c - cornC[k]);
          if ((dr > dc ? dr : dc) <= 2.0f) out.anchorMask[r] |= (uint8_t)(1u << c);
        }
    }
    out.anchorsFound = true;
    const float zi0 = cI[P[0]], zj0 = cJ[P[0]];
    const float zi1 = cI[P[1]], zj1 = cJ[P[1]];
    const float zi2 = cI[P[2]], zj2 = cJ[P[2]];
    const float det = (zi1 - zi0) * (zj2 - zj0) - (zi2 - zi0) * (zj1 - zj0);
    if (fabsf(det) < 1e-3f) {
      dbgCvReject_ = "degenerate anchors"; out.prompt = Prompt::Searching; return out;
    }
    auto solveAff = [&](float f0, float f1, float f2, float &a, float &b, float &c) {
      a = ((f1 - f0) * (zj2 - zj0) - (f2 - f0) * (zj1 - zj0)) / det;
      b = ((zi1 - zi0) * (f2 - f0) - (zi2 - zi0) * (f1 - f0)) / det;
      c = f0 - a * zi0 - b * zj0;
    };
    float ua, ub, uc, va, vb, vc;
    solveAff(CU[P[0]], CU[P[1]], CU[P[2]], ua, ub, uc);   // u (col axis)
    solveAff(CV[P[0]], CV[P[1]], CV[P[2]], va, vb, vc);   // v (row axis)
    for (uint8_t r = 0; r < 8; r++) {
      for (uint8_t c = 0; c < 8; c++) {
        if (!valid8inlier[r][c] || ambGrid[r][c]) continue;
        const float zi = (float)r + 0.5f, zj = (float)c + 0.5f;
        const float u = ua * zi + ub * zj + uc;
        const float v = va * zi + vb * zj + vc;
        if (u < 0.0f || u >= 1.0f || v < 0.0f || v >= 1.0f) continue;
        uint8_t cc2 = (uint8_t)(u * 4.0f); if (cc2 > 3) cc2 = 3;
        uint8_t cr2 = (uint8_t)(v * 4.0f); if (cr2 > 3) cr2 = 3;
        const uint8_t i = (uint8_t)(cr2 * 4 + cc2);
        dbgCvCellTotal_[i]++;
        if (nearGrid[r][c]) dbgCvCellNear_[i]++;
      }
    }
  }

  // 8a. Push current frame's per-cell votes into the sliding-window history.
  for (uint8_t i = 0; i < 16; i++) {
    cvCellNearHist_[cvHistWrite_][i] = dbgCvCellNear_[i];
    cvCellTotalHist_[cvHistWrite_][i] = dbgCvCellTotal_[i];
  }
  cvHistWrite_ = (cvHistWrite_ + 1) % kCvVoteHistory;
  if (cvHistCount_ < kCvVoteHistory) cvHistCount_++;

  // 8b. Per-cell majority vote across the WINDOWED HISTORY → bits.
  //     Single-frame jitter on borderline cells averages out; consistent-
  //     correct cells dominate the sum and classify stably.
  uint8_t bits16[16];
  uint8_t cellConfW[16];          // DC34-154: per-cell windowed vote confidence 0..255
  uint8_t minConf = 255;          // weakest WELL-SAMPLED cell's confidence (the lock gate)
  uint8_t nJudged = 0;            // cells with enough votes to judge confidence
  uint8_t nFilled = 0;
  uint8_t nearCells = 0;
  for (uint8_t i = 0; i < 16; i++) {
    uint16_t nearSum = 0;
    uint16_t totalSum = 0;
    for (uint8_t k = 0; k < cvHistCount_; k++) {
      nearSum  += cvCellNearHist_[k][i];
      totalSum += cvCellTotalHist_[k][i];
    }
    if (totalSum == 0) {
      bits16[i] = 0;
      cellConfW[i] = 0;           // unfilled cell -> no signal (CRC handles the guess)
      continue;
    }
    nFilled++;
    // How one-sided was the windowed vote? 0 = 50/50 (ambiguous), 255 = unanimous.
    {
      const uint16_t maj = (nearSum * 2 > totalSum) ? nearSum : (uint16_t)(totalSum - nearSum);
      uint16_t conf = (uint16_t)((uint32_t)(maj - totalSum / 2) * 255U / (totalSum - totalSum / 2));
      if (conf > 255) conf = 255;
      cellConfW[i] = (uint8_t)conf;
      // Only well-sampled cells constrain the lock confidence (a 1-2 vote cell
      // is too sparse to judge; CRC covers its guessed bit).
      if (totalSum >= kCellMinVotes) {
        if (conf < minConf) minConf = conf;
        nJudged++;
      }
    }
    // Strict majority across history. Sparse cells (e.g. orient corner with
    // only 1 zone/frame) get classified by whatever their lone zone reads;
    // history smooths single-frame noise, and the orient marker's spatial
    // structure validates the classification at decode time.
    const bool isNear = (nearSum * 2 > totalSum);
    bits16[i] = (oneIsNear ? (isNear ? 1 : 0) : (isNear ? 0 : 1));
    if (isNear) nearCells++;
    // Also fill the dbgDepth16_/Valid_ slots so dumpDebug shows something.
    dbgDepth16_[i] = (uint16_t)(isNear ? nearMm : farMm);
    dbgDepth16Valid_[i] = true;
    dbgBits16_[i] = bits16[i];
  }

  if (nFilled < 12) {
    dbgCvReject_ = "fewer than 12 cells filled";
    out.prompt = Prompt::Searching;
    return out;
  }

  // Density gate: need at least kCvMinNearDensityPct/100 of cells NEAR.
  // (catches cases where the tag isn't actually present and noise dominates.)
  if ((uint16_t)nearCells * 100U < (uint16_t)kCvMinNearDensityPct * (uint16_t)nFilled) {
    dbgCvReject_ = "near-cell density too low";
    out.prompt = Prompt::Searching;
    return out;
  }

  // 9. Decode. Anchor spec (DC34-155) uses its own layout decode; legacy uses
  //    the orientation-marker rotation+mirror fallback.
  bool usedMirror = false;
  HRCode4x4::DecodeResult decoded = useAnchor_
      ? decodeAnchorBits(bits16, cvExpectedRot_)
      : decodeWithMirrorFallback(bits16, usedMirror);
  dbgDecode_ = decoded;
  dbgUsedMirror_ = usedMirror;

  // 10. Lock state machinery (mirrors the legacy pipeline's logic).
  // Whitelist (optional): drop CRC-valid IDs that don't pass the host
  // validator before the tracker/lock counters move.
  const bool whitelistOk =
      (decoded.status == HRCode4x4::DecodeStatus::Ok) &&
      (!idValidator_ || idValidator_((int)decoded.id));

  // DC34-154: the read must be CONFIDENT (the weakest filled cell's windowed
  // vote is strongly one-sided) to advance the progress bar OR lock. Fragile/
  // marginal reads (the "looks good but decodes wrong" class) get AdjustWeak
  // and never accumulate -> no confident-wrong unlock, and the bar reflects a
  // trustworthy read instead of mere coverage.
  //
  // DC34-155 anchor mode: SECDED(12,7) already gives a distance-4 guarantee
  // (correct any 1-cell flip, REJECT any 2-cell error), which is a stronger
  // trust signal than the windowed-confidence heuristic -- and that heuristic
  // would veto the very boundary-straddling cell ECC exists to fix (its vote is
  // ~50/50, so minConf never clears the floor and the tag can decode Ok forever
  // without ever locking). Rely on the ECC + the 12-consecutive-same-id lock
  // requirement (a flickering wrong read changes id and resets lockRun_).
  const bool confident = useAnchor_
      ? (decoded.status == HRCode4x4::DecodeStatus::Ok)
      : ((nJudged >= kCellMinJudged) && (minConf >= kCellConfidenceFloor));

  const bool ok =
      whitelistOk &&
      (nFilled >= kMinValidCells) &&
      confident;

  if (whitelistOk && confident) {
    if ((int)decoded.id == trackCandidateId_) {
      if (trackRun_ < 255) trackRun_++;
    } else {
      trackCandidateId_ = (int)decoded.id;
      trackRun_ = 1;
    }
  } else {
    trackCandidateId_ = -1;
    trackRun_ = 0;
  }

  // DC34-155 anchor lock policy. Any consistent-id Ok read advances the lock:
  // a real tag can read *corrected* (syn!=0) EVERY frame when one cell is
  // consistently misread at a given pose (seen on hardware), and SECDED validly
  // recovers the id -- so requiring a *clean* (syn==0) read to advance would
  // stall the lock and defeat the whole point of the ECC. Wrong locks are
  // instead prevented upstream by decodeAnchorBits picking the CORRECT rotation
  // (guard-far/clean preference) so garbage-rotation ids don't get read in the
  // first place, plus the 12-consecutive-SAME-id requirement. A non-ok frame
  // DECAYS the run (tolerance for an isolated blip) rather than zeroing it.
  // Legacy mode unchanged (advance on ok, hard reset otherwise).
  // Gentle decay (1): a marginal-but-correct read (one cell consistently
  // misread -> syn=1 every frame, so any extra flicker is a double-error =
  // rejected frame) still climbs to a lock despite occasional non-ok frames.
  // A wrong id doesn't benefit -- an id CHANGE resets the run to 1 in the ok
  // branch, and decay only nibbles the id-less non-ok frames.
  static constexpr uint8_t kLockRunDecay = 1;
  // DC34-155 TILT GATE (anchor mode): only advance the lock when the tag reads
  // reasonably FLAT-ON. Steep tilt makes the 3-anchor affine mis-sample cells
  // -> an UNSTABLE read (id-16 flips to 5/96 hand-held). Measured on hardware:
  // flat = ~0.07 mm/zone (stable, correct); wrong CLEAN reads (id-5) floored at
  // ~0.71 mm/zone. 0.5 sits below that floor and well above flat, so locking is
  // confined to the reliable near-flat regime (the surface case that is 100%).
  // A tilted frame is treated like a non-ok frame (decays the run); the level
  // bubble already nudges the user to flatten.
  static constexpr float kMaxTiltMmPerZone = 0.5f;
  // CLEAN-EVIDENCE requirement (anchor mode, the primary wrong-lock guard):
  // measured on hardware, a marginal-alignment WRONG read is CORRECTED-only
  // (id-72 read syn=6 every frame -- a skewed sampling only lands on a valid
  // codeword AFTER SECDED bends a cell), whereas a correctly-aligned tag produces
  // occasional EXACT-codeword frames (id-16 syn=0). So the run climbs on any
  // consistent-id read (fast, no stall on a consistently-corrected good cell) but
  // the lock only COMMITS once >= kAnchorMinCleanFrames clean frames of that id
  // have been seen. Corrected-only garbage never accrues clean evidence -> can't
  // lock; a real tag does. Biased to correctness: a marginal-but-correct read that
  // never reads clean won't lock (re-scan / reposition) rather than risk a wrong
  // unlock.
  static constexpr uint8_t kAnchorMinCleanFrames = 2;
  // DISTANCE gate (anchor): refuse to lock when the tag is held too CLOSE. Below
  // ~kMinLockDistMm the 4x4 fills the sensor FoV, the recessed moat (the far
  // reference) drops out of view, and the near/far threshold + affine degrade ->
  // a clean but WRONG codeword (measured: id-88 -> 61/54/77 at ~58mm; reliable at
  // the ~76-86mm sweet spot). The overlay's sweet-spot ring already guides the
  // user to pull back; this just stops a too-close read from committing a lock.
  static constexpr int16_t kMinLockDistMm = 70;
  const bool distOk = (!useAnchor_ || nearMm >= kMinLockDistMm);
  const bool flatEnough = (!useAnchor_ || dbgCvTiltMmPerZone_ <= kMaxTiltMmPerZone);
  const bool cleanFrame = (decoded.crc_read == 0);   // anchor: SECDED syndrome 0
  if (useAnchor_ && voteLockEnabled_) {
    // === DC34-156 window-plurality-with-margin lock (mirrors vote_lock_model.py) ===
    // A frame votes only if it passed the decode + pose gates. Clean frames weigh
    // more than corrected, biasing toward exact codewords WITHOUT hard-gating on
    // them (the hard 2-clean gate was the timeout cause).
    if (ok && flatEnough && distOk) {
      badFrames_ = 0;
      const uint8_t w  = cleanFrame ? kVoteCleanWeight : kVoteCorrWeight;
      const uint8_t id = (uint8_t)(decoded.id & 0x7F);
      const uint8_t oldW = voteRingW_[voteRingPos_];      // age out overwritten slot
      if (oldW) {
        const uint8_t oldId = voteRingId_[voteRingPos_];
        voteVotes_[oldId] = (voteVotes_[oldId] >= oldW) ? (uint8_t)(voteVotes_[oldId] - oldW) : 0;
        if (oldW == kVoteCleanWeight && voteClean_[oldId]) voteClean_[oldId]--;
      }
      voteRingId_[voteRingPos_] = id;                     // record this frame
      voteRingW_[voteRingPos_]  = w;
      voteRingPos_ = (uint8_t)((voteRingPos_ + 1) % kVoteWindow);
      voteVotes_[id] = (voteVotes_[id] <= (uint8_t)(255 - w)) ? (uint8_t)(voteVotes_[id] + w) : 255;
      if (cleanFrame && voteClean_[id] < 255) voteClean_[id]++;   // telemetry only (not gated)
    } else {
      if (badFrames_ < 255) badFrames_++;
      // A brief pose blip does NOT age the window (the ring ages only on new votes),
      // so accumulated correct evidence survives a one-frame wobble. A committed
      // lock is STICKY (only clearLockInternal() on a new scan releases it) -- no
      // badFrames clear here, matching vote_lock_model.py's latched result.
    }
    // Top-2 vote-getters over the window.
    uint8_t topId = 0, topV = 0, secondV = 0;
    for (int i = 0; i < 128; i++) {
      const uint8_t v = voteVotes_[i];
      if (v > topV) { secondV = topV; topV = v; topId = (uint8_t)i; }
      else if (v > secondV) { secondV = v; }
    }
    const bool haveEvidence = (topV >= kVoteEvidenceFloor);
    const bool haveMargin   = ((uint16_t)topV * 10U >= (uint16_t)kVoteMarginX10 * (uint16_t)secondV);
    const bool guardOk      = cvGuardClear_;
    // NO clean-frame gate: clean-ness is captured in the vote WEIGHT (clean=+2),
    // never as a commit requirement -- a corrected-only-but-correct tag (never
    // reads syn==0, the common case at ~2 zones/cell) must still lock. Margin +
    // pose gates + whitelist are the safety. voteClean_ is telemetry only.
    // STICKY: commit only while still unlocked (lockedId_ < 0). Once set, the
    // winner is held until clearLockInternal() (a new scan) -- a post-lock
    // plurality shift can never overwrite it or fire a second, different unlock
    // (matches vote_lock_model.py; removes reliance on the caller's stopOnLock).
    if (lockedId_ < 0 && haveEvidence && haveMargin && guardOk && ok && flatEnough && distOk) {
      lockedId_ = topId;
    }
    // Surface the winner in the legacy fields so the progress bar + dumpDebug read it.
    lockCandidateId_ = (topV ? (int)topId : -1);   // -1 "no candidate" when window empty
    lockRun_ = topV;               // reused as "evidence accumulated" for the bar
    lockCleanCount_ = voteClean_[topId];
  } else {
    // === Legacy consecutive-run + clean-gate lock (unchanged; used when
    // voteLockEnabled_ is false OR in non-anchor mode) ===
    if (ok && flatEnough && distOk) {
      badFrames_ = 0;
      if ((int)decoded.id == lockCandidateId_) {
        if (lockRun_ < 255) lockRun_++;
        if (cleanFrame && lockCleanCount_ < 255) lockCleanCount_++;
      } else {
        lockCandidateId_ = (int)decoded.id;
        lockRun_ = 1;
        lockCleanCount_ = cleanFrame ? 1 : 0;
      }
      const bool cleanOk = (!useAnchor_ || lockCleanCount_ >= kAnchorMinCleanFrames);
      const bool guardOk = (!useAnchor_ || cvGuardClear_);
      if (lockRun_ >= lockRequired_ && cleanOk && guardOk) {
        lockedId_ = (int)decoded.id;
      }
    } else {
      if (useAnchor_) {
        if (lockRun_ > kLockRunDecay) {
          lockRun_ = (uint8_t)(lockRun_ - kLockRunDecay);
        } else {
          lockRun_ = 0;
          lockCandidateId_ = -1;
          lockCleanCount_ = 0;
        }
      } else {
        lockCandidateId_ = -1;
        lockRun_ = 0;
      }
      if (badFrames_ < 255) badFrames_++;
      if (badFrames_ >= kLockClearBadFrames) lockedId_ = -1;
    }
  }

  out.decode = decoded;
  out.ok = ok;
  out.lockedId = lockedId_;
  out.run = lockRun_;
  out.runRequired = lockRequired_;
  if (lockedId_ >= 0) {
    out.progress = 100;
  } else if (lockRequired_ > 0) {
    // Progress reflects REAL lock-readiness.
    if (useAnchor_ && voteLockEnabled_) {
      // Vote mode: scale the winner's evidence (lockRun_ = topV) to the floor,
      // and HOLD below full until an actual commit. A commit sets lockedId_>=0,
      // which the branch ABOVE renders as 100 the same frame -- so a not-yet-
      // committed read (evidence high but margin unmet) pauses at <=90 and
      // triggers the Prompt::Locking "hold steady" state instead of showing a
      // full bar that then resets. No margin recompute needed here.
      uint32_t p = (100UL * (uint32_t)lockRun_) / (uint32_t)kVoteEvidenceFloor;  // lockRun_ = topV
      if (p > 90) p = 90;
      out.progress = (uint8_t)p;
    } else {
      // Legacy anchor / non-anchor bar (unchanged): use lockRun_ (anchor) or
      // trackRun_, cap below completion until >= kAnchorMinCleanFrames clean.
      const uint32_t counter = useAnchor_ ? lockRun_ : trackRun_;
      uint8_t p = (uint8_t)((100UL * counter) / lockRequired_);
      if (p > 100) p = 100;
      if (useAnchor_ && lockCleanCount_ < kAnchorMinCleanFrames && p > 80) p = 80;
      out.progress = p;
    }
  } else {
    out.progress = 0;
  }
  // "Locking - hold steady": bar full/near-full but not yet committed (still
  // accruing clean-frame evidence). Stops a paused-full bar reading as
  // "finished, then failed" on a marginal tag. Only overrides the tracking
  // prompts, never the movement nudges (Move closer / Back up / Square up).
  if (lockedId_ < 0 && useAnchor_ && out.progress >= 70 &&
      (out.prompt == Prompt::HoldSteady || out.prompt == Prompt::AdjustWeak)) {
    out.prompt = Prompt::Locking;
  }
  out.validCells = nFilled;
  out.separationMm = sepMm;
  out.threshold = (int)lrintf(threshold);
  out.residualMode = false;
  out.usedMirror = usedMirror;
  out.roiX = bMinC;
  out.roiY = bMinR;
  out.roiSize = (uint8_t)((bboxH > bboxW) ? bboxH : bboxW);
  // Prompt: a real, ACTIONABLE "good/bad" signal. We're past the density/coverage
  // gates here, so there IS tag-like structure -> never "Searching". In anchor
  // mode, turn the not-locking cases into concrete guidance the user can act on:
  // too CLOSE -> "Back up" (MoveFarther), too FAR -> "Move closer" (MoveCloser),
  // otherwise present-but-not-locking -> "Hold flat & steady" (AdjustWeak).
  static constexpr int16_t kMaxGoodLockDistMm = 105;
  if (lockedId_ >= 0) {
    out.prompt = Prompt::Locked;
  } else if (ok) {
    out.prompt = Prompt::HoldSteady;
  } else if (useAnchor_ && nearMm > 0 && nearMm < kMinLockDistMm) {
    out.prompt = Prompt::MoveFarther;   // too close -> the close-range misread zone
  } else if (useAnchor_ && nearMm > kMaxGoodLockDistMm) {
    out.prompt = Prompt::MoveCloser;    // too far -> weak/undersampled read
  } else {
    out.prompt = Prompt::AdjustWeak;    // right distance but tilted/marginal -> hold flat & steady
  }

  // Per-cell feedback for the overlay: the WINDOWED vote confidence (matches the
  // lock gate), so the UI colours track exactly what gates the lock.
  for (uint8_t i = 0; i < 16; i++) {
    out.cells[i].valid = (dbgCvCellTotal_[i] > 0);
    out.cells[i].bit = bits16[i];
    out.cells[i].confidence = cellConfW[i];
  }

  // Save sep into the legacy debug fields too so dumpDebug shows it consistently.
  dbgSepMm_ = sepMm;
  dbgThreshold_ = (int)lrintf(threshold);

  return out;
}

} // namespace HRScan
