// No allocation per frame (ROADMAP.md, section 3.1, "Data-oriented
// checks"): each module's steady state, after a warm-up, counted for 100
// frames with every heap allocation the measuring thread makes, through
// tests/alloctrace.h, which says what each platform's hook sees and how
// each allocation is put down to the code that asked for it: only ours
// (this executable's) must be none. Where no hook is available the test
// says so and passes nothing off as checked.
//
// A frame, per module: ApAudio's render (the audio callback) and update
// with sounds playing and the limiter on; ApMath's ray casts against a
// mesh's BVH; ApJobs' reference pool running a task; ApText laying the
// same label out again with a layout it keeps. Aprend's frame needs a GPU:
// ../apricottests measures it.
//
// Written in C, as the other tests of C headers are. Takes ApText's data
// folder as its argument, and ApAudio's as its second.

#include "audio/apaudio.h"
#include "jobs/apjobs.h"
#include "math/apmathbvh.h"
#include "text/aptext.h"
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
    printf("aprinoalloc_tests: the hook counted %lu of 2 known allocations of ours\n", alloc_trace_ours());
    CHECK(alloc_trace_ours() == 2);
}

// Ours must be none; what other modules asked for is reported, not
// counted.
static void report(const char *what)
{
    char line[256];
    snprintf(line, sizeof(line), "aprinoalloc_tests: %s, %d frames", what, FRAMES);
    alloc_trace_report(line, 4);
    if (alloc_trace_ours() != 0)
        failures++;
}

// ---- ApAudio ---------------------------------------------------------------------

static const SPUDAUDIO_CHANNEL_POSITION stereo[2] = {SPUDAUDIO_CHANNEL_POSITION_FL, SPUDAUDIO_CHANNEL_POSITION_FR};
static float tone[4800];
static float out[2 * 512];

static void test_audio(void)
{
    APAUDIO_MIXER_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APAUDIO_MIXER_DESC;
    desc.format.sample_format = SPUDAUDIO_SAMPLE_FORMAT_F32;
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
    desc.resampler = APAUDIO_RESAMPLER_LINEAR;
    desc.limiter = APAUDIO_LIMITER_ON;
    desc.limiter_lookahead_us = 1500;
    desc.limiter_release_us = 80000;
    desc.limiter_ceiling = 0.97f;
    desc.gain = 1.0f;
    apaudio_mixer mixer = NULL;
    CHECK(apaudio_mixer_create(&desc, &mixer, NULL) == APRESULT_OK);
    if (!mixer)
        return;
    for (size_t i = 0; i < sizeof(tone) / sizeof(tone[0]); i++)
        tone[i] = (float)((i % 96) < 48 ? 0.5 : -0.5);
    APAUDIO_SOUND_DESC sd;
    memset(&sd, 0, sizeof(sd));
    sd.base.type = APRI_STRUCT_TYPE_APAUDIO_SOUND_DESC;
    sd.samples = tone;
    sd.frame_count = sizeof(tone) / sizeof(tone[0]);
    sd.channel_count = 1;
    sd.sample_rate = 44100; // resampled
    sd.virtual_behavior = APAUDIO_VIRTUAL_KILL;
    APAUDIO_SOUND sound = 0;
    CHECK(apaudio_sound_create(mixer, &sd, &sound, NULL) == APRESULT_OK);
    APAUDIO_PLAY_DESC play;
    memset(&play, 0, sizeof(play));
    play.base.type = APRI_STRUCT_TYPE_APAUDIO_PLAY_DESC;
    play.gain = 1.0f;
    play.rate = 1.0f;
    play.virtual_behavior = APAUDIO_VIRTUAL_KILL;
    play.priority = 128;
    play.loop = true;
    for (int i = 0; i < 3; i++)
    {
        APAUDIO_VOICE voice = 0;
        CHECK(apaudio_play(mixer, sound, &play, &voice) == APRESULT_OK);
    }
    // The warm-up: the queue drained, the voices started.
    for (int f = 0; f < 4; f++)
    {
        CHECK(apaudio_mixer_update(mixer, NULL) == APRESULT_OK);
        apaudio_mixer_render(mixer, out, 512, NULL);
    }
    start_counting();
    for (int f = 0; f < FRAMES; f++)
    {
        CHECK(apaudio_mixer_update(mixer, NULL) == APRESULT_OK);
        apaudio_mixer_render(mixer, out, 512, NULL);
    }
    stop_counting();
    report("ApAudio's update and render, three looping voices resampled, the limiter on");
    apaudio_mixer_destroy(mixer);
}

