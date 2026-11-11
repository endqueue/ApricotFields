// Written by hand, beside make_misuse_tests.py: for each handle type of the
// modules that run without a GPU, a factory that makes one through the
// public API with the smallest valid desc, then destroys it, and returns
// the destroyed handle (the workspace's DECISIONS.md, card X16). NULL if it
// couldn't be made, said on stdout. tests/rbt_misuse_tests.c, generated,
// calls every public function whose first parameter is one of these types
// with a fresh destroyed handle, in a Debug build not under a sanitizer.
//
// A factory destroys what it made to get there (a codec table, a mixer)
// after the handle it returns, or before when the handle needs it gone
// first, as an entry reader goes before its reader.
//
// Rend's handle types have no factory here: they need a GPU device.
// apricottests' test_destroyed_handles calls Rend's.

#ifndef MISUSE_FACTORIES_H
#define MISUSE_FACTORIES_H

#include "archive/rbtarchive.h"
#include "audio/rbtaudio.h"
#include "audio/rbtaudiocues.h"
#include "jobs/rbtjobs.h"
#include "math/rbtmathbvh.h"
#include "math/rbtmathpoly.h"
#include "sync/rbtsyncnet.h"
#include "text/rbttext.h"
#include <stdio.h>
#include <string.h>

static void factory_failed(const char *type, const char *step, RBT_RESULT result)
{
    printf("misuse factory for %s: %s returned %d\n", type, step, (int)result);
}

// ---- Rabbittus Archive ----

static rbtarchive_codecs dead_rbtarchive_codecs(void)
{
    rbtarchive_codecs codecs = NULL;
    RBT_RESULT result = rbtarchive_codecs_create(&codecs);
    if (result != RBT_RESULT_OK)
    {
        factory_failed("rbtarchive_codecs", "rbtarchive_codecs_create", result);
        return NULL;
    }
    rbtarchive_codecs_destroy(codecs);
    return codecs;
}

// An archive written into memory, for the readers.
typedef struct misuse_archive
{
    uint8_t bytes[1024];
    uint64_t size;
} misuse_archive;

static bool misuse_archive_write(void *user, const void *data, uint64_t count)
{
    misuse_archive *archive = (misuse_archive *)user;
    if (count > sizeof(archive->bytes) - archive->size)
        return false;
    memcpy(archive->bytes + archive->size, data, (size_t)count);
    archive->size += count;
    return true;
}

static bool misuse_archive_read(void *user, uint64_t offset, void *buffer, uint64_t count)
{
    const misuse_archive *archive = (const misuse_archive *)user;
    if (offset > archive->size || count > archive->size - offset)
        return false;
    memcpy(buffer, archive->bytes + offset, (size_t)count);
    return true;
}

static rbtarchive_writer make_misuse_writer(rbtarchive_codecs codecs, misuse_archive *archive)
{
    RBTARCHIVE_WRITER_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_ARCHIVE_WRITER_DESC;
    desc.sink.user = archive;
    desc.sink.write = misuse_archive_write;
    desc.codecs = codecs;
    rbtarchive_writer writer = NULL;
    RBT_RESULT result = rbtarchive_writer_create(&desc, &writer);
    if (result != RBT_RESULT_OK)
        factory_failed("rbtarchive_writer", "rbtarchive_writer_create", result);
    return writer;
}

static rbtarchive_writer dead_rbtarchive_writer(void)
{
    static misuse_archive archive;
    archive.size = 0;
    rbtarchive_codecs codecs = NULL;
    if (rbtarchive_codecs_create(&codecs) != RBT_RESULT_OK)
        return NULL;
    rbtarchive_writer writer = make_misuse_writer(codecs, &archive);
    rbtarchive_writer_destroy(writer);
    rbtarchive_codecs_destroy(codecs);
    return writer;
}

