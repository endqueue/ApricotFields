// No allocation per frame (ROADMAP.md, section 3.1, "Data-oriented
// checks"): each module's steady state, after a warm-up, counted for 100
// frames with every heap allocation the measuring thread makes, through
// tests/alloctrace.h, which says what each platform's hook sees and how
// each allocation is put down to the code that asked for it: only ours
// (this executable's) must be none. Where no hook is available the test
// says so and passes nothing off as checked.
//
// A frame, per module: Rabbittus Audio's render (the audio callback) and update
// with sounds playing and the limiter on; Rabbittus Math's ray casts against a
// mesh's BVH; Rabbittus Jobs' reference pool running a task; Rabbittus Text laying the
// same label out again with a layout it keeps. Rend's frame needs a GPU:
// ../rabbittustests measures it.
//
// Written in C, as the other tests of C headers are. Takes Rabbittus Text's data
// folder as its argument, and Rabbittus Audio's as its second.

#include "audio/rbtaudio.h"
#include "jobs/rbtjobs.h"
#include "math/rbtmathbvh.h"
#include "text/rbttext.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(condition)                                                                                              \
    do                                                                                                                \
    {                                                                                                                 \
        if (!(condition))                                                                                             \
        {                                                                                                             \
            printf("FAIL (%s:%d): %s\n", __FILE__, __LINE__, #condition);                                            \
            failures++;                                                                                               \
        }                                                                                                             \
    } while (0)

// ---- Counting ----------------------------------------------------------------

#include "alloctrace.h"

static void start_counting(void)
{
    alloc_trace_start();
}
static void stop_counting(void)
{
    alloc_trace_stop();
}

#define FRAMES 100

// The hook counts at all: one malloc and one calloc of the measuring thread,
// counted as two of ours. A zero everywhere else means nothing if this is
// zero too.
static void test_hook_counts(void)
{
    start_counting();
    void *volatile a = malloc(24);
    void *volatile b = calloc(3, 8);
    stop_counting();
    free(a);
    free(b);
    printf("rbtnoalloc_tests: the hook counted %lu of 2 known allocations of ours\n", alloc_trace_ours());
    CHECK(alloc_trace_ours() == 2);
}

// Ours must be none; what other modules asked for is reported, not
// counted.
static void report(const char *what)
{
    char line[256];
    snprintf(line, sizeof(line), "rbtnoalloc_tests: %s, %d frames", what, FRAMES);
    alloc_trace_report(line, 4);
    if (alloc_trace_ours() != 0)
        failures++;
}

// ---- Rabbittus Audio ---------------------------------------------------------------------

static const CRAUDIO_CHANNEL_POSITION stereo[2] = {CRAUDIO_CHANNEL_POSITION_FL, CRAUDIO_CHANNEL_POSITION_FR};
static float tone[4800];
static float out[2 * 512];

static void test_audio(void)
{
    RBTAUDIO_MIXER_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_AUDIO_MIXER_DESC;
    desc.format.sample_format = CRAUDIO_SAMPLE_FORMAT_F32;
    desc.format.sample_rate = 48000;
    desc.format.channel_count = 2;
    desc.format.interleaved = true;
    desc.format.positions = stereo;
    desc.max_frames = 512;
    desc.max_voices = 64;
    desc.max_real_voices = 16;
    desc.max_fading_voices = 4;
    desc.transition_fade_us = 10000;
    desc.max_sounds = 8;
    desc.max_concurrency_groups = 4;
    desc.max_buses = 4;
    desc.max_modifiers = 4;
    desc.queue_capacity = 64;
    desc.resampler = RBTAUDIO_RESAMPLER_LINEAR;
    desc.limiter = RBTAUDIO_LIMITER_ON;
    desc.limiter_lookahead_us = 1500;
    desc.limiter_release_us = 80000;
    desc.limiter_ceiling = 0.97f;
    desc.gain = 1.0f;
    rbtaudio_mixer mixer = NULL;
    CHECK(rbtaudio_mixer_create(&desc, &mixer, NULL) == RBT_RESULT_OK);
    if (!mixer)
        return;
    for (size_t i = 0; i < sizeof(tone) / sizeof(tone[0]); i++)
        tone[i] = (float)((i % 96) < 48 ? 0.5 : -0.5);
    RBTAUDIO_SOUND_DESC sd;
    memset(&sd, 0, sizeof(sd));
    sd.base.type = RBT_STRUCT_TYPE_AUDIO_SOUND_DESC;
    sd.samples = tone;
    sd.frame_count = sizeof(tone) / sizeof(tone[0]);
    sd.channel_count = 1;
    sd.sample_rate = 44100; // resampled
    sd.virtual_behavior = RBTAUDIO_VIRTUAL_KILL;
    RBTAUDIO_SOUND sound = 0;
    CHECK(rbtaudio_sound_create(mixer, &sd, &sound, NULL) == RBT_RESULT_OK);
    RBTAUDIO_PLAY_DESC play;
    memset(&play, 0, sizeof(play));
    play.base.type = RBT_STRUCT_TYPE_AUDIO_PLAY_DESC;
    play.gain = 1.0f;
    play.rate = 1.0f;
    play.placement = RBTAUDIO_PLACEMENT_ANGLES;
    play.virtual_behavior = RBTAUDIO_VIRTUAL_KILL;
    play.priority = 128;
    play.loop = true;
    for (int i = 0; i < 3; i++)
    {
        RBTAUDIO_VOICE voice = 0;
        CHECK(rbtaudio_play(mixer, sound, &play, &voice) == RBT_RESULT_OK);
    }
    // The warm-up: the queue drained, the voices started.
    for (int f = 0; f < 4; f++)
    {
        CHECK(rbtaudio_mixer_update(mixer, NULL) == RBT_RESULT_OK);
        rbtaudio_mixer_render(mixer, out, 512, NULL);
    }
    start_counting();
    for (int f = 0; f < FRAMES; f++)
    {
        CHECK(rbtaudio_mixer_update(mixer, NULL) == RBT_RESULT_OK);
        rbtaudio_mixer_render(mixer, out, 512, NULL);
    }
    stop_counting();
    report("Rabbittus Audio's update and render, three looping voices resampled, the limiter on");
    rbtaudio_mixer_destroy(mixer);
}

// Rabbittus Audio's streamed voices (RBTAUDIO_TODO.md item 15): a voice of a
// streamed sound playing, looped, past its prefetch, update, render and
// collect; with no scheduler, so the decoding runs inside the update on this
// thread, and on a pool of four (this thread counted as it waits). The
// stream's start - its one allocation, its decoder's open and first fill -
// is counted apart and reported, not checked. Pending while the mixer
// refuses streams (RBT_RESULT_NOT_IMPLEMENTED).

static bool read_file(const char *dir, const char *name, unsigned char **out, uint64_t *out_size)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir ? dir : ".", name);
    FILE *file = fopen(path, "rb");
    CHECK(file != NULL);
    if (!file)
        return false;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    *out = (unsigned char *)malloc(size > 0 ? (size_t)size : 1);
    bool read = *out && size > 0 && fread(*out, 1, (size_t)size, file) == (size_t)size;
    fclose(file);
    CHECK(read);
    *out_size = (uint64_t)size;
    return read;
}

