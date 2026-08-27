#pragma once
// audio_driver.h — Header-only audio driver for Clip-Boy badge
//
// Uses audio-tools library (selective includes to avoid namespace conflicts
// with AsyncTCP/ClipBoy). Provides:
//   - I2S tone generation for theremin
//   - MP3 playback from PROGMEM via minimp3 decoder (public-domain / CC0;
//     replaced libhelix to drop its GPL/RPSL copyleft from the build)
//
// Included from ui_test.ino AFTER clipboy_pins.h.

// Include config first so USE_URL_ARDUINO gets defined, then kill it
// before CoreAudio.h pulls in AudioHttp → esp_http_client → http_parser
// which conflicts with ESPAsyncWebServer's HTTP_DELETE/HTTP_GET defines.
#include <AudioToolsConfig.h>
#undef USE_URL_ARDUINO
#undef USE_AUDIO_SERVER
#include <AudioTools.h>
// minimp3 directly (public domain / CC0). We do NOT use audio-tools' streaming
// MP3 wrapper -- its chunked decode() spins forever on a partial frame. Instead
// each short sound is decoded whole to PCM (aud_mp3_decode) and streamed raw.
// MINIMP3_IMPLEMENTATION is compiled once in minimp3_impl.cpp.
#include "minimp3.h"
// Streaming MP3 decoder core (shared with the host proof scripts/tests/
// mp3_stream_host.c). Lets multi-minute radio beds decode frame-by-frame from a
// file instead of whole-into-PSRAM (78 s @ 8 kHz would be ~14 MB). CC0.
#include <FS.h>
#include "mp3_stream.h"
#include <SparkFun_VL53L5CX_Library.h>
#include <ClipBoyTheremin.h>

#include "clipboy_pins.h"

// Sound data (PROGMEM)
#include "snd_scan_search_mp3.h"
#include "snd_scanning_mp3.h"        // Jeff Kaale via upbeat.io -- looping scan bed (trial)
#include "snd_scan_ok_mp3.h"
#include "snd_scan_stop_mp3.h"

using namespace audio_tools;

// ─── State ─────────────────────────────────────────────────────────────────

static I2SStream                       aud_i2s;
static VolumeStream                    aud_volume(aud_i2s);

// ClipBoyTheremin library instance
ClipTheremin::Theremin                 aud_theremin;
static volatile bool                   aud_theremin_active = false;

// SB3 -- a UI tap click used to KILL a running theremin permanently.
//
// audio_mp3_play() pre-empts the theremin because both share the single I2S render
// stream, but audio_theremin_stop() is a bare flag clear with no re-arm anywhere except
// theremin_enable(). So any tap on a button-class widget (the always-present status-bar
// FL / ? buttons) silenced the synth for good, while the poll timer and sensor stayed up
// -- the button still read "Disable", the status bar still said active, and the four bars
// FROZE at their last values. Reads as a hang, not a stop. Owner-confirmed on hardware
// 2026-07-25 (flashlight lit + tone died + bars frozen).
//
// Two flags now, because ONE cannot express the difference:
//   aud_theremin_want   = user-level intent (Enable/Disable). Owned by core 1.
//   aud_theremin_active = is the core-0 task rendering it right now.
// A short clip clears `active` and leaves `want` set; the audio task restores `active`
// from `want` once no clip owns the stream. want=true + active=false outside a clip is
// the SB3 signature, which is what makes it testable rather than audible-only.
//
// CB_THEREMIN_DUCK_CLICK 0 = taps are simply SILENT while the theremin runs (audio_play_click
// early-returns before it ever reaches audio_mp3_play, so the synth is untouched).
// 1 = duck-and-resume: the click plays and the core-0 task restores the synth when the clip
// drains (~100 ms gap), matching the contract the geiger already honors.
// **DEFAULT IS 0 — owner's call after hearing both on hardware (2026-07-26): "taps going silent
// is the right move."** The audible cost is small by construction: this only mutes the
// status-bar FL and ? buttons, because sliders are excluded from the click hook and the voice
// dropdowns are locked while the theremin runs. Flip to 1 for duck-and-resume; both paths are
// implemented and tested, and SB3's regression test passes either way (it asserts the synth is
// still running after a tap, which is true of both).
#ifndef CB_THEREMIN_DUCK_CLICK
  #define CB_THEREMIN_DUCK_CLICK 0
#endif
static volatile bool                   aud_theremin_want   = false;

// MP3 playback state — each sound is decoded whole to raw PCM (stereo int16 @
// AUD_RATE) on play, then the task streams it to I2S. No streaming decoder in
// the hot path.
#define AUD_RATE 44100
static int16_t * volatile              aud_mp3_pcm     = NULL;  // PSRAM, stereo interleaved
static volatile size_t                 aud_mp3_frames  = 0;     // stereo frames
static volatile size_t                 aud_mp3_pos     = 0;     // current stereo frame
static volatile bool                   aud_mp3_loop    = false;
static volatile bool                   aud_mp3_active  = false;
// false = aud_mp3_pcm is BORROWED from a caller that owns and outlives it, so
// audio_mp3_stop() must not free it. See audio_pcm16_play().
static volatile bool                   aud_mp3_owns_pcm = true;

static bool   aud_initialized = false;

// Mutexes for cross-core safety
static SemaphoreHandle_t aud_mp3_mutex     = NULL;  // protects MP3 state transitions
static SemaphoreHandle_t aud_theremin_mutex = NULL;  // protects theremin feed/render

// MP3 task on core 0
static TaskHandle_t aud_mp3_task_handle = NULL;

// ─── Synthesized geiger ticks ──────────────────────────────────────────────
// Renders damped-sine click transients directly to I2S, Poisson-ish timing
// driven by aud_geiger_rate (clicks/sec). Replaces the MP3-based geiger that
// suffered from decoder-reinit pops when switching intensity segments.
static volatile bool     aud_geiger_active = false;
static volatile uint16_t aud_geiger_rate   = 0;
// Owned exclusively by the audio task — no mutex needed:
static uint32_t aud_geiger_phase_samp = 0;   // samples into current click (0 = idle)
static float    aud_geiger_phase_rad  = 0.0f;
static uint32_t aud_geiger_next_samp  = 0;   // samples until next click fires

static const uint32_t GEIGER_CLICK_SAMPLES = 441;      // ~10ms @ 44.1kHz
static const float    GEIGER_CLICK_FREQ    = 2500.0f;  // Hz — crisp tik
static const float    GEIGER_DECAY         = 0.012f;   // per-sample exp decay
static const float    GEIGER_AMPLITUDE     = 24000.0f; // peak int16 ~73%

static size_t audio_geiger_render(int16_t *buf, size_t frames) {
    const float FREQ_INC = 2.0f * 3.14159265f * GEIGER_CLICK_FREQ / 44100.0f;
    const uint16_t rate = aud_geiger_rate;  // snapshot — may update between frames
    for (size_t i = 0; i < frames; i++) {
        int16_t s = 0;
        // Render active click (damped sine)
        if (aud_geiger_phase_samp > 0) {
            float env = expf(-(float)(aud_geiger_phase_samp - 1) * GEIGER_DECAY);
            s = (int16_t)(env * sinf(aud_geiger_phase_rad) * GEIGER_AMPLITUDE);
            aud_geiger_phase_rad += FREQ_INC;
            aud_geiger_phase_samp++;
            if (aud_geiger_phase_samp > GEIGER_CLICK_SAMPLES) aud_geiger_phase_samp = 0;
        }
        // Schedule next click
        if (rate > 0) {
            if (aud_geiger_next_samp == 0) {
                aud_geiger_phase_samp = 1;
                aud_geiger_phase_rad  = 0.0f;
                uint32_t mean = 44100u / rate;
                if (mean < 2) mean = 2;
                // 50%-150% of mean for Poisson-like jitter
                aud_geiger_next_samp = (mean / 2) + (esp_random() % mean);
            } else {
                aud_geiger_next_samp--;
            }
        }
        buf[i * 2]     = s;
        buf[i * 2 + 1] = s;
    }
    return frames;
}

static void audio_geiger_start(void) {
    aud_geiger_phase_samp = 0;
    aud_geiger_next_samp  = 0;
    aud_geiger_active     = true;
}

static void audio_geiger_stop(void) {
    aud_geiger_active     = false;
    aud_geiger_rate       = 0;
    aud_geiger_phase_samp = 0;
    aud_geiger_next_samp  = 0;
}

static void audio_geiger_set_rate(int rate_per_sec) {
    if (rate_per_sec < 0)   rate_per_sec = 0;
    if (rate_per_sec > 400) rate_per_sec = 400;  // cap — beyond this it's buzz anyway
    aud_geiger_rate = (uint16_t)rate_per_sec;
}

