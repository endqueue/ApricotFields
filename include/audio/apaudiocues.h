/**
 * @file apaudiocues.h
 * @brief ApAudio's cues: named sounds over a mixer (apaudio.h), defined by
 * sound banks.
 *
 * Built in src/audio/apaudiocues.cpp. apricotfields.h doesn't include it
 * yet.
 *
 * This is the event model of FMOD Studio and Wwise - code states what
 * happened ("tool.wall.placed", "footstep.gravel"), data decides how it
 * sounds.
 *
 * A cue table holds cue names and what the loaded banks define for each:
 * which sounds (one picked at random each play), gain, pitch variation,
 * bus, priority, instance limits, loop, fades. A bank is a manifest.json
 * and the WAV files it names (format below). Banks load over each other:
 * a later bank's cues replace the same names in earlier ones and leave the
 * rest, so an application ships defaults and a user's bank overrides a
 * few. Loading a bank again - its files edited - is the same thing, so
 * sounds change in a running application.
 *
 * General purpose: it knows no cue names and no bus names. The caller says
 * which of its mixer's buses banks may route to, and by what names; the
 * names cues have are whatever its banks and code agree on.
 *
 * It does no I/O of its own, like ApArchive: a bank's files come through
 * an APAUDIO_BANK_SOURCE the caller implements (a folder, files embedded
 * in the executable), or from a ZIP archive through an APARCHIVE_SOURCE.
 *
 * Results, error messages and struct bases are apaudio.h's (Conventions).
 * Here, also:
 * - APRESULT_CORRUPT_DATA: a malformed bank: its manifest, a sound it
 *   names and doesn't hold, a WAV that isn't mono or stereo.
 * - APRESULT_LIMIT: a bank whose new cue names don't fit `max_cues`, or
 *   whose sounds or groups the mixer has no slot for; a play with the
 *   mixer's queue full; a start with `max_playings` already live.
 * - APRESULT_NOT_FOUND: a cue that's 0 or unknown.
 *
 * @par Bank format
 * manifest.json, read strictly - an unknown key, a missing required one or
 * an out-of-range value fails the bank, so a typo shows up where its
 * author sees it rather than as a cue that's quietly wrong. A malformed
 * bank loads nothing, rather than half of itself.
 * @code
 *   {
 *     "version": 1,
 *     "cues": {
 *       "tool.wall.placed": {
 *         "sounds": ["placed_1.wav", "placed_2.wav"],
 *         "bus": "feedback",
 *         "gain": 0.8, "pitch_variation": 0.03, "priority": 160,
 *         "max_instances": 2, "retrigger_ms": 15,
 *         "loop": false, "exclusive": false,
 *         "fade_in_ms": 0, "fade_out_ms": 0
 *       }
 *     },
 *     "buses": { "accessibility": { "duck": { "feedback": 0.3 } } }
 *   }
 * @endcode
 * Required: "version" (1), "cues", and each cue's "sounds" and "bus". Each
 * key, with its default in parentheses:
 * - `sounds`: files of the bank, WAV, mono or stereo; one is picked at
 *   random each play. May be empty: the cue is defined and plays nothing.
 * - `bus`: one of the table's buses, by name.
 * - `gain`: linear, finite, >= 0 (1).
 * - `pitch_variation`: a random playback-rate offset each play, as a
 *   fraction either side: 0 to under 1 (0).
 * - `priority`: 0-255, as APAUDIO_PLAY_DESC's `priority` (128).
 * - `max_instances`, `retrigger_ms`: with either, the cue gets a
 *   concurrency group of its own, STOP_OLDEST (none).
 * - `loop`: plays until stopped (false).
 * - `exclusive`: started (apaudio_cues_start()), it fades out its bus's
 *   other exclusive playing: music (false).
 * - `fade_in_ms`: recorded for the caller (APAUDIO_CUE_INFO); not applied
 *   yet (0).
 * - `fade_out_ms`: how long apaudio_cues_stop() fades it out (0).
 *
 * "buses" holds settings per bus: "duck" lowers other buses, each to a
 * gain from 0 to 1, while any voice on this bus is audible. A later bank's
 * settings for a bus replace an earlier one's.
 *
 * @par Threads
 * apricore.h's rule ("Threads"), with apaudio.h's roles ("Threads"): a cue
 * table's control calls are made by one caller at a time, together with
 * its mixer's control calls (they create and destroy the mixer's sounds),
 * and its "any" calls by any number at once, from any thread. Each call
 * below names its role on its "Thread:" line: "control" or "any".
 */

