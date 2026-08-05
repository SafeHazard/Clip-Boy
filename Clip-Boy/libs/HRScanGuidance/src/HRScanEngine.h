#pragma once

#include <Arduino.h>
#include <HRCode4x4.h>

namespace HRScan {

enum class Prompt : uint8_t {
  HoldSteady = 0,
  Searching,
  TimedOut,
  MoveLeft,
  MoveRight,
  MoveUp,
  MoveDown,
  MoveCloser,
  MoveFarther,
  Locked,
  AdjustWeak,  // tag found, but the read is low-confidence -> nudge (closer/flatter/recenter)
  Locking      // bar full, accruing the clean-frame evidence to commit -> "hold steady"
};

enum class Profile : uint8_t {
  Strict15mm = 0,
  Sensitive10mm = 1,
};

struct CellFeedback {
  bool valid;
  uint8_t bit;
  uint8_t confidence; // 0..255
};

struct Result {
  HRCode4x4::DecodeResult decode;
  bool ok;
  bool timedOut;
  int lockedId;
  uint8_t run;
  uint8_t runRequired;
  uint8_t progress; // 0..100, visual lock progress
  uint32_t scanElapsedMs;
  uint8_t validCells;
  float separationMm;
  int threshold;
  bool residualMode;
  bool usedMirror;
  uint8_t roiX;
  uint8_t roiY;
  uint8_t roiSize;
  Prompt prompt;
  CellFeedback cells[16];
  // DC34-155: the 3 identified anchor corners (8x8 zone bitmask, bit c of
  // anchorMask[r] => zone (r,c) belongs to an anchor). The overlay highlights
  // these so the user sees "found your corners" and can align -> fewer skew
  // misreads. anchorsFound=false when localization hasn't locked 3 anchors.
  bool anchorsFound = false;
  uint8_t anchorMask[8] = {0};
};

class Engine {
public:
  void begin(Profile profile = Profile::Strict15mm);
  void setProfile(Profile profile);
  Profile profile() const;
  void setExpectedId(int idOrMinusOne);
  void clearLock();

  // CV decoder toggle. When true, processFrame() dispatches to a different
  // pipeline that locates the tag via cluster analysis on the 8x8 depth
  // map (k-means k=2, near/far), then divides the NEAR-cluster bounding
  // box into 4x4 cells via majority vote. Avoids the brute-force ROI
  // search of the legacy pipeline, which produces spurious orient+CRC
  // matches on dense bit patterns. Default false (legacy behavior).
  void setUseCV(bool enabled) { useCV_ = enabled; }
  bool useCV() const { return useCV_; }

  // HR spec v2 (DC34-155): decode the anchor/fiducial layout (A3-D13 — 3 corner
  // anchors + 13 data cells, pose-corrected sampling) instead of the legacy
  // orientation-marker grid. Off by default until the collectible catalog is
  // regenerated as anchor tags. Only affects the CV path.
  void setUseAnchor(bool enabled) { useAnchor_ = enabled; }
  // DC34-156 (first-scan-success): window-plurality-with-margin lock toggle.
  // ON = plurality-vote commit; OFF = legacy consecutive-run + 2-clean gate
  // (kept for the hardware A/B). Anchor mode only; default ON.
  void setVoteLock(bool enabled) { voteLockEnabled_ = enabled; }
  bool voteLock() const { return voteLockEnabled_; }
  // Fixed-orientation tags: pin the guard to a known bbox corner (0=TL,1=TR,
  // 2=BL,3=BR) so guard-cell bleed can't steal the pose. -1 = legacy weakest-blob.
  void setFixedGuardCorner(int corner) { fixedGuardCorner_ = (int8_t)corner; }
  int  fixedGuardCorner() const { return fixedGuardCorner_; }
  uint8_t dbgWeakestCorner() const { return dbgWeakest_; }
  bool useAnchor() const { return useAnchor_; }

  // Manual-entry decode (feature/manual-entry-grid, Task 1). Decode a
  // hand-entered 4x4 tag into a collectible id, reusing the anchor/SECDED
  // decoder. userView[r][c] = true where the tag bump is RAISED, in the
  // orientation the USER sees it (TL/TR/BR corners raised, BL flat) -- i.e.
  // X-mirrored vs the decoder's coordinate space. Returns the decoded id
  // (0..127), or -1 if the read is not a valid single-error-correctable tag.
  static int decodeUserGrid(const bool userView[4][4]);
  // Inverse: encode an id into the user-view 4x4 (for verify-first pre-fill).
  static void encodeUserGrid(int id, bool userView[4][4]);