typedef struct memory_bytes
{
    const unsigned char *bytes;
    uint64_t size;
} memory_bytes;

static bool memory_read(void *user, uint64_t offset, void *buffer, uint64_t count)
{
    const memory_bytes *memory = (const memory_bytes *)user;
    if (offset > memory->size || count > memory->size - offset)
        return false;
    memcpy(buffer, memory->bytes + offset, (size_t)count);
    return true;
}

static void stream_scenario(const char *dir, const char *name, RBTAUDIO_STREAM_CODEC codec, const char *codec_name,
                            const RBTJOBS_SCHEDULER *scheduler)
{
    unsigned char *bytes = NULL;
    uint64_t size = 0;
    if (!read_file(dir, name, &bytes, &size))
    {
        free(bytes);
        return;
    }
    memory_bytes memory = {bytes, size};
    RBTAUDIO_MIXER_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_AUDIO_MIXER_DESC;
    desc.format.sample_format = CRAUDIO_SAMPLE_FORMAT_F32;
    desc.format.sample_rate = 48000;
    desc.format.channel_count = 2;
    desc.format.interleaved = true;
    desc.format.positions = stereo;
    desc.max_frames = 512;
    desc.max_voices = 16;
    desc.max_real_voices = 4;
    desc.max_fading_voices = 2;
    desc.transition_fade_us = 10000;
    desc.max_sounds = 4;
    desc.queue_capacity = 16;
    desc.resampler = RBTAUDIO_RESAMPLER_SINC;
    desc.limiter = RBTAUDIO_LIMITER_OFF;
    desc.gain = 1.0f;
    desc.max_streams = 2;
    desc.scheduler = scheduler;
    rbtaudio_mixer mixer = NULL;
    RBT_RESULT result = rbtaudio_mixer_create(&desc, &mixer, NULL);
    if (result == RBT_RESULT_NOT_IMPLEMENTED)
    {
        printf("rbtnoalloc_tests: Rabbittus Audio's streamed %s voice: pending (not implemented yet)\n", codec_name);
        free(bytes);
        return;
    }
    CHECK(result == RBT_RESULT_OK);
    RBTAUDIO_SOUND_DESC sd;
    memset(&sd, 0, sizeof(sd));
    sd.base.type = RBT_STRUCT_TYPE_AUDIO_SOUND_DESC;
    sd.virtual_behavior = RBTAUDIO_VIRTUAL_CONTINUE;
    sd.stream_codec = codec;
    sd.stream_source.user = &memory;
    sd.stream_source.size = size;
    sd.stream_source.read = memory_read;
    sd.stream_ring_us = 100000;
    RBTAUDIO_SOUND sound = 0;
    CHECK(mixer && rbtaudio_sound_create(mixer, &sd, &sound, NULL) == RBT_RESULT_OK);
    if (sound)
    {
        RBTAUDIO_PLAY_DESC play;
        memset(&play, 0, sizeof(play));
        play.base.type = RBT_STRUCT_TYPE_AUDIO_PLAY_DESC;
        play.gain = 1.0f;
        play.rate = 1.0f;
        play.placement = RBTAUDIO_PLACEMENT_ANGLES;
        play.priority = 128;
        play.loop = true;
        RBTAUDIO_VOICE voice = 0;
        CHECK(rbtaudio_play(mixer, sound, &play, &voice) == RBT_RESULT_OK);
        // The start, apart: the first render (the prefetch), and the update
        // that opens the stream and fills its ring.
        start_counting();
        rbtaudio_mixer_render(mixer, out, 512, NULL);
        CHECK(rbtaudio_mixer_update(mixer, NULL) == RBT_RESULT_OK);
        stop_counting();
        printf("rbtnoalloc_tests: Rabbittus Audio's streamed %s voice's start%s: %lu allocation(s) of ours (the stream's "
               "block%s), not checked\n",
               codec_name, scheduler ? " on a pool" : "", alloc_trace_ours(),
               codec == RBTAUDIO_STREAM_CODEC_OPUS ? ", and opusfile's and libopus's own" : "");
        // The warm-up: past the prefetch, the ring wrapping.
        for (int f = 0; f < 8; f++)
        {
            rbtaudio_mixer_render(mixer, out, 512, NULL);
            CHECK(rbtaudio_mixer_update(mixer, NULL) == RBT_RESULT_OK);
            rbtaudio_mixer_collect(mixer);
        }
        start_counting();
        for (int f = 0; f < FRAMES; f++)
        {
            rbtaudio_mixer_render(mixer, out, 512, NULL);
            CHECK(rbtaudio_mixer_update(mixer, NULL) == RBT_RESULT_OK);
            rbtaudio_mixer_collect(mixer);
        }
        stop_counting();
        char what[160];
        snprintf(what, sizeof(what), "Rabbittus Audio's streamed %s voice, update, render and collect%s", codec_name,
                 scheduler ? ", decoding on a pool of four" : ", decoding in the update");
        report(what);
        CHECK(rbtaudio_sound_get_underrun_frames(mixer, sound) == 0);
    }
    rbtaudio_mixer_destroy(mixer);
    free(bytes);
}