// ─── MP3 -> PCM decode (minimp3) ────────────────────────────────────────────
// Decode a (PROGMEM) MP3 whole to stereo-interleaved int16 PCM at AUD_RATE.
// Bypasses audio-tools' streaming wrapper. Returns a ps_malloc'd buffer + the
// stereo frame count, or NULL. Caller frees with free().
static int16_t *aud_mp3_decode(const uint8_t *mp3, size_t len, size_t *outFrames) {
    *outFrames = 0;
    mp3dec_t *dec   = (mp3dec_t *)malloc(sizeof(mp3dec_t));
    int16_t  *frame = (int16_t *)malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t));
    if (!dec || !frame) { free(dec); free(frame); return NULL; }
    mp3dec_init(dec);

    size_t   cap  = AUD_RATE;                 // ~1s mono to start
    int16_t *mono = (int16_t *)ps_malloc(cap * sizeof(int16_t));
    if (!mono) { free(dec); free(frame); return NULL; }
    size_t mn = 0;
    int    srcRate = AUD_RATE;

    mp3dec_frame_info_t info;
    const uint8_t *p = mp3;
    int remain = (int)len;
    while (remain > 0) {
        int samples = mp3dec_decode_frame(dec, p, remain, frame, &info);
        if (info.frame_bytes <= 0) break;          // no further complete frame
        p += info.frame_bytes; remain -= info.frame_bytes;
        if (samples == 0) continue;                // skipped (ID3/junk)
        srcRate = info.hz > 0 ? info.hz : AUD_RATE;
        uint8_t ch = info.channels > 0 ? info.channels : 1;
        if (mn + (size_t)samples > cap) {
            size_t ncap = (mn + samples) * 2;
            int16_t *nb = (int16_t *)ps_realloc(mono, ncap * sizeof(int16_t));
            if (!nb) { free(dec); free(frame); free(mono); return NULL; }
            mono = nb; cap = ncap;
        }
        for (int i = 0; i < samples; i++) {
            mono[mn++] = (ch == 1) ? frame[i]
                       : (int16_t)(((int32_t)frame[i * ch] + frame[i * ch + 1]) / 2);
        }
    }
    free(dec); free(frame);
    if (mn == 0) { free(mono); return NULL; }

    // mono@srcRate -> stereo@AUD_RATE (linear interp; passthrough when equal)
    size_t outF = (srcRate == AUD_RATE) ? mn
                : (size_t)(((uint64_t)mn * AUD_RATE) / (uint32_t)srcRate);
    if (outF == 0) outF = 1;
    int16_t *out = (int16_t *)ps_malloc(outF * 2 * sizeof(int16_t));
    if (!out) { free(mono); return NULL; }
    if (srcRate == AUD_RATE) {
        for (size_t i = 0; i < mn; i++) { out[i*2] = mono[i]; out[i*2+1] = mono[i]; }
    } else {
        for (size_t i = 0; i < outF; i++) {
            float   sp = (float)i * (float)srcRate / (float)AUD_RATE;
            size_t  s0 = (size_t)sp;
            float   fr = sp - (float)s0;
            int16_t a  = mono[s0 < mn ? s0 : mn - 1];
            int16_t b  = mono[(s0 + 1) < mn ? s0 + 1 : mn - 1];
            int16_t v  = (int16_t)((float)a + ((float)b - (float)a) * fr);
            out[i*2] = v; out[i*2+1] = v;
        }
    }
    free(mono);
    *outFrames = outF;
    return out;
}

// ─── UI progress tone (rising sine for the screensaver unlock hold) ─────────
// A soft sine whose pitch the UI updates live to track a progress gesture.
// Rendered sample-by-sample like the geiger ticks, mixed through aud_volume so
// it honors the user's volume + mute. Amplitude ramps in (~6 ms fade-in) / out
// (~12 ms fade-out) to avoid clicks/pops. Sounds ONLY while explicitly active.
#define AUD_TONE_PEAK 11000.0f          // ~0.34 full-scale: present but gentle
static volatile bool    aud_tone_active   = false;
static volatile float   aud_tone_freq     = 440.0f;
static volatile float   aud_tone_target   = 0.0f;   // target amplitude 0..1
static volatile int32_t aud_tone_autostop = -1;     // frames until auto fade; <0 = none
static float aud_tone_amp   = 0.0f;     // current amplitude (audio-task side)
static float aud_tone_phase = 0.0f;

static size_t audio_tone_render(int16_t *buf, size_t frames) {
    const float dphase = 6.2831853f * aud_tone_freq / (float)AUD_RATE;
    const float atk = 1.0f / (0.006f * (float)AUD_RATE);   // ~6 ms fade-in
    const float rel = 1.0f / (0.012f * (float)AUD_RATE);   // ~12 ms fade-out
    for (size_t i = 0; i < frames; i++) {
        if (aud_tone_autostop == 0) { aud_tone_target = 0.0f; aud_tone_autostop = -1; }
        else if (aud_tone_autostop > 0) aud_tone_autostop--;
        float tg = aud_tone_target;
        if (aud_tone_amp < tg)      { aud_tone_amp += atk; if (aud_tone_amp > tg) aud_tone_amp = tg; }
        else if (aud_tone_amp > tg) { aud_tone_amp -= rel; if (aud_tone_amp < tg) aud_tone_amp = tg; }
        int16_t v = (int16_t)(sinf(aud_tone_phase) * aud_tone_amp * AUD_TONE_PEAK);
        aud_tone_phase += dphase;
        if (aud_tone_phase > 6.2831853f) aud_tone_phase -= 6.2831853f;
        buf[i * 2] = v; buf[i * 2 + 1] = v;
    }
    return frames;
}

// ─── Radio tuning static (procedural) ───────────────────────────────────────
// Broadband hiss whose loudness the radio UI drives from how far off-tune the
// dial is: loud off-band, fading to silence as you lock. No storage, seamless
// loop, and — unlike an MP3 — it tracks the dial continuously (this IS the
// DC34-133 "static fades to clean at lock" affordance). The UI sets
// aud_static_level (0..1); the render slews toward it so there are no clicks.
#define AUD_STATIC_PEAK 9000.0f
static volatile bool  aud_static_active = false;
static volatile float aud_static_level  = 0.0f;   // UI target 0..1
static float          aud_static_amp    = 0.0f;   // smoothed (audio-task side)
static uint32_t       aud_noise_state   = 0x1234567u;
static inline int aud_noise_next(void) {           // xorshift32 -> [-32768,32767]
    uint32_t x = aud_noise_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    aud_noise_state = x;
    return (int)((int32_t)(x & 0xFFFFu) - 32768);
}
static size_t audio_static_render(int16_t *buf, size_t frames) {
    const float slew = 1.0f / (0.010f * (float)AUD_RATE);   // ~10ms toward target
    for (size_t i = 0; i < frames; i++) {
        float tg = aud_static_level;
        if      (aud_static_amp < tg) { aud_static_amp += slew; if (aud_static_amp > tg) aud_static_amp = tg; }
        else if (aud_static_amp > tg) { aud_static_amp -= slew; if (aud_static_amp < tg) aud_static_amp = tg; }
        int16_t n = (int16_t)(((float)aud_noise_next() / 32768.0f) * aud_static_amp * AUD_STATIC_PEAK);
        buf[i * 2] = n; buf[i * 2 + 1] = n;
    }
    return frames;
}
static void audio_static_start(void)          { aud_static_active = true; }
static void audio_static_stop(void)           { aud_static_active = false; aud_static_amp = 0.0f; aud_static_level = 0.0f; }
static void audio_static_set_level(float lv)  { if (lv < 0) lv = 0; else if (lv > 1) lv = 1; aud_static_level = lv; }

