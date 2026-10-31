#ifndef APAUDIO_STREAM_HPP
#define APAUDIO_STREAM_HPP

// Internal header - never included outside ApricotFields/src/audio/.
//
// The streaming half of ApAudio's decoders (apaudio.h, "Streamed sounds"):
// FLAC, Ogg Vorbis and Ogg Opus opened over a caller's APAUDIO_SOURCE and
// decoded a block at a time into the caller's floats. apaudio.cpp owns the
// streams, their rings and when they decode; this reads the bytes, in
// apaudio_decode.cpp beside the whole-file decoders, so both decode with the
// same code and give the same bits.

#include "audio/apaudio.h"
#include "apaudio_stream_reader.h"
#include "../apriplaindata.hpp"
#include <stddef.h>
#include <stdint.h>

// What part of a stream failed, for the message the update makes.
enum : uint32_t
{
    STREAM_PART_NONE,
    STREAM_PART_HEADER, // the header isn't the codec's: [codec_error]
    STREAM_PART_READ,   // the source's read failed: [count] bytes at [offset]
    STREAM_PART_PACKET, // a packet at frame [frame] couldn't be decoded: [codec_error]
    STREAM_PART_SHORT,  // the data ended at frame [frame], before the [count] the header states
    STREAM_PART_SEEK,   // seeking to frame [frame] failed: [codec_error]
    STREAM_PART_MEMORY, // no memory for [count] bytes
    STREAM_PART_FORMAT, // [count] channels at [offset] frames a second, which the mixer doesn't play
    STREAM_PART_LENGTH, // the file states no length
    STREAM_PART_LIMIT,  // a Vorbis source of [count] bytes, past the decoder's 2 GiB
};

// A stream's failure: what the update's message says.
struct StreamFailure
{
    APRESULT result;
    uint32_t part;
    int32_t codec_error;
    uint64_t offset;
    uint64_t count;
    uint64_t frame;
};
APRI_PLAIN_DATA(StreamFailure);

// What a source's header says, and the room its decoder needs.
struct StreamInfo
{
    APAUDIO_STREAM_CODEC codec;
    uint32_t channels;
    uint32_t rate;
    uint64_t stated_frames; // the length the file states
    uint64_t flac_bytes;    // dr_flac's one allocation, measured when the sound was made
};
APRI_PLAIN_DATA(StreamInfo);

// A codec's decoder over a reader: at the start of a stream's block, or on
// the system heap while a sound is made.
struct StreamDecoder
{
    StreamInfo info;
    apaudio_stream_reader reader;
    // drflac *, stb_vorbis * or OggOpusFile *.
    void *state;
    // dr_flac's allocations, bumped through [arena] in the block; with none,
    // from the system heap, their bytes summed into [arena_used].
    unsigned char *arena;
    size_t arena_size;
    size_t arena_used;
    // An allocation dr_flac asked for and didn't get, and its size.
    bool allocation_failed;
    size_t allocation_bytes;
    // stb_vorbis's state (VORBIS_ARENA_BYTES) and read buffer.
    unsigned char *vorbis_arena;
    // Opus's packets, each decoded whole into [scratch] (OPUS_PACKET_FRAMES
    // frames); the [pending] frames from [pending_at] not yet taken.
    float *scratch;
    uint32_t pending_at;
    uint32_t pending;
    // The sound frame decoded next.
    uint64_t position;
    // Made on the system heap (a sound's create): close frees what open
    // took.
    bool heap;
};

// The bytes a stream's block needs before its ring, for a sound of [info]:
// the decoder and its codec's own room, each at a cache line.
size_t apaudio_stream_decoder_bytes(const StreamInfo &info);

// Opens [decoder] over [source] with [codec]: in [memory], which holds
// apaudio_stream_decoder_bytes([info]) for the [info] the sound was made
// with, or with [memory] NULL on the system heap, filling decoder.info from
// the header. False with [failure] filled.
bool apaudio_stream_open(StreamDecoder &decoder, APAUDIO_STREAM_CODEC codec, const APAUDIO_SOURCE &source, void *memory,
                         const StreamInfo *info, StreamFailure &failure);

// Decodes up to [frames] frames into [out] (interleaved, info.channels of
// them): how many; 0 at the data's end, or on a failure, with
// [failure].result set.
uint32_t apaudio_stream_read(StreamDecoder &decoder, float *out, uint32_t frames, StreamFailure &failure);

// Moves to sound frame [frame], by the positions the file states. In a
// well-formed file FLAC and Vorbis land exactly, bits included, and Opus
// exactly in place, its decoder's state after a seek not the continuous
// decode's (the workspace's DECISIONS.md, card A34); in a broken one the
// stated positions can disagree with the frames decoded in order. Frame 0
// starts the decoder over: at the first frame (FLAC), the first audio page
// (Vorbis), the first link's start (Opus).
bool apaudio_stream_seek(StreamDecoder &decoder, uint64_t frame, StreamFailure &failure);

// Closes [decoder], freeing what a heap open took. Its block is the caller's.
void apaudio_stream_close(StreamDecoder &decoder);

// "FLAC", "Vorbis" or "Opus", for messages.
const char *apaudio_stream_codec_name(APAUDIO_STREAM_CODEC codec);

#endif // APAUDIO_STREAM_HPP
