/*
 * rbtaudio_encode_draft.h - Rabbittus Audio's offline resampler and Ogg Opus
 * encoder: decoded samples in memory to samples at another rate, and to an
 * Ogg Opus file in memory, the inverse of rbtaudio_opus_decode().
 *
 * DRAFT, 2026-11-06, second version (the first the same day), revised after
 * its review: resampling is a call of its own, rbtaudio_pcm_resample(),
 * over the mixer's resamplers, and the encoder takes only the rates Opus
 * codes at, so no resampling is hidden inside it; Hazel compared.
 * APPROVED by the user 2026-11-06, as it stands after one review, and
 * built the same day (include/audio/rbtaudio.h; RBTAUDIO_TODO.md). This file
 * is the record of what was decided and why, and is no longer to be built
 * from. Not compilable as it stands. Every declaration is marked
 * [ADDED]; nothing in rbtaudio.h changes. Written against
 * include/audio/rbtaudio.h, src/audio/ and CMakeLists.txt as read on
 * 2026-11-06.
 *
 * ---------------------------------------------------------------------------
 * Why here
 * ---------------------------------------------------------------------------
 *
 * Turning samples into a compressed file any Rabbittus Audio caller can stream back
 * is general purpose: an asset tool, a recorder, an annotation memo.
 * Rabbittus already builds libopus 1.5.2 whole (its encoder included,
 * unused) and libogg 1.3.6 for its decoder; a caller above building its
 * own copies would link libopus twice into one program. Resampling a sound
 * offline is the same: the mixer has a windowed-sinc resampler
 * (src/audio/rbtaudio.cpp, SINC_ZEROS 8, its table built once per mixer)
 * and a linear one, and nothing outside the mixer can use either.
 *
 * ---------------------------------------------------------------------------
 * The answers this draft gives (each for approval)
 * ---------------------------------------------------------------------------
 *
 * 1. IN MEMORY, BOTH WAYS, AS THE DECODERS ARE: RBTAUDIO_PCM in; out, a new
 *    RBTAUDIO_PCM (freed with rbtaudio_pcm_free(), as a decode's) or the
 *    file's bytes in memory Rabbittus Audio allocates (malloc), freed with
 *    rbtaudio_encoded_free(). An encoder writing to a caller's sink in
 *    pieces (for files past memory) is a later addition if a caller needs
 *    one.
 * 2. RESAMPLING IS ITS OWN CALL. rbtaudio_pcm_resample() takes the samples,
 *    the rate wanted and RBTAUDIO_RESAMPLER (LINEAR or SINC, the mixer's
 *    own, UNSPECIFIED refused) and gives new samples. The sinc filter is
 *    the mixer's, its table built per call (a few kilobytes: offline
 *    work). Its length: frame_count times the ratio, rounded to nearest;
 *    the ends read as silence past the samples, as the mixer does at a
 *    sound's ends.
 * 3. THE ENCODER TAKES ONLY OPUS'S RATES (8,000, 12,000, 16,000, 24,000 or
 *    48,000 Hz); any other is RBT_RESULT_INVALID_ARGUMENT, its message
 *    naming rbtaudio_pcm_resample(). Every other choice is the caller's, in
 *    a based desc: the bit rate, VBR or not, the complexity, the Opus
 *    application (music and effects, speech, or low delay), the rate the
 *    sound was made at (written in OpusHead as its "input sample rate", a
 *    note for players, not acted on), and the Ogg stream's serial number
 *    (the Ogg specification asks for a random one; a caller wanting the
 *    same bytes from the same input gives a fixed one). Nothing is
 *    defaulted.
 * 4. THE FILE: RFC 7845's Ogg Opus. OpusHead with the encoder's lookahead
 *    as its pre-skip, channel mapping family 0 (mono or stereo; more
 *    channels are RBT_RESULT_UNSUPPORTED until a caller needs family 1);
 *    OpusTags with the vendor string "Rabbittus Audio" and no comments; 20 ms
 *    frames; the last page's granule position set so rbtaudio_opus_decode()
 *    returns exactly the input's frames (at 48 kHz: times 48,000 over the
 *    input's rate). A test holds it to that.
 * 5. MEMORY: the encoder's state is one allocation of
 *    opus_encoder_get_size(), initialised with opus_encoder_init(), so
 *    libopus allocates nothing of its own; the output grows by doubling,
 *    from a first size of the input's length at the bit rate plus the
 *    pages' overhead.
 * 6. DETERMINISM: the same input, desc and build give the same bytes, from
 *    both calls. libopus' float encoder may give other bytes on another CPU
 *    architecture; tests compare the decoded samples (within the bit rate's
 *    error, as SNR), not the bytes, across platforms. The resampler is the
 *    mixer's code, with its floating-point contraction as the mixer has it.
 *
 * ---------------------------------------------------------------------------
 * ABI
 * ---------------------------------------------------------------------------
 *
 * Added: one based desc (RBT_STRUCT_TYPE_AUDIO_OPUS_ENCODE_DESC, the next
 * free in Rabbittus Audio's range, 3009), one frozen output struct, one enum, three
 * functions. No break. check.sh --update-abi lists them; the misuse
 * generator gains them.
 *
 * ---------------------------------------------------------------------------
 * Tests (rbtaudio_tests, extended)
 * ---------------------------------------------------------------------------
 *
 * - Resampling: 44,100 to 48,000 and back, each resampler, mono and
 *   stereo; the length rule; a sine's frequency and level kept (SNR floor
 *   per resampler, measured once and written beside the test); the same
 *   rate in and out the same samples (each resampler); one frame.
 * - Encoding, then rbtaudio_opus_decode(): mono and stereo; each of Opus's
 *   rates; one frame, under one Opus frame, an exact number of frames and
 *   not; silence; the decoded frame count exactly the input's at 48 kHz,
 *   and the SNR of a sine sweep above a floor per bit rate measured once
 *   and written beside the test.
 * - The same input and desc twice: the same bytes, from each call.
 * - Every argument and desc field refused, in order, with nothing
 *   allocated (out zeroed); 44,100 Hz refused by the encoder;
 *   RBT_RESULT_UNSUPPORTED for three channels; NaN and infinities refused.
 * - Out of memory at each allocation (Rabbittus Audio's test allocator hook, as the
 *   decoders' tests), nothing leaked.
 * - No fuzz target: neither reads untrusted bytes (RBTAUDIO_PCM's samples
 *   are the caller's floats, each checked finite).
 *
 * ---------------------------------------------------------------------------
 * What was compared
 * ---------------------------------------------------------------------------
 *
 * - Hazel (.refs, src/Hazel/Audio/): decodes WAV, FLAC and Vorbis through
 *   miniaudio and stb_vorbis (AudioFileUtils.cpp) and packs the source
 *   files' bytes, unchanged, into its sound bank (SoundBank.cpp,
 *   ResourceManager.cpp); it encodes nothing and resamples only in its
 *   mixer. Nothing to take, but the confirmation that a cooked sound is a
 *   file the runtime's own decoder reads.
 * - libopusenc (xiph): ope_encoder_create_callbacks, its own resampler
 *   (speexdsp's), a random serial unless set. Not vendored: a third
 *   library for one page writer over libogg, and its resampler would be a
 *   second one beside the mixer's, hidden in the encoder.
 * - opus-tools' opusenc: the same OpusHead choices (input rate kept as a
 *   note, lookahead as pre-skip, 20 ms frames); it resamples inside, which
 *   answer 3 takes out.
 * - FMOD's and Wwise's asset tools encode Opus and Vorbis offline from
 *   decoded PCM, at a bit rate the author picks per sound.
 */

