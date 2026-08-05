#pragma once

#include <cstdint>
#include <cstddef>
#include <SparkFun_VL53L5CX_Library.h>
#include "CBVoice.h"

namespace ClipTheremin {

static constexpr uint8_t MAX_VOICES = 8;

struct Config {
  uint32_t sampleRate = 44100;        // Must match I2S config
  uint8_t  channels = 2;              // Stereo output
  float    masterVolume = 1.0f;       // Master volume multiplier
  // Distance mapping
  int      minDistMm = 50;           // Closest usable distance
  int      maxDistMm = 400;          // Farthest usable distance
  int      silenceDistMm = 500;      // Beyond this = silence
  // Smoothing (applied per sample: 0.0 = frozen, 1.0 = instant)
  float    pitchSmoothing = 0.005f;
  float    volumeSmoothing = 0.01f;
  // Sample limits
  size_t   maxSampleBytes = 2 * 1024 * 1024;       // Max compressed MP3 size
  size_t   maxDecodedFrames = 44100UL * 30UL;       // Max 30 seconds decoded mono PCM
};

class Theremin {
public:
  Theremin();
  ~Theremin();

  // Lifecycle
  bool begin(const Config &cfg);
  void end();

  // Voice management
  LoadError loadVoice(uint8_t slot, const VoiceConfig &vcfg);
  void clearVoice(uint8_t slot);
  void clearAllVoices();
  VoiceType voiceType(uint8_t slot) const;

  // Sensor data input (call at sensor frame rate, ~15Hz, from core 1)
  void feed(const VL53L5CX_ResultsData &data);

  // Audio rendering (call from audio loop/task on core 0)
  // Fills interleaved stereo PCM (or mono if channels==1)
  // Returns number of frames actually written
  size_t render(int16_t *buf, size_t frames);

  // Live volume control — updates masterVolume in cfg_ without re-init.
  // Snapshot at begin() is fine for most uses, but UI sliders need this
  // so volume changes take effect mid-render instead of on next begin().
  void setMasterVolume(float v);

  // Status
  bool isActive() const;
  uint8_t activeVoiceMask() const;

  // Per-voice state (for UI display)
  float voiceFreq(uint8_t slot) const;
  float voiceVolume(uint8_t slot) const;
  int voiceDistance(uint8_t slot) const;

private:
  Config cfg_;
  Voice voices_[MAX_VOICES];
  bool begun_ = false;

  // Per-column distance (set by feed, read by render)
  volatile int columnDist_[MAX_VOICES];  // -1 = no valid reading

  // Distance-to-frequency mapping (logarithmic)
  float distToFreq(int distMm, float minHz, float maxHz) const;

  // Distance-to-playback-rate mapping (logarithmic)
  float distToPlaybackRate(int distMm) const;

  // Distance-to-volume mapping
  float distToVolume(int distMm) const;

  // Soft clipping function
  static int16_t softClip(int32_t sample);
};

} // namespace ClipTheremin
