#include "audio/apaudio.h"

#include <dr_flac.h>
#include <opusfile.h>

#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// stb_vorbis has no header of its own: these are the declarations of the
// calls used here, as stb_vorbis.c makes them (built with
// STB_VORBIS_NO_STDIO and STB_VORBIS_NO_PUSHDATA_API, CMakeLists.txt).
extern "C"
{
    typedef struct stb_vorbis stb_vorbis;
    typedef struct
    {
        char *alloc_buffer;
        int alloc_buffer_length_in_bytes;
    } stb_vorbis_alloc;
    typedef struct
    {
        unsigned int sample_rate;
        int channels;
        unsigned int setup_memory_required;
        unsigned int setup_temp_memory_required;
        unsigned int temp_memory_required;
        int max_frame_size;
    } stb_vorbis_info;
    stb_vorbis *stb_vorbis_open_memory(const unsigned char *data, int len, int *error, const stb_vorbis_alloc *alloc_buffer);
    stb_vorbis_info stb_vorbis_get_info(stb_vorbis *f);
    unsigned int stb_vorbis_stream_length_in_samples(stb_vorbis *f);
    int stb_vorbis_get_samples_float_interleaved(stb_vorbis *f, int channels, float *buffer, int num_floats);
    void stb_vorbis_close(stb_vorbis *f);
}

// The three compressed formats ApAudio reads (apaudio.h, "Decoding"): each
// decodes a whole file held in memory into an APAUDIO_PCM of interleaved
// floats, allocated with malloc as apaudio_wav_decode()'s are, so
// apaudio_pcm_free() frees any of them. Nothing here touches a mixer or any
// shared memory: every call may run on any thread, at once with any other.

namespace
{

void clear(APAUDIO_ERROR *error)
{
    if (error)
        error->message[0] = '\0';
}

APRESULT fail(APAUDIO_ERROR *error, APRESULT result, const char *format, ...)
{
    if (error)
    {
        va_list args;
        va_start(args, format);
        vsnprintf(error->message, sizeof(error->message), format, args);
        va_end(args);
    }
    return result;
}

// The checks every decode call starts with: [out_pcm] zeroed, [data] there
// if it has a size, the size within what the decoder takes.
APRESULT start(const void *data, uint64_t size, uint64_t max_frames, APAUDIO_PCM *out_pcm, uint64_t largest,
               const char *format, APAUDIO_ERROR *error)
{
    clear(error);
    if (!out_pcm)
        return fail(error, APRESULT_INVALID_ARGUMENT, "out_pcm is null");
    *out_pcm = APAUDIO_PCM{};
    if (!data && size != 0)
        return fail(error, APRESULT_INVALID_ARGUMENT, "data is null with a size of %llu", (unsigned long long)size);
    if (max_frames == 0)
        return fail(error, APRESULT_INVALID_ARGUMENT, "max_frames is 0");
    if (size == 0)
        return fail(error, APRESULT_CORRUPT_DATA, "the %s file is empty", format);
    if (size > largest)
        return fail(error, APRESULT_LIMIT, "the %s file is %llu bytes, past the %llu its decoder reads", format,
                    (unsigned long long)size, (unsigned long long)largest);
    return APRESULT_OK;
}

// What stb_vorbis may use for a file's setup and its decoding state: a
// buffer of its own, so no file can make it ask for more (a comment's
// length is a 32-bit field it would allocate). Ordinary files need a few
// hundred kilobytes.
constexpr int VORBIS_ARENA_BYTES = 2 << 20;

// Room for [frames] frames of [channels] floats, or NULL if that's past
// what memory can hold.
float *allocate_samples(uint64_t frames, uint32_t channels)
{
    if (channels == 0)
        return nullptr;
    if (frames > SIZE_MAX / sizeof(float) / channels)
        return nullptr;
    size_t count = (size_t)frames * channels;
    return (float *)malloc(count ? count * sizeof(float) : sizeof(float));
}

// Decoded frames, in room that grows as they come. The room starts at the
// length a file states, but never past what its bytes could decode to
// ([most_per_byte] frames a byte): a stated length is the file's to lie
// about, and a decoder writes only frames it decoded.
struct frames_t
{
    float *samples{nullptr};
    uint64_t count{0};
    uint64_t capacity{0};
    uint32_t channels{0};
    // The caller's max_frames, plus one: a decode that fills it has more
    // than the caller takes.
    uint64_t limit{0};
};

bool frames_start(frames_t &frames, uint32_t channels, uint64_t stated, uint64_t size, uint64_t most_per_byte,
                  uint64_t max_frames)
{
    uint64_t bound = size > UINT64_MAX / most_per_byte ? UINT64_MAX : size * most_per_byte;
    uint64_t capacity = stated < bound ? stated : bound;
    if (capacity < 4096)
        capacity = 4096;
    frames.limit = max_frames < UINT64_MAX ? max_frames + 1 : max_frames;
    if (capacity > frames.limit)
        capacity = frames.limit;
    frames.channels = channels;
    frames.samples = allocate_samples(capacity, channels);
    frames.capacity = frames.samples ? capacity : 0;
    return frames.samples != nullptr;
}

// Room for at least one frame more. False, with the room as it was, if
// there's no memory for it.
bool frames_grow(frames_t &frames)
{
    if (frames.count < frames.capacity)
        return true;
    uint64_t larger = frames.capacity * 2;
    if (larger > frames.limit)
        larger = frames.limit;
    if (larger <= frames.capacity)
        return false;
    if (larger > SIZE_MAX / sizeof(float) / frames.channels)
        return false;
    float *grown = (float *)realloc(frames.samples, (size_t)larger * frames.channels * sizeof(float));
    if (!grown)
        return false;
    frames.samples = grown;
    frames.capacity = larger;
    return true;
}

} // namespace