/* ---- Resampling [ADDED] -------------------------------------------------- */

/**
 * @brief [ADDED] Resamples samples in memory to another rate, with one of
 * the mixer's resamplers.
 *
 * Thread: any (it touches no mixer).
 *
 * @param[in]  pcm         The samples: any channel count, a rate from
 *                         8,000 to 192,000, at least one frame, every
 *                         sample finite. Only read during the call.
 * @param[in]  sample_rate The rate wanted: 8,000 to 192,000.
 * @param[in]  resampler   LINEAR or SINC.
 * @param[out] out_pcm     The samples at @p sample_rate, as many frames as
 *                         @p pcm's times the ratio of the rates, rounded to
 *                         nearest, at least one; to free with
 *                         rbtaudio_pcm_free(); zeroed on failure.
 * @param[out] error       Why it failed, if it did. May be NULL.
 *
 * @return RBT_RESULT_OK; RBT_RESULT_INVALID_ARGUMENT for a NULL @p pcm or
 *         @p out_pcm, no frames, no channels, a rate out of range, a
 *         sample not finite, or an UNSPECIFIED or unknown @p resampler
 *         (each its own check, in parameter order, the message naming
 *         it); RBT_RESULT_LIMIT for an output past 2^32 frames;
 *         RBT_RESULT_OUT_OF_MEMORY. Changes nothing on failure.
 */