// ─── Sonar ping (procedural, HR scanner) ────────────────────────────────────
// A periodic downward-chirp ping with a fast rise + exponential decay,
// repeating at a CONSTANT peak so it never fades across cycles. (The old
// scan-search.mp3 was a one-shot that decayed to silence, so looping it sounded
// like it kept getting fainter.) No storage.
#define AUD_SONAR_PERIOD 1.15f            // seconds between pings
#define AUD_SONAR_PEAK   10000.0f
static volatile bool aud_sonar_active = false;
static float         aud_sonar_t      = 0.0f;   // seconds into the current cycle
static float         aud_sonar_phase  = 0.0f;
static size_t audio_sonar_render(int16_t *buf, size_t frames) {
    const float dt = 1.0f / (float)AUD_RATE;
    for (size_t i = 0; i < frames; i++) {
        float env = 0.0f;
        if (aud_sonar_t < 0.6f) {                       // audible portion of the cycle
            float rise = aud_sonar_t < 0.005f ? (aud_sonar_t / 0.005f) : 1.0f;  // ~5ms rise
            env = rise * expf(-aud_sonar_t / 0.12f);                            // ~120ms decay
        }
        float sweep = aud_sonar_t < 0.4f ? (aud_sonar_t / 0.4f) : 1.0f;
        float f = 900.0f - 420.0f * sweep;              // 900 -> 480 Hz downward chirp
        aud_sonar_phase += 6.2831853f * f * dt;
        if (aud_sonar_phase > 6.2831853f) aud_sonar_phase -= 6.2831853f;
        int16_t v = (int16_t)(sinf(aud_sonar_phase) * env * AUD_SONAR_PEAK);
        buf[i * 2] = v; buf[i * 2 + 1] = v;
        aud_sonar_t += dt;
        if (aud_sonar_t >= AUD_SONAR_PERIOD) aud_sonar_t = 0.0f;   // constant-peak restart
    }
    return frames;
}
static void audio_sonar_start(void) { aud_sonar_t = 0.0f; aud_sonar_phase = 0.0f; aud_sonar_active = true; }
static void audio_sonar_stop(void)  { aud_sonar_active = false; }

#ifdef RADIO_PCM_TEST
// ─── Radio PCM streaming spike (DC34-129/136) ──────────────────────────────
// Streams mono 8-bit G.711 mu-law from flash: expand -> int16 (256-entry
// table), linear-upsample rate->AUD_RATE, duplicate mono->stereo, write in
// small chunks. NEVER decode-whole (a few minutes of int16 PCM blows PSRAM) —
// this proves the streaming path the radio music beds will use. Storage here is
// PROGMEM; the shipping version reads the same loop from a littlefs file.
static const uint8_t * volatile aud_pcm_data   = NULL;   // mu-law, flash
static volatile uint32_t        aud_pcm_len    = 0;      // mu-law sample count
static volatile uint32_t        aud_pcm_pos    = 0;      // current sample
static volatile uint32_t        aud_pcm_rate   = 11025;  // source Hz
static volatile bool            aud_pcm_loop   = false;
static volatile bool            aud_pcm_active = false;
static int16_t aud_ulaw_tbl[256];                        // G.711 expand table
// Size = max_chunk(256 src samples) * max_upsample(8) * 2 (stereo). The task
// loops fill exactly this when chunk==256 && up==8, so the write index `m` hits
// the last slot with no spare. INVARIANT: keep chunk clamped <=256 AND up
// clamped <=8 (both enforced in the branches) or this overflows -- if you raise
// either clamp, grow this buffer to match.
static int16_t aud_pcm_mix[256 * 8 * 2];

// littleFS streaming variant (perf validation): same upsample/output path, but
// the mu-law source is read from a littlefs file in a read-ahead buffer instead
// of a flash pointer. Instrumented for read latency + underrun risk.
#include <LittleFS.h>
extern "C" {   // the asset .c is compiled as C -> C linkage
    extern const uint8_t  radio_test_pcm_ulaw[];
    extern const uint32_t radio_test_pcm_len;
    extern const uint32_t radio_test_pcm_rate;
}
static File              aud_lfs_file;
static volatile bool     aud_lfs_active   = false;
static volatile bool     aud_lfs_loop     = false;
static uint32_t          aud_lfs_rate     = 11025;
static uint8_t           aud_lfs_buf[4096];              // mu-law read-ahead
static size_t            aud_lfs_buf_len  = 0;
static size_t            aud_lfs_buf_pos  = 0;
static uint32_t          aud_lfs_read_max_us = 0;        // perf: worst read
static uint64_t          aud_lfs_read_tot_us = 0;        // perf: sum
static uint32_t          aud_lfs_read_cnt    = 0;
static uint32_t          aud_lfs_risky       = 0;        // reads > 40ms (gap risk)
static uint32_t          aud_lfs_stat_ms     = 0;

// Standard G.711 mu-law -> linear16 expansion (matches ffmpeg pcm_mulaw).
static int16_t aud_ulaw_expand(uint8_t u) {
    u = ~u;
    int t = ((u & 0x0f) << 3) + 0x84;
    t <<= (u & 0x70) >> 4;
    return (u & 0x80) ? (int16_t)(0x84 - t) : (int16_t)(t - 0x84);
}
static void aud_ulaw_init(void) {
    for (int i = 0; i < 256; i++) aud_ulaw_tbl[i] = aud_ulaw_expand((uint8_t)i);
}

static void audio_mp3_stop(void);   // defined later; needed by audio_pcm_play

static void audio_pcm_stop(void) {
    bool was = aud_pcm_active || aud_lfs_active || aud_pcm_data || (bool)aud_lfs_file;
    if (!was) return;
    aud_pcm_active = false;
    aud_lfs_active = false;
    vTaskDelay(pdMS_TO_TICKS(30));   // let the task exit its branch before we free
    aud_pcm_data = NULL;
    aud_pcm_len = aud_pcm_pos = 0;
    aud_pcm_loop = false;
    if (aud_lfs_file) {
        if (aud_lfs_read_cnt)
            CB_LOGF("[LFS] SUMMARY reads=%u maxRead=%uus avgRead=%uus risky(>40ms)=%u\n",
                          (unsigned)aud_lfs_read_cnt, (unsigned)aud_lfs_read_max_us,
                          (unsigned)(aud_lfs_read_tot_us / aud_lfs_read_cnt),
                          (unsigned)aud_lfs_risky);
        aud_lfs_file.close();
    }
    aud_lfs_buf_len = aud_lfs_buf_pos = 0;
}

// Seed /radio_test.pcm on littlefs from the PROGMEM clip (once, if absent or
// wrong size). Lets us test the real littlefs read path before the
// littlefs-image flashing pipeline (DC34-130) exists.
static bool radio_pcm_seed_lfs(const char *path) {
    uint32_t want = radio_test_pcm_len;
    File ex = LittleFS.open(path, "r");
    if (ex) { size_t sz = ex.size(); ex.close(); if (sz == want) return true; }
    CB_LOGF("[LFS] seeding %s (%u bytes)...\n", path, (unsigned)want);
    File f = LittleFS.open(path, "w");
    if (!f) { CB_LOGLN("[LFS] seed open failed"); return false; }
    uint32_t t0 = millis();
    for (uint32_t off = 0; off < want; ) {
        uint32_t n = (want - off < 4096) ? (want - off) : 4096;
        if (f.write(radio_test_pcm_ulaw + off, n) != n) {
            CB_LOGLN("[LFS] seed write short (FS full?)"); f.close(); return false;
        }
        off += n;
    }
    f.close();
    CB_LOGF("[LFS] seeded %u bytes in %lu ms\n",
                  (unsigned)want, (unsigned long)(millis() - t0));
    return true;
}

// Stream a mu-law clip FROM littlefs (the production read path).
static void audio_pcm_play_lfs(const char *path, bool loop) {
    if (!aud_initialized) return;
    if (aud_mp3_active) audio_mp3_stop();
    audio_pcm_stop();
    aud_lfs_file = LittleFS.open(path, "r");
    if (!aud_lfs_file) { CB_LOGF("[LFS] open %s failed\n", path); return; }
    aud_lfs_rate = radio_test_pcm_rate;
    aud_lfs_buf_len = aud_lfs_buf_pos = 0;
    aud_lfs_loop = loop;
    aud_lfs_read_max_us = aud_lfs_read_cnt = aud_lfs_risky = 0;
    aud_lfs_read_tot_us = 0;
    aud_lfs_stat_ms = millis();
    aud_lfs_active = true;   // set last
    CB_LOGF("[LFS] stream %s (%u B) @ %u Hz%s\n", path,
                  (unsigned)aud_lfs_file.size(), (unsigned)aud_lfs_rate,
                  loop ? " loop" : "");
}

// Convenience for the test triggers: seed if needed, then stream from littlefs.
static void radio_pcm_test_lfs(void) {
    if (radio_pcm_seed_lfs("/radio_test.pcm"))
        audio_pcm_play_lfs("/radio_test.pcm", false);
}