// ApAudio's streamed voices (APAUDIO_TODO.md item 15): a voice of a
// streamed sound playing, looped, past its prefetch, update, render and
// collect; with no scheduler, so the decoding runs inside the update on this
// thread, and on a pool of four (this thread counted as it waits). The
// stream's start - its one allocation, its decoder's open and first fill -
// is counted apart and reported, not checked. Pending while the mixer
// refuses streams (APRESULT_NOT_IMPLEMENTED).

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

static void stream_scenario(const char *dir, const char *name, APAUDIO_STREAM_CODEC codec, const char *codec_name,
                            const APJOBS_SCHEDULER *scheduler)
{
    unsigned char *bytes = NULL;
    uint64_t size = 0;
    if (!read_file(dir, name, &bytes, &size))
    {
        free(bytes);
        return;
    }
    memory_bytes memory = {bytes, size};
    APAUDIO_MIXER_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APAUDIO_MIXER_DESC;
    desc.format.sample_format = SPUDAUDIO_SAMPLE_FORMAT_F32;
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
    desc.resampler = APAUDIO_RESAMPLER_SINC;
    desc.limiter = APAUDIO_LIMITER_OFF;
    desc.gain = 1.0f;
    desc.max_streams = 2;
    desc.scheduler = scheduler;
    apaudio_mixer mixer = NULL;
    APRESULT result = apaudio_mixer_create(&desc, &mixer, NULL);
    if (result == APRESULT_NOT_IMPLEMENTED)
    {
        printf("aprinoalloc_tests: ApAudio's streamed %s voice: pending (not implemented yet)\n", codec_name);
        free(bytes);
        return;
    }
    CHECK(result == APRESULT_OK);
    APAUDIO_SOUND_DESC sd;
    memset(&sd, 0, sizeof(sd));
    sd.base.type = APRI_STRUCT_TYPE_APAUDIO_SOUND_DESC;
    sd.virtual_behavior = APAUDIO_VIRTUAL_CONTINUE;
    sd.stream_codec = codec;
    sd.stream_source.user = &memory;
    sd.stream_source.size = size;
    sd.stream_source.read = memory_read;
    sd.stream_ring_us = 100000;
    APAUDIO_SOUND sound = 0;
    CHECK(mixer && apaudio_sound_create(mixer, &sd, &sound, NULL) == APRESULT_OK);
    if (sound)
    {
        APAUDIO_PLAY_DESC play;
        memset(&play, 0, sizeof(play));
        play.base.type = APRI_STRUCT_TYPE_APAUDIO_PLAY_DESC;
        play.gain = 1.0f;
        play.rate = 1.0f;
        play.priority = 128;
        play.loop = true;
        APAUDIO_VOICE voice = 0;
        CHECK(apaudio_play(mixer, sound, &play, &voice) == APRESULT_OK);
        // The start, apart: the first render (the prefetch), and the update
        // that opens the stream and fills its ring.
        start_counting();
        apaudio_mixer_render(mixer, out, 512, NULL);
        CHECK(apaudio_mixer_update(mixer, NULL) == APRESULT_OK);
        stop_counting();
        printf("aprinoalloc_tests: ApAudio's streamed %s voice's start%s: %lu allocation(s) of ours (the stream's "
               "block%s), not checked\n",
               codec_name, scheduler ? " on a pool" : "", alloc_trace_ours(),
               codec == APAUDIO_STREAM_CODEC_OPUS ? ", and opusfile's and libopus's own" : "");
        // The warm-up: past the prefetch, the ring wrapping.
        for (int f = 0; f < 8; f++)
        {
            apaudio_mixer_render(mixer, out, 512, NULL);
            CHECK(apaudio_mixer_update(mixer, NULL) == APRESULT_OK);
            apaudio_mixer_collect(mixer);
        }
        start_counting();
        for (int f = 0; f < FRAMES; f++)
        {
            apaudio_mixer_render(mixer, out, 512, NULL);
            CHECK(apaudio_mixer_update(mixer, NULL) == APRESULT_OK);
            apaudio_mixer_collect(mixer);
        }
        stop_counting();
        char what[160];
        snprintf(what, sizeof(what), "ApAudio's streamed %s voice, update, render and collect%s", codec_name,
                 scheduler ? ", decoding on a pool of four" : ", decoding in the update");
        report(what);
        CHECK(apaudio_sound_get_underrun_frames(mixer, sound) == 0);
    }
    apaudio_mixer_destroy(mixer);
    free(bytes);
}

