#pragma once
// ─── Firmware status-notice logging ──────────────────────────────────────────
// The serial port doubles as the surface players type the ARG / Marauder CLI
// into, so the chatty internal-state notices ([CFG]/[SS]/[EXIN]/[AUD]/[HR]/[CRT]/
// ...) are SILENCED in release builds. They are still emitted under:
//   - TEST_HARNESS (--test) — our serial test scripts parse them, and it's the
//     dev build, so the noise is welcome there;
//   - CLIPBOY_VERBOSE — an explicit opt-in for debugging a release issue.
// The user-facing CLI / ARG output ([CLIP]/[SIGNAL_9]/the puzzle text/the Marauder
// `help` + tool output) is NOT routed through these macros, so it always prints.
//
// Use: CB_LOGLN("[TAG] msg")   for Serial.println
//      CB_LOGF("[TAG] %d", x)  for Serial.printf
//      CB_LOGP("[TAG] ")       for Serial.print
// The no-op form consumes its args (so calls compile away cleanly); keep the
// args side-effect-free (they are just literals / variable reads).

// cb_serial_quiet gates ALL CB_LOG chatter at runtime. The harness sets it true
// around binary transfers (screenshot/audio) so the CORE-0 audio/neopixel tasks
// can't splice a log line into the length-delimited binary stream on Serial
// (the root cause of the USB-CDC test-session desync -- see the `quiet` cmd).
// Defined in ui_test.ino. Only odr-used when CB_LOG is non-no-op (test/verbose).
extern volatile bool cb_serial_quiet;

#if defined(TEST_HARNESS) || defined(CLIPBOY_VERBOSE)
  #define CB_LOGLN(x)   do { if (!cb_serial_quiet) Serial.println(x); } while (0)
  #define CB_LOGF(...)  do { if (!cb_serial_quiet) Serial.printf(__VA_ARGS__); } while (0)
  #define CB_LOGP(x)    do { if (!cb_serial_quiet) Serial.print(x); } while (0)
#else
  #define CB_LOGLN(x)   do {} while (0)
  #define CB_LOGF(...)  do {} while (0)
  #define CB_LOGP(x)    do {} while (0)
#endif
