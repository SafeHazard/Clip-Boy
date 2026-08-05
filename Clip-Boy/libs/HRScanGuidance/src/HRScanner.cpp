#include "HRScanner.h"

namespace HRScan {

void Scanner::initDriverPointersIfNeeded() {
  if (driverPointersInitialized_) return;
  imager_.VL53L5CX_i2c = nullptr;
  imager_.Dev = nullptr;
  driverPointersInitialized_ = true;
}

void Scanner::freeDriverAllocations() {
  initDriverPointersIfNeeded();
  if (imager_.Dev) {
    delete imager_.Dev;
    imager_.Dev = nullptr;
  }
  if (imager_.VL53L5CX_i2c) {
    delete imager_.VL53L5CX_i2c;
    imager_.VL53L5CX_i2c = nullptr;
  }
}

bool Scanner::initSensor() {
  initDriverPointersIfNeeded();
  wire_.begin(cfg_.i2cSda, cfg_.i2cScl, cfg_.i2cClockHz);
  delay(50);

  bool found = false;
  const uint8_t attempts = (cfg_.startupAttempts == 0) ? 1 : cfg_.startupAttempts;
  for (uint8_t attempt = 0; attempt < attempts && !found; attempt++) {
    if (attempt > 0) delay(200);
    freeDriverAllocations();
    found = imager_.begin((DEFAULT_I2C_ADDR >> 1), wire_);
  }
  if (!found) {
    freeDriverAllocations();
    wire_.end();
    return false;
  }

  (void)imager_.setRangingFrequency(cfg_.sensorFps);
  if (!imager_.setResolution(8 * 8)) {
    freeDriverAllocations();
    wire_.end();
    return false;
  }
  if (!imager_.startRanging()) {
    freeDriverAllocations();
    wire_.end();
    return false;
  }
  return true;
}

bool Scanner::begin(const Config &cfg) {
  end();
  cfg_ = cfg;

  if (!overlay_.begin(cfg_.overlay)) {
    end();
    return false;
  }
  if (!initSensor()) {
    end();
    return false;
  }

  engine_.begin(cfg_.profile);
  engine_.setExpectedId(cfg_.expectedId);

  began_ = true;
  enabled_ = true;
  hasResult_ = false;
  timedOut_ = false;
  enabledSinceMs_ = millis();
  prevLockedId_ = -1;
  return true;
}

void Scanner::end() {
  if (enabled_) {
    (void)setEnabled(false);
  }
  wire_.end();
  freeDriverAllocations();
  overlay_.end();
  began_ = false;
  enabled_ = false;
  hasResult_ = false;
  timedOut_ = false;
  enabledSinceMs_ = 0;
  prevLockedId_ = -1;
}

bool Scanner::setEnabled(bool enable) {
  if (!began_) return false;
  if (enable == enabled_) return true;

  if (!enable) {
    imager_.stopRanging();
    (void)imager_.setPowerMode(SF_VL53L5CX_POWER_MODE::SLEEP);
    enabled_ = false;
    timedOut_ = false;
    enabledSinceMs_ = 0;
    engine_.clearLock();
    prevLockedId_ = -1;
    return true;
  }

  (void)imager_.setPowerMode(SF_VL53L5CX_POWER_MODE::WAKEUP);
  if (!imager_.setResolution(8 * 8)) return false;
  (void)imager_.setRangingFrequency(cfg_.sensorFps);
  if (!imager_.startRanging()) return false;

  enabled_ = true;
  timedOut_ = false;
  enabledSinceMs_ = millis();
  engine_.clearLock();
  prevLockedId_ = -1;
  return true;
}

void Scanner::tick() {
  if (!began_ || !enabled_) return;
  const uint32_t now = millis();

  if (cfg_.scanTimeoutMs > 0 && !timedOut_) {
    const uint32_t elapsed = now - enabledSinceMs_;
    if (elapsed >= cfg_.scanTimeoutMs) {
      timedOut_ = true;
      if (!hasResult_) {
        lastResult_ = Result{};
        lastResult_.decode.status = HRCode4x4::DecodeStatus::InvalidInput;
        lastResult_.lockedId = -1;
      }
      lastResult_.timedOut = true;
      lastResult_.scanElapsedMs = elapsed;
      if (lastResult_.lockedId < 0) {
        lastResult_.prompt = Prompt::TimedOut;
      }
      hasResult_ = true;
      overlay_.update(lastResult_);
      if (cfg_.stopOnTimeout) {
        (void)setEnabled(false);
        return;
      }
    }
  }

  if (!imager_.isDataReady()) return;
  if (!imager_.getRangingData(&measurementData_)) return;

  lastResult_ = engine_.processFrame(
      measurementData_.distance_mm,
      measurementData_.nb_target_detected,
      measurementData_.target_status,
      cfg_.oneIsNear);
  lastResult_.scanElapsedMs = now - enabledSinceMs_;
  lastResult_.timedOut = timedOut_;
  if (timedOut_ && lastResult_.lockedId < 0) {
    lastResult_.prompt = Prompt::TimedOut;
  }
  hasResult_ = true;

  overlay_.update(lastResult_);

  const int curLock = lastResult_.lockedId;
  if (curLock >= 0 && curLock != prevLockedId_) {
    LockAction action = LockAction::Continue;
    if (lockCb_) action = lockCb_(lastResult_, lockCbUser_);

    // Precedence: the callback's ClearLock wins over stopOnLock. Without
    // this, stopOnLock=true disables the scanner before a tolerant
    // whitelist callback can veto a noise-induced false-lock.
    if (action == LockAction::ClearLock) {
      engine_.clearLock();
      prevLockedId_ = -1;
      return;
    }
    if (cfg_.stopOnLock || action == LockAction::StopScanner) {
      (void)setEnabled(false);
    }
  }

  prevLockedId_ = curLock;
}

void Scanner::feedMockData(const VL53L5CX_ResultsData &data) {
  if (!enabled_) return;

  uint32_t now = millis();

  // Timeout check (same as tick)
  if (cfg_.scanTimeoutMs > 0 && !timedOut_) {
    uint32_t elapsed = now - enabledSinceMs_;
    if (elapsed >= cfg_.scanTimeoutMs) {
      timedOut_ = true;
      lastResult_.timedOut = true;
      lastResult_.prompt = Prompt::TimedOut;
      hasResult_ = true;
      overlay_.update(lastResult_);
      if (cfg_.stopOnTimeout) {
        (void)setEnabled(false);
        return;
      }
    }
  }

  // Process frame through engine (same as tick lines 154-182)
  lastResult_ = engine_.processFrame(
      data.distance_mm,
      data.nb_target_detected,
      data.target_status,
      cfg_.oneIsNear);
  lastResult_.scanElapsedMs = now - enabledSinceMs_;
  lastResult_.timedOut = timedOut_;
  if (timedOut_ && lastResult_.lockedId < 0) {
    lastResult_.prompt = Prompt::TimedOut;
  }
  hasResult_ = true;

  overlay_.update(lastResult_);

  const int curLock = lastResult_.lockedId;
  if (curLock >= 0 && curLock != prevLockedId_) {
    LockAction action = LockAction::Continue;
    if (lockCb_) action = lockCb_(lastResult_, lockCbUser_);

    // Precedence: the callback's ClearLock wins over stopOnLock. Without
    // this, stopOnLock=true disables the scanner before a tolerant
    // whitelist callback can veto a noise-induced false-lock.
    if (action == LockAction::ClearLock) {
      engine_.clearLock();
      prevLockedId_ = -1;
      return;
    }
    if (cfg_.stopOnLock || action == LockAction::StopScanner) {
      (void)setEnabled(false);
    }
  }

  prevLockedId_ = curLock;
}

} // namespace HRScan
