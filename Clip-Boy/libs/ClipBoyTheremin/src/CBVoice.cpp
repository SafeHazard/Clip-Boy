#include "CBVoice.h"
#include "CBWaveforms.h"

#include <cmath>

namespace ClipTheremin {

Voice::Voice() {}

LoadError Voice::load(const VoiceConfig &cfg, size_t maxCompressedBytes, size_t maxDecodedFrames) {
  clear();

  type_ = cfg.type;
  maxVolume_ = cfg.maxVolume;
  minFreqHz_ = cfg.minFreqHz;
  maxFreqHz_ = cfg.maxFreqHz;
  phase_ = 0.0f;
  samplePhase_ = 0.0f;
  currentFreq_ = 0.0f;
  currentVolume_ = 0.0f;
  targetFreq_ = 0.0f;
  targetVolume_ = 0.0f;
  active_ = false;

  if (cfg.type == VoiceType::None) {
    return LoadError::Ok;
  }

  if (isTone()) {
    // Tone types need no additional loading
    return LoadError::Ok;
  }

  if (cfg.type == VoiceType::SampleFlash) {
    LoadError err = decodeFlashMP3(cfg.flashData, cfg.flashSize,
                                    sampleBuf_, maxCompressedBytes, maxDecodedFrames);
    if (err != LoadError::Ok) {
      type_ = VoiceType::None;
    }
    return err;
  }

  if (cfg.type == VoiceType::SampleSD) {
    LoadError err = decodeSDMP3(cfg.sdPath, sampleBuf_,
                                 maxCompressedBytes, maxDecodedFrames);
    if (err != LoadError::Ok) {
      type_ = VoiceType::None;
    }
    return err;
  }

  return LoadError::Ok;
}

void Voice::clear() {
  active_ = false;
  targetFreq_ = 0.0f;
  targetVolume_ = 0.0f;
  currentFreq_ = 0.0f;
  currentVolume_ = 0.0f;
  phase_ = 0.0f;
  samplePhase_ = 0.0f;
  type_ = VoiceType::None;
  maxVolume_ = 1.0f;
  sampleBuf_.free();
}

bool Voice::isTone() const {
  return type_ == VoiceType::Sine ||
         type_ == VoiceType::Square ||
         type_ == VoiceType::Sawtooth ||
         type_ == VoiceType::Triangle;
}

bool Voice::isSample() const {
  return type_ == VoiceType::SampleFlash ||
         type_ == VoiceType::SampleSD;
}

void Voice::setTargets(float freqOrRate, float volume) {
  targetFreq_ = freqOrRate;
  targetVolume_ = volume;
  active_ = true;
}

void Voice::deactivate() {
  targetVolume_ = 0.0f;
  // Don't immediately set active_ = false; let volume fade out first
  // The render loop will deactivate when volume reaches ~0
}

int16_t Voice::renderSample(float pitchSmoothing, float volumeSmoothing, uint32_t sampleRate) {
  if (type_ == VoiceType::None) return 0;

  // Smooth pitch/freq toward target
  currentFreq_ += (targetFreq_ - currentFreq_) * pitchSmoothing;

  // Smooth volume toward target
  currentVolume_ += (targetVolume_ - currentVolume_) * volumeSmoothing;

  // If volume has faded to near-zero and target is zero, fully deactivate
  if (currentVolume_ < 0.001f && targetVolume_ < 0.001f) {
    active_ = false;
    currentVolume_ = 0.0f;
    return 0;
  }

  int16_t raw = 0;

  if (isTone()) {
    raw = generateTone();
    // Advance phase accumulator
    float phaseInc = currentFreq_ / (float)sampleRate;
    phase_ += phaseInc;
    if (phase_ >= 1.0f) phase_ -= 1.0f;
    if (phase_ < 0.0f) phase_ = 0.0f;
  } else if (isSample()) {
    raw = generateSample(currentFreq_, sampleRate);
  }

  // Apply volume
  float scaled = (float)raw * currentVolume_ * maxVolume_;

  // Clamp to int16_t range
  if (scaled > 32767.0f) scaled = 32767.0f;
  if (scaled < -32767.0f) scaled = -32767.0f;

  return (int16_t)scaled;
}

int16_t Voice::generateTone() {
  return generateWaveform((uint8_t)type_, phase_);
}

int16_t Voice::generateSample(float playbackRate, uint32_t sampleRate) {
  if (!sampleBuf_.valid || sampleBuf_.frames == 0 || sampleBuf_.data == nullptr) {
    return 0;
  }

  size_t totalFrames = sampleBuf_.frames;

  // Get integer and fractional parts of current position
  size_t idx0 = (size_t)samplePhase_;
  float frac = samplePhase_ - (float)idx0;

  // Wrap index
  idx0 = idx0 % totalFrames;
  size_t idx1 = (idx0 + 1) % totalFrames;

  // Linear interpolation between adjacent samples
  float s0 = (float)sampleBuf_.data[idx0];
  float s1 = (float)sampleBuf_.data[idx1];
  float interpolated = s0 + frac * (s1 - s0);

  // Advance sample position by playback rate
  // playbackRate is relative: 1.0 = normal speed, 2.0 = double speed, etc.
  float advance = playbackRate * (float)sampleBuf_.sampleRate / (float)sampleRate;
  samplePhase_ += advance;

  // Wrap around for looping
  if (samplePhase_ >= (float)totalFrames) {
    samplePhase_ -= (float)totalFrames;
  }

  return (int16_t)interpolated;
}

} // namespace ClipTheremin