  // Signed tag-relative tilt gradient (mm depth-change per zone), from the
  // depth-plane fit over near zones -> drives the optional tag-relative level
  // bubble. Both ~0 when the badge is flat-on to the tag; 0 when no tag seen.
  void tiltGradient(float &gr, float &gc) const { gr = dbgCvTiltGr_; gc = dbgCvTiltGc_; }

  // Per-sensor FLAT-FIELD calibration. cal[64] = the per-zone depth offset (mm)
  // measured on a flat surface (deviation from a plane fit) -- the sensor's fixed
  // zone-geometry distortion. Subtracted from every frame so a flat reads flat,
  // sharpening the near/far bump threshold (what sparse tags need). Row-major
  // (r*8+c). Persisted by the UI to NVS + reloaded at boot.
  void setZoneCal(const int16_t cal[64]) { memcpy(zoneCal_, cal, sizeof(zoneCal_)); hasCal_ = true; }
  void clearZoneCal() { memset(zoneCal_, 0, sizeof(zoneCal_)); hasCal_ = false; }
  bool hasZoneCal() const { return hasCal_; }
  // Diagnostics from the last finishCalCapture (for the Settings cal modal).
  int lastCalZones() const { return lastCalZones_; }   // # zones covered (of 64)
  int lastCalAvgMm() const { return lastCalAvgMm_; }   // mean captured distance (mm)

  // Flat-field calibration CAPTURE. beginCalCapture() clears any cal and starts
  // averaging frames (aim at a flat matte surface at scan distance). After ~1-2s,
  // finishCalCapture(out) plane-fits the average (removing absolute distance +
  // any hold tilt, leaving the sensor's fixed bowl), stores the per-zone residual
  // as the live cal, and returns it in out[64] for the caller to persist to NVS.
  // Returns false if too few frames/zones were captured.
  void beginCalCapture();
  int  finishCalCapture(int16_t out[64]);   // returns # zones covered; hasZoneCal() true on success
  bool isCalCapturing() const { return calCapturing_; }

  // Optional ID screening hook. Called whenever the engine has a
  // CRC-valid candidate, BEFORE lock-voting begins. Return false to
  // tell the engine "this ID is not a real target" -- the candidate
  // is dropped before the run/lockRun trackers advance, so the
  // progress bar never fills on noise IDs and there's no 12-frame
  // wait while we vote up only to reject post-lock.
  // Pass nullptr to disable screening (default).
  using IdValidator = bool (*)(int id);
  void setIdValidator(IdValidator fn) { idValidator_ = fn; }

  Result processFrame(const int16_t distanceMm64[64],
                      const uint8_t nbTargetDetected64[64],
                      const uint8_t targetStatus64[64],
                      bool oneIsNear);

  // Diagnostic dump of the most recent processFrame's intermediates.
  // Prints raw 8x8 mm/valid, filtered 8x8 mm, best ROI, sampled 4x4 depth,
  // bits, threshold, separation, decode result, lock state. Designed for
  // copy-paste analysis from a serial monitor.
  void dumpDebug(Print &out) const;

  // Read-only accessors to the most recent processFrame()'s raw 8x8 view.
  // Useful for sensor-aiming visualizations (see Minimal.ino's heatmap).
  const int16_t (*lastFilteredMm() const)[8] { return dbgFiltMm_; }
  const bool    (*lastFilteredValid() const)[8] { return dbgFiltValid_; }

private:
  static constexpr uint8_t kFilterWindow = 9;
  static constexpr uint8_t kRoiMin = 6;     // was 4 — exclude tiny ROIs that find spurious matches in tag sub-regions
  static constexpr uint8_t kRoiMax = 8;
  static constexpr uint8_t kMinValidCells = 12;
  static constexpr uint8_t kLockClearBadFrames = 30;
  static constexpr uint8_t kRoiSizeTarget = 7;  // was 6 — match expected tag-fills-FoV alignment at production scan distance

  // Scoring weights
  static constexpr int kScoreCenterWeight = 40;
  static constexpr int kScoreSizeWeight = 0;   // was 30 — don't bias against larger ROIs that better cover the tag
  static constexpr int kScoreSepWeight = 2;
  static constexpr int kScoreBadSepPenalty = 120;