static void test_audio_streams(const char *dir)
{
    static const char *const names[3] = {"stream.flac", "stream.ogg", "stream.opus"};
    static const APAUDIO_STREAM_CODEC codecs[3] = {APAUDIO_STREAM_CODEC_FLAC, APAUDIO_STREAM_CODEC_VORBIS,
                                                   APAUDIO_STREAM_CODEC_OPUS};
    static const char *const codec_names[3] = {"FLAC", "Ogg Vorbis", "Ogg Opus"};
    APJOBS_POOL_DESC pd;
    memset(&pd, 0, sizeof(pd));
    pd.base.type = APRI_STRUCT_TYPE_APJOBS_POOL_DESC;
    pd.thread_count = 3;
    pd.max_tasks = 4;
    apjobs_pool pool = NULL;
    CHECK(apjobs_pool_create(&pd, &pool, NULL) == APRESULT_OK);
    APJOBS_SCHEDULER scheduler;
    memset(&scheduler, 0, sizeof(scheduler));
    scheduler.base.type = APRI_STRUCT_TYPE_APJOBS_SCHEDULER;
    if (pool)
        CHECK(apjobs_pool_get_scheduler(pool, &scheduler) == APRESULT_OK);
    for (int c = 0; c < 3; c++)
    {
        stream_scenario(dir, names[c], codecs[c], codec_names[c], NULL);
        if (pool)
            stream_scenario(dir, names[c], codecs[c], codec_names[c], &scheduler);
    }
    apjobs_pool_destroy(pool);
}

// ---- ApMath ----------------------------------------------------------------------

