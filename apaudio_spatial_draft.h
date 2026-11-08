/*
 * apaudio_spatial_draft.h - ApAudio's spatial audio (APAUDIO_TODO.md item
 * 16, its first bullet): sources and listeners placed in the world,
 * distance attenuation, cones and doppler, worked out in
 * apaudio_mixer_update() and counted in ranking.
 *
 * DRAFT, 2026-11-08, first version. APPROVED by the user 2026-11-08 by
 * standing instruction ("do all that is recommended", given while the
 * session ran unattended): every answer below is its recommendation, and
 * is built as written. Each is in ../claude_made_decisions.md, open for
 * review. Written against include/audio/apaudio.h, src/audio/apaudio.cpp
 * and src/audio/apaudiocues.cpp as read on 2026-11-08. Not compilable as it
 * stands.
 *
 * ---------------------------------------------------------------------------
 * The answers this draft gives (each for approval)
 * ---------------------------------------------------------------------------
 *
 * 1. LISTENERS: SEVERAL PER MIXER, EACH VOICE HEARD FROM ITS NEAREST.
 *    A mixer made with APAUDIO_SPATIAL_DESC chained on has up to
 *    `max_listeners`. The caller sets them all at once each step
 *    (apaudio_mixer_set_listeners(): position, velocity, forward, up); a
 *    world-placed voice is attenuated, panned and doppler-shifted for the
 *    listener nearest it. That is Unreal's rule for split-screen (each
 *    sound uses the closest listener) and FMOD Core's (up to 8 listeners,
 *    the nearest sets the attenuation). Wwise renders an emitter once per
 *    listener and sums them, a mix per pair: not taken, since one output
 *    heard by every player pans each sound once, and the per-pair mix costs
 *    a voice per listener. Zero listeners is allowed: world-placed voices
 *    then have a gain of 0 (nobody hears them) and rank as such.
 *
 * 2. PLACEMENT IS SAID, NOT INFERRED. APAUDIO_PLAY_DESC gains `placement`
 *    (APAUDIO_PLACEMENT: UNSPECIFIED, 0, refused; ANGLES; WORLD), `emitter`
 *    and `attenuation`. ANGLES is today's voice: `direction` read (NULL: no
 *    direction), apaudio_voice_set_direction() moves it. WORLD: `direction`
 *    must be NULL, `emitter` (its first place) and `attenuation` are
 *    required, and apaudio_voice_set_direction() refuses the voice - its
 *    direction is the update's. A zeroed desc picks nothing, as `rate`
 *    already refuses 0. Fixed for the voice's life.
 *
 * 3. EMITTERS MOVE IN BATCHES, APPLIED AT THE NEXT UPDATE.
 *    apaudio_mixer_set_emitters(mixer, voices, emitters, count, error): many
 *    voices' places in one call, the control role (the update's own role,
 *    so its inputs need no atomics), stored; the next
 *    apaudio_mixer_update() works them out. Validated whole before anything
 *    is stored: one bad entry refuses the call and changes nothing. A voice
 *    finished or unknown is skipped, as every voice call does; a live voice
 *    placed by ANGLES is refused (INVALID_ARGUMENT, naming its index).
 *
 * 4. ATTENUATION IS A SHARED SETTINGS OBJECT, as Unreal's attenuation
 *    assets, Wwise's attenuation ShareSets and Hazel's SpatializationConfig
 *    asset: APAUDIO_ATTENUATION, a generational ID like a bus, created from
 *    a desc (control role), `max_attenuations` slots. A voice names one at
 *    play. Per-voice parameters would cost every voice record the curve and
 *    every call its fields. Each holds:
 *    - a distance range, `min_distance` .. `max_distance` (gain 1 inside
 *      the first; the curve's end value held past the second);
 *    - a rolloff (APAUDIO_ROLLOFF): NONE (gain 1 at every distance, for a
 *      setting that only cones or doppler-shifts), INVERSE, LINEAR and
 *      EXPONENT - OpenAL's three clamped models, with its rolloff factor
 *      (EXPONENT is a straight line in dB against log distance, -6 dB per
 *      doubling at a factor of 1: what "logarithmic" curves are in most
 *      tools) - and TABLE, the caller's points (at least 2), each a place
 *      in the range (0 at `min_distance`, 1 at `max_distance`, as Unreal's
 *      custom curves are normalised over their falloff) and a gain in
 *      [0, 1], interpolated linearly in gain; copied at creation;
 *    - a cone: inner and outer full angles in (0, 2 pi], inner <= outer,
 *      and the gain outside the outer, linear between (OpenAL's, Hazel's,
 *      miniaudio's). 2 pi and 2 pi: no cone;
 *    - doppler: a factor >= 0 (0: none);
 *    - a priority offset at `max_distance`, -255 to 255, scaled linearly
 *      over the range and added to the voice's priority, clamped to 0..255
 *      (Wwise's "priority offset at max distance"). 0: none.
 *    A low-pass by distance (air absorption) and one outside the cone wait
 *    for item 12's per-voice filter; each will be a struct chained on this
 *    desc. Destroying an attenuation leaves its voices heard with none: no
 *    distance gain, cone, doppler or offset, panned from their places, as
 *    a destroyed concurrency group's voices play with no group.
 *
 * 5. UNITS AND AXES ARE THE CALLER'S, STATED ONCE. Positions are
 *    ApriDVec3 (double: large worlds), velocities and axes ApriVec3, in the
 *    caller's units. APAUDIO_SPATIAL_DESC says `speed_of_sound` in those
 *    units per second (343 for metres, ApricotFields' unit, written in the
 *    comment, never assumed) and `handedness` (APAUDIO_HANDEDNESS: RIGHT,
 *    LEFT; 0 refused), which is what makes a listener's right
 *    forward x up or up x forward. Doppler takes the caller's velocities,
 *    never ones derived from places between steps (that would make the
 *    pitch depend on the step's timing).
 *
 * 6. AUDIBILITY COUNTS DISTANCE. The update works out each world voice's
 *    spatial gain (distance times cone) before it ranks, and ranks on the
 *    render's published audibility times that gain: a far voice goes
 *    virtual under `virtual_threshold`, and comes back as the listener
 *    nears it, by its virtual behavior. Its priority is offset by distance
 *    (4). A new concurrency resolution, APAUDIO_RESOLUTION_STOP_FARTHEST:
 *    the group's voice farthest from its listener stops if it is at least
 *    as far as the new one, else the new play is dropped; ties go to the
 *    oldest; a voice placed by ANGLES counts as at distance 0.
 *
 * 7. WHERE THE MATHS RUNS: apaudio_mixer_update(), the control role, in
 *    one pass over the voice records before ranking (nearest listener,
 *    direction in the listener's frame, distance and cone gain, doppler,
 *    priority offset). The results - gain, doppler rate, packed direction,
 *    distance, priority - are posted with the update's decisions, through
 *    the same triple buffer, so the render takes them whole with the
 *    decisions and the audio thread only reads values. The render ramps the
 *    gain and the pan across its block and glides the rate (what a voice
 *    change does already), so a moving source doesn't click. The pass is
 *    on the caller's thread, not the scheduler: at 1,024 voices it is
 *    microseconds (measured, item 16's numbers), and the output is the same
 *    bits at any worker count by construction.
 *
 * 8. A WORLD VOICE WAITS FOR ITS FIRST PLACE. A new world-placed play
 *    doesn't take the render's fast path: it starts on the first render
 *    after an update has placed it (and its start time, if later), so it
 *    never sounds at a wrong gain or direction - as FMOD's advice to start
 *    a 3D channel paused until its attributes are set. Its latency is at
 *    most one update's interval; `start_time_ns` keeps it constant.
 *
 * 9. STEREO SOUNDS placed in the world take the distance gain, the cone and
 *    doppler, and ignore the direction, as they do today.
 *
 * 10. A SOURCE AT THE LISTENER (distance 0) keeps the direction it had, so
 *    one passing through the listener doesn't jump.
 *
 * 11. CUES stay as they are (placed by angles, or not at all): a cue played
 *    in the world is a later addition (a struct chained on
 *    APAUDIO_CUE_PLAY_DESC) when a caller needs one; apaudio_play() takes
 *    every world sound now.
 *
 * Not offered, each a later addition: per-voice readback of distance or
 * gain, changing an attenuation in place (create another, and name it at
 * the next play), a listener cone, spread by source size, binaural and
 * ambisonics (item 16's later bullets), occlusion.
 *
 * ---------------------------------------------------------------------------
 * ABI
 * ---------------------------------------------------------------------------
 *
 * Breaks, listed by check.sh --update-abi:
 * - APAUDIO_PLAY_DESC grows by `placement`, `emitter` and `attenuation` at
 *   its end; `placement` 0 is refused, so every caller sets it (ApAudio's
 *   own cues, its tests and its bench; the other caller in the workspace calls apaudio_play()
 *   through the cues only, so is unchanged).
 * Additions: APAUDIO_RESOLUTION_STOP_FARTHEST (5); APAUDIO_PLACEMENT,
 * APAUDIO_HANDEDNESS, APAUDIO_ROLLOFF; APAUDIO_SPATIAL_DESC (3010, chained
 * onto APAUDIO_MIXER_DESC, the first chain ApAudio takes) and
 * APAUDIO_ATTENUATION_DESC (3011); APAUDIO_LISTENER, APAUDIO_EMITTER and
 * APAUDIO_ATTENUATION_POINT, frozen; APAUDIO_ATTENUATION; five functions.
 * apaudio.h includes aprimath.h for ApriDVec3 and ApriVec3.
 *
 * ---------------------------------------------------------------------------
 * Tests (apaudio_tests, extended; pending until built)
 * ---------------------------------------------------------------------------
 *
 * - Each rolloff's gain, rendered offline (no device) from a constant
 *   source, against the formula at several distances, inside min and past
 *   max; a table; the cone inside, between and outside; doppler for a
 *   source approaching and receding (the rendered sine's period), and a
 *   factor of 0; handedness (right ear louder for a source on the right,
 *   for each); the nearest of two listeners; zero listeners silent.
 * - A source moving across the listener rendered with no step between
 *   samples larger than the signal's own.
 * - Ranking: a far voice virtual under the threshold and real again when
 *   near; the priority offset; STOP_FARTHEST.
 * - The refusals, in order (placement 0, WORLD with a direction, without an
 *   emitter or attenuation, ANGLES voice in set_emitters, set_direction on
 *   a world voice, a non-finite place, a zero or parallel forward and up,
 *   too many listeners, every attenuation desc field), and the calls
 *   UNSUPPORTED on a mixer with no spatial desc.
 * - aprinoalloc_tests: 256 moving world voices, set_emitters + update +
 *   render, nothing allocated, plain and with a scheduler.
 * - Bit-identical renders at no scheduler and 1 and 4 workers.
 * - A two-thread test: the control role moving emitters and updating while
 *   another thread renders, for -tsan.
 *
 * ---------------------------------------------------------------------------
 * What was compared
 * ---------------------------------------------------------------------------
 *
 * - Hazel (.refs, src/Hazel/Audio/): one listener; a SpatializationConfig
 *   asset shared by sounds (Inverse, Linear, Exponential like OpenAL's
 *   clamped models, min and max distance and gain, a rolloff, a source cone,
 *   a doppler factor with a fixed speed of sound, air absorption still a
 *   TODO, spread from source size); its maths on miniaudio's spatializer,
 *   doppler as OpenAL 1.1's formula with velocities clamped below the speed
 *   of sound; VBAP panning. Taken: the shared settings object, the three
 *   models, the cone, the formula. Not taken: a fixed speed of sound and
 *   a single listener.
 * - OpenAL 1.1: the distance models and doppler formula used here.
 * - miniaudio's ma_spatializer: the same models, min and max gain, a
 *   listener cone, doppler; parameters per sound, the maths on the audio
 *   thread. Here they're per shared object, and off the audio thread.
 * - FMOD: up to 8 listeners, the nearest attenuating; 3D attributes
 *   applied at System::update (start a channel paused to set them first);
 *   custom rolloff curves as points.
 * - Wwise: attenuation ShareSets with curves over distance, cones,
 *   priority offset at max distance, listeners per emitter summed.
 * - Unreal: attenuation assets (Linear, Logarithmic, Inverse, LogReverse,
 *   NaturalSound, Custom curve over the normalised falloff), the closest
 *   listener in split-screen, priority attenuation.
 */

