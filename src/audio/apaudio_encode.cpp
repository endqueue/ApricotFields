#include "audio/apaudio.h"
#include "apaudio_internal.hpp"
#include "../apristructbase.hpp"

#include <math.h>
#include <ogg/ogg.h>
#include <opus.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Resampling samples in memory, and encoding them as Ogg Opus (apaudio.h,
// "Resampling" and "Encoding"; apaudio_encode_draft.h). Nothing here touches
// a mixer or any shared memory: every call may run on any thread, at once
// with any other. Memory comes from malloc, as the decoders' does, so
// apaudio_pcm_free() frees a resample's samples as it does a decode's.

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

// The rates either call takes, inclusive.
constexpr uint32_t RATE_MIN = 8000;
constexpr uint32_t RATE_MAX = 192000;

// The index of the first sample of [samples] that isn't finite, or [count].
size_t first_not_finite(const float *samples, size_t count)
{
    for (size_t i = 0; i < count; i++)
    {
        if (!isfinite(samples[i]))
            return i;
    }
    return count;
}

// The checks both calls make of their samples, after the null check, in
// field order: channels (the caller's own range, [max_channels] its top),
// rate (the caller's), frames, the samples there, every sample finite.
// [channels_unsupported] is the result for more than [max_channels].
APRESULT check_samples(const APAUDIO_PCM *pcm, uint32_t max_channels, bool (*rate_ok)(uint32_t), const char *rates,
                       APAUDIO_ERROR *error)
{
    if (pcm->channel_count == 0)
        return fail(error, APRESULT_INVALID_ARGUMENT, "pcm has no channels");
    if (pcm->channel_count > max_channels)
        return fail(error, APRESULT_UNSUPPORTED, "pcm has %u channels; at most %u are encoded", pcm->channel_count,
                    max_channels);
    if (!rate_ok(pcm->sample_rate))
        return fail(error, APRESULT_INVALID_ARGUMENT, "pcm's sample_rate is %u; %s", pcm->sample_rate, rates);
    if (pcm->frame_count == 0)
        return fail(error, APRESULT_INVALID_ARGUMENT, "pcm has no frames");
    if (!pcm->samples)
        return fail(error, APRESULT_INVALID_ARGUMENT, "pcm's samples are null");
    if (pcm->frame_count > SIZE_MAX / pcm->channel_count)
        return fail(error, APRESULT_LIMIT, "pcm has more samples than memory can hold");
    size_t count = (size_t)pcm->frame_count * pcm->channel_count;
    size_t at = first_not_finite(pcm->samples, count);
    if (at != count)
        return fail(error, APRESULT_INVALID_ARGUMENT, "pcm's sample %llu isn't finite", (unsigned long long)at);
    return APRESULT_OK;
}

bool any_rate(uint32_t rate)
{
    return rate >= RATE_MIN && rate <= RATE_MAX;
}

bool opus_rate(uint32_t rate)
{
    return rate == 8000 || rate == 12000 || rate == 16000 || rate == 24000 || rate == 48000;
}

// Frame [i] of the output read at frame i * from / to of the input: the
// whole frame, and the rest as a fraction of one.
struct place_t
{
    uint64_t frame;
    float fraction;
};

place_t place_of(uint64_t i, uint32_t from, uint32_t to)
{
    uint64_t at = i * from;
    return place_t{at / to, (float)((double)(at % to) / (double)to)};
}

// The mixer's linear interpolation (apaudio.cpp, render_source), over any
// number of channels, frames past the last silent.
void resample_linear(const APAUDIO_PCM &in, uint32_t to, float *out, uint64_t out_frames)
{
    uint32_t channels = in.channel_count;
    for (uint64_t i = 0; i < out_frames; i++)
    {
        place_t p = place_of(i, in.sample_rate, to);
        bool silent_after = p.frame + 1 >= in.frame_count;
        for (uint32_t c = 0; c < channels; c++)
        {
            float a = in.samples[p.frame * channels + c];
            float b = silent_after ? 0.0f : in.samples[(p.frame + 1) * channels + c];
            out[i * channels + c] = a + (b - a) * p.fraction;
        }
    }
}