static void test_bvh(void)
{
    // A grid of 32 x 32 quads, two triangles each.
    enum { N = 32 };
    static ApriDVec3 positions[(N + 1) * (N + 1)];
    static uint32_t indices[N * N * 6];
    for (int y = 0; y <= N; y++)
        for (int x = 0; x <= N; x++)
        {
            ApriDVec3 p = {(double)x, (double)y, (double)((x * 7 + y * 3) % 5) * 0.1};
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
    APMATH_MESH_BVH_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APMATH_MESH_BVH_DESC;
    desc.positions = positions;
    desc.position_count = (N + 1) * (N + 1);
    desc.indices = indices;
    desc.index_count = k;
    apmath_mesh_bvh bvh = NULL;
    CHECK(apmath_mesh_bvh_create(&desc, &bvh) == APRESULT_OK);
    if (!bvh)
        return;
    unsigned long hits = 0;
    start_counting();
    for (int f = 0; f < FRAMES; f++)
        for (int r = 0; r < 64; r++)
        {
            APMATH_RAY ray;
            memset(&ray, 0, sizeof(ray));
            ray.origin.x = 0.5 + (double)((f * 13 + r * 7) % N);
            ray.origin.y = 0.5 + (double)((f * 5 + r * 11) % N);
            ray.origin.z = 10.0;
            ray.direction.z = -1.0;
            ray.t_min = 0.0;
            ray.t_max = 100.0;
            APMATH_MESH_HIT hit;
            hits += apmath_mesh_bvh_cast(bvh, &ray, &hit);
        }
    stop_counting();
    CHECK(hits == FRAMES * 64);
    report("ApMath's ray casts, 64 a frame");

    // Volume tests over the same grid: a box over part of it, its bounds,
    // then both mesh tests, placed by a transform, with a clip plane.
    APMATH_PLANE planes[6] = {
        {{1, 0, 0}, -2.0}, {{-1, 0, 0}, 9.0}, {{0, 1, 0}, -2.0}, {{0, -1, 0}, 9.0}, {{0, 0, 1}, 5.0}, {{0, 0, -1}, 5.0},
    };
    APMATH_PLANE clip = {{0, 0, -1}, 0.3};
    APMATH_VOLUME volume;
    volume.planes = planes;
    volume.plane_count = 6;
    volume.clip_planes = &clip;
    volume.clip_plane_count = 1;
    ApriDMat4 move;
    memset(&move, 0, sizeof(move));
    move.m[0] = move.m[5] = move.m[10] = move.m[15] = 1.0;
    APMATH_AABB bounds = apmath_mesh_bvh_get_bounds(bvh);
    unsigned long found = 0;
    start_counting();
    for (int f = 0; f < FRAMES; f++)
    {
        move.m[12] = (double)(f % 8) - 4.0;
        found += apmath_aabb_test_volume(&bounds, &move, &volume) == APMATH_OVERLAP_PARTIAL;
        found += apmath_mesh_bvh_overlaps_volume(bvh, &move, &volume);
        found += !apmath_mesh_bvh_within_volume(bvh, &move, &volume);
    }
    stop_counting();
    CHECK(found == FRAMES * 3);
    report("ApMath's volume tests, a box, its mesh's overlap and within, a frame");

    // An instance BVH of 10,000 placements of the grid that move: each frame
    // 1% of them set and refitted, every eighth frame all of them set and
    // the tree rebuilt, then shapes overlapped and swept (the in-place calls
    // and the shape queries, apmath_bvh_queries_draft.h).
    enum
    {
        INSTANCES = 10000
    };
    static ApriDMat4 matrices[INSTANCES];
    static APMATH_BVH_INSTANCE instances[INSTANCES];
    static uint32_t indices_all[INSTANCES];
    for (uint32_t i = 0; i < INSTANCES; i++)
    {
        memset(&matrices[i], 0, sizeof(ApriDMat4));
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
    APMATH_INSTANCE_BVH_DESC instance_desc;
    memset(&instance_desc, 0, sizeof(instance_desc));
    instance_desc.base.type = APRI_STRUCT_TYPE_APMATH_INSTANCE_BVH_DESC;
    instance_desc.instances = instances;
    instance_desc.instance_count = INSTANCES;
    apmath_instance_bvh placed = NULL;
    CHECK(apmath_instance_bvh_create(&instance_desc, &placed) == APRESULT_OK);
    if (!placed)
    {
        apmath_mesh_bvh_destroy(bvh);
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
            CHECK(apmath_instance_bvh_set_instances(placed, indices_all, instances, INSTANCES, NULL) == APRESULT_OK);
            apmath_instance_bvh_rebuild(placed);
        }
        else
        {
            CHECK(apmath_instance_bvh_set_instances(placed, indices_all + (f % 100) * 100, instances + (f % 100) * 100,
                                                    100, NULL) == APRESULT_OK);
            apmath_instance_bvh_refit(placed);
        }
        touches += apmath_instance_bvh_get_cost(placed) > 0.0;
        for (int s = 0; s < 8; s++)
        {
            APMATH_SHAPE shape;
            memset(&shape, 0, sizeof(shape));
            shape.kind = (APMATH_SHAPE_KIND)(1 + s % 3);
            shape.center.x = 0.3 + (double)((f * 7 + s * 13) % 100);
            shape.center.y = 0.3 + (double)((f * 3 + s * 29) % 100);
            shape.center.z = 2.0;
            shape.rotation.w = 1.0;
            shape.radius = shape.kind == APMATH_SHAPE_KIND_BOX ? 0.0 : 0.2;
            shape.half_height = 0.3;
            shape.half_extents.x = shape.half_extents.y = shape.half_extents.z = 0.2;
            uint32_t list[16];
            touches += apmath_instance_bvh_overlaps_shape(placed, &shape, NULL);
            touches += apmath_instance_bvh_overlap_all(placed, &shape, NULL, list, 16) > 0;
            APMATH_SWEEP sweep;
            memset(&sweep, 0, sizeof(sweep));
            sweep.shape = shape;
            sweep.direction.z = -1.0;
            sweep.t_max = 10.0;
            APMATH_SWEEP_HIT hit, hits[4];
            touches += apmath_instance_bvh_sweep(placed, &sweep, NULL, &hit);
            touches += apmath_instance_bvh_sweep_all(placed, &sweep, NULL, hits, 4) > 0;
        }
    }
    stop_counting();
    CHECK(touches > 0);
    report("ApMath's instance BVH, 10,000 moving: set, refit or rebuild, and 8 shapes overlapped and swept a frame");
    apmath_instance_bvh_destroy(placed);
    apmath_mesh_bvh_destroy(bvh);

    // A box BVH of 100,000 solid boxes: set, refit, rebuild and every query.
    enum
    {
        BOXES = 100000
    };
    APMATH_BVH_BOX *boxes = (APMATH_BVH_BOX *)malloc(sizeof(APMATH_BVH_BOX) * BOXES);
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
    APMATH_BOX_BVH_DESC box_desc;
    memset(&box_desc, 0, sizeof(box_desc));
    box_desc.base.type = APRI_STRUCT_TYPE_APMATH_BOX_BVH_DESC;
    box_desc.boxes = boxes;
    box_desc.box_count = BOXES;
    apmath_box_bvh box_bvh = NULL;
    CHECK(apmath_box_bvh_create(&box_desc, &box_bvh, NULL) == APRESULT_OK);
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
            CHECK(apmath_box_bvh_set_boxes(box_bvh, box_indices + (f * 1000) % BOXES, boxes + (f * 1000) % BOXES, 1000,
                                           NULL) == APRESULT_OK);
            if (f % 16 == 15)
                apmath_box_bvh_rebuild(box_bvh);
            else
                apmath_box_bvh_refit(box_bvh);
            APMATH_RAY ray;
            memset(&ray, 0, sizeof(ray));
            ray.origin.x = 0.25 + (double)(f % 316);
            ray.origin.y = 0.25;
            ray.origin.z = 5.0;
            ray.direction.y = 1.0;
            ray.direction.z = -0.02;
            ray.t_max = 400.0;
            APMATH_BOX_HIT hit, hits[8];
            met += apmath_box_bvh_cast(box_bvh, &ray, NULL, &hit);
            met += apmath_box_bvh_cast_all(box_bvh, &ray, NULL, hits, 8) > 0;
            APMATH_SHAPE shape;
            memset(&shape, 0, sizeof(shape));
            shape.kind = APMATH_SHAPE_KIND_SPHERE;
            shape.center.x = 10.0 + f % 50;
            shape.center.y = 10.0;
            shape.center.z = 0.25;
            shape.radius = 2.0;
            uint32_t list[8];
            met += apmath_box_bvh_overlaps_shape(box_bvh, &shape, NULL);
            met += apmath_box_bvh_overlap_all(box_bvh, &shape, NULL, list, 8) > 0;
            APMATH_SWEEP sweep;
            memset(&sweep, 0, sizeof(sweep));
            sweep.shape = shape;
            sweep.shape.center.z = 6.0;
            sweep.direction.z = -1.0;
            sweep.t_max = 10.0;
            met += apmath_box_bvh_sweep(box_bvh, &sweep, NULL, &hit);
            met += apmath_box_bvh_sweep_all(box_bvh, &sweep, NULL, hits, 8) > 0;
        }
        stop_counting();
        CHECK(met > 0);
        report("ApMath's box BVH, 100,000 boxes, 1,000 set a frame, refit or rebuilt, every query");
        apmath_box_bvh_destroy(box_bvh);
    }
    free(box_indices);
    free(boxes);
}

