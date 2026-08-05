#pragma once

#include <cstdint>
#include <cstddef>

namespace ClipTheremin {

enum class LoadError : uint8_t {
  Ok = 0,
  FileNotFound,
  FileReadError,
  DecodeFailed,
  FileTooLarge,
  OutOfMemory,
  SDNotMounted,
  InvalidSlot,
};

// Decoded PCM sample buffer (mono, 16-bit, in PSRAM)
struct SampleBuffer {
  int16_t *data = nullptr;    // PSRAM-allocated mono PCM
  size_t   frames = 0;        // Number of mono samples
  uint32_t sampleRate = 0;    // Original sample rate from MP3
  bool     valid = false;

  void free();
};

// Decode MP3 from flash (PROGMEM) into a SampleBuffer in PSRAM
// maxCompressedBytes: reject if source > this size
// maxDecodedFrames: truncate decoded output to this many mono samples
LoadError decodeFlashMP3(const uint8_t *flashData, size_t flashSize,
                         SampleBuffer &out,
                         size_t maxCompressedBytes,
                         size_t maxDecodedFrames);

// Decode MP3 from SD card file into a SampleBuffer in PSRAM
// Caller must have already mounted the SD card via SD.begin() or similar.
LoadError decodeSDMP3(const char *path,
                      SampleBuffer &out,
                      size_t maxCompressedBytes,
                      size_t maxDecodedFrames);

} // namespace ClipTheremin