/* ---- [CHANGED] Concurrency: a resolution by distance -------------------- */

enum {
	/* ...REJECT_NEW (1) to STOP_LOWEST_PRIORITY (4) unchanged... */
	/** [ADDED] The group's voice farthest from its listener stops, if it is
	 * at least as far as the new one; otherwise the new play is dropped.
	 * Ties go to the oldest. A voice placed by ANGLES is at distance 0. */
	APAUDIO_RESOLUTION_STOP_FARTHEST = 5,
};

/* ---- [ADDED] The mixer's spatial settings ------------------------------- */

typedef uint32_t APAUDIO_HANDEDNESS;
enum {
	APAUDIO_HANDEDNESS_UNSPECIFIED = 0, /* refused */
	APAUDIO_HANDEDNESS_RIGHT = 1,       /* right = forward x up */
	APAUDIO_HANDEDNESS_LEFT = 2,        /* right = up x forward */
};

/* Chained onto APAUDIO_MIXER_DESC (base.next). Without it, WORLD plays and
 * the calls below are APRESULT_UNSUPPORTED. */
typedef struct APAUDIO_SPATIAL_DESC {
	APRI_STRUCT_BASE base;            /* APRI_STRUCT_TYPE_APAUDIO_SPATIAL_DESC, 3010 */
	APAUDIO_HANDEDNESS handedness;    /* the caller's world; not UNSPECIFIED */
	uint32_t max_listeners;           /* at least 1 */
	uint32_t max_attenuations;        /* at least 1 */
	float speed_of_sound;             /* the caller's units per second (343 for metres); finite, > 0 */
} APAUDIO_SPATIAL_DESC;

