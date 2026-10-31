#include "audio/apaudio.h"
#include "apaudio_stream.hpp"

#include <dr_flac.h>
#include <opusfile.h>

#include <limits.h>
#include <new>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// stb_vorbis has no header of its own: these are the declarations of the
// calls used here, as stb_vorbis.c makes them (built with
// STB_VORBIS_NO_PUSHDATA_API, and its FILE an apaudio_stream_reader:
// CMakeLists.txt, "ApAudio's decoders").
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
    stb_vorbis *stb_vorbis_open_file_section(apaudio_stream_reader *file, int close_handle_on_close, int *error,
                                             const stb_vorbis_alloc *alloc_buffer, unsigned int len);
    int stb_vorbis_seek(stb_vorbis *f, unsigned int sample_number);
    int stb_vorbis_seek_start(stb_vorbis *f);
    int stb_vorbis_get_error(stb_vorbis *f);
}

// The three compressed formats ApAudio reads (apaudio.h, "Decoding"): each
// decodes a whole file held in memory into an APAUDIO_PCM of interleaved
// floats, allocated with malloc as apaudio_wav_decode()'s are, so
// apaudio_pcm_free() frees any of them. Nothing here touches a mixer or any
// shared memory: every call may run on any thread, at once with any other.
// Below them, the same decoders streamed (apaudio_stream.hpp).

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

// ---- Streaming (apaudio_stream.hpp) ---------------------------------------------
//
// A streamed sound's voices decode through the same decoders as the calls
// above, opened over a reader of the caller's source instead of memory:
// dr_flac through its read, seek and tell callbacks, its one allocation
// bumped into the stream's block; stb_vorbis through its FILE * path, which
// its changed copy reads through apaudio_stream_reader (CMakeLists.txt), its
// state in an arena in the block; opusfile through op_open_callbacks with a
// seek callback, so every link is found at open, its own state on the system
// heap (opusfile and libopus allocate with malloc and take no allocator).
// Each fills the caller's floats a block at a time.