// Play a mu-law clip from flash. Mutually exclusive with mp3/theremin via the
// task's if/else-if ordering (only one source renders per iteration).
static void audio_pcm_play(const uint8_t *ulaw, uint32_t len, uint32_t rate, bool loop) {
    if (!aud_initialized || !ulaw || !len) return;
    if (aud_mp3_active) audio_mp3_stop();
    if (aud_pcm_active) audio_pcm_stop();
    aud_pcm_data = ulaw; aud_pcm_len = len; aud_pcm_rate = rate;
    aud_pcm_pos = 0; aud_pcm_loop = loop;
    aud_pcm_active = true;   // set last
    CB_LOGF("[PCM] play %u samples @ %u Hz (~%.1fs)%s\n",
                  (unsigned)len, (unsigned)rate, (float)len / (float)rate,
                  loop ? " loop" : "");
}
#endif // RADIO_PCM_TEST

// ─── Streaming MP3 (production radio path) ──────────────────────────────────
// Decode a multi-minute MP3 frame-by-frame from a file (SD or LittleFS) or a
// PROGMEM buffer, resampling mono@srcHz -> stereo@AUD_RATE on the fly. Fixed
// memory: a few-KB compressed window (inside mp3s_t) + one frame's worth of
// resampled output. Proven byte-identical to whole-file decode + always
// terminating by scripts/tests/mp3_stream_host.c. Runs in the core-0 audio task
// (which is why its stack is enlarged — minimp3 scratch is ~6-7 KB).
static mp3s_t          aud_mp3s;                  // streaming decoder state
static File            aud_mp3s_file;             // source when streaming from a FS
static const uint8_t * aud_mp3s_prog     = NULL;  // source when streaming from PROGMEM
static uint32_t        aud_mp3s_prog_len = 0, aud_mp3s_prog_pos = 0;
static volatile bool   aud_mp3s_active   = false;
// Is the streaming decoder actually producing audio right now? Used by the screensaver
// inhibit so "a clip is playing" is derived from the ENGINE rather than from a UI flag that
// could latch true after something else silenced the stream.
static inline bool audio_mp3_stream_is_playing(void) { return aud_mp3s_active; }
static volatile bool   aud_mp3s_loop     = false;
static bool            aud_mp3s_from_file = false;
static float           aud_mp3s_ratio    = 0.0f;  // srcHz/AUD_RATE (0 = unknown yet)
static float           aud_mp3s_phase    = 0.0f;  // fractional resampler position
static int16_t         aud_mp3s_prev     = 0;     // last source sample (interp carry)
// Worst case one MP3 frame (1152 mono samples) upsampled from the lowest rate we
// support (8 kHz -> 44.1 kHz = ~5.5x) => ~6350 stereo pairs. Sized with margin.
static const size_t    AUD_MP3S_OUT_N = 13312;   // element count (was a fixed array)
static int16_t        *aud_mp3s_out = NULL;       // ps_malloc'd in audio_init (PSRAM)

// Pull callbacks for mp3_stream. They return 0 at EOF; loop is handled in the
// task (re-seek + re-init) so the decoder always starts clean at the loop point.
static size_t aud_mp3s_read_file(void *ctx, uint8_t *dst, size_t want) {
    (void)ctx;
    if (!aud_mp3s_file) return 0;
    int r = aud_mp3s_file.read(dst, want);
    return (r > 0) ? (size_t)r : 0;
}
static size_t aud_mp3s_read_prog(void *ctx, uint8_t *dst, size_t want) {
    (void)ctx;
    uint32_t rem = aud_mp3s_prog_len - aud_mp3s_prog_pos;
    size_t n = (want < rem) ? want : rem;
    if (n) { memcpy(dst, aud_mp3s_prog + aud_mp3s_prog_pos, n); aud_mp3s_prog_pos += (uint32_t)n; }
    return n;
}

static void aud_mp3s_reset_decoder(void) {
    if (aud_mp3s_from_file) mp3s_init(&aud_mp3s, aud_mp3s_read_file, NULL);
    else                    mp3s_init(&aud_mp3s, aud_mp3s_read_prog, NULL);
    aud_mp3s_ratio = 0.0f; aud_mp3s_phase = 0.0f; aud_mp3s_prev = 0;
}

static void audio_mp3_stream_stop(void) {
    if (!aud_mp3s_active && !aud_mp3s_file && !aud_mp3s_prog) return;
    aud_mp3s_active = false;
    vTaskDelay(pdMS_TO_TICKS(30));   // let the task leave its branch before we free
    if (aud_mp3s_file) aud_mp3s_file.close();
    aud_mp3s_prog = NULL; aud_mp3s_prog_len = aud_mp3s_prog_pos = 0;
    aud_mp3s_loop = false;
}

static void audio_mp3_stop(void);   // fwd (defined later)

// Stream an MP3 from any FS (SD or LittleFS). Mutually exclusive with the
// decode-whole MP3 path and with any prior stream.
static void audio_mp3_stream_play_file(fs::FS &fs, const char *path, bool loop) {
    if (!aud_initialized) return;
    if (aud_mp3_active) audio_mp3_stop();
    audio_mp3_stream_stop();
    aud_mp3s_file = fs.open(path, "r");
    if (!aud_mp3s_file) { CB_LOGF("[MP3S] open %s failed\n", path); return; }
    aud_mp3s_from_file = true; aud_mp3s_loop = loop;
    aud_mp3s_reset_decoder();
    aud_mp3s_active = true;   // set last
    CB_LOGF("[MP3S] stream %s (%u B)%s\n", path,
                  (unsigned)aud_mp3s_file.size(), loop ? " loop" : "");
}

// Stream an MP3 straight out of a PROGMEM buffer (the MVP path — no littlefs
// image needed; a single firmware flash carries the clip).
static void audio_mp3_stream_play_progmem(const uint8_t *data, uint32_t len, bool loop) {
    if (!aud_initialized || !data || !len) return;
    if (aud_mp3_active) audio_mp3_stop();
    audio_mp3_stream_stop();
    aud_mp3s_prog = data; aud_mp3s_prog_len = len; aud_mp3s_prog_pos = 0;
    aud_mp3s_from_file = false; aud_mp3s_loop = loop;
    aud_mp3s_reset_decoder();
    aud_mp3s_active = true;
    CB_LOGF("[MP3S] stream PROGMEM (%u B)%s\n", (unsigned)len, loop ? " loop" : "");
}