static void test_audio_streams(const char *dir)
{
    static const char *const names[3] = {"stream.flac", "stream.ogg", "stream.opus"};
    static const RBTAUDIO_STREAM_CODEC codecs[3] = {RBTAUDIO_STREAM_CODEC_FLAC, RBTAUDIO_STREAM_CODEC_VORBIS,
                                                   RBTAUDIO_STREAM_CODEC_OPUS};
    static const char *const codec_names[3] = {"FLAC", "Ogg Vorbis", "Ogg Opus"};
    RBTJOBS_POOL_DESC pd;
    memset(&pd, 0, sizeof(pd));
    pd.base.type = RBT_STRUCT_TYPE_JOBS_POOL_DESC;
    pd.thread_count = 3;
    pd.max_tasks = 4;
    rbtjobs_pool pool = NULL;
    CHECK(rbtjobs_pool_create(&pd, &pool, NULL) == RBT_RESULT_OK);
    RBTJOBS_SCHEDULER scheduler;
    memset(&scheduler, 0, sizeof(scheduler));
    scheduler.base.type = RBT_STRUCT_TYPE_JOBS_SCHEDULER;
    if (pool)
        CHECK(rbtjobs_pool_get_scheduler(pool, &scheduler) == RBT_RESULT_OK);
    for (int c = 0; c < 3; c++)
    {
        stream_scenario(dir, names[c], codecs[c], codec_names[c], NULL);
        if (pool)
            stream_scenario(dir, names[c], codecs[c], codec_names[c], &scheduler);
    }
    rbtjobs_pool_destroy(pool);
}

// ---- Rabbittus Math ----------------------------------------------------------------------

// Rabbittus Audio's spatial audio (RBTAUDIO_TODO.md item 16): 256 world-placed
// voices, every one moved each frame (rbtaudio_mixer_set_emitters()), two
// listeners set each frame, then the update and the render; with no
// scheduler, and on a pool of four (the mixer's scheduler, read only for
// streams, so nothing differs but that it is given one).

