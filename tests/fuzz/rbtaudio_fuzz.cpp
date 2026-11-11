// rbtaudio_fuzz - Rabbittus Audio's decoders over a file held in memory: WAV, FLAC
// (dr_flac), Ogg Vorbis (stb_vorbis) and Ogg Opus (opusfile and libopus);
// and the same three decoders streamed (RBTAUDIO_TODO.md item 15).
//
// The input's first byte picks the decoder (its value modulo 4: WAV, FLAC,
// Vorbis, Opus) and the mode: with bit 2 (4) set, FLAC, Vorbis and Opus are
// streamed, and bit 3 (8) loops the voice; the rest is the file. Beyond not
// crashing, it checks:
// - a failure leaves the PCM zeroed and a terminated message;
// - a success has a channel and a rate, samples for every frame it
//   claims (each is read), and frees cleanly.
// Streamed: the file is a streamed sound's source on a mixer at the file's
// own rate, read through the streaming decoders as its voice plays (its
// ring a few renders long, so it refills, wraps and, looped, seeks), and
// - every read stays within the source, and a refused create leaves no
//   sound;
// - a failure is a result with a terminated message;
// - while both succeed, every render is the bits a second mixer renders
//   from the whole-file decode of the same bytes.

#include "rbtfuzz.hpp"

#include "audio/rbtaudio.h"

#include <string.h>