#ifndef APAUDIOCUES_H
#define APAUDIOCUES_H

#include "apaudio.h"
#include "../archive/aparchive.h"

#if __cplusplus
extern "C" {
#endif

/* ---- Bank format ----------------------------------------------------- */

/* The bank format is in this file's description ("Bank format"). */

/* ---- Cue table ------------------------------------------------------- */

/**
 * @brief Opaque handle to a cue table.
 */
typedef struct apaudio_cues_t *apaudio_cues;

/** A cue name's size in bytes, including the terminating NUL. */
#define APAUDIO_CUE_NAME_SIZE 64

/**
 * @brief One of the mixer's buses that banks may route cues to.
 */
/* Frozen: an array element. It never changes; a new need gets a new type. */
typedef struct APAUDIO_CUES_BUS {
	/** What manifests call it; copied. Not empty, and unique in the table. */
	const char *name;
	/** The mixer bus the cues routed here play on. Not 0. */
	APAUDIO_BUS bus;
	/**
	 * What its cues' voices do while not real (KILL for feedback, CONTINUE
	 * for music...); required.
	 */
	APAUDIO_VIRTUAL_BEHAVIOR virtual_behavior;
} APAUDIO_CUES_BUS;

/**
 * @brief Configuration descriptor for a cue table.
 *
 * `mixer`, `buses`, `bus_count`, `max_cues`, `max_playings`, `max_voices` and
 * `sample_rate` are all required, none of them 0.
 */
typedef struct APAUDIO_CUES_DESC {
	/** This struct's type, APRI_STRUCT_TYPE_APAUDIO_CUES_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** The mixer cues play on. It outlives the table. */
	apaudio_mixer mixer;
	/**
	 * #bus_count buses, at least one, copied. Elsewhere a bus is named by
	 * its index here. The master isn't normally one of them: everything
	 * plays under it.
	 */
	const APAUDIO_CUES_BUS *buses;
	/** The number of #buses. */
	uint32_t bus_count;
	/**
	 * Cue names the table can hold, found or defined. Names are only ever
	 * added, which is what keeps an APAUDIO_CUE valid for the table's
	 * life.
	 */
	uint32_t max_cues;
	/** Sustained playings (apaudio_cues_start()) live at once. */
	uint32_t max_playings;
	/**
	 * The mixer's `max_voices`: the instance limit of a cue that has a
	 * `retrigger_ms` but no `max_instances`.
	 */
	uint32_t max_voices;
	/**
	 * The mixer's output rate, for fades given in milliseconds. Keep it
	 * current with apaudio_cues_set_sample_rate().
	 */
	uint32_t sample_rate;
	/**
	 * The most a bank's manifest and sounds may come to, in bytes,
	 * uncompressed.
	 */
	uint64_t max_bank_size;
	/** How fast a bus's ducking comes in, in microseconds. */
	uint32_t duck_fade_in_us;
	/** How fast a bus's ducking lets go, in microseconds. */
	uint32_t duck_fade_out_us;
} APAUDIO_CUES_DESC;

/**
 * @brief Creates an empty cue table: no names, no banks.
 *
 * Thread: control.
 *
 * @param[in]  desc     Table configuration. Only read during the call.
 * @param[out] out_cues The table, or NULL on failure.
 * @param[out] error    Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT for a NULL @p desc or
 *         @p out_cues, a required field that's 0, or a bus without a name
 *         of its own, a mixer bus and a virtual behavior; a struct-base
 *         failure for @p desc; APRESULT_OUT_OF_MEMORY.
 *
 * @see apaudio_cues_destroy()
 */
APRESULT apaudio_cues_create(
    const APAUDIO_CUES_DESC *desc,
    apaudio_cues *out_cues,
    APAUDIO_ERROR *error);
/**
 * @brief Destroys a cue table.
 *
 * Thread: control. Destroys the table and, on its mixer, every sound,
 * concurrency group and ducking modifier its banks made. Voices still
 * playing stop at the next render.
 *
 * @warning Destroy it before the mixer.
 *
 * @param[in] cues Table to destroy. NULL is accepted and does nothing.
 */
void apaudio_cues_destroy(apaudio_cues cues);

/**
 * @brief Tells the table its mixer's output rate changed
 * (apaudio_mixer_set_format()).
 *
 * Thread: any.
 *
 * @param[in] cues        The table. NULL does nothing.
 * @param[in] sample_rate The new rate. 0 is ignored.
 */
void apaudio_cues_set_sample_rate(apaudio_cues cues, uint32_t sample_rate);

/**
 * @brief Forgets playings whose voice ended by itself - a sound that
 * doesn't loop played to its end, or one the mixer dropped.
 *
 * Thread: control. Call it once a frame, after apaudio_mixer_update().
 *
 * @param[in] cues The table.
 */
void apaudio_cues_update(apaudio_cues cues);

/* ---- Banks ----------------------------------------------------------- */

/**
 * @brief Where a bank's files come from, by the names its manifest uses
 * ("manifest.json" itself first).
 *
 * Called only during the load call, on the thread making it.
 */
/* Frozen: callbacks for one load call. It never changes; a new need gets a new type. */
typedef struct APAUDIO_BANK_SOURCE {
	/** Passed back as the first argument of each callback. */
	void *user;
	/**
	 * The size in bytes of the file named `name`, in `out_size`; false if
	 * the bank doesn't hold it - including a name the source won't serve,
	 * such as one leading out of a folder.
	 */
	bool (*get_size)(void *user, const char *name, uint64_t *out_size);
	/**
	 * Copies the file named `name` whole into `buffer`, of the `size`
	 * #get_size gave; false on any failure.
	 */
	bool (*read)(void *user, const char *name, void *buffer, uint64_t size);
} APAUDIO_BANK_SOURCE;

/**
 * @brief Loads a bank over what's loaded.
 *
 * Thread: control. Reads the bank behind @p source whole, decoding its
 * sounds, then applies it over what's loaded: its sounds become the
 * mixer's, its cues replace the same names, its buses' ducking replaces
 * theirs. A play sees all of the bank or none of it. Cues it replaces let
 * go of their sounds; voices playing those stop at the next render.
 *
 * @param[in]  cues   The table.
 * @param[in]  source Where the bank's files come from.
 * @param[out] error  Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK. Failures load nothing: APRESULT_CORRUPT_DATA
 *         (malformed, with why in @p error); APRESULT_IO (the source failed
 *         reading a file it holds); APRESULT_LIMIT; APRESULT_OUT_OF_MEMORY;
 *         APRESULT_INVALID_ARGUMENT.
 */
APRESULT apaudio_cues_load_bank(
    apaudio_cues cues,
    const APAUDIO_BANK_SOURCE *source,
    APAUDIO_ERROR *error);

/**
 * @brief As apaudio_cues_load_bank(), for a bank that's a ZIP archive
 * holding manifest.json at its root.
 *
 * Thread: control.
 *
 * @param[in]  cues    The table.
 * @param[in]  archive Random-access input over the archive (aparchive.h).
 * @param[out] error   Why it failed, when it did. May be NULL.
 *
 * @return As apaudio_cues_load_bank(); APRESULT_NOT_IMPLEMENTED until
 *         ApArchive's reader exists.
 */
APRESULT apaudio_cues_load_zip_bank(
    apaudio_cues cues,
    const APARCHIVE_SOURCE *archive,
    APAUDIO_ERROR *error);

/**
 * @brief Forgets what every loaded bank defined.
 *
 * Thread: control. Leaves the table as it was before the first bank: no
 * cue is defined, and no bus ducks another. Cue names and their handles
 * stay, and so does what the caller set - cues turned off
 * (apaudio_cues_set_enabled()) and buses muted
 * (apaudio_cues_set_bus_muted()). The sounds, concurrency groups and
 * ducking modifiers the banks made are destroyed on the mixer; voices
 * playing those sounds stop at the next render. A play sees every bank or
 * none.
 *
 * This is how a bank is taken away. A cue keeps only its latest
 * definition, so one bank can't be unloaded from under the others: clear,
 * then load the banks that are to remain, in order.
 *
 * It can't fail.
 *
 * @param[in] cues The table. Clearing an empty table, or a null one, does
 *                 nothing.
 */
void apaudio_cues_clear_banks(apaudio_cues cues);

/* ---- Cues ------------------------------------------------------------ */

/**
 * @brief A cue: a handle to a cue name - its index in the table plus one.
 *
 * Never 0, never stale. It names the cue, not a bank's definition of it: a
 * cue no loaded bank defines is valid and silent, so cues can be found
 * before or after banks load, in any order.
 */
typedef uint64_t APAUDIO_CUE;

/**
 * @brief Finds the cue named @p name, adding the name if it's new.
 *
 * Thread: any. Find cues once and keep the handles: playing by handle
 * never looks a name up.
 *
 * @param[in]  cues    The table.
 * @param[in]  name    The cue's name, under APAUDIO_CUE_NAME_SIZE bytes.
 * @param[out] out_cue The cue; 0 on failure.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT for an empty or too long
 *         name (or a NULL argument); APRESULT_LIMIT with `max_cues` names
 *         already known; APRESULT_OUT_OF_MEMORY.
 */
APRESULT apaudio_cues_find(apaudio_cues cues, const char *name, APAUDIO_CUE *out_cue);

/**
 * @brief Turns one cue on or off, wherever it's played from.
 *
 * Thread: any. Cues are on until turned off. Turning one off stops nothing
 * already playing. The latest call wins.
 *
 * @param[in] cues    The table.
 * @param[in] cue     The cue.
 * @param[in] enabled Whether it plays.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT for a NULL @p cues;
 *         APRESULT_NOT_FOUND for a @p cue that's 0 or unknown.
 */
APRESULT apaudio_cues_set_enabled(apaudio_cues cues, APAUDIO_CUE cue, bool enabled);

/**
 * @brief While set, a play of a cue on the bus at @p bus_index plays
 * nothing and succeeds.
 *
 * Thread: any. It doesn't mute the bus - apaudio_bus_set_muted() does, and
 * a voice started on a muted bus waits, silent, for the unmute. This is
 * for feedback that would be wrong late: set both.
 *
 * @param[in] cues      The table.
 * @param[in] bus_index The bus, by its index in the table's buses.
 * @param[in] muted     Whether its cues play nothing.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT for an index past the
 *         table's buses (or a NULL @p cues).
 */
APRESULT apaudio_cues_set_bus_muted(apaudio_cues cues, uint32_t bus_index, bool muted);

/**
 * @brief Gets the modifier ducking other buses while the bus at
 * @p bus_index sounds.
 *
 * Thread: control. It is from the last bank with settings for the bus, and
 * is replaced, and the old one destroyed, by a later bank.
 *
 * @param[in] cues      The table.
 * @param[in] bus_index The bus, by its index in the table's buses.
 *
 * @return The modifier; 0 if the bus ducks nothing (or for a NULL @p cues
 *         or an index past the table's buses).
 */
APAUDIO_MODIFIER apaudio_cues_get_bus_ducking(apaudio_cues cues, uint32_t bus_index);

/**
 * @brief What the loaded banks define for a cue.
 *
 * The members are the manifest's keys of the same names (see the bank
 * format in this file's description).
 */
typedef struct APAUDIO_CUE_INFO {
	/** This struct's type, APRI_STRUCT_TYPE_APAUDIO_CUE_INFO, set by the caller (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE_OUT base;
	/**
	 * False for a cue no loaded bank defines; the rest is then its
	 * defaults.
	 */
	bool defined;
	/** Plays until stopped. */
	bool loop;
	/** Started, it fades out its bus's other exclusive playing. */
	bool exclusive;
	/** 0-255, as APAUDIO_PLAY_DESC's `priority`. */
	uint8_t priority;
	/** How many sounds it picks from. */
	uint32_t sound_count;
	/** Its bus, by its index in the table's buses. */
	uint32_t bus_index;
	/** Linear gain. */
	float gain;
	/** The random playback-rate offset each play, as a fraction either side. */
	float pitch_variation;
	/** 0: no limit of its own. */
	uint32_t max_instances;
	/** The retrigger time of its concurrency group, in milliseconds. */
	uint32_t retrigger_ms;
	/** Recorded for the caller; not applied yet. */
	uint32_t fade_in_ms;
	/** How long apaudio_cues_stop() fades it out. */
	uint32_t fade_out_ms;
} APAUDIO_CUE_INFO;

/**
 * @brief Reads what the loaded banks define for a cue.
 *
 * Thread: any.
 *
 * @param[in]     cues     The table.
 * @param[in]     cue      The cue.
 * @param[in,out] out_info Filled; its base the caller sets
 *                         (APRI_STRUCT_TYPE_APAUDIO_CUE_INFO), and it is
 *                         left as it was.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT for a NULL argument; a
 *         struct-base failure for @p out_info; APRESULT_NOT_FOUND for a
 *         @p cue that's 0 or unknown.
 */
APRESULT apaudio_cues_get_info(apaudio_cues cues, APAUDIO_CUE cue, APAUDIO_CUE_INFO *out_info);

/* ---- Playing --------------------------------------------------------- */

/**
 * @brief How one play of a cue differs from the cue's own settings.
 */
typedef struct APAUDIO_CUE_PLAY_DESC {
	/** This struct's type, APRI_STRUCT_TYPE_APAUDIO_CUE_PLAY_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** Times the cue's own gain: linear, finite, >= 0. */
	float gain;
	/** As APAUDIO_PLAY_DESC's; NULL for none. */
	const APAUDIO_DIRECTION *direction;
	/** As APAUDIO_PLAY_DESC's; 0 for as soon as possible. */
	uint64_t start_time_ns;
} APAUDIO_CUE_PLAY_DESC;

/**
 * @brief Plays @p cue once: one of its sounds, on its bus, with its gain,
 * pitch variation, priority and limits.
 *
 * Thread: any. Never blocks on sound.
 *
 * A cue that's off, that no bank defines, that has no sounds, or whose bus
 * is muted here plays nothing and succeeds. So does one whose sound a bank
 * replaced in the instant between picking it and playing it. What the
 * mixer then does with the voice - its group's limits, real or virtual -
 * is apaudio_play()'s.
 *
 * @param[in]  cues      The table.
 * @param[in]  cue       The cue.
 * @param[in]  desc      May be NULL (gain 1, no direction).
 * @param[out] out_voice The mixer's voice, or 0 when nothing was queued.
 *                       May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT for a NULL @p cues, or a
 *         @p desc whose gain or direction isn't valid; a struct-base
 *         failure for @p desc; APRESULT_NOT_FOUND for a @p cue that's 0 or
 *         unknown; APRESULT_LIMIT with the mixer's queue full.
 */
APRESULT apaudio_cues_play(
    apaudio_cues cues,
    APAUDIO_CUE cue,
    const APAUDIO_CUE_PLAY_DESC *desc,
    APAUDIO_VOICE *out_voice);

/**
 * @brief As apaudio_cues_play(), for a sustained playing - music, a loop -
 * that the table keeps track of until it ends.
 *
 * Thread: any. apaudio_cues_stop() fades it out over its cue's
 * `fade_out_ms`, and an exclusive cue starting fades out its bus's other
 * exclusive playing. The playing is named by its voice.
 *
 * @param[in]  cues      The table.
 * @param[in]  cue       The cue.
 * @param[in]  desc      As apaudio_cues_play()'s.
 * @param[out] out_voice The playing's voice, or 0 when nothing was queued.
 *                       Required.
 *
 * @return As apaudio_cues_play(), and APRESULT_LIMIT also with
 *         `max_playings` already live.
 */
APRESULT apaudio_cues_start(
    apaudio_cues cues,
    APAUDIO_CUE cue,
    const APAUDIO_CUE_PLAY_DESC *desc,
    APAUDIO_VOICE *out_voice);

/**
 * @brief Ends the playing @p voice.
 *
 * Thread: any. A voice that isn't a live playing does nothing.
 *
 * @param[in] cues  The table.
 * @param[in] voice The playing, as apaudio_cues_start() gave it.
 * @param[in] fade  Whether to fade it out over its cue's `fade_out_ms`,
 *                  else end it at once.
 */
void apaudio_cues_stop(apaudio_cues cues, APAUDIO_VOICE voice, bool fade);

/**
 * @brief Whether @p voice is a playing that's started and not yet stopped
 * or seen to have ended.
 *
 * Thread: any. One that ends by itself is seen at the next
 * apaudio_cues_update(). A stopped playing isn't live even while it fades
 * out.
 *
 * @param[in] cues  The table.
 * @param[in] voice The playing, as apaudio_cues_start() gave it.
 *
 * @return True while it's live; false otherwise, or for a NULL @p cues.
 */
bool apaudio_cues_is_playing(apaudio_cues cues, APAUDIO_VOICE voice);

#if __cplusplus
}
#endif

#endif // APAUDIOCUES_H
