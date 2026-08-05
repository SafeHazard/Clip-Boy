// minimp3_impl.cpp -- compiles the minimp3 decoder implementation exactly once.
//
// minimp3.h is implementation-guarded: the decoder code only compiles in a TU
// that defines MINIMP3_IMPLEMENTATION. CodecMP3Mini.h (included from
// audio_driver.h) pulls in only the declarations, so this dedicated TU provides
// the definitions. minimp3 is public domain (CC0) -- it replaced libhelix to
// drop the GPL/RPSL copyleft from the audio path.
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
