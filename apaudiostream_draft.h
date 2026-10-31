/*
 * apaudiostream_draft.h - ApAudio's streamed voices (APAUDIO_TODO.md, item
 * 15's other half: "decoding into a ring the owner's update fills, so the
 * audio thread never decodes or allocates"), for review. FLAC, Ogg Vorbis
 * and Ogg Opus, through the decoders item 15 built.
 *
 * DRAFT, 2026-10-31; second version the same day, after comparing D1-D7
 * with Unreal Engine and with CryEngine's middleware (below), the changes
 * agreed: D2 decodes on the caller's scheduler, D3 gains a prefetch, D4's
 * message names the voice, D5 ranks by gain, D7 counts per sound.
 *
 * APPROVED 2026-10-31 by the user: D1-D7 as stated here (APAUDIO_TODO.md
 * item 15; the workspace's DECISIONS.md, "Decided"). The declarations
 * marked [ADDED] and [CHANGED] are in include/audio/apaudio.h as written
 * here since the same day, and abi.txt lists the breaks below; this file is
 * the record of what was decided and why, and is no longer to be built
 * from. Not compilable as it stands: it redeclares APAUDIO_SOUND_DESC,
 * APAUDIO_MIXER_DESC and apaudio_mixer_update(), which apaudio.h now
 * declares. Where building it found the draft wrong or incomplete, a card
 * says so (DECISIONS.md, card A34: Opus after a seek).
 *
 * ---------------------------------------------------------------------------
 * The shape, in one paragraph
 * ---------------------------------------------------------------------------
 *
 * A streamed sound is a sound: APAUDIO_SOUND_DESC names an encoded source and
 * its codec in place of decoded samples, and the caller plays it with
 * apaudio_play() like any other, with every voice setting, bus, group and
 * virtual behavior it has today. apaudio_sound_create() reads the file's
 * header and decodes its start, which every voice of it plays first, so a
 * voice starts at once. Each voice decodes its own way through the rest,
 * and only while it is real: each apaudio_mixer_update() hands the decoding
 * of every real streamed voice to the caller's scheduler as one task, to
 * run while the caller's frame goes on, and the next update waits for it
 * before it ranks; with no scheduler the update decodes before it returns.
 * The render reads the prefetch and the rings and nothing else, so the
 * audio thread never decodes, allocates or waits. A ring that runs dry
 * plays silence and is counted on its sound. A voice going virtual, ending
 * or being stopped lets its decoder go, freed by apaudio_mixer_collect()
 * once a render has confirmed it no longer reads the ring.
 *
 * ---------------------------------------------------------------------------
 * What this decides that no card has (each becomes a card if overturned)
 * ---------------------------------------------------------------------------
 *
 *  D1. A stream is a kind of sound, not a kind of play. apaudio_play() takes
 *      a sound and is Thread: any and never allocates, so it can't open a
 *      decoder; what is encoded, in which codec, at what rate and length,
 *      how its voices go virtual and which bus they go to are the sound's;
 *      where each playing has got to is the voice's. So the source is in
 *      APAUDIO_SOUND_DESC, and no new handle, play call or voice type exists.
 *      The per-voice decoding state has no lifetime apart from its voice, so
 *      it is not a handle. Each voice decodes on its own, so one streamed
 *      sound plays any number of times at once (FMOD's streams can't).
 *  D2. Decoding is a task on the caller's scheduler (APJOBS_SCHEDULER,
 *      apjobs.h, in APAUDIO_MIXER_DESC; NULL for none). An update waits for
 *      the task the last one started, ranks, then starts the next: one item
 *      per real streamed voice, each opening its decoder if it has none (from
 *      the system heap: the one allocation a stream makes, with its ring in
 *      the same block) and topping its ring up. The task runs beside the
 *      caller's frame, its source reads blocking a worker, never the frame;
 *      with no scheduler, the update runs it before returning. Every control
 *      call that frees or changes what the task touches (collect,
 *      set_format, the mixer's destroy) waits for it first. ApAudio still
 *      starts no thread. Not preallocated at mixer creation: a ring's size
 *      depends on its sound's rate, a caller's allocator for ApricotFields
 *      waits on card X15, and opusfile and libopus call malloc with no hook.
 *  D3. A streamed voice holds a stream (a decoder and a ring) only while it
 *      is real or fading. A virtual one holds none and costs no decoding:
 *      CONTINUE moves its place on by the clock, as an in-memory voice's,
 *      and its decoder seeks there when it is real again; RESTART comes back
 *      at 0; PAUSE, and a voice on a held bus, keep their place and seek back
 *      to it (every decoder here seeks exactly). A voice starting at frame 0
 *      (a new play, a RESTART) plays the sound's prefetch, its decoded start,
 *      at once, while its stream opens to carry on from the prefetch's end;
 *      one coming back elsewhere (CONTINUE, PAUSE) is silent until the
 *      update after, and fades in from there. max_streams caps streams as
 *      max_real_voices caps real slots: a streamed voice ranked real with
 *      every stream taken is virtual.
 *  D4. apaudio_mixer_update() returns APRESULT and takes an APAUDIO_ERROR,
 *      for a stream that fails mid-way (a read, a corrupt packet, a decoder
 *      that couldn't be opened): its voice ends, and the update after the
 *      task that met it says so - the first failure as the result, its
 *      message naming the voice's ID, the codec and the part, and how many
 *      more failed. A per-call error is ApricotFields' rule; when ApAudio
 *      has events from playback (APAUDIO_TODO.md item 13), stream failures
 *      become events there. An ABI break (below).
 *  D5. A streamed voice is ranked by its gain alone (its sound's loudness
 *      taken as 1), not by the content's level: nothing measures the whole
 *      file, and a sample of its start misleads (a quiet intro to a loud
 *      track). Whether any sound, streamed or not, should take a loudness
 *      the caller measured when it cooked the file is a question for a card,
 *      for both kinds at once.
 *  D6. The caller's bytes come through APAUDIO_SOURCE: random access by
 *      offset (the decoders seek, for loops, RESTART, CONTINUE and the
 *      prefetch's end). Memory the caller keeps alive is a source whose read
 *      copies; no second way in, since the copy of compressed bytes is small
 *      beside decoding them. The read is called by apaudio_sound_create() on
 *      its caller's thread, and by the decoding task on the scheduler's
 *      workers, several at once, for one source too (two voices of one
 *      sound): a read must be callable concurrently, and may block. Its own
 *      type, not ApArchive's APARCHIVE_SOURCE, because its thread rule and
 *      lifetime differ and a sound's bytes are not an archive's.
 *  D7. Underruns are counted per sound (apaudio_sound_get_underrun_frames()),
 *      where the ring length that fixes them is set (`stream_ring_us`).
 *
 * ---------------------------------------------------------------------------
 * Compared with Unreal Engine and CryEngine (checked 2026-10-31)
 * ---------------------------------------------------------------------------
 *
 * CryEngine streams no audio itself: its audio layer (ATL) hands sounds to a
 * middleware, Wwise by default or FMOD, so its column is theirs. Only what
 * the sources below say is stated; "not confirmed" marks what they didn't.
 *
 *  D1  Unreal: streaming is the sound wave's, set per USoundWave (its
 *      loading behavior and the audio stream cache) [U2].
 *      FMOD: a stream is a Sound (FMOD_CREATESTREAM) with one decode buffer
 *      and file handle, so it plays once at a time; open it again to play it
 *      twice [F1]. Wwise: per source, in authoring; not confirmed here.
 *  D2  Unreal: chunks are read with asynchronous IAsyncReadRequests [U2];
 *      on which thread they are decoded, not confirmed. Wwise: the blocking
 *      and deferred low-level I/O hooks run on Wwise's own threads, so
 *      playback isn't blocked [W2]. Both keep I/O off the frame; this does
 *      it on the caller's scheduler, since a library here starts no thread.
 *  D3  Unreal: EVirtualizationMode is Disabled, PlayWhenSilent (keeps
 *      playing silently, using a voice), Restart (a looping sound from its
 *      start) and SeekRestart (marked experimental: virtual, seeks to where
 *      a looping sound would be) [U1]; its stream cache can prime or retain a
 *      wave's first chunk [U2]. Wwise: virtual voices play from the
 *      beginning, from the elapsed time (a streamed sound may pause before
 *      it resumes; Vorbis needs a seek table to be exact) or resume, and
 *      their buffers are freed while virtual except to resume [W1]; its
 *      zero-latency streaming keeps a prefetch of the start in memory [W3].
 *      FMOD: a virtual sound comes back at the offset time says, or from its
 *      start with a flag [F3].
 *  D4  Unreal: not confirmed. FMOD: a non-blocking open's state, and a
 *      stream's, are read back with Sound::getOpenState [F1]. Wwise: not
 *      confirmed. None returns a mid-stream failure from a frame call; both
 *      middlewares report state for the caller to poll or watch, which is
 *      what item 13's events will be here.
 *  D5  FMOD: real and virtual are decided by an estimated audibility from
 *      distance, volume, priority and occlusion [F2] - gains, not content.
 *      Wwise: a voice goes virtual below a volume threshold or when pushed
 *      out by limits and priority [W4]. Unreal: not confirmed.
 *  D6  Wwise: the caller implements the low-level I/O hook, blocking or
 *      deferred [W2]. FMOD: the caller can supply file callbacks; not
 *      confirmed in detail here. Unreal: its own asynchronous file reads
 *      [U2], no caller hook.
 *  D7  FMOD: getOpenState's `starving` says, per sound, that the decoder
 *      ran ahead of the file buffer; the docs say to mute until it isn't, or
 *      raise System::setStreamBufferSize [F1]. Wwise: source starvation is
 *      reported, with the prefetch's length showing as silence [W3].
 *      Unreal: trimming the stream cache can make streaming underrun [U2].
 *
 *  [U1] dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/EVirtualizationMode
 *  [U2] dev.epicgames.com/documentation/en-us/unreal-engine/an-overview-of-audio-stream-caching-in-unreal-engine
 *  [F1] fmod.com/docs/2.03/api/core-api-sound.html (Sound::getOpenState; FMOD_CREATESTREAM's note)
 *  [F2] fmod.com/docs/2.03/api/managing-resources-in-the-core-api.html
 *  [F3] FMOD's FMOD_VIRTUAL_PLAYFROMSTART, as its Rust bindings' docs quote it (docs.rs/fmod)
 *  [W1] Wwise 2015.1.9 User's Guide, "Managing the priority of sounds" (an
 *       old version; the current guide wasn't reached)
 *  [W2] Open 3D Engine's AudioEngineWwise API reference (CBlockingDevice_wwise,
 *       CStreamingDevice_wwise), and Audiokinetic's Q&A 2102
 *  [W3] Audiokinetic Q&A 11855 and 5069 (zero latency, prefetch length,
 *       starvation)
 *  [W4] Audiokinetic Q&A 6164 and 8659 (virtual voices, the volume threshold)
 *
 * ---------------------------------------------------------------------------
 * What it adds to the public API, and why each earns its place
 * ---------------------------------------------------------------------------
 *
 *  APAUDIO_SOURCE (struct, frozen)  Where a streamed sound's bytes come from.
 *      The caller hands over bytes it doesn't want copied whole into the
 *      mixer (the point of streaming), and ApAudio does no I/O (D6).
 *  APAUDIO_STREAM_CODEC (enum)      Which decoder reads the source: the three
 *      that exist. NONE (0) means "not streamed: the samples are in the
 *      desc", so a zeroed new field keeps every existing desc's meaning. No
 *      WAV: not asked for, not built.
 *  APAUDIO_SOUND_DESC::stream_codec, ::stream_source, ::stream_ring_us
 *      [ADDED, at the end] The choice of streaming, the bytes, and how far
 *      ahead each voice decodes (also the prefetch's length). The sound's
 *      (D1).
 *  APAUDIO_MIXER_DESC::max_streams, ::scheduler [ADDED, at the end] The
 *      budget for streaming, as max_real_voices is for mixing; and where
 *      decoding runs (D2). 0 and NULL: as before (no streams; and, with
 *      streams, decoding inside the update).
 *  apaudio_mixer_update()           [CHANGED] returns APRESULT, takes an
 *      APAUDIO_ERROR (D4). No new call: decoding is part of the update the
 *      caller already makes each frame.
 *  apaudio_sound_get_underrun_frames() [ADDED] The one way a caller learns a
 *      sound's ring is too short for its update cadence (D7).
 *
 * Nothing else is added: no stream handle, no streamer object, no play call,
 * no per-voice stream getter, no file API, no thread. Removed: nothing.
 *
 * ---------------------------------------------------------------------------
 * ABI breaks (check.sh --update-abi, when built)
 * ---------------------------------------------------------------------------
 *
 *  - apaudio_mixer_update: void(apaudio_mixer) ->
 *    APRESULT(apaudio_mixer, APAUDIO_ERROR *). Every caller updated in the
 *    same change, and this repo's own tests.
 *  - struct APAUDIO_SOUND_DESC grows at its end (stream_codec,
 *    stream_source, stream_ring_us); struct APAUDIO_MIXER_DESC grows at its
 *    end (max_streams, scheduler). Before 1.0 a field at the end is
 *    allowed, listed as a break; zero in each means what the desc meant
 *    before.
 *  - Added: APAUDIO_SOURCE, APAUDIO_STREAM_CODEC and its values,
 *    apaudio_sound_get_underrun_frames. apaudio.h includes jobs/apjobs.h
 *    (as apaudiocues.h includes aparchive.h).
 *
 * ---------------------------------------------------------------------------
 * Data, memory and threads (for the implementation)
 * ---------------------------------------------------------------------------
 *
 *  - The prefetch: the sound's own block, decoded at create, stream_ring_us
 *    of it (or the whole sound, if shorter), interleaved floats at its own
 *    rate; read by the render as an in-memory sound's samples are, freed
 *    with the sound.
 *  - Stream records: max_streams plain records, made with the mixer, each
 *    the ring's two counters (frames written, frames read, monotonic
 *    uint64, through <atomic>), its block, frame count and channel count,
 *    its decoder's state, its voice, and the failure it met. Array of
 *    structs: the task and the render each touch one record's few fields
 *    together; the written counter (the task's) and the read counter (the
 *    render's) sit on separate cache lines, so the two never write one line.
 *  - The ring: one block per stream, interleaved floats at the sound's own
 *    rate, stream_ring_us of the source plus the resampler's reach (the
 *    sinc's half width behind the read position). The voice record gains
 *    its stream's index; the resampler reads source frames from the
 *    prefetch below its end and through the ring's wrap past it, in place of
 *    Sound::samples.
 *  - One writer and one reader per ring: the task's item for that stream
 *    writes past `written` and publishes it (release); the render reads up
 *    to `written` (acquire) and publishes `read`. One task at a time, waited
 *    for by the next update, so an item never meets another for its stream.
 *    Neither side waits for the other. A render short of frames mixes
 *    silence for the rest, adds them to the sound's underrun count (an
 *    atomic), and goes on; the voice's place still moves, so a late task
 *    doesn't make it drift behind the clock.
 *  - Letting go: a stream given up (voice virtual, stopped, ended, its sound
 *    destroyed) is marked by the render that stops reading it, with the
 *    render's sequence; apaudio_mixer_collect() waits for the task, then
 *    frees the decoder and ring once a later render has finished (the
 *    sounds' retire_sequence scheme); only then is the record free.
 *  - Decoders: dr_flac through its read and seek callbacks, its allocation
 *    callbacks pointed into the stream's block; stb_vorbis with its arena in
 *    the stream's block (the 2 MiB the whole-file decode uses; a source of 2
 *    GiB or more refused, as today); opusfile through op_open_callbacks with
 *    a seek callback, so every link is found at open and op_read doesn't
 *    allocate after, from the system heap. The fuzzer's fixes are kept: the
 *    stream paths use the same changed copies of dr_flac and stb_vorbis.
 *  - Bits: what a stream renders depends only on its bytes, the renders'
 *    sizes and when updates ran, never on the scheduler or its worker
 *    count, since each item writes only its own ring.
 *
 * ---------------------------------------------------------------------------
 * Tests (written before the code; pending while the calls return
 * APRESULT_NOT_IMPLEMENTED)
 * ---------------------------------------------------------------------------
 *
 *  apaudio_tests.c:
 *  - The same samples: each test file (tests/data/apaudio/, FLAC, Vorbis,
 *    Opus) played streamed and from its whole-file decode, rendered at 64-
 *    and 480-frame renders with an update between: bit for bit the same,
 *    at both sizes, looped and not, at rates 1, 0.5 and 2, with no
 *    scheduler and on ApJobs' pool at 1 and 4 workers.
 *  - At once: a new play's first render already has the prefetch's samples;
 *    the join from prefetch to ring is seamless (the same bits as above).
 *  - Underrun: renders with no update between, past the ring: silence, the
 *    sound's count exactly the frames short, another sound's untouched, the
 *    voice carrying on in time after.
 *  - Virtual and back: CONTINUE comes back where the clock says (the
 *    whole-file decode's samples from there), RESTART from 0 at once, PAUSE
 *    from where it stopped; KILL ends.
 *  - max_streams: one more streamed voice than streams is virtual, and real
 *    when another ends. One sound played twice at once: two streams.
 *  - Refusals at create, each its code and a message naming the part:
 *    every argument, each codec's garbage, empty and truncated headers, a
 *    source read failing (APRESULT_IO), a ring shorter than one render's
 *    reach at the fastest rate; and mid-stream (a file corrupted past its
 *    prefetch, a read failing after N bytes): the next update's result and
 *    message naming the voice, the voice ended, others playing on.
 *  - Two threads: one renders in a loop while the other plays, stops and
 *    destroys streamed voices and their sounds, updates and collects, on a
 *    pool of four; no read of a freed ring (ASan here; TSan on the Mac
 *    next), every stopped voice gone after the next update and collect.
 *  aprinoalloc_tests.c: a streamed voice of each codec playing, update and
 *    render, 100 frames, plain and on a pool: none of ours (a stream's start
 *    measured apart).
 *  apri_misuse_tests.c: regenerated (the changed and added calls).
 *  apaudio_fuzz: a mode bit for streamed decoding (the input as a source,
 *    read through the streaming decoders in small pieces with seeks), its
 *    output compared with the whole-file decode of the same bytes when both
 *    succeed; 15 minutes on Windows (apricotfields-windows-d3d12-fuzz).
 */

