#pragma once

#include <cstdint>
#include <cstddef>
#include "CBSampleDecoder.h"

namespace ClipTheremin {

enum class VoiceType : uint8_t {
  None = 0,
  Sine,           // 1
  Square,         // 2
  Sawtooth,       // 3
  Triangle,       // 4
  SampleFlash,    // 5
  SampleSD,       // 6
};

struct VoiceConfig {
  VoiceType type = VoiceType::None;
  // For tone types (Sine/Square/Sawtooth/Triangle):
  float minFreqHz  = 100.0f;   // Frequency at max distance
  float maxFreqHz  = 2000.0f;  // Frequency at min distance
  // For sample types:
  const uint8_t *flashData = nullptr;  // PROGMEM pointer (SampleFlash)
  size_t flashSize = 0;
  const char *sdPath = nullptr;        // SD file path (SampleSD)
  // Common:
  float maxVolume = 1.0f;              // Per-voice volume cap (0.0-1.0)
};

// Internal voice state, managed by Theremin
class Voice {
public:
  Voice();

  // Setup
  LoadError load(const VoiceConfig &cfg, size_t maxCompressedBytes, size_t maxDecodedFrames);
  void clear();

  // State queries
  VoiceType type() const { return type_; }
  bool isTone() const;
  bool isSample() const;
  bool isActive() const { return active_; }

  // Sensor-driven targets (set by feed(), read by render())
  void setTargets(float freqOrRate, float volume);
  void deactivate();

  // Rendering: generate one mono sample, advance phase
  int16_t renderSample(float pitchSmoothing, float volumeSmoothing, uint32_t sampleRate);

  // Accessors for UI
  float currentFreq() const { return currentFreq_; }
  float currentVolume() const { return currentVolume_; }
  float maxVolume() const { return maxVolume_; }
  float minFreqHz() const { return minFreqHz_; }
  float maxFreqHz() const { return maxFreqHz_; }

private:
  VoiceType type_ = VoiceType::None;
  float maxVolume_ = 1.0f;

  // Tone parameters
  float minFreqHz_ = 100.0f;
  float maxFreqHz_ = 2000.0f;

  // Phase accumulator (0.0 to 1.0)
  float phase_ = 0.0f;

  // Sample buffer (for SampleFlash/SampleSD)
  SampleBuffer sampleBuf_;
  float samplePhase_ = 0.0f;  // Fractional position in sample buffer

  // Smoothed current values
  float currentFreq_ = 0.0f;
  float currentVolume_ = 0.0f;

  // Target values (set atomically from feed thread)
  volatile float targetFreq_ = 0.0f;
  volatile float targetVolume_ = 0.0f;
  volatile bool active_ = false;

  // Generate tone sample at current phase
  int16_t generateTone();

  // Generate sample-playback sample at current phase
  int16_t generateSample(float playbackRate, uint32_t sampleRate);
};

} // namespace ClipTheremin
