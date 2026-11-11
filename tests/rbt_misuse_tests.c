// Written by tests/misuse/make_misuse_tests.py from abi.txt: not by hand.
// Every public function of Rabbittus called with every argument zeroed
// (ROADMAP.md, section 3.1). None may crash; one returning RBT_RESULT returns
// an error unless the generator lists zeroes as a valid call for it.
// Then, in a Debug build not under a sanitizer, every one whose first
// parameter is a handle type misuse/misuse_factories.h can make, called
// with a destroyed handle (DECISIONS.md, card X16): each refuses.
//
// Handle types with no factory, not called with a destroyed handle:
// - A Rend handle needs a GPU device, which these tests never make; apricottests/tests/rbtrend_gpu_tests.c, test_destroyed_handles, calls these.
//   Not covered: rbtrend_batch, rbtrend_binding_layout, rbtrend_binding_set, rbtrend_blas, rbtrend_buffer, rbtrend_command_list, rbtrend_compute_pass, rbtrend_compute_pipeline, rbtrend_draw_list, rbtrend_exposure, rbtrend_framebuffer, rbtrend_gpu_timer, rbtrend_graphics_pipeline, rbtrend_index_pool, rbtrend_instance, rbtrend_lit_environment, rbtrend_lit_scene, rbtrend_material, rbtrend_render_pass, rbtrend_sampler, rbtrend_shader, rbtrend_swap_chain, rbtrend_texture2d, rbtrend_texture3d, rbtrend_texture_view, rbtrend_tlas, rbtrend_vertex_pool.

#include "rbtrend.h"
#include "archive/rbtarchive.h"
#include "audio/rbtaudio.h"
#include "audio/rbtaudiocues.h"
#include "jobs/rbtjobs.h"
#include "math/rbtmathbvh.h"
#include "math/rbtmathpoly.h"
#include "sync/rbtsyncnet.h"
#include "text/rbttext.h"
#include "rbtmath.h"
#include "misuse/misuse_factories.h"
#include <stdio.h>
#include <string.h>

#if defined(_MSC_VER) && _DEBUG
#include <crtdbg.h>
#endif

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define MISUSE_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define MISUSE_SANITIZED 1
#endif
#endif
#ifndef MISUSE_SANITIZED
#define MISUSE_SANITIZED 0
#endif

static int failures = 0;

static void expect_error(const char *name, RBT_RESULT result)
{
    if (result == RBT_RESULT_OK)
    {
        printf("FAIL %s: returned RBT_RESULT_OK for zeroes\n", name);
        failures++;
    }
}

#if _DEBUG && !MISUSE_SANITIZED
static int destroyed_calls = 0;

static void factory_missing(const char *name)
{
    printf("FAIL %s: its factory made no handle to destroy\n", name);
    failures++;
}

static void expect_refused(const char *name, RBT_RESULT result)
{
    destroyed_calls++;
    if (result == RBT_RESULT_OK)
    {
        printf("FAIL %s: returned RBT_RESULT_OK for a destroyed handle\n", name);
        failures++;
    }
}

// A getter given a destroyed handle returns what it returns for NULL.
static void expect_null_fallback(const char *name, const void *got, const void *for_null, size_t size)
{
    destroyed_calls++;
    if (memcmp(got, for_null, size) != 0)
    {
        printf("FAIL %s: a destroyed handle returned other than NULL's fallback\n", name);
        failures++;
    }
}