// Single core-0 audio renderer. ONE source plays at a time, by a fixed PRIORITY
// if/else-if chain: mp3 (incl. UI tap/click sounds) > pcm/littlefs stream >
// theremin > geiger > UI tone > idle. There is no mixer.
//
// Emergent "ducking for free": a higher-priority SHORT sound (a ~80ms tap click)
// preempts a lower one, and because the lower source's state is just SKIPPED (not
// torn down — e.g. a streaming file handle + buffer position are preserved), it
// RESUMES from the exact same point when the click ends. For short sounds this is
// indistinguishable from ducking, with zero mixing cost. The limit: a LONG source
// (theremin/geiger) fully SUPPRESSES anything below it for its whole duration
// (silence, not duck-under). This is why "radio in the background" needs no mixer
// for nav/taps but would need real mixing to coexist with theremin/geiger
// (DC34-137 / docs/radio-pcm-spike.md).
static void aud_mp3_task(void *param) {
    (void)param;
    for (;;) {
        if (aud_mp3_active && aud_mp3_pcm) {
            xSemaphoreTake(aud_mp3_mutex, portMAX_DELAY);
            if (!aud_mp3_active || !aud_mp3_pcm) {
                // Stopped by audio_mp3_stop() while we waited for the mutex
                xSemaphoreGive(aud_mp3_mutex);
                vTaskDelay(1);
                continue;
            }
            size_t   pos    = aud_mp3_pos;
            size_t   frames = aud_mp3_frames;
            int16_t *pcm    = aud_mp3_pcm;
            size_t   avail  = (pos < frames) ? (frames - pos) : 0;
            size_t   chunk  = (avail < 256) ? avail : 256;   // stereo frames
            aud_mp3_pos += chunk;
            if (aud_mp3_pos >= frames) {
                if (aud_mp3_loop) aud_mp3_pos = 0;
                else              aud_mp3_active = false;
            }
            xSemaphoreGive(aud_mp3_mutex);
            if (chunk > 0) {
                // Write from a SCRATCH copy, not the persistent PCM buffer:
                // VolumeStream scales its input IN PLACE, so writing the source
                // directly re-attenuates it every loop -> the sound fades out across
                // cycles (only shows on a LOOP, which replays the same buffer; one-
                // shots and the fresh-per-frame stream path are unaffected). Copy so
                // only the throwaway scratch is scaled. stereo int16 = 4 bytes/frame.
                static int16_t scr[256 * 2];   // max chunk = 256 stereo frames
                memcpy(scr, pcm + pos * 2, chunk * 4);
                aud_volume.write((uint8_t *)scr, chunk * 4);
            }
            vTaskDelay(1);  // yield briefly
        }
        else if (aud_mp3s_active) {
            // Streaming MP3: decode one frame (mono @ srcHz), fractional-resample
            // to stereo @ AUD_RATE, write. Lower priority than the decode-whole
            // MP3 path, so a UI tap click preempts-and-resumes this for free (the
            // stream's file position + decoder state are preserved, not torn down).
            int16_t mono[MINIMP3_MAX_SAMPLES_PER_FRAME / 2 + 8];
            int n = mp3s_next(&aud_mp3s, mono);
            if (n <= 0) {                                   // end of stream
                if (aud_mp3s_loop) {
                    if (aud_mp3s_from_file) { if (aud_mp3s_file) aud_mp3s_file.seek(0); }
                    else                     aud_mp3s_prog_pos = 0;
                    aud_mp3s_reset_decoder();               // clean decoder at loop point
                } else {
                    aud_mp3s_active = false;
                }
                vTaskDelay(1);
            } else {
                if (aud_mp3s_ratio <= 0.0f) {               // learn rate from 1st frame
                    int hz = aud_mp3s.hz > 0 ? aud_mp3s.hz : AUD_RATE;
                    aud_mp3s_ratio = (float)hz / (float)AUD_RATE;
                }
                const size_t CAP = AUD_MP3S_OUT_N - 2;   // pointer now; use element count
                size_t m = 0;
                for (int i = 0; i < n; i++) {
                    int16_t cur = mono[i];
                    // emit output samples until we cross into the next source sample
                    while (aud_mp3s_phase < 1.0f && m + 2 <= CAP) {
                        int16_t v = (int16_t)((float)aud_mp3s_prev +
                                    (float)(cur - aud_mp3s_prev) * aud_mp3s_phase);
                        aud_mp3s_out[m++] = v;   // L
                        aud_mp3s_out[m++] = v;   // R
                        aud_mp3s_phase += aud_mp3s_ratio;
                    }
                    aud_mp3s_phase -= 1.0f;
                    aud_mp3s_prev = cur;
                }
                if (m > 0) aud_volume.write((uint8_t *)aud_mp3s_out, m * sizeof(int16_t));
                vTaskDelay(1);
            }
        }
        else if (aud_static_active) {
            int16_t sbuf[512];              // 256 stereo frames of tuning hiss
            audio_static_render(sbuf, 256);
            aud_volume.write((uint8_t *)sbuf, 256 * 4);
            vTaskDelay(1);
        }
        else if (aud_sonar_active) {
            int16_t pbuf[512];              // 256 stereo frames of sonar ping
            audio_sonar_render(pbuf, 256);
            aud_volume.write((uint8_t *)pbuf, 256 * 4);
            vTaskDelay(1);
        }
#ifdef RADIO_PCM_TEST
        else if (aud_pcm_active && aud_pcm_data) {
            const uint8_t *d   = aud_pcm_data;
            uint32_t       len = aud_pcm_len;
            uint32_t       pos = aud_pcm_pos;
            uint32_t       up  = AUD_RATE / aud_pcm_rate;       // 44100/11025 = 4
            if (up < 1) up = 1;
            if (up > 8) up = 8;
            uint32_t avail = (pos < len) ? (len - pos) : 0;
            uint32_t chunk = (avail < 256) ? avail : 256;       // src samples
            size_t   m = 0;
            for (uint32_t i = 0; i < chunk; i++) {
                int16_t a = aud_ulaw_tbl[d[pos + i]];
                uint32_t ni = pos + i + 1;
                int16_t b = (ni < len) ? aud_ulaw_tbl[d[ni]]
                          : (aud_pcm_loop ? aud_ulaw_tbl[d[0]] : a);
                for (uint32_t k = 0; k < up; k++) {             // linear upsample
                    int16_t v = (int16_t)(a + (int32_t)(b - a) * (int32_t)k / (int32_t)up);
                    aud_pcm_mix[m++] = v;                       // L
                    aud_pcm_mix[m++] = v;                       // R
                }
            }
            pos += chunk;
            if (pos >= len) { if (aud_pcm_loop) pos = 0; else aud_pcm_active = false; }
            aud_pcm_pos = pos;
            if (m > 0) aud_volume.write((uint8_t *)aud_pcm_mix, m * sizeof(int16_t));
            vTaskDelay(1);
        }
        else if (aud_lfs_active && aud_lfs_file) {
            // Refill the read-ahead buffer from littlefs when drained (timed).
            if (aud_lfs_buf_pos >= aud_lfs_buf_len) {
                uint32_t t0 = micros();
                int r = aud_lfs_file.read(aud_lfs_buf, sizeof(aud_lfs_buf));
                uint32_t dt = micros() - t0;
                if (dt > aud_lfs_read_max_us) aud_lfs_read_max_us = dt;
                aud_lfs_read_tot_us += dt; aud_lfs_read_cnt++;
                if (dt > 40000) aud_lfs_risky++;            // >40ms = audible-gap risk
                if (r <= 0) {                               // EOF
                    if (aud_lfs_loop) { aud_lfs_file.seek(0); r = aud_lfs_file.read(aud_lfs_buf, sizeof(aud_lfs_buf)); }
                    if (r <= 0) { aud_lfs_active = false; vTaskDelay(1); continue; }
                }
                aud_lfs_buf_len = (size_t)r; aud_lfs_buf_pos = 0;
                if (millis() - aud_lfs_stat_ms > 2000) {    // periodic live stats
                    aud_lfs_stat_ms = millis();
                    CB_LOGF("[LFS] reads=%u maxRead=%uus avg=%uus risky=%u\n",
                                  (unsigned)aud_lfs_read_cnt, (unsigned)aud_lfs_read_max_us,
                                  (unsigned)(aud_lfs_read_tot_us / aud_lfs_read_cnt),
                                  (unsigned)aud_lfs_risky);
                }
            }
            uint32_t up = AUD_RATE / aud_lfs_rate;
            if (up < 1) up = 1; if (up > 8) up = 8;
            size_t avail = aud_lfs_buf_len - aud_lfs_buf_pos;
            size_t chunk = (avail < 256) ? avail : 256;
            size_t m = 0;
            for (size_t i = 0; i < chunk; i++) {
                int16_t a = aud_ulaw_tbl[aud_lfs_buf[aud_lfs_buf_pos + i]];
                size_t ni = aud_lfs_buf_pos + i + 1;
                int16_t b = (ni < aud_lfs_buf_len) ? aud_ulaw_tbl[aud_lfs_buf[ni]] : a;
                for (uint32_t k = 0; k < up; k++) {
                    int16_t v = (int16_t)(a + (int32_t)(b - a) * (int32_t)k / (int32_t)up);
                    aud_pcm_mix[m++] = v; aud_pcm_mix[m++] = v;
                }
            }
            aud_lfs_buf_pos += chunk;
            if (m > 0) aud_volume.write((uint8_t *)aud_pcm_mix, m * sizeof(int16_t));
            vTaskDelay(1);
        }
#endif
        else {
            // SB3 duck-and-resume. Reaching this else means NO clip owns the stream (neither
            // the batch-MP3 nor the streaming branch matched), so core 0 reconciles the render
            // flag to the user's intent. Deliberately an unconditional reconcile rather than a
            // one-shot un-duck, so it SELF-CORRECTS: if audio_theremin_stop() on core 1 races
            // this and loses, the next iteration (~1-10 ms) forces `active` back to false
            // instead of latching a synth the user just disabled. The cost of losing that race
            // is one 256-frame chunk of render, not a permanent state error.
            // NOTE: audio_suspend() must clear `want` too, or this reconcile would re-arm the
            // theremin during its 50 ms drain and defeat it -- see the flag block there.
            if (aud_theremin_active != aud_theremin_want) aud_theremin_active = aud_theremin_want;
            if (aud_theremin_active) {
                // Render theremin audio when active and MP3 is not playing
                int16_t tbuf[512];  // 256 stereo frames
                xSemaphoreTake(aud_theremin_mutex, portMAX_DELAY);
                size_t wrote = aud_theremin.render(tbuf, 256);
                xSemaphoreGive(aud_theremin_mutex);
                if (wrote > 0) {
                    aud_volume.write((uint8_t *)tbuf, wrote * 4);
#ifdef TEST_HARNESS
                    extern void test_audio_capture(const int16_t *, size_t);
                    test_audio_capture(tbuf, wrote);
#endif
                }
                vTaskDelay(1);
            } else if (aud_geiger_active) {
                // Synthesize geiger ticks — damped sine clicks at aud_geiger_rate/sec
                int16_t gbuf[512];  // 256 stereo frames
                size_t wrote = audio_geiger_render(gbuf, 256);
                if (wrote > 0) {
                    aud_volume.write((uint8_t *)gbuf, wrote * 4);
                }
                vTaskDelay(1);
            } else if (aud_tone_active) {
                // UI progress tone (screensaver unlock). Lowest priority — any
                // MP3/theremin/geiger preempts it (none run during the hold).
                int16_t tnbuf[512];  // 256 stereo frames
                audio_tone_render(tnbuf, 256);
                aud_volume.write((uint8_t *)tnbuf, 256 * 4);
                if (aud_tone_target == 0.0f && aud_tone_amp <= 0.0f) aud_tone_active = false;
                vTaskDelay(1);
            } else {
                vTaskDelay(10); // idle wait
            }
        }
    }
}