  // CV-mode tunables
  static constexpr int16_t kCvSepMinMm = 8;     // min near/far cluster separation
  static constexpr int16_t kCvSepMaxMm = 30;    // max separation; rejects scenes with no tag-like depth structure
  static constexpr uint8_t kCvBboxMin = 3;      // bbox at least 3 zones in each dim
  static constexpr uint8_t kCvBboxMax = 8;      // bbox at most full FoV
  static constexpr uint8_t kCvMinValidZones = 32; // need at least half the FoV with valid depth
  static constexpr uint8_t kCvMinNearDensityPct = 15; // near cluster must cover ≥15% of bbox cells (catches the orient row)
  static constexpr uint8_t kCvVoteHistory = 5;  // # of past frames to sum vote totals across
  // DC34-154: a frame only counts toward the lock (and the progress bar) if the
  // WEAKEST decoded cell's windowed near/far vote is at least this one-sided
  // (0=50/50, 255=unanimous). Stops confident locks on fragile/marginal reads
  // (the "looks good but decodes wrong" class). KEY CALIBRATION KNOB — tune
  // against the fragile-pattern fixtures + real-tag scans (too high rejects good
  // tags). ~96 ≈ weakest cell must be ~69% one-sided across the 5-frame window.
  static constexpr uint8_t kCellConfidenceFloor = 96;
  // Only cells with at least this many windowed votes constrain the confidence
  // (a 1-2 vote cell is too sparse to judge -- CRC covers its guessed bit), and
  // at least kCellMinJudged such cells must qualify or the read is too sparse to
  // trust. Tunable alongside kCellConfidenceFloor. (kCellMinVotes lowered 4->3:
  // over ~7 covered zones the 16 cells average ~3 votes each, so a floor of 4
  // false-rejected correct anchor decodes -- see the id-16 hardware trace where
  // a valid id=16/CRC-ok read had only 7 cells at >=4 votes but 12 at >=3.)
  static constexpr uint8_t kCellMinVotes = 3;
  static constexpr uint8_t kCellMinJudged = 8;

  // Runtime profile params
  float minSeparationMm_ = 8.0f;
  uint8_t lockRequired_ = 8;

  // Expected ID prior (optional)
  bool useExpectedIdPrior_ = true;
  int expectedId_ = 128;

  // Lock state
  int lockCandidateId_ = -1;
  uint8_t lockRun_ = 0;
  uint8_t lockCleanCount_ = 0;  // DC34-155: # of CLEAN (syn==0) frames seen for the
                                // current candidate. Anchor lock requires >=
                                // kAnchorMinCleanFrames: a marginal-alignment WRONG
                                // read is corrected-only (syn!=0 every frame), so it
                                // never accrues clean evidence and can't lock; a real
                                // tag produces exact-codeword frames and does.
  int trackCandidateId_ = -1;
  uint8_t trackRun_ = 0;
  int lockedId_ = -1;
  uint8_t badFrames_ = 0;

  // DC34-156 window-plurality-with-margin lock (mirrors vote_lock_model.py).
  // The true id dominates the decode window; lock the plurality winner once it
  // clears an evidence floor AND a >=margin over the runner-up. NO clean-frame
  // gate -- clean-ness is captured in the vote WEIGHT (clean=+2), so a corrected-
  // only-but-correct tag still locks. Replaces the consecutive-run + hard-2-clean
  // gate (which starved on corrected reads -> timeouts, and trusted a minority
  // clean-neighbor -> wrong locks). All KNOBS are hardware-tuned (see the A/B).
  static constexpr uint8_t kVoteWindow = 24;         // sliding window length (frames)
  static constexpr uint8_t kVoteEvidenceFloor = 12;  // min weighted votes to commit
  static constexpr uint8_t kVoteMarginX10 = 20;      // winner*10 >= this * runner-up
  static constexpr uint8_t kVoteCleanWeight = 2;     // weight of a clean (syn==0) frame
  static constexpr uint8_t kVoteCorrWeight = 1;      // weight of a corrected (syn!=0 Ok) frame
  bool     voteLockEnabled_ = true;                  // A/B toggle (default vote-lock ON)
  uint8_t  voteVotes_[128] = {0};                    // weighted votes per id, in-window
  uint8_t  voteClean_[128] = {0};                    // # clean frames per id, in-window (telemetry only)
  uint8_t  voteRingId_[kVoteWindow] = {0};           // ring: contributing frame ids
  uint8_t  voteRingW_[kVoteWindow]  = {0};           // ring: their weights (0 = empty slot)
  uint8_t  voteRingPos_ = 0;                         // next ring write index

