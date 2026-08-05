// mp3_stream_host.c — host-side proof of the streaming MP3 core (mp3_stream.h).
//
// Decodes an MP3 two ways and asserts they are identical:
//   A) whole-file reference (mirrors audio_driver.h aud_mp3_decode's frame walk)
//   B) streaming via mp3_stream.h, fed through a deliberately TINY, frame-
//      misaligned read callback to hammer the refill / partial-frame path.
// Both drive the same minimp3 decoder over the same byte stream, so correct
// window management => byte-for-byte identical mono PCM. It also proves the
// stream TERMINATES (the audio-tools wrapper's bug was spinning forever on a
// partial frame at EOF). Exit 0 = PASS.
//
// Build: scripts/tests/build_mp3_host.cmd  (MSVC via vcvars64)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#include "mp3_stream.h"

// Reference: decode the whole file, collecting downmixed mono @ source rate.
static int16_t *decode_whole(const uint8_t *mp3, size_t len, size_t *outN, int *hz, int *ch) {
    mp3dec_t dec; mp3dec_init(&dec);
    int16_t frame[MINIMP3_MAX_SAMPLES_PER_FRAME];
    size_t cap = 1u << 20, n = 0;
    int16_t *mono = (int16_t *)malloc(cap * sizeof(int16_t));
    mp3dec_frame_info_t info;
    const uint8_t *p = mp3; int remain = (int)len;
    *hz = 0; *ch = 0;
    while (remain > 0) {
        int s = mp3dec_decode_frame(&dec, p, remain, frame, &info);
        if (info.frame_bytes <= 0) break;
        p += info.frame_bytes; remain -= info.frame_bytes;
        if (s == 0) continue;
        *hz = info.hz; *ch = info.channels;
        int c = info.channels > 0 ? info.channels : 1;
        if (n + (size_t)s > cap) { cap = (n + s) * 2; mono = (int16_t *)realloc(mono, cap * sizeof(int16_t)); }
        for (int i = 0; i < s; i++)
            mono[n++] = (c == 1) ? frame[i] : (int16_t)(((int32_t)frame[i*c] + frame[i*c+1]) / 2);
    }
    *outN = n; return mono;
}

// Streaming source: hand out at most `chunk` bytes per call (frame-misaligned).
typedef struct { const uint8_t *d; size_t len, pos, chunk; } memsrc;
static size_t memread(void *ctx, uint8_t *dst, size_t want) {
    memsrc *m = (memsrc *)ctx;
    size_t n = m->len - m->pos;
    if (n > want)     n = want;
    if (n > m->chunk) n = m->chunk;
    memcpy(dst, m->d + m->pos, n);
    m->pos += n;
    return n;
}

static int run_one(const uint8_t *buf, long fl, const int16_t *A, size_t nA, int hzA, size_t chunk) {
    memsrc src = { buf, (size_t)fl, 0, chunk };
    mp3s_t s; mp3s_init(&s, memread, &src);
    size_t capB = nA + 4096, nB = 0;
    int16_t *B = (int16_t *)malloc(capB * sizeof(int16_t));
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME / 2 + 8];
    unsigned iter = 0, ITER_CAP = (unsigned)(fl + 200000);  // generous termination guard
    for (;;) {
        if (++iter > ITER_CAP) { printf("  chunk=%-5zu FAIL: no termination (iter cap)\n", chunk); free(B); return 0; }
        int got = mp3s_next(&s, pcm);
        if (got == 0) break;
        if (nB + (size_t)got > capB) { capB = (nB + got) * 2; B = (int16_t *)realloc(B, capB * sizeof(int16_t)); }
        memcpy(B + nB, pcm, (size_t)got * sizeof(int16_t)); nB += got;
    }
    int match = (nA == nB) && (memcmp(A, B, nA * sizeof(int16_t)) == 0);
    printf("  chunk=%-5zu stream_samples=%-8zu iters=%-7u %s\n",
           chunk, nB, iter, match ? "PASS" : "FAIL");
    (void)hzA;
    free(B);
    return match;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s file.mp3\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 2; }
    fseek(f, 0, SEEK_END); long fl = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc(fl);
    if (fread(buf, 1, fl, f) != (size_t)fl) { fprintf(stderr, "read short\n"); return 2; }
    fclose(f);

    size_t nA; int hzA, chA;
    int16_t *A = decode_whole(buf, fl, &nA, &hzA, &chA);
    double dur = hzA ? (double)nA / hzA : 0;
    printf("file=%s size=%ld\n", argv[1], fl);
    printf("reference (whole): samples=%zu hz=%d ch=%d dur=%.2fs\n", nA, hzA, chA, dur);

    // Stress the streaming path across chunk sizes: 1 byte (pathological), a
    // fraction of a frame, ~one frame, and larger than a frame.
    size_t chunks[] = { 1, 61, 512, 1600, 4096 };
    int all = 1;
    for (size_t i = 0; i < sizeof(chunks)/sizeof(chunks[0]); i++)
        all &= run_one(buf, fl, A, nA, hzA, chunks[i]);

    printf("RESULT: %s\n", all ? "PASS (streaming == whole-file, all chunk sizes, terminates)" : "FAIL");
    free(A); free(buf);
    return all ? 0 : 1;
}