// A reader over a one-entry archive, and the codec table it reads with.
static rbtarchive_reader make_misuse_reader(misuse_archive *archive, rbtarchive_codecs *out_codecs)
{
    *out_codecs = NULL;
    archive->size = 0;
    rbtarchive_codecs codecs = NULL;
    if (rbtarchive_codecs_create(&codecs) != RBT_RESULT_OK)
        return NULL;
    rbtarchive_writer writer = make_misuse_writer(codecs, archive);
    if (!writer)
    {
        rbtarchive_codecs_destroy(codecs);
        return NULL;
    }
    RBTARCHIVE_ENTRY_DESC entry;
    memset(&entry, 0, sizeof(entry));
    entry.base.type = RBT_STRUCT_TYPE_ARCHIVE_ENTRY_DESC;
    entry.name = "a";
    entry.method = RBTARCHIVE_METHOD_STORED;
    entry.dos_date = 0x0021;
    RBT_RESULT result = rbtarchive_writer_add_entry(writer, &entry, "x", 1, NULL);
    if (result == RBT_RESULT_OK)
        result = rbtarchive_writer_finish(writer, NULL);
    rbtarchive_writer_destroy(writer);
    if (result != RBT_RESULT_OK)
    {
        factory_failed("rbtarchive_reader", "writing its archive", result);
        rbtarchive_codecs_destroy(codecs);
        return NULL;
    }
    RBTARCHIVE_READER_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_ARCHIVE_READER_DESC;
    desc.source.user = archive;
    desc.source.size = archive->size;
    desc.source.read = misuse_archive_read;
    desc.codecs = codecs;
    desc.max_entry_count = UINT32_MAX;
    desc.max_central_directory_size = UINT64_MAX;
    desc.max_entry_uncompressed_size = UINT64_MAX;
    desc.max_total_uncompressed_size = UINT64_MAX;
    desc.max_compression_ratio = UINT32_MAX;
    rbtarchive_reader reader = NULL;
    result = rbtarchive_reader_open(&desc, &reader, NULL);
    if (result != RBT_RESULT_OK)
    {
        factory_failed("rbtarchive_reader", "rbtarchive_reader_open", result);
        rbtarchive_codecs_destroy(codecs);
        return NULL;
    }
    *out_codecs = codecs;
    return reader;
}

static rbtarchive_reader dead_rbtarchive_reader(void)
{
    static misuse_archive archive;
    rbtarchive_codecs codecs = NULL;
    rbtarchive_reader reader = make_misuse_reader(&archive, &codecs);
    if (!reader)
        return NULL;
    rbtarchive_reader_destroy(reader);
    rbtarchive_codecs_destroy(codecs);
    return reader;
}

static rbtarchive_entry_reader dead_rbtarchive_entry_reader(void)
{
    static misuse_archive archive;
    rbtarchive_codecs codecs = NULL;
    rbtarchive_reader reader = make_misuse_reader(&archive, &codecs);
    if (!reader)
        return NULL;
    rbtarchive_entry_reader entry = NULL;
    RBT_RESULT result = rbtarchive_reader_open_entry(reader, 0, &entry, NULL);
    if (result != RBT_RESULT_OK)
        factory_failed("rbtarchive_entry_reader", "rbtarchive_reader_open_entry", result);
    // An entry reader goes before its reader.
    rbtarchive_entry_reader_destroy(entry);
    rbtarchive_reader_destroy(reader);
    rbtarchive_codecs_destroy(codecs);
    return entry;
}

// ---- Rabbittus Audio ----

static const CRAUDIO_CHANNEL_POSITION misuse_stereo[2] = {CRAUDIO_CHANNEL_POSITION_FL,
                                                            CRAUDIO_CHANNEL_POSITION_FR};

