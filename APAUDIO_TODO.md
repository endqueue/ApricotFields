# ApAudio TODO

What `include/audio/apaudio.h` gets wrong, and what it lacks compared with a good
audio framework (FMOD, Wwise, miniaudio, XAudio2, AVAudioEngine). ApAudio is in the
build (`CMakeLists.txt`, with headless tests in `tests/apaudio_tests.c`), and
`src/audio/apaudio.cpp` implements the whole header: the "For the implementation"
notes below describe what it does, except where it says otherwise at the top of
that file. It is included on its own: `aprend.h` is Aprend's umbrella only.

Items 1-4, the design bugs that were cheap to fix before implementing and
expensive after, are fixed in the header, and so are items 5, 6 and 11 (6 and 11 as
one design, sized for Apricot later becoming a game engine). Buses (item 10), the
last missing feature that would have forced API changes later, are built.
Everything else can be added without breaking callers.

Cues and sound banks - names over the mixer, defined by a manifest and its WAVs - are
`include/audio/apaudiocues.h` (`src/audio/apaudiocues.cpp`), general purpose so any
application has them. Its only test is `test_cues_clear_banks` in
`tests/apaudio_tests.c`, which loads banks from memory and clears them
(`apaudio_cues_clear_banks()`); since 2026-10-30 `test_cues_playing` covers
playing, sustained playings, exclusive starts, enabling, muting, the
playing limit, the sample rate and ZIP banks; the manifest's rules and
ducking's levels are still untested here. That call and its test are compiled and pass on
macOS (2026-10-22).

**What has run where (2026-10-30):** the dither, the limiter, collecting,
the folded flag and the three decoders build and pass `apaudio_tests` on
macOS (Apple M5 Pro, Debug) plain, under ASan and under TSan, and
`apaudio_fuzz` ran there (below, item 15). A caller's audio tests pass
over it. **Windows and Linux have built none of it**: there, libopus
through its own CMake under MSVC and GCC, libogg's `config_types.h` as
written here, `apaudio_tests` and the fuzz target's replay.
**Windows, 2026-10-30** (Windows 11 (ThinkPad P1 Gen 5; MSVC 14.51, Vulkan SDK 1.4.363.0, LLVM clang 23 for the fuzz preset)): libopus, libogg (its
`config_types.h` as written here), opusfile, dr_flac and stb_vorbis built
by MSVC and by clang with no configure change; `apaudio_tests` and the
fuzz replay pass on `windows-d3d12` and `windows-vulkan` in Debug, Release
and RelWithDebInfo, and under both `-asan` presets; `apaudio_fuzz` ran
10 minutes there for the first time (558,633 runs): nothing found.
`aprinoalloc_tests`' update and render make no allocation there either.
**Linux has built none of it.**

## Wrong things: design bugs in the draft

### 1. A lost stop can leave a looping sound playing forever - fixed

`apaudio_voice_stop` went through the same fixed-size queue as `apaudio_play`, and
returned `APRESULT_LIMIT` (the request dropped) when the queue was full. With
`loop = true`, a dropped stop meant the sound never ended.

- **Fixed in the header:** a voice exists from the moment `apaudio_play` returns
  it, and holds its own stop request. The mixer allocates `max_voices +
  queue_capacity` voice records at creation, so an accepted play always has one.
  `apaudio_voice_stop` sets the request on the record instead of queueing it, so it
  never needs queue space and never fails for a valid mixer. `queue_capacity` now
  counts plays only. A voice stopped before the audio thread starts it never
  sounds.
- **For the implementation:** the stop request and its `release_frames` go in one
  atomic word on the record. The voice ID encodes the record's index plus a
  generation count, so the lookup is a direct index and a stale ID never stops the
  record's next voice. The audio thread returns finished records to a lock-free
  free list.

### 2. Playing and destroying the same sound can crash - fixed

`apaudio_play(sound, ...)` was allowed from any thread, and `apaudio_sound_destroy`
freed the handle at once on the control thread. A play running at the same moment
used freed memory.

- **Fixed in the header:** sounds are generational IDs (`APAUDIO_SOUND`), like
  `APAUDIO_VOICE`: a slot in a table sized by `APAUDIO_MIXER_DESC.max_sounds`, plus
  the slot's reuse count. Destroying a sound bumps the count, so its ID is stale at
  once and `apaudio_play` returns `APRESULT_NOT_FOUND`. The audio thread re-checks
  the ID as it starts a voice, which catches a play that raced the destroy. Memory
  is freed, and the slot reused, only after the audio thread confirms it's done.
  `apaudio_play` and `apaudio_sound_destroy` now take the mixer.