#ifndef APAUDIOSTREAM_DRAFT_H
#define APAUDIOSTREAM_DRAFT_H

#include <audio/apaudio.h>
#include <jobs/apjobs.h>

#if __cplusplus
extern "C" {
#endif

/* ---- Streamed sounds [ADDED] ---------------------------------------- */

/**
 * @brief Where a streamed sound's encoded bytes come from: random access,
 * read as its voices need them.
 *
 * ApAudio does no I/O of its own: a file, a mapped pack entry or memory the
 * caller keeps alive are all a #read the caller writes.
 *
 * Thread: #read is called by apaudio_sound_create(), on its caller's
 * thread, and by the decoding each apaudio_mixer_update() starts: on the
 * scheduler's workers (APAUDIO_MIXER_DESC `scheduler`), several at once,
 * for one source too, and beside the caller's own calls; or, with no
 * scheduler, inside the update. Never on the audio thread. It may block: a
 * worker waits, not the frame.
 *
 * Lifetime: the source, and whatever #user points at, must stay valid from
 * apaudio_sound_create() until the sound is destroyed and a later
 * apaudio_mixer_collect() has returned, or the mixer is destroyed: until then
 * a voice of it may still read.
 */
/* Frozen: held by value in APAUDIO_SOUND_DESC. It never changes; a new need gets a new type. */
typedef struct APAUDIO_SOURCE {
	/** Passed to #read as its `user`. May be NULL. */
	void *user;
	/** The encoded bytes' size, in bytes. Not 0. */
	uint64_t size;
	/**
	 * Copies exactly `count` bytes at `offset` into `buffer` and returns
	 * true; false on any failure, a short read included, which fails what
	 * was reading (APRESULT_IO). `offset + count` is never past #size. Never
	 * NULL.
	 */
	bool (*read)(void *user, uint64_t offset, void *buffer, uint64_t count);
} APAUDIO_SOURCE;

/**
 * @brief Which decoder a streamed sound is read with.
 */
typedef uint32_t APAUDIO_STREAM_CODEC;
enum {
	/** Not streamed: the sound's samples are in the desc, decoded. */
	APAUDIO_STREAM_CODEC_NONE = 0,
	/** FLAC (dr_flac), as apaudio_flac_decode() reads it. */
	APAUDIO_STREAM_CODEC_FLAC = 1,
	/** Ogg Vorbis (stb_vorbis), as apaudio_vorbis_decode() reads it. */
	APAUDIO_STREAM_CODEC_VORBIS = 2,
	/** Ogg Opus (libopus, opusfile), as apaudio_opus_decode() reads it: at
	 * 48,000 frames a second always. */
	APAUDIO_STREAM_CODEC_OPUS = 3,
};

/* ---- APAUDIO_SOUND_DESC [CHANGED]: three fields at its end ----------------
 *
 * As it would read, the existing fields' comments changed only where a
 * streamed sound reads them differently.
 */
typedef struct APAUDIO_SOUND_DESC {
	/** This struct's type, APRI_STRUCT_TYPE_APAUDIO_SOUND_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/**
	 * #frame_count frames of #channel_count interleaved samples at
	 * #sample_rate Hz, nominally in [-1, 1]. Copied. NULL for a streamed
	 * sound (#stream_codec not NONE).
	 */
	const float *samples;
	/** How many frames #samples holds. 0 for a streamed sound: the file
	 * says. */
	uint64_t frame_count;
	/**
	 * 1: mono - can be played from a direction. 2: stereo (left, right) -
	 * plays on the output's FL and FR, or mixed down on a mono output;
	 * ignores direction. Others: APRESULT_UNSUPPORTED. 0 for a streamed
	 * sound: the file says, and a file of other counts is UNSUPPORTED.
	 */
	uint32_t channel_count;
	/**
	 * The samples' own rate, kept: voices resample from it as they play
	 * (APAUDIO_RESAMPLER). 1000 to 768000 Hz. 0 for a streamed sound: the
	 * file says, and a rate outside that is UNSUPPORTED.
	 */
	uint32_t sample_rate;
	/** What its voices do while not real; required. */
	APAUDIO_VIRTUAL_BEHAVIOR virtual_behavior;
	/** The group its plays join unless a play names another; 0 for none. */
	APAUDIO_CONCURRENCY concurrency;
	/** The bus its plays go to unless a play names another; 0 for the master. */
	APAUDIO_BUS bus;
	/**
	 * NONE (0): the sound is #samples. Otherwise the sound is streamed:
	 * #stream_source read with this codec as its voices play (this file's
	 * "Streamed sounds"), and the mixer must have been made with
	 * `max_streams` above 0 (APRESULT_UNSUPPORTED otherwise). A streamed
	 * sound's voices are ranked by their gain alone: its loudness is taken
	 * as 1, not measured.
	 */
	APAUDIO_STREAM_CODEC stream_codec;
	/** A streamed sound's encoded bytes. Not read for NONE. */
	APAUDIO_SOURCE stream_source;
	/**
	 * A streamed sound: how far ahead of what it plays each voice decodes,
	 * in microseconds of the sound at its own rate - its ring's length - and
	 * how much of its start is decoded at create and kept, so a voice
	 * starting at its start plays at once. The ring must outlast the longest
	 * time between two apaudio_mixer_update() calls, times the fastest rate
	 * the voice plays at; past that, the voice plays silence until its
	 * stream catches up (counted: apaudio_sound_get_underrun_frames()). Not
	 * 0, and at least what one render can take at the fastest rate (8 x
	 * `max_frames` output frames, in the sound's frames), else
	 * APRESULT_INVALID_ARGUMENT; at most 10,000,000 (10 s). 100,000 to
	 * 500,000 is usual. Not read for NONE.
	 */
	uint32_t stream_ring_us;
} APAUDIO_SOUND_DESC;

/*
 * apaudio_sound_create(), for a streamed sound [CHANGED, comment only]:
 *
 * Reads the stream's header through `stream_source.read` - its channel
 * count, rate and length - and decodes its first `stream_ring_us`, the
 * prefetch every voice of it starts with. It keeps the source and the
 * prefetch, not the rest of the bytes. Each voice opens a decoder of its own
 * when it is first real (this file's "Streamed sounds").
 *
 * Adds to its failures: APRESULT_UNSUPPORTED for a streamed sound on a mixer
 * with no `max_streams`, or a file of a channel count or rate the mixer
 * doesn't play; APRESULT_CORRUPT_DATA for bytes the codec can't read as its
 * format, an empty source included; APRESULT_IO for a read that failed. The
 * message names the codec and the part (the header, a packet of the
 * prefetch, a read at an offset).
 */

/* ---- APAUDIO_MIXER_DESC [CHANGED]: two fields at its end ------------------
 *
 *	...every existing field, then:
 *
 *	/ **
 *	 * How many streamed voices decode at once: each holds a decoder and a
 *	 * ring while it is real or fading (this file's "Streamed sounds"). A
 *	 * streamed voice ranked real with every stream taken is virtual, as
 *	 * one with no real slot is. 0: none - streamed sounds are refused, as
 *	 * before. At most #max_real_voices.
 *	 * /
 *	uint32_t max_streams;
 *	/ **
 *	 * Where streamed voices are decoded: each apaudio_mixer_update() starts
 *	 * one task on it and the next waits for it (apjobs.h). Copied, as
 *	 * apjobs.h asks; its `user` must outlive the mixer. NULL: decoded
 *	 * inside each update, on its caller's thread. Not read with no
 *	 * #max_streams.
 *	 * /
 *	const APJOBS_SCHEDULER *scheduler;
 */

/* ---- apaudio_mixer_update() [CHANGED] ------------------------------------ */

/**
 * @brief Waits for the decoding the last update started, ranks every voice
 * and decides which are real (this file's "Prioritization"), applies
 * concurrency groups' limits, starts and ends triggered modifiers
 * (ducking), and starts decoding every real streamed voice's next frames.
 *
 * Thread: control. Call it once a frame - a main loop, a render loop - and
 * more often than any streamed sound's `stream_ring_us`. Without it, new
 * plays still start at once (the render's fast path; a streamed one plays
 * its prefetch), but virtual voices never come back and streamed voices
 * never play past their prefetch. Never blocks on the audio thread. With a
 * scheduler (APAUDIO_MIXER_DESC `scheduler`), the decoding runs on it after
 * the update returns, and the next update, or apaudio_mixer_collect(),
 * apaudio_mixer_set_format() or apaudio_mixer_destroy(), waits for it; with
 * none, it runs before the update returns. The decoding opens the decoder of
 * each streamed voice newly real, from the system heap: the one allocation
 * a stream makes. Frees nothing: apaudio_mixer_collect() does.
 *
 * A streamed voice whose stream fails (a read, bytes its codec can't decode
 * past the prefetch, a decoder that couldn't be opened) ends with a short
 * fade; the others play on. The failure is told by the update that waits
 * for the decoding that met it: the next one with a scheduler, this one
 * without.
 *
 * @param[in]  mixer Mixer to update.
 * @param[out] error Why a stream failed, if one did: the first of those this
 *                   update learnt of, naming its voice's ID, the codec and
 *                   the part, and how many more failed. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT for a null @p mixer; for
 *         the first stream failure this update learnt of, APRESULT_IO (its
 *         read failed), APRESULT_CORRUPT_DATA (its bytes) or
 *         APRESULT_OUT_OF_MEMORY (its decoder couldn't be opened). The
 *         ranking and every other voice's decoding are done either way.
 */
APRESULT apaudio_mixer_update(
    apaudio_mixer mixer,
    APAUDIO_ERROR *error);

/* ---- Underruns [ADDED] --------------------------------------------------- */

/**
 * @brief How many frames of silence a streamed sound's voices have played,
 * since it was made, because a ring had run dry.
 *
 * Thread: any. Counted by the render, in the mixer's output frames, summed
 * over the sound's voices. Nonzero means the sound's `stream_ring_us` is
 * too short for how often apaudio_mixer_update() runs, or its source's reads
 * too slow. Never reset; compare two reads.
 *
 * @param[in] mixer Mixer it belongs to.
 * @param[in] sound Sound to ask about.
 *
 * @return The count; 0 for an in-memory sound, a stale or 0 @p sound, or a
 *         null @p mixer.
 */
uint64_t apaudio_sound_get_underrun_frames(
    apaudio_mixer mixer,
    APAUDIO_SOUND sound);

/*
 * apaudio.h's opening comment [CHANGED]: "Out of scope: streaming (sounds are
 * whole, in memory)" goes; a section "Streamed sounds" is added, from this
 * file's "The shape, in one paragraph" and D3; the threads paragraph names
 * the decoding task (on the caller's scheduler, beside every role, touching
 * only its rings and sources) and which control calls wait for it.
 */

#if __cplusplus
}
#endif

#endif /* APAUDIOSTREAM_DRAFT_H */