static void spatial_scenario(const RBTJOBS_SCHEDULER *scheduler)
{
    enum
    {
        VOICES = 256
    };
    RBTAUDIO_SPATIAL_DESC spatial;
    memset(&spatial, 0, sizeof(spatial));
    spatial.base.type = RBT_STRUCT_TYPE_AUDIO_SPATIAL_DESC;
    spatial.handedness = RBTAUDIO_HANDEDNESS_RIGHT;
    spatial.max_listeners = 2;
    spatial.max_attenuations = 2;
    spatial.speed_of_sound = 343.0f;
    RBTAUDIO_MIXER_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_AUDIO_MIXER_DESC;
    desc.base.next = &spatial.base;
    desc.format.sample_format = CRAUDIO_SAMPLE_FORMAT_F32;
    desc.format.sample_rate = 48000;
    desc.format.channel_count = 2;
    desc.format.interleaved = true;
    desc.format.positions = stereo;
    desc.max_frames = 512;
    desc.max_voices = VOICES;
    desc.max_real_voices = 64;
    desc.max_fading_voices = 8;
    desc.transition_fade_us = 10000;
    desc.max_sounds = 2;
    desc.max_streams = scheduler ? 1 : 0;
    desc.scheduler = scheduler;
    desc.queue_capacity = VOICES;
    desc.resampler = RBTAUDIO_RESAMPLER_LINEAR;
    desc.limiter = RBTAUDIO_LIMITER_OFF;
    desc.virtual_threshold = 0.001f;
    desc.gain = 1.0f;
    rbtaudio_mixer mixer = NULL;
    CHECK(rbtaudio_mixer_create(&desc, &mixer, NULL) == RBT_RESULT_OK);
    if (!mixer)
        return;
    RBTAUDIO_SOUND_DESC sd;
    memset(&sd, 0, sizeof(sd));
    sd.base.type = RBT_STRUCT_TYPE_AUDIO_SOUND_DESC;
    sd.samples = tone;
    sd.frame_count = sizeof(tone) / sizeof(tone[0]);
    sd.channel_count = 1;
    sd.sample_rate = 48000;
    sd.virtual_behavior = RBTAUDIO_VIRTUAL_CONTINUE;
    RBTAUDIO_SOUND sound = 0;
    CHECK(rbtaudio_sound_create(mixer, &sd, &sound, NULL) == RBT_RESULT_OK);
    RBTAUDIO_ATTENUATION_DESC ad;
    memset(&ad, 0, sizeof(ad));
    ad.base.type = RBT_STRUCT_TYPE_AUDIO_ATTENUATION_DESC;
    ad.rolloff = RBTAUDIO_ROLLOFF_INVERSE;
    ad.min_distance = 1.0f;
    ad.max_distance = 50.0f;
    ad.rolloff_factor = 1.0f;
    ad.cone_inner_angle = 2.0f;
    ad.cone_outer_angle = 4.0f;
    ad.cone_outer_gain = 0.3f;
    ad.doppler_factor = 1.0f;
    ad.priority_offset = -50;
    RBTAUDIO_ATTENUATION attenuation = 0;
    CHECK(rbtaudio_attenuation_create(mixer, &ad, &attenuation, NULL) == RBT_RESULT_OK);
    static RBTAUDIO_VOICE voices[VOICES];
    static RBTAUDIO_EMITTER places[VOICES];
    for (int v = 0; v < VOICES; v++)
    {
        memset(&places[v], 0, sizeof(places[v]));
        places[v].position.x = 2.0 * cos(v);
        places[v].position.z = 2.0 + v * 0.2;
        places[v].forward.x = 1.0f;
        RBTAUDIO_PLAY_DESC play;
        memset(&play, 0, sizeof(play));
        play.base.type = RBT_STRUCT_TYPE_AUDIO_PLAY_DESC;
        play.gain = 0.1f;
        play.rate = 1.0f;
        play.priority = (uint8_t)(v % 200);
        play.loop = true;
        play.placement = RBTAUDIO_PLACEMENT_WORLD;
        play.emitter = &places[v];
        play.attenuation = attenuation;
        CHECK(rbtaudio_play(mixer, sound, &play, &voices[v]) == RBT_RESULT_OK);
    }
    RBTAUDIO_LISTENER listeners[2];
    memset(listeners, 0, sizeof(listeners));
    listeners[0].forward.z = -1.0f;
    listeners[0].up.y = 1.0f;
    listeners[1] = listeners[0];
    listeners[1].position.x = 40.0;
    for (int f = 0; f < FRAMES + 4; f++)
    {
        if (f == 4)
            start_counting();
        for (int v = 0; v < VOICES; v++)
        {
            places[v].position.x += 0.01;
            places[v].velocity.x = 0.6f;
        }
        listeners[0].position.z = 0.05 * f;
        CHECK(rbtaudio_mixer_set_listeners(mixer, listeners, 2, NULL) == RBT_RESULT_OK);
        CHECK(rbtaudio_mixer_set_emitters(mixer, voices, places, VOICES, NULL) == RBT_RESULT_OK);
        CHECK(rbtaudio_mixer_update(mixer, NULL) == RBT_RESULT_OK);
        rbtaudio_mixer_render(mixer, out, 512, NULL);
    }
    stop_counting();
    report(scheduler ? "Rabbittus Audio's spatial audio, 256 moving voices, two listeners, a pool of four"
                     : "Rabbittus Audio's spatial audio, 256 moving voices, two listeners");
    rbtaudio_mixer_destroy(mixer);
}

static void test_audio_spatial(void)
{
    spatial_scenario(NULL);
    RBTJOBS_POOL_DESC pd;
    memset(&pd, 0, sizeof(pd));
    pd.base.type = RBT_STRUCT_TYPE_JOBS_POOL_DESC;
    pd.thread_count = 3;
    pd.max_tasks = 4;
    pd.debug_name = "rbtnoalloc_tests spatial";
    rbtjobs_pool pool = NULL;
    CHECK(rbtjobs_pool_create(&pd, &pool, NULL) == RBT_RESULT_OK);
    if (pool)
    {
        RBTJOBS_SCHEDULER scheduler;
        memset(&scheduler, 0, sizeof(scheduler));
        scheduler.base.type = RBT_STRUCT_TYPE_JOBS_SCHEDULER;
        CHECK(rbtjobs_pool_get_scheduler(pool, &scheduler) == RBT_RESULT_OK);
        spatial_scenario(&scheduler);
    }
    rbtjobs_pool_destroy(pool);
}

