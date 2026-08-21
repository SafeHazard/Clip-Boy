// On-screen status for the transmitter.
//
// This is not decoration. The badge has no other feedback channel that can be
// trusted: the USB serial console on this board does not reliably surface, so a
// firmware with no display looks exactly like a firmware that failed to boot.
// The screen is the boot indicator.
#pragma once

void sim_ui_init(void);
void sim_ui_tick(void);

// Show a fatal message instead of the status page, so a failed radio bring-up
// is visible on the badge rather than silent.
void sim_ui_fatal(const char* msg);