static void calls_with_destroyed_handles(void)
{
    {
        rbtarchive_codecs dead = dead_rbtarchive_codecs();
        if (!dead)
            factory_missing("rbtarchive_codecs_add");
        else
            expect_refused("rbtarchive_codecs_add", rbtarchive_codecs_add(dead, (const RBTARCHIVE_CODEC *){0}));
    }
    {
        rbtarchive_codecs dead = dead_rbtarchive_codecs();
        if (!dead)
            factory_missing("rbtarchive_codecs_add_deflate");
        else
            expect_refused("rbtarchive_codecs_add_deflate", rbtarchive_codecs_add_deflate(dead));
    }
    {
        rbtarchive_codecs dead = dead_rbtarchive_codecs();
        if (!dead)
            factory_missing("rbtarchive_codecs_compress");
        else
            expect_refused("rbtarchive_codecs_compress", rbtarchive_codecs_compress(dead, (uint16_t){0}, (int32_t){0}, (const void *){0}, (uint64_t){0}, (void *){0}, (uint64_t){0}, (uint64_t *){0}, (RBTARCHIVE_ERROR *){0}));
    }
    {
        rbtarchive_codecs dead = dead_rbtarchive_codecs();
        if (!dead)
            factory_missing("rbtarchive_codecs_compress_bound");
        else
            expect_refused("rbtarchive_codecs_compress_bound", rbtarchive_codecs_compress_bound(dead, (uint16_t){0}, (uint64_t){0}, (uint64_t *){0}));
    }
    {
        rbtarchive_codecs dead = dead_rbtarchive_codecs();
        if (!dead)
            factory_missing("rbtarchive_codecs_destroy");
        else
        {
            rbtarchive_codecs_destroy(dead);
            destroyed_calls++;
        }
    }
    {
        _Bool for_null = rbtarchive_codecs_has((rbtarchive_codecs){0}, (uint16_t){0});
        rbtarchive_codecs dead = dead_rbtarchive_codecs();
        if (!dead)
            factory_missing("rbtarchive_codecs_has");
        else
        {
            _Bool got = rbtarchive_codecs_has(dead, (uint16_t){0});
            expect_null_fallback("rbtarchive_codecs_has", &got, &for_null, sizeof(got));
        }
    }
    {
        rbtarchive_entry_reader dead = dead_rbtarchive_entry_reader();
        if (!dead)
            factory_missing("rbtarchive_entry_reader_destroy");
        else
        {
            rbtarchive_entry_reader_destroy(dead);
            destroyed_calls++;
        }
    }
    {
        rbtarchive_entry_reader dead = dead_rbtarchive_entry_reader();
        if (!dead)
            factory_missing("rbtarchive_entry_reader_read");
        else
            expect_refused("rbtarchive_entry_reader_read", rbtarchive_entry_reader_read(dead, (void *){0}, (uint64_t){0}, (uint64_t *){0}, (RBTARCHIVE_ERROR *){0}));
    }
    {
        rbtarchive_reader dead = dead_rbtarchive_reader();
        if (!dead)
            factory_missing("rbtarchive_reader_destroy");
        else
        {
            rbtarchive_reader_destroy(dead);
            destroyed_calls++;
        }
    }
    {
        rbtarchive_reader dead = dead_rbtarchive_reader();
        if (!dead)
            factory_missing("rbtarchive_reader_find");
        else
            expect_refused("rbtarchive_reader_find", rbtarchive_reader_find(dead, (const char *){0}, (uint32_t *){0}));
    }
    {
        rbtarchive_reader dead = dead_rbtarchive_reader();
        if (!dead)
            factory_missing("rbtarchive_reader_get_comment");
        else
            expect_refused("rbtarchive_reader_get_comment", rbtarchive_reader_get_comment(dead, (const void **){0}, (uint16_t *){0}));
    }
    {
        rbtarchive_reader dead = dead_rbtarchive_reader();
        if (!dead)
            factory_missing("rbtarchive_reader_get_data_offset");
        else
            expect_refused("rbtarchive_reader_get_data_offset", rbtarchive_reader_get_data_offset(dead, (uint32_t){0}, (uint64_t *){0}, (RBTARCHIVE_ERROR *){0}));
    }
    {
        rbtarchive_reader dead = dead_rbtarchive_reader();
        if (!dead)
            factory_missing("rbtarchive_reader_get_entry");
        else
            expect_refused("rbtarchive_reader_get_entry", rbtarchive_reader_get_entry(dead, (uint32_t){0}, (RBTARCHIVE_ENTRY_INFO *){0}));
    }
    {
        uint32_t for_null = rbtarchive_reader_get_entry_count((rbtarchive_reader){0});
        rbtarchive_reader dead = dead_rbtarchive_reader();
        if (!dead)
            factory_missing("rbtarchive_reader_get_entry_count");
        else
        {
            uint32_t got = rbtarchive_reader_get_entry_count(dead);
            expect_null_fallback("rbtarchive_reader_get_entry_count", &got, &for_null, sizeof(got));
        }
    }
    {
        uint64_t for_null = rbtarchive_reader_get_offset_shift((rbtarchive_reader){0});
        rbtarchive_reader dead = dead_rbtarchive_reader();
        if (!dead)
            factory_missing("rbtarchive_reader_get_offset_shift");
        else
        {
            uint64_t got = rbtarchive_reader_get_offset_shift(dead);
            expect_null_fallback("rbtarchive_reader_get_offset_shift", &got, &for_null, sizeof(got));
        }
    }
    {
        rbtarchive_reader dead = dead_rbtarchive_reader();
        if (!dead)
            factory_missing("rbtarchive_reader_open_entry");
        else
            expect_refused("rbtarchive_reader_open_entry", rbtarchive_reader_open_entry(dead, (uint32_t){0}, (rbtarchive_entry_reader *){0}, (RBTARCHIVE_ERROR *){0}));
    }
    {
        rbtarchive_reader dead = dead_rbtarchive_reader();
        if (!dead)
            factory_missing("rbtarchive_reader_read_entry");
        else
            expect_refused("rbtarchive_reader_read_entry", rbtarchive_reader_read_entry(dead, (uint32_t){0}, (void *){0}, (uint64_t){0}, (RBTARCHIVE_ERROR *){0}));
    }
    {
        rbtarchive_writer dead = dead_rbtarchive_writer();
        if (!dead)
            factory_missing("rbtarchive_writer_add_entry");
        else
            expect_refused("rbtarchive_writer_add_entry", rbtarchive_writer_add_entry(dead, (const RBTARCHIVE_ENTRY_DESC *){0}, (const void *){0}, (uint64_t){0}, (RBTARCHIVE_ERROR *){0}));
    }
    {
        rbtarchive_writer dead = dead_rbtarchive_writer();
        if (!dead)
            factory_missing("rbtarchive_writer_add_raw_entry");
        else
            expect_refused("rbtarchive_writer_add_raw_entry", rbtarchive_writer_add_raw_entry(dead, (const RBTARCHIVE_ENTRY_DESC *){0}, (const void *){0}, (uint64_t){0}, (uint64_t){0}, (uint32_t){0}, (RBTARCHIVE_ERROR *){0}));
    }
    {
        rbtarchive_writer dead = dead_rbtarchive_writer();
        if (!dead)
            factory_missing("rbtarchive_writer_begin_entry");
        else
            expect_refused("rbtarchive_writer_begin_entry", rbtarchive_writer_begin_entry(dead, (const RBTARCHIVE_ENTRY_DESC *){0}, (RBTARCHIVE_ERROR *){0}));
    }
    {
        rbtarchive_writer dead = dead_rbtarchive_writer();
        if (!dead)
            factory_missing("rbtarchive_writer_destroy");
        else
        {
            rbtarchive_writer_destroy(dead);
            destroyed_calls++;
        }
    }
    {
        rbtarchive_writer dead = dead_rbtarchive_writer();
        if (!dead)
            factory_missing("rbtarchive_writer_end_entry");
        else
            expect_refused("rbtarchive_writer_end_entry", rbtarchive_writer_end_entry(dead, (RBTARCHIVE_ERROR *){0}));
    }
    {
        rbtarchive_writer dead = dead_rbtarchive_writer();
        if (!dead)
            factory_missing("rbtarchive_writer_finish");
        else
            expect_refused("rbtarchive_writer_finish", rbtarchive_writer_finish(dead, (RBTARCHIVE_ERROR *){0}));
    }
    {
        uint64_t for_null = rbtarchive_writer_get_offset((rbtarchive_writer){0});
        rbtarchive_writer dead = dead_rbtarchive_writer();
        if (!dead)
            factory_missing("rbtarchive_writer_get_offset");
        else
        {
            uint64_t got = rbtarchive_writer_get_offset(dead);
            expect_null_fallback("rbtarchive_writer_get_offset", &got, &for_null, sizeof(got));
        }
    }
    {
        rbtarchive_writer dead = dead_rbtarchive_writer();
        if (!dead)
            factory_missing("rbtarchive_writer_remove_entry");
        else
            expect_refused("rbtarchive_writer_remove_entry", rbtarchive_writer_remove_entry(dead, (const char *){0}, (RBTARCHIVE_ERROR *){0}));
    }
    {
        rbtarchive_writer dead = dead_rbtarchive_writer();
        if (!dead)
            factory_missing("rbtarchive_writer_set_comment");
        else
            expect_refused("rbtarchive_writer_set_comment", rbtarchive_writer_set_comment(dead, (const void *){0}, (uint16_t){0}, (RBTARCHIVE_ERROR *){0}));
    }
    {
        rbtarchive_writer dead = dead_rbtarchive_writer();
        if (!dead)
            factory_missing("rbtarchive_writer_write_entry_data");
        else
            expect_refused("rbtarchive_writer_write_entry_data", rbtarchive_writer_write_entry_data(dead, (const void *){0}, (uint64_t){0}, (RBTARCHIVE_ERROR *){0}));
    }
    {
        rbtaudio_mixer dead = dead_rbtaudio_mixer();
        if (!dead)
            factory_missing("rbtaudio_attenuation_create");
        else
            expect_refused("rbtaudio_attenuation_create", rbtaudio_attenuation_create(dead, (const RBTAUDIO_ATTENUATION_DESC *){0}, (RBTAUDIO_ATTENUATION *){0}, (RBTAUDIO_ERROR *){0}));
    }
    {
        rbtaudio_mixer dead = dead_rbtaudio_mixer();
        if (!dead)
            factory_missing("rbtaudio_attenuation_destroy");
        else
        {
            rbtaudio_attenuation_destroy(dead, (RBTAUDIO_ATTENUATION){0});
            destroyed_calls++;
        }
    }
    {
        rbtaudio_mixer dead = dead_rbtaudio_mixer();
        if (!dead)
            factory_missing("rbtaudio_bus_create");
        else
            expect_refused("rbtaudio_bus_create", rbtaudio_bus_create(dead, (const RBTAUDIO_BUS_DESC *){0}, (RBTAUDIO_BUS *){0}, (RBTAUDIO_ERROR *){0}));
    }
    {
        rbtaudio_mixer dead = dead_rbtaudio_mixer();
        if (!dead)
            factory_missing("rbtaudio_bus_destroy");
        else
        {
            rbtaudio_bus_destroy(dead, (RBTAUDIO_BUS){0});
            destroyed_calls++;
        }
    }
    {
        rbtaudio_mixer dead = dead_rbtaudio_mixer();
        if (!dead)
            factory_missing("rbtaudio_bus_set_gain");
        else
            expect_refused("rbtaudio_bus_set_gain", rbtaudio_bus_set_gain(dead, (RBTAUDIO_BUS){0}, (float){0}, (uint32_t){0}));
    }
    {
        rbtaudio_mixer dead = dead_rbtaudio_mixer();
        if (!dead)
            factory_missing("rbtaudio_bus_set_muted");
        else
            expect_refused("rbtaudio_bus_set_muted", rbtaudio_bus_set_muted(dead, (RBTAUDIO_BUS){0}, (_Bool){0}, (uint32_t){0}));
    }
    {
        rbtaudio_mixer dead = dead_rbtaudio_mixer();
        if (!dead)
            factory_missing("rbtaudio_bus_set_parent");
        else
            expect_refused("rbtaudio_bus_set_parent", rbtaudio_bus_set_parent(dead, (RBTAUDIO_BUS){0}, (RBTAUDIO_BUS){0}));
    }
    {
        rbtaudio_mixer dead = dead_rbtaudio_mixer();
        if (!dead)
            factory_missing("rbtaudio_bus_set_paused");
        else
            expect_refused("rbtaudio_bus_set_paused", rbtaudio_bus_set_paused(dead, (RBTAUDIO_BUS){0}, (_Bool){0}, (uint32_t){0}));
    }
    {
        rbtaudio_mixer dead = dead_rbtaudio_mixer();
        if (!dead)
            factory_missing("rbtaudio_bus_set_rate");
        else
            expect_refused("rbtaudio_bus_set_rate", rbtaudio_bus_set_rate(dead, (RBTAUDIO_BUS){0}, (float){0}, (uint32_t){0}));
    }
    {
        rbtaudio_mixer dead = dead_rbtaudio_mixer();
        if (!dead)
            factory_missing("rbtaudio_concurrency_create");
        else
            expect_refused("rbtaudio_concurrency_create", rbtaudio_concurrency_create(dead, (const RBTAUDIO_CONCURRENCY_DESC *){0}, (RBTAUDIO_CONCURRENCY *){0}, (RBTAUDIO_ERROR *){0}));
    }
    {
        rbtaudio_mixer dead = dead_rbtaudio_mixer();
        if (!dead)
            factory_missing("rbtaudio_concurrency_destroy");
        else
        {
            rbtaudio_concurrency_destroy(dead, (RBTAUDIO_CONCURRENCY){0});
            destroyed_calls++;
        }
    }
    {
        rbtaudio_cues dead = dead_rbtaudio_cues();
        if (!dead)
            factory_missing("rbtaudio_cues_clear_banks");
        else
        {
            rbtaudio_cues_clear_banks(dead);
            destroyed_calls++;
        }
    }
    {
        rbtaudio_cues dead = dead_rbtaudio_cues();
        if (!dead)
            factory_missing("rbtaudio_cues_destroy");
        else
        {
            rbtaudio_cues_destroy(dead);
            destroyed_calls++;
        }
    }
    {
        rbtaudio_cues dead = dead_rbtaudio_cues();
        if (!dead)
            factory_missing("rbtaudio_cues_find");
        else
            expect_refused("rbtaudio_cues_find", rbtaudio_cues_find(dead, (const char *){0}, (RBTAUDIO_CUE *){0}));
    }
    {
        RBTAUDIO_MODIFIER for_null = rbtaudio_cues_get_bus_ducking((rbtaudio_cues){0}, (uint32_t){0});
        rbtaudio_cues dead = dead_rbtaudio_cues();
        if (!dead)
            factory_missing("rbtaudio_cues_get_bus_ducking");
        else
        {
            RBTAUDIO_MODIFIER got = rbtaudio_cues_get_bus_ducking(dead, (uint32_t){0});
            expect_null_fallback("rbtaudio_cues_get_bus_ducking", &got, &for_null, sizeof(got));
        }
    }
    {
        rbtaudio_cues dead = dead_rbtaudio_cues();
        if (!dead)
            factory_missing("rbtaudio_cues_get_info");
        else
            expect_refused("rbtaudio_cues_get_info", rbtaudio_cues_get_info(dead, (RBTAUDIO_CUE){0}, (RBTAUDIO_CUE_INFO *){0}));
    }
    {
        _Bool for_null = rbtaudio_cues_is_playing((rbtaudio_cues){0}, (RBTAUDIO_VOICE){0});
        rbtaudio_cues dead = dead_rbtaudio_cues();
        if (!dead)
            factory_missing("rbtaudio_cues_is_playing");
        else
        {
            _Bool got = rbtaudio_cues_is_playing(dead, (RBTAUDIO_VOICE){0});
            expect_null_fallback("rbtaudio_cues_is_playing", &got, &for_null, sizeof(got));
        }
    }
    {
        rbtaudio_cues dead = dead_rbtaudio_cues();
        if (!dead)
            factory_missing("rbtaudio_cues_load_bank");
        else
            expect_refused("rbtaudio_cues_load_bank", rbtaudio_cues_load_bank(dead, (const RBTAUDIO_BANK_SOURCE *){0}, (RBTAUDIO_ERROR *){0}));
    }
    {
        rbtaudio_cues dead = dead_rbtaudio_cues();
        if (!dead)
            factory_missing("rbtaudio_cues_load_zip_bank");
        else
            expect_refused("rbtaudio_cues_load_zip_bank", rbtaudio_cues_load_zip_bank(dead, (const RBTARCHIVE_SOURCE *){0}, (RBTAUDIO_ERROR *){0}));
    }
    {
        rbtaudio_cues dead = dead_rbtaudio_cues();
        if (!dead)
            factory_missing("rbtaudio_cues_play");
        else
            expect_refused("rbtaudio_cues_play", rbtaudio_cues_play(dead, (RBTAUDIO_CUE){0}, (const RBTAUDIO_CUE_PLAY_DESC *){0}, (RBTAUDIO_VOICE *){0}));
    }
    {
        rbtaudio_cues dead = dead_rbtaudio_cues();
        if (!dead)
            factory_missing("rbtaudio_cues_set_bus_muted");
        else
            expect_refused("rbtaudio_cues_set_bus_muted", rbtaudio_cues_set_bus_muted(dead, (uint32_t){0}, (_Bool){0}));
    }
    {
        rbtaudio_cues dead = dead_rbtaudio_cues();
        if (!dead)
            factory_missing("rbtaudio_cues_set_enabled");
        else
            expect_refused("rbtaudio_cues_set_enabled", rbtaudio_cues_set_enabled(dead, (RBTAUDIO_CUE){0}, (_Bool){0}));
    }
    {
        rbtaudio_cues dead = dead_rbtaudio_cues();
        if (!dead)
            factory_missing("rbtaudio_cues_set_sample_rate");
        else
        {
            rbtaudio_cues_set_sample_rate(dead, (uint32_t){0});
            destroyed_calls++;
        }
    }
    {
        rbtaudio_cues dead = dead_rbtaudio_cues();
        if (!dead)
            factory_missing("rbtaudio_cues_start");
        else
            expect_refused("rbtaudio_cues_start", rbtaudio_cues_start(dead, (RBTAUDIO_CUE){0}, (const RBTAUDIO_CUE_PLAY_DESC *){0}, (RBTAUDIO_VOICE *){0}));
    }
    {
        rbtaudio_cues dead = dead_rbtaudio_cues();
        if (!dead)
            factory_missing("rbtaudio_cues_stop");
        else
        {
            rbtaudio_cues_stop(dead, (RBTAUDIO_VOICE){0}, (_Bool){0});
            destroyed_calls++;
        }
    }
    {
        rbtaudio_cues dead = dead_rbtaudio_cues();
        if (!dead)
            factory_missing("rbtaudio_cues_update");
        else
        {
            rbtaudio_cues_update(dead);
            destroyed_calls++;
        }
    }
    {
        rbtaudio_mixer dead = dead_rbtaudio_mixer();
        if (!dead)
            factory_missing("rbtaudio_mixer_collect");
        else
        {
            rbtaudio_mixer_collect(dead);
            destroyed_calls++;
        }
    }
    {
        rbtaudio_mixer dead = dead_rbtaudio_mixer();
        if (!dead)
            factory_missing("rbtaudio_mixer_destroy");
        else
        {
            rbtaudio_mixer_destroy(dead);
            destroyed_calls++;
        }
    }
    {
        RBTAUDIO_BUS for_null = rbtaudio_mixer_get_master_bus((rbtaudio_mixer){0});
        rbtaudio_mixer dead = dead_rbtaudio_mixer();
        if (!dead)
            factory_missing("rbtaudio_mixer_get_master_bus");
        else
        {
            RBTAUDIO_BUS got = rbtaudio_mixer_get_master_bus(dead);
            expect_null_fallback("rbtaudio_mixer_get_master_bus", &got, &for_null, sizeof(got));
        }
    }
    {
        rbtaudio_mixer dead = dead_rbtaudio_mixer();
        if (!dead)
            factory_missing("rbtaudio_mixer_render");
        else
        {
            rbtaudio_mixer_render(dead, (void *){0}, (uint32_t){0}, (const CRAUDIO_CALLBACK_INFO *){0});
            destroyed_calls++;
        }
    }
    {
        rbtaudio_mixer dead = dead_rbtaudio_mixer();
        if (!dead)
            factory_missing("rbtaudio_mixer_set_emitters");
        else
            expect_refused("rbtaudio_mixer_set_emitters", rbtaudio_mixer_set_emitters(dead, (const RBTAUDIO_VOICE *){0}, (const RBTAUDIO_EMITTER *){0}, (uint32_t){0}, (RBTAUDIO_ERROR *){0}));
    }
    {
        rbtaudio_mixer dead = dead_rbtaudio_mixer();
        if (!dead)
            factory_missing("rbtaudio_mixer_set_format");
        else
            expect_refused("rbtaudio_mixer_set_format", rbtaudio_mixer_set_format(dead, (const RBTAUDIO_FORMAT *){0}, (uint32_t){0}, (RBTAUDIO_ERROR *){0}));
    }
    {
        rbtaudio_mixer dead = dead_rbtaudio_mixer();
        if (!dead)
            factory_missing("rbtaudio_mixer_set_listeners");
        else
            expect_refused("rbtaudio_mixer_set_listeners", rbtaudio_mixer_set_listeners(dead, (const RBTAUDIO_LISTENER *){0}, (uint32_t){0}, (RBTAUDIO_ERROR *){0}));
    }
    {
        rbtaudio_mixer dead = dead_rbtaudio_mixer();
        if (!dead)
            factory_missing("rbtaudio_mixer_update");
        else
            expect_refused("rbtaudio_mixer_update", rbtaudio_mixer_update(dead, (RBTAUDIO_ERROR *){0}));
    }
    {
        rbtaudio_mixer dead = dead_rbtaudio_mixer();
        if (!dead)
            factory_missing("rbtaudio_modifier_create");
        else
            expect_refused("rbtaudio_modifier_create", rbtaudio_modifier_create(dead, (const RBTAUDIO_MODIFIER_DESC *){0}, (RBTAUDIO_MODIFIER *){0}, (RBTAUDIO_ERROR *){0}));
    }
    {
        rbtaudio_mixer dead = dead_rbtaudio_mixer();
        if (!dead)
            factory_missing("rbtaudio_modifier_destroy");
        else
        {
            rbtaudio_modifier_destroy(dead, (RBTAUDIO_MODIFIER){0});
            destroyed_calls++;
        }
    }
    {
        rbtaudio_mixer dead = dead_rbtaudio_mixer();
        if (!dead)
            factory_missing("rbtaudio_modifier_set_active");
        else
            expect_refused("rbtaudio_modifier_set_active", rbtaudio_modifier_set_active(dead, (RBTAUDIO_MODIFIER){0}, (_Bool){0}));
    }
    {
        rbtaudio_mixer dead = dead_rbtaudio_mixer();
        if (!dead)
            factory_missing("rbtaudio_play");
        else
            expect_refused("rbtaudio_play", rbtaudio_play(dead, (RBTAUDIO_SOUND){0}, (const RBTAUDIO_PLAY_DESC *){0}, (RBTAUDIO_VOICE *){0}));
    }
    {
        rbtaudio_mixer dead = dead_rbtaudio_mixer();
        if (!dead)
            factory_missing("rbtaudio_sound_create");
        else
            expect_refused("rbtaudio_sound_create", rbtaudio_sound_create(dead, (const RBTAUDIO_SOUND_DESC *){0}, (RBTAUDIO_SOUND *){0}, (RBTAUDIO_ERROR *){0}));
    }
    {
        rbtaudio_mixer dead = dead_rbtaudio_mixer();
        if (!dead)
            factory_missing("rbtaudio_sound_destroy");
        else
        {
            rbtaudio_sound_destroy(dead, (RBTAUDIO_SOUND){0});
            destroyed_calls++;
        }
    }
    {
        uint64_t for_null = rbtaudio_sound_get_underrun_frames((rbtaudio_mixer){0}, (RBTAUDIO_SOUND){0});
        rbtaudio_mixer dead = dead_rbtaudio_mixer();
        if (!dead)
            factory_missing("rbtaudio_sound_get_underrun_frames");
        else
        {
            uint64_t got = rbtaudio_sound_get_underrun_frames(dead, (RBTAUDIO_SOUND){0});
            expect_null_fallback("rbtaudio_sound_get_underrun_frames", &got, &for_null, sizeof(got));
        }
    }
    {
        _Bool for_null = rbtaudio_voice_is_folded((rbtaudio_mixer){0}, (RBTAUDIO_VOICE){0});
        rbtaudio_mixer dead = dead_rbtaudio_mixer();
        if (!dead)
            factory_missing("rbtaudio_voice_is_folded");
        else
        {
            _Bool got = rbtaudio_voice_is_folded(dead, (RBTAUDIO_VOICE){0});
            expect_null_fallback("rbtaudio_voice_is_folded", &got, &for_null, sizeof(got));
        }
    }
    {
        _Bool for_null = rbtaudio_voice_is_live((rbtaudio_mixer){0}, (RBTAUDIO_VOICE){0});
        rbtaudio_mixer dead = dead_rbtaudio_mixer();
        if (!dead)
            factory_missing("rbtaudio_voice_is_live");
        else
        {
            _Bool got = rbtaudio_voice_is_live(dead, (RBTAUDIO_VOICE){0});
            expect_null_fallback("rbtaudio_voice_is_live", &got, &for_null, sizeof(got));
        }
    }
    {
        rbtaudio_mixer dead = dead_rbtaudio_mixer();
        if (!dead)
            factory_missing("rbtaudio_voice_set_direction");
        else
            expect_refused("rbtaudio_voice_set_direction", rbtaudio_voice_set_direction(dead, (RBTAUDIO_VOICE){0}, (const RBTAUDIO_DIRECTION *){0}));
    }
    {
        rbtaudio_mixer dead = dead_rbtaudio_mixer();
        if (!dead)
            factory_missing("rbtaudio_voice_set_gain");
        else
            expect_refused("rbtaudio_voice_set_gain", rbtaudio_voice_set_gain(dead, (RBTAUDIO_VOICE){0}, (float){0}));
    }
    {
        rbtaudio_mixer dead = dead_rbtaudio_mixer();
        if (!dead)
            factory_missing("rbtaudio_voice_set_rate");
        else
            expect_refused("rbtaudio_voice_set_rate", rbtaudio_voice_set_rate(dead, (RBTAUDIO_VOICE){0}, (float){0}));
    }
    {
        rbtaudio_mixer dead = dead_rbtaudio_mixer();
        if (!dead)
            factory_missing("rbtaudio_voice_stop");
        else
            expect_refused("rbtaudio_voice_stop", rbtaudio_voice_stop(dead, (RBTAUDIO_VOICE){0}, (uint32_t){0}));
    }
    {
        rbtjobs_pool dead = dead_rbtjobs_pool();
        if (!dead)
            factory_missing("rbtjobs_pool_destroy");
        else
        {
            rbtjobs_pool_destroy(dead);
            destroyed_calls++;
        }
    }
    {
        rbtjobs_pool dead = dead_rbtjobs_pool();
        if (!dead)
            factory_missing("rbtjobs_pool_get_scheduler");
        else
            expect_refused("rbtjobs_pool_get_scheduler", rbtjobs_pool_get_scheduler(dead, (RBTJOBS_SCHEDULER *){0}));
    }
    {
        _Bool for_null = rbtmath_box_bvh_cast((rbtmath_box_bvh){0}, (const RBTMATH_RAY *){0}, (const RBTMATH_CAST_DESC *){0}, (RBTMATH_BOX_HIT *){0});
        rbtmath_box_bvh dead = dead_rbtmath_box_bvh();
        if (!dead)
            factory_missing("rbtmath_box_bvh_cast");
        else
        {
            _Bool got = rbtmath_box_bvh_cast(dead, (const RBTMATH_RAY *){0}, (const RBTMATH_CAST_DESC *){0}, (RBTMATH_BOX_HIT *){0});
            expect_null_fallback("rbtmath_box_bvh_cast", &got, &for_null, sizeof(got));
        }
    }
    {
        uint32_t for_null = rbtmath_box_bvh_cast_all((rbtmath_box_bvh){0}, (const RBTMATH_RAY *){0}, (const RBTMATH_CAST_DESC *){0}, (RBTMATH_BOX_HIT *){0}, (uint32_t){0});
        rbtmath_box_bvh dead = dead_rbtmath_box_bvh();
        if (!dead)
            factory_missing("rbtmath_box_bvh_cast_all");
        else
        {
            uint32_t got = rbtmath_box_bvh_cast_all(dead, (const RBTMATH_RAY *){0}, (const RBTMATH_CAST_DESC *){0}, (RBTMATH_BOX_HIT *){0}, (uint32_t){0});
            expect_null_fallback("rbtmath_box_bvh_cast_all", &got, &for_null, sizeof(got));
        }
    }
    {
        rbtmath_box_bvh dead = dead_rbtmath_box_bvh();
        if (!dead)
            factory_missing("rbtmath_box_bvh_destroy");
        else
        {
            rbtmath_box_bvh_destroy(dead);
            destroyed_calls++;
        }
    }
    {
        double for_null = rbtmath_box_bvh_get_cost((rbtmath_box_bvh){0});
        rbtmath_box_bvh dead = dead_rbtmath_box_bvh();
        if (!dead)
            factory_missing("rbtmath_box_bvh_get_cost");
        else
        {
            double got = rbtmath_box_bvh_get_cost(dead);
            expect_null_fallback("rbtmath_box_bvh_get_cost", &got, &for_null, sizeof(got));
        }
    }
    {
        uint32_t for_null = rbtmath_box_bvh_overlap_all((rbtmath_box_bvh){0}, (const RBTMATH_SHAPE *){0}, (const RBTMATH_CAST_DESC *){0}, (uint32_t *){0}, (uint32_t){0});
        rbtmath_box_bvh dead = dead_rbtmath_box_bvh();
        if (!dead)
            factory_missing("rbtmath_box_bvh_overlap_all");
        else
        {
            uint32_t got = rbtmath_box_bvh_overlap_all(dead, (const RBTMATH_SHAPE *){0}, (const RBTMATH_CAST_DESC *){0}, (uint32_t *){0}, (uint32_t){0});
            expect_null_fallback("rbtmath_box_bvh_overlap_all", &got, &for_null, sizeof(got));
        }
    }
    {
        _Bool for_null = rbtmath_box_bvh_overlaps_shape((rbtmath_box_bvh){0}, (const RBTMATH_SHAPE *){0}, (const RBTMATH_CAST_DESC *){0});
        rbtmath_box_bvh dead = dead_rbtmath_box_bvh();
        if (!dead)
            factory_missing("rbtmath_box_bvh_overlaps_shape");
        else
        {
            _Bool got = rbtmath_box_bvh_overlaps_shape(dead, (const RBTMATH_SHAPE *){0}, (const RBTMATH_CAST_DESC *){0});
            expect_null_fallback("rbtmath_box_bvh_overlaps_shape", &got, &for_null, sizeof(got));
        }
    }
    {
        rbtmath_box_bvh dead = dead_rbtmath_box_bvh();
        if (!dead)
            factory_missing("rbtmath_box_bvh_rebuild");
        else
        {
            rbtmath_box_bvh_rebuild(dead);
            destroyed_calls++;
        }
    }
    {
        rbtmath_box_bvh dead = dead_rbtmath_box_bvh();
        if (!dead)
            factory_missing("rbtmath_box_bvh_refit");
        else
        {
            rbtmath_box_bvh_refit(dead);
            destroyed_calls++;
        }
    }
    {
        rbtmath_box_bvh dead = dead_rbtmath_box_bvh();
        if (!dead)
            factory_missing("rbtmath_box_bvh_set_boxes");
        else
            expect_refused("rbtmath_box_bvh_set_boxes", rbtmath_box_bvh_set_boxes(dead, (const uint32_t *){0}, (const RBTMATH_BVH_BOX *){0}, (uint32_t){0}, (RBTMATH_ERROR *){0}));
    }
    {
        _Bool for_null = rbtmath_box_bvh_sweep((rbtmath_box_bvh){0}, (const RBTMATH_SWEEP *){0}, (const RBTMATH_CAST_DESC *){0}, (RBTMATH_BOX_HIT *){0});
        rbtmath_box_bvh dead = dead_rbtmath_box_bvh();
        if (!dead)
            factory_missing("rbtmath_box_bvh_sweep");
        else
        {
            _Bool got = rbtmath_box_bvh_sweep(dead, (const RBTMATH_SWEEP *){0}, (const RBTMATH_CAST_DESC *){0}, (RBTMATH_BOX_HIT *){0});
            expect_null_fallback("rbtmath_box_bvh_sweep", &got, &for_null, sizeof(got));
        }
    }
    {
        uint32_t for_null = rbtmath_box_bvh_sweep_all((rbtmath_box_bvh){0}, (const RBTMATH_SWEEP *){0}, (const RBTMATH_CAST_DESC *){0}, (RBTMATH_BOX_HIT *){0}, (uint32_t){0});
        rbtmath_box_bvh dead = dead_rbtmath_box_bvh();
        if (!dead)
            factory_missing("rbtmath_box_bvh_sweep_all");
        else
        {
            uint32_t got = rbtmath_box_bvh_sweep_all(dead, (const RBTMATH_SWEEP *){0}, (const RBTMATH_CAST_DESC *){0}, (RBTMATH_BOX_HIT *){0}, (uint32_t){0});
            expect_null_fallback("rbtmath_box_bvh_sweep_all", &got, &for_null, sizeof(got));
        }
    }
    {
        _Bool for_null = rbtmath_instance_bvh_cast((rbtmath_instance_bvh){0}, (const RBTMATH_RAY *){0}, (const RBTMATH_CAST_DESC *){0}, (RBTMATH_INSTANCE_HIT *){0});
        rbtmath_instance_bvh dead = dead_rbtmath_instance_bvh();
        if (!dead)
            factory_missing("rbtmath_instance_bvh_cast");
        else
        {
            _Bool got = rbtmath_instance_bvh_cast(dead, (const RBTMATH_RAY *){0}, (const RBTMATH_CAST_DESC *){0}, (RBTMATH_INSTANCE_HIT *){0});
            expect_null_fallback("rbtmath_instance_bvh_cast", &got, &for_null, sizeof(got));
        }
    }
    {
        uint32_t for_null = rbtmath_instance_bvh_cast_all((rbtmath_instance_bvh){0}, (const RBTMATH_RAY *){0}, (const RBTMATH_CAST_DESC *){0}, (RBTMATH_INSTANCE_HIT *){0}, (uint32_t){0});
        rbtmath_instance_bvh dead = dead_rbtmath_instance_bvh();
        if (!dead)
            factory_missing("rbtmath_instance_bvh_cast_all");
        else
        {
            uint32_t got = rbtmath_instance_bvh_cast_all(dead, (const RBTMATH_RAY *){0}, (const RBTMATH_CAST_DESC *){0}, (RBTMATH_INSTANCE_HIT *){0}, (uint32_t){0});
            expect_null_fallback("rbtmath_instance_bvh_cast_all", &got, &for_null, sizeof(got));
        }
    }
    {
        rbtmath_instance_bvh dead = dead_rbtmath_instance_bvh();
        if (!dead)
            factory_missing("rbtmath_instance_bvh_destroy");
        else
        {
            rbtmath_instance_bvh_destroy(dead);
            destroyed_calls++;
        }
    }
    {
        RBTMATH_AABB for_null = rbtmath_instance_bvh_get_bounds((rbtmath_instance_bvh){0});
        rbtmath_instance_bvh dead = dead_rbtmath_instance_bvh();
        if (!dead)
            factory_missing("rbtmath_instance_bvh_get_bounds");
        else
        {
            RBTMATH_AABB got = rbtmath_instance_bvh_get_bounds(dead);
            expect_null_fallback("rbtmath_instance_bvh_get_bounds", &got, &for_null, sizeof(got));
        }
    }
    {
        double for_null = rbtmath_instance_bvh_get_cost((rbtmath_instance_bvh){0});
        rbtmath_instance_bvh dead = dead_rbtmath_instance_bvh();
        if (!dead)
            factory_missing("rbtmath_instance_bvh_get_cost");
        else
        {
            double got = rbtmath_instance_bvh_get_cost(dead);
            expect_null_fallback("rbtmath_instance_bvh_get_cost", &got, &for_null, sizeof(got));
        }
    }
    {
        uint32_t for_null = rbtmath_instance_bvh_get_instance_count((rbtmath_instance_bvh){0});
        rbtmath_instance_bvh dead = dead_rbtmath_instance_bvh();
        if (!dead)
            factory_missing("rbtmath_instance_bvh_get_instance_count");
        else
        {
            uint32_t got = rbtmath_instance_bvh_get_instance_count(dead);
            expect_null_fallback("rbtmath_instance_bvh_get_instance_count", &got, &for_null, sizeof(got));
        }
    }
    {
        uint32_t for_null = rbtmath_instance_bvh_overlap_all((rbtmath_instance_bvh){0}, (const RBTMATH_SHAPE *){0}, (const RBTMATH_CAST_DESC *){0}, (uint32_t *){0}, (uint32_t){0});
        rbtmath_instance_bvh dead = dead_rbtmath_instance_bvh();
        if (!dead)
            factory_missing("rbtmath_instance_bvh_overlap_all");
        else
        {
            uint32_t got = rbtmath_instance_bvh_overlap_all(dead, (const RBTMATH_SHAPE *){0}, (const RBTMATH_CAST_DESC *){0}, (uint32_t *){0}, (uint32_t){0});
            expect_null_fallback("rbtmath_instance_bvh_overlap_all", &got, &for_null, sizeof(got));
        }
    }
    {
        _Bool for_null = rbtmath_instance_bvh_overlaps_shape((rbtmath_instance_bvh){0}, (const RBTMATH_SHAPE *){0}, (const RBTMATH_CAST_DESC *){0});
        rbtmath_instance_bvh dead = dead_rbtmath_instance_bvh();
        if (!dead)
            factory_missing("rbtmath_instance_bvh_overlaps_shape");
        else
        {
            _Bool got = rbtmath_instance_bvh_overlaps_shape(dead, (const RBTMATH_SHAPE *){0}, (const RBTMATH_CAST_DESC *){0});
            expect_null_fallback("rbtmath_instance_bvh_overlaps_shape", &got, &for_null, sizeof(got));
        }
    }
    {
        rbtmath_instance_bvh dead = dead_rbtmath_instance_bvh();
        if (!dead)
            factory_missing("rbtmath_instance_bvh_rebuild");
        else
        {
            rbtmath_instance_bvh_rebuild(dead);
            destroyed_calls++;
        }
    }
    {
        rbtmath_instance_bvh dead = dead_rbtmath_instance_bvh();
        if (!dead)
            factory_missing("rbtmath_instance_bvh_refit");
        else
        {
            rbtmath_instance_bvh_refit(dead);
            destroyed_calls++;
        }
    }
    {
        rbtmath_instance_bvh dead = dead_rbtmath_instance_bvh();
        if (!dead)
            factory_missing("rbtmath_instance_bvh_set_instances");
        else
            expect_refused("rbtmath_instance_bvh_set_instances", rbtmath_instance_bvh_set_instances(dead, (const uint32_t *){0}, (const RBTMATH_BVH_INSTANCE *){0}, (uint32_t){0}, (RBTMATH_ERROR *){0}));
    }
    {
        _Bool for_null = rbtmath_instance_bvh_sweep((rbtmath_instance_bvh){0}, (const RBTMATH_SWEEP *){0}, (const RBTMATH_CAST_DESC *){0}, (RBTMATH_SWEEP_HIT *){0});
        rbtmath_instance_bvh dead = dead_rbtmath_instance_bvh();
        if (!dead)
            factory_missing("rbtmath_instance_bvh_sweep");
        else
        {
            _Bool got = rbtmath_instance_bvh_sweep(dead, (const RBTMATH_SWEEP *){0}, (const RBTMATH_CAST_DESC *){0}, (RBTMATH_SWEEP_HIT *){0});
            expect_null_fallback("rbtmath_instance_bvh_sweep", &got, &for_null, sizeof(got));
        }
    }
    {
        uint32_t for_null = rbtmath_instance_bvh_sweep_all((rbtmath_instance_bvh){0}, (const RBTMATH_SWEEP *){0}, (const RBTMATH_CAST_DESC *){0}, (RBTMATH_SWEEP_HIT *){0}, (uint32_t){0});
        rbtmath_instance_bvh dead = dead_rbtmath_instance_bvh();
        if (!dead)
            factory_missing("rbtmath_instance_bvh_sweep_all");
        else
        {
            uint32_t got = rbtmath_instance_bvh_sweep_all(dead, (const RBTMATH_SWEEP *){0}, (const RBTMATH_CAST_DESC *){0}, (RBTMATH_SWEEP_HIT *){0}, (uint32_t){0});
            expect_null_fallback("rbtmath_instance_bvh_sweep_all", &got, &for_null, sizeof(got));
        }
    }
    {
        _Bool for_null = rbtmath_mesh_bvh_cast((rbtmath_mesh_bvh){0}, (const RBTMATH_RAY *){0}, (RBTMATH_MESH_HIT *){0});
        rbtmath_mesh_bvh dead = dead_rbtmath_mesh_bvh();
        if (!dead)
            factory_missing("rbtmath_mesh_bvh_cast");
        else
        {
            _Bool got = rbtmath_mesh_bvh_cast(dead, (const RBTMATH_RAY *){0}, (RBTMATH_MESH_HIT *){0});
            expect_null_fallback("rbtmath_mesh_bvh_cast", &got, &for_null, sizeof(got));
        }
    }
    {
        rbtmath_mesh_bvh dead = dead_rbtmath_mesh_bvh();
        if (!dead)
            factory_missing("rbtmath_mesh_bvh_destroy");
        else
        {
            rbtmath_mesh_bvh_destroy(dead);
            destroyed_calls++;
        }
    }
    {
        RBTMATH_AABB for_null = rbtmath_mesh_bvh_get_bounds((rbtmath_mesh_bvh){0});
        rbtmath_mesh_bvh dead = dead_rbtmath_mesh_bvh();
        if (!dead)
            factory_missing("rbtmath_mesh_bvh_get_bounds");
        else
        {
            RBTMATH_AABB got = rbtmath_mesh_bvh_get_bounds(dead);
            expect_null_fallback("rbtmath_mesh_bvh_get_bounds", &got, &for_null, sizeof(got));
        }
    }
    {
        uint32_t for_null = rbtmath_mesh_bvh_get_triangle_count((rbtmath_mesh_bvh){0});
        rbtmath_mesh_bvh dead = dead_rbtmath_mesh_bvh();
        if (!dead)
            factory_missing("rbtmath_mesh_bvh_get_triangle_count");
        else
        {
            uint32_t got = rbtmath_mesh_bvh_get_triangle_count(dead);
            expect_null_fallback("rbtmath_mesh_bvh_get_triangle_count", &got, &for_null, sizeof(got));
        }
    }
    {
        _Bool for_null = rbtmath_mesh_bvh_overlaps_shape((rbtmath_mesh_bvh){0}, (const RbtDMat4 *){0}, (const RBTMATH_SHAPE *){0});
        rbtmath_mesh_bvh dead = dead_rbtmath_mesh_bvh();
        if (!dead)
            factory_missing("rbtmath_mesh_bvh_overlaps_shape");
        else
        {
            _Bool got = rbtmath_mesh_bvh_overlaps_shape(dead, (const RbtDMat4 *){0}, (const RBTMATH_SHAPE *){0});
            expect_null_fallback("rbtmath_mesh_bvh_overlaps_shape", &got, &for_null, sizeof(got));
        }
    }
    {
        _Bool for_null = rbtmath_mesh_bvh_overlaps_volume((rbtmath_mesh_bvh){0}, (const RbtDMat4 *){0}, (const RBTMATH_VOLUME *){0});
        rbtmath_mesh_bvh dead = dead_rbtmath_mesh_bvh();
        if (!dead)
            factory_missing("rbtmath_mesh_bvh_overlaps_volume");
        else
        {
            _Bool got = rbtmath_mesh_bvh_overlaps_volume(dead, (const RbtDMat4 *){0}, (const RBTMATH_VOLUME *){0});
            expect_null_fallback("rbtmath_mesh_bvh_overlaps_volume", &got, &for_null, sizeof(got));
        }
    }
    {
        _Bool for_null = rbtmath_mesh_bvh_sweep((rbtmath_mesh_bvh){0}, (const RbtDMat4 *){0}, (const RBTMATH_SWEEP *){0}, (RBTMATH_SWEEP_HIT *){0});
        rbtmath_mesh_bvh dead = dead_rbtmath_mesh_bvh();
        if (!dead)
            factory_missing("rbtmath_mesh_bvh_sweep");
        else
        {
            _Bool got = rbtmath_mesh_bvh_sweep(dead, (const RbtDMat4 *){0}, (const RBTMATH_SWEEP *){0}, (RBTMATH_SWEEP_HIT *){0});
            expect_null_fallback("rbtmath_mesh_bvh_sweep", &got, &for_null, sizeof(got));
        }
    }
    {
        _Bool for_null = rbtmath_mesh_bvh_within_volume((rbtmath_mesh_bvh){0}, (const RbtDMat4 *){0}, (const RBTMATH_VOLUME *){0});
        rbtmath_mesh_bvh dead = dead_rbtmath_mesh_bvh();
        if (!dead)
            factory_missing("rbtmath_mesh_bvh_within_volume");
        else
        {
            _Bool got = rbtmath_mesh_bvh_within_volume(dead, (const RbtDMat4 *){0}, (const RBTMATH_VOLUME *){0});
            expect_null_fallback("rbtmath_mesh_bvh_within_volume", &got, &for_null, sizeof(got));
        }
    }
    {
        rbtmath_poly dead = dead_rbtmath_poly();
        if (!dead)
            factory_missing("rbtmath_poly_destroy");
        else
        {
            rbtmath_poly_destroy(dead);
            destroyed_calls++;
        }
    }
    {
        RBTMATH_POLY_SET for_null = rbtmath_poly_get_set((rbtmath_poly){0});
        rbtmath_poly dead = dead_rbtmath_poly();
        if (!dead)
            factory_missing("rbtmath_poly_get_set");
        else
        {
            RBTMATH_POLY_SET got = rbtmath_poly_get_set(dead);
            expect_null_fallback("rbtmath_poly_get_set", &got, &for_null, sizeof(got));
        }
    }
    {
        rbtsync_session dead = dead_rbtsync_session();
        if (!dead)
            factory_missing("rbtsync_session_destroy");
        else
        {
            rbtsync_session_destroy(dead);
            destroyed_calls++;
        }
    }
    {
        rbtsync_session dead = dead_rbtsync_session();
        if (!dead)
            factory_missing("rbtsync_session_disconnect_peer");
        else
            expect_refused("rbtsync_session_disconnect_peer", rbtsync_session_disconnect_peer(dead, (RBTSYNC_PEER){0}));
    }
    {
        RBTSYNC_PEER for_null = rbtsync_session_get_local_peer((rbtsync_session){0});
        rbtsync_session dead = dead_rbtsync_session();
        if (!dead)
            factory_missing("rbtsync_session_get_local_peer");
        else
        {
            RBTSYNC_PEER got = rbtsync_session_get_local_peer(dead);
            expect_null_fallback("rbtsync_session_get_local_peer", &got, &for_null, sizeof(got));
        }
    }
    {
        rbtsync_session dead = dead_rbtsync_session();
        if (!dead)
            factory_missing("rbtsync_session_get_port");
        else
            expect_refused("rbtsync_session_get_port", rbtsync_session_get_port(dead, (uint32_t){0}, (uint16_t *){0}));
    }
    {
        rbtsync_session dead = dead_rbtsync_session();
        if (!dead)
            factory_missing("rbtsync_session_poll");
        else
            expect_refused("rbtsync_session_poll", rbtsync_session_poll(dead, (RBTSYNC_EVENT *){0}));
    }
    {
        rbtsync_session dead = dead_rbtsync_session();
        if (!dead)
            factory_missing("rbtsync_session_send");
        else
            expect_refused("rbtsync_session_send", rbtsync_session_send(dead, (uint32_t){0}, (uint64_t){0}, (const void *){0}, (uint32_t){0}));
    }
    {
        rbtsync_session dead = dead_rbtsync_session();
        if (!dead)
            factory_missing("rbtsync_session_send_snapshot");
        else
            expect_refused("rbtsync_session_send_snapshot", rbtsync_session_send_snapshot(dead, (RBTSYNC_PEER){0}, (const void *){0}, (uint64_t){0}));
    }
    {
        rbtsync_session dead = dead_rbtsync_session();
        if (!dead)
            factory_missing("rbtsync_session_wait");
        else
            expect_refused("rbtsync_session_wait", rbtsync_session_wait(dead, (uint32_t){0}));
    }
    {
        rbtsync_session dead = dead_rbtsync_session();
        if (!dead)
            factory_missing("rbtsync_session_wake");
        else
        {
            rbtsync_session_wake(dead);
            destroyed_calls++;
        }
    }
    {
        rbttext_atlas dead = dead_rbttext_atlas();
        if (!dead)
            factory_missing("rbttext_atlas_add_layout");
        else
            expect_refused("rbttext_atlas_add_layout", rbttext_atlas_add_layout(dead, (rbttext_layout){0}, (RBTTEXT_ERROR *){0}));
    }
    {
        rbttext_atlas dead = dead_rbttext_atlas();
        if (!dead)
            factory_missing("rbttext_atlas_destroy");
        else
        {
            rbttext_atlas_destroy(dead);
            destroyed_calls++;
        }
    }
    {
        rbttext_atlas dead = dead_rbttext_atlas();
        if (!dead)
            factory_missing("rbttext_atlas_find_glyph");
        else
            expect_refused("rbttext_atlas_find_glyph", rbttext_atlas_find_glyph(dead, (rbttext_face){0}, (uint32_t){0}, (RBTTEXT_ATLAS_GLYPH *){0}));
    }
    {
        rbttext_atlas dead = dead_rbttext_atlas();
        if (!dead)
            factory_missing("rbttext_atlas_get_info");
        else
            expect_refused("rbttext_atlas_get_info", rbttext_atlas_get_info(dead, (RBTTEXT_ATLAS_INFO *){0}));
    }
    {
        rbttext_atlas dead = dead_rbttext_atlas();
        if (!dead)
            factory_missing("rbttext_atlas_get_page");
        else
            expect_refused("rbttext_atlas_get_page", rbttext_atlas_get_page(dead, (uint32_t){0}, (const void **){0}));
    }
    {
        rbttext_atlas dead = dead_rbttext_atlas();
        if (!dead)
            factory_missing("rbttext_atlas_insert");
        else
            expect_refused("rbttext_atlas_insert", rbttext_atlas_insert(dead, (const RBTTEXT_GLYPH_GENERATE_DESC *){0}, (const void *){0}, (uint64_t){0}, (RBTTEXT_ATLAS_GLYPH *){0}, (RBTTEXT_ERROR *){0}));
    }
    {
        rbttext_atlas dead = dead_rbttext_atlas();
        if (!dead)
            factory_missing("rbttext_atlas_save");
        else
            expect_refused("rbttext_atlas_save", rbttext_atlas_save(dead, (const rbttext_face *){0}, (uint32_t){0}, (void *){0}, (uint64_t){0}, (uint64_t *){0}, (RBTTEXT_ERROR *){0}));
    }
    {
        rbttext_atlas dead = dead_rbttext_atlas();
        if (!dead)
            factory_missing("rbttext_atlas_take_dirty");
        else
            expect_refused("rbttext_atlas_take_dirty", rbttext_atlas_take_dirty(dead, (RBTTEXT_DIRTY_RECT *){0}, (uint32_t){0}, (uint32_t *){0}));
    }
    {
        rbttext_chain dead = dead_rbttext_chain();
        if (!dead)
            factory_missing("rbttext_chain_destroy");
        else
        {
            rbttext_chain_destroy(dead);
            destroyed_calls++;
        }
    }
    {
        rbttext_chain dead = dead_rbttext_chain();
        if (!dead)
            factory_missing("rbttext_chain_find_glyph");
        else
            expect_refused("rbttext_chain_find_glyph", rbttext_chain_find_glyph(dead, (uint32_t){0}, (uint32_t *){0}, (uint32_t *){0}));
    }
    {
        rbttext_face for_null = rbttext_chain_get_face((rbttext_chain){0}, (uint32_t){0});
        rbttext_chain dead = dead_rbttext_chain();
        if (!dead)
            factory_missing("rbttext_chain_get_face");
        else
        {
            rbttext_face got = rbttext_chain_get_face(dead, (uint32_t){0});
            expect_null_fallback("rbttext_chain_get_face", &got, &for_null, sizeof(got));
        }
    }
    {
        uint32_t for_null = rbttext_chain_get_face_count((rbttext_chain){0});
        rbttext_chain dead = dead_rbttext_chain();
        if (!dead)
            factory_missing("rbttext_chain_get_face_count");
        else
        {
            uint32_t got = rbttext_chain_get_face_count(dead);
            expect_null_fallback("rbttext_chain_get_face_count", &got, &for_null, sizeof(got));
        }
    }
    {
        rbttext_face dead = dead_rbttext_face();
        if (!dead)
            factory_missing("rbttext_face_destroy");
        else
        {
            rbttext_face_destroy(dead);
            destroyed_calls++;
        }
    }
    {
        rbttext_face dead = dead_rbttext_face();
        if (!dead)
            factory_missing("rbttext_face_find_glyph");
        else
            expect_refused("rbttext_face_find_glyph", rbttext_face_find_glyph(dead, (uint32_t){0}, (uint32_t *){0}));
    }
    {
        rbttext_face dead = dead_rbttext_face();
        if (!dead)
            factory_missing("rbttext_face_get_axis");
        else
            expect_refused("rbttext_face_get_axis", rbttext_face_get_axis(dead, (uint32_t){0}, (RBTTEXT_AXIS *){0}));
    }
    {
        uint32_t for_null = rbttext_face_get_axis_count((rbttext_face){0});
        rbttext_face dead = dead_rbttext_face();
        if (!dead)
            factory_missing("rbttext_face_get_axis_count");
        else
        {
            uint32_t got = rbttext_face_get_axis_count(dead);
            expect_null_fallback("rbttext_face_get_axis_count", &got, &for_null, sizeof(got));
        }
    }
    {
        rbttext_face dead = dead_rbttext_face();
        if (!dead)
            factory_missing("rbttext_face_get_glyph_strokes");
        else
            expect_refused("rbttext_face_get_glyph_strokes", rbttext_face_get_glyph_strokes(dead, (uint32_t){0}, (const RbtVec2 **){0}, (uint32_t *){0}, (const RBTTEXT_STROKE_POLYLINE **){0}, (uint32_t *){0}));
    }
    {
        rbttext_face dead = dead_rbttext_face();
        if (!dead)
            factory_missing("rbttext_face_get_metrics");
        else
            expect_refused("rbttext_face_get_metrics", rbttext_face_get_metrics(dead, (RBTTEXT_FACE_METRICS *){0}));
    }
    {
        rbttext_layout dead = dead_rbttext_layout();
        if (!dead)
            factory_missing("rbttext_layout_build");
        else
            expect_refused("rbttext_layout_build", rbttext_layout_build(dead, (const RBTTEXT_LAYOUT_DESC *){0}, (RBTTEXT_ERROR *){0}));
    }
    {
        rbttext_layout dead = dead_rbttext_layout();
        if (!dead)
            factory_missing("rbttext_layout_destroy");
        else
        {
            rbttext_layout_destroy(dead);
            destroyed_calls++;
        }
    }
    {
        rbttext_layout dead = dead_rbttext_layout();
        if (!dead)
            factory_missing("rbttext_layout_get_glyphs");
        else
            expect_refused("rbttext_layout_get_glyphs", rbttext_layout_get_glyphs(dead, (const RBTTEXT_GLYPH **){0}, (uint32_t *){0}));
    }
    {
        rbttext_layout dead = dead_rbttext_layout();
        if (!dead)
            factory_missing("rbttext_layout_get_info");
        else
            expect_refused("rbttext_layout_get_info", rbttext_layout_get_info(dead, (RBTTEXT_LAYOUT_INFO *){0}));
    }
    {
        rbttext_layout dead = dead_rbttext_layout();
        if (!dead)
            factory_missing("rbttext_layout_get_lines");
        else
            expect_refused("rbttext_layout_get_lines", rbttext_layout_get_lines(dead, (const RBTTEXT_LINE **){0}, (uint32_t *){0}));
    }
    {
        rbttext_layout dead = dead_rbttext_layout();
        if (!dead)
            factory_missing("rbttext_layout_get_runs");
        else
            expect_refused("rbttext_layout_get_runs", rbttext_layout_get_runs(dead, (const RBTTEXT_RUN **){0}, (uint32_t *){0}));
    }
    {
        rbttext_layout dead = dead_rbttext_layout();
        if (!dead)
            factory_missing("rbttext_layout_get_strokes");
        else
            expect_refused("rbttext_layout_get_strokes", rbttext_layout_get_strokes(dead, (RBTTEXT_SEGMENT *){0}, (uint32_t){0}, (uint32_t *){0}));
    }
}
#endif