static void test_bvh(void)
{
    // A grid of 32 x 32 quads, two triangles each.
    enum { N = 32 };
    static RbtDVec3 positions[(N + 1) * (N + 1)];
    static uint32_t indices[N * N * 6];
    for (int y = 0; y <= N; y++)
        for (int x = 0; x <= N; x++)
        {
            RbtDVec3 p = {(double)x, (double)y, (double)((x * 7 + y * 3) % 5) * 0.1};
            positions[y * (N + 1) + x] = p;
        }
    uint32_t k = 0;
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++)
        {
            const uint32_t a = (uint32_t)(y * (N + 1) + x), b = a + 1, c = a + N + 1, d = c + 1;
            const uint32_t quad[6] = {a, b, c, b, d, c};
            memcpy(indices + k, quad, sizeof(quad));
            k += 6;
        }
    RBTMATH_MESH_BVH_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_MATH_MESH_BVH_DESC;
    desc.positions = positions;
    desc.position_count = (N + 1) * (N + 1);
    desc.indices = indices;
    desc.index_count = k;
    rbtmath_mesh_bvh bvh = NULL;
    CHECK(rbtmath_mesh_bvh_create(&desc, &bvh) == RBT_RESULT_OK);
    if (!bvh)
        return;
    unsigned long hits = 0;
    start_counting();
    for (int f = 0; f < FRAMES; f++)
        for (int r = 0; r < 64; r++)
        {
            RBTMATH_RAY ray;
            memset(&ray, 0, sizeof(ray));
            ray.origin.x = 0.5 + (double)((f * 13 + r * 7) % N);
            ray.origin.y = 0.5 + (double)((f * 5 + r * 11) % N);
            ray.origin.z = 10.0;
            ray.direction.z = -1.0;
            ray.t_min = 0.0;
            ray.t_max = 100.0;
            RBTMATH_MESH_HIT hit;
            hits += rbtmath_mesh_bvh_cast(bvh, &ray, &hit);
        }
    stop_counting();
    CHECK(hits == FRAMES * 64);
    report("Rabbittus Math's ray casts, 64 a frame");

    // Volume tests over the same grid: a box over part of it, its bounds,
    // then both mesh tests, placed by a transform, with a clip plane.
    RBTMATH_PLANE planes[6] = {
        {{1, 0, 0}, -2.0}, {{-1, 0, 0}, 9.0}, {{0, 1, 0}, -2.0}, {{0, -1, 0}, 9.0}, {{0, 0, 1}, 5.0}, {{0, 0, -1}, 5.0},
    };
    RBTMATH_PLANE clip = {{0, 0, -1}, 0.3};
    RBTMATH_VOLUME volume;
    volume.planes = planes;
    volume.plane_count = 6;
    volume.clip_planes = &clip;
    volume.clip_plane_count = 1;
    RbtDMat4 move;
    memset(&move, 0, sizeof(move));
    move.m[0] = move.m[5] = move.m[10] = move.m[15] = 1.0;
    RBTMATH_AABB bounds = rbtmath_mesh_bvh_get_bounds(bvh);
    unsigned long found = 0;
    start_counting();
    for (int f = 0; f < FRAMES; f++)
    {
        move.m[12] = (double)(f % 8) - 4.0;
        found += rbtmath_aabb_test_volume(&bounds, &move, &volume) == RBTMATH_OVERLAP_PARTIAL;
        found += rbtmath_mesh_bvh_overlaps_volume(bvh, &move, &volume);
        found += !rbtmath_mesh_bvh_within_volume(bvh, &move, &volume);
    }
    stop_counting();
    CHECK(found == FRAMES * 3);
    report("Rabbittus Math's volume tests, a box, its mesh's overlap and within, a frame");

    // An instance BVH of 10,000 placements of the grid that move: each frame
    // 1% of them set and refitted, every eighth frame all of them set and
    // the tree rebuilt, then shapes overlapped and swept (the in-place calls
    // and the shape queries, rbtmath_bvh_queries_draft.h).
    enum
    {
        INSTANCES = 10000
    };
    static RbtDMat4 matrices[INSTANCES];
    static RBTMATH_BVH_INSTANCE instances[INSTANCES];
    static uint32_t indices_all[INSTANCES];
    for (uint32_t i = 0; i < INSTANCES; i++)
    {
        memset(&matrices[i], 0, sizeof(RbtDMat4));
        matrices[i].m[0] = matrices[i].m[5] = matrices[i].m[10] = 0.05;
        matrices[i].m[12] = (double)(i % 100);
        matrices[i].m[13] = (double)(i / 100);
        matrices[i].m[15] = 1.0;
        instances[i].mesh = bvh;
        instances[i].transform = &matrices[i];
        instances[i].id = i;
        instances[i].mask = 1;
        indices_all[i] = i;
    }
    RBTMATH_INSTANCE_BVH_DESC instance_desc;
    memset(&instance_desc, 0, sizeof(instance_desc));
    instance_desc.base.type = RBT_STRUCT_TYPE_MATH_INSTANCE_BVH_DESC;
    instance_desc.instances = instances;
    instance_desc.instance_count = INSTANCES;
    rbtmath_instance_bvh placed = NULL;
    CHECK(rbtmath_instance_bvh_create(&instance_desc, &placed) == RBT_RESULT_OK);
    if (!placed)
    {
        rbtmath_mesh_bvh_destroy(bvh);
        return;
    }
    unsigned long touches = 0;
    start_counting();
    for (int f = 0; f < FRAMES; f++)
    {
        for (uint32_t i = 0; i < INSTANCES; i++)
            matrices[i].m[14] = (double)((f + (int)i) % 3) * 0.1;
        if (f % 8 == 7)
        {
            CHECK(rbtmath_instance_bvh_set_instances(placed, indices_all, instances, INSTANCES, NULL) == RBT_RESULT_OK);
            rbtmath_instance_bvh_rebuild(placed);
        }
        else
        {
            CHECK(rbtmath_instance_bvh_set_instances(placed, indices_all + (f % 100) * 100, instances + (f % 100) * 100,
                                                    100, NULL) == RBT_RESULT_OK);
            rbtmath_instance_bvh_refit(placed);
        }
        touches += rbtmath_instance_bvh_get_cost(placed) > 0.0;
        for (int s = 0; s < 8; s++)
        {
            RBTMATH_SHAPE shape;
            memset(&shape, 0, sizeof(shape));
            shape.kind = (RBTMATH_SHAPE_KIND)(1 + s % 3);
            shape.center.x = 0.3 + (double)((f * 7 + s * 13) % 100);
            shape.center.y = 0.3 + (double)((f * 3 + s * 29) % 100);
            shape.center.z = 2.0;
            shape.rotation.w = 1.0;
            shape.radius = shape.kind == RBTMATH_SHAPE_KIND_BOX ? 0.0 : 0.2;
            shape.half_height = 0.3;
            shape.half_extents.x = shape.half_extents.y = shape.half_extents.z = 0.2;
            uint32_t list[16];
            touches += rbtmath_instance_bvh_overlaps_shape(placed, &shape, NULL);
            touches += rbtmath_instance_bvh_overlap_all(placed, &shape, NULL, list, 16) > 0;
            RBTMATH_SWEEP sweep;
            memset(&sweep, 0, sizeof(sweep));
            sweep.shape = shape;
            sweep.direction.z = -1.0;
            sweep.t_max = 10.0;
            RBTMATH_SWEEP_HIT hit, hits[4];
            touches += rbtmath_instance_bvh_sweep(placed, &sweep, NULL, &hit);
            touches += rbtmath_instance_bvh_sweep_all(placed, &sweep, NULL, hits, 4) > 0;
        }
    }
    stop_counting();
    CHECK(touches > 0);
    report("Rabbittus Math's instance BVH, 10,000 moving: set, refit or rebuild, and 8 shapes overlapped and swept a frame");
    rbtmath_instance_bvh_destroy(placed);
    rbtmath_mesh_bvh_destroy(bvh);

    // A box BVH of 100,000 solid boxes: set, refit, rebuild and every query.
    enum
    {
        BOXES = 100000
    };
    RBTMATH_BVH_BOX *boxes = (RBTMATH_BVH_BOX *)malloc(sizeof(RBTMATH_BVH_BOX) * BOXES);
    uint32_t *box_indices = (uint32_t *)malloc(sizeof(uint32_t) * BOXES);
    CHECK(boxes && box_indices);
    if (!boxes || !box_indices)
    {
        free(boxes);
        free(box_indices);
        return;
    }
    for (uint32_t i = 0; i < BOXES; i++)
    {
        double x = (double)(i % 316), y = (double)(i / 316 % 316), z = (double)(i / 99856);
        boxes[i].box.lo.x = x;
        boxes[i].box.lo.y = y;
        boxes[i].box.lo.z = z;
        boxes[i].box.hi.x = x + 0.5;
        boxes[i].box.hi.y = y + 0.5;
        boxes[i].box.hi.z = z + 0.5;
        boxes[i].id = i;
        boxes[i].mask = 1;
        box_indices[i] = i;
    }
    RBTMATH_BOX_BVH_DESC box_desc;
    memset(&box_desc, 0, sizeof(box_desc));
    box_desc.base.type = RBT_STRUCT_TYPE_MATH_BOX_BVH_DESC;
    box_desc.boxes = boxes;
    box_desc.box_count = BOXES;
    rbtmath_box_bvh box_bvh = NULL;
    CHECK(rbtmath_box_bvh_create(&box_desc, &box_bvh, NULL) == RBT_RESULT_OK);
    unsigned long met = 0;
    if (box_bvh)
    {
        start_counting();
        for (int f = 0; f < FRAMES; f++)
        {
            for (uint32_t i = 0; i < 1000; i++)
            {
                boxes[(f * 1000 + i) % BOXES].box.lo.z += 0.01;
                boxes[(f * 1000 + i) % BOXES].box.hi.z += 0.01;
            }
            CHECK(rbtmath_box_bvh_set_boxes(box_bvh, box_indices + (f * 1000) % BOXES, boxes + (f * 1000) % BOXES, 1000,
                                           NULL) == RBT_RESULT_OK);
            if (f % 16 == 15)
                rbtmath_box_bvh_rebuild(box_bvh);
            else
                rbtmath_box_bvh_refit(box_bvh);
            RBTMATH_RAY ray;
            memset(&ray, 0, sizeof(ray));
            ray.origin.x = 0.25 + (double)(f % 316);
            ray.origin.y = 0.25;
            ray.origin.z = 5.0;
            ray.direction.y = 1.0;
            ray.direction.z = -0.02;
            ray.t_max = 400.0;
            RBTMATH_BOX_HIT hit, hits[8];
            met += rbtmath_box_bvh_cast(box_bvh, &ray, NULL, &hit);
            met += rbtmath_box_bvh_cast_all(box_bvh, &ray, NULL, hits, 8) > 0;
            RBTMATH_SHAPE shape;
            memset(&shape, 0, sizeof(shape));
            shape.kind = RBTMATH_SHAPE_KIND_SPHERE;
            shape.center.x = 10.0 + f % 50;
            shape.center.y = 10.0;
            shape.center.z = 0.25;
            shape.radius = 2.0;
            uint32_t list[8];
            met += rbtmath_box_bvh_overlaps_shape(box_bvh, &shape, NULL);
            met += rbtmath_box_bvh_overlap_all(box_bvh, &shape, NULL, list, 8) > 0;
            RBTMATH_SWEEP sweep;
            memset(&sweep, 0, sizeof(sweep));
            sweep.shape = shape;
            sweep.shape.center.z = 6.0;
            sweep.direction.z = -1.0;
            sweep.t_max = 10.0;
            met += rbtmath_box_bvh_sweep(box_bvh, &sweep, NULL, &hit);
            met += rbtmath_box_bvh_sweep_all(box_bvh, &sweep, NULL, hits, 8) > 0;
        }
        stop_counting();
        CHECK(met > 0);
        report("Rabbittus Math's box BVH, 100,000 boxes, 1,000 set a frame, refit or rebuilt, every query");
        rbtmath_box_bvh_destroy(box_bvh);
    }
    free(box_indices);
    free(boxes);
}

