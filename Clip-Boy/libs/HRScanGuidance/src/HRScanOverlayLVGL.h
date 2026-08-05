#pragma once

#include <Arduino.h>
#include <lvgl.h>
#include "HRScanEngine.h"

namespace HRScan {

// Overlay layout:
//
//   ┌─────────────┬─────────────┐
//   │  ◉ level    │  TITLE       │
//   │             │              │
//   │  ┌───────┐  │  Instruction │  <- text wraps in right pane
//   │  │ 8x8   │  │  text...     │
//   │  │ heat- │  │              │
//   │  │ map   │  │              │
//   │  └───────┘  │  Locked: 55  │
//   │  ▓▓░░░░░    │              │
//   └─────────────┴─────────────┘
//
// Heatmap state outlines:
//   Idle       : dim border (theme @ 30% alpha)
//   Detected   : theme accent border, solid
//   Decoding   : white border, 2x thickness
//   Locked     : white border + brighten cells
//
// Level bubble: small circle in the upper-left of the left pane. Inner
// dot drifts with roll/pitch. Centers + turns white when level (±3°).

struct OverlayConfig {
  int x = 0;
  int y = 0;
  int width = 320;
  int height = 240;

  // Theme palette. Heatmap NEAR cells use themeAccent; FAR fades to themeBg.
  // For Flashbang (B/W) theme set grayscale=true to render in lightness only.
  lv_color_t themeAccent = lv_color_hex(0xFFB700);   // amber default (Mojave)
  lv_color_t themeBg     = lv_color_hex(0x000000);
  bool grayscale = false;

  const char *titleText       = "HR Scanner";
  const char *instructionText = "Aim the badge so the bumps fill the grid.";

  bool showHeatmap      = true;
  bool showLevelBubble  = true;
  bool showProgressBar  = true;
  bool showLockStatus   = true;
  bool showCodeGuides   = true;   // HR-code chrome (3 corner guides + legend); off for calibration aim

  // Distance-feedback heatmap.
  //
  // The cell color is driven by how close the zone's depth is to the
  // sweet spot, NOT by NEAR-vs-FAR per se:
  //   - At sweetSpotMm:                themeAccent (brightest)
  //   - At sweetSpotMm ± brightRangeMm: themeBg    (black/dim)
  //   - Outside that window:           themeBg    (clamped)
  // So "too close" zones AND "too far" zones both go dark; only the
  // right-distance zones light up. Reads instantly.
  //
  // The narrower ±sweetSpotToleranceMm window additionally gets a white
  // 1-px border for the "you're perfectly centered" cue.
  //
  // Set sweetSpotMm = 0 to disable the sweet-spot driving entirely
  // (falls back to a generic NEAR=bright gradient via auto-scale).
  int16_t sweetSpotMm                 = 75;   // empirical: 7.5 cm
  int16_t sweetSpotBrightRangeMm      = 15;   // 1.5 cm fade window each side
  int16_t sweetSpotToleranceMm        = 8;    // narrower "perfect" indicator
};

class OverlayLVGL {
public:
  bool begin(const OverlayConfig &cfg);
  void end();
  void update(const Result &r);

  // Optional: feed raw 8x8 sensor data for the heatmap. If never called,
  // cells stay dim. Call any time before next update().
  void setSensorData(const int16_t mm[8][8], const bool valid[8][8]);

  // Optional: feed IMU roll/pitch in degrees for the level bubble.
  // Convention: roll = X-axis tilt (+ = right tilt), pitch = Y-axis tilt
  // (+ = nose-up). Bubble centers when both within ±3°.
  void setLevel(float roll_deg, float pitch_deg);

private:
  // Weak = tag present but the read is low-confidence (DC34-154): amber border,
  // distinct from Detected/Decoding so "looks good but unreadable" is obvious.
  enum class ScanState : uint8_t { Idle, Detected, Decoding, Locked, Weak };

  OverlayConfig cfg_;
  bool          began_ = false;

  // Containers
  lv_obj_t *root_       = nullptr;   // overall bounding rect (transparent)
  lv_obj_t *heatmapBox_ = nullptr;   // wraps the 64 cells, owns the outline border
  lv_obj_t *cells_[64]  = {nullptr};
  lv_obj_t *levelOuter_ = nullptr;
  lv_obj_t *levelDot_   = nullptr;
  lv_obj_t *progressBar_ = nullptr;
  lv_obj_t *title_       = nullptr;
  lv_obj_t *instruction_ = nullptr;
  lv_obj_t *lockStatus_  = nullptr;

  // Sensor data
  int16_t  mm_[8][8] = {{0}};
  bool     valid_[8][8] = {{false}};
  bool     sensorFresh_ = false;

  // DC34-155: identified anchor zones (from Result), highlighted in the heatmap.
  bool     anchorsFound_ = false;
  uint8_t  anchorMask_[8] = {0};

  // Level data
  float    rollDeg_ = 0.0f;
  float    pitchDeg_ = 0.0f;
  bool     levelFresh_ = false;

  ScanState lastState_ = ScanState::Idle;

  // Build helpers
  void buildLayout();
  void teardownLayout();

  // Per-frame updates
  void renderHeatmap();
  void renderLevelBubble();
  void renderProgressBar(uint8_t progressPct);
  void renderText(const Result &r);
  void renderOutline(ScanState s);

  // Helpers
  ScanState resultToState(const Result &r) const;
  lv_color_t depthToColor(int16_t mm, bool valid, int16_t lo, int16_t hi) const;
  static const char *promptText(Prompt p);
};

} // namespace HRScan
