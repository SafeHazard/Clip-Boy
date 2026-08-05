#pragma once

#include <cstdint>
#include <cmath>

namespace ClipTheremin {

// --- Sine lookup table (1024 entries, Q15 fixed-point) ---
static constexpr uint16_t SINE_TABLE_SIZE = 1024;

// Runtime-initialized sine table (avoids constexpr array init issues on ESP32)
struct SineTable {
  int16_t table[SINE_TABLE_SIZE];
  bool initialized = false;

  void init() {
    if (initialized) return;
    for (int i = 0; i < SINE_TABLE_SIZE; i++) {
      table[i] = (int16_t)(32767.0f * sinf(2.0f * 3.14159265f * (float)i / (float)SINE_TABLE_SIZE));
    }
    initialized = true;
  }

  // Phase is 0.0 to 1.0, returns -32767 to 32767
  inline int16_t lookup(float phase) const {
    float idx = phase * SINE_TABLE_SIZE;
    int i0 = (int)idx & (SINE_TABLE_SIZE - 1);
    int i1 = (i0 + 1) & (SINE_TABLE_SIZE - 1);
    float frac = idx - (int)idx;
    return (int16_t)((float)table[i0] + frac * (float)(table[i1] - table[i0]));
  }
};

// Global sine table instance (inline to ensure single instance across TUs)
inline SineTable g_sineTable;

// --- Waveform generation functions ---
// All take phase in [0.0, 1.0) and return sample in [-32767, 32767]

inline int16_t waveformSine(float phase) {
  return g_sineTable.lookup(phase);
}

inline int16_t waveformSquare(float phase) {
  return (phase < 0.5f) ? 32767 : -32767;
}

inline int16_t waveformSawtooth(float phase) {
  // Ramp from -32767 to 32767 over one period
  return (int16_t)((phase * 2.0f - 1.0f) * 32767.0f);
}

inline int16_t waveformTriangle(float phase) {
  // Triangle: rise 0->0.25, fall 0.25->0.75, rise 0.75->1.0
  float val;
  if (phase < 0.25f) {
    val = phase * 4.0f;          // 0 to 1
  } else if (phase < 0.75f) {
    val = 1.0f - (phase - 0.25f) * 4.0f;  // 1 to -1
  } else {
    val = -1.0f + (phase - 0.75f) * 4.0f; // -1 to 0
  }
  return (int16_t)(val * 32767.0f);
}

// Dispatch waveform by type index (1=Sine, 2=Square, 3=Saw, 4=Tri)
// Matches VoiceType enum values for tone types
inline int16_t generateWaveform(uint8_t typeIdx, float phase) {
  switch (typeIdx) {
    case 1: return waveformSine(phase);
    case 2: return waveformSquare(phase);
    case 3: return waveformSawtooth(phase);
    case 4: return waveformTriangle(phase);
    default: return 0;
  }
}

} // namespace ClipTheremin
