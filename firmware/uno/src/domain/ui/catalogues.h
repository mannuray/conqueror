#pragma once
#include <stdint.h>

#include "domain/ui/sound_sequencer.h"

namespace cq {

// Copies the notes of a catalogue sound (PROGMEM on the Uno) into `out`; returns the count (0 = unknown).
uint8_t soundNotes(proto::SoundId id, Note* out);

}  // namespace cq