// ---- ApJobs ----------------------------------------------------------------------

static void sum_task(uint32_t first, uint32_t end, uint32_t worker, void *data)
{
    (void)worker;
    unsigned long *sums = (unsigned long *)data;
    for (uint32_t i = first; i < end; i++)
        sums[i] = i * 2u;
}

static void test_jobs(void)
{
    APJOBS_POOL_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APJOBS_POOL_DESC;
    desc.thread_count = 2;
    desc.max_tasks = 4;
    apjobs_pool pool = NULL;
    CHECK(apjobs_pool_create(&desc, &pool, NULL) == APRESULT_OK);
    if (!pool)
        return;
    APJOBS_SCHEDULER scheduler;
    memset(&scheduler, 0, sizeof(scheduler));
    scheduler.base.type = APRI_STRUCT_TYPE_APJOBS_SCHEDULER;
    CHECK(apjobs_pool_get_scheduler(pool, &scheduler) == APRESULT_OK);
    static unsigned long sums[10000];
    for (int f = 0; f < 4; f++)
        CHECK(apjobs_run(&scheduler, sum_task, 10000, 64, sums) == APRESULT_OK);
    start_counting();
    for (int f = 0; f < FRAMES; f++)
        apjobs_run(&scheduler, sum_task, 10000, 64, sums);
    stop_counting();
    CHECK(sums[9999] == 19998);
    report("ApJobs' pool running a task of 10,000 items, the waiting thread counted");
    apjobs_pool_destroy(pool);
}