namespace
{

// An Opus packet decodes to at most 120 ms: 5,760 frames at 48 kHz. Decoding
// into room for a whole one keeps op_read_float() from allocating a buffer of
// its own the first time a packet doesn't fit.
constexpr uint32_t OPUS_PACKET_FRAMES = 5760;
// stb_vorbis reads a byte at a time: its reader takes the source this much at
// once.
constexpr uint32_t VORBIS_READ_BYTES = 16 << 10;
// stb_vorbis's offsets are signed 32-bit: a source of 2 GiB or more is
// refused, as apaudio_vorbis_decode() refuses one.
constexpr uint64_t VORBIS_MAX_BYTES = INT_MAX;

size_t line_round(size_t bytes)
{
    return (bytes + 63) & ~(size_t)63;
}

// Fills [failure] with what the reader met, if its source's read failed,
// else with [result] and [part]. Always false, for a caller's return.
bool stream_failed(const StreamDecoder &decoder, StreamFailure &failure, APRESULT result, uint32_t part,
                   int32_t codec_error)
{
    failure = StreamFailure{};
    failure.frame = decoder.position;
    if (decoder.reader.failed)
    {
        failure.result = APRESULT_IO;
        failure.part = STREAM_PART_READ;
        failure.offset = decoder.reader.failed_offset;
        failure.count = decoder.reader.failed_count;
        return false;
    }
    failure.result = result;
    failure.part = part;
    failure.codec_error = codec_error;
    return false;
}

StreamDecoder &decoder_of(void *user)
{
    return *static_cast<StreamDecoder *>(user);
}

// dr_flac's callbacks.
size_t flac_read(void *user, void *out, size_t bytes)
{
    return (size_t)apaudio_stream_reader_read(&decoder_of(user).reader, out, bytes);
}

drflac_bool32 flac_seek(void *user, int offset, drflac_seek_origin origin)
{
    apaudio_stream_reader &reader = decoder_of(user).reader;
    int64_t base = origin == DRFLAC_SEEK_SET ? 0 : origin == DRFLAC_SEEK_CUR ? (int64_t)reader.position
                                                                              : (int64_t)reader.size;
    int64_t to = base + offset;
    if (to < 0)
        return DRFLAC_FALSE;
    return apaudio_stream_reader_seek(&reader, (uint64_t)to) ? DRFLAC_TRUE : DRFLAC_FALSE;
}

drflac_bool32 flac_tell(void *user, drflac_int64 *cursor)
{
    *cursor = (drflac_int64)decoder_of(user).reader.position;
    return DRFLAC_TRUE;
}

// dr_flac allocates once, at open (with no metadata callback and no Ogg):
// bumped through the block's arena, or from the heap and counted.
void *flac_malloc(size_t bytes, void *user)
{
    StreamDecoder &decoder = decoder_of(user);
    void *memory = nullptr;
    if (!decoder.arena)
    {
        memory = malloc(bytes);
        if (memory)
            decoder.arena_used += bytes;
    }
    else
    {
        size_t at = line_round(decoder.arena_used);
        if (at <= decoder.arena_size && bytes <= decoder.arena_size - at)
        {
            decoder.arena_used = at + bytes;
            memory = decoder.arena + at;
        }
    }
    if (!memory)
    {
        decoder.allocation_failed = true;
        decoder.allocation_bytes = bytes;
    }
    return memory;
}

void *flac_realloc(void *memory, size_t bytes, void *user)
{
    // Only dr_flac's whole-file reads reallocate, and none is made here.
    if (!memory)
        return flac_malloc(bytes, user);
    return nullptr;
}

void flac_free(void *memory, void *user)
{
    if (!decoder_of(user).arena)
        free(memory);
}

// opusfile's callbacks.
int opus_read(void *user, unsigned char *out, int bytes)
{
    apaudio_stream_reader &reader = decoder_of(user).reader;
    if (bytes <= 0)
        return 0;
    uint64_t got = apaudio_stream_reader_read(&reader, out, (uint64_t)bytes);
    return reader.failed ? -1 : (int)got;
}

int opus_seek(void *user, opus_int64 offset, int whence)
{
    apaudio_stream_reader &reader = decoder_of(user).reader;
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t)reader.position : (int64_t)reader.size;
    if (offset > INT64_MAX - base)
        return -1;
    int64_t to = base + offset;
    if (to < 0 || !apaudio_stream_reader_seek(&reader, (uint64_t)to))
        return -1;
    return 0;
}

opus_int64 opus_tell(void *user)
{
    return (opus_int64)decoder_of(user).reader.position;
}

bool open_flac(StreamDecoder &decoder, unsigned char *room, const StreamInfo *info, StreamFailure &failure)
{
    if (room)
    {
        decoder.arena = room;
        decoder.arena_size = line_round((size_t)info->flac_bytes);
    }
    drflac_allocation_callbacks callbacks;
    callbacks.pUserData = &decoder;
    callbacks.onMalloc = flac_malloc;
    callbacks.onRealloc = flac_realloc;
    callbacks.onFree = flac_free;
    drflac *flac = drflac_open(flac_read, flac_seek, flac_tell, &decoder, &callbacks);
    if (!flac)
    {
        // Its one allocation refused: the heap's, or, in a block, more than
        // the sound was made with (the source changed since).
        if (decoder.allocation_failed && !decoder.reader.failed)
        {
            stream_failed(decoder, failure, APRESULT_OUT_OF_MEMORY, STREAM_PART_MEMORY, 0);
            failure.count = decoder.allocation_bytes;
            return false;
        }
        return stream_failed(decoder, failure, APRESULT_CORRUPT_DATA, STREAM_PART_HEADER, 0);
    }
    decoder.state = flac;
    decoder.info.channels = flac->channels;
    decoder.info.rate = flac->sampleRate;
    decoder.info.stated_frames = flac->totalPCMFrameCount;
    if (!room)
        decoder.info.flac_bytes = decoder.arena_used;
    return true;
}

