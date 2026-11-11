#ifndef RBTAUDIO_INTERNAL_HPP
#define RBTAUDIO_INTERNAL_HPP

// Internal header - never included outside rabbittus/src/audio/.

#include "audio/rbtaudio.h"
#include "../rbtoverlap.hpp"

// The guard of [mixer]'s control calls (../rbtoverlap.hpp), which a cue
// table's control calls share: they create and destroy the mixer's sounds
// (rbtaudiocues.h, "Threads"). The mixer's struct is rbtaudio.cpp's own.
rbt_overlap &rbtaudio_mixer_control_guard(rbtaudio_mixer mixer);

// The windowed-sinc resampler's table (rbtaudio.cpp): a Kaiser-windowed sinc,
// RBTAUDIO_SINC_ZEROS zero crossings each side, RBTAUDIO_SINC_RESOLUTION
// entries per crossing. The mixer's and rbtaudio_pcm_resample()'s
// (rbtaudio_encode.cpp).
constexpr int RBTAUDIO_SINC_ZEROS = 8;
constexpr int RBTAUDIO_SINC_RESOLUTION = 256;

// The table: RBTAUDIO_SINC_ZEROS * RBTAUDIO_SINC_RESOLUTION entries and two
// zeros past the end, so an entry and the next can always be read. Freed
// with free(); null if there was no memory.
float *rbtaudio_sinc_table_build();

#endif // RBTAUDIO_INTERNAL_HPP
