#include "HRScanOverlayLVGL.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace HRScan {

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------

const char *OverlayLVGL::promptText(Prompt p) {
  switch (p) {
    case Prompt::Locked:      return "Locked";
    case Prompt::Locking:     return "Locking - hold steady";   // bar full, committing
    case Prompt::Searching:   return "Searching...";
    case Prompt::AdjustWeak:  return "Square up & hold";    // tag found, not locking -> align badge
                                                            // parallel to the tag face (any aim angle)
    case Prompt::MoveFarther: return "Back up";             // tag too close
    case Prompt::MoveCloser:  return "Move closer";         // tag too far
    case Prompt::TimedOut:    return "Timed Out";
    case Prompt::HoldSteady:
    case Prompt::MoveLeft:
    case Prompt::MoveRight:
    case Prompt::MoveUp:
    case Prompt::MoveDown:
    default:                  return "Hold Steady";
  }
}

OverlayLVGL::ScanState OverlayLVGL::resultToState(const Result &r) const {
  if (r.lockedId >= 0) return ScanState::Locked;
  // DC34-154: a structured-but-low-confidence read (the "looks good but decodes
  // wrong" class) -> Weak, checked BEFORE Decoding so it can't masquerade as a
  // clean decode. The engine sets prompt=AdjustWeak for exactly this case.
  if (r.prompt == Prompt::AdjustWeak ||
      r.prompt == Prompt::MoveCloser ||
      r.prompt == Prompt::MoveFarther) return ScanState::Weak;
  // r.run > 0 means a CONFIDENT, stable Ok decode is accumulating toward a lock
  // (track run only advances on confident frames now).
  if (r.run > 0) {
    return ScanState::Decoding;
  }
  // Engine ran cluster detection successfully (validCells indicates cells
  // had enough zones to vote). This is "tag detected, but bits don't form
  // a valid HR code yet" — typical when aiming.
  if (r.validCells >= 8) return ScanState::Detected;
  return ScanState::Idle;
}

// Map a depth-mm value to a theme-tinted heatmap color.
// NEAR (lo end) -> full theme accent. FAR (hi end) -> dim ~10% accent.
// Invalid -> deep dim.
// Grayscale mode (Flashbang) -> map to lightness gradient instead of tinting.
lv_color_t OverlayLVGL::depthToColor(int16_t mm, bool valid, int16_t lo, int16_t hi) const {
  (void)lo; (void)hi;  // legacy params -- now we drive off cfg.sweetSpotMm.
  if (!valid || mm <= 0) {
    return cfg_.grayscale ? lv_color_hex(0xC8C8C8) : lv_color_hex(0x222222);
  }

  // Brightness = how close the zone's depth is to the sweet spot.
  // At sweet spot exactly:   t = 0     -> themeAccent (full brightness)
  // At ±brightRange edge:    t = 255   -> themeBg     (dark)
  // Beyond that range:       clamped   -> themeBg
  int32_t delta = (int32_t)mm - (int32_t)cfg_.sweetSpotMm;
  if (delta < 0) delta = -delta;
  int32_t range = cfg_.sweetSpotBrightRangeMm > 0
                  ? cfg_.sweetSpotBrightRangeMm : 1;
  int t = (int)((delta * 255) / range);
  if (t < 0) t = 0;
  if (t > 255) t = 255;

  if (cfg_.grayscale) {
    // Flashbang: sweet-spot = darkest (printed black), edges = paper white.
    uint8_t v = (uint8_t)t;
    return lv_color_make(v, v, v);
  }
  // Blend themeAccent -> themeBg by t.
  lv_color_t a = cfg_.themeAccent;
  lv_color_t b = cfg_.themeBg;
  uint8_t ar = (a.red   << 3) | (a.red   >> 2);
  uint8_t ag = (a.green << 2) | (a.green >> 4);
  uint8_t ab = (a.blue  << 3) | (a.blue  >> 2);
  uint8_t br = (b.red   << 3) | (b.red   >> 2);
  uint8_t bg = (b.green << 2) | (b.green >> 4);
  uint8_t bb = (b.blue  << 3) | (b.blue  >> 2);
  uint8_t r = (uint8_t)(((255 - t) * ar + t * br) / 255);
  uint8_t g = (uint8_t)(((255 - t) * ag + t * bg) / 255);
  uint8_t b8 = (uint8_t)(((255 - t) * ab + t * bb) / 255);
  return lv_color_make(r, g, b8);
}

