#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <SparkFun_VL53L5CX_Library.h>

#include <HRScanEngine.h>
#include <HRScanOverlayLVGL.h>

namespace HRScan {

enum class LockAction : uint8_t {
  Continue = 0,
  StopScanner,
  ClearLock,
};

using LockCallback = LockAction (*)(const Result &result, void *user);

struct Config {
  int i2cSda = 11;
  int i2cScl = 10;
  uint32_t i2cClockHz = 400000;
  uint8_t startupAttempts = 3;
  uint8_t sensorFps = 15;
  bool oneIsNear = true;
  bool stopOnLock = false;
  uint32_t scanTimeoutMs = 0;
  bool stopOnTimeout = false;
  int expectedId = 128;
  Profile profile = Profile::Strict15mm;

  OverlayConfig overlay;
};

class Scanner {
public:
  bool begin(const Config &cfg);
  void end();
  void tick();

  bool setEnabled(bool enable);
  bool isEnabled() const { return enabled_; }

  void setLockCallback(LockCallback cb, void *user = nullptr) {
    lockCb_ = cb;
    lockCbUser_ = user;
  }

  const Result &result() const { return lastResult_; }
  bool hasResult() const { return hasResult_; }

  // Pass-through to Engine::dumpDebug — prints last frame's intermediates.
  void dumpDebug(Print &out) const { engine_.dumpDebug(out); }

  // Pass-through CV mode toggles.
  void setUseCV(bool enabled) { engine_.setUseCV(enabled); }
  bool useCV() const { return engine_.useCV(); }

  // DC34-155: anchor/fiducial decode (HR spec v2). Off by default.
  void setUseAnchor(bool enabled) { engine_.setUseAnchor(enabled); }
  bool useAnchor() const { return engine_.useAnchor(); }
  void setVoteLock(bool enabled) { engine_.setVoteLock(enabled); }
  bool voteLock() const { return engine_.voteLock(); }
  void setFixedGuardCorner(int corner) { engine_.setFixedGuardCorner(corner); }
  int  fixedGuardCorner() const { return engine_.fixedGuardCorner(); }
  void tiltGradient(float &gr, float &gc) const { engine_.tiltGradient(gr, gc); }
  void setZoneCal(const int16_t cal[64]) { engine_.setZoneCal(cal); }
  void clearZoneCal() { engine_.clearZoneCal(); }
  bool hasZoneCal() const { return engine_.hasZoneCal(); }
  void beginCalCapture() { engine_.beginCalCapture(); }
  int  finishCalCapture(int16_t out[64]) { return engine_.finishCalCapture(out); }
  bool isCalCapturing() const { return engine_.isCalCapturing(); }
  int  lastCalZones() const { return engine_.lastCalZones(); }
  int  lastCalAvgMm() const { return engine_.lastCalAvgMm(); }

  // Pass-through ID screening hook (engine-level fail-fast on bad IDs).
  void setIdValidator(Engine::IdValidator fn) { engine_.setIdValidator(fn); }

  // Reset the scan-timeout clock. Useful for hosts that want to extend
  // the deadline while the engine is making visible progress (e.g. a
  // candidate is tracking via 'Hold Steady' but hasn't locked yet) so
  // the user doesn't time out mid-scan despite doing the right thing.
  void resetTimeout() {
    enabledSinceMs_ = millis();
    timedOut_ = false;
  }

  // Pass-through to Engine's raw 8x8 view (most recent frame).
  const int16_t (*lastFilteredMm() const)[8] { return engine_.lastFilteredMm(); }
  const bool    (*lastFilteredValid() const)[8] { return engine_.lastFilteredValid(); }

  // Feed the overlay's heatmap with the most recent sensor frame. Should be
  // called once per loop after tick(); the overlay redraws on next update().
  void feedOverlaySensor() {
    overlay_.setSensorData(engine_.lastFilteredMm(), engine_.lastFilteredValid());
  }

  // Feed the overlay's level bubble. Pass IMU roll/pitch in degrees. If you
  // don't have an IMU, pass (0, 0) and the bubble will display "level".
  void feedOverlayLevel(float rollDeg, float pitchDeg) {
    overlay_.setLevel(rollDeg, pitchDeg);
  }

  // Feed mock VL53L5CX data for testing (bypasses real sensor I2C reads).
  // Processes one frame through the HR engine as if the sensor produced it.
  void feedMockData(const VL53L5CX_ResultsData &data);

  // Access the I2C bus the scanner owns. Useful for sharing the bus with
  // an IMU or other peripherals on the same SDA/SCL pair (e.g. QMI8658
  // at 0x6B alongside the VL53L5CX at 0x29). Only valid after begin().
  TwoWire &wire() { return wire_; }

private:
  void initDriverPointersIfNeeded();
  void freeDriverAllocations();
  bool initSensor();

  Config cfg_{};
  SparkFun_VL53L5CX imager_;
  VL53L5CX_ResultsData measurementData_;
  TwoWire wire_ = TwoWire(1);
  Engine engine_;
  OverlayLVGL overlay_;

  bool began_ = false;
  bool enabled_ = false;
  bool hasResult_ = false;
  bool driverPointersInitialized_ = false;
  bool timedOut_ = false;
  uint32_t enabledSinceMs_ = 0;
  int prevLockedId_ = -1;
  Result lastResult_{};

  LockCallback lockCb_ = nullptr;
  void *lockCbUser_ = nullptr;
};

} // namespace HRScan