RBT_RESULT rbtaudio_pcm_resample(
    const RBTAUDIO_PCM *pcm,
    uint32_t sample_rate,
    RBTAUDIO_RESAMPLER resampler,
    RBTAUDIO_PCM *out_pcm,
    RBTAUDIO_ERROR *error);

/* ---- Encoding [ADDED] ---------------------------------------------------- */

/** @brief [ADDED] What an Opus stream is tuned for: libopus' application. */
typedef uint32_t RBTAUDIO_OPUS_APPLICATION;
enum {
	/** Not chosen: refused. */
	RBTAUDIO_OPUS_APPLICATION_UNSPECIFIED = 0,
	/** Music and sound effects: OPUS_APPLICATION_AUDIO. */
	RBTAUDIO_OPUS_APPLICATION_AUDIO = 1,
	/** Speech: OPUS_APPLICATION_VOIP. */
	RBTAUDIO_OPUS_APPLICATION_VOIP = 2,
	/** The least delay, either: OPUS_APPLICATION_RESTRICTED_LOWDELAY. */
	RBTAUDIO_OPUS_APPLICATION_RESTRICTED_LOWDELAY = 3,
};

/**
 * @brief [ADDED] How rbtaudio_opus_encode() encodes.
 */
typedef struct RBTAUDIO_OPUS_ENCODE_DESC {
	/** RBT_STRUCT_TYPE_AUDIO_OPUS_ENCODE_DESC, and its chain (rbtcore.h, "Struct bases"). */
	RBT_STRUCT_BASE base;
	/** Bits a second, every channel together: 6,000 to 510,000. */
	uint32_t bitrate;
	/** libopus' complexity: 0 (fastest) to 10 (best). */
	uint32_t complexity;
	/** What the stream is tuned for. Not UNSPECIFIED. */
	RBTAUDIO_OPUS_APPLICATION application;
	/** The rate the sound was made at, before any resampling, written in
	 * OpusHead as a note for players: 1 to 4,294,967,295, not acted on. */
	uint32_t original_sample_rate;
	/** The Ogg stream's serial number, written as given. */
	uint32_t ogg_serial;
	/** true: variable bit rate around #bitrate (libopus' VBR). false:
	 * constant. */
	bool vbr;
} RBTAUDIO_OPUS_ENCODE_DESC;

/**
 * @brief [ADDED] An encoded file in memory, from an encode call.
 */
/* Frozen: filled per call. It never changes; a new need gets a new type. */
typedef struct RBTAUDIO_ENCODED {
	/** The file's bytes; NULL when #size is 0. */
	void *data;
	/** How many. */
	uint64_t size;
} RBTAUDIO_ENCODED;

/**
 * @brief [ADDED] Encodes samples as an Ogg Opus file (RFC 7845) in memory,
 * which rbtaudio_opus_decode() and streamed sounds read.
 *
 * Thread: any (it touches no mixer).
 *
 * @param[in]  pcm         The samples: 1 or 2 channels at 8,000, 12,000,
 *                         16,000, 24,000 or 48,000 Hz (others through
 *                         rbtaudio_pcm_resample() first), at least one
 *                         frame, every sample finite. Only read during the
 *                         call.
 * @param[in]  desc        How to encode. Only read during the call.
 * @param[out] out_encoded The file, to free with rbtaudio_encoded_free();
 *                         zeroed on failure.
 * @param[out] error       Why it failed, if it did. May be NULL.
 *
 * @return RBT_RESULT_OK; RBT_RESULT_INVALID_ARGUMENT for a NULL @p pcm,
 *         @p desc or @p out_encoded, no frames, a rate Opus doesn't code
 *         at, a sample not finite, or a desc field out of its range (each
 *         its own check, in parameter and field order, the message naming
 *         it); a struct-base code for @p desc's base; RBT_RESULT_UNSUPPORTED
 *         for more than two channels; RBT_RESULT_OUT_OF_MEMORY. Changes
 *         nothing on failure.
 */
RBT_RESULT rbtaudio_opus_encode(
    const RBTAUDIO_PCM *pcm,
    const RBTAUDIO_OPUS_ENCODE_DESC *desc,
    RBTAUDIO_ENCODED *out_encoded,
    RBTAUDIO_ERROR *error);

/**
 * @brief [ADDED] Frees what an encode call made, and zeroes @p encoded.
 *
 * @param[in,out] encoded The file to free. NULL is accepted and does
 *                        nothing.
 */
void rbtaudio_encoded_free(RBTAUDIO_ENCODED *encoded);