int main(void)
{
    int calls = 0;
    expect_error("rbtarchive_codecs_add", rbtarchive_codecs_add((rbtarchive_codecs){0}, (const RBTARCHIVE_CODEC *){0}));
    calls++;
    expect_error("rbtarchive_codecs_add_deflate", rbtarchive_codecs_add_deflate((rbtarchive_codecs){0}));
    calls++;
    expect_error("rbtarchive_codecs_compress", rbtarchive_codecs_compress((rbtarchive_codecs){0}, (uint16_t){0}, (int32_t){0}, (const void *){0}, (uint64_t){0}, (void *){0}, (uint64_t){0}, (uint64_t *){0}, (RBTARCHIVE_ERROR *){0}));
    calls++;
    expect_error("rbtarchive_codecs_compress_bound", rbtarchive_codecs_compress_bound((rbtarchive_codecs){0}, (uint16_t){0}, (uint64_t){0}, (uint64_t *){0}));
    calls++;
    expect_error("rbtarchive_codecs_create", rbtarchive_codecs_create((rbtarchive_codecs *){0}));
    calls++;
    rbtarchive_codecs_destroy((rbtarchive_codecs){0});
    calls++;
    (void)rbtarchive_codecs_has((rbtarchive_codecs){0}, (uint16_t){0});
    calls++;
    (void)rbtarchive_compute_crc32((uint32_t){0}, (const void *){0}, (uint64_t){0});
    calls++;
    rbtarchive_entry_reader_destroy((rbtarchive_entry_reader){0});
    calls++;
    expect_error("rbtarchive_entry_reader_read", rbtarchive_entry_reader_read((rbtarchive_entry_reader){0}, (void *){0}, (uint64_t){0}, (uint64_t *){0}, (RBTARCHIVE_ERROR *){0}));
    calls++;
    expect_error("rbtarchive_extra_find", rbtarchive_extra_find((const void *){0}, (uint16_t){0}, (uint16_t){0}, (const void **){0}, (uint16_t *){0}));
    calls++;
    expect_error("rbtarchive_extra_get_ntfs_times", rbtarchive_extra_get_ntfs_times((const void *){0}, (uint16_t){0}, (RBTARCHIVE_FILE_TIMES *){0}));
    calls++;
    expect_error("rbtarchive_extra_get_unicode_path", rbtarchive_extra_get_unicode_path((const void *){0}, (uint16_t){0}, (const char *){0}, (uint16_t){0}, (const char **){0}, (uint16_t *){0}));
    calls++;
    expect_error("rbtarchive_extra_get_unix_times", rbtarchive_extra_get_unix_times((const void *){0}, (uint16_t){0}, (RBTARCHIVE_FILE_TIMES *){0}));
    calls++;
    expect_error("rbtarchive_extra_put_ntfs_times", rbtarchive_extra_put_ntfs_times((const RBTARCHIVE_FILE_TIMES *){0}, (void *){0}, (uint16_t){0}, (uint16_t *){0}));
    calls++;
    expect_error("rbtarchive_extra_put_unicode_path", rbtarchive_extra_put_unicode_path((const char *){0}, (uint16_t){0}, (const char *){0}, (uint16_t){0}, (void *){0}, (uint16_t){0}, (uint16_t *){0}));
    calls++;
    expect_error("rbtarchive_extra_put_unix_times", rbtarchive_extra_put_unix_times((const RBTARCHIVE_FILE_TIMES *){0}, (_Bool){0}, (void *){0}, (uint16_t){0}, (uint16_t *){0}));
    calls++;
    (void)rbtarchive_is_safe_name((const char *){0}, (uint64_t){0});
    calls++;
    rbtarchive_reader_destroy((rbtarchive_reader){0});
    calls++;
    expect_error("rbtarchive_reader_find", rbtarchive_reader_find((rbtarchive_reader){0}, (const char *){0}, (uint32_t *){0}));
    calls++;
    expect_error("rbtarchive_reader_get_comment", rbtarchive_reader_get_comment((rbtarchive_reader){0}, (const void **){0}, (uint16_t *){0}));
    calls++;
    expect_error("rbtarchive_reader_get_data_offset", rbtarchive_reader_get_data_offset((rbtarchive_reader){0}, (uint32_t){0}, (uint64_t *){0}, (RBTARCHIVE_ERROR *){0}));
    calls++;
    expect_error("rbtarchive_reader_get_entry", rbtarchive_reader_get_entry((rbtarchive_reader){0}, (uint32_t){0}, (RBTARCHIVE_ENTRY_INFO *){0}));
    calls++;
    (void)rbtarchive_reader_get_entry_count((rbtarchive_reader){0});
    calls++;
    (void)rbtarchive_reader_get_offset_shift((rbtarchive_reader){0});
    calls++;
    expect_error("rbtarchive_reader_open", rbtarchive_reader_open((const RBTARCHIVE_READER_DESC *){0}, (rbtarchive_reader *){0}, (RBTARCHIVE_ERROR *){0}));
    calls++;
    expect_error("rbtarchive_reader_open_entry", rbtarchive_reader_open_entry((rbtarchive_reader){0}, (uint32_t){0}, (rbtarchive_entry_reader *){0}, (RBTARCHIVE_ERROR *){0}));
    calls++;
    expect_error("rbtarchive_reader_read_entry", rbtarchive_reader_read_entry((rbtarchive_reader){0}, (uint32_t){0}, (void *){0}, (uint64_t){0}, (RBTARCHIVE_ERROR *){0}));
    calls++;
    expect_error("rbtarchive_writer_add_entry", rbtarchive_writer_add_entry((rbtarchive_writer){0}, (const RBTARCHIVE_ENTRY_DESC *){0}, (const void *){0}, (uint64_t){0}, (RBTARCHIVE_ERROR *){0}));
    calls++;
    expect_error("rbtarchive_writer_add_raw_entry", rbtarchive_writer_add_raw_entry((rbtarchive_writer){0}, (const RBTARCHIVE_ENTRY_DESC *){0}, (const void *){0}, (uint64_t){0}, (uint64_t){0}, (uint32_t){0}, (RBTARCHIVE_ERROR *){0}));
    calls++;
    expect_error("rbtarchive_writer_begin_entry", rbtarchive_writer_begin_entry((rbtarchive_writer){0}, (const RBTARCHIVE_ENTRY_DESC *){0}, (RBTARCHIVE_ERROR *){0}));
    calls++;
    expect_error("rbtarchive_writer_create", rbtarchive_writer_create((const RBTARCHIVE_WRITER_DESC *){0}, (rbtarchive_writer *){0}));
    calls++;
    expect_error("rbtarchive_writer_create_update", rbtarchive_writer_create_update((const RBTARCHIVE_UPDATE_DESC *){0}, (rbtarchive_writer *){0}, (RBTARCHIVE_ERROR *){0}));
    calls++;
    rbtarchive_writer_destroy((rbtarchive_writer){0});
    calls++;
    expect_error("rbtarchive_writer_end_entry", rbtarchive_writer_end_entry((rbtarchive_writer){0}, (RBTARCHIVE_ERROR *){0}));
    calls++;
    expect_error("rbtarchive_writer_finish", rbtarchive_writer_finish((rbtarchive_writer){0}, (RBTARCHIVE_ERROR *){0}));
    calls++;
    (void)rbtarchive_writer_get_offset((rbtarchive_writer){0});
    calls++;
    expect_error("rbtarchive_writer_remove_entry", rbtarchive_writer_remove_entry((rbtarchive_writer){0}, (const char *){0}, (RBTARCHIVE_ERROR *){0}));
    calls++;
    expect_error("rbtarchive_writer_set_comment", rbtarchive_writer_set_comment((rbtarchive_writer){0}, (const void *){0}, (uint16_t){0}, (RBTARCHIVE_ERROR *){0}));
    calls++;
    expect_error("rbtarchive_writer_write_entry_data", rbtarchive_writer_write_entry_data((rbtarchive_writer){0}, (const void *){0}, (uint64_t){0}, (RBTARCHIVE_ERROR *){0}));
    calls++;
    expect_error("rbtaudio_attenuation_create", rbtaudio_attenuation_create((rbtaudio_mixer){0}, (const RBTAUDIO_ATTENUATION_DESC *){0}, (RBTAUDIO_ATTENUATION *){0}, (RBTAUDIO_ERROR *){0}));
    calls++;
    rbtaudio_attenuation_destroy((rbtaudio_mixer){0}, (RBTAUDIO_ATTENUATION){0});
    calls++;
    expect_error("rbtaudio_bus_create", rbtaudio_bus_create((rbtaudio_mixer){0}, (const RBTAUDIO_BUS_DESC *){0}, (RBTAUDIO_BUS *){0}, (RBTAUDIO_ERROR *){0}));
    calls++;
    rbtaudio_bus_destroy((rbtaudio_mixer){0}, (RBTAUDIO_BUS){0});
    calls++;
    expect_error("rbtaudio_bus_set_gain", rbtaudio_bus_set_gain((rbtaudio_mixer){0}, (RBTAUDIO_BUS){0}, (float){0}, (uint32_t){0}));
    calls++;
    expect_error("rbtaudio_bus_set_muted", rbtaudio_bus_set_muted((rbtaudio_mixer){0}, (RBTAUDIO_BUS){0}, (_Bool){0}, (uint32_t){0}));
    calls++;
    expect_error("rbtaudio_bus_set_parent", rbtaudio_bus_set_parent((rbtaudio_mixer){0}, (RBTAUDIO_BUS){0}, (RBTAUDIO_BUS){0}));
    calls++;
    expect_error("rbtaudio_bus_set_paused", rbtaudio_bus_set_paused((rbtaudio_mixer){0}, (RBTAUDIO_BUS){0}, (_Bool){0}, (uint32_t){0}));
    calls++;
    expect_error("rbtaudio_bus_set_rate", rbtaudio_bus_set_rate((rbtaudio_mixer){0}, (RBTAUDIO_BUS){0}, (float){0}, (uint32_t){0}));
    calls++;
    expect_error("rbtaudio_concurrency_create", rbtaudio_concurrency_create((rbtaudio_mixer){0}, (const RBTAUDIO_CONCURRENCY_DESC *){0}, (RBTAUDIO_CONCURRENCY *){0}, (RBTAUDIO_ERROR *){0}));
    calls++;
    rbtaudio_concurrency_destroy((rbtaudio_mixer){0}, (RBTAUDIO_CONCURRENCY){0});
    calls++;
    rbtaudio_cues_clear_banks((rbtaudio_cues){0});
    calls++;
    expect_error("rbtaudio_cues_create", rbtaudio_cues_create((const RBTAUDIO_CUES_DESC *){0}, (rbtaudio_cues *){0}, (RBTAUDIO_ERROR *){0}));
    calls++;
    rbtaudio_cues_destroy((rbtaudio_cues){0});
    calls++;
    expect_error("rbtaudio_cues_find", rbtaudio_cues_find((rbtaudio_cues){0}, (const char *){0}, (RBTAUDIO_CUE *){0}));
    calls++;
    (void)rbtaudio_cues_get_bus_ducking((rbtaudio_cues){0}, (uint32_t){0});
    calls++;
    expect_error("rbtaudio_cues_get_info", rbtaudio_cues_get_info((rbtaudio_cues){0}, (RBTAUDIO_CUE){0}, (RBTAUDIO_CUE_INFO *){0}));
    calls++;
    (void)rbtaudio_cues_is_playing((rbtaudio_cues){0}, (RBTAUDIO_VOICE){0});
    calls++;
    expect_error("rbtaudio_cues_load_bank", rbtaudio_cues_load_bank((rbtaudio_cues){0}, (const RBTAUDIO_BANK_SOURCE *){0}, (RBTAUDIO_ERROR *){0}));
    calls++;
    expect_error("rbtaudio_cues_load_zip_bank", rbtaudio_cues_load_zip_bank((rbtaudio_cues){0}, (const RBTARCHIVE_SOURCE *){0}, (RBTAUDIO_ERROR *){0}));
    calls++;
    expect_error("rbtaudio_cues_play", rbtaudio_cues_play((rbtaudio_cues){0}, (RBTAUDIO_CUE){0}, (const RBTAUDIO_CUE_PLAY_DESC *){0}, (RBTAUDIO_VOICE *){0}));
    calls++;
    expect_error("rbtaudio_cues_set_bus_muted", rbtaudio_cues_set_bus_muted((rbtaudio_cues){0}, (uint32_t){0}, (_Bool){0}));
    calls++;
    expect_error("rbtaudio_cues_set_enabled", rbtaudio_cues_set_enabled((rbtaudio_cues){0}, (RBTAUDIO_CUE){0}, (_Bool){0}));
    calls++;
    rbtaudio_cues_set_sample_rate((rbtaudio_cues){0}, (uint32_t){0});
    calls++;
    expect_error("rbtaudio_cues_start", rbtaudio_cues_start((rbtaudio_cues){0}, (RBTAUDIO_CUE){0}, (const RBTAUDIO_CUE_PLAY_DESC *){0}, (RBTAUDIO_VOICE *){0}));
    calls++;
    rbtaudio_cues_stop((rbtaudio_cues){0}, (RBTAUDIO_VOICE){0}, (_Bool){0});
    calls++;
    rbtaudio_cues_update((rbtaudio_cues){0});
    calls++;
    rbtaudio_encoded_free((RBTAUDIO_ENCODED *){0});
    calls++;
    expect_error("rbtaudio_flac_decode", rbtaudio_flac_decode((const void *){0}, (uint64_t){0}, (uint64_t){0}, (RBTAUDIO_PCM *){0}, (RBTAUDIO_ERROR *){0}));
    calls++;
    (void)rbtaudio_format_from_craudio((const CRAUDIO_FORMAT *){0});
    calls++;
    rbtaudio_mixer_collect((rbtaudio_mixer){0});
    calls++;
    expect_error("rbtaudio_mixer_create", rbtaudio_mixer_create((const RBTAUDIO_MIXER_DESC *){0}, (rbtaudio_mixer *){0}, (RBTAUDIO_ERROR *){0}));
    calls++;
    rbtaudio_mixer_destroy((rbtaudio_mixer){0});
    calls++;
    (void)rbtaudio_mixer_get_master_bus((rbtaudio_mixer){0});
    calls++;
    rbtaudio_mixer_render((rbtaudio_mixer){0}, (void *){0}, (uint32_t){0}, (const CRAUDIO_CALLBACK_INFO *){0});
    calls++;
    expect_error("rbtaudio_mixer_set_emitters", rbtaudio_mixer_set_emitters((rbtaudio_mixer){0}, (const RBTAUDIO_VOICE *){0}, (const RBTAUDIO_EMITTER *){0}, (uint32_t){0}, (RBTAUDIO_ERROR *){0}));
    calls++;
    expect_error("rbtaudio_mixer_set_format", rbtaudio_mixer_set_format((rbtaudio_mixer){0}, (const RBTAUDIO_FORMAT *){0}, (uint32_t){0}, (RBTAUDIO_ERROR *){0}));
    calls++;
    expect_error("rbtaudio_mixer_set_listeners", rbtaudio_mixer_set_listeners((rbtaudio_mixer){0}, (const RBTAUDIO_LISTENER *){0}, (uint32_t){0}, (RBTAUDIO_ERROR *){0}));
    calls++;
    rbtaudio_mixer_stream_callback((craudio_stream){0}, (void *){0}, (uint32_t){0}, (const CRAUDIO_CALLBACK_INFO *){0}, (void *){0});
    calls++;
    expect_error("rbtaudio_mixer_update", rbtaudio_mixer_update((rbtaudio_mixer){0}, (RBTAUDIO_ERROR *){0}));
    calls++;
    expect_error("rbtaudio_modifier_create", rbtaudio_modifier_create((rbtaudio_mixer){0}, (const RBTAUDIO_MODIFIER_DESC *){0}, (RBTAUDIO_MODIFIER *){0}, (RBTAUDIO_ERROR *){0}));
    calls++;
    rbtaudio_modifier_destroy((rbtaudio_mixer){0}, (RBTAUDIO_MODIFIER){0});
    calls++;
    expect_error("rbtaudio_modifier_set_active", rbtaudio_modifier_set_active((rbtaudio_mixer){0}, (RBTAUDIO_MODIFIER){0}, (_Bool){0}));
    calls++;
    expect_error("rbtaudio_opus_decode", rbtaudio_opus_decode((const void *){0}, (uint64_t){0}, (uint64_t){0}, (RBTAUDIO_PCM *){0}, (RBTAUDIO_ERROR *){0}));
    calls++;
    expect_error("rbtaudio_opus_encode", rbtaudio_opus_encode((const RBTAUDIO_PCM *){0}, (const RBTAUDIO_OPUS_ENCODE_DESC *){0}, (RBTAUDIO_ENCODED *){0}, (RBTAUDIO_ERROR *){0}));
    calls++;
    rbtaudio_pcm_free((RBTAUDIO_PCM *){0});
    calls++;
    expect_error("rbtaudio_pcm_resample", rbtaudio_pcm_resample((const RBTAUDIO_PCM *){0}, (uint32_t){0}, (RBTAUDIO_RESAMPLER){0}, (RBTAUDIO_PCM *){0}, (RBTAUDIO_ERROR *){0}));
    calls++;
    expect_error("rbtaudio_play", rbtaudio_play((rbtaudio_mixer){0}, (RBTAUDIO_SOUND){0}, (const RBTAUDIO_PLAY_DESC *){0}, (RBTAUDIO_VOICE *){0}));
    calls++;
    expect_error("rbtaudio_sound_create", rbtaudio_sound_create((rbtaudio_mixer){0}, (const RBTAUDIO_SOUND_DESC *){0}, (RBTAUDIO_SOUND *){0}, (RBTAUDIO_ERROR *){0}));
    calls++;
    rbtaudio_sound_destroy((rbtaudio_mixer){0}, (RBTAUDIO_SOUND){0});
    calls++;
    (void)rbtaudio_sound_get_underrun_frames((rbtaudio_mixer){0}, (RBTAUDIO_SOUND){0});
    calls++;
    (void)rbtaudio_voice_is_folded((rbtaudio_mixer){0}, (RBTAUDIO_VOICE){0});
    calls++;
    (void)rbtaudio_voice_is_live((rbtaudio_mixer){0}, (RBTAUDIO_VOICE){0});
    calls++;
    expect_error("rbtaudio_voice_set_direction", rbtaudio_voice_set_direction((rbtaudio_mixer){0}, (RBTAUDIO_VOICE){0}, (const RBTAUDIO_DIRECTION *){0}));
    calls++;
    expect_error("rbtaudio_voice_set_gain", rbtaudio_voice_set_gain((rbtaudio_mixer){0}, (RBTAUDIO_VOICE){0}, (float){0}));
    calls++;
    expect_error("rbtaudio_voice_set_rate", rbtaudio_voice_set_rate((rbtaudio_mixer){0}, (RBTAUDIO_VOICE){0}, (float){0}));
    calls++;
    expect_error("rbtaudio_voice_stop", rbtaudio_voice_stop((rbtaudio_mixer){0}, (RBTAUDIO_VOICE){0}, (uint32_t){0}));
    calls++;
    expect_error("rbtaudio_vorbis_decode", rbtaudio_vorbis_decode((const void *){0}, (uint64_t){0}, (uint64_t){0}, (RBTAUDIO_PCM *){0}, (RBTAUDIO_ERROR *){0}));
    calls++;
    expect_error("rbtaudio_wav_decode", rbtaudio_wav_decode((const void *){0}, (uint64_t){0}, (RBTAUDIO_PCM *){0}, (RBTAUDIO_ERROR *){0}));
    calls++;
    (void)rbtjobs_get_processor_count();
    calls++;
    expect_error("rbtjobs_pool_create", rbtjobs_pool_create((const RBTJOBS_POOL_DESC *){0}, (rbtjobs_pool *){0}, (RBTJOBS_ERROR *){0}));
    calls++;
    rbtjobs_pool_destroy((rbtjobs_pool){0});
    calls++;
    expect_error("rbtjobs_pool_get_scheduler", rbtjobs_pool_get_scheduler((rbtjobs_pool){0}, (RBTJOBS_SCHEDULER *){0}));
    calls++;
    expect_error("rbtjobs_run", rbtjobs_run((const RBTJOBS_SCHEDULER *){0}, (RBTJOBS_TASK_FUNCTION){0}, (uint32_t){0}, (uint32_t){0}, (void *){0}));
    calls++;
    expect_error("rbtjobs_scheduler_check", rbtjobs_scheduler_check((const RBTJOBS_SCHEDULER *){0}, (RBTJOBS_ERROR *){0}));
    calls++;
    (void)rbtmath_aabb_test_volume((const RBTMATH_AABB *){0}, (const RbtDMat4 *){0}, (const RBTMATH_VOLUME *){0});
    calls++;
    (void)rbtmath_box_bvh_cast((rbtmath_box_bvh){0}, (const RBTMATH_RAY *){0}, (const RBTMATH_CAST_DESC *){0}, (RBTMATH_BOX_HIT *){0});
    calls++;
    (void)rbtmath_box_bvh_cast_all((rbtmath_box_bvh){0}, (const RBTMATH_RAY *){0}, (const RBTMATH_CAST_DESC *){0}, (RBTMATH_BOX_HIT *){0}, (uint32_t){0});
    calls++;
    expect_error("rbtmath_box_bvh_create", rbtmath_box_bvh_create((const RBTMATH_BOX_BVH_DESC *){0}, (rbtmath_box_bvh *){0}, (RBTMATH_ERROR *){0}));
    calls++;
    rbtmath_box_bvh_destroy((rbtmath_box_bvh){0});
    calls++;
    (void)rbtmath_box_bvh_get_cost((rbtmath_box_bvh){0});
    calls++;
    (void)rbtmath_box_bvh_overlap_all((rbtmath_box_bvh){0}, (const RBTMATH_SHAPE *){0}, (const RBTMATH_CAST_DESC *){0}, (uint32_t *){0}, (uint32_t){0});
    calls++;
    (void)rbtmath_box_bvh_overlaps_shape((rbtmath_box_bvh){0}, (const RBTMATH_SHAPE *){0}, (const RBTMATH_CAST_DESC *){0});
    calls++;
    rbtmath_box_bvh_rebuild((rbtmath_box_bvh){0});
    calls++;
    rbtmath_box_bvh_refit((rbtmath_box_bvh){0});
    calls++;
    expect_error("rbtmath_box_bvh_set_boxes", rbtmath_box_bvh_set_boxes((rbtmath_box_bvh){0}, (const uint32_t *){0}, (const RBTMATH_BVH_BOX *){0}, (uint32_t){0}, (RBTMATH_ERROR *){0}));
    calls++;
    (void)rbtmath_box_bvh_sweep((rbtmath_box_bvh){0}, (const RBTMATH_SWEEP *){0}, (const RBTMATH_CAST_DESC *){0}, (RBTMATH_BOX_HIT *){0});
    calls++;
    (void)rbtmath_box_bvh_sweep_all((rbtmath_box_bvh){0}, (const RBTMATH_SWEEP *){0}, (const RBTMATH_CAST_DESC *){0}, (RBTMATH_BOX_HIT *){0}, (uint32_t){0});
    calls++;
    (void)rbtmath_instance_bvh_cast((rbtmath_instance_bvh){0}, (const RBTMATH_RAY *){0}, (const RBTMATH_CAST_DESC *){0}, (RBTMATH_INSTANCE_HIT *){0});
    calls++;
    (void)rbtmath_instance_bvh_cast_all((rbtmath_instance_bvh){0}, (const RBTMATH_RAY *){0}, (const RBTMATH_CAST_DESC *){0}, (RBTMATH_INSTANCE_HIT *){0}, (uint32_t){0});
    calls++;
    expect_error("rbtmath_instance_bvh_create", rbtmath_instance_bvh_create((const RBTMATH_INSTANCE_BVH_DESC *){0}, (rbtmath_instance_bvh *){0}));
    calls++;
    rbtmath_instance_bvh_destroy((rbtmath_instance_bvh){0});
    calls++;
    (void)rbtmath_instance_bvh_get_bounds((rbtmath_instance_bvh){0});
    calls++;
    (void)rbtmath_instance_bvh_get_cost((rbtmath_instance_bvh){0});
    calls++;
    (void)rbtmath_instance_bvh_get_instance_count((rbtmath_instance_bvh){0});
    calls++;
    (void)rbtmath_instance_bvh_overlap_all((rbtmath_instance_bvh){0}, (const RBTMATH_SHAPE *){0}, (const RBTMATH_CAST_DESC *){0}, (uint32_t *){0}, (uint32_t){0});
    calls++;
    (void)rbtmath_instance_bvh_overlaps_shape((rbtmath_instance_bvh){0}, (const RBTMATH_SHAPE *){0}, (const RBTMATH_CAST_DESC *){0});
    calls++;
    rbtmath_instance_bvh_rebuild((rbtmath_instance_bvh){0});
    calls++;
    rbtmath_instance_bvh_refit((rbtmath_instance_bvh){0});
    calls++;
    expect_error("rbtmath_instance_bvh_set_instances", rbtmath_instance_bvh_set_instances((rbtmath_instance_bvh){0}, (const uint32_t *){0}, (const RBTMATH_BVH_INSTANCE *){0}, (uint32_t){0}, (RBTMATH_ERROR *){0}));
    calls++;
    (void)rbtmath_instance_bvh_sweep((rbtmath_instance_bvh){0}, (const RBTMATH_SWEEP *){0}, (const RBTMATH_CAST_DESC *){0}, (RBTMATH_SWEEP_HIT *){0});
    calls++;
    (void)rbtmath_instance_bvh_sweep_all((rbtmath_instance_bvh){0}, (const RBTMATH_SWEEP *){0}, (const RBTMATH_CAST_DESC *){0}, (RBTMATH_SWEEP_HIT *){0}, (uint32_t){0});
    calls++;
    (void)rbtmath_mesh_bvh_cast((rbtmath_mesh_bvh){0}, (const RBTMATH_RAY *){0}, (RBTMATH_MESH_HIT *){0});
    calls++;
    expect_error("rbtmath_mesh_bvh_create", rbtmath_mesh_bvh_create((const RBTMATH_MESH_BVH_DESC *){0}, (rbtmath_mesh_bvh *){0}));
    calls++;
    rbtmath_mesh_bvh_destroy((rbtmath_mesh_bvh){0});
    calls++;
    (void)rbtmath_mesh_bvh_get_bounds((rbtmath_mesh_bvh){0});
    calls++;
    (void)rbtmath_mesh_bvh_get_triangle_count((rbtmath_mesh_bvh){0});
    calls++;
    (void)rbtmath_mesh_bvh_overlaps_shape((rbtmath_mesh_bvh){0}, (const RbtDMat4 *){0}, (const RBTMATH_SHAPE *){0});
    calls++;
    (void)rbtmath_mesh_bvh_overlaps_volume((rbtmath_mesh_bvh){0}, (const RbtDMat4 *){0}, (const RBTMATH_VOLUME *){0});
    calls++;
    (void)rbtmath_mesh_bvh_sweep((rbtmath_mesh_bvh){0}, (const RbtDMat4 *){0}, (const RBTMATH_SWEEP *){0}, (RBTMATH_SWEEP_HIT *){0});
    calls++;
    (void)rbtmath_mesh_bvh_within_volume((rbtmath_mesh_bvh){0}, (const RbtDMat4 *){0}, (const RBTMATH_VOLUME *){0});
    calls++;
    expect_error("rbtmath_poly_boolean", rbtmath_poly_boolean((const RBTMATH_POLY_BOOLEAN_DESC *){0}, (rbtmath_poly *){0}, (RBTMATH_ERROR *){0}));
    calls++;
    rbtmath_poly_destroy((rbtmath_poly){0});
    calls++;
    (void)rbtmath_poly_get_set((rbtmath_poly){0});
    calls++;
    (void)rbtmath_poly_get_triangulation_capacity((const RBTMATH_POLY_SET *){0});
    calls++;
    expect_error("rbtmath_poly_locate_point", rbtmath_poly_locate_point((const RBTMATH_POLY_SET *){0}, (RBTMATH_POLY_FILL){0}, (RBTMATH_POLY_POINT){0}, (RBTMATH_POLY_PLACE *){0}, (RBTMATH_ERROR *){0}));
    calls++;
    expect_error("rbtmath_poly_offset_edges", rbtmath_poly_offset_edges((const RBTMATH_POLY_OFFSET_DESC *){0}, (rbtmath_poly *){0}, (RBTMATH_ERROR *){0}));
    calls++;
    expect_error("rbtmath_poly_skeleton", rbtmath_poly_skeleton((const RBTMATH_POLY_SKELETON_DESC *){0}, (rbtmath_poly *){0}, (RBTMATH_ERROR *){0}));
    calls++;
    expect_error("rbtmath_poly_triangulate", rbtmath_poly_triangulate((const RBTMATH_POLY_SET *){0}, (uint32_t *){0}, (uint32_t){0}, (uint32_t *){0}, (RBTMATH_ERROR *){0}));
    calls++;
    expect_error("rbtmath_poly_validate", rbtmath_poly_validate((const RBTMATH_POLY_SET *){0}, (RBTMATH_POLY_DEFECT_INFO *){0}, (RBTMATH_ERROR *){0}));
    calls++;
    (void)rbtmath_ray_clip_aabb((RBTMATH_RAY *){0}, (const RBTMATH_AABB *){0});
    calls++;
    expect_error("rbtrend_batch_add_circles", rbtrend_batch_add_circles((rbtrend_batch){0}, (const RBTREND_BATCH_CIRCLE *){0}, (uint32_t){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_batch_add_glyphs", rbtrend_batch_add_glyphs((rbtrend_batch){0}, (const RBTREND_BATCH_GLYPH *){0}, (uint32_t){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_batch_add_line", rbtrend_batch_add_line((rbtrend_batch){0}, (const RBTREND_BATCH_LINE *){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_batch_add_quads", rbtrend_batch_add_quads((rbtrend_batch){0}, (const RBTREND_BATCH_QUAD *){0}, (uint32_t){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_batch_begin", rbtrend_batch_begin((rbtrend_batch){0}, (const RBTREND_BATCH_FRAME *){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_batch_create", rbtrend_batch_create((rbtrend_instance){0}, (const RBTREND_BATCH_DESC *){0}, (rbtrend_batch *){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_batch_debug_arrow", rbtrend_batch_debug_arrow((rbtrend_batch){0}, (const float *){0}, (const float *){0}, (float){0}, (const float *){0}, (float){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_batch_debug_axes", rbtrend_batch_debug_axes((rbtrend_batch){0}, (const float *){0}, (float){0}, (float){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_batch_debug_box", rbtrend_batch_debug_box((rbtrend_batch){0}, (const float *){0}, (const float *){0}, (const float *){0}, (const float *){0}, (const float *){0}, (float){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_batch_debug_sphere", rbtrend_batch_debug_sphere((rbtrend_batch){0}, (const float *){0}, (float){0}, (const float *){0}, (float){0}, (RBTREND_ERROR *){0}));
    calls++;
    rbtrend_batch_destroy((rbtrend_batch){0});
    calls++;
    (void)rbtrend_batch_get_shape_count((rbtrend_batch){0});
    calls++;
    expect_error("rbtrend_binding_layout_create", rbtrend_binding_layout_create((rbtrend_instance){0}, (const RBTREND_BINDING_DESC *){0}, (uint32_t){0}, (rbtrend_binding_layout *){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_binding_layout_create_from_shaders", rbtrend_binding_layout_create_from_shaders((rbtrend_instance){0}, (const rbtrend_shader *){0}, (uint32_t){0}, (uint32_t){0}, (rbtrend_binding_layout *){0}, (RBTREND_ERROR *){0}));
    calls++;
    rbtrend_binding_layout_destroy((rbtrend_binding_layout){0});
    calls++;
    expect_error("rbtrend_binding_layout_find", rbtrend_binding_layout_find((rbtrend_binding_layout){0}, (const char *){0}, (uint32_t *){0}, (RBTREND_BINDING_DESC *){0}));
    calls++;
    expect_error("rbtrend_binding_layout_get_binding", rbtrend_binding_layout_get_binding((rbtrend_binding_layout){0}, (uint32_t){0}, (RBTREND_BINDING_DESC *){0}));
    calls++;
    (void)rbtrend_binding_layout_get_binding_count((rbtrend_binding_layout){0});
    calls++;
    expect_error("rbtrend_binding_layout_get_block", rbtrend_binding_layout_get_block((rbtrend_binding_layout){0}, (uint32_t){0}, (uint32_t *){0}, (const RBTREND_SHADER_MEMBER **){0}, (uint32_t *){0}));
    calls++;
    expect_error("rbtrend_binding_layout_get_uniform_layout", rbtrend_binding_layout_get_uniform_layout((rbtrend_binding_layout){0}, (uint32_t){0}, (RBTREND_UNIFORM_LAYOUT *){0}));
    calls++;
    expect_error("rbtrend_binding_set_create", rbtrend_binding_set_create((rbtrend_binding_layout){0}, (const RBTREND_BINDING_SET_ENTRY *){0}, (uint32_t){0}, (rbtrend_binding_set *){0}, (RBTREND_ERROR *){0}));
    calls++;
    rbtrend_binding_set_destroy((rbtrend_binding_set){0});
    calls++;
    expect_error("rbtrend_binding_set_update", rbtrend_binding_set_update((rbtrend_binding_set){0}, (const RBTREND_BINDING_SET_ENTRY *){0}, (uint32_t){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_blas_create", rbtrend_blas_create((rbtrend_instance){0}, (const RBTREND_BLAS_DESC *){0}, (rbtrend_blas *){0}, (RBTREND_ERROR *){0}));
    calls++;
    rbtrend_blas_destroy((rbtrend_blas){0});
    calls++;
    expect_error("rbtrend_blas_get_info", rbtrend_blas_get_info((rbtrend_blas){0}, (RBTREND_BLAS_INFO *){0}, (RBTREND_ERROR *){0}));
    calls++;
    (void)rbtrend_blas_get_slot((rbtrend_blas){0});
    calls++;
    (void)rbtrend_get_blend_additive();
    calls++;
    (void)rbtrend_get_blend_alpha();
    calls++;
    (void)rbtrend_get_blend_premultiplied();
    calls++;
    expect_error("rbtrend_buffer_create", rbtrend_buffer_create((rbtrend_instance){0}, (const RBTREND_BUFFER_DESC *){0}, (rbtrend_buffer *){0}, (RBTREND_ERROR *){0}));
    calls++;
    rbtrend_buffer_destroy((rbtrend_buffer){0});
    calls++;
    expect_error("rbtrend_buffer_get_desc", rbtrend_buffer_get_desc((rbtrend_buffer){0}, (RBTREND_BUFFER_DESC *){0}));
    calls++;
    expect_error("rbtrend_buffer_write", rbtrend_buffer_write((rbtrend_buffer){0}, (uint64_t){0}, (const void *){0}, (uint64_t){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_buffer_write_uniform", rbtrend_buffer_write_uniform((rbtrend_buffer){0}, (const RBTREND_UNIFORM_LAYOUT *){0}, (uint32_t){0}, (const void *){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_command_list_compile", rbtrend_command_list_compile((rbtrend_command_list){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_command_list_create", rbtrend_command_list_create((rbtrend_instance){0}, (rbtrend_command_list *){0}, (RBTREND_ERROR *){0}));
    calls++;
    rbtrend_command_list_destroy((rbtrend_command_list){0});
    calls++;
    rbtrend_command_list_reset((rbtrend_command_list){0});
    calls++;
    rbtrend_command_list_send((rbtrend_command_list){0}, (RBTREND_COMMAND){0});
    calls++;
    expect_error("rbtrend_command_list_submit", rbtrend_command_list_submit((rbtrend_command_list){0}, (crgpu_command_queue){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_compute_pass_create", rbtrend_compute_pass_create((rbtrend_instance){0}, (const RBTREND_COMPUTE_PASS_DESC *){0}, (rbtrend_compute_pass *){0}, (RBTREND_ERROR *){0}));
    calls++;
    rbtrend_compute_pass_destroy((rbtrend_compute_pass){0});
    calls++;
    expect_error("rbtrend_compute_pass_set_binding_set", rbtrend_compute_pass_set_binding_set((rbtrend_compute_pass){0}, (uint32_t){0}, (rbtrend_binding_set){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_compute_pipeline_create", rbtrend_compute_pipeline_create((rbtrend_instance){0}, (const RBTREND_COMPUTE_PIPELINE_DESC *){0}, (rbtrend_compute_pipeline *){0}, (RBTREND_ERROR *){0}));
    calls++;
    rbtrend_compute_pipeline_destroy((rbtrend_compute_pipeline){0});
    calls++;
    expect_error("rbtrend_compute_pipeline_get_desc", rbtrend_compute_pipeline_get_desc((rbtrend_compute_pipeline){0}, (RBTREND_COMPUTE_PIPELINE_DESC *){0}));
    calls++;
    expect_error("rbtrend_draw_list_add", rbtrend_draw_list_add((rbtrend_draw_list){0}, (const RBTREND_DRAW_MESH *){0}, (rbtrend_material){0}, (uint32_t){0}, (const void *){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_draw_list_build", rbtrend_draw_list_build((rbtrend_draw_list){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_draw_list_create", rbtrend_draw_list_create((rbtrend_instance){0}, (const RBTREND_DRAW_LIST_DESC *){0}, (rbtrend_draw_list *){0}, (RBTREND_ERROR *){0}));
    calls++;
    rbtrend_draw_list_destroy((rbtrend_draw_list){0});
    calls++;
    expect_error("rbtrend_draw_list_get_counts", rbtrend_draw_list_get_counts((rbtrend_draw_list){0}, (uint32_t *){0}, (uint32_t *){0}, (uint32_t *){0}));
    calls++;
    rbtrend_draw_list_reset((rbtrend_draw_list){0});
    calls++;
    expect_error("rbtrend_exposure_create", rbtrend_exposure_create((rbtrend_instance){0}, (const RBTREND_EXPOSURE_DESC *){0}, (rbtrend_exposure *){0}, (RBTREND_ERROR *){0}));
    calls++;
    rbtrend_exposure_destroy((rbtrend_exposure){0});
    calls++;
    expect_error("rbtrend_framebuffer_clear_colors", rbtrend_framebuffer_clear_colors((rbtrend_framebuffer){0}, (crgpu_command_queue){0}, (float){0}, (float){0}, (float){0}, (float){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_framebuffer_clear_depth", rbtrend_framebuffer_clear_depth((rbtrend_framebuffer){0}, (crgpu_command_queue){0}, (_Bool){0}, (_Bool){0}, (float){0}, (uint32_t){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_framebuffer_create", rbtrend_framebuffer_create((rbtrend_instance){0}, (const RBTREND_FRAMEBUFFER_DESC *){0}, (rbtrend_framebuffer *){0}, (RBTREND_ERROR *){0}));
    calls++;
    rbtrend_framebuffer_destroy((rbtrend_framebuffer){0});
    calls++;
    (void)rbtrend_framebuffer_get_color_attachment((rbtrend_framebuffer){0}, (uint32_t){0});
    calls++;
    (void)rbtrend_framebuffer_get_depth_attachment((rbtrend_framebuffer){0});
    calls++;
    expect_error("rbtrend_framebuffer_get_desc", rbtrend_framebuffer_get_desc((rbtrend_framebuffer){0}, (RBTREND_FRAMEBUFFER_DESC *){0}));
    calls++;
    expect_error("rbtrend_framebuffer_resize", rbtrend_framebuffer_resize((rbtrend_framebuffer){0}, (uint32_t){0}, (uint32_t){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_instance_get_fullscreen_vertex_shader", rbtrend_instance_get_fullscreen_vertex_shader((rbtrend_instance){0}, (rbtrend_shader *){0}, (RBTREND_ERROR *){0}));
    calls++;
    (void)rbtrend_get_optional_gpu_features();
    calls++;
    expect_error("rbtrend_gpu_timer_create", rbtrend_gpu_timer_create((rbtrend_instance){0}, (const RBTREND_GPU_TIMER_DESC *){0}, (rbtrend_gpu_timer *){0}, (RBTREND_ERROR *){0}));
    calls++;
    rbtrend_gpu_timer_destroy((rbtrend_gpu_timer){0});
    calls++;
    expect_error("rbtrend_gpu_timer_get_ms", rbtrend_gpu_timer_get_ms((rbtrend_gpu_timer){0}, (uint32_t){0}, (double *){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_graphics_pipeline_create", rbtrend_graphics_pipeline_create((rbtrend_instance){0}, (const RBTREND_GRAPHICS_PIPELINE_DESC *){0}, (rbtrend_graphics_pipeline *){0}, (RBTREND_ERROR *){0}));
    calls++;
    rbtrend_graphics_pipeline_destroy((rbtrend_graphics_pipeline){0});
    calls++;
    expect_error("rbtrend_graphics_pipeline_get_desc", rbtrend_graphics_pipeline_get_desc((rbtrend_graphics_pipeline){0}, (RBTREND_GRAPHICS_PIPELINE_DESC *){0}));
    calls++;
    expect_error("rbtrend_index_pool_allocate", rbtrend_index_pool_allocate((rbtrend_index_pool){0}, (uint32_t){0}, (const void *){0}, (RBTREND_INDEX_RANGE *){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_index_pool_create", rbtrend_index_pool_create((rbtrend_instance){0}, (const RBTREND_INDEX_POOL_DESC *){0}, (rbtrend_index_pool *){0}, (RBTREND_ERROR *){0}));
    calls++;
    rbtrend_index_pool_destroy((rbtrend_index_pool){0});
    calls++;
    expect_error("rbtrend_index_pool_free", rbtrend_index_pool_free((rbtrend_index_pool){0}, (const RBTREND_INDEX_RANGE *){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_index_pool_get_usage", rbtrend_index_pool_get_usage((rbtrend_index_pool){0}, (uint64_t *){0}, (uint64_t *){0}));
    calls++;
    rbtrend_index_pool_trim((rbtrend_index_pool){0});
    calls++;
    expect_error("rbtrend_instance_create", rbtrend_instance_create((const RBTREND_INSTANCE_DESC *){0}, (rbtrend_instance *){0}, (RBTREND_ERROR *){0}));
    calls++;
    rbtrend_instance_destroy((rbtrend_instance){0});
    calls++;
    expect_error("rbtrend_instance_get_desc", rbtrend_instance_get_desc((rbtrend_instance){0}, (RBTREND_INSTANCE_DESC *){0}));
    calls++;
    (void)rbtrend_instance_get_frame_index((rbtrend_instance){0});
    calls++;
    expect_error("rbtrend_instance_get_render_target_pool", rbtrend_instance_get_render_target_pool((rbtrend_instance){0}, (RBTREND_RENDER_TARGET_POOL_INFO *){0}));
    calls++;
    expect_error("rbtrend_instance_next_frame", rbtrend_instance_next_frame((rbtrend_instance){0}, (uint64_t){0}, (uint32_t *){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_instance_set_render_target_retirement", rbtrend_instance_set_render_target_retirement((rbtrend_instance){0}, (uint32_t){0}, (uint64_t){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_instance_trim_render_targets", rbtrend_instance_trim_render_targets((rbtrend_instance){0}, (uint32_t){0}, (uint32_t *){0}));
    calls++;
    expect_error("rbtrend_instance_wait_idle", rbtrend_instance_wait_idle((rbtrend_instance){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_lit_environment_create", rbtrend_lit_environment_create((rbtrend_instance){0}, (const RBTREND_LIT_ENVIRONMENT_DESC *){0}, (rbtrend_lit_environment *){0}, (RBTREND_ERROR *){0}));
    calls++;
    rbtrend_lit_environment_destroy((rbtrend_lit_environment){0});
    calls++;
    (void)rbtrend_lit_environment_get_specular_mip_count((rbtrend_lit_environment){0});
    calls++;
    expect_error("rbtrend_lit_fit_cascades", rbtrend_lit_fit_cascades((const RBTREND_LIT_CASCADE_FIT *){0}, (RBTREND_LIT_SHADOW_MAPS *){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_lit_get_material_layout", rbtrend_lit_get_material_layout((rbtrend_instance){0}, (rbtrend_binding_layout *){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_lit_get_scene_layout", rbtrend_lit_get_scene_layout((rbtrend_instance){0}, (rbtrend_binding_layout *){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_lit_get_shader", rbtrend_lit_get_shader((rbtrend_instance){0}, (RBTREND_LIT_SHADER){0}, (rbtrend_shader *){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_lit_get_shadow_layout", rbtrend_lit_get_shadow_layout((rbtrend_instance){0}, (rbtrend_binding_layout *){0}, (RBTREND_ERROR *){0}));
    calls++;
    (void)rbtrend_get_lit_instance_layout();
    calls++;
    expect_error("rbtrend_lit_material_create", rbtrend_lit_material_create((rbtrend_instance){0}, (rbtrend_material *){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_lit_material_get_map_uv_transform", rbtrend_lit_material_get_map_uv_transform((rbtrend_material){0}, (RBTREND_LIT_MAP){0}, (float *){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_lit_material_get_values", rbtrend_lit_material_get_values((rbtrend_material){0}, (RBTREND_LIT_MATERIAL_VALUES *){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_lit_material_set_map", rbtrend_lit_material_set_map((rbtrend_material){0}, (RBTREND_LIT_MAP){0}, (rbtrend_texture_view){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_lit_material_set_map_uv_transform", rbtrend_lit_material_set_map_uv_transform((rbtrend_material){0}, (RBTREND_LIT_MAP){0}, (const float *){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_lit_material_set_sampler", rbtrend_lit_material_set_sampler((rbtrend_material){0}, (rbtrend_sampler){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_lit_material_set_values", rbtrend_lit_material_set_values((rbtrend_material){0}, (const RBTREND_LIT_MATERIAL_VALUES *){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_lit_point_shadow_view_projection", rbtrend_lit_point_shadow_view_projection((const RBTREND_LIT_SCENE_LIGHT *){0}, (uint32_t){0}, (float){0}, (_Bool){0}, (float *){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_lit_scene_create", rbtrend_lit_scene_create((rbtrend_instance){0}, (rbtrend_lit_scene *){0}, (RBTREND_ERROR *){0}));
    calls++;
    rbtrend_lit_scene_destroy((rbtrend_lit_scene){0});
    calls++;
    (void)rbtrend_lit_scene_get_material((rbtrend_lit_scene){0});
    calls++;
    (void)rbtrend_lit_scene_get_shadow_pass_material((rbtrend_lit_scene){0}, (uint32_t){0}, (uint32_t){0});
    calls++;
    expect_error("rbtrend_lit_scene_set_ambient", rbtrend_lit_scene_set_ambient((rbtrend_lit_scene){0}, (const RBTREND_LIT_AMBIENT *){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_lit_scene_set_ambient_occlusion", rbtrend_lit_scene_set_ambient_occlusion((rbtrend_lit_scene){0}, (rbtrend_texture_view){0}, (float){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_lit_scene_set_bent_normals", rbtrend_lit_scene_set_bent_normals((rbtrend_lit_scene){0}, (_Bool){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_lit_scene_set_light_shadow", rbtrend_lit_scene_set_light_shadow((rbtrend_lit_scene){0}, (uint32_t){0}, (const RBTREND_LIT_LIGHT_SHADOW *){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_lit_scene_set_lights", rbtrend_lit_scene_set_lights((rbtrend_lit_scene){0}, (const RBTREND_LIT_SCENE_LIGHT *){0}, (uint32_t){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_lit_scene_set_scene_color", rbtrend_lit_scene_set_scene_color((rbtrend_lit_scene){0}, (rbtrend_texture_view){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_lit_scene_set_shadow_atlas", rbtrend_lit_scene_set_shadow_atlas((rbtrend_lit_scene){0}, (rbtrend_texture_view){0}, (_Bool){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_lit_scene_set_view", rbtrend_lit_scene_set_view((rbtrend_lit_scene){0}, (const RBTREND_LIT_VIEW *){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_lit_spot_shadow_view_projection", rbtrend_lit_spot_shadow_view_projection((const RBTREND_LIT_SCENE_LIGHT *){0}, (float){0}, (_Bool){0}, (float *){0}, (RBTREND_ERROR *){0}));
    calls++;
    (void)rbtrend_get_lit_vertex_color_layout();
    calls++;
    (void)rbtrend_get_lit_vertex_layout();
    calls++;
    expect_error("rbtrend_material_copy", rbtrend_material_copy((rbtrend_material){0}, (rbtrend_material *){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_material_create", rbtrend_material_create((rbtrend_instance){0}, (const RBTREND_MATERIAL_DESC *){0}, (rbtrend_material *){0}, (RBTREND_ERROR *){0}));
    calls++;
    rbtrend_material_destroy((rbtrend_material){0});
    calls++;
    expect_error("rbtrend_material_find_uniform", rbtrend_material_find_uniform((rbtrend_binding_layout){0}, (const char *){0}, (RBTREND_MATERIAL_UNIFORM *){0}, (RBTREND_SHADER_MEMBER *){0}, (RBTREND_ERROR *){0}));
    calls++;
    (void)rbtrend_material_get_layout((rbtrend_material){0});
    calls++;
    expect_error("rbtrend_material_get_uniform_value", rbtrend_material_get_uniform_value((rbtrend_material){0}, (RBTREND_MATERIAL_UNIFORM){0}, (void *){0}, (RBTREND_ERROR *){0}));
    calls++;
    (void)rbtrend_material_is_complete((rbtrend_material){0});
    calls++;
    expect_error("rbtrend_material_set_sampler", rbtrend_material_set_sampler((rbtrend_material){0}, (const char *){0}, (uint32_t){0}, (rbtrend_sampler){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_material_set_storage_buffer", rbtrend_material_set_storage_buffer((rbtrend_material){0}, (const char *){0}, (uint32_t){0}, (rbtrend_buffer){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_material_set_texture", rbtrend_material_set_texture((rbtrend_material){0}, (const char *){0}, (uint32_t){0}, (rbtrend_texture_view){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_material_set_uniform", rbtrend_material_set_uniform((rbtrend_material){0}, (const char *){0}, (const void *){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_material_set_uniform_buffer", rbtrend_material_set_uniform_buffer((rbtrend_material){0}, (const char *){0}, (uint32_t){0}, (rbtrend_buffer){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_material_set_uniform_bytes", rbtrend_material_set_uniform_bytes((rbtrend_material){0}, (const char *){0}, (uint32_t){0}, (uint32_t){0}, (const void *){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_material_set_uniform_value", rbtrend_material_set_uniform_value((rbtrend_material){0}, (RBTREND_MATERIAL_UNIFORM){0}, (const void *){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_render_pass_create", rbtrend_render_pass_create((rbtrend_instance){0}, (const RBTREND_RENDER_PASS_DESC *){0}, (rbtrend_render_pass *){0}, (RBTREND_ERROR *){0}));
    calls++;
    rbtrend_render_pass_destroy((rbtrend_render_pass){0});
    calls++;
    (void)rbtrend_render_pass_get_pipeline((rbtrend_render_pass){0});
    calls++;
    expect_error("rbtrend_render_pass_get_size", rbtrend_render_pass_get_size((rbtrend_render_pass){0}, (uint32_t *){0}, (uint32_t *){0}));
    calls++;
    expect_error("rbtrend_render_pass_set_binding_set", rbtrend_render_pass_set_binding_set((rbtrend_render_pass){0}, (uint32_t){0}, (rbtrend_binding_set){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_sampler_create", rbtrend_sampler_create((rbtrend_instance){0}, (const RBTREND_SAMPLER_DESC *){0}, (rbtrend_sampler *){0}, (RBTREND_ERROR *){0}));
    calls++;
    rbtrend_sampler_destroy((rbtrend_sampler){0});
    calls++;
    expect_error("rbtrend_sampler_get_desc", rbtrend_sampler_get_desc((rbtrend_sampler){0}, (RBTREND_SAMPLER_DESC *){0}));
    calls++;
    (void)rbtrend_sampler_get_crgpu_sampler((rbtrend_sampler){0});
    calls++;
    expect_error("rbtrend_shader_create", rbtrend_shader_create((rbtrend_instance){0}, (const RBTREND_SHADER_PACKAGE *){0}, (rbtrend_shader *){0}, (RBTREND_ERROR *){0}));
    calls++;
    rbtrend_shader_destroy((rbtrend_shader){0});
    calls++;
    (void)rbtrend_shader_get_stage((rbtrend_shader){0});
    calls++;
    expect_error("rbtrend_swap_chain_acquire", rbtrend_swap_chain_acquire((rbtrend_swap_chain){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_swap_chain_create", rbtrend_swap_chain_create((rbtrend_instance){0}, (const RBTREND_SWAP_CHAIN_DESC *){0}, (rbtrend_swap_chain *){0}, (RBTREND_ERROR *){0}));
    calls++;
    rbtrend_swap_chain_destroy((rbtrend_swap_chain){0});
    calls++;
    expect_error("rbtrend_swap_chain_get_back_buffer_size", rbtrend_swap_chain_get_back_buffer_size((rbtrend_swap_chain){0}, (uint32_t *){0}, (uint32_t *){0}));
    calls++;
    expect_error("rbtrend_swap_chain_get_desc", rbtrend_swap_chain_get_desc((rbtrend_swap_chain){0}, (RBTREND_SWAP_CHAIN_DESC *){0}));
    calls++;
    expect_error("rbtrend_swap_chain_present", rbtrend_swap_chain_present((rbtrend_swap_chain){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_swap_chain_resize", rbtrend_swap_chain_resize((rbtrend_swap_chain){0}, (uint32_t){0}, (uint32_t){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_texture2d_create", rbtrend_texture2d_create((rbtrend_instance){0}, (const RBTREND_TEXTURE2D_DESC *){0}, (rbtrend_texture2d *){0}, (RBTREND_ERROR *){0}));
    calls++;
    rbtrend_texture2d_destroy((rbtrend_texture2d){0});
    calls++;
    expect_error("rbtrend_texture2d_generate_mips", rbtrend_texture2d_generate_mips((rbtrend_texture2d){0}, (crgpu_command_queue){0}, (uint32_t){0}, (CRGPU_FILTER){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_texture2d_get_data", rbtrend_texture2d_get_data((rbtrend_texture2d){0}, (crgpu_command_queue){0}, (uint32_t){0}, (uint32_t){0}, (uint32_t){0}, (uint32_t){0}, (uint32_t){0}, (uint32_t){0}, (void *){0}, (uint64_t){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_texture2d_get_desc", rbtrend_texture2d_get_desc((rbtrend_texture2d){0}, (RBTREND_TEXTURE2D_DESC *){0}));
    calls++;
    (void)rbtrend_texture2d_get_crgpu_image((rbtrend_texture2d){0});
    calls++;
    expect_error("rbtrend_texture2d_resize", rbtrend_texture2d_resize((rbtrend_texture2d){0}, (uint32_t){0}, (uint32_t){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_texture2d_update", rbtrend_texture2d_update((rbtrend_texture2d){0}, (crgpu_command_queue){0}, (uint32_t){0}, (uint32_t){0}, (uint32_t){0}, (uint32_t){0}, (uint32_t){0}, (uint32_t){0}, (const void *){0}, (uint64_t){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_texture3d_create", rbtrend_texture3d_create((rbtrend_instance){0}, (const RBTREND_TEXTURE3D_DESC *){0}, (rbtrend_texture3d *){0}, (RBTREND_ERROR *){0}));
    calls++;
    rbtrend_texture3d_destroy((rbtrend_texture3d){0});
    calls++;
    expect_error("rbtrend_texture3d_get_data", rbtrend_texture3d_get_data((rbtrend_texture3d){0}, (crgpu_command_queue){0}, (uint32_t){0}, (uint32_t){0}, (uint32_t){0}, (uint32_t){0}, (uint32_t){0}, (uint32_t){0}, (uint32_t){0}, (void *){0}, (uint64_t){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_texture3d_get_desc", rbtrend_texture3d_get_desc((rbtrend_texture3d){0}, (RBTREND_TEXTURE3D_DESC *){0}));
    calls++;
    (void)rbtrend_texture3d_get_crgpu_image((rbtrend_texture3d){0});
    calls++;
    (void)rbtrend_texture3d_get_crgpu_image_view((rbtrend_texture3d){0});
    calls++;
    expect_error("rbtrend_texture3d_resize", rbtrend_texture3d_resize((rbtrend_texture3d){0}, (uint32_t){0}, (uint32_t){0}, (uint32_t){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_texture3d_update", rbtrend_texture3d_update((rbtrend_texture3d){0}, (crgpu_command_queue){0}, (uint32_t){0}, (uint32_t){0}, (uint32_t){0}, (uint32_t){0}, (uint32_t){0}, (uint32_t){0}, (uint32_t){0}, (const void *){0}, (uint64_t){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_texture_view_create_2d", rbtrend_texture_view_create_2d((rbtrend_texture2d){0}, (const RBTREND_TEXTURE_VIEW_DESC *){0}, (rbtrend_texture_view *){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_texture_view_create_3d", rbtrend_texture_view_create_3d((rbtrend_texture3d){0}, (const RBTREND_TEXTURE_VIEW_DESC *){0}, (rbtrend_texture_view *){0}, (RBTREND_ERROR *){0}));
    calls++;
    rbtrend_texture_view_destroy((rbtrend_texture_view){0});
    calls++;
    expect_error("rbtrend_texture_view_get_desc", rbtrend_texture_view_get_desc((rbtrend_texture_view){0}, (RBTREND_TEXTURE_VIEW_DESC *){0}));
    calls++;
    (void)rbtrend_texture_view_get_dimension((rbtrend_texture_view){0});
    calls++;
    (void)rbtrend_texture_view_get_crgpu_image_view((rbtrend_texture_view){0});
    calls++;
    (void)rbtrend_texture_view_get_texture2d((rbtrend_texture_view){0});
    calls++;
    (void)rbtrend_texture_view_get_type((rbtrend_texture_view){0});
    calls++;
    expect_error("rbtrend_tlas_create", rbtrend_tlas_create((rbtrend_instance){0}, (const RBTREND_TLAS_DESC *){0}, (rbtrend_tlas *){0}, (RBTREND_ERROR *){0}));
    calls++;
    rbtrend_tlas_destroy((rbtrend_tlas){0});
    calls++;
    expect_error("rbtrend_tlas_get_info", rbtrend_tlas_get_info((rbtrend_tlas){0}, (RBTREND_TLAS_INFO *){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_tlas_set_instance_count", rbtrend_tlas_set_instance_count((rbtrend_tlas){0}, (uint32_t){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_tlas_set_materials", rbtrend_tlas_set_materials((rbtrend_tlas){0}, (uint32_t){0}, (const rbtrend_material *){0}, (uint32_t){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_tlas_write_instances", rbtrend_tlas_write_instances((rbtrend_tlas){0}, (uint32_t){0}, (const RBTREND_RAY_INSTANCE *){0}, (uint32_t){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_transient_allocate", rbtrend_transient_allocate((rbtrend_instance){0}, (uint64_t){0}, (uint64_t){0}, (RBTREND_TRANSIENT_SPAN *){0}, (RBTREND_ERROR *){0}));
    calls++;
    (void)rbtrend_transient_get_frame_usage((rbtrend_instance){0});
    calls++;
    expect_error("rbtrend_uniform_layout_get_size", rbtrend_uniform_layout_get_size((const RBTREND_UNIFORM_LAYOUT *){0}, (uint32_t *){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_uniform_layout_get_uniform_index", rbtrend_uniform_layout_get_uniform_index((const RBTREND_UNIFORM_LAYOUT *){0}, (const char *){0}, (uint32_t *){0}));
    calls++;
    (void)rbtrend_uniform_type_get_size((RBTREND_UNIFORM_TYPE){0});
    calls++;
    (void)rbtrend_vertex_element_type_get_size((RBTREND_VERTEX_ELEMENT_TYPE){0});
    calls++;
    (void)rbtrend_vertex_layout_get_element_index((const RBTREND_VERTEX_LAYOUT *){0}, (const char *){0});
    calls++;
    expect_error("rbtrend_vertex_pool_allocate", rbtrend_vertex_pool_allocate((rbtrend_vertex_pool){0}, (uint32_t){0}, (const void *){0}, (RBTREND_VERTEX_RANGE *){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_vertex_pool_create", rbtrend_vertex_pool_create((rbtrend_instance){0}, (const RBTREND_VERTEX_POOL_DESC *){0}, (rbtrend_vertex_pool *){0}, (RBTREND_ERROR *){0}));
    calls++;
    rbtrend_vertex_pool_destroy((rbtrend_vertex_pool){0});
    calls++;
    expect_error("rbtrend_vertex_pool_free", rbtrend_vertex_pool_free((rbtrend_vertex_pool){0}, (const RBTREND_VERTEX_RANGE *){0}, (RBTREND_ERROR *){0}));
    calls++;
    expect_error("rbtrend_vertex_pool_get_usage", rbtrend_vertex_pool_get_usage((rbtrend_vertex_pool){0}, (uint64_t *){0}, (uint64_t *){0}));
    calls++;
    rbtrend_vertex_pool_trim((rbtrend_vertex_pool){0});
    calls++;
    rbt_debug_name_set((void *){0}, (const char *){0});
    calls++;
    (void)rbt_get_version();
    calls++;
    rbtsync_session_destroy((rbtsync_session){0});
    calls++;
    expect_error("rbtsync_session_disconnect_peer", rbtsync_session_disconnect_peer((rbtsync_session){0}, (RBTSYNC_PEER){0}));
    calls++;
    (void)rbtsync_session_get_local_peer((rbtsync_session){0});
    calls++;
    expect_error("rbtsync_session_get_port", rbtsync_session_get_port((rbtsync_session){0}, (uint32_t){0}, (uint16_t *){0}));
    calls++;
    expect_error("rbtsync_session_host", rbtsync_session_host((const RBTSYNC_SESSION_DESC *){0}, (const RBTSYNC_HOST_DESC *){0}, (rbtsync_session *){0}, (RBTSYNC_ERROR *){0}));
    calls++;
    expect_error("rbtsync_session_join", rbtsync_session_join((const RBTSYNC_SESSION_DESC *){0}, (const RBTSYNC_JOIN_DESC *){0}, (rbtsync_session *){0}, (RBTSYNC_ERROR *){0}));
    calls++;
    expect_error("rbtsync_session_poll", rbtsync_session_poll((rbtsync_session){0}, (RBTSYNC_EVENT *){0}));
    calls++;
    expect_error("rbtsync_session_send", rbtsync_session_send((rbtsync_session){0}, (uint32_t){0}, (uint64_t){0}, (const void *){0}, (uint32_t){0}));
    calls++;
    expect_error("rbtsync_session_send_snapshot", rbtsync_session_send_snapshot((rbtsync_session){0}, (RBTSYNC_PEER){0}, (const void *){0}, (uint64_t){0}));
    calls++;
    expect_error("rbtsync_session_wait", rbtsync_session_wait((rbtsync_session){0}, (uint32_t){0}));
    calls++;
    rbtsync_session_wake((rbtsync_session){0});
    calls++;
    expect_error("rbttext_atlas_add_layout", rbttext_atlas_add_layout((rbttext_atlas){0}, (rbttext_layout){0}, (RBTTEXT_ERROR *){0}));
    calls++;
    expect_error("rbttext_atlas_create", rbttext_atlas_create((const RBTTEXT_ATLAS_DESC *){0}, (rbttext_atlas *){0}, (RBTTEXT_ERROR *){0}));
    calls++;
    rbttext_atlas_destroy((rbttext_atlas){0});
    calls++;
    expect_error("rbttext_atlas_find_glyph", rbttext_atlas_find_glyph((rbttext_atlas){0}, (rbttext_face){0}, (uint32_t){0}, (RBTTEXT_ATLAS_GLYPH *){0}));
    calls++;
    expect_error("rbttext_atlas_get_info", rbttext_atlas_get_info((rbttext_atlas){0}, (RBTTEXT_ATLAS_INFO *){0}));
    calls++;
    expect_error("rbttext_atlas_get_page", rbttext_atlas_get_page((rbttext_atlas){0}, (uint32_t){0}, (const void **){0}));
    calls++;
    expect_error("rbttext_atlas_insert", rbttext_atlas_insert((rbttext_atlas){0}, (const RBTTEXT_GLYPH_GENERATE_DESC *){0}, (const void *){0}, (uint64_t){0}, (RBTTEXT_ATLAS_GLYPH *){0}, (RBTTEXT_ERROR *){0}));
    calls++;
    expect_error("rbttext_atlas_load", rbttext_atlas_load((const RBTTEXT_ATLAS_LOAD_DESC *){0}, (rbttext_atlas *){0}, (RBTTEXT_ERROR *){0}));
    calls++;
    expect_error("rbttext_atlas_save", rbttext_atlas_save((rbttext_atlas){0}, (const rbttext_face *){0}, (uint32_t){0}, (void *){0}, (uint64_t){0}, (uint64_t *){0}, (RBTTEXT_ERROR *){0}));
    calls++;
    expect_error("rbttext_atlas_take_dirty", rbttext_atlas_take_dirty((rbttext_atlas){0}, (RBTTEXT_DIRTY_RECT *){0}, (uint32_t){0}, (uint32_t *){0}));
    calls++;
    expect_error("rbttext_chain_create", rbttext_chain_create((const RBTTEXT_CHAIN_DESC *){0}, (rbttext_chain *){0}, (RBTTEXT_ERROR *){0}));
    calls++;
    rbttext_chain_destroy((rbttext_chain){0});
    calls++;
    expect_error("rbttext_chain_find_glyph", rbttext_chain_find_glyph((rbttext_chain){0}, (uint32_t){0}, (uint32_t *){0}, (uint32_t *){0}));
    calls++;
    (void)rbttext_chain_get_face((rbttext_chain){0}, (uint32_t){0});
    calls++;
    (void)rbttext_chain_get_face_count((rbttext_chain){0});
    calls++;
    expect_error("rbttext_face_create", rbttext_face_create((const RBTTEXT_FACE_DESC *){0}, (rbttext_face *){0}, (RBTTEXT_ERROR *){0}));
    calls++;
    expect_error("rbttext_face_create_hershey", rbttext_face_create_hershey((const RBTTEXT_HERSHEY_FACE_DESC *){0}, (rbttext_face *){0}, (RBTTEXT_ERROR *){0}));
    calls++;
    expect_error("rbttext_face_create_lff", rbttext_face_create_lff((const RBTTEXT_LFF_FACE_DESC *){0}, (rbttext_face *){0}, (RBTTEXT_ERROR *){0}));
    calls++;
    expect_error("rbttext_face_create_shx", rbttext_face_create_shx((const RBTTEXT_SHX_FACE_DESC *){0}, (rbttext_face *){0}, (RBTTEXT_ERROR *){0}));
    calls++;
    expect_error("rbttext_face_create_stroke", rbttext_face_create_stroke((const RBTTEXT_STROKE_FACE_DESC *){0}, (rbttext_face *){0}, (RBTTEXT_ERROR *){0}));
    calls++;
    rbttext_face_destroy((rbttext_face){0});
    calls++;
    expect_error("rbttext_face_find_glyph", rbttext_face_find_glyph((rbttext_face){0}, (uint32_t){0}, (uint32_t *){0}));
    calls++;
    expect_error("rbttext_face_get_axis", rbttext_face_get_axis((rbttext_face){0}, (uint32_t){0}, (RBTTEXT_AXIS *){0}));
    calls++;
    (void)rbttext_face_get_axis_count((rbttext_face){0});
    calls++;
    expect_error("rbttext_face_get_glyph_strokes", rbttext_face_get_glyph_strokes((rbttext_face){0}, (uint32_t){0}, (const RbtVec2 **){0}, (uint32_t *){0}, (const RBTTEXT_STROKE_POLYLINE **){0}, (uint32_t *){0}));
    calls++;
    expect_error("rbttext_face_get_metrics", rbttext_face_get_metrics((rbttext_face){0}, (RBTTEXT_FACE_METRICS *){0}));
    calls++;
    expect_error("rbttext_generate_glyph", rbttext_generate_glyph((const RBTTEXT_GLYPH_GENERATE_DESC *){0}, (void *){0}, (uint64_t){0}, (RBTTEXT_GLYPH_IMAGE *){0}, (RBTTEXT_ERROR *){0}));
    calls++;
    expect_error("rbttext_layout_build", rbttext_layout_build((rbttext_layout){0}, (const RBTTEXT_LAYOUT_DESC *){0}, (RBTTEXT_ERROR *){0}));
    calls++;
    expect_error("rbttext_layout_create", rbttext_layout_create((rbttext_layout *){0}));
    calls++;
    rbttext_layout_destroy((rbttext_layout){0});
    calls++;
    expect_error("rbttext_layout_get_glyphs", rbttext_layout_get_glyphs((rbttext_layout){0}, (const RBTTEXT_GLYPH **){0}, (uint32_t *){0}));
    calls++;
    expect_error("rbttext_layout_get_info", rbttext_layout_get_info((rbttext_layout){0}, (RBTTEXT_LAYOUT_INFO *){0}));
    calls++;
    expect_error("rbttext_layout_get_lines", rbttext_layout_get_lines((rbttext_layout){0}, (const RBTTEXT_LINE **){0}, (uint32_t *){0}));
    calls++;
    expect_error("rbttext_layout_get_runs", rbttext_layout_get_runs((rbttext_layout){0}, (const RBTTEXT_RUN **){0}, (uint32_t *){0}));
    calls++;
    expect_error("rbttext_layout_get_strokes", rbttext_layout_get_strokes((rbttext_layout){0}, (RBTTEXT_SEGMENT *){0}, (uint32_t){0}, (uint32_t *){0}));
    calls++;
    printf("rbt_misuse_tests: %d calls with zeroes, %d failure(s)\n", calls, failures);
#if _DEBUG && !MISUSE_SANITIZED
    // Each refusal below prints a line on stderr: they are expected.
    const int zero_failures = failures;
#if defined(_MSC_VER)
    // The check reads a destroyed handle's memory. From here on the debug
    // CRT keeps every block freed, filled and never given back to the
    // Windows heap, which decommits freed pages: reading one faults (one
    // run in about forty did, 2026-10-31). carrottus's harness keeps its
    // freed blocks through its allocator; Rabbittus uses malloc.
    _CrtSetDbgFlag(_CrtSetDbgFlag(_CRTDBG_REPORT_FLAG) | _CRTDBG_DELAY_FREE_MEM_DF);
#endif
    calls_with_destroyed_handles();
    printf("rbt_misuse_tests: %d calls with a destroyed handle (of 144 generated), %d failure(s)\n", destroyed_calls, failures - zero_failures);
#elif _DEBUG
    printf("rbt_misuse_tests: calls with a destroyed handle skipped: under a sanitizer, whose heap reports the read of freed memory itself\n");
#else
    printf("rbt_misuse_tests: calls with a destroyed handle skipped: a Release build doesn't check for one\n");
#endif
    return failures ? 1 : 0;
}