// ---- Rabbittus Jobs ----------------------------------------------------------------------

static void sum_task(uint32_t first, uint32_t end, uint32_t worker, void *data)
{
    (void)worker;
    unsigned long *sums = (unsigned long *)data;
    for (uint32_t i = first; i < end; i++)
        sums[i] = i * 2u;
}

static void test_jobs(void)
{
    RBTJOBS_POOL_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_JOBS_POOL_DESC;
    desc.thread_count = 2;
    desc.max_tasks = 4;
    rbtjobs_pool pool = NULL;
    CHECK(rbtjobs_pool_create(&desc, &pool, NULL) == RBT_RESULT_OK);
    if (!pool)
        return;
    RBTJOBS_SCHEDULER scheduler;
    memset(&scheduler, 0, sizeof(scheduler));
    scheduler.base.type = RBT_STRUCT_TYPE_JOBS_SCHEDULER;
    CHECK(rbtjobs_pool_get_scheduler(pool, &scheduler) == RBT_RESULT_OK);
    static unsigned long sums[10000];
    for (int f = 0; f < 4; f++)
        CHECK(rbtjobs_run(&scheduler, sum_task, 10000, 64, sums) == RBT_RESULT_OK);
    start_counting();
    for (int f = 0; f < FRAMES; f++)
        rbtjobs_run(&scheduler, sum_task, 10000, 64, sums);
    stop_counting();
    CHECK(sums[9999] == 19998);
    report("Rabbittus Jobs' pool running a task of 10,000 items, the waiting thread counted");
    rbtjobs_pool_destroy(pool);
}