// The mixer's windowed sinc (apaudio.cpp, sinc_frame): the same table,
// taps, weights and stretch, over any number of channels, frames outside
// the samples silent. [out] starts zeroed.
void resample_sinc(const APAUDIO_PCM &in, uint32_t to, const float *table, float *out, uint64_t out_frames)
{
    uint32_t channels = in.channel_count;
    float stretch = in.sample_rate > to ? (float)in.sample_rate / (float)to : 1.0f;
    int half = (int)ceilf((float)APAUDIO_SINC_ZEROS * stretch);
    float to_table = (float)APAUDIO_SINC_RESOLUTION / stretch;
    const float limit = (float)(APAUDIO_SINC_ZEROS * APAUDIO_SINC_RESOLUTION);
    int64_t count = (int64_t)in.frame_count;
    for (uint64_t i = 0; i < out_frames; i++)
    {
        place_t p = place_of(i, in.sample_rate, to);
        float *frame_out = out + i * channels;
        for (int k = -half + 1; k <= half; k++)
        {
            float at = fabsf((float)k - p.fraction) * to_table;
            if (at >= limit)
                continue;
            int entry = (int)at;
            float weight = table[entry] + (table[entry + 1] - table[entry]) * (at - (float)entry);
            int64_t frame = (int64_t)p.frame + k;
            if (frame < 0 || frame >= count)
                continue;
            const float *frame_in = in.samples + frame * channels;
            for (uint32_t c = 0; c < channels; c++)
                frame_out[c] += weight * frame_in[c];
        }
        for (uint32_t c = 0; c < channels; c++)
            frame_out[c] /= stretch;
    }
}

// ---- Ogg Opus ---------------------------------------------------------------

// An Opus frame of 20 ms, the length every packet here has.
constexpr uint32_t FRAME_MS = 20;
// The largest packet libopus writes for one frame is 1,275 bytes; room for
// more costs nothing and is what its documentation asks for.
constexpr int32_t MAX_PACKET_BYTES = 4000;

// The file's bytes, in room that doubles as pages come.
struct bytes_t
{
    unsigned char *data{nullptr};
    size_t size{0};
    size_t capacity{0};
};

bool append(bytes_t &bytes, const unsigned char *data, long count)
{
    size_t need = bytes.size + (size_t)count;
    if (need > bytes.capacity)
    {
        size_t larger = bytes.capacity ? bytes.capacity : 4096;
        while (larger < need)
            larger *= 2;
        unsigned char *grown = (unsigned char *)realloc(bytes.data, larger);
        if (!grown)
            return false;
        bytes.data = grown;
        bytes.capacity = larger;
    }
    memcpy(bytes.data + bytes.size, data, (size_t)count);
    bytes.size = need;
    return true;
}

bool append_page(bytes_t &bytes, const ogg_page &page)
{
    return append(bytes, page.header, page.header_len) && append(bytes, page.body, page.body_len);
}

// Every page the stream holds, ended or not ([flush]) at its last packet.
bool drain(ogg_stream_state &stream, bytes_t &bytes, bool flush)
{
    ogg_page page;
    while (flush ? ogg_stream_flush(&stream, &page) : ogg_stream_pageout(&stream, &page))
    {
        if (!append_page(bytes, page))
            return false;
    }
    return true;
}

void put_u16(unsigned char *at, uint32_t value)
{
    at[0] = (unsigned char)(value & 0xFF);
    at[1] = (unsigned char)((value >> 8) & 0xFF);
}

void put_u32(unsigned char *at, uint32_t value)
{
    put_u16(at, value & 0xFFFF);
    put_u16(at + 2, value >> 16);
}

int application_of(APAUDIO_OPUS_APPLICATION application)
{
    switch (application)
    {
    case APAUDIO_OPUS_APPLICATION_VOIP:
        return OPUS_APPLICATION_VOIP;
    case APAUDIO_OPUS_APPLICATION_RESTRICTED_LOWDELAY:
        return OPUS_APPLICATION_RESTRICTED_LOWDELAY;
    default:
        return OPUS_APPLICATION_AUDIO;
    }
}