/* ---- [ADDED] Listeners --------------------------------------------------- */

/* Frozen: an array element. */
typedef struct APAUDIO_LISTENER {
	ApriDVec3 position;
	ApriVec3 velocity;   /* units per second, for doppler */
	ApriVec3 forward;    /* not zero */
	ApriVec3 up;         /* not zero, not parallel to forward; made perpendicular to it */
} APAUDIO_LISTENER;

/* Thread: control. Replaces every listener; @p count 0 to max_listeners
 * (APRESULT_LIMIT past it). Each checked finite, forward and up as above;
 * a failed call changes nothing. Applied at the next update. */
APRESULT apaudio_mixer_set_listeners(
    apaudio_mixer mixer,
    const APAUDIO_LISTENER *listeners,
    uint32_t count,
    APAUDIO_ERROR *error);

/* ---- [ADDED] Attenuation -------------------------------------------------- */

typedef uint64_t APAUDIO_ATTENUATION; /* generational, never 0, stale at once when destroyed */

typedef uint32_t APAUDIO_ROLLOFF;
enum {
	APAUDIO_ROLLOFF_UNSPECIFIED = 0, /* refused */
	APAUDIO_ROLLOFF_NONE = 1,        /* 1 at every distance */
	APAUDIO_ROLLOFF_INVERSE = 2,     /* min / (min + f (d - min)) */
	APAUDIO_ROLLOFF_LINEAR = 3,      /* 1 - f (d - min) / (max - min), not below 0 */
	APAUDIO_ROLLOFF_EXPONENT = 4,    /* (d / min) ^ -f */
	APAUDIO_ROLLOFF_TABLE = 5,       /* the caller's points */
};