// ─── Init ──────────────────────────────────────────────────────────────────

static bool audio_init(float volume) {
    if (aud_initialized) {
        CB_LOGLN("[WARN] audio_init() called twice, ignoring");
        return true;
    }

    // Create mutexes
    aud_mp3_mutex     = xSemaphoreCreateMutex();
    aud_theremin_mutex = xSemaphoreCreateMutex();

    // I2S config
    auto i2s_cfg = aud_i2s.defaultConfig(TX_MODE);
    i2s_cfg.sample_rate = 44100;
    i2s_cfg.bits_per_sample = 16;
    i2s_cfg.channels = 2;
    i2s_cfg.pin_bck  = CB_I2S_BCLK;
    i2s_cfg.pin_ws   = CB_I2S_LRCK;
    i2s_cfg.pin_data = CB_I2S_DOUT;

    if (!aud_i2s.begin(i2s_cfg)) {
        CB_LOGLN("[WARN] I2S init failed");
        return false;
    }

    // Volume control
    auto vol_cfg = aud_volume.defaultConfig();
    vol_cfg.volume = volume;
    aud_volume.begin(vol_cfg);

    // Streaming-MP3 output buffer -> PSRAM (was 26 KB of static internal DRAM: the
    // largest single allocation the radio/streaming merge added to the shipping
    // build). The core-0 audio task is its sole user and only touches it after this
    // point, so PSRAM placement is safe; this reclaims scarce internal DRAM for
    // WiFi/BT/DMA. Fall back to internal heap if PSRAM is somehow unavailable.
    // (aud_pcm_mix / aud_lfs_buf are RADIO_PCM_TEST-only and not in this build.)
    aud_mp3s_out = (int16_t *)ps_malloc(sizeof(int16_t) * AUD_MP3S_OUT_N);
    if (!aud_mp3s_out) aud_mp3s_out = (int16_t *)malloc(sizeof(int16_t) * AUD_MP3S_OUT_N);
    if (!aud_mp3s_out) {
        CB_LOGLN("[WARN] audio mp3s_out buffer alloc failed");
        return false;
    }

    aud_initialized = true;

#ifdef RADIO_PCM_TEST
    aud_ulaw_init();   // build the mu-law -> int16 expansion table (radio spike)
#endif

    // Start MP3 decoder task on core 0 (UI/LVGL runs on core 1). Stack enlarged
    // from 8 KB: the streaming radio path runs minimp3 IN this task, whose
    // per-frame scratch (~6-7 KB) plus the frame/mono buffers peak well past 8 KB.
    xTaskCreatePinnedToCore(aud_mp3_task, "mp3", 24576, NULL, 2, &aud_mp3_task_handle, 0);

    CB_LOGLN("[OK] Audio initialized");
    return true;
}

// ─── Volume ────────────────────────────────────────────────────────────────

static void audio_set_volume(float vol) {
    if (!aud_initialized) return;
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;
    aud_volume.setVolume(vol);
}

// ─── Theremin control ──────────────────────────────────────────────────────

static void audio_mp3_stop(void);  // forward declaration

static void audio_theremin_start() {
    if (aud_mp3_active) audio_mp3_stop();
    aud_theremin_want   = true;
    aud_theremin_active = true;
}

static void audio_theremin_stop() {
    // Clear the INTENT first, then the render flag. An explicit stop (Disable, nav away,
    // screensaver, Dark Charge, teardown) must never be undone by a ducked clip ending --
    // if `want` survived here, the next click would resurrect a theremin the user just
    // turned off, with the sensor already powered down.
    aud_theremin_want   = false;
    aud_theremin_active = false;
}

static bool audio_theremin_is_active() {
    return aud_theremin_active;
}

// User-level intent, true across a duck. Use this (not is_active) to answer "is the
// theremin in use?"; is_active answers only "is core 0 rendering it this instant".
static bool audio_theremin_wants_active() {
    return aud_theremin_want;
}

// ─── Theremin band detection ──────────────────────────────────────────────
// The ClipBoyTheremin library treats each of the 8 sensor columns as one
// voice slot. We expose only 4 "bands" (left→right, 2 columns each) so a
// single hand reliably drives one voice — easier to play than the 8-column
// version.
//
// Per-band detection algorithm (replaces simple averaging — averaging
// caused weird pitch jumps when a single zone flickered between valid and
// invalid status, or when far-target reads got mixed in with hand reads):
//
//   1. Collect every zone in the band's 16-cell slice with valid status
//      (5 or 9) AND distance ≤ AUD_BAND_MAX_DIST_MM.
//   2. Sort the valid distances ascending.
//   3. Require ≥k valid zones (aud_band_k, configurable 1-8).
//   4. The k smallest must agree within aud_band_agreement_mm of each
//      other (i.e., dists[k-1] - dists[0] ≤ agreement). This rejects
//      single-zone glitches where one sensor briefly reports an unrelated
//      close distance.
//   5. If accepted, the band's distance is the AVERAGE of those k
//      smallest values (smoother than min, still tracks the closest
//      object — which is the hand).
//
// k=1 reduces to "use the closest valid zone" (fast but jumpy).
// k=2/3 with agreement=20mm is the sweet spot for hand-sized targets.
//
// Synthetic frame: we stuff the band's distance into column `band` (0-3)
// of a fake VL53L5CX_ResultsData and pass that to the library. Cols 4-7
// stay invalid so voice slots 4-7 never fire. Library is unmodified.
//
// 100ms hold: brief sensor dropouts replay the last good distance for up
// to AUD_BAND_HOLD_MS before letting the voice deactivate.
//
// Anything beyond AUD_BAND_MAX_DIST_MM is treated as "no detection" so
// far hand sweeps don't produce loud sub-bass thumps.
static const uint32_t AUD_BAND_HOLD_MS     = 100;
static const int      AUD_BAND_MAX_DIST_MM = 400;  // matches ClipTheremin::Config::maxDistMm

// Tuning — written from the UI sliders via setters below
static volatile uint8_t aud_band_k            = 2;   // 1-8: required agreeing zones
static volatile uint8_t aud_band_agreement_mm = 20;  // 5-50: max spread of k smallest

static int      aud_band_last_dist[4]    = { -1, -1, -1, -1 };
static uint32_t aud_band_last_good_ms[4] = { 0, 0, 0, 0 };

// Thread-safe wrapper for theremin feed (called from core 1 / LVGL timer)
static void audio_theremin_feed(const VL53L5CX_ResultsData &results) {
    if (!aud_theremin_active || !aud_theremin_mutex) return;

    VL53L5CX_ResultsData synth;
    memset(&synth, 0, sizeof(synth));

    // Snapshot tuning — they're volatile and could be written mid-frame
    uint8_t k    = aud_band_k;
    if (k < 1) k = 1;
    if (k > 8) k = 8;
    int     agree = (int)aud_band_agreement_mm;

    uint32_t now = millis();
    for (uint8_t band = 0; band < 4; band++) {
        // Collect valid distances from this band's 16-zone slice
        int dists[16];
        int n = 0;
        uint8_t col0 = band * 2;
        uint8_t col1 = col0 + 1;
        for (uint8_t row = 0; row < 8; row++) {
            for (uint8_t c = col0; c <= col1; c++) {
                uint8_t idx = row * 8 + c;
                uint8_t st  = results.target_status[idx];
                if (st == 5 || st == 9) {
                    int16_t d = results.distance_mm[idx];
                    if (d > 0 && d <= AUD_BAND_MAX_DIST_MM) {
                        dists[n++] = (int)d;
                    }
                }
            }
        }

        int dist = -1;
        if (n >= k) {
            // Insertion sort ascending — N≤16, simple and fast
            for (int i = 1; i < n; i++) {
                int v = dists[i];
                int j = i - 1;
                while (j >= 0 && dists[j] > v) {
                    dists[j + 1] = dists[j];
                    j--;
                }
                dists[j + 1] = v;
            }
            // k smallest must agree within `agree` mm of each other
            if (dists[k - 1] - dists[0] <= agree) {
                int32_t sum = 0;
                for (int i = 0; i < k; i++) sum += dists[i];
                dist = (int)(sum / k);
                aud_band_last_dist[band]    = dist;
                aud_band_last_good_ms[band] = now;
            }
        }

        // No fresh reading? Engage 100 ms hold using last good distance.
        if (dist < 0 &&
            aud_band_last_dist[band] >= 0 &&
            (now - aud_band_last_good_ms[band]) <= AUD_BAND_HOLD_MS) {
            dist = aud_band_last_dist[band];
        }

        // Stuff the band's distance into column `band` (0..3) so the
        // library's per-column processing drives voice slot `band`.
        if (dist > 0) {
            for (uint8_t row = 0; row < 8; row++) {
                uint8_t idx = row * 8 + band;
                synth.distance_mm[idx]        = (int16_t)dist;
                synth.target_status[idx]      = 5;
                synth.nb_target_detected[idx] = 1;
            }
        }
    }

    xSemaphoreTake(aud_theremin_mutex, portMAX_DELAY);
    aud_theremin.feed(synth);
    xSemaphoreGive(aud_theremin_mutex);
}