- **Rejected alternatives:** a documented "don't play while destroying" rule only
  moves the bug to the caller. Reference counting doesn't close the race by itself:
  a thread releasing the last reference while another starts a play still frees
  memory under it.

## The mixer runs at the device's rate (decided 2026-10-28 with the user, card S4)

A mixer runs at the sample rate and channel count of the stream it feeds,
and the caller opens that stream in the device's own format with no OS
conversion, so nothing converts between the mixer and the device: each voice
is resampled as it plays (item 3), and latency and timing are the device's
own. As FMOD and Wwise mix at the device's rate. SpudAudio's OS conversion
stays for callers that want it, reported and chosen in full (spudlib's
`docs/spudaudio.md`).

### 3. Resampling at load time - fixed

`apaudio_sound_create` converted each sound to the mixer's sample rate once, "never
while playing". Serious mixers keep sounds at their original rate and resample each
voice as it plays. Converting at load:

- tied every sound to one device's rate, so a device change at another rate forced
  the caller to rebuild every sound from samples it kept;
- made pitch and playback-rate changes impossible.

- **Fixed in the header:** sounds keep their own rate (1000 to 768000 Hz) and each
  voice is resampled as it plays. `APAUDIO_RESAMPLER` moved from the sound desc to
  `APAUDIO_MIXER_DESC`; `SINC` band-limits when playing faster, so it doesn't alias.
- **Playback rate:** `APAUDIO_PLAY_DESC.rate` (pitch and speed together), in
  `[0.125, 8]`. Zero is rejected, so a zeroed desc doesn't silently pick a rate.
  Changing it while playing is item 5.
- **Device change:** new `apaudio_mixer_set_format()` changes the mixer's output
  format and `max_frames` between the old stream stopping and the new one starting.
  Sounds and handles are kept, and playing voices carry on from where they were.
- **For the implementation:** keep each voice's position as a fixed-point
  fraction of source frames (e.g. 32.32), so long loops don't drift. Sinc
  needs a few frames of history per voice, sized from `max_voices` at creation.

### 4. `APAUDIO_MIXER_DESC` breaks the `struct_size` versioning scheme - fixed

(`struct_size` was replaced by struct bases on 2026-10-19, `apricore.h`; the
rule this item set, that no desc holds another module's struct by value,
stands.)

It embedded `SPUDAUDIO_FORMAT`, whose size SpudLib controls. If SpudLib added a
field, every later ApAudio field would move, which is exactly what `struct_size`
exists to prevent.

- **Fixed in the header:** a new frozen `APAUDIO_FORMAT` holds only what the mixer
  needs: sample format, rate, channel count, interleaved, and a *pointer* to
  `channel_count` channel positions (copied by the call). It never grows, so
  versioned structs may hold it by value. `APAUDIO_MIXER_DESC.format` and
  `apaudio_mixer_set_format()` both take it, and the conventions now state the
  rule: no versioned struct holds another module's struct by value.
- **Converting:** `static inline apaudio_format_from_spudaudio()` builds one from a
  `SPUDAUDIO_FORMAT`. Being inline, it compiles against the caller's
  `spudaudio.h`, so it reads the caller's struct as the caller built it.