/* Frozen: an array element. */
typedef struct APAUDIO_ATTENUATION_POINT {
	float place;  /* 0 at min_distance .. 1 at max_distance */
	float gain;   /* linear, [0, 1] */
} APAUDIO_ATTENUATION_POINT;

typedef struct APAUDIO_ATTENUATION_DESC {
	APRI_STRUCT_BASE base;                   /* APRI_STRUCT_TYPE_APAUDIO_ATTENUATION_DESC, 3011 */
	APAUDIO_ROLLOFF rolloff;                 /* not UNSPECIFIED */
	float min_distance;                      /* finite, > 0 */
	float max_distance;                      /* finite, > min_distance */
	float rolloff_factor;                    /* INVERSE, LINEAR, EXPONENT: finite, > 0; else not read */
	const APAUDIO_ATTENUATION_POINT *points; /* TABLE: copied; places from 0 rising strictly to 1 */
	uint32_t point_count;                    /* TABLE: at least 2; else not read */
	float cone_inner_angle;                  /* full angle, (0, 2 pi] */
	float cone_outer_angle;                  /* [inner, 2 pi] */
	float cone_outer_gain;                   /* [0, 1] */
	float doppler_factor;                    /* finite, >= 0; 0 none */
	int32_t priority_offset;                 /* at max_distance, -255 .. 255; 0 none */
} APAUDIO_ATTENUATION_DESC;