  // Temporal filter history
  int16_t histMm_[kFilterWindow][8][8];
  bool histValid_[kFilterWindow][8][8];
  uint8_t histWrite_ = 0;
  uint8_t histCount_ = 0;

  // Most recent processFrame() snapshot, populated for dumpDebug().
  int16_t  dbgRawMm_[8][8] = {{0}};
  bool     dbgRawValid_[8][8] = {{false}};
  int16_t  dbgFiltMm_[8][8] = {{0}};
  bool     dbgFiltValid_[8][8] = {{false}};
  uint16_t dbgDepth16_[16] = {0};
  bool     dbgDepth16Valid_[16] = {false};
  int16_t  dbgResidual16_[16] = {0};
  bool     dbgResidualValid_[16] = {false};
  uint8_t  dbgBits16_[16] = {0};
  uint8_t  dbgRoiX_ = 0, dbgRoiY_ = 0, dbgRoiSize_ = 0;
  int      dbgThreshold_ = 0;
  float    dbgSepMm_ = 0.0f;
  bool     dbgResidualMode_ = true;
  bool     dbgUsedMirror_ = false;
  HRCode4x4::DecodeResult dbgDecode_{};
  uint32_t dbgFrameCount_ = 0;

  // Optional ID screening hook (see setIdValidator).
  IdValidator idValidator_ = nullptr;

  // CV mode state and snapshot fields. Populated when useCV_ is true.
  bool     useCV_ = false;
  bool     useAnchor_ = false;   // DC34-155: anchor/fiducial decode (CV path only)
  // Cluster centroids and per-zone classification for the most recent CV frame.
  int16_t  dbgCvNearMm_ = 0;
  int16_t  dbgCvFarMm_ = 0;
  float    dbgCvSepMm_ = 0.0f;
  int      cvExpectedRot_ = -1;        // DC34-155: pose-derived de-rotation for the
                                       // anchor decoder (from the weakest/guard corner);
                                       // -1 until localization sets it. Fixes rotation
                                       // WITHOUT trusting the bleed-prone guard cell.
  float    dbgCvTiltMmPerZone_ = 0.0f; // DC34-155: tag-relative tilt (depth-plane
                                       // gradient over near zones). High => the
                                       // badge is angled to the tag -> the 3-anchor
                                       // affine mis-samples cells (skew misreads).
  float    dbgCvTiltGr_ = 0.0f;        // signed row/col components of that gradient
  float    dbgCvTiltGc_ = 0.0f;        // (mm/zone) -> drives the tag-relative level bubble
  uint8_t  dbgCorner_[4] = {0,0,0,0};  // weighted strength per bbox corner TL,TR,BL,BR (debug)
  uint8_t  dbgMiss_ = 0;               // which corner (0-3) was USED as the guard (debug)
  uint8_t  dbgWeakest_ = 0;            // which corner was WEAKEST (compare to pinned; debug)
  int8_t   fixedGuardCorner_ = 3;      // pinned guard corner (BR, fixed-orientation tags); -1 = weakest-blob
  bool     cvGuardClear_ = false;      // guard corner clearly emptier than anchors -> pose trustworthy
  int16_t  zoneCal_[64] = {0};         // per-zone flat-field baseline (mm), subtracted each frame
  bool     hasCal_ = false;            // a calibration is loaded
  int      lastCalZones_ = 0;          // # zones covered by the last finishCalCapture (UI)
  int16_t  lastCalAvgMm_ = 0;          // mean captured distance (mm) of that capture (UI)
  bool     calCapturing_ = false;      // averaging flat-surface frames for a new cal
  int32_t  calSum_[64] = {0};          // per-zone depth accumulator during capture
  uint16_t calCount_[64] = {0};        // per-zone frame count during capture
  uint8_t  dbgCvNearMask_[8] = {0};   // bit per column of zones tagged NEAR
  uint8_t  dbgCvBboxMinR_ = 0, dbgCvBboxMinC_ = 0;
  uint8_t  dbgCvBboxMaxR_ = 0, dbgCvBboxMaxC_ = 0;
  bool     dbgCvBboxValid_ = false;
  // Per-cell vote totals (NEAR count / total count) for the 4x4 grid.
  uint8_t  dbgCvCellNear_[16] = {0};
  uint8_t  dbgCvCellTotal_[16] = {0};
  // Sliding window of cell votes across recent frames; the actual decode
  // uses the sum across this history to suppress per-frame cell-classification
  // noise on borderline cells.
  uint8_t  cvCellNearHist_[kCvVoteHistory][16] = {{0}};
  uint8_t  cvCellTotalHist_[kCvVoteHistory][16] = {{0}};
  uint8_t  cvHistWrite_ = 0;
  uint8_t  cvHistCount_ = 0;
  const char *dbgCvReject_ = nullptr;  // if non-null, rejection reason