extern "C"
{

APRESULT apaudio_flac_decode(const void *data, uint64_t size, uint64_t max_frames, APAUDIO_PCM *out_pcm,
                             APAUDIO_ERROR *error)
{
    if (APRESULT result = start(data, size, max_frames, out_pcm, SIZE_MAX, "FLAC", error))
        return result;
    drflac *flac = drflac_open_memory(data, (size_t)size, nullptr);
    if (!flac)
        return fail(error, APRESULT_CORRUPT_DATA, "the bytes aren't a FLAC file dr_flac can decode");
    if (flac->channels == 0 || flac->sampleRate == 0)
    {
        unsigned channels = flac->channels, rate = flac->sampleRate;
        drflac_close(flac);
        return fail(error, APRESULT_CORRUPT_DATA, "the FLAC file has %u channels at %u frames a second", channels, rate);
    }
    // A FLAC block of up to 65,535 frames can be a few bytes (a constant
    // subframe): 65,536 frames a byte bounds what a file can hold.
    frames_t frames;
    if (!frames_start(frames, flac->channels, flac->totalPCMFrameCount, size, 65536, max_frames))
    {
        drflac_close(flac);
        return fail(error, APRESULT_OUT_OF_MEMORY, "no memory for the decoded frames");
    }
    for (;;)
    {
        if (frames.count >= frames.limit)
            break; // past max_frames: refused below
        if (!frames_grow(frames))
        {
            free(frames.samples);
            drflac_close(flac);
            return fail(error, APRESULT_OUT_OF_MEMORY, "no memory for %llu frames", (unsigned long long)frames.capacity * 2);
        }
        drflac_uint64 got = drflac_read_pcm_frames_f32(flac, frames.capacity - frames.count,
                                                       frames.samples + (size_t)frames.count * frames.channels);
        if (got == 0)
            break;
        frames.count += got;
    }
    uint32_t rate = flac->sampleRate;
    drflac_close(flac);
    if (frames.count > max_frames)
    {
        free(frames.samples);
        return fail(error, APRESULT_LIMIT, "the FLAC file decodes to more than max_frames (%llu)", (unsigned long long)max_frames);
    }
    out_pcm->samples = frames.samples;
    out_pcm->frame_count = frames.count;
    out_pcm->channel_count = frames.channels;
    out_pcm->sample_rate = rate;
    return APRESULT_OK;
}

APRESULT apaudio_vorbis_decode(const void *data, uint64_t size, uint64_t max_frames, APAUDIO_PCM *out_pcm,
                               APAUDIO_ERROR *error)
{
    if (APRESULT result = start(data, size, max_frames, out_pcm, INT_MAX, "Ogg Vorbis", error))
        return result;
    stb_vorbis_alloc arena;
    arena.alloc_buffer = (char *)malloc(VORBIS_ARENA_BYTES);
    arena.alloc_buffer_length_in_bytes = VORBIS_ARENA_BYTES;
    if (!arena.alloc_buffer)
        return fail(error, APRESULT_OUT_OF_MEMORY, "no memory for the Vorbis decoder");
    int open_error = 0;
    stb_vorbis *vorbis = stb_vorbis_open_memory((const unsigned char *)data, (int)size, &open_error, &arena);
    if (!vorbis)
    {
        free(arena.alloc_buffer);
        if (open_error == 3 /* VORBIS_outofmem: the arena */)
            return fail(error, APRESULT_LIMIT, "the Ogg Vorbis file's setup needs more than the decoder's %d bytes",
                        VORBIS_ARENA_BYTES);
        return fail(error, APRESULT_CORRUPT_DATA, "the bytes aren't an Ogg Vorbis file stb_vorbis can decode (its error %d)",
                    open_error);
    }
    stb_vorbis_info info = stb_vorbis_get_info(vorbis);
    if (info.channels <= 0 || info.sample_rate == 0)
    {
        stb_vorbis_close(vorbis);
        free(arena.alloc_buffer);
        return fail(error, APRESULT_CORRUPT_DATA, "the Vorbis stream has %d channels at %u frames a second", info.channels,
                    info.sample_rate);
    }
    uint32_t channels = (uint32_t)info.channels;
    // A Vorbis packet decodes to at most 4,096 frames (its largest block's
    // half) and takes at least a byte.
    frames_t frames;
    if (!frames_start(frames, channels, stb_vorbis_stream_length_in_samples(vorbis), size, 4096, max_frames))
    {
        stb_vorbis_close(vorbis);
        free(arena.alloc_buffer);
        return fail(error, APRESULT_OUT_OF_MEMORY, "no memory for the decoded frames");
    }
    for (;;)
    {
        if (frames.count >= frames.limit)
            break; // past max_frames: refused below
        if (!frames_grow(frames))
        {
            free(frames.samples);
            stb_vorbis_close(vorbis);
            free(arena.alloc_buffer);
            return fail(error, APRESULT_OUT_OF_MEMORY, "no memory for %llu frames", (unsigned long long)frames.capacity * 2);
        }
        uint64_t room = (frames.capacity - frames.count) * channels;
        int floats = room > (uint64_t)INT_MAX ? INT_MAX / (int)channels * (int)channels : (int)room;
        int got = stb_vorbis_get_samples_float_interleaved(vorbis, (int)channels,
                                                           frames.samples + (size_t)frames.count * channels, floats);
        if (got <= 0)
            break;
        frames.count += (uint64_t)got;
    }
    stb_vorbis_close(vorbis);
    free(arena.alloc_buffer);
    if (frames.count > max_frames)
    {
        free(frames.samples);
        return fail(error, APRESULT_LIMIT, "the Ogg Vorbis file decodes to more than max_frames (%llu)",
                    (unsigned long long)max_frames);
    }
    out_pcm->samples = frames.samples;
    out_pcm->frame_count = frames.count;
    out_pcm->channel_count = channels;
    out_pcm->sample_rate = info.sample_rate;
    return APRESULT_OK;
}

APRESULT apaudio_opus_decode(const void *data, uint64_t size, uint64_t max_frames, APAUDIO_PCM *out_pcm,
                             APAUDIO_ERROR *error)
{
    if (APRESULT result = start(data, size, max_frames, out_pcm, SIZE_MAX, "Ogg Opus", error))
        return result;
    int open_error = 0;
    OggOpusFile *file = op_open_memory((const unsigned char *)data, (size_t)size, &open_error);
    if (!file)
        return fail(error, open_error == OP_EFAULT ? APRESULT_OUT_OF_MEMORY : APRESULT_CORRUPT_DATA,
                    "the bytes aren't an Ogg Opus file opusfile can decode (its error %d)", open_error);
    int channel_count = op_channel_count(file, -1);
    ogg_int64_t total = op_pcm_total(file, -1);
    if (channel_count <= 0 || total < 0)
    {
        op_free(file);
        return fail(error, APRESULT_CORRUPT_DATA, "the Opus stream has %d channels and %lld frames", channel_count,
                    (long long)total);
    }
    uint32_t channels = (uint32_t)channel_count;
    // An Opus packet decodes to at most 120 ms (5,760 frames at 48 kHz) and
    // takes at least a byte.
    frames_t frames;
    if (!frames_start(frames, channels, (uint64_t)total, size, 5760, max_frames))
    {
        op_free(file);
        return fail(error, APRESULT_OUT_OF_MEMORY, "no memory for the decoded frames");
    }
    for (;;)
    {
        if (frames.count >= frames.limit)
            break; // past max_frames: refused below
        if (!frames_grow(frames))
        {
            free(frames.samples);
            op_free(file);
            return fail(error, APRESULT_OUT_OF_MEMORY, "no memory for %llu frames", (unsigned long long)frames.capacity * 2);
        }
        uint64_t room = (frames.capacity - frames.count) * channels;
        int floats = room > (uint64_t)INT_MAX ? INT_MAX / (int)channels * (int)channels : (int)room;
        int link = 0;
        int got = op_read_float(file, frames.samples + (size_t)frames.count * channels, floats, &link);
        if (got == OP_HOLE)
            continue; // a gap in the data: opusfile goes on after it
        if (got < 0)
        {
            free(frames.samples);
            op_free(file);
            return fail(error, APRESULT_CORRUPT_DATA, "decoding the Opus stream failed (opusfile's error %d)", got);
        }
        if (got == 0)
            break;
        // One channel count for every link: a file whose links differ
        // would need a layout per part.
        if (op_channel_count(file, link) != channel_count)
        {
            free(frames.samples);
            op_free(file);
            return fail(error, APRESULT_UNSUPPORTED, "the Opus file's links have different channel counts");
        }
        frames.count += (uint64_t)got;
    }
    op_free(file);
    if (frames.count > max_frames)
    {
        free(frames.samples);
        return fail(error, APRESULT_LIMIT, "the Ogg Opus file decodes to more than max_frames (%llu)",
                    (unsigned long long)max_frames);
    }
    out_pcm->samples = frames.samples;
    out_pcm->frame_count = frames.count;
    out_pcm->channel_count = channels;
    // Opus always decodes at 48 kHz (apaudio.h).
    out_pcm->sample_rate = 48000;
    return APRESULT_OK;
}
}
