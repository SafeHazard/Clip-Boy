// Minimal QMI8658 driver — accelerometer-only, just enough to compute
// roll/pitch for the level bubble. Avoids pulling in SensorLib so the
// HRScanGuidance example stays self-contained.
//
// Wire convention: caller provides a TwoWire & already begun() with the
// correct SDA/SCL pins. The IMU is at 0x6B on the Waveshare ESP32-S3 2.8
// board (SA0 tied high). Coexists with the VL53L5CX at 0x29.

#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <math.h>

namespace HRScanIMU {

static constexpr uint8_t kAddr = 0x6B;

// Register map (subset).
enum : uint8_t {
  REG_WHO_AM_I = 0x00,
  REG_CTRL1    = 0x02,
  REG_CTRL2    = 0x03,
  REG_CTRL7    = 0x08,
  REG_AX_L     = 0x35,
};

class QMI8658 {
public:
  bool begin(TwoWire &wire) {
    wire_ = &wire;

    uint8_t who = 0;
    if (!read(REG_WHO_AM_I, &who, 1) || who != 0x05) {
      return false;
    }

    // CTRL1: bit6 ADDR_AI=1 (auto-increment burst), little-endian (BE=0).
    if (!writeReg(REG_CTRL1, 0x40)) return false;

    // CTRL2: aFS=4G (001), aODR=250Hz (0101) → 0x15.
    if (!writeReg(REG_CTRL2, 0x15)) return false;

    // CTRL7: enable accel only (bit0=1).
    if (!writeReg(REG_CTRL7, 0x01)) return false;

    delay(20);
    return true;
  }

  // Power down accel/gyro. Keeps the I2C link intact but stops sensor
  // sampling so a host that's not currently scanning doesn't burn IMU
  // current. Pair with begin() on next scan start.
  void end() {
    if (wire_) writeReg(REG_CTRL7, 0x00);
  }

  // Read accel as g-units. Returns false on I2C error.
  bool readAccelG(float &ax, float &ay, float &az) {
    uint8_t b[6];
    if (!read(REG_AX_L, b, 6)) return false;
    int16_t rx = (int16_t)((uint16_t)b[1] << 8 | b[0]);
    int16_t ry = (int16_t)((uint16_t)b[3] << 8 | b[2]);
    int16_t rz = (int16_t)((uint16_t)b[5] << 8 | b[4]);
    constexpr float kLsbPerG = 8192.0f;   // 4G full-scale on int16 → 32768/4
    ax = rx / kLsbPerG;
    ay = ry / kLsbPerG;
    az = rz / kLsbPerG;
    return true;
  }

  // Compute roll/pitch in degrees from a fresh accel sample.
  // Decoupled formulation: roll = atan2(ax, az), pitch = atan2(ay, az).
  // Each axis depends on its own gravity component plus Z, so neither
  // collapses to zero when the other moves -- avoids the "pitch dead"
  // failure mode of the cross-coupled atan2(-ax, sqrt(ay+az)) form.
  // The badge↔chip axis convention is determined empirically; if pitch
  // and roll are swapped on this board, swap names at the call site.
  // Mounting bias is removed by captureLevelOffset().
  bool readLevel(float &rollDeg, float &pitchDeg) {
    float ax, ay, az;
    if (!readAccelG(ax, ay, az)) return false;
    lastAx_ = ax; lastAy_ = ay; lastAz_ = az;
    // Waveshare ESP32-S3 2.8: chip Z axis points INTO the badge face, so
    // face-up at rest reads az ≈ -1g. Negate Z so atan2 doesn't sit on
    // the ±pi wraparound at level. Chip X axis points opposite the
    // badge's "right" (right tilt → ax negative), so negate ax to match
    // the overlay's "roll → bubble +X" convention.
    rollDeg  = atan2f(-ax, -az) * 57.2957795f - rollOffsetDeg_;
    pitchDeg = atan2f(ay,  -az) * 57.2957795f - pitchOffsetDeg_;
    return true;
  }

  // Read raw accel + the tare-offsets (for diagnostic dump).
  void lastSample(float &ax, float &ay, float &az,
                  float &rollOffDeg, float &pitchOffDeg) const {
    ax = lastAx_; ay = lastAy_; az = lastAz_;
    rollOffDeg = rollOffsetDeg_; pitchOffDeg = pitchOffsetDeg_;
  }

  // Capture the current orientation as "level". Average a few samples to
  // squash sensor noise. Call when the badge is set on a flat surface.
  bool captureLevelOffset(uint8_t samples = 16) {
    float sumR = 0, sumP = 0;
    uint8_t got = 0;
    rollOffsetDeg_ = 0;
    pitchOffsetDeg_ = 0;
    for (uint8_t i = 0; i < samples; ++i) {
      float r, p;
      if (readLevel(r, p)) { sumR += r; sumP += p; got++; }
      delay(10);
    }
    if (!got) return false;
    rollOffsetDeg_  = sumR / got;
    pitchOffsetDeg_ = sumP / got;
    return true;
  }

private:
  TwoWire *wire_ = nullptr;
  float rollOffsetDeg_ = 0.0f;
  float pitchOffsetDeg_ = 0.0f;
  float lastAx_ = 0, lastAy_ = 0, lastAz_ = 0;

  bool writeReg(uint8_t reg, uint8_t val) {
    wire_->beginTransmission(kAddr);
    wire_->write(reg);
    wire_->write(val);
    return wire_->endTransmission() == 0;
  }

  bool read(uint8_t reg, uint8_t *dst, size_t n) {
    wire_->beginTransmission(kAddr);
    wire_->write(reg);
    if (wire_->endTransmission(false) != 0) return false;
    size_t got = wire_->requestFrom((int)kAddr, (int)n);
    if (got != n) return false;
    for (size_t i = 0; i < n; ++i) dst[i] = wire_->read();
    return true;
  }
};

}  // namespace HRScanIMU
