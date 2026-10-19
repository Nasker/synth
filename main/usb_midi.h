#pragma once

#include <stdbool.h>

void usb_midi_start(void);
bool usb_midi_seen_activity(void);   // true once any MIDI message has been received
