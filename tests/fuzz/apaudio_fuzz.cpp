// apaudio_fuzz - ApAudio's decoders over a file held in memory: WAV, FLAC
// (dr_flac), Ogg Vorbis (stb_vorbis) and Ogg Opus (opusfile and libopus).
//
// The input's first byte picks the decoder (its value modulo 4: WAV, FLAC,
// Vorbis, Opus), the rest is the file. Beyond not crashing, it checks:
// - a failure leaves the PCM zeroed and a terminated message;
// - a success has a channel and a rate, samples for every frame it
//   claims (each is read), and frees cleanly.

#include "aprifuzz.hpp"

#include "audio/apaudio.h"

#include <string.h>

namespace
{
// Past this many frames a fuzzed file only spends time and memory: the
// decoders' own limit.
constexpr uint64_t MAX_FRAMES = 1u << 22;
constexpr uint64_t MAX_SAMPLES = 1u << 24;

APRESULT wav(const void *data, uint64_t size, APAUDIO_PCM *pcm, APAUDIO_ERROR *error)
{
    return apaudio_wav_decode(data, size, pcm, error);
}
APRESULT flac(const void *data, uint64_t size, APAUDIO_PCM *pcm, APAUDIO_ERROR *error)
{
    return apaudio_flac_decode(data, size, MAX_FRAMES, pcm, error);
}
APRESULT vorbis(const void *data, uint64_t size, APAUDIO_PCM *pcm, APAUDIO_ERROR *error)
{
    return apaudio_vorbis_decode(data, size, MAX_FRAMES, pcm, error);
}
APRESULT opus(const void *data, uint64_t size, APAUDIO_PCM *pcm, APAUDIO_ERROR *error)
{
    return apaudio_opus_decode(data, size, MAX_FRAMES, pcm, error);
}
typedef APRESULT (*decode_t)(const void *, uint64_t, APAUDIO_PCM *, APAUDIO_ERROR *);
const decode_t decoders[4] = {wav, flac, vorbis, opus};
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 1)
        return 0;
    decode_t decode = decoders[data[0] % 4];
    APAUDIO_PCM pcm;
    memset(&pcm, 0xFF, sizeof(pcm));
    APAUDIO_ERROR error;
    APRESULT result = decode(data + 1, size - 1, &pcm, &error);
    APRIFUZZ_CHECK(memchr(error.message, '\0', sizeof(error.message)) != nullptr);
    if (result != APRESULT_OK)
    {
        APRIFUZZ_CHECK(pcm.samples == nullptr && pcm.frame_count == 0 && pcm.channel_count == 0);
        return 0;
    }
    APRIFUZZ_CHECK(pcm.channel_count > 0);
    APRIFUZZ_CHECK(pcm.sample_rate > 0);
    APRIFUZZ_CHECK(pcm.samples != nullptr);
    // Every sample the PCM claims is there to read (ASan sees one that
    // isn't); what a broken file decodes to is its own business.
    uint64_t samples = pcm.frame_count * pcm.channel_count;
    volatile float sink = 0.0f;
    for (uint64_t i = 0; i < samples && i < MAX_SAMPLES; i++)
        sink = sink + pcm.samples[i];
    (void)sink;
    apaudio_pcm_free(&pcm);
    APRIFUZZ_CHECK(pcm.samples == nullptr);
    return 0;
}
