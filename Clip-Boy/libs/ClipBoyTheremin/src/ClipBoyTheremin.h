#pragma once

// ClipBoyTheremin — Main include
// Multi-voice theremin using VL53L5CX 8x8 ToF sensor.
// Produces PCM int16_t samples; does NOT own I2S or sensor.

#include "CBTheremin.h"
#include "CBVoice.h"
#include "CBWaveforms.h"
#include "CBSampleDecoder.h"