bool open_vorbis(StreamDecoder &decoder, unsigned char *room, StreamFailure &failure)
{
    if (decoder.reader.size > VORBIS_MAX_BYTES)
    {
        stream_failed(decoder, failure, APRESULT_LIMIT, STREAM_PART_LIMIT, 0);
        failure.count = decoder.reader.size;
        return false;
    }
    if (!room)
    {
        room = (unsigned char *)malloc(VORBIS_ARENA_BYTES + VORBIS_READ_BYTES);
        if (!room)
        {
            stream_failed(decoder, failure, APRESULT_OUT_OF_MEMORY, STREAM_PART_MEMORY, 0);
            failure.count = VORBIS_ARENA_BYTES + VORBIS_READ_BYTES;
            return false;
        }
    }
    decoder.vorbis_arena = room;
    decoder.reader.buffer = room + VORBIS_ARENA_BYTES;
    decoder.reader.buffer_capacity = VORBIS_READ_BYTES;
    stb_vorbis_alloc arena;
    arena.alloc_buffer = (char *)room;
    arena.alloc_buffer_length_in_bytes = VORBIS_ARENA_BYTES;
    int open_error = 0;
    stb_vorbis *vorbis = stb_vorbis_open_file_section(&decoder.reader, 0, &open_error, &arena,
                                                      (unsigned int)decoder.reader.size);
    if (!vorbis)
    {
        // VORBIS_outofmem: the arena.
        if (open_error == 3)
            return stream_failed(decoder, failure, APRESULT_LIMIT, STREAM_PART_LIMIT, open_error);
        return stream_failed(decoder, failure, APRESULT_CORRUPT_DATA, STREAM_PART_HEADER, open_error);
    }
    decoder.state = vorbis;
    stb_vorbis_info info = stb_vorbis_get_info(vorbis);
    decoder.info.channels = info.channels > 0 ? (uint32_t)info.channels : 0;
    decoder.info.rate = info.sample_rate;
    // The stated length: the last page's position, which stb_vorbis finds
    // by reading the file's end. Only at create: a stream has it already.
    if (decoder.heap)
        decoder.info.stated_frames = stb_vorbis_stream_length_in_samples(vorbis);
    if (decoder.reader.failed)
        return stream_failed(decoder, failure, APRESULT_IO, STREAM_PART_READ, 0);
    return true;
}

bool open_opus(StreamDecoder &decoder, unsigned char *room, StreamFailure &failure)
{
    OpusFileCallbacks callbacks;
    callbacks.read = opus_read;
    callbacks.seek = opus_seek;
    callbacks.tell = opus_tell;
    callbacks.close = nullptr;
    int open_error = 0;
    OggOpusFile *file = op_open_callbacks(&decoder, &callbacks, nullptr, 0, &open_error);
    if (!file)
        return stream_failed(decoder, failure, open_error == OP_EFAULT ? APRESULT_OUT_OF_MEMORY : APRESULT_CORRUPT_DATA,
                             open_error == OP_EFAULT ? STREAM_PART_MEMORY : STREAM_PART_HEADER, open_error);
    decoder.state = file;
    int channels = op_channel_count(file, -1);
    ogg_int64_t total = op_pcm_total(file, -1);
    // One channel count for every link, as apaudio_opus_decode() refuses a
    // file whose links differ.
    for (int link = 0; link < op_link_count(file); link++)
    {
        if (op_channel_count(file, link) != channels)
            return stream_failed(decoder, failure, APRESULT_UNSUPPORTED, STREAM_PART_FORMAT, 0);
    }
    decoder.info.channels = channels > 0 ? (uint32_t)channels : 0;
    decoder.info.rate = 48000;
    decoder.info.stated_frames = total > 0 ? (uint64_t)total : 0;
    if (!room)
    {
        if (decoder.info.channels == 0 || decoder.info.channels > 255)
            return stream_failed(decoder, failure, APRESULT_CORRUPT_DATA, STREAM_PART_HEADER, 0);
        room = (unsigned char *)malloc((size_t)OPUS_PACKET_FRAMES * decoder.info.channels * sizeof(float));
        if (!room)
        {
            stream_failed(decoder, failure, APRESULT_OUT_OF_MEMORY, STREAM_PART_MEMORY, 0);
            failure.count = (uint64_t)OPUS_PACKET_FRAMES * decoder.info.channels * sizeof(float);
            return false;
        }
    }
    decoder.scratch = (float *)room;
    return true;
}

