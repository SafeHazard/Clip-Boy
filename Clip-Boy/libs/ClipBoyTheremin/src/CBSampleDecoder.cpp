#include "CBSampleDecoder.h"

#include <Arduino.h>
#include <FS.h>
#include <SD.h>
// minimp3 (public domain / CC0) replaced libhelix here to drop the GPL/RPSL
// copyleft from the build. Declarations only; MINIMP3_IMPLEMENTATION is
// compiled once in the sketch (ui_test/minimp3_impl.cpp).
#include "minimp3.h"

namespace ClipTheremin {

void SampleBuffer::free() {
  if (data) {
    ::free(data);  // ps_malloc uses standard free()
    data = nullptr;
  }
  frames = 0;
  sampleRate = 0;
  valid = false;
}

// ----- Internal decoder context -----

struct DecodeContext {
  int16_t *buf;
  size_t   capacity;    // max mono frames
  size_t   written;     // mono frames written so far
  uint32_t sampleRate;
  uint8_t  channels;
  bool     overflow;
};

// Finalize: shrink-copy the decode buffer to exact size
static LoadError finalize(DecodeContext &dc, SampleBuffer &out) {
  if (dc.written == 0) {
    ::free(dc.buf);
    return LoadError::DecodeFailed;
  }

  // Allocate exact-fit buffer in PSRAM
  size_t exactBytes = dc.written * sizeof(int16_t);
  int16_t *exact = (int16_t *)ps_malloc(exactBytes);
  if (!exact) {
    ::free(dc.buf);
    return LoadError::OutOfMemory;
  }

  memcpy(exact, dc.buf, exactBytes);
  ::free(dc.buf);

  out.data = exact;
  out.frames = dc.written;
  out.sampleRate = dc.sampleRate;
  out.valid = true;
  return LoadError::Ok;
}

// Initialize a DecodeContext with a PSRAM buffer
static LoadError initContext(DecodeContext &dc, size_t maxDecodedFrames) {
  size_t bufBytes = maxDecodedFrames * sizeof(int16_t);
  int16_t *buf = (int16_t *)ps_malloc(bufBytes);
  if (!buf) return LoadError::OutOfMemory;

  dc.buf = buf;
  dc.capacity = maxDecodedFrames;
  dc.written = 0;
  dc.sampleRate = 44100;
  dc.channels = 1;
  dc.overflow = false;
  return LoadError::Ok;
}

// ----- Core decode: minimp3 over a contiguous MP3 buffer -----
// `mp3` may point at PROGMEM (ESP32-S3 maps flash into the address space, so
// minimp3's byte reads work directly) or a RAM/PSRAM buffer.
static LoadError decodeBuffer(const uint8_t *mp3, size_t mp3Size,
                              SampleBuffer &out, size_t maxDecodedFrames) {
  DecodeContext dc;
  LoadError err = initContext(dc, maxDecodedFrames);
  if (err != LoadError::Ok) return err;

  // mp3dec_t (~6.5 KB) + one frame of PCM (1152*2 samples). Heap, not stack.
  mp3dec_t *dec = (mp3dec_t *)malloc(sizeof(mp3dec_t));
  int16_t  *pcm = (int16_t *)malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t));
  if (!dec || !pcm) {
    if (dec) ::free(dec);
    if (pcm) ::free(pcm);
    ::free(dc.buf);
    return LoadError::OutOfMemory;
  }
  mp3dec_init(dec);

  mp3dec_frame_info_t info;
  const uint8_t *p = mp3;
  int remaining = (int)mp3Size;

  while (remaining > 0) {
    int samples = mp3dec_decode_frame(dec, p, remaining, pcm, &info);
    if (info.frame_bytes <= 0) break;     // no further frames found
    p         += info.frame_bytes;
    remaining -= info.frame_bytes;
    if (samples == 0) continue;           // skipped non-audio (ID3/junk)

    dc.sampleRate = (uint32_t)info.hz;
    dc.channels   = (uint8_t)info.channels;
    uint8_t ch = info.channels > 0 ? (uint8_t)info.channels : 1;
    for (int i = 0; i < samples; i++) {
      if (dc.written >= dc.capacity) { dc.overflow = true; break; }
      if (ch == 1) {
        dc.buf[dc.written++] = pcm[i];
      } else {
        // Downmix stereo to mono: average L+R
        int32_t mix = ((int32_t)pcm[i * ch] + (int32_t)pcm[i * ch + 1]) / 2;
        dc.buf[dc.written++] = (int16_t)mix;
      }
    }
    if (dc.overflow) break;
  }

  ::free(dec);
  ::free(pcm);
  return finalize(dc, out);
}

// ----- Flash MP3 decode -----

LoadError decodeFlashMP3(const uint8_t *flashData, size_t flashSize,
                         SampleBuffer &out,
                         size_t maxCompressedBytes,
                         size_t maxDecodedFrames) {
  out.free();

  if (!flashData || flashSize == 0) return LoadError::FileNotFound;
  if (flashSize > maxCompressedBytes) return LoadError::FileTooLarge;

  // PROGMEM is memory-mapped on the ESP32-S3 -- decode straight from flash.
  return decodeBuffer(flashData, flashSize, out, maxDecodedFrames);
}

// ----- SD card MP3 decode -----

LoadError decodeSDMP3(const char *path,
                      SampleBuffer &out,
                      size_t maxCompressedBytes,
                      size_t maxDecodedFrames) {
  out.free();

  if (!path || path[0] == '\0') return LoadError::FileNotFound;

  // Check if SD is accessible by trying to open the file
  File file = SD.open(path, FILE_READ);
  if (!file) {
    // Distinguish SD not mounted vs file not found
    File root = SD.open("/");
    if (!root) {
      return LoadError::SDNotMounted;
    }
    root.close();
    return LoadError::FileNotFound;
  }

  size_t fileSize = file.size();
  if (fileSize == 0) {
    file.close();
    return LoadError::FileReadError;
  }
  if (fileSize > maxCompressedBytes) {
    file.close();
    return LoadError::FileTooLarge;
  }

  // Read the whole file into PSRAM, then decode (SD isn't memory-mapped).
  uint8_t *comp = (uint8_t *)ps_malloc(fileSize);
  if (!comp) {
    file.close();
    return LoadError::OutOfMemory;
  }
  size_t rd = file.read(comp, fileSize);
  file.close();
  if (rd != fileSize) {
    ::free(comp);
    return LoadError::FileReadError;
  }

  LoadError err = decodeBuffer(comp, fileSize, out, maxDecodedFrames);
  ::free(comp);
  return err;
}

} // namespace ClipTheremin
