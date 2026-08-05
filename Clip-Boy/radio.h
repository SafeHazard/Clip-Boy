#pragma once
// =============================================================================
// radio.h — "SegFault-Tec FM" station model (DC34-129 / DC34-131..134)
// -----------------------------------------------------------------------------
// The data backbone for the full-screen radio. show_radio() (DC34-131) builds
// against this; the oscilloscope (132), tuning dial (133) and audio (134) read
// these rows.
//
// AUDIO: each station owns a set of clips by FILE PREFIX -- clip_prefix N means
// every audio/N-*.mp3 (e.g. prefix 1 = 1-1/1-2/1-3.mp3). Clips are embedded +
// captioned by scripts/embed_audio.py into g_radio_clips[] (radio_audio_gen).
// The radio plays a station's unlocked clips in RANDOM order on an endless loop
// (Bryce, 2026-07-05). Some clips are gated (see radio_clip_available in ui_nav.h):
// the πr8 r4di0 songs 1-2/1-3 only unlock after ARG puzzle 1 is complete.
//   file-prefix -> station:  1=πr8 r4di0 (numbers/ARG), 2=WGHOUL, 3=WLAN-FM,
//                            4=Tab Street, 5=RD0-SH0K.
//
// LINEUP LOCKED (panel + owner, 2026-06-30): narrowed 9 candidates -> 5 stations.
// UNLOCK = collectible COUNT. gate_count = collectibles you must have found before
// the station tunes in (0 = always). Owner spread: 0 / 1 / 25 / 50 / 75.
//
// ACCESSIBILITY: every clip ships a caption (its ID3 comment, pulled at build time)
// -- mandatory on πr8 r4di0 so the ARG P1 breadcrumb (verbatim hex, decodes to
// "USB2serial:clipcli startgame") isn't audio-only. The "Transcript" button shows
// the currently-playing clip's caption.
//
// Entry = the secret-real "Whether Radio" SAO; gating reuses the collectibles
// system (coll_count_found()).
// =============================================================================
#include <stdint.h>
#include "radio_audio_gen.h"   // g_radio_clips[] {name,data,len,caption} — generated from audio/*.mp3

// Oscilloscope signature per station (DC34-132 draws to these). Design §4d.
typedef enum {
    RSCOPE_TALK = 0,  // spiky        — DJ/voice-led
    RSCOPE_MUSIC,     // rolling      — instrumental bed
    RSCOPE_NUMBERS,   // cold sine + blips — numbers/coded station
    RSCOPE_BEACON,    // slow pulse   — distress/looping beacon
    RSCOPE_DEAD,      // flatline + hiss — cursed/haunted / dead-air anchor
} radio_scope_t;

typedef struct {
    const char    *name;       // station identity
    const char    *desc;       // deadpan TV-guide blurb
    uint16_t       freq_dkhz;  // dial position, FM ×10 (885 = 88.5 MHz)
    radio_scope_t  scope;      // oscilloscope signature
    uint8_t        gate_count; // collectibles required to unlock (0 = always available)
    uint8_t        clip_prefix;// audio file prefix: plays every audio/<prefix>-*.mp3
    uint8_t        alternate;  // 1 = strict speech<->music alternation (2 shuffle-bags);
                               // 0 = single combined shuffle-bag (even, no forced cadence)
} RadioStation;

// The locked 5, in dial order. (gate_count spread: 75 / 1 / 25 / 50 / 0.)
static const RadioStation radio_stations[] = {
    { "RD0-SH0K",       "PLACEHOLDER: the anchor station-ID that glitches out before it can finish expanding its own call-sign; drifts to dead-air hiss. Resolves fully only at 100%.", 875,  RSCOPE_DEAD,    75, 5, 0 },
    { "WLAN-FM",        "PLACEHOLDER: 2am quiet-storm vibe bed; a dreamy ASMR host incongruously soothed by clean, flat network diagrams.",                                            889,  RSCOPE_MUSIC,    1, 3, 0 },
    { "Tab Street",     "PLACEHOLDER: a hype-y finfluencer pumping soda-tab portfolios. Self-evidently absurd + a rapid-fire 'not financial advice' disclaimer.",                       947,  RSCOPE_TALK,    25, 4, 1 },
    { "WGHOUL",         "PLACEHOLDER: irradiated morning-zoo. Chad Chadwell confidently mistranslates the ghoul's moans-that-may-be-Morse into traffic reports.",              1011, RSCOPE_BEACON,  50, 2, 1 },
    { "πr8 r4di0",      "PLACEHOLDER: the boot-sector pirate frequency. A bubbly anime numbers voice reads correct hex digit groups - the ARG P1 breadcrumb.",                          1079, RSCOPE_NUMBERS,  0, 1, 0 },
};
#define NUM_RADIO_STATIONS  (sizeof(radio_stations) / sizeof(radio_stations[0]))
