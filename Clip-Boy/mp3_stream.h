// mp3_stream.h — portable, pull-based streaming MP3 decoder core.
//
// WHY THIS EXISTS
//   The badge's radio beds are multi-minute MP3s (e.g. 1-1.mp3 = 78 s @ 8 kHz).
//   Decoding one whole to int16 PCM in PSRAM is impossible (78 s * 44100 * 2ch *
//   2B ≈ 14 MB). We must STREAM: keep a few-KB compressed window, decode one
//   frame at a time, and feed the I2S task incrementally.
//
//   minimp3 (CC0/public-domain, already vendored in libs/minimp3) supports this
//   natively via mp3dec_decode_frame(); the whole-file path in audio_driver.h
//   already walks frames the same way. The ONLY new/risky logic is the streaming
//   window: refill when a frame straddles the buffer end, and — critically —
//   TERMINATE at end-of-stream instead of spinning forever on a partial frame.
//   That infinite-loop-on-partial-frame is exactly the bug that made audio-tools'
//   MP3DecoderMini wrapper wedge core 0 (see CLAUDE.md / memory audio_minimp3).
//
//   This core is dependency-light (stdint/string + minimp3) and takes a pull
//   callback for its input, so the SAME code runs in:
//     - the host test (scripts/tests/mp3_stream_host.c) — proves it byte-for-byte
//       matches whole-file decode and always terminates, and
//     - the firmware (audio_driver.h) — source = a littlefs File or PROGMEM ptr,
//       output resampled mono->stereo into the I2S ring.
//
// LICENSE: this shim is first-party (project license). minimp3 stays CC0.
#ifndef MP3_STREAM_H
#define MP3_STREAM_H

#include <stdint.h>
#include <string.h>
#include "minimp3.h"

// Compressed read-ahead window. Must exceed the largest possible MP3 frame
// (MPEG1 L3 worst case ≈ 1441 B; keep generous slack) so a whole frame is
// almost always resident and refills are rare.
#ifndef MP3S_BUFSZ
#define MP3S_BUFSZ 4096
#endif
// Refill threshold: top up whenever fewer than this many bytes remain, so a
// full frame is available to the decoder without a mid-frame stall.
#define MP3S_MAXFRAME 1600

// Pull callback: copy up to `want` bytes of the raw MP3 stream into `dst`,
// return the number actually copied. Return 0 to signal end-of-source.
typedef size_t (*mp3s_read_fn)(void *ctx, uint8_t *dst, size_t want);

typedef struct {
    mp3dec_t     dec;
    mp3s_read_fn read;
    void        *ctx;
    uint8_t      buf[MP3S_BUFSZ];
    size_t       buf_len;      // valid bytes currently in buf
    int          eof;          // source exhausted
    int          hz, ch;       // last frame's sample rate / channel count
    uint64_t     frames_out;   // stats: audio frames emitted
    uint64_t     samples_out;  // stats: mono samples emitted
} mp3s_t;

static inline void mp3s_init(mp3s_t *s, mp3s_read_fn read, void *ctx) {
    memset(s, 0, sizeof(*s));
    mp3dec_init(&s->dec);
    s->read = read;
    s->ctx  = ctx;
}

// Top up buf from the source as FULL as possible; returns bytes added (0 at/near
// EOF). Loops over read() so a source that hands back tiny chunks still yields a
// full window — the decoder must always see a whole frame's worth or minimp3
// will skip bytes one at a time and make no progress.
static inline size_t mp3s__fill(mp3s_t *s) {
    if (s->eof) return 0;
    size_t added = 0;
    while (!s->eof) {
        size_t space = MP3S_BUFSZ - s->buf_len;
        if (!space) break;
        size_t n = s->read(s->ctx, s->buf + s->buf_len, space);
        if (n == 0) { s->eof = 1; break; }
        s->buf_len += n;
        added      += n;
    }
    return added;
}

// Decode the next audio frame into `pcm_mono` (capacity must be at least
// MINIMP3_MAX_SAMPLES_PER_FRAME/2 int16). Stereo input is downmixed to mono to
// match the whole-file path. Returns the mono sample count (>0), or 0 once the
// stream is fully drained.
//
// GUARANTEED TO TERMINATE: the frame_bytes==0 path returns 0 at EOF, and a
// full-buffer-with-no-decodable-frame condition (corrupt/garbage input, e.g. a
// malformed file on an SD card) force-drops a byte to resync rather than
// spinning — so a bad asset can't wedge the audio task.
static inline int mp3s_next(mp3s_t *s, int16_t *pcm_mono) {
    int16_t tmp[MINIMP3_MAX_SAMPLES_PER_FRAME];
    mp3dec_frame_info_t info;
    for (;;) {
        if (s->buf_len < MP3S_MAXFRAME && !s->eof) mp3s__fill(s);

        int samples = mp3dec_decode_frame(&s->dec, s->buf, (int)s->buf_len, tmp, &info);

        if (info.frame_bytes > 0) {
            // Consumed frame_bytes (may include skipped ID3/garbage). Slide the
            // window down so the next frame starts at buf[0].
            memmove(s->buf, s->buf + info.frame_bytes, s->buf_len - info.frame_bytes);
            s->buf_len -= info.frame_bytes;
            if (samples > 0) {
                s->hz = info.hz;
                s->ch = info.channels;
                int ch = info.channels > 0 ? info.channels : 1;
                if (ch == 1) {
                    memcpy(pcm_mono, tmp, (size_t)samples * sizeof(int16_t));
                } else {
                    for (int i = 0; i < samples; i++)
                        pcm_mono[i] = (int16_t)(((int32_t)tmp[i * ch] + tmp[i * ch + 1]) / 2);
                }
                s->frames_out++;
                s->samples_out += (uint64_t)samples;
                return samples;
            }
            continue;  // skipped a non-audio frame (ID3/junk); keep going
        }

        // frame_bytes == 0: minimp3 needs more bytes to form a frame.
        if (s->eof) return 0;                 // true end of stream
        size_t got = mp3s__fill(s);
        if (got == 0) {
            if (s->eof) return 0;             // fill hit EOF
            if (s->buf_len == MP3S_BUFSZ) {   // full + undecodable => resync
                memmove(s->buf, s->buf + 1, s->buf_len - 1);
                s->buf_len--;
            } else {
                return 0;                     // source stalled; treat as end
            }
        }
        // else: got more bytes, retry the decode
    }
}

#endif // MP3_STREAM_H
