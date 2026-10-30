#ifndef APAUDIO_INTERNAL_HPP
#define APAUDIO_INTERNAL_HPP

// Internal header - never included outside ApricotFields/src/audio/.

#include "audio/apaudio.h"
#include "../aprioverlap.hpp"

// The guard of [mixer]'s control calls (../aprioverlap.hpp), which a cue
// table's control calls share: they create and destroy the mixer's sounds
// (apaudiocues.h, "Threads"). The mixer's struct is apaudio.cpp's own.
apri_overlap &apaudio_mixer_control_guard(apaudio_mixer mixer);

#endif // APAUDIO_INTERNAL_HPP