// ---- ApText ----------------------------------------------------------------------

static void test_text(const char *dir)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/aptext_variable_abc.ttf", dir ? dir : ".");
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
    APTEXT_FACE_DESC fd;
    memset(&fd, 0, sizeof(fd));
    fd.base.type = APRI_STRUCT_TYPE_APTEXT_FACE_DESC;
    fd.data = font;
    fd.data_size = (uint64_t)size;
    aptext_face face = NULL;
    CHECK(aptext_face_create(&fd, &face, NULL) == APRESULT_OK);
    APTEXT_CHAIN_DESC cd;
    memset(&cd, 0, sizeof(cd));
    cd.base.type = APRI_STRUCT_TYPE_APTEXT_CHAIN_DESC;
    cd.faces = &face;
    cd.face_count = 1;
    aptext_chain chain = NULL;
    CHECK(face && aptext_chain_create(&cd, &chain, NULL) == APRESULT_OK);
    aptext_layout layout = NULL;
    CHECK(aptext_layout_create(&layout) == APRESULT_OK);
    if (chain && layout)
    {
        static const char label[] = "abc cab bca\nacb";
        APTEXT_LAYOUT_DESC ld;
        memset(&ld, 0, sizeof(ld));
        ld.base.type = APRI_STRUCT_TYPE_APTEXT_LAYOUT_DESC;
        ld.chain = chain;
        ld.text = label;
        ld.text_size = (uint32_t)strlen(label);
        ld.direction = APTEXT_DIRECTION_LTR;
        ld.features = APTEXT_FEATURE_KERNING | APTEXT_FEATURE_LIGATURES;
        ld.em_size = 16.0f;
        ld.align = APTEXT_ALIGN_LEFT;
        ld.line_spacing = 1.0f;
        for (int f = 0; f < 4; f++)
            CHECK(aptext_layout_build(layout, &ld, NULL) == APRESULT_OK);
        start_counting();
        for (int f = 0; f < FRAMES; f++)
            aptext_layout_build(layout, &ld, NULL);
        stop_counting();
        report("ApText laying one label out again with the layout it keeps");
    }
    aptext_layout_destroy(layout);
    aptext_chain_destroy(chain);
    aptext_face_destroy(face);
    free(font);
}

int main(int argc, char **argv)
{
    if (!alloc_trace_available())
    {
        printf("aprinoalloc_tests: no allocation hook on this platform or build (a sanitizer's heap): nothing "
               "measured\n");
        return 0;
    }
    test_hook_counts();
    test_audio();
    test_audio_streams(argc > 2 ? argv[2] : NULL);
    test_bvh();
    test_jobs();
    test_text(argc > 1 ? argv[1] : NULL);
    if (failures)
    {
        printf("aprinoalloc_tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("aprinoalloc_tests: no allocation in any steady state\n");
    return 0;
}
