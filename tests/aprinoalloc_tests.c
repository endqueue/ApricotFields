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
// folder as its argument.

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
        apaudio_mixer_update(mixer);
        apaudio_mixer_render(mixer, out, 512, NULL);
    }
    start_counting();
    for (int f = 0; f < FRAMES; f++)
    {
        apaudio_mixer_update(mixer);
        apaudio_mixer_render(mixer, out, 512, NULL);
    }
    stop_counting();
    report("ApAudio's update and render, three looping voices resampled, the limiter on");
    apaudio_mixer_destroy(mixer);
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
    apmath_mesh_bvh_destroy(bvh);
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