// What an encode holds, let go of on every path.
struct encode_t
{
    OpusEncoder *encoder{nullptr};
    ogg_stream_state stream{};
    bool stream_made{false};
    bytes_t bytes{};

    ~encode_t()
    {
        free(encoder);
        if (stream_made)
            ogg_stream_clear(&stream);
        free(bytes.data);
    }
};

} // namespace

extern "C"
{

APRESULT apaudio_pcm_resample(const APAUDIO_PCM *pcm, uint32_t sample_rate, APAUDIO_RESAMPLER resampler,
                              APAUDIO_PCM *out_pcm, APAUDIO_ERROR *error)
{
    clear(error);
    if (out_pcm)
        *out_pcm = APAUDIO_PCM{};
    if (!pcm)
        return fail(error, APRESULT_INVALID_ARGUMENT, "pcm is null");
    if (APRESULT checked = check_samples(pcm, UINT32_MAX, any_rate, "the rates taken are 8,000 to 192,000", error))
        return checked;
    if (sample_rate < RATE_MIN)
        return fail(error, APRESULT_INVALID_ARGUMENT, "sample_rate is %u, under 8,000", sample_rate);
    if (sample_rate > RATE_MAX)
        return fail(error, APRESULT_INVALID_ARGUMENT, "sample_rate is %u, past 192,000", sample_rate);
    if (resampler != APAUDIO_RESAMPLER_LINEAR && resampler != APAUDIO_RESAMPLER_SINC)
        return fail(error, APRESULT_INVALID_ARGUMENT, "resampler is %u, neither LINEAR nor SINC", resampler);
    if (!out_pcm)
        return fail(error, APRESULT_INVALID_ARGUMENT, "out_pcm is null");

    // As many frames as the input's times the ratio, rounded to nearest, at
    // least one.
    uint64_t frames = pcm->frame_count;
    if (frames > (UINT64_MAX - pcm->sample_rate) / sample_rate)
        return fail(error, APRESULT_LIMIT, "pcm has too many frames to resample");
    uint64_t out_frames = (frames * sample_rate + pcm->sample_rate / 2) / pcm->sample_rate;
    if (out_frames == 0)
        out_frames = 1;
    if (out_frames >= (UINT64_C(1) << 32))
        return fail(error, APRESULT_LIMIT, "resampled, pcm would have %llu frames, past 2^32",
                    (unsigned long long)out_frames);
    uint32_t channels = pcm->channel_count;
    if (out_frames > SIZE_MAX / sizeof(float) / channels)
        return fail(error, APRESULT_LIMIT, "resampled, pcm would have more samples than memory can hold");
    float *samples = (float *)calloc((size_t)out_frames * channels, sizeof(float));
    if (!samples)
        return fail(error, APRESULT_OUT_OF_MEMORY, "no memory for %llu frames", (unsigned long long)out_frames);

    if (sample_rate == pcm->sample_rate)
        memcpy(samples, pcm->samples, (size_t)out_frames * channels * sizeof(float));
    else if (resampler == APAUDIO_RESAMPLER_LINEAR)
        resample_linear(*pcm, sample_rate, samples, out_frames);
    else
    {
        float *table = apaudio_sinc_table_build();
        if (!table)
        {
            free(samples);
            return fail(error, APRESULT_OUT_OF_MEMORY, "no memory for the sinc resampler's table");
        }
        resample_sinc(*pcm, sample_rate, table, samples, out_frames);
        free(table);
    }

    out_pcm->samples = samples;
    out_pcm->frame_count = out_frames;
    out_pcm->channel_count = channels;
    out_pcm->sample_rate = sample_rate;
    return APRESULT_OK;
}

APRESULT apaudio_opus_encode(const APAUDIO_PCM *pcm, const APAUDIO_OPUS_ENCODE_DESC *desc, APAUDIO_ENCODED *out_encoded,
                             APAUDIO_ERROR *error)
{
    clear(error);
    if (out_encoded)
        *out_encoded = APAUDIO_ENCODED{};
    if (!pcm)
        return fail(error, APRESULT_INVALID_ARGUMENT, "pcm is null");
    if (APRESULT checked = check_samples(pcm, 2, opus_rate,
                                         "Opus codes at 8,000, 12,000, 16,000, 24,000 or 48,000: resample it first "
                                         "with apaudio_pcm_resample()",
                                         error))
        return checked;
    if (!desc)
        return fail(error, APRESULT_INVALID_ARGUMENT, "desc is null");
    if (APRESULT result = apri_struct_check(&desc->base, APRI_STRUCT_TYPE_APAUDIO_OPUS_ENCODE_DESC))
        return fail(error, result, "desc %s", apri_struct_check_message(result));
    if (desc->bitrate < 6000)
        return fail(error, APRESULT_INVALID_ARGUMENT, "desc's bitrate is %u, under 6,000", desc->bitrate);
    if (desc->bitrate > 510000)
        return fail(error, APRESULT_INVALID_ARGUMENT, "desc's bitrate is %u, past 510,000", desc->bitrate);
    if (desc->complexity > 10)
        return fail(error, APRESULT_INVALID_ARGUMENT, "desc's complexity is %u, past 10", desc->complexity);
    if (desc->application != APAUDIO_OPUS_APPLICATION_AUDIO && desc->application != APAUDIO_OPUS_APPLICATION_VOIP &&
        desc->application != APAUDIO_OPUS_APPLICATION_RESTRICTED_LOWDELAY)
        return fail(error, APRESULT_INVALID_ARGUMENT, "desc's application is %u, not one of APAUDIO_OPUS_APPLICATION's",
                    desc->application);
    if (desc->original_sample_rate == 0)
        return fail(error, APRESULT_INVALID_ARGUMENT, "desc's original_sample_rate is 0");
    if (!out_encoded)
        return fail(error, APRESULT_INVALID_ARGUMENT, "out_encoded is null");

    uint32_t channels = pcm->channel_count;
    uint32_t rate = pcm->sample_rate;
    uint32_t scale = 48000 / rate; // Ogg Opus counts at 48 kHz
    uint64_t frames = pcm->frame_count;
    // The granule positions are signed 64-bit counts at 48 kHz, the
    // lookahead and one frame on top.
    if (frames > (uint64_t)(INT64_MAX / 48000) * rate / 2)
        return fail(error, APRESULT_LIMIT, "pcm is too long for Ogg's granule positions");

    encode_t encode;
    encode.encoder = (OpusEncoder *)malloc((size_t)opus_encoder_get_size((int)channels));
    if (!encode.encoder)
        return fail(error, APRESULT_OUT_OF_MEMORY, "no memory for the Opus encoder");
    int made = opus_encoder_init(encode.encoder, (opus_int32)rate, (int)channels, application_of(desc->application));
    if (made != OPUS_OK)
        return fail(error, APRESULT_INVALID_ARGUMENT, "libopus refused the encoder's settings (its error %d)", made);
    opus_encoder_ctl(encode.encoder, OPUS_SET_BITRATE((opus_int32)desc->bitrate));
    opus_encoder_ctl(encode.encoder, OPUS_SET_VBR(desc->vbr ? 1 : 0));
    opus_encoder_ctl(encode.encoder, OPUS_SET_COMPLEXITY((opus_int32)desc->complexity));
    opus_int32 lookahead = 0;
    opus_encoder_ctl(encode.encoder, OPUS_GET_LOOKAHEAD(&lookahead));

    if (ogg_stream_init(&encode.stream, (int)desc->ogg_serial) != 0)
        return fail(error, APRESULT_OUT_OF_MEMORY, "no memory for the Ogg stream");
    encode.stream_made = true;
    // A first size of the sound's length at the bit rate, and the pages'
    // overhead.
    uint64_t seconds = frames / rate + 1;
    uint64_t guess = seconds * (desc->bitrate / 8) * 5 / 4 + 4096;
    encode.bytes.capacity = guess < (64u << 20) ? (size_t)guess : (64u << 20);
    encode.bytes.data = (unsigned char *)malloc(encode.bytes.capacity);
    if (!encode.bytes.data)
        return fail(error, APRESULT_OUT_OF_MEMORY, "no memory for the encoded file");

    // OpusHead (RFC 7845, 5.1), alone on the first page.
    uint32_t pre_skip = (uint32_t)lookahead * scale;
    unsigned char head[19];
    memcpy(head, "OpusHead", 8);
    head[8] = 1;
    head[9] = (unsigned char)channels;
    put_u16(head + 10, pre_skip);
    put_u32(head + 12, desc->original_sample_rate);
    put_u16(head + 16, 0); // output gain
    head[18] = 0;          // channel mapping family 0
    ogg_packet packet{};
    packet.packet = head;
    packet.bytes = sizeof(head);
    packet.b_o_s = 1;
    packet.packetno = 0;
    if (ogg_stream_packetin(&encode.stream, &packet) != 0 || !drain(encode.stream, encode.bytes, true))
        return fail(error, APRESULT_OUT_OF_MEMORY, "no memory for the Ogg pages");

    // OpusTags (5.2), on a page of its own.
    static const char vendor[] = "ApAudio";
    unsigned char tags[8 + 4 + sizeof(vendor) - 1 + 4];
    memcpy(tags, "OpusTags", 8);
    put_u32(tags + 8, (uint32_t)(sizeof(vendor) - 1));
    memcpy(tags + 12, vendor, sizeof(vendor) - 1);
    put_u32(tags + 12 + sizeof(vendor) - 1, 0);
    packet = ogg_packet{};
    packet.packet = tags;
    packet.bytes = sizeof(tags);
    packet.packetno = 1;
    if (ogg_stream_packetin(&encode.stream, &packet) != 0 || !drain(encode.stream, encode.bytes, true))
        return fail(error, APRESULT_OUT_OF_MEMORY, "no memory for the Ogg pages");

    // The audio: enough frames that the encoder's lookahead is flushed, the
    // last padded with silence; the last packet's granule position trims
    // the decoded length to the input's.
    uint32_t frame_size = rate * FRAME_MS / 1000;
    uint64_t total = frames + (uint64_t)lookahead;
    uint64_t packets = (total + frame_size - 1) / frame_size;
    int64_t end_granule = (int64_t)pre_skip + (int64_t)(frames * scale);
    float input[48000 * FRAME_MS / 1000 * 2];
    unsigned char output[MAX_PACKET_BYTES];
    for (uint64_t k = 0; k < packets; k++)
    {
        uint64_t first = k * frame_size;
        for (uint32_t i = 0; i < frame_size; i++)
        {
            uint64_t frame = first + i;
            for (uint32_t c = 0; c < channels; c++)
                input[i * channels + c] = frame < frames ? pcm->samples[frame * channels + c] : 0.0f;
        }
        opus_int32 written = opus_encode_float(encode.encoder, input, (int)frame_size, output, MAX_PACKET_BYTES);
        if (written < 0)
            return fail(error, APRESULT_INVALID_ARGUMENT, "libopus couldn't encode frame %llu (its error %d)",
                        (unsigned long long)k, (int)written);
        bool last = k + 1 == packets;
        packet = ogg_packet{};
        packet.packet = output;
        packet.bytes = written;
        packet.e_o_s = last ? 1 : 0;
        packet.granulepos = last ? end_granule : (int64_t)((k + 1) * frame_size * scale);
        packet.packetno = (ogg_int64_t)(2 + k);
        if (ogg_stream_packetin(&encode.stream, &packet) != 0 || !drain(encode.stream, encode.bytes, last))
            return fail(error, APRESULT_OUT_OF_MEMORY, "no memory for the Ogg pages");
    }

    out_encoded->data = encode.bytes.data;
    out_encoded->size = encode.bytes.size;
    encode.bytes = bytes_t{};
    return APRESULT_OK;
}

void apaudio_encoded_free(APAUDIO_ENCODED *encoded)
{
    if (!encoded)
        return;
    free(encoded->data);
    *encoded = APAUDIO_ENCODED{};
}
}