namespace
{
// Past this many frames a fuzzed file only spends time and memory: the
// decoders' own limit.
constexpr uint64_t MAX_FRAMES = 1u << 22;
constexpr uint64_t MAX_SAMPLES = 1u << 24;
// The streamed mode's renders: 64 frames, at most 400 of them.
constexpr uint32_t RENDER_FRAMES = 64;
constexpr int MAX_RENDERS = 400;

RBT_RESULT wav(const void *data, uint64_t size, RBTAUDIO_PCM *pcm, RBTAUDIO_ERROR *error)
{
    return rbtaudio_wav_decode(data, size, pcm, error);
}
RBT_RESULT flac(const void *data, uint64_t size, RBTAUDIO_PCM *pcm, RBTAUDIO_ERROR *error)
{
    return rbtaudio_flac_decode(data, size, MAX_FRAMES, pcm, error);
}
RBT_RESULT vorbis(const void *data, uint64_t size, RBTAUDIO_PCM *pcm, RBTAUDIO_ERROR *error)
{
    return rbtaudio_vorbis_decode(data, size, MAX_FRAMES, pcm, error);
}
RBT_RESULT opus(const void *data, uint64_t size, RBTAUDIO_PCM *pcm, RBTAUDIO_ERROR *error)
{
    return rbtaudio_opus_decode(data, size, MAX_FRAMES, pcm, error);
}
typedef RBT_RESULT (*decode_t)(const void *, uint64_t, RBTAUDIO_PCM *, RBTAUDIO_ERROR *);
const decode_t decoders[4] = {wav, flac, vorbis, opus};

struct source_t
{
    const uint8_t *bytes;
    uint64_t size;
};

bool source_read(void *user, uint64_t offset, void *buffer, uint64_t count)
{
    const source_t *source = static_cast<const source_t *>(user);
    RBTFUZZ_CHECK(offset <= source->size && count <= source->size - offset);
    memcpy(buffer, source->bytes + offset, (size_t)count);
    return true;
}

const CRAUDIO_CHANNEL_POSITION stereo[2] = {CRAUDIO_CHANNEL_POSITION_FL, CRAUDIO_CHANNEL_POSITION_FR};

RBTAUDIO_MIXER_DESC mixer_desc(uint32_t rate, uint32_t max_streams)
{
    RBTAUDIO_MIXER_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_AUDIO_MIXER_DESC;
    desc.format.sample_format = CRAUDIO_SAMPLE_FORMAT_F32;
    desc.format.sample_rate = rate;
    desc.format.channel_count = 2;
    desc.format.interleaved = true;
    desc.format.positions = stereo;
    desc.max_frames = RENDER_FRAMES;
    desc.max_voices = 4;
    desc.max_real_voices = 2;
    desc.max_sounds = 2;
    desc.queue_capacity = 4;
    desc.resampler = RBTAUDIO_RESAMPLER_LINEAR;
    desc.limiter = RBTAUDIO_LIMITER_OFF;
    desc.gain = 1.0f;
    desc.max_streams = max_streams;
    return desc;
}

void streamed(RBTAUDIO_STREAM_CODEC codec, bool loop, const uint8_t *data, size_t size)
{
    // The whole-file decode first: what the stream must match, and the rate
    // the mixers run at (the stream then plays it unresampled).
    RBTAUDIO_PCM pcm;
    memset(&pcm, 0, sizeof(pcm));
    RBT_RESULT whole = decoders[codec](data, size, &pcm, nullptr);
    uint32_t rate = whole == RBT_RESULT_OK && pcm.sample_rate >= 1000 && pcm.sample_rate <= 768000 ? pcm.sample_rate
                                                                                                : 48000;
    RBTAUDIO_MIXER_DESC desc = mixer_desc(rate, 1);
    rbtaudio_mixer mixer = nullptr;
    if (rbtaudio_mixer_create(&desc, &mixer, nullptr) != RBT_RESULT_OK)
    {
        rbtaudio_pcm_free(&pcm);
        return;
    }
    source_t source = {data, size};
    RBTAUDIO_SOUND_DESC sd;
    memset(&sd, 0, sizeof(sd));
    sd.base.type = RBT_STRUCT_TYPE_AUDIO_SOUND_DESC;
    sd.virtual_behavior = RBTAUDIO_VIRTUAL_CONTINUE;
    sd.stream_codec = codec;
    sd.stream_source.user = &source;
    sd.stream_source.size = size;
    sd.stream_source.read = source_read;
    // A few renders' worth at the fastest rate (8 x 64 frames), and a
    // little: the ring refills every update.
    sd.stream_ring_us = (uint32_t)(8ull * RENDER_FRAMES * 1000000 / rate + 1000);
    RBTAUDIO_SOUND sound = 99;
    RBTAUDIO_ERROR error;
    RBT_RESULT created = rbtaudio_sound_create(mixer, &sd, &sound, &error);
    RBTFUZZ_CHECK(memchr(error.message, '\0', sizeof(error.message)) != nullptr);
    if (created != RBT_RESULT_OK)
    {
        RBTFUZZ_CHECK(sound == 0);
        rbtaudio_mixer_destroy(mixer);
        rbtaudio_pcm_free(&pcm);
        return;
    }
    // The reference: the whole-file decode, in memory.
    rbtaudio_mixer reference = nullptr;
    RBTAUDIO_SOUND whole_sound = 0;
    if (whole == RBT_RESULT_OK && pcm.frame_count > 0 && (pcm.channel_count == 1 || pcm.channel_count == 2))
    {
        RBTAUDIO_MIXER_DESC rd = mixer_desc(rate, 0);
        if (rbtaudio_mixer_create(&rd, &reference, nullptr) == RBT_RESULT_OK)
        {
            RBTAUDIO_SOUND_DESC wd;
            memset(&wd, 0, sizeof(wd));
            wd.base.type = RBT_STRUCT_TYPE_AUDIO_SOUND_DESC;
            wd.samples = pcm.samples;
            wd.frame_count = pcm.frame_count;
            wd.channel_count = pcm.channel_count;
            wd.sample_rate = pcm.sample_rate;
            wd.virtual_behavior = RBTAUDIO_VIRTUAL_CONTINUE;
            if (rbtaudio_sound_create(reference, &wd, &whole_sound, nullptr) != RBT_RESULT_OK)
                whole_sound = 0;
        }
    }
    RBTAUDIO_PLAY_DESC play;
    memset(&play, 0, sizeof(play));
    play.base.type = RBT_STRUCT_TYPE_AUDIO_PLAY_DESC;
    play.gain = 1.0f;
    play.rate = 1.0f;
    play.priority = 128;
    play.loop = loop;
    play.placement = RBTAUDIO_PLACEMENT_ANGLES;
    RBTAUDIO_VOICE voice = 0;
    RBTFUZZ_CHECK(rbtaudio_play(mixer, sound, &play, &voice) == RBT_RESULT_OK);
    if (whole_sound)
        RBTFUZZ_CHECK(rbtaudio_play(reference, whole_sound, &play, nullptr) == RBT_RESULT_OK);
    static float out[2 * RENDER_FRAMES], expected[2 * RENDER_FRAMES];
    bool compare = whole_sound != 0;
    for (int r = 0; r < MAX_RENDERS && rbtaudio_voice_is_live(mixer, voice); r++)
    {
        rbtaudio_mixer_render(mixer, out, RENDER_FRAMES, nullptr);
        if (compare)
        {
            rbtaudio_mixer_render(reference, expected, RENDER_FRAMES, nullptr);
            RBTFUZZ_CHECK(memcmp(out, expected, sizeof(out)) == 0);
        }
        RBT_RESULT updated = rbtaudio_mixer_update(mixer, &error);
        RBTFUZZ_CHECK(memchr(error.message, '\0', sizeof(error.message)) != nullptr);
        if (updated != RBT_RESULT_OK)
        {
            RBTFUZZ_CHECK(updated == RBT_RESULT_CORRUPT_DATA || updated == RBT_RESULT_OUT_OF_MEMORY ||
                           updated == RBT_RESULT_IO);
            compare = false;
        }
        rbtaudio_mixer_collect(mixer);
    }
    rbtaudio_sound_destroy(mixer, sound);
    rbtaudio_mixer_collect(mixer);
    rbtaudio_mixer_destroy(mixer);
    rbtaudio_mixer_destroy(reference);
    rbtaudio_pcm_free(&pcm);
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 1)
        return 0;
    uint32_t pick = data[0] % 4;
    if ((data[0] & 4) && pick != 0)
    {
        streamed((RBTAUDIO_STREAM_CODEC)pick, (data[0] & 8) != 0, data + 1, size - 1);
        return 0;
    }
    decode_t decode = decoders[pick];
    RBTAUDIO_PCM pcm;
    memset(&pcm, 0xFF, sizeof(pcm));
    RBTAUDIO_ERROR error;
    RBT_RESULT result = decode(data + 1, size - 1, &pcm, &error);
    RBTFUZZ_CHECK(memchr(error.message, '\0', sizeof(error.message)) != nullptr);
    if (result != RBT_RESULT_OK)
    {
        RBTFUZZ_CHECK(pcm.samples == nullptr && pcm.frame_count == 0 && pcm.channel_count == 0);
        return 0;
    }
    RBTFUZZ_CHECK(pcm.channel_count > 0);
    RBTFUZZ_CHECK(pcm.sample_rate > 0);
    // A file whose audio is empty decodes to no frames, and may have no
    // samples (RBTAUDIO_PCM).
    RBTFUZZ_CHECK(pcm.samples != nullptr || pcm.frame_count == 0);
    // Every sample the PCM claims is there to read (ASan sees one that
    // isn't); what a broken file decodes to is its own business.
    uint64_t samples = pcm.frame_count * pcm.channel_count;
    volatile float sink = 0.0f;
    for (uint64_t i = 0; i < samples && i < MAX_SAMPLES; i++)
        sink = sink + pcm.samples[i];
    (void)sink;
    rbtaudio_pcm_free(&pcm);
    RBTFUZZ_CHECK(pcm.samples == nullptr);
    return 0;
}