// A tree of tasks in one group, each enqueuing its two children into it
// (rbtjobs.h, RBTJOBS_SCHEDULER_GROUPS).
#define JOBS_TREE_COUNT 1000

struct JobsTree;

typedef struct JobsTreeNode
{
    struct JobsTree *tree;
    uint32_t id;
} JobsTreeNode;

typedef struct JobsTree
{
    const RBTJOBS_SCHEDULER_GROUPS *groups;
    void *group;
    void *user;
    JobsTreeNode nodes[JOBS_TREE_COUNT];
    uint8_t hits[JOBS_TREE_COUNT];
} JobsTree;

static void tree_task(uint32_t first, uint32_t end, uint32_t worker, void *data)
{
    (void)first;
    (void)end;
    JobsTreeNode *node = (JobsTreeNode *)data;
    JobsTree *tree = node->tree;
    tree->hits[node->id] = 1;
    for (uint32_t c = node->id * 2 + 1; c <= node->id * 2 + 2 && c < JOBS_TREE_COUNT; c++)
        tree->groups->group_enqueue(tree->group, worker, tree_task, 1, 1, &tree->nodes[c], tree->user);
}

static void test_job_groups(void)
{
    RBTJOBS_POOL_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_JOBS_POOL_DESC;
    desc.thread_count = 3;
    desc.max_tasks = 64;
    rbtjobs_pool pool = NULL;
    CHECK(rbtjobs_pool_create(&desc, &pool, NULL) == RBT_RESULT_OK);
    if (!pool)
        return;
    RBTJOBS_SCHEDULER scheduler;
    CHECK(rbtjobs_pool_get_scheduler(pool, &scheduler) == RBT_RESULT_OK);
    CHECK(scheduler.base.next != NULL);
    if (!scheduler.base.next)
    {
        rbtjobs_pool_destroy(pool);
        return;
    }
    static JobsTree tree;
    tree.groups = (const RBTJOBS_SCHEDULER_GROUPS *)scheduler.base.next;
    tree.user = scheduler.user;
    tree.group = tree.groups->group_create(scheduler.user);
    CHECK(tree.group != NULL);
    for (uint32_t i = 0; i < JOBS_TREE_COUNT; i++)
    {
        tree.nodes[i].tree = &tree;
        tree.nodes[i].id = i;
    }
    start_counting();
    for (int f = 0; f < 100; f++)
    {
        memset(tree.hits, 0, sizeof(tree.hits));
        tree.groups->group_enqueue(tree.group, 0, tree_task, 1, 1, &tree.nodes[0], tree.user);
        tree.groups->group_wait(tree.group, tree.user);
    }
    stop_counting();
    uint32_t ran = 0;
    for (uint32_t i = 0; i < JOBS_TREE_COUNT; i++)
        ran += tree.hits[i];
    CHECK(ran == JOBS_TREE_COUNT);
    report("Rabbittus Jobs' pool running a tree of 1,000 tasks in one group, 100 times, the waiting thread counted");
    tree.groups->group_destroy(tree.group, tree.user);
    rbtjobs_pool_destroy(pool);
}