/* Thread: control. Allocates TABLE's points. APRESULT_LIMIT with every slot
 * taken; UNSUPPORTED with no spatial desc. */
APRESULT apaudio_attenuation_create(
    apaudio_mixer mixer,
    const APAUDIO_ATTENUATION_DESC *desc,
    APAUDIO_ATTENUATION *out_attenuation,
    APAUDIO_ERROR *error);
/* Thread: control. Its voices are heard with none from the next update. */
void apaudio_attenuation_destroy(apaudio_mixer mixer, APAUDIO_ATTENUATION attenuation);

/* ---- [ADDED] Emitters ----------------------------------------------------- */

/* Frozen: an array element. */
typedef struct APAUDIO_EMITTER {
	ApriDVec3 position;
	ApriVec3 velocity;  /* units per second, for doppler */
	ApriVec3 forward;   /* its cone's axis; zero: it has none (no cone gain) */
} APAUDIO_EMITTER;

typedef uint32_t APAUDIO_PLACEMENT;
enum {
	APAUDIO_PLACEMENT_UNSPECIFIED = 0, /* refused */
	APAUDIO_PLACEMENT_ANGLES = 1,      /* by `direction`, or none */
	APAUDIO_PLACEMENT_WORLD = 2,       /* by `emitter`, from the nearest listener */
};

/* [CHANGED] APAUDIO_PLAY_DESC gains, at its end:
 *   APAUDIO_PLACEMENT placement;          // not UNSPECIFIED
 *   const APAUDIO_EMITTER *emitter;       // WORLD: its first place, copied; required. ANGLES: NULL
 *   APAUDIO_ATTENUATION attenuation;      // WORLD: required (0 or stale refused at play). ANGLES: 0
 * and `direction` must be NULL for WORLD. apaudio_play() returns
 * APRESULT_UNSUPPORTED for WORLD on a mixer with no spatial desc. */

/* Thread: control. @p count voices' places, applied at the next update.
 * Checked whole first: a non-finite place or a live voice placed by ANGLES
 * refuses the call (INVALID_ARGUMENT, the message naming the index) and
 * stores nothing. Finished or unknown voices are skipped. */
APRESULT apaudio_mixer_set_emitters(
    apaudio_mixer mixer,
    const APAUDIO_VOICE *voices,
    const APAUDIO_EMITTER *emitters,
    uint32_t count,
    APAUDIO_ERROR *error);

/* [CHANGED] apaudio_voice_set_direction(): APRESULT_INVALID_ARGUMENT for a
 * live voice placed in the world. */