static rbtaudio_mixer make_misuse_mixer(void)
{
    RBTAUDIO_MIXER_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_AUDIO_MIXER_DESC;
    desc.format.sample_format = CRAUDIO_SAMPLE_FORMAT_F32;
    desc.format.sample_rate = 48000;
    desc.format.channel_count = 2;
    desc.format.interleaved = true;
    desc.format.positions = misuse_stereo;
    desc.max_frames = 64;
    desc.max_voices = 4;
    desc.max_real_voices = 2;
    desc.max_fading_voices = 1;
    desc.transition_fade_us = 1000;
    desc.max_sounds = 2;
    desc.max_concurrency_groups = 1;
    desc.max_buses = 2;
    desc.max_modifiers = 1;
    desc.queue_capacity = 4;
    desc.resampler = RBTAUDIO_RESAMPLER_LINEAR;
    desc.limiter = RBTAUDIO_LIMITER_OFF;
    desc.gain = 1.0f;
    rbtaudio_mixer mixer = NULL;
    RBT_RESULT result = rbtaudio_mixer_create(&desc, &mixer, NULL);
    if (result != RBT_RESULT_OK)
        factory_failed("rbtaudio_mixer", "rbtaudio_mixer_create", result);
    return mixer;
}

static rbtaudio_mixer dead_rbtaudio_mixer(void)
{
    rbtaudio_mixer mixer = make_misuse_mixer();
    rbtaudio_mixer_destroy(mixer);
    return mixer;
}

static rbtaudio_cues dead_rbtaudio_cues(void)
{
    rbtaudio_mixer mixer = make_misuse_mixer();
    if (!mixer)
        return NULL;
    RBTAUDIO_CUES_BUS bus;
    memset(&bus, 0, sizeof(bus));
    bus.name = "master";
    bus.bus = rbtaudio_mixer_get_master_bus(mixer);
    bus.virtual_behavior = RBTAUDIO_VIRTUAL_KILL;
    RBTAUDIO_CUES_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_AUDIO_CUES_DESC;
    desc.mixer = mixer;
    desc.buses = &bus;
    desc.bus_count = 1;
    desc.max_cues = 1;
    desc.max_playings = 1;
    desc.max_voices = 4;
    desc.sample_rate = 48000;
    desc.max_bank_size = 1024;
    desc.duck_fade_in_us = 1000;
    desc.duck_fade_out_us = 1000;
    rbtaudio_cues cues = NULL;
    RBT_RESULT result = rbtaudio_cues_create(&desc, &cues, NULL);
    if (result != RBT_RESULT_OK)
        factory_failed("rbtaudio_cues", "rbtaudio_cues_create", result);
    rbtaudio_cues_destroy(cues);
    rbtaudio_mixer_destroy(mixer);
    return cues;
}

// ---- Rabbittus Jobs ----

static rbtjobs_pool dead_rbtjobs_pool(void)
{
    RBTJOBS_POOL_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_JOBS_POOL_DESC;
    desc.thread_count = 1;
    desc.max_tasks = 1;
    rbtjobs_pool pool = NULL;
    RBT_RESULT result = rbtjobs_pool_create(&desc, &pool, NULL);
    if (result != RBT_RESULT_OK)
        factory_failed("rbtjobs_pool", "rbtjobs_pool_create", result);
    rbtjobs_pool_destroy(pool);
    return pool;
}

// ---- Rabbittus Math ----