- **Rejected:** a pointer to `SPUDAUDIO_FORMAT` (it has no `struct_size`, so an
  older caller's smaller struct would be over-read); copying `positions[64]` (ties
  ApAudio's layout to `SPUDAUDIO_MAX_CHANNELS`); having the mixer read the stream's
  config itself (the stream needs the mixer as user data at creation, and it would
  break device-free tests and caller-chosen layouts).
- **Precedent:** Vulkan never grows a struct; CoreAudio keeps
  `AudioStreamBasicDescription` frozen and passes `AudioChannelLayout` by pointer.

### 5. Voice settings are fixed once a sound starts - fixed

Gain and direction were only set in `APAUDIO_PLAY_DESC`. If the camera orbits during
a 2-second sound, its direction goes stale.

- **Fixed in the header:** `apaudio_voice_set_gain`, `apaudio_voice_set_direction`
  and `apaudio_voice_set_rate`, from any thread. Like stops (item 1), a change is
  recorded on the voice itself, not queued, so it never needs queue space and
  can't be lost. Each setting holds only its latest value: changes between two
  renders coalesce, and the next render ramps to the last one (gain and pan
  across the render, rate as a glide), so nothing clicks or zippers. A voice
  changed before it starts starts with the new value; a stopping voice keeps the
  values it's fading out with.
- **For the implementation:** each setting is one atomic word on the voice record
  plus a "changed" bit. Gain and rate are single floats. The direction packs into
  one 64-bit word as two floats (azimuth, elevation), which is ample precision for
  panning and avoids a lock or a sequence counter. The audio thread swaps each
  changed word out once per render and ramps from the voice's current smoothed
  value.

### 6. Stolen voices are cut off instantly - fixed, with item 11

`APAUDIO_VOICE_STEAL_OLDEST` cut the old voice dead, which clicks. Stealing by age
was also a weak policy.

- **Fixed in the header, together with item 11,** as one prioritization system in
  the FMOD/Wwise/Unreal mould. There's no separate voice stealing any more:
  - **Logical vs real voices.** `max_voices` logical voices (the `APAUDIO_VOICE`
    IDs, cheap records) and `max_real_voices` mixing slots (the CPU budget).
  - **Ranking:** `APAUDIO_PLAY_DESC.priority` (0-255) first, then audibility: the
    voice's gain times its sound's loudness, measured once at creation. A voice
    never loses its slot to a lower priority. `virtual_threshold` makes very quiet
    voices virtual even with slots free. Ties within a few dB, and `min_real_us`,
    stop near-equal voices from swapping back and forth.
  - **Fades on every transition** (`transition_fade_us`). A pushed-out voice fades
    in one of `max_fading_voices` extra slots, so its replacement starts at once.
  - **`APAUDIO_VIRTUAL_BEHAVIOR`** per sound, overridable per play: `KILL` (UI
    sounds and one-shots), `CONTINUE`, `RESTART`, `PAUSE`.
  - **Concurrency groups** (`APAUDIO_CONCURRENCY`, generational IDs) replace
    per-sound limits: shared by many sounds, with `max_instances`, a
    `resolution` (`REJECT_NEW`, `STOP_OLDEST`, `STOP_QUIETEST`,
    `STOP_LOWEST_PRIORITY`), `retrigger_us` and `older_instance_gain`. A group's
    limit is a design rule that stops voices; the real-voice limit is a budget
    that only virtualizes them.
  - **`apaudio_mixer_update()`** on the control thread, once a frame, does the
    ranking. A new play doesn't wait for it: the render's fast path starts it in a
    free slot, or in place of the lowest-ranked real voice it outranks.
- **For the implementation:** start simple. Rank by sorting a few dozen voices;
  build `KILL` and `CONTINUE`, and `REJECT_NEW`, `STOP_OLDEST` and
  `STOP_QUIETEST`, first. The real-voice set the update decides is posted to the
  audio thread through a lock-free double buffer. The fast path uses the scores
  from the last update.

### 7. The integer output path is crude - fixed

Integer formats are hard-clipped with no limiter and no dither. A dozen overlapping
clicks can easily sum past 1.0.

- **Fix:** dither (TPDF) when converting to 16-bit, and an optional master limiter
  or soft clip.
- **Decided 2026-10-28 with the user (card A26):** TPDF dither always on in conversion to 16-bit (float
  output needs none); a lookahead limiter as an explicit field of the mixer's
  desc with no default, on or off by the caller's choice, since it changes the
  sound. As FMOD and Wwise.
- **Built 2026-10-30.** 16-bit output adds two uniform draws of half a
  bit each (a mixer-owned xorshift on the audio thread) before rounding;
  24- and 32-bit output aren't dithered. `APAUDIO_MIXER_DESC::limiter`
  (`APAUDIO_LIMITER_OFF` or `_ON`; 0 refused) with
  `limiter_lookahead_us` (1 to 100,000), `limiter_release_us` and
  `limiter_ceiling` (0 to 1], read only with it on. The limiter takes
  each frame's gain need (ceiling over its loudest channel), a sliding
  minimum over the lookahead, then a box average over the lookahead
  again - smooth, and at or below every need it passes, so nothing goes
  over the ceiling - and lets the gain back up no faster than the
  release; the mix comes out a lookahead late. Its buffers are made with
  the format's (create, `apaudio_mixer_set_format`): nothing allocates or
  locks on the audio thread. Tested (`test_dither`, `test_limiter`):
  silence dithers to a bit either side, a quarter-bit level averages back
  out of the dither, a quiet sound passes untouched a lookahead late, a
  mix of 2 never passes a 0.9 ceiling and settles on it, the same mix
  clips with the limiter off, and the refusals.

### 8. Folding rear sounds to the front is misleading on stereo - the flag built

On a stereo layout a sound behind the listener is mirrored to the front, so it
sounds like one in front. For an accessibility use, that's actively wrong.

- **Fix:** a "behind" cue, such as a gentle low-pass filter (needs item 12), or at
  least a flag saying the direction was folded.
- **Decided 2026-10-28 with the user (card A26):** the flag now, on the voice, saying its direction was
  folded from behind; the low-pass cue once item 12's per-voice filter exists.
- **Flag built 2026-10-30:** `apaudio_voice_is_folded(mixer, voice)`
  ("any" role), what the last render that panned the voice found: the
  render stores the voice's ID in an atomic when a ring it pans on has the
  rear gap and the direction lies in it. Tested (`test_folded`): behind on
  stereo folded, ahead and directionless not, a turn to face it clears
  it, behind on 5.1 not. The low-pass cue waits for item 12.

### 9. Smaller problems

- **Freed sound memory is reclaimed "by a later control call",** so an idle app can
  hold it indefinitely. An explicit `apaudio_mixer_collect()` is clearer.
  **Decided 2026-10-28 with the user (card A26):** the explicit `apaudio_mixer_collect()`, called by the
  owner from its update (the root `CLAUDE.md`'s "Threading": nothing behind the
  caller's back). **Built 2026-10-30:** `apaudio_mixer_collect()` (control);
  the update, `apaudio_mixer_set_format()` and `apaudio_sound_destroy()`
  no longer free anything, and `apaudio_sound_create()` collects only when
  it finds every slot taken. Tested (`test_collect`); ASan and leaks see
  the rest. A caller's audio update calls it after the mixer's.
- **A play that ends before sounding is invisible to the caller.** A play dropped
  by its group's `REJECT_NEW` or `retrigger_us`, or killed for lack of a real slot,
  still got a voice ID from `apaudio_play`, and the caller can't find out. Item 13's
  events ("voice killed", "voice went virtual") are the fix.

## Real gaps: things a good framework has

### 10. Buses (submix groups) - built

Voices are routed to buses (`apaudio_bus_*`), a tree under the master bus, each
with its own gain, rate, mute and pause; modifiers scale buses together, and a
modifier with a bus trigger is ducking (lowering one bus while another plays).
`test_buses` and `test_modifiers` in `tests/apaudio_tests.c` cover them. Still to
come: processing on a bus's own mix, effects and metering (`apaudio.h`).

### 11. Voice priority and virtual voices - fixed, with item 6

There was no priority, and no virtual voices: tracking sounds too quiet to hear
without mixing them, so they come back correctly. Both are now part of item 6's
prioritization system.

### 12. Effects

No filters, reverb, compressor or limiter. A per-voice low-pass filter is the
minimum: it gives occlusion and "behind" cues (item 8).

### 13. Events from playback

No voice-finished or loop-point notifications (delivered off the audio thread), and
no way to ask how far a voice has played. `apaudio_voice_is_live()` is the polled
part of this: whether a voice has ended. Why it ended, and events, are still missing.

### 14. Monitoring

No peak levels, active voice count, audio-thread CPU time or glitch counts.

### 15. Streaming and compressed formats - the decoders built

Sounds are whole, in memory, and WAV only. Fine for clicks, not for anything longer.
Needs streamed voices and FLAC, Opus and Vorbis decoders.

**Decided 2026-10-28 with the user (card A26):** dr_flac (FLAC), libopus with opusfile (Opus), stb_vorbis
(Ogg Vorbis), all small and permissively licensed, vendored in `../.deps/`
and linked privately. No MP3: Opus does more at the sizes that matter.

**Decoders built 2026-10-30:** `apaudio_flac_decode`, `apaudio_vorbis_decode`
and `apaudio_opus_decode`, each a whole file in memory to an `APAUDIO_PCM`
as the WAV decode is, with a `max_frames` the caller sets
(`APRESULT_LIMIT` past it: silence compresses to nothing). Pinned and
fetched when `../.deps/` has no copy: dr_libs `flac-0.13.3`, stb at
`003501f`, libogg `v1.3.6`, libopus `v1.5.2` (its own CMake, float,
static), opusfile `v0.12` (no HTTP); each its own static library, warnings
off, linked PRIVATE (`CMakeLists.txt`, "ApAudio's decoders"). No room is
sized from a length a file states past what its bytes could decode to
(4,096 frames a byte for Vorbis, 5,760 for Opus, 65,536 for FLAC); it grows
as frames decode. dr_flac and stb_vorbis are read as they are and written
into the build directory changed, as Hosek and Wilkie's data is: dr_flac,
without SIMD, never writes its CPU capability flags; stb_vorbis's CRC table
is constants (`src/audio/apaudio_vorbis_crc.inc`) and its other tables
`const`, so decodes on many threads write no shared memory. stb_vorbis gets
a 2 MiB arena for its state, and five fixes for what `apaudio_fuzz` found
(below). Opus comes back at 48 kHz always. Tested (`test_decoders`,
`tests/data/apaudio/`, a 0.1 s two-tone stereo file made with ffmpeg as
WAV, FLAC, Vorbis and Opus): FLAC equal to the WAV sample for sample,
Vorbis and Opus the WAV's length, channels, rate and energy within a
tenth, each refusing the others' files, garbage, empty input, NULLs, a
`max_frames` of 0, and too few frames, a truncated file decoding or
refused, never past its end.

`apaudio_fuzz` (`tests/fuzz/`, new; the first byte picks the decoder, the
seeds are the four test files) found, in its first hour on macOS:
- stb_vorbis: a vendor or comment length past 2^31 read as negative, so
  `len + 1` wrapped and its setup allocators returned memory outside their
  buffer (a heap use-after-free with the arena, a write before the buffer
  without it); a comment list whose allocation failed read as NULL while
  tearing down; a codebook of dimension 0 dividing by it. Fixed in the
  rewrite: lengths below 0 refused, the allocators refuse sizes below 0 or
  that would wrap, the teardown checks the list, a zero dimension is an
  invalid setup. Its signed shifts, wrapping sums and out-of-range float
  casts - undefined in C, harmless on every compiler here - are left to
  ASan: UBSan's checks for those three are off for it alone.
- ApricotFields' own: the Vorbis room sized from a stated length of 33 GB,
  and a FLAC message read after its decoder was closed. Fixed.
After the fixes, a last 15-minute run from the corpus (1,135,369 inputs,
Debug, ASan and UBSan) found nothing. Not run on Windows or Linux.
Each input is kept in `tests/fuzz/regressions/apaudio_fuzz/` and replayed
by every build. Not done: streamed voices (decoding into a ring the owner's
update fills, so the audio thread never decodes or allocates) - a design
of its own, next.

### 16. Spatial audio beyond angles at play time

**Wanted by a second caller (2026-10-28):** a game engine's listener and
emitter components feed world places here each step, so world-space sources,
distance attenuation, cones and doppler, with streamed voices (item 15), are
the next work; binaural later.

- **World-space sources and a listener:** frameworks place sources and a listener in
  3D, with distance attenuation curves, directional cones and doppler. Angles are
  enough for a sound placed once, but a moving listener needs updates (item 5).
  Items 6 and 11 made this the next game-engine step: audibility should include
  distance attenuation, `apaudio_mixer_update()` is where listener and emitter maths
  goes, and it enables a `STOP_FARTHEST` resolution and distance-based priority
  offsets. The voice record should leave room for a world position alongside its
  direction.
- **Binaural (HRTF) rendering:** elevation can't be heard on ordinary stereo, and most
  users listen on headphones or laptop speakers.
- **Ambisonics,** for many simultaneous sources.

## Platform behavior a caller has to handle

Not ApAudio's to decide, but nothing in the design prompts its callers to handle it.

- **System UI sound conventions.** On macOS, UI sounds should normally go to the
  system sounds (alert) device (`SPUDAUDIO_DEFAULT_ROLE_SYSTEM_SOUNDS`) and respect
  "Play user interface sound effects". That's the caller's choice.
- **iOS audio session.** On the Apple roadmap, iOS needs an `AVAudioSession`
  category, or sounds are silenced by the ringer switch and fight with other apps'
  audio. Nothing in SpudAudio or ApAudio models that yet.
- **Following the default device** when it changes is the caller's job by design.
  SpudAudio reports device events on CoreAudio, WASAPI and ALSA
  (`spudaudio_set_device_event_callback`); only the stub built with no audio API
  refuses.