// Live tuning setters — called from UI slider callbacks
static void audio_theremin_set_k(uint8_t k) {
    if (k < 1) k = 1;
    if (k > 8) k = 8;
    aud_band_k = k;
}

static void audio_theremin_set_agreement(uint8_t mm) {
    if (mm < 5)  mm = 5;
    if (mm > 50) mm = 50;
    aud_band_agreement_mm = mm;
}

// Live volume — written from the slider callback. The library's
// masterVolume usually only takes effect at begin(); this routes through
// the new setMasterVolume() so slider changes are heard immediately.
static void audio_theremin_set_volume(float v) {
    if (!aud_theremin_mutex) return;
    xSemaphoreTake(aud_theremin_mutex, portMAX_DELAY);
    aud_theremin.setMasterVolume(v);
    xSemaphoreGive(aud_theremin_mutex);
}

// Reset per-band hold state — call when starting/stopping the theremin
// so a stale "last good" reading from an earlier session can't replay.
static void audio_theremin_reset_bands(void) {
    for (int i = 0; i < 4; i++) {
        aud_band_last_dist[i]    = -1;
        aud_band_last_good_ms[i] = 0;
    }
}

// ─── MP3 playback ─────────────────────────────────────────────────────────

// Per-sound play/stop logging spams the serial during scanning (the sonar ping
// loops every ~1s), fast enough to overrun the USB-CDC TX buffer and stall the
// badge under a slow reader. Gated OFF by default; build with -DDEBUG_VERBOSE to
// restore it. (Errors + suspend/resume stay unconditional -- they're rare.)
#ifdef DEBUG_VERBOSE
  #define AUD_VLOG(...) Serial.printf(__VA_ARGS__)
#else
  #define AUD_VLOG(...) ((void)0)
#endif

static void audio_mp3_stop(void) {
    if (!aud_mp3_active && !aud_mp3_pcm) return;
    xSemaphoreTake(aud_mp3_mutex, portMAX_DELAY);
    aud_mp3_active = false;     // signal task to stop streaming
    xSemaphoreGive(aud_mp3_mutex);
    // Let the task finish any in-flight write before we free the PCM buffer.
    vTaskDelay(pdMS_TO_TICKS(30));
    xSemaphoreTake(aud_mp3_mutex, portMAX_DELAY);
    int16_t *old   = aud_mp3_pcm;
    bool     owned = aud_mp3_owns_pcm;
    aud_mp3_pcm    = NULL;
    aud_mp3_frames = 0;
    aud_mp3_pos    = 0;
    aud_mp3_loop   = false;
    aud_mp3_owns_pcm = true;          // back to the default for the next player
    xSemaphoreGive(aud_mp3_mutex);
    if (old && owned) free(old);      // a borrowed buffer belongs to its caller
    CB_LOGLN("[AUD] MP3 stopped");
}

// duck_theremin: this clip is a brief, subordinate sound (the UI tap click) that should
// PRE-EMPT a running theremin rather than end it -- the audio task restores the synth when
// the clip drains. Left false for every other caller (radio, collectibles, ARG, sonar), so
// their behavior is byte-identical to before: they still stop the theremin outright.
static void audio_mp3_play(const uint8_t *data, size_t len, bool loop, float gain = 1.0f,
                           bool duck_theremin = false) {
    if (!aud_initialized) return;
    // Stop any current MP3 (frees its PCM) or theremin.
    if (aud_mp3_active || aud_mp3_pcm) audio_mp3_stop();
    if (aud_theremin_active) {
#if CB_THEREMIN_DUCK_CLICK
        // Never duck for a LOOP: it has no natural end, so the synth would sit silent until
        // something else stopped the loop. Only the short one-shot click ducks.
        if (duck_theremin && !loop) aud_theremin_active = false;  // `want` stays set
        else                       audio_theremin_stop();
#else
        (void)duck_theremin;
        audio_theremin_stop();
#endif
    }

    // Decode the whole sound to PCM up front (fast for these short clips).
    size_t frames = 0;
    int16_t *pcm = aud_mp3_decode(data, len, &frames);
    if (!pcm || frames == 0) {
        if (pcm) free(pcm);
        CB_LOGLN("[AUD] MP3 decode failed");
        return;
    }

    // LOOP-ONLY: trim leading/trailing silence so the wrap is tight. minimp3
    // decodes the WHOLE file including the MP3 encoder-delay padding (~1152
    // pure-zero samples at the head, plus tail padding) -- on a loop that dead
    // air replays every cycle as an audible PAUSE (a desktop player hides it via
    // gapless LAME/iTunSMPB tags; we don't parse those). Strip the pure-silence
    // ends here. One-shots are left untouched (a ~50ms onset delay is inaudible,
    // and trimming could clip an intentional chime tail).
    if (loop && frames > 0) {
        const int16_t TH = 64;   // encoder-delay is exact 0; real audio clears this
        size_t s = 0, e = frames;
        while (s < e) {
            int16_t l = pcm[s * 2], r = pcm[s * 2 + 1];
            if (l > TH || l < -TH || r > TH || r < -TH) break;
            s++;
        }
        while (e > s) {
            int16_t l = pcm[(e - 1) * 2], r = pcm[(e - 1) * 2 + 1];
            if (l > TH || l < -TH || r > TH || r < -TH) break;
            e--;
        }
        if (e > s && (s > 0 || e < frames)) {
            size_t nf = e - s;
            if (s > 0) memmove(pcm, pcm + s * 2, nf * 2 * sizeof(int16_t));
            CB_LOGF("[AUD] loop trim: %u -> %u frames\n", (unsigned)frames, (unsigned)nf);
            frames = nf;
        }
    }

    // Optional fixed attenuation so this clip plays below primary audio
    // (used for the UI tap click). gain == 1.0 = no change.
    if (gain < 0.999f) {
        size_t n = frames * 2;  // interleaved stereo int16
        for (size_t i = 0; i < n; i++) pcm[i] = (int16_t)((float)pcm[i] * gain);
    }

    xSemaphoreTake(aud_mp3_mutex, portMAX_DELAY);
    aud_mp3_pcm    = pcm;
    aud_mp3_frames = frames;
    aud_mp3_pos    = 0;
    aud_mp3_loop   = loop;
    aud_mp3_owns_pcm = true;   // we decoded it, so we free it
    aud_mp3_active = true;  // set last so the task sees consistent state
    xSemaphoreGive(aud_mp3_mutex);
    CB_LOGF("[AUD] MP3 play (%u frames, loop=%d)\n",
                  (unsigned)frames, loop);
}