static const RbtDVec3 misuse_triangle_positions[3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
static const uint32_t misuse_triangle_indices[3] = {0, 1, 2};

static rbtmath_mesh_bvh make_misuse_mesh_bvh(void)
{
    RBTMATH_MESH_BVH_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_MATH_MESH_BVH_DESC;
    desc.positions = misuse_triangle_positions;
    desc.position_count = 3;
    desc.indices = misuse_triangle_indices;
    desc.index_count = 3;
    rbtmath_mesh_bvh bvh = NULL;
    RBT_RESULT result = rbtmath_mesh_bvh_create(&desc, &bvh);
    if (result != RBT_RESULT_OK)
        factory_failed("rbtmath_mesh_bvh", "rbtmath_mesh_bvh_create", result);
    return bvh;
}

static rbtmath_mesh_bvh dead_rbtmath_mesh_bvh(void)
{
    rbtmath_mesh_bvh bvh = make_misuse_mesh_bvh();
    rbtmath_mesh_bvh_destroy(bvh);
    return bvh;
}

static rbtmath_instance_bvh dead_rbtmath_instance_bvh(void)
{
    rbtmath_mesh_bvh mesh = make_misuse_mesh_bvh();
    if (!mesh)
        return NULL;
    static const RbtDMat4 identity = {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
    RBTMATH_BVH_INSTANCE instance;
    memset(&instance, 0, sizeof(instance));
    instance.mesh = mesh;
    instance.transform = &identity;
    RBTMATH_INSTANCE_BVH_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_MATH_INSTANCE_BVH_DESC;
    desc.instances = &instance;
    desc.instance_count = 1;
    rbtmath_instance_bvh bvh = NULL;
    RBT_RESULT result = rbtmath_instance_bvh_create(&desc, &bvh);
    if (result != RBT_RESULT_OK)
        factory_failed("rbtmath_instance_bvh", "rbtmath_instance_bvh_create", result);
    rbtmath_instance_bvh_destroy(bvh);
    rbtmath_mesh_bvh_destroy(mesh);
    return bvh;
}

static rbtmath_box_bvh dead_rbtmath_box_bvh(void)
{
    RBTMATH_BVH_BOX box;
    memset(&box, 0, sizeof(box));
    box.box.hi.x = box.box.hi.y = box.box.hi.z = 1.0;
    box.mask = 1;
    RBTMATH_BOX_BVH_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_MATH_BOX_BVH_DESC;
    desc.boxes = &box;
    desc.box_count = 1;
    rbtmath_box_bvh bvh = NULL;
    RBT_RESULT result = rbtmath_box_bvh_create(&desc, &bvh, NULL);
    if (result != RBT_RESULT_OK)
        factory_failed("rbtmath_box_bvh", "rbtmath_box_bvh_create", result);
    rbtmath_box_bvh_destroy(bvh);
    return bvh;
}

static rbtmath_poly dead_rbtmath_poly(void)
{
    static const RBTMATH_POLY_POINT square[4] = {{0, 0}, {4, 0}, {4, 4}, {0, 4}};
    static const uint32_t ring_ends[1] = {4};
    RBTMATH_POLY_BOOLEAN_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_MATH_POLY_BOOLEAN_DESC;
    desc.op = RBTMATH_POLY_OP_UNION;
    desc.fill = RBTMATH_POLY_FILL_NON_ZERO;
    desc.subject.points = square;
    desc.subject.point_count = 4;
    desc.subject.ring_ends = ring_ends;
    desc.subject.ring_count = 1;
    rbtmath_poly poly = NULL;
    RBT_RESULT result = rbtmath_poly_boolean(&desc, &poly, NULL);
    if (result != RBT_RESULT_OK)
        factory_failed("rbtmath_poly", "rbtmath_poly_boolean", result);
    rbtmath_poly_destroy(poly);
    return poly;
}

// ---- Rabbittus Sync ----

static CR_ALLOCATOR misuse_allocator;

// A session hosting on the loopback address, at a port the system picks.
static rbtsync_session dead_rbtsync_session(void)
{
    static const RBTSYNC_CHANNEL_MODE channels[1] = {RBTSYNC_CHANNEL_ORDERED};
    static const char *const addresses[1] = {"127.0.0.1"};
    misuse_allocator = cr_allocator_system();
    RBTSYNC_SESSION_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_SYNC_SESSION_DESC;
    desc.allocator = &misuse_allocator;
    desc.application_id = 0x4D4953555345ull;
    desc.application_version = 1;
    desc.channels = channels;
    desc.channel_count = 1;
    desc.peer_info = "misuse";
    desc.peer_info_size = 6;
    desc.max_message_size = 64;
    desc.max_backlog_size = 1024;
    desc.heartbeat_interval_ms = 200;
    desc.peer_timeout_ms = 1000;
    desc.handshake_timeout_ms = 1000;
    RBTSYNC_HOST_DESC host;
    memset(&host, 0, sizeof(host));
    host.base.type = RBT_STRUCT_TYPE_SYNC_HOST_DESC;
    host.listen_addresses = addresses;
    host.listen_address_count = 1;
    host.port = 0;
    host.listen_backlog = 1;
    host.max_peers = 1;
    rbtsync_session session = NULL;
    RBT_RESULT result = rbtsync_session_host(&desc, &host, &session, NULL);
    if (result != RBT_RESULT_OK)
        factory_failed("rbtsync_session", "rbtsync_session_host", result);
    rbtsync_session_destroy(session);
    return session;
}

// ---- Rabbittus Text ----

static const RbtVec2 misuse_stroke_points[2] = {{0.0f, 0.0f}, {0.5f, 0.0f}};
static const RBTTEXT_STROKE_POLYLINE misuse_stroke_polylines[1] = {{0, 2}};
static const RBTTEXT_STROKE_GLYPH misuse_stroke_glyphs[1] = {{0x2D, 0.5f, 0, 1}};

static rbttext_face make_misuse_face(void)
{
    RBTTEXT_STROKE_FACE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_TEXT_STROKE_FACE_DESC;
    desc.glyphs = misuse_stroke_glyphs;
    desc.glyph_count = 1;
    desc.polylines = misuse_stroke_polylines;
    desc.polyline_count = 1;
    desc.points = misuse_stroke_points;
    desc.point_count = 2;
    desc.metrics.ascender = 0.75f;
    desc.metrics.descender = -0.25f;
    desc.metrics.line_gap = 0.0f;
    rbttext_face face = NULL;
    RBT_RESULT result = rbttext_face_create_stroke(&desc, &face, NULL);
    if (result != RBT_RESULT_OK)
        factory_failed("rbttext_face", "rbttext_face_create_stroke", result);
    return face;
}

static rbttext_face dead_rbttext_face(void)
{
    rbttext_face face = make_misuse_face();
    rbttext_face_destroy(face);
    return face;
}

static rbttext_chain dead_rbttext_chain(void)
{
    rbttext_face face = make_misuse_face();
    if (!face)
        return NULL;
    RBTTEXT_CHAIN_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_TEXT_CHAIN_DESC;
    desc.faces = &face;
    desc.face_count = 1;
    rbttext_chain chain = NULL;
    RBT_RESULT result = rbttext_chain_create(&desc, &chain, NULL);
    if (result != RBT_RESULT_OK)
        factory_failed("rbttext_chain", "rbttext_chain_create", result);
    rbttext_chain_destroy(chain);
    rbttext_face_destroy(face);
    return chain;
}

static rbttext_layout dead_rbttext_layout(void)
{
    rbttext_layout layout = NULL;
    RBT_RESULT result = rbttext_layout_create(&layout);
    if (result != RBT_RESULT_OK)
        factory_failed("rbttext_layout", "rbttext_layout_create", result);
    rbttext_layout_destroy(layout);
    return layout;
}

static rbttext_atlas dead_rbttext_atlas(void)
{
    RBTTEXT_ATLAS_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_TEXT_ATLAS_DESC;
    desc.pixels_per_em = 16.0f;
    desc.distance_range = 2.0f;
    desc.page_width = 64;
    desc.page_height = 64;
    desc.max_pages = 1;
    rbttext_atlas atlas = NULL;
    RBT_RESULT result = rbttext_atlas_create(&desc, &atlas, NULL);
    if (result != RBT_RESULT_OK)
        factory_failed("rbttext_atlas", "rbttext_atlas_create", result);
    rbttext_atlas_destroy(atlas);
    return atlas;
}

#endif // MISUSE_FACTORIES_H
