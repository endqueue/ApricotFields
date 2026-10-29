/*
 * Built in src/audio/apaudio.cpp, with headless tests in
 * tests/apaudio_tests.c. apricotfields.h doesn't include it yet.
 */

#ifndef APAUDIO_H
#define APAUDIO_H

#include "../apricore.h"
#include <spudaudio.h>
#include <stdbool.h>
#include <stdint.h>

#if __cplusplus
extern "C" {
#endif

/**
 * @file apaudio.h
 * @brief ApricotAudio ("ApAudio") - a real-time mixer over SpudAudio: voices,
 * each playing a decoded sound with a gain, a playback rate and optionally a
 * direction, mixed and panned for the output's real speaker layout.
 *
 * Voices are logical: many can exist (`max_voices`), but only the
 * `max_real_voices` that matter most are mixed. Which ones is decided by
 * priority first, then audibility; the rest are virtual - silent, and
 * depending on their sound's APAUDIO_VIRTUAL_BEHAVIOR either keeping their
 * place to come back later, or ended. Concurrency groups cap how many
 * voices of a kind (footsteps, detent ticks) play at once. There's no
 * separate voice stealing: a voice pushed out of the real ones is simply
 * virtualized, or ended, with a fade either way.
 *
 * Voices are routed to buses, a tree under the master bus (the mixer's
 * output). A bus's gain, rate, mute and pause pass down to every voice
 * under it; modifiers - snapshots, and ducking - scale buses together
 * without overwriting each other.
 *
 * General purpose: it knows nothing about what a sound means or when one
 * should play. That's the caller's. Naming sounds, and
 * defining them in sound banks, is apaudiocues.h, over this.
 *
 * It never touches a device. Like Aprend with SpudGPU, the caller makes
 * every SpudAudio decision - instance, device, format, period, thread
 * priority - creates the stream, and hands it apaudio_mixer_stream_callback()
 * with the mixer as user data. The caller also starts, stops, polls and,
 * after DEVICE_LOST, recreates the stream. Rendering is a plain call
 * (apaudio_mixer_render()), so a mixer runs without any device at all - e.g.
 * in tests, rendering into a buffer and checking the samples.
 *
 * Out of scope: streaming (sounds are whole, in memory), effects (filters,
 * reverb), distance attenuation (the caller sets the gain), and decoding
 * anything but WAV. Binaural (HRTF) rendering for headphones is a possible
 * later addition.
 *
 * @par Threads
 * apricore.h's rule ("Threads"), with a mixer shared on purpose between
 * three roles that may overlap each other. Every call names its role on its
 * "Thread:" line:
 * - control: one caller at a time across the mixer's control calls - the
 *   mixer's owner (e.g. a UI loop, a game's main loop), from whichever
 *   thread it calls, as long as no two control calls overlap. Creating and
 *   destroying mixers, sounds, concurrency groups, buses and modifiers,
 *   re-parenting buses, and updating the mixer (apaudio_mixer_update()).
 * - any: any number of callers at once, from any thread, overlapping each
 *   other and the control and audio calls - including input handlers and
 *   the audio thread itself. Never blocks, never allocates: playing,
 *   stopping and changing voices, changing buses' gain, rate, mute and
 *   pause, and activating modifiers.
 * - audio: one render of the mixer at a time (the stream's callback), on
 *   whichever thread the stream calls it from - "the audio thread" below -
 *   overlapping control and any calls. Never blocks, never allocates,
 *   never frees.
 *
 * ApAudio starts no thread: the audio thread is the stream's.
 *
 * @par Results
 * Calls return APRESULT (apricore.h). The general codes mean, here:
 * - APRESULT_INVALID_ARGUMENT: also a format the mixer can't render (no
 *   full channel layout, planar with more channels than it can address),
 *   or a bus parent that would make a cycle.
 * - APRESULT_OUT_OF_MEMORY: only when creating something, or in
 *   apaudio_mixer_set_format() - rendering and playing never allocate.
 * - APRESULT_LIMIT: too many plays waiting for the next render
 *   (apaudio_play() - a stop never fails), or every slot of its kind in use
 *   (apaudio_sound_create(), apaudio_concurrency_create(),
 *   apaudio_bus_create(), apaudio_modifier_create()).
 * - APRESULT_NOT_FOUND: a sound that's 0 or destroyed (apaudio_play()).
 * - APRESULT_UNSUPPORTED: a sound or WAV file this module doesn't handle
 *   (channel count, sample encoding).
 * - APRESULT_CORRUPT_DATA: a malformed WAV file.
 *
 * @par Error messages
 * Calls that can fail for a reason worth a sentence take an APAUDIO_ERROR
 * pointer (NULL to skip it), as aparchive.h's do.
 *
 * @par Struct bases
 * The descs start with a `base` (apricore.h, "Struct bases"): the caller
 * sets `base.type` to the desc's own APRI_STRUCT_TYPE_APAUDIO_* and
 * `base.next` to NULL. A wrong type is APRESULT_STRUCT_WRONG_TYPE and a
 * chained struct APRESULT_STRUCT_UNKNOWN_IN_CHAIN: nothing chains onto any
 * of them yet. APAUDIO_ERROR, APAUDIO_FORMAT, APAUDIO_MODIFIER_TARGET,
 * APAUDIO_DIRECTION and APAUDIO_PCM are frozen: they never grow (a new
 * need gets a new type), which is what lets a desc hold one by value. No
 * desc holds another module's struct by value - its layout isn't ApAudio's
 * to keep fixed - so SpudAudio's formats come in through APAUDIO_FORMAT.
 *
 * @par Prioritization
 * Which voices are real (mixed) and which virtual.
 * @par
 * Voices are ranked by priority (APAUDIO_PLAY_DESC `priority`), then by
 * audibility: the voice's current gain, times its bus chain's gain and
 * active modifiers (see APAUDIO_BUS), times its sound's loudness (RMS,
 * measured when the sound is created) - so turning a bus down or muting it
 * frees its voices' mixing slots. The top `max_real_voices` are real; the
 * rest are virtual, and so is any voice quieter than the mixer's
 * `virtual_threshold` even when real slots are free. A voice never loses
 * its real slot to one of lower priority. Ties within a few dB, and a voice
 * real for less than `min_real_us`, don't swap, so near-equal voices don't
 * trade places every update. A voice on a muted or paused bus is virtual
 * whatever its rank, keeping its place (as PAUSE, whatever its own
 * behavior) until the bus is unmuted or resumed.
 * @par
 * A voice becoming virtual fades out, and one becoming real fades in, over
 * `transition_fade_us`. A voice pushed out to make room keeps mixing while
 * it fades, in one of `max_fading_voices` extra slots, so the voice taking
 * its place starts at once; with those all in use, the oldest fade is cut
 * short.
 * @par
 * Ranking runs in apaudio_mixer_update(), a control call. A new play
 * doesn't wait for it: the render starts it at once in a free real slot,
 * or in place of the lowest-ranked real voice (as of the last update) if it
 * outranks it; otherwise it starts virtual. The next update ranks it
 * properly.
 *
 * @par Changing a bus
 * Thread: any. Like voice changes, recorded on the bus itself, not queued -
 * never lost, latest value wins - and applied over `fade_us` from the next
 * render (0: across that render only, so it still doesn't click). These set
 * the bus's own values; modifiers scale them without changing them. Volume
 * sliders belong here; anything that comes and goes - a pause menu, a
 * cutscene, ducking - belongs in a modifier, so it can't undo another
 * system's change.
 * @par
 * INVALID_ARGUMENT for a value its desc would reject; NOT_FOUND for a
 * stale or 0 bus.
 *
 * @par Changing a voice
 * Changing a voice as it plays: its gain, direction and playback rate, as
 * APAUDIO_PLAY_DESC set them - e.g. following the camera as it orbits
 * during a sound.
 * @par
 * Thread: any. Like a stop, a change is recorded on the voice itself, not
 * queued: it never waits for queue space and can't be lost. Each setting
 * holds only its latest value - changes made between two renders
 * coalesce, and the next render applies the last one, ramped across that
 * render so it doesn't click or zipper. A voice changed before it starts
 * starts with the new value.
 * @par
 * APRESULT_INVALID_ARGUMENT for a null mixer or a value APAUDIO_PLAY_DESC
 * would reject; then nothing changes. A voice that's stopping, finished
 * or unknown: APRESULT_OK, nothing to do - a stopping voice keeps the
 * values it's fading out with.
 */

/* ---- Conventions ----------------------------------------------------- */

/** @brief Size of APAUDIO_ERROR's `message`, its terminating NUL included. */
#define APAUDIO_ERROR_MESSAGE_SIZE 256

/**
 * @brief Why a call failed, in a sentence. Filled per call.
 */
/* Frozen: filled per call. It never changes; a new need gets a new type. */
typedef struct APAUDIO_ERROR {
	/** NUL-terminated, truncated to fit. */
	char message[APAUDIO_ERROR_MESSAGE_SIZE];
} APAUDIO_ERROR;

/* ---- Formats --------------------------------------------------------- */

/**
 * @brief What a mixer writes.
 *
 * The fields mean what SPUDAUDIO_FORMAT's do, and the enums are SpudAudio's
 * own (plain uint32_t / uint8_t values, only ever added to) - only the
 * layout is ApAudio's.
 */
/* Frozen: a value, held by value in APAUDIO_MIXER_DESC. It never changes; a new need gets a new type. */
typedef struct APAUDIO_FORMAT {
	/**
	 * Any SpudAudio sample format. Mixing is in float; integer formats are
	 * clamped (hard-clipped) on conversion, with no limiter: keeping the sum
	 * in range is the caller's, through its gains.
	 */
	SPUDAUDIO_SAMPLE_FORMAT sample_format;
	/** Frames per second. */
	uint32_t sample_rate;
	/** How many channels, and how many entries #positions holds. */
	uint32_t channel_count;
	/** true: one interleaved buffer. false: planar, one buffer per channel. */
	bool interleaved;
	/**
	 * #channel_count channel positions, a full layout (no UNSPECIFIED),
	 * copied by the call it's passed to. A stream granted without a layout
	 * needs the caller to say what its channels are - point #positions at
	 * e.g. {FL, FR} - since only the caller knows what it's willing to
	 * assume.
	 */
	const SPUDAUDIO_CHANNEL_POSITION *positions;
} APAUDIO_FORMAT;

/**
 * @brief Converts a SpudAudio format to an APAUDIO_FORMAT.
 *
 * Inline, so it's compiled with the caller's spudaudio.h and reads the
 * caller's SPUDAUDIO_FORMAT as the caller built it, whatever version
 * ApricotFields was built with.
 *
 * @param[in] format The format, e.g. a stream's granted
 *                   SPUDAUDIO_STREAM_CONFIG `format`. Not NULL.
 *
 * @return @p format as an APAUDIO_FORMAT. It points into @p format's
 *         `positions`: keep @p format alive until the call taking the
 *         result returns.
 */
static inline APAUDIO_FORMAT apaudio_format_from_spudaudio(const SPUDAUDIO_FORMAT *format)
{
	APAUDIO_FORMAT result;
	result.sample_format = format->sample_format;
	result.sample_rate = format->sample_rate;
	result.channel_count = format->channel_count;
	result.interleaved = format->interleaved;
	result.positions = format->positions;
	return result;
}

/* ---- Mixer ----------------------------------------------------------- */

/**
 * @brief Opaque handle to a mixer: its voices, sounds, buses, modifiers and
 * concurrency groups.
 */
typedef struct apaudio_mixer_t *apaudio_mixer;

/**
 * @brief What a voice does while it isn't real.
 *
 * Chosen per sound, overridable per play: a UI click and a music loop want
 * opposite things. See this file's "Prioritization".
 */
typedef uint32_t APAUDIO_VIRTUAL_BEHAVIOR;
enum {
	/** Rejected for a sound; for a play, "the sound's". */
	APAUDIO_VIRTUAL_UNSPECIFIED = 0,
	/**
	 * Ends instead of going virtual: one-shots and UI sounds, which are
	 * wrong if they come back late.
	 */
	APAUDIO_VIRTUAL_KILL = 1,
	/**
	 * Keeps its place, silently, and comes back where it would have been:
	 * music, ambience, dialogue. A one-shot that would come back with less
	 * than `transition_fade_us` left ends instead.
	 */
	APAUDIO_VIRTUAL_CONTINUE = 2,
	/** Comes back from the start: loops whose phase doesn't matter. */
	APAUDIO_VIRTUAL_RESTART = 3,
	/** Freezes its place while virtual, and comes back from there. */
	APAUDIO_VIRTUAL_PAUSE = 4,
};

/**
 * @brief How voices are resampled as they play, from their sound's own rate
 * (times their playback rate) to the mixer's.
 *
 * A sound is never converted when it's created: it keeps its own rate, so
 * it survives the mixer changing format (apaudio_mixer_set_format()), and
 * voices can play it at any rate. A voice whose rates match is copied, not
 * resampled.
 */
typedef uint32_t APAUDIO_RESAMPLER;
enum {
	/** Rejected (zeroed descs choose nothing). */
	APAUDIO_RESAMPLER_UNSPECIFIED = 0,
	/**
	 * Linear interpolation: cheap; dulls the top octave and aliases when
	 * playing faster than the mixer's rate.
	 */
	APAUDIO_RESAMPLER_LINEAR = 1,
	/**
	 * Windowed sinc, band-limited to whichever rate is lower so playing
	 * faster doesn't alias: what a UI sound normally wants.
	 */
	APAUDIO_RESAMPLER_SINC = 2,
};

/**
 * @brief Configuration descriptor for a mixer.
 */
typedef struct APAUDIO_MIXER_DESC {
	/** This struct's type, APRI_STRUCT_TYPE_APAUDIO_MIXER_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/**
	 * What the mixer writes: the stream's granted format, usually through
	 * apaudio_format_from_spudaudio().
	 */
	APAUDIO_FORMAT format;
	/**
	 * The most frames one render may ask for: the stream's
	 * SPUDAUDIO_STREAM_CONFIG `max_callback_frames`. Sizes the mixer's
	 * scratch buffers once, here.
	 */
	uint32_t max_frames;
	/**
	 * Voices: how many can exist at once, real or virtual. Cheap - a record
	 * each, no mixing.
	 */
	uint32_t max_voices;
	/** How many voices are mixed at once: the CPU budget. At most #max_voices. */
	uint32_t max_real_voices;
	/**
	 * Extra mixing slots for voices fading out after losing their real
	 * slot (see this file's "Prioritization").
	 */
	uint32_t max_fading_voices;
	/**
	 * A real voice quieter than this (linear gain, e.g. 0.001 for -60 dB)
	 * goes virtual even with real slots free. 0: only a lack of slots
	 * makes a voice virtual.
	 */
	float virtual_threshold;
	/** How long a voice fades out going virtual, or in coming back. */
	uint32_t transition_fade_us;
	/**
	 * How long a voice stays real before it can be pushed out by one of
	 * equal priority.
	 */
	uint32_t min_real_us;
	/** Sound slots: how many sounds can exist at once (APAUDIO_SOUND). */
	uint32_t max_sounds;
	/** Concurrency group slots (APAUDIO_CONCURRENCY). */
	uint32_t max_concurrency_groups;
	/** Bus slots (APAUDIO_BUS), besides the master. */
	uint32_t max_buses;
	/** Modifier slots (APAUDIO_MODIFIER). */
	uint32_t max_modifiers;
	/**
	 * Plays that can wait for the next render before APRESULT_LIMIT. At
	 * least #max_voices is sensible. Stops don't count: they're recorded on
	 * the voice itself (apaudio_voice_stop()).
	 */
	uint32_t queue_capacity;
	/** How voices are resampled: LINEAR or SINC. */
	APAUDIO_RESAMPLER resampler;
	/**
	 * The master bus's starting gain, linear: 1 leaves the mix as it is.
	 * Finite, >= 0.
	 */
	float gain;
} APAUDIO_MIXER_DESC;

/**
 * @brief Creates a mixer.
 *
 * Thread: control. Allocates everything the mixer will need while
 * rendering.
 *
 * @param[in]  desc      Mixer configuration. Only read during the call.
 * @param[out] out_mixer The new mixer; set to NULL on failure.
 * @param[out] error     Why it failed, if it did. May be NULL.
 *
 * @return APRESULT_OK, or the failure (this file's "Results").
 *
 * @see apaudio_mixer_destroy()
 */
APRESULT apaudio_mixer_create(
    const APAUDIO_MIXER_DESC *desc,
    apaudio_mixer *out_mixer,
    APAUDIO_ERROR *error);
/**
 * @brief Destroys a mixer and every sound made from it.
 *
 * Thread: control.
 *
 * @warning Stop (or destroy) the stream rendering it first: nothing may be
 * rendering it, or about to.
 *
 * @param[in] mixer Mixer to destroy. NULL is accepted and does nothing.
 */
void apaudio_mixer_destroy(apaudio_mixer mixer);

/**
 * @brief Changes what the mixer writes - for a stream recreated on another
 * device, or after the device's rate changed.
 *
 * Thread: control. The same rules apply as to APAUDIO_MIXER_DESC's `format`
 * and `max_frames`. Sounds are kept (they hold their own rate), and so are
 * voices: one playing carries on from where it was, at the new rate. On
 * failure the mixer is unchanged.
 *
 * @warning Nothing may be rendering the mixer, or about to: call it between
 * stopping the old stream and starting the new one.
 *
 * @param[in]  mixer      Mixer to change.
 * @param[in]  format     What it writes from now on, as APAUDIO_MIXER_DESC's
 *                        `format`.
 * @param[in]  max_frames As APAUDIO_MIXER_DESC's `max_frames`.
 * @param[out] error      Why it failed, if it did. May be NULL.
 *
 * @return APRESULT_OK, or the failure (this file's "Results").
 */
APRESULT apaudio_mixer_set_format(
    apaudio_mixer mixer,
    const APAUDIO_FORMAT *format,
    uint32_t max_frames,
    APAUDIO_ERROR *error);

/**
 * @brief Ranks every voice and decides which are real (this file's
 * "Prioritization"), applies concurrency groups' limits, starts and ends
 * triggered modifiers (ducking), and frees the memory of destroyed sounds
 * the audio thread has let go of.
 *
 * Thread: control. Call it once a frame - a main loop, a render loop.
 * Without it, new plays still start at once (the render's fast path), but
 * virtual voices never come back and destroyed sounds' memory waits for the
 * next control call. Never blocks on the audio thread.
 *
 * @param[in] mixer Mixer to update.
 */
void apaudio_mixer_update(apaudio_mixer mixer);

/* ---- Rendering ------------------------------------------------------- */

/**
 * @brief Mixes the real voices' next frames into a buffer, and advances the
 * virtual ones' places.
 *
 * Thread: audio. Applies the plays queued, and the stops and voice changes
 * recorded, since the last render first. Silence when nothing plays.
 *
 * @param[in]  mixer       Mixer to render.
 * @param[out] frames      Where the frames go, in the mixer's format,
 *                         replacing what's there: one buffer if
 *                         interleaved, else void*[channel_count].
 * @param[in]  frame_count How many frames to render: at most the desc's
 *                         `max_frames`.
 * @param[in]  info        The stream callback's info. May be NULL. With a
 *                         valid host time, voices with a start time
 *                         (APAUDIO_PLAY_DESC `start_time_ns`) begin on the
 *                         frame matching it.
 */
void apaudio_mixer_render(
    apaudio_mixer mixer,
    void *frames,
    uint32_t frame_count,
    const SPUDAUDIO_CALLBACK_INFO *info);

/**
 * @brief A SPUDAUDIO_STREAM_CALLBACK that renders the mixer passed as
 * @p user_data.
 *
 * Thread: audio. Pass it, and the mixer, in SPUDAUDIO_STREAM_DESC. The
 * stream's granted format and `max_callback_frames` must be the mixer's
 * (as created, or as last set by apaudio_mixer_set_format()).
 *
 * @param[in]  stream      The stream calling it.
 * @param[out] frames      The stream's buffer, rendered into as by
 *                         apaudio_mixer_render().
 * @param[in]  frame_count How many frames the stream asks for.
 * @param[in]  info        The stream's callback info.
 * @param[in]  user_data   The apaudio_mixer to render.
 */
void apaudio_mixer_stream_callback(
    spudaudio_stream stream,
    void *frames,
    uint32_t frame_count,
    const SPUDAUDIO_CALLBACK_INFO *info,
    void *user_data);

/* ---- Buses ----------------------------------------------------------- */

/**
 * @brief A bus: a node in a tree under the master bus, which is the mixer's
 * output.
 *
 * Every voice is routed to one bus. A generational ID like APAUDIO_SOUND:
 * never 0, stale at once when destroyed.
 *
 * What a bus does comes in two kinds:
 * - Properties - gain, rate, mute, pause - pass down to every voice under
 *   it, through child buses: a voice's effective gain is its own times
 *   every bus's on its way to the master (and their modifiers'), and the
 *   same for rate. Rate is a property, not processing, because slowing
 *   down a bus means slowing down each voice; a mix can't be resampled
 *   into that. Gain is linear, so a bus with only properties needs no
 *   mixing of its own: its gain is folded into its voices'.
 * - Processing on the bus's own mix - effects and metering, still to
 *   come. Only a bus with some gets a mix buffer.
 *
 * Every bus mixes in the output's layout.
 */
typedef uint64_t APAUDIO_BUS;

/**
 * @brief Gets the master bus: the mixer's output, the root of the tree.
 *
 * Thread: any. It can't be destroyed or re-parented. Where a desc says 0
 * means the master, this is the same bus; pass it explicitly to route a
 * play there when its sound names another.
 *
 * @param[in] mixer Mixer whose master bus to get.
 *
 * @return The master bus, or 0 if @p mixer is NULL.
 */
APAUDIO_BUS apaudio_mixer_get_master_bus(apaudio_mixer mixer);

/**
 * @brief Configuration descriptor for a bus.
 */
typedef struct APAUDIO_BUS_DESC {
	/** This struct's type, APRI_STRUCT_TYPE_APAUDIO_BUS_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** The bus it sits under. 0: the master. */
	APAUDIO_BUS parent;
	/** Linear; finite, >= 0. */
	float gain;
	/**
	 * A rate every voice under it plays at, on top of its own: 1 plays them
	 * as they are. Finite, in [0.125, 8].
	 */
	float rate;
} APAUDIO_BUS_DESC;

/**
 * @brief Creates a bus.
 *
 * Thread: control. Usable at once - a play routed to it straight after
 * plays there, never through the master instead: creating a bus publishes
 * the tree to the audio thread itself, without waiting for an update.
 *
 * @param[in]  mixer   Mixer to create it in.
 * @param[in]  desc    Bus configuration. Only read during the call.
 * @param[out] out_bus The new bus; set to 0 on failure.
 * @param[out] error   Why it failed, if it did. May be NULL.
 *
 * @return APRESULT_OK, or the failure (this file's "Results").
 *
 * @see apaudio_bus_destroy()
 */
APRESULT apaudio_bus_create(
    apaudio_mixer mixer,
    const APAUDIO_BUS_DESC *desc,
    APAUDIO_BUS *out_bus,
    APAUDIO_ERROR *error);
/**
 * @brief Destroys a bus.
 *
 * Thread: control. @p bus's ID is stale at once. Its voices and child buses
 * move to its parent rather than going silent - a game rebuilding its mix
 * at a level change doesn't cut sounds off - and modifiers' targets naming
 * it are dropped.
 *
 * @param[in] mixer Mixer it belongs to.
 * @param[in] bus   Bus to destroy. A stale, 0 or master bus does nothing.
 */
void apaudio_bus_destroy(apaudio_mixer mixer, APAUDIO_BUS bus);
/**
 * @brief Moves a bus, with its voices and children, under another.
 *
 * Thread: control.
 *
 * @param[in] mixer  Mixer they belong to.
 * @param[in] bus    Bus to move.
 * @param[in] parent Its new parent. 0: the master.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT if @p parent is @p bus or
 *         under it; APRESULT_NOT_FOUND if either is stale.
 */
APRESULT apaudio_bus_set_parent(apaudio_mixer mixer, APAUDIO_BUS bus, APAUDIO_BUS parent);

/**
 * @brief Sets a bus's own gain.
 *
 * The rules are in this file's "Changing a bus".
 *
 * @param[in] mixer   Mixer it belongs to.
 * @param[in] bus     Bus to change.
 * @param[in] gain    As APAUDIO_BUS_DESC `gain`.
 * @param[in] fade_us How long the change takes, from the next render.
 *
 * @return APRESULT_OK, APRESULT_INVALID_ARGUMENT or APRESULT_NOT_FOUND.
 */
APRESULT apaudio_bus_set_gain(apaudio_mixer mixer, APAUDIO_BUS bus, float gain, uint32_t fade_us);
/**
 * @brief Sets a bus's own rate.
 *
 * The rules are in this file's "Changing a bus".
 *
 * @param[in] mixer   Mixer it belongs to.
 * @param[in] bus     Bus to change.
 * @param[in] rate    As APAUDIO_BUS_DESC `rate`.
 * @param[in] fade_us How long the change takes, from the next render.
 *
 * @return APRESULT_OK, APRESULT_INVALID_ARGUMENT or APRESULT_NOT_FOUND.
 */
APRESULT apaudio_bus_set_rate(apaudio_mixer mixer, APAUDIO_BUS bus, float rate, uint32_t fade_us);
/**
 * @brief Mutes or unmutes a bus.
 *
 * The rules are in this file's "Changing a bus".
 *
 * @param[in] mixer   Mixer it belongs to.
 * @param[in] bus     Bus to change.
 * @param[in] muted   Whether it's muted.
 * @param[in] fade_us How long the change takes, from the next render.
 *
 * @return APRESULT_OK, APRESULT_INVALID_ARGUMENT or APRESULT_NOT_FOUND.
 */
APRESULT apaudio_bus_set_muted(apaudio_mixer mixer, APAUDIO_BUS bus, bool muted, uint32_t fade_us);
/**
 * @brief Pauses or resumes a bus.
 *
 * Pausing freezes every voice under the bus in place - a game's pause menu,
 * while the UI bus carries on - fading out over @p fade_us; resuming fades
 * them back in from where they were. The rules are in this file's
 * "Changing a bus".
 *
 * @param[in] mixer   Mixer it belongs to.
 * @param[in] bus     Bus to change.
 * @param[in] paused  Whether it's paused.
 * @param[in] fade_us How long the change takes, from the next render.
 *
 * @return APRESULT_OK, APRESULT_INVALID_ARGUMENT or APRESULT_NOT_FOUND.
 */
APRESULT apaudio_bus_set_paused(apaudio_mixer mixer, APAUDIO_BUS bus, bool paused, uint32_t fade_us);

/* ---- Modifiers -------------------------------------------------------- */

/**
 * @brief A modifier: a set of bus gains and rates that scales those buses
 * while it's active.
 *
 * A snapshot (pause menu, cutscene, underwater), or, with a trigger,
 * ducking (dialogue lowering music). Each fades in and out on its own. A
 * bus's effective gain is its own times every active modifier's for it,
 * and the same for rate. Multiplying is order-independent, so independent
 * systems never undo each other: a cutscene ending restores music to
 * whatever the pause menu has it at, not to what it was when the cutscene
 * began. A generational ID like APAUDIO_BUS.
 */
typedef uint64_t APAUDIO_MODIFIER;

/**
 * @brief One bus's scaling.
 */
/* Frozen: an array element. It never changes; a new need gets a new type. */
typedef struct APAUDIO_MODIFIER_TARGET {
	/** The bus it scales. */
	APAUDIO_BUS bus;
	/** Multiplies the bus's gain: linear, finite, >= 0. */
	float gain;
	/** Multiplies the bus's rate: finite, in [0.125, 8]. */
	float rate;
} APAUDIO_MODIFIER_TARGET;

/**
 * @brief Configuration descriptor for a modifier.
 */
typedef struct APAUDIO_MODIFIER_DESC {
	/** This struct's type, APRI_STRUCT_TYPE_APAUDIO_MODIFIER_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** #target_count targets, copied; at most one per bus. */
	const APAUDIO_MODIFIER_TARGET *targets;
	/** How many entries #targets holds. */
	uint32_t target_count;
	/** How long its effect takes to fade in when it becomes active. */
	uint32_t fade_in_us;
	/** How long its effect takes to fade out when it stops being active. */
	uint32_t fade_out_us;
	/**
	 * 0: active only through apaudio_modifier_set_active(). A bus: also
	 * active while that bus has audible voices - real, and above the
	 * mixer's `virtual_threshold`; virtual ones don't count. That's ducking.
	 * Decided in apaudio_mixer_update() from the previous update's voices,
	 * so two buses ducking each other can't loop within a frame.
	 */
	APAUDIO_BUS trigger;
} APAUDIO_MODIFIER_DESC;

/**
 * @brief Creates a modifier. A new modifier is inactive.
 *
 * Thread: control.
 *
 * @param[in]  mixer        Mixer to create it in.
 * @param[in]  desc         Modifier configuration. Only read during the call.
 * @param[out] out_modifier The new modifier; set to 0 on failure.
 * @param[out] error        Why it failed, if it did. May be NULL.
 *
 * @return APRESULT_OK, or the failure (this file's "Results").
 *
 * @see apaudio_modifier_destroy()
 */
APRESULT apaudio_modifier_create(
    apaudio_mixer mixer,
    const APAUDIO_MODIFIER_DESC *desc,
    APAUDIO_MODIFIER *out_modifier,
    APAUDIO_ERROR *error);
/**
 * @brief Destroys a modifier.
 *
 * Thread: control. Its effect fades out over its `fade_out_us`; its ID is
 * stale at once.
 *
 * @param[in] mixer    Mixer it belongs to.
 * @param[in] modifier Modifier to destroy. A stale or 0 one does nothing.
 */
void apaudio_modifier_destroy(apaudio_mixer mixer, APAUDIO_MODIFIER modifier);
/**
 * @brief Activates or deactivates a modifier.
 *
 * Thread: any. Fades over its `fade_in_us` or `fade_out_us` from the next
 * render. A triggered modifier is active while either this or its trigger
 * says so. Recorded on the modifier, not queued: never lost.
 *
 * @param[in] mixer    Mixer it belongs to.
 * @param[in] modifier Modifier to change.
 * @param[in] active   Whether it's active.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT for a null @p mixer;
 *         APRESULT_NOT_FOUND for a stale or 0 @p modifier.
 */
APRESULT apaudio_modifier_set_active(apaudio_mixer mixer, APAUDIO_MODIFIER modifier, bool active);

/* ---- Concurrency groups ---------------------------------------------- */

/**
 * @brief A limit on how many voices of a kind play at once - every footstep
 * variant together, or a tool's detent ticks - shared by every sound and
 * play that names it.
 *
 * Where the real-voice limit is a CPU budget and never ends a voice by
 * itself, a group's limit is a design rule: what it stops, stays stopped. A
 * generational ID like APAUDIO_SOUND: never 0; stale once destroyed, and
 * then a sound or play naming it plays with no group.
 */
typedef uint64_t APAUDIO_CONCURRENCY;

/**
 * @brief What a full group does with a new play.
 */
typedef uint32_t APAUDIO_RESOLUTION;
enum {
	/** Rejected. */
	APAUDIO_RESOLUTION_UNSPECIFIED = 0,
	/** The new play is dropped. */
	APAUDIO_RESOLUTION_REJECT_NEW = 1,
	/**
	 * The group's voice that started longest ago stops, fading over
	 * `transition_fade_us`.
	 */
	APAUDIO_RESOLUTION_STOP_OLDEST = 2,
	/** The group's least audible voice stops. */
	APAUDIO_RESOLUTION_STOP_QUIETEST = 3,
	/**
	 * The group's lowest-priority voice stops, if it's lower than or equal
	 * to the new one's; otherwise the new play is dropped. Ties go to the
	 * oldest.
	 */
	APAUDIO_RESOLUTION_STOP_LOWEST_PRIORITY = 4,
};

/**
 * @brief Configuration descriptor for a concurrency group.
 */
typedef struct APAUDIO_CONCURRENCY_DESC {
	/** This struct's type, APRI_STRUCT_TYPE_APAUDIO_CONCURRENCY_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** Voices of the group that can exist at once, real or virtual. At least 1. */
	uint32_t max_instances;
	/** What the group does with a new play when full. */
	APAUDIO_RESOLUTION resolution;
	/**
	 * A play coming less than this after the group's last start is
	 * dropped, so one gesture can't fire the same tick twice in a buffer.
	 * 0: none.
	 */
	uint32_t retrigger_us;
	/**
	 * Each newer start scales the group's older voices' gain by this, so a
	 * pile-up stays clear instead of building. 1: none; in (0, 1].
	 */
	float older_instance_gain;
} APAUDIO_CONCURRENCY_DESC;

/**
 * @brief Creates a concurrency group.
 *
 * Thread: control.
 *
 * @param[in]  mixer     Mixer to create it in.
 * @param[in]  desc      Group configuration. Only read during the call.
 * @param[out] out_group The new group; set to 0 on failure.
 * @param[out] error     Why it failed, if it did. May be NULL.
 *
 * @return APRESULT_OK, or the failure (this file's "Results").
 *
 * @see apaudio_concurrency_destroy()
 */
APRESULT apaudio_concurrency_create(
    apaudio_mixer mixer,
    const APAUDIO_CONCURRENCY_DESC *desc,
    APAUDIO_CONCURRENCY *out_group,
    APAUDIO_ERROR *error);
/**
 * @brief Destroys a concurrency group.
 *
 * Thread: control. The group's ID is stale at once; its voices carry on,
 * ungrouped.
 *
 * @param[in] mixer Mixer it belongs to.
 * @param[in] group Group to destroy. A stale or 0 one does nothing.
 */
void apaudio_concurrency_destroy(apaudio_mixer mixer, APAUDIO_CONCURRENCY group);

/* ---- Sounds ---------------------------------------------------------- */

/**
 * @brief A sound: an ID, not a pointer - a slot in the mixer's sound table
 * (sized by `max_sounds`, allocated at creation) plus that slot's reuse
 * count.
 *
 * Never 0. Destroying a sound bumps its slot's count, so its ID goes stale
 * at once: any call given it afterwards - from any thread, racing the
 * destroy or long after - is turned away, never sent into freed memory.
 * That's what lets apaudio_play() be safe from any thread while sounds come
 * and go.
 *
 * Thread: an ID is a value, and may be handed to a call from any thread at
 * any time; one that went stale meanwhile, even by a destroy that overlaps
 * the call, is turned away. Which calls may overlap is each call's own
 * "Thread:" line.
 *
 * A slot is reused only once the audio thread has confirmed it's done with
 * the sound before, and a reused slot has a new count, so an old ID never
 * plays the new sound. IDs are only checked against the mixer they're
 * given to: another mixer's ID is a caller error - NOT_FOUND, or whatever
 * this mixer holds in that slot - though never memory-unsafe.
 */
typedef uint64_t APAUDIO_SOUND;

/**
 * @brief Configuration descriptor for a sound.
 */
typedef struct APAUDIO_SOUND_DESC {
	/** This struct's type, APRI_STRUCT_TYPE_APAUDIO_SOUND_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/**
	 * #frame_count frames of #channel_count interleaved samples at
	 * #sample_rate Hz, nominally in [-1, 1]. Copied.
	 */
	const float *samples;
	/** How many frames #samples holds. */
	uint64_t frame_count;
	/**
	 * 1: mono - can be played from a direction. 2: stereo (left, right) -
	 * plays on the output's FL and FR, or mixed down on a mono output;
	 * ignores direction. Others: APRESULT_UNSUPPORTED.
	 */
	uint32_t channel_count;
	/**
	 * The samples' own rate, kept: voices resample from it as they play
	 * (APAUDIO_RESAMPLER). 1000 to 768000 Hz.
	 */
	uint32_t sample_rate;
	/** What its voices do while not real; required. */
	APAUDIO_VIRTUAL_BEHAVIOR virtual_behavior;
	/** The group its plays join unless a play names another; 0 for none. */
	APAUDIO_CONCURRENCY concurrency;
	/** The bus its plays go to unless a play names another; 0 for the master. */
	APAUDIO_BUS bus;
} APAUDIO_SOUND_DESC;

/**
 * @brief Creates a sound the mixer can play, at its own rate.
 *
 * Thread: control. Its loudness, for ranking voices, is measured here,
 * once.
 *
 * @param[in]  mixer     Mixer to create it in.
 * @param[in]  desc      Sound configuration. Only read during the call.
 * @param[out] out_sound The new sound; set to 0 on failure.
 * @param[out] error     Why it failed, if it did. May be NULL.
 *
 * @return APRESULT_OK, or the failure (this file's "Results").
 *
 * @see apaudio_sound_destroy()
 */
APRESULT apaudio_sound_create(
    apaudio_mixer mixer,
    const APAUDIO_SOUND_DESC *desc,
    APAUDIO_SOUND *out_sound,
    APAUDIO_ERROR *error);
/**
 * @brief Destroys a sound.
 *
 * Thread: control. @p sound's ID is stale at once (see APAUDIO_SOUND), and
 * voices playing it stop at the next render. Its memory is freed, and its
 * slot reusable, once the audio thread has confirmed it's done with it -
 * by a later control call on the mixer (the audio thread never
 * frees), or with the mixer.
 *
 * @param[in] mixer Mixer it belongs to.
 * @param[in] sound Sound to destroy. A stale or 0 one does nothing.
 */
void apaudio_sound_destroy(apaudio_mixer mixer, APAUDIO_SOUND sound);

/* ---- Voices ---------------------------------------------------------- */

/**
 * @brief One playing of a sound - a logical voice.
 *
 * Never 0, never reused within a mixer's life, so a stale one is harmless:
 * stopping a voice that has finished does nothing.
 *
 * A voice exists from the moment apaudio_play() returns it - before the
 * audio thread has started it, and whether it's real or virtual - and
 * keeps its ID through every change between the two. It holds its own stop
 * request and latest changes (apaudio_voice_set_gain() and the others), so
 * stopping or changing one never waits for queue space and can't be lost:
 * a looping voice always ends when told to. (The mixer's voice records are
 * allocated at creation, `max_voices` + `queue_capacity` of them, which is
 * how a play that's accepted always has one.)
 *
 * Thread: an ID is a value, and may be handed to the "any" calls from any
 * thread, at the same time as the control calls and the audio thread's
 * render.
 */
typedef uint64_t APAUDIO_VOICE;

/**
 * @brief Where a sound comes from, relative to the listener facing the
 * screen, in radians.
 *
 * Rendered for the mixer's layout by amplitude panning between the nearest
 * speakers (constant power; on stereo that's ordinary stereo panning). LFE,
 * NA and AUX channels get none of it. A layout without height speakers
 * ignores `elevation`; one without rear speakers (stereo) folds sounds
 * behind to the front, mirrored, since it can't place them behind.
 */
/* Frozen: a value. It never changes; a new need gets a new type. */
typedef struct APAUDIO_DIRECTION {
	/** 0 ahead, positive to the right, +-pi behind. */
	double azimuth;
	/** Positive up, in [-pi/2, pi/2]. */
	double elevation;
} APAUDIO_DIRECTION;

/**
 * @brief How apaudio_play() plays a sound.
 */
typedef struct APAUDIO_PLAY_DESC {
	/** This struct's type, APRI_STRUCT_TYPE_APAUDIO_PLAY_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** Linear; finite, >= 0. */
	float gain;
	/**
	 * 0 lowest .. 255 highest. Ranks first, before audibility: a voice
	 * never loses its real slot to one of lower priority.
	 */
	uint8_t priority;
	/** UNSPECIFIED (0): the sound's. */
	APAUDIO_VIRTUAL_BEHAVIOR virtual_behavior;
	/** 0: the sound's group. */
	APAUDIO_CONCURRENCY concurrency;
	/**
	 * 0: the sound's bus (apaudio_mixer_get_master_bus() for the master
	 * explicitly). A stale bus: the master.
	 */
	APAUDIO_BUS bus;
	/**
	 * Playback rate: 1 plays the sound as recorded, 2 an octave up and twice
	 * as fast, 0.5 an octave down and half as fast. Finite, in [0.125, 8].
	 * 0 is outside it, so a zeroed desc fails rather than picking a rate.
	 * The voice plays at this times its buses' and modifiers' rates,
	 * clamped to [0.125, 8].
	 */
	float rate;
	/**
	 * NULL: no direction - a mono sound plays equally from the front
	 * speakers' centre (FC, or FL and FR). Ignored for stereo sounds.
	 */
	const APAUDIO_DIRECTION *direction;
	/**
	 * Host monotonic time (spudperf_get_monotonic_time_ns()'s clock) the
	 * sound should reach the speakers at, or 0 for as soon as possible. A
	 * time already past plays as soon as possible. Without host timing from
	 * the stream (no HOST_TIME_VALID), it's ignored. Lets a caller play a
	 * sound a fixed delay after its cause - e.g. the click's input time -
	 * so it lands with constant latency instead of one jittering by a
	 * buffer.
	 */
	uint64_t start_time_ns;
	/** Loops until stopped. */
	bool loop;
} APAUDIO_PLAY_DESC;

/**
 * @brief Queues a playing of a sound for the next render.
 *
 * Thread: any. The next render starts it: first its group's rules
 * (`retrigger_us`, then `max_instances` and `resolution`), which may drop
 * it; then a real slot if it earns one, or else virtual (this file's
 * "Prioritization") - which ends it at once if its behavior is KILL. A
 * voice ended this way, or dropped, before sounding is like one stopped
 * before it started.
 *
 * A play racing the sound's destroy may return APRESULT_OK and never sound:
 * the audio thread checks the ID again as it starts the voice and drops it
 * if it's stale - like a voice stopped before it started.
 *
 * @param[in]  mixer     Mixer to play it on.
 * @param[in]  sound     Sound to play.
 * @param[in]  desc      How to play it. Only read during the call.
 * @param[out] out_voice Its voice; set to 0 on failure. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_LIMIT if the queue is full - the request is
 *         dropped; APRESULT_NOT_FOUND if @p sound is 0 or destroyed.
 */
APRESULT apaudio_play(
    apaudio_mixer mixer,
    APAUDIO_SOUND sound,
    const APAUDIO_PLAY_DESC *desc,
    APAUDIO_VOICE *out_voice);

/**
 * @brief Stops a voice at the next render.
 *
 * Thread: any. Never fails for a valid mixer: the request is recorded on
 * the voice, not queued, so it doesn't compete with plays for queue space.
 * A voice stopped before it started never sounds. Stopping a voice already
 * stopping, finished or unknown does nothing.
 *
 * @param[in] mixer          Mixer it belongs to.
 * @param[in] voice          Voice to stop.
 * @param[in] release_frames How many frames it fades out over (0 cuts it
 *                           off - which can click).
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT only for a null @p mixer.
 */
APRESULT apaudio_voice_stop(
    apaudio_mixer mixer,
    APAUDIO_VOICE voice,
    uint32_t release_frames);

/**
 * @brief Whether a voice still exists.
 *
 * Thread: any. A voice exists from the moment apaudio_play() returns it -
 * before it has started, real or virtual, and while it fades out after a
 * stop - until it has ended: played to its sound's end, stopped, dropped
 * by its group or for want of a real slot (KILL), or its sound destroyed.
 * An end is seen once the render that ended it has finished, so a caller
 * waiting on a voice polls this.
 *
 * @param[in] mixer Mixer it belongs to.
 * @param[in] voice Voice to check.
 *
 * @return true while @p voice exists; false for a finished, unknown or 0
 *         voice, or a null @p mixer.
 */
bool apaudio_voice_is_live(
    apaudio_mixer mixer,
    APAUDIO_VOICE voice);

/**
 * @brief Changes a voice's gain as it plays.
 *
 * The rules are in this file's "Changing a voice".
 *
 * @param[in] mixer Mixer it belongs to.
 * @param[in] voice Voice to change.
 * @param[in] gain  As APAUDIO_PLAY_DESC `gain`.
 *
 * @return APRESULT_OK, or APRESULT_INVALID_ARGUMENT.
 */
APRESULT apaudio_voice_set_gain(
    apaudio_mixer mixer,
    APAUDIO_VOICE voice,
    float gain);
/**
 * @brief Changes a voice's direction as it plays.
 *
 * Ignored for stereo sounds, as at play. The pan moves smoothly between the
 * old and new direction's speaker gains. The rules are in this file's
 * "Changing a voice".
 *
 * @param[in] mixer     Mixer it belongs to.
 * @param[in] voice     Voice to change.
 * @param[in] direction As APAUDIO_PLAY_DESC `direction`: copied; NULL for no
 *                      direction.
 *
 * @return APRESULT_OK, or APRESULT_INVALID_ARGUMENT.
 */
APRESULT apaudio_voice_set_direction(
    apaudio_mixer mixer,
    APAUDIO_VOICE voice,
    const APAUDIO_DIRECTION *direction);
/**
 * @brief Changes a voice's playback rate as it plays.
 *
 * A glide to the new rate across the next render, so the pitch bends
 * rather than steps. The voice carries on from where it was in the sound.
 * The rules are in this file's "Changing a voice".
 *
 * @param[in] mixer Mixer it belongs to.
 * @param[in] voice Voice to change.
 * @param[in] rate  As APAUDIO_PLAY_DESC `rate`.
 *
 * @return APRESULT_OK, or APRESULT_INVALID_ARGUMENT.
 */
APRESULT apaudio_voice_set_rate(
    apaudio_mixer mixer,
    APAUDIO_VOICE voice,
    float rate);

/* ---- WAV ------------------------------------------------------------- */

/**
 * @brief Decoded samples, as APAUDIO_SOUND_DESC takes them.
 *
 * Owned: free with apaudio_pcm_free().
 */
/* Frozen: decoded samples, as a value. It never changes; a new need gets a new type. */
typedef struct APAUDIO_PCM {
	/** #frame_count frames of #channel_count samples, interleaved. */
	float *samples;
	/** How many frames #samples holds. */
	uint64_t frame_count;
	/** Samples per frame. */
	uint32_t channel_count;
	/** Frames per second. */
	uint32_t sample_rate;
} APAUDIO_PCM;

/**
 * @brief Decodes a RIFF WAVE file held in memory.
 *
 * Thread: any (it touches no mixer). PCM 8/16/24/32-bit and IEEE float
 * 32/64-bit, plain or WAVE_FORMAT_EXTENSIBLE, any channel count. Reading
 * the file is the caller's.
 *
 * @param[in]  data    The file's bytes. May be NULL only if @p size is 0.
 * @param[in]  size    How many bytes @p data holds.
 * @param[out] out_pcm The decoded samples, to free with apaudio_pcm_free();
 *                     zeroed on failure.
 * @param[out] error   Why it failed, if it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_UNSUPPORTED for compressed encodings
 *         (ADPCM, mu-law...); APRESULT_CORRUPT_DATA for a broken file.
 */
APRESULT apaudio_wav_decode(
    const void *data,
    uint64_t size,
    APAUDIO_PCM *out_pcm,
    APAUDIO_ERROR *error);
/**
 * @brief Frees what apaudio_wav_decode() decoded, and zeroes @p pcm.
 *
 * @param[in,out] pcm Samples to free. NULL is accepted and does nothing.
 */
void apaudio_pcm_free(APAUDIO_PCM *pcm);

#if __cplusplus
}
#endif

#endif // APAUDIO_H