// Takes up to [frames] of the Opus packet decoded last into [out].
uint32_t take_pending(StreamDecoder &decoder, float *out, uint32_t frames)
{
    uint32_t take = decoder.pending < frames ? decoder.pending : frames;
    uint32_t channels = decoder.info.channels;
    memcpy(out, decoder.scratch + (size_t)decoder.pending_at * channels, (size_t)take * channels * sizeof(float));
    decoder.pending_at += take;
    decoder.pending -= take;
    return take;
}

// Decodes Opus's next packet into the scratch: its frames, 0 at the end, or
// -1 on a failure.
int opus_next_packet(StreamDecoder &decoder, StreamFailure &failure)
{
    OggOpusFile *file = static_cast<OggOpusFile *>(decoder.state);
    for (;;)
    {
        int got = op_read_float(file, decoder.scratch, (int)(OPUS_PACKET_FRAMES * decoder.info.channels), nullptr);
        if (got == OP_HOLE)
            continue; // a gap in the data: opusfile goes on after it, as the whole-file decode does
        if (got < 0)
        {
            stream_failed(decoder, failure, APRESULT_CORRUPT_DATA, STREAM_PART_PACKET, got);
            return -1;
        }
        decoder.pending_at = 0;
        decoder.pending = (uint32_t)got;
        return got;
    }
}

} // namespace

size_t apaudio_stream_decoder_bytes(const StreamInfo &info)
{
    size_t bytes = line_round(sizeof(StreamDecoder));
    switch (info.codec)
    {
    case APAUDIO_STREAM_CODEC_FLAC:
        return bytes + line_round((size_t)info.flac_bytes);
    case APAUDIO_STREAM_CODEC_VORBIS:
        return bytes + line_round(VORBIS_ARENA_BYTES + VORBIS_READ_BYTES);
    default:
        return bytes + line_round((size_t)OPUS_PACKET_FRAMES * info.channels * sizeof(float));
    }
}

bool apaudio_stream_open(StreamDecoder &decoder, APAUDIO_STREAM_CODEC codec, const APAUDIO_SOURCE &source, void *memory,
                         const StreamInfo *info, StreamFailure &failure)
{
    decoder.info.codec = codec;
    decoder.reader.user = source.user;
    decoder.reader.size = source.size;
    decoder.reader.read = source.read;
    decoder.heap = memory == nullptr;
    unsigned char *room = memory ? (unsigned char *)memory + line_round(sizeof(StreamDecoder)) : nullptr;
    bool opened = codec == APAUDIO_STREAM_CODEC_FLAC     ? open_flac(decoder, room, info, failure)
                  : codec == APAUDIO_STREAM_CODEC_VORBIS ? open_vorbis(decoder, room, failure)
                                                         : open_opus(decoder, room, failure);
    if (!opened)
    {
        apaudio_stream_close(decoder);
        return false;
    }
    // A stream reads the source its sound was made from: a header that says
    // otherwise now is bytes that changed under it.
    if (info && (decoder.info.channels != info->channels || decoder.info.rate != info->rate))
    {
        apaudio_stream_close(decoder);
        return stream_failed(decoder, failure, APRESULT_CORRUPT_DATA, STREAM_PART_HEADER, 0);
    }
    return true;
}