// ----------------------------------------------------------------------------
// begin / end
// ----------------------------------------------------------------------------

bool OverlayLVGL::begin(const OverlayConfig &cfg) {
  end();
  cfg_ = cfg;
  buildLayout();
  began_ = true;
  return true;
}

void OverlayLVGL::end() {
  if (root_) {
    lv_obj_delete(root_);
  }
  teardownLayout();
  began_ = false;
}

void OverlayLVGL::teardownLayout() {
  root_ = nullptr;
  heatmapBox_ = nullptr;
  levelOuter_ = nullptr;
  levelDot_ = nullptr;
  progressBar_ = nullptr;
  title_ = nullptr;
  instruction_ = nullptr;
  lockStatus_ = nullptr;
  for (uint8_t i = 0; i < 64; i++) cells_[i] = nullptr;
}

void OverlayLVGL::buildLayout() {
  lv_obj_t *scr = lv_screen_active();

  // Root container: overall bounding rect, transparent
  root_ = lv_obj_create(scr);
  lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(root_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_pos(root_, cfg_.x, cfg_.y);
  lv_obj_set_size(root_, cfg_.width, cfg_.height);
  lv_obj_set_style_bg_opa(root_, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(root_, 0, 0);
  lv_obj_set_style_pad_all(root_, 0, 0);

  // Layout split: left pane half, right pane half.
  const int leftW  = cfg_.width / 2;
  const int rightW = cfg_.width - leftW;
  const int rightX = leftW;

  // ---- LEFT PANE ----

  // Heatmap box: square, centered horizontally in left pane, slightly above
  // vertical center to leave room for the progress bar below.
  if (cfg_.showHeatmap) {
    int hmSize = (leftW < (cfg_.height - 40)) ? leftW - 20 : (cfg_.height - 40);
    if (hmSize < 64) hmSize = 64;
    if (hmSize > 160) hmSize = 160;
    const int hmX = (leftW - hmSize) / 2;
    const int hmY = (cfg_.height - hmSize - 24) / 2;  // 24 reserves room for progress

    heatmapBox_ = lv_obj_create(root_);
    lv_obj_remove_flag(heatmapBox_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(heatmapBox_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(heatmapBox_, hmX, hmY);
    lv_obj_set_size(heatmapBox_, hmSize, hmSize);
    lv_obj_set_style_pad_all(heatmapBox_, 0, 0);
    lv_obj_set_style_radius(heatmapBox_, 0, 0);
    lv_obj_set_style_bg_color(heatmapBox_, cfg_.themeBg, 0);
    lv_obj_set_style_bg_opa(heatmapBox_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(heatmapBox_, 1, 0);
    lv_obj_set_style_border_color(heatmapBox_, cfg_.themeAccent, 0);
    lv_obj_set_style_border_opa(heatmapBox_, LV_OPA_30, 0);

    const int cellPx = hmSize / 8;
    for (uint8_t i = 0; i < 64; i++) {
      lv_obj_t *c = lv_obj_create(heatmapBox_);
      lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
      const int r = i / 8, col = i % 8;
      lv_obj_set_size(c, cellPx - 1, cellPx - 1);
      lv_obj_set_pos(c, col * cellPx, r * cellPx);
      lv_obj_set_style_radius(c, 0, 0);
      lv_obj_set_style_pad_all(c, 0, 0);
      lv_obj_set_style_border_width(c, 0, 0);
      lv_obj_set_style_bg_color(c, cfg_.themeBg, 0);
      lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
      cells_[i] = c;
    }

    // Static CORNER GUIDES: a 2x2-cell target box at 3 corners (TL/TR/BR) so
    // users line the tag's 3 near-corner clusters up with them ("align the 3
    // corners with the guides"). Dim accent outline, drawn on top of the cells;
    // the bold white found-corner outline appears INSIDE a guide once it locks.
    static const uint8_t kGuideCell[3][2] = { {0, 0}, {6, 0}, {6, 6} };  // col,row: TL, TR, BR
    if (cfg_.showCodeGuides) for (int g = 0; g < 3; g++) {
      lv_obj_t *gd = lv_obj_create(heatmapBox_);
      lv_obj_remove_flag(gd, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_remove_flag(gd, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_size(gd, 2 * cellPx - 1, 2 * cellPx - 1);
      lv_obj_set_pos(gd, kGuideCell[g][0] * cellPx, kGuideCell[g][1] * cellPx);
      lv_obj_set_style_radius(gd, 0, 0);
      lv_obj_set_style_pad_all(gd, 0, 0);
      lv_obj_set_style_bg_opa(gd, LV_OPA_TRANSP, 0);
      lv_obj_set_style_border_width(gd, 2, 0);
      lv_obj_set_style_border_color(gd, cfg_.themeAccent, 0);
      lv_obj_set_style_border_opa(gd, LV_OPA_50, 0);
    }

    // Progress bar under the heatmap, centered to its width
    if (cfg_.showProgressBar) {
      const int barW = hmSize * 4 / 5;
      const int barH = 6;
      progressBar_ = lv_bar_create(root_);
      lv_obj_set_size(progressBar_, barW, barH);
      lv_obj_set_pos(progressBar_, hmX + (hmSize - barW) / 2, hmY + hmSize + 6);
      lv_bar_set_range(progressBar_, 0, 100);
      lv_bar_set_value(progressBar_, 0, LV_ANIM_OFF);
      lv_obj_set_style_bg_color(progressBar_, cfg_.themeBg, 0);
      lv_obj_set_style_bg_opa(progressBar_, LV_OPA_60, 0);
      lv_obj_set_style_border_width(progressBar_, 1, 0);
      lv_obj_set_style_border_color(progressBar_, cfg_.themeAccent, 0);
      lv_obj_set_style_border_opa(progressBar_, LV_OPA_50, 0);
      lv_obj_set_style_radius(progressBar_, 1, 0);
      lv_obj_set_style_bg_color(progressBar_, cfg_.themeAccent, LV_PART_INDICATOR);
      lv_obj_set_style_bg_opa(progressBar_, LV_OPA_COVER, LV_PART_INDICATOR);
    }
  }

  // Level bubble: small circle in top-right corner so it doesn't overlap
  // the heatmap (which fills most of the left pane).
  if (cfg_.showLevelBubble) {
    const int blOuter = 28;
    const int blDot   = 8;
    const int blX = cfg_.width - blOuter - 6;
    const int blY = 6;

    levelOuter_ = lv_obj_create(root_);
    lv_obj_remove_flag(levelOuter_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(levelOuter_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(levelOuter_, blX, blY);
    lv_obj_set_size(levelOuter_, blOuter, blOuter);
    lv_obj_set_style_radius(levelOuter_, blOuter / 2, 0);
    lv_obj_set_style_bg_color(levelOuter_, cfg_.themeBg, 0);
    lv_obj_set_style_bg_opa(levelOuter_, LV_OPA_60, 0);
    lv_obj_set_style_border_width(levelOuter_, 1, 0);
    lv_obj_set_style_border_color(levelOuter_, cfg_.themeAccent, 0);
    lv_obj_set_style_border_opa(levelOuter_, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(levelOuter_, 0, 0);

    levelDot_ = lv_obj_create(levelOuter_);
    lv_obj_remove_flag(levelDot_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(levelDot_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(levelDot_, blDot, blDot);
    lv_obj_set_style_radius(levelDot_, blDot / 2, 0);
    lv_obj_set_style_bg_color(levelDot_, cfg_.themeAccent, 0);
    lv_obj_set_style_bg_opa(levelDot_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(levelDot_, 0, 0);
    lv_obj_set_style_pad_all(levelDot_, 0, 0);
    // Center initially.
    lv_obj_set_pos(levelDot_, (blOuter - blDot) / 2 - 1, (blOuter - blDot) / 2 - 1);
  }

  // ---- RIGHT PANE ----

  if (cfg_.titleText && cfg_.titleText[0]) {
    title_ = lv_label_create(root_);
    lv_label_set_long_mode(title_, LV_LABEL_LONG_DOT);
    // Reserve room for the level bubble in the top-right corner.
    int titleW = rightW - 16;
    if (cfg_.showLevelBubble) titleW -= 36;
    lv_obj_set_width(title_, titleW);
    lv_label_set_text(title_, cfg_.titleText);
    lv_obj_set_style_text_color(title_, cfg_.themeAccent, 0);
    lv_obj_set_style_text_font(title_, &lv_font_montserrat_18, 0);
    lv_obj_set_pos(title_, rightX + 8, 8);
  }

  if (cfg_.instructionText && cfg_.instructionText[0]) {
    instruction_ = lv_label_create(root_);
    lv_label_set_long_mode(instruction_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(instruction_, rightW - 16);
    lv_label_set_text(instruction_, cfg_.instructionText);
    lv_obj_set_style_text_color(instruction_, cfg_.themeAccent, 0);
    lv_obj_set_style_text_opa(instruction_, LV_OPA_80, 0);
    lv_obj_set_style_text_font(instruction_, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(instruction_, rightX + 8, 36);
  }

  // ---- LEGEND: what the heatmap squares mean (static; helps users read it) ----
  // A sample swatch + label per state so users don't have to reverse-engineer the
  // colors. Matches renderHeatmap: dim = far/empty, bright accent = near/code,
  // bright + bold outline = a found corner (anchor). Hidden in calibration (the
  // HR-code legend is meaningless there) via showCodeGuides.
  if (cfg_.showHeatmap && cfg_.showCodeGuides) {
    // Swatch fills MUST mirror depthToColor per theme, or the legend lies: on
    // Flashbang the heatmap uses a grayscale ramp (near/code = BLACK, far/empty =
    // light 0xC8C8C8), never themeAccent. Found-corner outline is WHITE on every
    // theme (matches renderHeatmap). A thin delineator keeps a black swatch
    // visible on the black overlay bg.
    const lv_color_t nearCol = cfg_.grayscale ? lv_color_black()       : cfg_.themeAccent;
    const lv_color_t farCol  = cfg_.grayscale ? lv_color_hex(0xC8C8C8) : cfg_.themeAccent;
    const lv_opa_t   farOpa  = cfg_.grayscale ? LV_OPA_COVER           : LV_OPA_30;
    const lv_color_t delin   = lv_color_hex(0x606060);
    struct { lv_color_t fill; lv_opa_t opa; int bw; lv_color_t bcol; const char *label; } rows[3] = {
      { farCol,  farOpa,       1, delin,            "far / empty" },
      { nearCol, LV_OPA_COVER, 1, delin,            "code (near)" },
      { nearCol, LV_OPA_COVER, 3, lv_color_white(), "a corner" },
    };
    int ly = 106;   // below the (up to ~4-line) instruction text
    for (int i = 0; i < 3; i++) {
      lv_obj_t *sw = lv_obj_create(root_);
      lv_obj_remove_flag(sw, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_remove_flag(sw, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_pos(sw, rightX + 8, ly);
      lv_obj_set_size(sw, 16, 16);
      lv_obj_set_style_radius(sw, 0, 0);
      lv_obj_set_style_pad_all(sw, 0, 0);
      lv_obj_set_style_bg_color(sw, rows[i].fill, 0);
      lv_obj_set_style_bg_opa(sw, rows[i].opa, 0);
      lv_obj_set_style_border_width(sw, rows[i].bw, 0);
      lv_obj_set_style_border_color(sw, rows[i].bcol, 0);
      lv_obj_set_style_border_opa(sw, LV_OPA_COVER, 0);

      lv_obj_t *lb = lv_label_create(root_);
      lv_label_set_text(lb, rows[i].label);
      lv_obj_set_style_text_color(lb, cfg_.themeAccent, 0);
      lv_obj_set_style_text_opa(lb, LV_OPA_80, 0);
      lv_obj_set_style_text_font(lb, &lv_font_montserrat_12, 0);
      lv_obj_set_pos(lb, rightX + 8 + 22, ly + 1);
      ly += 22;
    }
  }

  if (cfg_.showLockStatus) {
    lockStatus_ = lv_label_create(root_);
    lv_label_set_text(lockStatus_, "Searching...");
    lv_obj_set_style_text_color(lockStatus_, cfg_.themeAccent, 0);
    lv_obj_set_style_text_font(lockStatus_, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(lockStatus_, rightX + 8, cfg_.height - 28);
  }
}

// ----------------------------------------------------------------------------
// Per-frame
// ----------------------------------------------------------------------------

void OverlayLVGL::setSensorData(const int16_t mm[8][8], const bool valid[8][8]) {
  memcpy(mm_, mm, sizeof(mm_));
  memcpy(valid_, valid, sizeof(valid_));
  sensorFresh_ = true;
}

void OverlayLVGL::setLevel(float roll_deg, float pitch_deg) {
  rollDeg_ = roll_deg;
  pitchDeg_ = pitch_deg;
  levelFresh_ = true;
}

void OverlayLVGL::update(const Result &r) {
  if (!began_ || !root_) return;

  ScanState s = resultToState(r);
  renderOutline(s);
  lastState_ = s;

  anchorsFound_ = r.anchorsFound;
  memcpy(anchorMask_, r.anchorMask, sizeof(anchorMask_));

  if (cfg_.showHeatmap) renderHeatmap();
  if (cfg_.showLevelBubble) renderLevelBubble();

  uint8_t pct = r.progress;
  if (pct > 100) pct = 100;
  if (cfg_.showProgressBar) renderProgressBar(pct);

  renderText(r);
}

void OverlayLVGL::renderHeatmap() {
  if (!heatmapBox_ || !cells_[0]) return;

  // depthToColor now drives brightness off cfg_.sweetSpotMm directly;
  // no auto-scale needed. lo/hi args are passed through as 0,0 (ignored).

  // (Sweet-spot per-cell white borders were removed -- they lit up EVERY near
  // cell at good distance, burying the 3 corner outlines in noise. Distance is
  // now conveyed by the prompt text (Back up / Move closer) instead.)
  for (uint8_t r = 0; r < 8; r++) {
    for (uint8_t c = 0; c < 8; c++) {
      // X-flip the display so the heatmap matches what the user sees when
      // facing the wall-mounted tag.
      lv_obj_t *cell = cells_[r * 8 + (7 - c)];
      lv_color_t col = depthToColor(mm_[r][c], valid_[r][c], 0, 0);
      lv_obj_set_style_bg_color(cell, col, 0);

      // Anchor corners take visual priority: a bold HIGH-CONTRAST outline says
      // "I found your 3 corners". Accent-on-accent (border==near-cell fill) was
      // near-invisible; use black on the light Flashbang theme, white otherwise,
      // so the corners pop against the accent-filled near cells on every theme.
      // ONLY the 3 found corners get an outline. Always WHITE: near/code cells
      // are amber (Mojave), green (Ribbit), or BLACK (Flashbang grayscale ramp) --
      // white contrasts all three. (The earlier grayscale?black was wrong: on
      // Flashbang near cells render black, so a black outline was invisible.)
      bool is_anchor = anchorsFound_ && (anchorMask_[r] & (1u << c));
      if (is_anchor) {
        lv_obj_set_style_border_width(cell, 3, 0);
        lv_obj_set_style_border_color(cell, lv_color_white(), 0);
        lv_obj_set_style_border_opa(cell, LV_OPA_COVER, 0);
      } else {
        lv_obj_set_style_border_width(cell, 0, 0);
      }
    }
  }
}

void OverlayLVGL::renderLevelBubble() {
  if (!levelOuter_ || !levelDot_) return;

  // Saturate roll/pitch to ±45° for the dot's drift extent.
  const float maxDeg = 45.0f;
  float rx = rollDeg_  / maxDeg;  if (rx > 1) rx = 1; if (rx < -1) rx = -1;
  float ry = pitchDeg_ / maxDeg;  if (ry > 1) ry = 1; if (ry < -1) ry = -1;

  const int outerSize = 28;
  const int dotSize   = 8;
  const int travel = (outerSize - dotSize) / 2 - 1;  // px from center to edge
  const int cx = (outerSize - dotSize) / 2 - 1;
  const int cy = (outerSize - dotSize) / 2 - 1;
  // Roll = right tilt -> dot drifts right. Pitch nose-up -> dot drifts up
  // (visually, on the screen, that's negative Y).
  int dx = (int)(rx * travel);
  int dy = (int)(-ry * travel);
  lv_obj_set_pos(levelDot_, cx + dx, cy + dy);

  const bool isLevel = (fabsf(rollDeg_) <= 3.0f) && (fabsf(pitchDeg_) <= 3.0f);
  lv_obj_set_style_bg_color(levelDot_,
      isLevel ? lv_color_hex(0xFFFFFF) : cfg_.themeAccent, 0);
}

void OverlayLVGL::renderProgressBar(uint8_t pct) {
  if (!progressBar_) return;
  lv_bar_set_value(progressBar_, pct, LV_ANIM_OFF);
}

void OverlayLVGL::renderText(const Result &r) {
  if (lockStatus_) {
    char buf[48];
    if (r.lockedId >= 0) {
      snprintf(buf, sizeof(buf), "Locked: %d", (int)r.lockedId);
    } else {
      snprintf(buf, sizeof(buf), "%s", promptText(r.prompt));
    }
    lv_label_set_text(lockStatus_, buf);
  }
}

void OverlayLVGL::renderOutline(ScanState s) {
  if (!heatmapBox_) return;
  if (s == lastState_) return;  // avoid LVGL churn when state unchanged

  switch (s) {
    case ScanState::Idle:
      lv_obj_set_style_border_color(heatmapBox_, cfg_.themeAccent, 0);
      lv_obj_set_style_border_opa(heatmapBox_, LV_OPA_30, 0);
      lv_obj_set_style_border_width(heatmapBox_, 1, 0);
      break;
    case ScanState::Detected:
      lv_obj_set_style_border_color(heatmapBox_, cfg_.themeAccent, 0);
      lv_obj_set_style_border_opa(heatmapBox_, LV_OPA_COVER, 0);
      lv_obj_set_style_border_width(heatmapBox_, 1, 0);
      break;
    case ScanState::Decoding:
      lv_obj_set_style_border_color(heatmapBox_, lv_color_hex(0xFFFFFF), 0);
      lv_obj_set_style_border_opa(heatmapBox_, LV_OPA_COVER, 0);
      lv_obj_set_style_border_width(heatmapBox_, 2, 0);
      break;
    case ScanState::Locked:
      lv_obj_set_style_border_color(heatmapBox_, lv_color_hex(0xFFFFFF), 0);
      lv_obj_set_style_border_opa(heatmapBox_, LV_OPA_COVER, 0);
      lv_obj_set_style_border_width(heatmapBox_, 3, 0);
      break;
    case ScanState::Weak:
      // amber warning — "I see a tag but the read is weak; adjust" (DC34-154).
      // Fixed amber (theme-independent) so the warning always reads as caution.
      lv_obj_set_style_border_color(heatmapBox_, lv_color_hex(0xFFB000), 0);
      lv_obj_set_style_border_opa(heatmapBox_, LV_OPA_COVER, 0);
      lv_obj_set_style_border_width(heatmapBox_, 2, 0);
      break;
  }
}

} // namespace HRScan
