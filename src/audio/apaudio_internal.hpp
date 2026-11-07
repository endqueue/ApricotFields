#ifndef APAUDIO_INTERNAL_HPP
#define APAUDIO_INTERNAL_HPP

// Internal header - never included outside ApricotFields/src/audio/.

#include "audio/apaudio.h"
#include "../aprioverlap.hpp"

// The guard of [mixer]'s control calls (../aprioverlap.hpp), which a cue
// table's control calls share: they create and destroy the mixer's sounds
// (apaudiocues.h, "Threads"). The mixer's struct is apaudio.cpp's own.
apri_overlap &apaudio_mixer_control_guard(apaudio_mixer mixer);

// The windowed-sinc resampler's table (apaudio.cpp): a Kaiser-windowed sinc,
// APAUDIO_SINC_ZEROS zero crossings each side, APAUDIO_SINC_RESOLUTION
// entries per crossing. The mixer's and apaudio_pcm_resample()'s
// (apaudio_encode.cpp).
constexpr int APAUDIO_SINC_ZEROS = 8;
constexpr int APAUDIO_SINC_RESOLUTION = 256;

// The table: APAUDIO_SINC_ZEROS * APAUDIO_SINC_RESOLUTION entries and two
// zeros past the end, so an entry and the next can always be read. Freed
// with free(); null if there was no memory.
float *apaudio_sinc_table_build();

#endif // APAUDIO_INTERNAL_HPP