  Profile profile_ = Profile::Strict15mm;

  static int bitCount8(uint8_t v);
  static int16_t medianSmall(int16_t *vals, uint8_t n);

  static bool zoneValid(uint8_t nbTargets, uint8_t targetStatus);
  static void orientFrame(const int16_t distanceMm64[64],
                          const uint8_t nbTargetDetected64[64],
                          const uint8_t targetStatus64[64],
                          int16_t outMm[8][8], bool outValid[8][8]);

  void pushHistory(const int16_t mm8[8][8], const bool valid8[8][8]);
  void median8x8(int16_t outMm[8][8], bool outValid[8][8]) const;

  static bool sampleBilinear8x8(const int16_t mm8[8][8], const bool valid8[8][8],
                                float y, float x, uint16_t &outMm);

  static uint8_t sampleRoi4x4(const int16_t mm8[8][8], const bool valid8[8][8],
                              uint8_t roiX, uint8_t roiY, uint8_t roiSize,
                              uint16_t depth16[16], bool valid16[16]);

  static bool fitPlane4x4(const uint16_t depth16[16], const bool valid16[16],
                          float &a, float &b, float &c0);

  static void detrend4x4(const uint16_t depth16[16], const bool valid16[16],
                         int16_t residual16[16], bool residualValid16[16]);

  static void splitAdaptiveSigned(const int16_t vals16[16], const bool valid16[16],
                                  int &threshold, float &sepMm);

  static void splitAdaptiveUnsigned(const uint16_t vals16[16], const bool valid16[16],
                                    int &threshold, float &sepMm);

  static void bitsFromResidual(const int16_t residual16[16], const bool valid16[16],
                               int threshold, bool oneIsNear, uint8_t bits16[16]);

  static void bitsFromDepth(const uint16_t depth16[16], const bool valid16[16],
                            int threshold, bool oneIsNear, uint8_t bits16[16]);

  static void bitsToGrid(const uint8_t bits16[16], uint8_t g[4][4]);
  static void mirrorGridHorizontal(const uint8_t inG[4][4], uint8_t outG[4][4]);

  static HRCode4x4::DecodeResult decodeWithMirrorFallback(const uint8_t bits16[16],
                                                          bool &usedMirror);

  // DC34-155: decode the anchor layout (A3-D13) from a 4x4 bit grid. Verifies
  // DC34-155: decode the anchor layout "A3-D13". The 3 corner anchors (0,0)(0,3)
  // (3,0) must be near; the 12 non-guard cells carry a SECDED(12,7) codeword over
  // a 7-bit id (corrects 1 cell, rejects 2). Rotation is fixed by expectedRot
  // (the pose-derived de-rotation from the localization's weakest/guard corner);
  // with expectedRot>=0 ONLY that rotation is decoded. expectedRot<0 falls back
  // to a 4-rotation search that returns only a UNIQUE clean id (never a guessed
  // tiebreak). Rotation-only (no mirror -- a mirror aliases ids). Returns Ok+id
  // or a non-Ok status. Must match build_anchor_grid() in hm_codegen.py.
  static HRCode4x4::DecodeResult decodeAnchorBits(const uint8_t bits16[16], int expectedRot);

  int scoreCandidate(const HRCode4x4::DecodeResult &decoded,
                     uint8_t validCells,
                     float separationMm,
                     bool usedMirror,
                     uint8_t roiX,
                     uint8_t roiY,
                     uint8_t roiSize) const;

  Prompt buildPrompt(bool ok, bool locked, HRCode4x4::DecodeStatus status) const;

  void clearLockInternal();

  // CV pipeline: alternative to the brute-force ROI search. Clusters the
  // 8x8 depths into NEAR/FAR via 1D k-means k=2, finds the bounding box
  // of NEAR-cluster zones, divides the bbox into 4x4 cells, and decodes
  // via majority vote per cell. Populates the dbgCv* snapshot fields.
  // Returns the same Result struct as the legacy pipeline.
  Result processFrameCV(const int16_t mm8[8][8], const bool valid8[8][8],
                        bool oneIsNear);
};

} // namespace HRScan
