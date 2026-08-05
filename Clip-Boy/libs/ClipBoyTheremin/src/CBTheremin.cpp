#include "CBTheremin.h"
#include "CBWaveforms.h"

#include <cmath>
#include <cstring>

namespace ClipTheremin {

Theremin::Theremin() {
  for (uint8_t i = 0; i < MAX_VOICES; i++) {
    columnDist_[i] = -1;
  }
}

Theremin::~Theremin() {
  end();
}

bool Theremin::begin(const Config &cfg) {
  if (begun_) end();

  cfg_ = cfg;
  begun_ = true;

  // Initialize sine lookup table
  g_sineTable.init();

  // Reset column distances
  for (uint8_t i = 0; i < MAX_VOICES; i++) {
    columnDist_[i] = -1;
  }

  return true;
}

void Theremin::end() {
  if (!begun_) return;
  clearAllVoices();
  begun_ = false;
}

LoadError Theremin::loadVoice(uint8_t slot, const VoiceConfig &vcfg) {
  if (slot >= MAX_VOICES) return LoadError::InvalidSlot;
  return voices_[slot].load(vcfg, cfg_.maxSampleBytes, cfg_.maxDecodedFrames);
}

void Theremin::clearVoice(uint8_t slot) {
  if (slot >= MAX_VOICES) return;
  voices_[slot].clear();
}

void Theremin::clearAllVoices() {
  for (uint8_t i = 0; i < MAX_VOICES; i++) {
    voices_[i].clear();
  }
}

VoiceType Theremin::voiceType(uint8_t slot) const {
  if (slot >= MAX_VOICES) return VoiceType::None;
  return voices_[slot].type();
}

void Theremin::feed(const VL53L5CX_ResultsData &data) {
  if (!begun_) return;

  // Process each column (0-7) of the 8x8 grid
  // VL53L5CX data is stored in row-major order: index = row * 8 + col
  for (uint8_t col = 0; col < 8; col++) {
    int32_t sum = 0;
    int count = 0;

    for (uint8_t row = 0; row < 8; row++) {
      uint8_t idx = row * 8 + col;
      uint8_t status = data.target_status[idx];

      // Only accept valid target status values (5 or 9)
      if (status == 5 || status == 9) {
        int16_t dist = data.distance_mm[idx];
        if (dist > 0) {
          sum += dist;
          count++;
        }
      }
    }

    if (count > 0) {
      int avgDist = (int)(sum / count);
      columnDist_[col] = avgDist;

      // Update voice targets if this slot has a voice loaded
      if (voices_[col].type() != VoiceType::None) {
        if (avgDist <= cfg_.silenceDistMm) {
          float vol = distToVolume(avgDist);

          if (voices_[col].isTone()) {
            float freq = distToFreq(avgDist, voices_[col].minFreqHz(), voices_[col].maxFreqHz());
            voices_[col].setTargets(freq, vol);
          } else if (voices_[col].isSample()) {
            float rate = distToPlaybackRate(avgDist);
            voices_[col].setTargets(rate, vol);
          }
        } else {
          voices_[col].deactivate();
          columnDist_[col] = -1;
        }
      }
    } else {
      columnDist_[col] = -1;
      if (voices_[col].type() != VoiceType::None) {
        voices_[col].deactivate();
      }
    }
  }
}

size_t Theremin::render(int16_t *buf, size_t frames) {
  if (!begun_ || !buf || frames == 0) return 0;

  // Count active voices for mixing gain
  uint8_t activeCount = 0;
  for (uint8_t i = 0; i < MAX_VOICES; i++) {
    if (voices_[i].type() != VoiceType::None &&
        (voices_[i].isActive() || voices_[i].currentVolume() > 0.001f)) {
      activeCount++;
    }
  }

  for (size_t f = 0; f < frames; f++) {
    int32_t mixedSample = 0;

    // Render and mix all voices
    for (uint8_t v = 0; v < MAX_VOICES; v++) {
      if (voices_[v].type() == VoiceType::None) continue;
      // Render even if not "active" to allow volume fade-out
      int16_t s = voices_[v].renderSample(cfg_.pitchSmoothing, cfg_.volumeSmoothing, cfg_.sampleRate);
      mixedSample += (int32_t)s;
    }

    // Apply master volume
    mixedSample = (int32_t)((float)mixedSample * cfg_.masterVolume);

    // Soft clip the mixed output
    int16_t clipped = softClip(mixedSample);

    // Write to output buffer (interleaved stereo or mono)
    if (cfg_.channels == 2) {
      buf[f * 2]     = clipped;  // Left
      buf[f * 2 + 1] = clipped;  // Right (duplicate mono)
    } else {
      buf[f] = clipped;
    }
  }

  return frames;
}

void Theremin::setMasterVolume(float v) {
  if (v < 0.0f) v = 0.0f;
  if (v > 1.0f) v = 1.0f;
  cfg_.masterVolume = v;
}

bool Theremin::isActive() const {
  for (uint8_t i = 0; i < MAX_VOICES; i++) {
    if (voices_[i].isActive()) return true;
  }
  return false;
}

uint8_t Theremin::activeVoiceMask() const {
  uint8_t mask = 0;
  for (uint8_t i = 0; i < MAX_VOICES; i++) {
    if (voices_[i].isActive()) mask |= (1 << i);
  }
  return mask;
}

float Theremin::voiceFreq(uint8_t slot) const {
  if (slot >= MAX_VOICES) return 0.0f;
  return voices_[slot].currentFreq();
}

float Theremin::voiceVolume(uint8_t slot) const {
  if (slot >= MAX_VOICES) return 0.0f;
  return voices_[slot].currentVolume();
}

int Theremin::voiceDistance(uint8_t slot) const {
  if (slot >= MAX_VOICES) return -1;
  return columnDist_[slot];
}

// --- Distance mapping functions ---

float Theremin::distToFreq(int distMm, float minHz, float maxHz) const {
  // Clamp distance to usable range
  float d = (float)distMm;
  float dMin = (float)cfg_.minDistMm;
  float dMax = (float)cfg_.maxDistMm;

  if (d < dMin) d = dMin;
  if (d > dMax) d = dMax;

  // Logarithmic mapping: closer = higher pitch
  // Normalize distance to 0..1 (0 = close/minDist, 1 = far/maxDist)
  float t = (d - dMin) / (dMax - dMin);

  // Logarithmic curve for musical feel
  // Map t (0=close, 1=far) to frequency (high to low)
  // Use exponential mapping: freq = maxHz * (minHz/maxHz)^t
  // When t=0 (close): freq = maxHz
  // When t=1 (far):   freq = minHz
  float freq = maxHz * powf(minHz / maxHz, t);

  return freq;
}

float Theremin::distToPlaybackRate(int distMm) const {
  // Map distance to playback rate using logarithmic curve
  // Close (~50mm) = 2.0x speed (higher pitch)
  // Normal (~200mm) = 1.0x speed
  // Far (~400mm) = 0.5x speed (lower pitch)
  float d = (float)distMm;
  float dMin = (float)cfg_.minDistMm;
  float dMax = (float)cfg_.maxDistMm;

  if (d < dMin) d = dMin;
  if (d > dMax) d = dMax;

  // Normalize to 0..1
  float t = (d - dMin) / (dMax - dMin);

  // Logarithmic: rate = 2.0 * (0.5/2.0)^t = 2.0 * 0.25^t
  // t=0 (close): rate = 2.0
  // t=1 (far):   rate = 0.5
  float rate = 2.0f * powf(0.25f, t);

  return rate;
}

float Theremin::distToVolume(int distMm) const {
  // Closer = louder, farther = quieter
  float d = (float)distMm;
  float dMin = (float)cfg_.minDistMm;
  float dMax = (float)cfg_.silenceDistMm;

  if (d < dMin) d = dMin;
  if (d > dMax) return 0.0f;

  // Linear volume falloff from 1.0 (at minDist) to 0.0 (at silenceDist)
  float vol = 1.0f - (d - dMin) / (dMax - dMin);

  // Apply a slight curve for more natural feel
  vol = vol * vol;  // Quadratic falloff

  if (vol < 0.0f) vol = 0.0f;
  if (vol > 1.0f) vol = 1.0f;

  return vol;
}

int16_t Theremin::softClip(int32_t sample) {
  // Tanh-style soft clipping
  // For small values, pass through. For large values, compress toward limits.
  if (sample >= -32767 && sample <= 32767) {
    return (int16_t)sample;
  }

  // Normalize to [-1, 1] range based on expected max (e.g., 8 voices at full scale)
  float normalized = (float)sample / 32767.0f;

  // Fast tanh approximation
  // tanh(x) ~ x / (1 + |x|) for a simpler curve
  // But we want something that saturates more gracefully
  float clipped;
  if (normalized > 0.0f) {
    clipped = 1.0f - 1.0f / (1.0f + normalized);
  } else {
    clipped = -1.0f + 1.0f / (1.0f - normalized);
  }

  return (int16_t)(clipped * 32767.0f);
}

} // namespace ClipTheremin