uint32_t apaudio_stream_read(StreamDecoder &decoder, float *out, uint32_t frames, StreamFailure &failure)
{
    uint32_t channels = decoder.info.channels;
    uint32_t done = 0;
    switch (decoder.info.codec)
    {
    case APAUDIO_STREAM_CODEC_FLAC:
        done = (uint32_t)drflac_read_pcm_frames_f32(static_cast<drflac *>(decoder.state), frames, out);
        break;
    case APAUDIO_STREAM_CODEC_VORBIS: {
        uint64_t floats = (uint64_t)frames * channels;
        int asked = floats > (uint64_t)INT_MAX ? INT_MAX / (int)channels * (int)channels : (int)floats;
        int got = stb_vorbis_get_samples_float_interleaved(static_cast<stb_vorbis *>(decoder.state), (int)channels, out,
                                                           asked);
        done = got > 0 ? (uint32_t)got : 0;
        break;
    }
    default:
        while (done < frames)
        {
            if (decoder.pending > 0)
            {
                done += take_pending(decoder, out + (size_t)done * channels, frames - done);
                continue;
            }
            int got = opus_next_packet(decoder, failure);
            if (got < 0)
                return 0;
            if (got == 0)
                break;
        }
        break;
    }
    // A read that failed is the source's failure, not the data's end.
    if (decoder.reader.failed)
    {
        stream_failed(decoder, failure, APRESULT_IO, STREAM_PART_READ, 0);
        return 0;
    }
    decoder.position += done;
    return done;
}

bool apaudio_stream_seek(StreamDecoder &decoder, uint64_t frame, StreamFailure &failure)
{
    decoder.pending = 0;
    decoder.pending_at = 0;
    switch (decoder.info.codec)
    {
    case APAUDIO_STREAM_CODEC_FLAC:
        if (!drflac_seek_to_pcm_frame(static_cast<drflac *>(decoder.state), frame))
        {
            decoder.position = frame;
            return stream_failed(decoder, failure, APRESULT_CORRUPT_DATA, STREAM_PART_SEEK, 0);
        }
        break;
    case APAUDIO_STREAM_CODEC_VORBIS: {
        stb_vorbis *vorbis = static_cast<stb_vorbis *>(decoder.state);
        int sought = frame > UINT_MAX ? 0 : frame == 0 ? stb_vorbis_seek_start(vorbis)
                                                       : stb_vorbis_seek(vorbis, (unsigned int)frame);
        if (!sought)
        {
            decoder.position = frame;
            return stream_failed(decoder, failure, APRESULT_CORRUPT_DATA, STREAM_PART_SEEK,
                                 stb_vorbis_get_error(vorbis));
        }
        break;
    }
    default: {
        int sought = op_pcm_seek(static_cast<OggOpusFile *>(decoder.state), (ogg_int64_t)frame);
        if (sought < 0)
        {
            decoder.position = frame;
            return stream_failed(decoder, failure, APRESULT_CORRUPT_DATA, STREAM_PART_SEEK, sought);
        }
        break;
    }
    }
    if (decoder.reader.failed)
        return stream_failed(decoder, failure, APRESULT_IO, STREAM_PART_READ, 0);
    decoder.position = frame;
    return true;
}

void apaudio_stream_close(StreamDecoder &decoder)
{
    switch (decoder.info.codec)
    {
    case APAUDIO_STREAM_CODEC_FLAC:
        if (decoder.state)
            drflac_close(static_cast<drflac *>(decoder.state));
        break;
    case APAUDIO_STREAM_CODEC_VORBIS:
        if (decoder.state)
            stb_vorbis_close(static_cast<stb_vorbis *>(decoder.state));
        if (decoder.heap)
            free(decoder.vorbis_arena);
        decoder.vorbis_arena = nullptr;
        break;
    default:
        if (decoder.state)
            op_free(static_cast<OggOpusFile *>(decoder.state));
        if (decoder.heap)
            free(decoder.scratch);
        decoder.scratch = nullptr;
        break;
    }
    decoder.state = nullptr;
}

const char *apaudio_stream_codec_name(APAUDIO_STREAM_CODEC codec)
{
    return codec == APAUDIO_STREAM_CODEC_FLAC ? "FLAC" : codec == APAUDIO_STREAM_CODEC_VORBIS ? "Vorbis" : "Opus";
}