// Play an ALREADY-DECODED stereo PCM buffer that the CALLER owns and keeps
// alive for the duration. Retriggering just rewinds to frame 0.
//
// This exists because audio_mp3_play() is far too expensive to fire on a game
// event: it calls audio_mp3_stop() first (a hard vTaskDelay(30 ms) on the
// caller, so the audio task can finish reading before the buffer is freed) and
// then decodes the whole clip with a ps_malloc/ps_realloc loop. Measured on
// hardware, one hit sound cost 233 ms of blocked main loop -- a visible freeze
// mid-swipe. Decode once, then come through here: no decode, no allocation, no
// stop(), no free. Nothing is freed on this path at all, so the audio task can
// never be left holding a dead pointer.
//
// The CALLER must call audio_mp3_stop() before freeing its buffer. That clears
// the pointer and waits out any in-flight write, and the ownership flag keeps
// it from freeing memory it did not allocate.
static void audio_pcm16_play(const int16_t *pcm, size_t frames) {
    if (!aud_initialized || !pcm || !frames) return;
    // A driver-owned buffer still installed must go through the normal path so
    // it actually gets freed. Costs the 30 ms once, not once per shot.
    if (aud_mp3_owns_pcm && aud_mp3_pcm) audio_mp3_stop();
    if (aud_theremin_active) audio_theremin_stop();
    xSemaphoreTake(aud_mp3_mutex, portMAX_DELAY);
    aud_mp3_pcm      = (int16_t *)pcm;
    aud_mp3_frames   = frames;
    aud_mp3_pos      = 0;
    aud_mp3_loop     = false;
    aud_mp3_owns_pcm = false;         // borrowed: never free this one
    aud_mp3_active   = true;          // set last so the task sees consistent state
    xSemaphoreGive(aud_mp3_mutex);
}

static bool audio_mp3_is_playing(void) {
    return aud_mp3_active;
}

// UI click sound. Fired on button press/tap callbacks for tactile audio
// feedback. Short (~4 KB) MP3; uses the same single-stream MP3 path
// so any longer-running MP3 (e.g. the scan search loop) gets
// interrupted -- which is fine because the click is short and the
// preceding audio resumes only when explicitly re-started by the
// caller's flow, never by the click.
#include "snd_click_mp3.h"
#include "snd_startup_mp3.h"   // boot sound (played early in setup())
// Tap click plays at a fixed fraction of system volume so it stays subordinate
// to primary audio (theremin/geiger/scan) — the phone/camera UI-sound convention.
#define CLICK_GAIN 0.40f
static inline void audio_play_click(void) {
    // Gated by the global mute (cfg.sound) AND the tap-sound toggle (cfg.ui_click),
    // so taps can be silenced without muting theremin/geiger/scan audio.
    if (!cfg.sound || !cfg.ui_click) return;
#if !CB_THEREMIN_DUCK_CLICK && !defined(CB_SB3_FAULT_INJECT)
    // SB3, conservative mode: a tap must never pre-empt the synth, so the click is simply
    // SILENT while the theremin runs. In practice this only mutes the status-bar FL and ?
    // buttons -- sliders are already excluded from the click hook, and the voice dropdowns
    // are locked while the theremin is running.
    if (aud_theremin_active) return;
#endif
    // FAULT INJECTION, tests only -- NEVER define CB_SB3_FAULT_INJECT in a shipping build.
    // Building with `-DCB_THEREMIN_DUCK_CLICK=0 -DCB_SB3_FAULT_INJECT` restores the original
    // SB3 defect (the click reaches audio_mp3_play, which stops the theremin outright with no
    // re-arm), so test_teardown_paths.py's SB3 case can be proven to go RED. A test that has
    // only ever seen fixed firmware proves nothing; this keeps the bug rebuildable on demand
    // instead of relying on someone having witnessed it once.
    // SB3, default mode: duck-and-resume. The click plays; the core-0 task restores the
    // theremin when the clip drains (~100 ms), so tactile feedback and the synth coexist.
    audio_mp3_play(click_mp3, click_mp3_len, false, CLICK_GAIN, /*duck_theremin=*/true);
}

// ─── UI progress tone control (see oscillator above) ────────────────────────
// Start a continuous tone at `freq` Hz; update its pitch live with set_freq;
// stop() fades it out. blip() plays a one-shot chirp that self-fades after ~ms
// (used for the unlock "complete" chime). All no-op when global sound is muted.
static inline void audio_tone_start(float freq) {
    if (!cfg.sound) return;
    aud_tone_freq = freq; aud_tone_phase = 0.0f; aud_tone_amp = 0.0f;
    aud_tone_autostop = -1; aud_tone_target = 1.0f; aud_tone_active = true;
}
static inline void audio_tone_set_freq(float freq) { aud_tone_freq = freq; }
static inline void audio_tone_stop(void) { aud_tone_target = 0.0f; }  // fade out
static inline void audio_tone_blip(float freq, uint32_t ms, float amp) {
    if (!cfg.sound) return;
    aud_tone_freq = freq;
    aud_tone_autostop = (int32_t)((uint64_t)ms * AUD_RATE / 1000);
    aud_tone_target = amp;   // caller picks loudness (keep modest on tiny speakers)
    if (!aud_tone_active) { aud_tone_amp = 0.0f; aud_tone_phase = 0.0f; aud_tone_active = true; }
}

// Kept for API compatibility. With the decode-to-PCM path there's no streaming
// decoder state to preserve, so a "swap" is just a fresh play.
static void audio_mp3_swap(const uint8_t *data, size_t len, bool loop) {
    audio_mp3_play(data, len, loop);
}

// ─── Audio loop tick ───────────────────────────────────────────────────────
// Must be called from loop() to pump audio samples to I2S.

static void audio_loop(void) {
    if (!aud_initialized) return;
    // MP3 and theremin playback both run on core 0 task — nothing to do here
}

// Fully stop I2S DMA + audio task (for WiFi join — I2S interrupts on Core 0
// conflict with WiFi event processing)
static void audio_suspend(void) {
    if (!aud_initialized) return;
    // Signal the task to stop writing audio data before we tear anything down.
    // vTaskSuspend is asynchronous for cross-core targets on ESP32-S3 SMP —
    // the task may still be mid-write when suspend returns.  By clearing the
    // active flags first and yielding, the task reaches its idle vTaskDelay(10)
    // path and stops touching I2S, so the subsequent end() is safe.
    // Clear EVERY render-source flag first -- not just mp3+theremin. Any active source
    // (streaming radio bed aud_mp3s, static, sonar, geiger, unlock tone) whose flag is
    // left set keeps the core-0 task in its render branch writing to aud_i2s, and the
    // end() below frees the peripheral out from under it -> core-0 LoadProhibited reboot
    // (e.g. WiFi-join while a radio bed plays). C1, review 2026-07-08.
    bool was_mp3      = aud_mp3_active;
    bool was_theremin = aud_theremin_active;
    bool was_mp3s     = aud_mp3s_active;
    bool was_static   = aud_static_active;
    bool was_sonar    = aud_sonar_active;
    bool was_geiger   = aud_geiger_active;
    bool was_tone     = aud_tone_active;
    // `want` MUST be saved+cleared alongside the render flags. The SB3 duck-and-resume
    // reconcile in the task's idle branch sets aud_theremin_active = aud_theremin_want every
    // iteration, and the task is still RUNNING throughout the 50 ms drain below -- so leaving
    // `want` set would re-arm the theremin mid-drain, keep the task in its render branch, and
    // hand the end() below a live writer. That is precisely the core-0 LoadProhibited reboot
    // this block exists to prevent (see the comment above), reached from a new direction.
    bool was_want     = aud_theremin_want;
    aud_theremin_want = false;
    aud_mp3_active = false;   aud_theremin_active = false; aud_mp3s_active = false;
    aud_static_active = false; aud_sonar_active = false;   aud_geiger_active = false;
    aud_tone_active = false;
    vTaskDelay(pdMS_TO_TICKS(50));  // let task drain into idle
    if (aud_mp3_task_handle) vTaskSuspend(aud_mp3_task_handle);
    aud_i2s.end();  // stops I2S DMA and frees the peripheral
    // Restore so resume continues whatever was playing.
    aud_mp3_active = was_mp3;   aud_theremin_active = was_theremin; aud_mp3s_active = was_mp3s;
    aud_static_active = was_static; aud_sonar_active = was_sonar;   aud_geiger_active = was_geiger;
    aud_tone_active = was_tone;
    aud_theremin_want = was_want;   // restored last: the task is suspended, so no reconcile races
    CB_LOGLN("[AUD] Suspended (I2S stopped)");
}

static void audio_resume(void) {
    if (!aud_initialized) return;
    auto i2s_cfg = aud_i2s.defaultConfig(TX_MODE);
    i2s_cfg.sample_rate = 44100;
    i2s_cfg.bits_per_sample = 16;
    i2s_cfg.channels = 2;
    i2s_cfg.pin_bck  = CB_I2S_BCLK;
    i2s_cfg.pin_ws   = CB_I2S_LRCK;
    i2s_cfg.pin_data = CB_I2S_DOUT;
    aud_i2s.begin(i2s_cfg);
    if (aud_mp3_task_handle) vTaskResume(aud_mp3_task_handle);
    CB_LOGLN("[AUD] Resumed (I2S restarted)");
}