// ---- Rabbittus Text ----------------------------------------------------------------------

static void test_text(const char *dir)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/rbttext_variable_abc.ttf", dir ? dir : ".");
    FILE *file = fopen(path, "rb");
    CHECK(file != NULL);
    if (!file)
        return;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    unsigned char *font = (unsigned char *)malloc((size_t)size);
    CHECK(font && fread(font, 1, (size_t)size, file) == (size_t)size);
    fclose(file);
    RBTTEXT_FACE_DESC fd;
    memset(&fd, 0, sizeof(fd));
    fd.base.type = RBT_STRUCT_TYPE_TEXT_FACE_DESC;
    fd.data = font;
    fd.data_size = (uint64_t)size;
    rbttext_face face = NULL;
    CHECK(rbttext_face_create(&fd, &face, NULL) == RBT_RESULT_OK);
    RBTTEXT_CHAIN_DESC cd;
    memset(&cd, 0, sizeof(cd));
    cd.base.type = RBT_STRUCT_TYPE_TEXT_CHAIN_DESC;
    cd.faces = &face;
    cd.face_count = 1;
    rbttext_chain chain = NULL;
    CHECK(face && rbttext_chain_create(&cd, &chain, NULL) == RBT_RESULT_OK);
    rbttext_layout layout = NULL;
    CHECK(rbttext_layout_create(&layout) == RBT_RESULT_OK);
    if (chain && layout)
    {
        static const char label[] = "abc cab bca\nacb";
        RBTTEXT_LAYOUT_DESC ld;
        memset(&ld, 0, sizeof(ld));
        ld.base.type = RBT_STRUCT_TYPE_TEXT_LAYOUT_DESC;
        ld.chain = chain;
        ld.text = label;
        ld.text_size = (uint32_t)strlen(label);
        ld.direction = RBTTEXT_DIRECTION_LTR;
        ld.features = RBTTEXT_FEATURE_KERNING | RBTTEXT_FEATURE_LIGATURES;
        ld.em_size = 16.0f;
        ld.align = RBTTEXT_ALIGN_LEFT;
        ld.line_spacing = 1.0f;
        for (int f = 0; f < 4; f++)
            CHECK(rbttext_layout_build(layout, &ld, NULL) == RBT_RESULT_OK);
        start_counting();
        for (int f = 0; f < FRAMES; f++)
            rbttext_layout_build(layout, &ld, NULL);
        stop_counting();
        report("Rabbittus Text laying one label out again with the layout it keeps");
    }
    rbttext_layout_destroy(layout);
    rbttext_chain_destroy(chain);
    rbttext_face_destroy(face);
    free(font);
}

int main(int argc, char **argv)
{
    if (!alloc_trace_available())
    {
        printf("rbtnoalloc_tests: no allocation hook on this platform or build (a sanitizer's heap): nothing "
               "measured\n");
        return 0;
    }
    test_hook_counts();
    test_audio();
    test_audio_streams(argc > 2 ? argv[2] : NULL);
    test_audio_spatial();
    test_bvh();
    test_jobs();
    test_job_groups();
    test_text(argc > 1 ? argv[1] : NULL);
    if (failures)
    {
        printf("rbtnoalloc_tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("rbtnoalloc_tests: no allocation in any steady state\n");
    return 0;
}
