// Written by hand, beside make_misuse_tests.py: for each handle type of the
// modules that run without a GPU, a factory that makes one through the
// public API with the smallest valid desc, then destroys it, and returns
// the destroyed handle (the workspace's DECISIONS.md, card X16). NULL if it
// couldn't be made, said on stdout. tests/apri_misuse_tests.c, generated,
// calls every public function whose first parameter is one of these types
// with a fresh destroyed handle, in a Debug build not under a sanitizer.
//
// A factory destroys what it made to get there (a codec table, a mixer)
// after the handle it returns, or before when the handle needs it gone
// first, as an entry reader goes before its reader.
//
// Aprend's handle types have no factory here: they need a GPU device.
// apricottests' test_destroyed_handles calls Aprend's.

#ifndef MISUSE_FACTORIES_H
#define MISUSE_FACTORIES_H

#include "archive/aparchive.h"
#include "audio/apaudio.h"
#include "audio/apaudiocues.h"
#include "jobs/apjobs.h"
#include "math/apmathbvh.h"
#include "math/apmathpoly.h"
#include "sync/apsyncnet.h"
#include "text/aptext.h"
#include <stdio.h>
#include <string.h>

static void factory_failed(const char *type, const char *step, APRESULT result)
{
    printf("misuse factory for %s: %s returned %d\n", type, step, (int)result);
}

// ---- ApArchive ----

static aparchive_codecs dead_aparchive_codecs(void)
{
    aparchive_codecs codecs = NULL;
    APRESULT result = aparchive_codecs_create(&codecs);
    if (result != APRESULT_OK)
    {
        factory_failed("aparchive_codecs", "aparchive_codecs_create", result);
        return NULL;
    }
    aparchive_codecs_destroy(codecs);
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

static aparchive_writer make_misuse_writer(aparchive_codecs codecs, misuse_archive *archive)
{
    APARCHIVE_WRITER_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APARCHIVE_WRITER_DESC;
    desc.sink.user = archive;
    desc.sink.write = misuse_archive_write;
    desc.codecs = codecs;
    aparchive_writer writer = NULL;
    APRESULT result = aparchive_writer_create(&desc, &writer);
    if (result != APRESULT_OK)
        factory_failed("aparchive_writer", "aparchive_writer_create", result);
    return writer;
}

static aparchive_writer dead_aparchive_writer(void)
{
    static misuse_archive archive;
    archive.size = 0;
    aparchive_codecs codecs = NULL;
    if (aparchive_codecs_create(&codecs) != APRESULT_OK)
        return NULL;
    aparchive_writer writer = make_misuse_writer(codecs, &archive);
    aparchive_writer_destroy(writer);
    aparchive_codecs_destroy(codecs);
    return writer;
}

// A reader over a one-entry archive, and the codec table it reads with.
static aparchive_reader make_misuse_reader(misuse_archive *archive, aparchive_codecs *out_codecs)
{
    *out_codecs = NULL;
    archive->size = 0;
    aparchive_codecs codecs = NULL;
    if (aparchive_codecs_create(&codecs) != APRESULT_OK)
        return NULL;
    aparchive_writer writer = make_misuse_writer(codecs, archive);
    if (!writer)
    {
        aparchive_codecs_destroy(codecs);
        return NULL;
    }
    APARCHIVE_ENTRY_DESC entry;
    memset(&entry, 0, sizeof(entry));
    entry.base.type = APRI_STRUCT_TYPE_APARCHIVE_ENTRY_DESC;
    entry.name = "a";
    entry.method = APARCHIVE_METHOD_STORED;
    entry.dos_date = 0x0021;
    APRESULT result = aparchive_writer_add_entry(writer, &entry, "x", 1, NULL);
    if (result == APRESULT_OK)
        result = aparchive_writer_finish(writer, NULL);
    aparchive_writer_destroy(writer);
    if (result != APRESULT_OK)
    {
        factory_failed("aparchive_reader", "writing its archive", result);
        aparchive_codecs_destroy(codecs);
        return NULL;
    }
    APARCHIVE_READER_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APARCHIVE_READER_DESC;
    desc.source.user = archive;
    desc.source.size = archive->size;
    desc.source.read = misuse_archive_read;
    desc.codecs = codecs;
    desc.max_entry_count = UINT32_MAX;
    desc.max_central_directory_size = UINT64_MAX;
    desc.max_entry_uncompressed_size = UINT64_MAX;
    desc.max_total_uncompressed_size = UINT64_MAX;
    desc.max_compression_ratio = UINT32_MAX;
    aparchive_reader reader = NULL;
    result = aparchive_reader_open(&desc, &reader, NULL);
    if (result != APRESULT_OK)
    {
        factory_failed("aparchive_reader", "aparchive_reader_open", result);
        aparchive_codecs_destroy(codecs);
        return NULL;
    }
    *out_codecs = codecs;
    return reader;
}

static aparchive_reader dead_aparchive_reader(void)
{
    static misuse_archive archive;
    aparchive_codecs codecs = NULL;
    aparchive_reader reader = make_misuse_reader(&archive, &codecs);
    if (!reader)
        return NULL;
    aparchive_reader_destroy(reader);
    aparchive_codecs_destroy(codecs);
    return reader;
}

static aparchive_entry_reader dead_aparchive_entry_reader(void)
{
    static misuse_archive archive;
    aparchive_codecs codecs = NULL;
    aparchive_reader reader = make_misuse_reader(&archive, &codecs);
    if (!reader)
        return NULL;
    aparchive_entry_reader entry = NULL;
    APRESULT result = aparchive_reader_open_entry(reader, 0, &entry, NULL);
    if (result != APRESULT_OK)
        factory_failed("aparchive_entry_reader", "aparchive_reader_open_entry", result);
    // An entry reader goes before its reader.
    aparchive_entry_reader_destroy(entry);
    aparchive_reader_destroy(reader);
    aparchive_codecs_destroy(codecs);
    return entry;
}

// ---- ApAudio ----

static const SPUDAUDIO_CHANNEL_POSITION misuse_stereo[2] = {SPUDAUDIO_CHANNEL_POSITION_FL,
                                                            SPUDAUDIO_CHANNEL_POSITION_FR};

static apaudio_mixer make_misuse_mixer(void)
{
    APAUDIO_MIXER_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APAUDIO_MIXER_DESC;
    desc.format.sample_format = SPUDAUDIO_SAMPLE_FORMAT_F32;
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
    desc.resampler = APAUDIO_RESAMPLER_LINEAR;
    desc.limiter = APAUDIO_LIMITER_OFF;
    desc.gain = 1.0f;
    apaudio_mixer mixer = NULL;
    APRESULT result = apaudio_mixer_create(&desc, &mixer, NULL);
    if (result != APRESULT_OK)
        factory_failed("apaudio_mixer", "apaudio_mixer_create", result);
    return mixer;
}

static apaudio_mixer dead_apaudio_mixer(void)
{
    apaudio_mixer mixer = make_misuse_mixer();
    apaudio_mixer_destroy(mixer);
    return mixer;
}

static apaudio_cues dead_apaudio_cues(void)
{
    apaudio_mixer mixer = make_misuse_mixer();
    if (!mixer)
        return NULL;
    APAUDIO_CUES_BUS bus;
    memset(&bus, 0, sizeof(bus));
    bus.name = "master";
    bus.bus = apaudio_mixer_get_master_bus(mixer);
    bus.virtual_behavior = APAUDIO_VIRTUAL_KILL;
    APAUDIO_CUES_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APAUDIO_CUES_DESC;
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
    apaudio_cues cues = NULL;
    APRESULT result = apaudio_cues_create(&desc, &cues, NULL);
    if (result != APRESULT_OK)
        factory_failed("apaudio_cues", "apaudio_cues_create", result);
    apaudio_cues_destroy(cues);
    apaudio_mixer_destroy(mixer);
    return cues;
}

// ---- ApJobs ----

static apjobs_pool dead_apjobs_pool(void)
{
    APJOBS_POOL_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APJOBS_POOL_DESC;
    desc.thread_count = 1;
    desc.max_tasks = 1;
    apjobs_pool pool = NULL;
    APRESULT result = apjobs_pool_create(&desc, &pool, NULL);
    if (result != APRESULT_OK)
        factory_failed("apjobs_pool", "apjobs_pool_create", result);
    apjobs_pool_destroy(pool);
    return pool;
}

// ---- ApMath ----

static const ApriDVec3 misuse_triangle_positions[3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
static const uint32_t misuse_triangle_indices[3] = {0, 1, 2};

static apmath_mesh_bvh make_misuse_mesh_bvh(void)
{
    APMATH_MESH_BVH_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APMATH_MESH_BVH_DESC;
    desc.positions = misuse_triangle_positions;
    desc.position_count = 3;
    desc.indices = misuse_triangle_indices;
    desc.index_count = 3;
    apmath_mesh_bvh bvh = NULL;
    APRESULT result = apmath_mesh_bvh_create(&desc, &bvh);
    if (result != APRESULT_OK)
        factory_failed("apmath_mesh_bvh", "apmath_mesh_bvh_create", result);
    return bvh;
}

static apmath_mesh_bvh dead_apmath_mesh_bvh(void)
{
    apmath_mesh_bvh bvh = make_misuse_mesh_bvh();
    apmath_mesh_bvh_destroy(bvh);
    return bvh;
}

static apmath_instance_bvh dead_apmath_instance_bvh(void)
{
    apmath_mesh_bvh mesh = make_misuse_mesh_bvh();
    if (!mesh)
        return NULL;
    static const ApriDMat4 identity = {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
    APMATH_BVH_INSTANCE instance;
    memset(&instance, 0, sizeof(instance));
    instance.mesh = mesh;
    instance.transform = &identity;
    APMATH_INSTANCE_BVH_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APMATH_INSTANCE_BVH_DESC;
    desc.instances = &instance;
    desc.instance_count = 1;
    apmath_instance_bvh bvh = NULL;
    APRESULT result = apmath_instance_bvh_create(&desc, &bvh);
    if (result != APRESULT_OK)
        factory_failed("apmath_instance_bvh", "apmath_instance_bvh_create", result);
    apmath_instance_bvh_destroy(bvh);
    apmath_mesh_bvh_destroy(mesh);
    return bvh;
}

static apmath_poly dead_apmath_poly(void)
{
    static const APMATH_POLY_POINT square[4] = {{0, 0}, {4, 0}, {4, 4}, {0, 4}};
    static const uint32_t ring_ends[1] = {4};
    APMATH_POLY_BOOLEAN_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APMATH_POLY_BOOLEAN_DESC;
    desc.op = APMATH_POLY_OP_UNION;
    desc.fill = APMATH_POLY_FILL_NON_ZERO;
    desc.subject.points = square;
    desc.subject.point_count = 4;
    desc.subject.ring_ends = ring_ends;
    desc.subject.ring_count = 1;
    apmath_poly poly = NULL;
    APRESULT result = apmath_poly_boolean(&desc, &poly, NULL);
    if (result != APRESULT_OK)
        factory_failed("apmath_poly", "apmath_poly_boolean", result);
    apmath_poly_destroy(poly);
    return poly;
}

// ---- ApSync ----

static SPUD_ALLOCATOR misuse_allocator;

// A session hosting on the loopback address, at a port the system picks.
static apsync_session dead_apsync_session(void)
{
    static const APSYNC_CHANNEL_MODE channels[1] = {APSYNC_CHANNEL_ORDERED};
    static const char *const addresses[1] = {"127.0.0.1"};
    misuse_allocator = spud_allocator_system();
    APSYNC_SESSION_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APSYNC_SESSION_DESC;
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
    APSYNC_HOST_DESC host;
    memset(&host, 0, sizeof(host));
    host.base.type = APRI_STRUCT_TYPE_APSYNC_HOST_DESC;
    host.listen_addresses = addresses;
    host.listen_address_count = 1;
    host.port = 0;
    host.listen_backlog = 1;
    host.max_peers = 1;
    apsync_session session = NULL;
    APRESULT result = apsync_session_host(&desc, &host, &session, NULL);
    if (result != APRESULT_OK)
        factory_failed("apsync_session", "apsync_session_host", result);
    apsync_session_destroy(session);
    return session;
}

// ---- ApText ----

static const ApriVec2 misuse_stroke_points[2] = {{0.0f, 0.0f}, {0.5f, 0.0f}};
static const APTEXT_STROKE_POLYLINE misuse_stroke_polylines[1] = {{0, 2}};
static const APTEXT_STROKE_GLYPH misuse_stroke_glyphs[1] = {{0x2D, 0.5f, 0, 1}};

static aptext_face make_misuse_face(void)
{
    APTEXT_STROKE_FACE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APTEXT_STROKE_FACE_DESC;
    desc.glyphs = misuse_stroke_glyphs;
    desc.glyph_count = 1;
    desc.polylines = misuse_stroke_polylines;
    desc.polyline_count = 1;
    desc.points = misuse_stroke_points;
    desc.point_count = 2;
    desc.metrics.ascender = 0.75f;
    desc.metrics.descender = -0.25f;
    desc.metrics.line_gap = 0.0f;
    aptext_face face = NULL;
    APRESULT result = aptext_face_create_stroke(&desc, &face, NULL);
    if (result != APRESULT_OK)
        factory_failed("aptext_face", "aptext_face_create_stroke", result);
    return face;
}

static aptext_face dead_aptext_face(void)
{
    aptext_face face = make_misuse_face();
    aptext_face_destroy(face);
    return face;
}

static aptext_chain dead_aptext_chain(void)
{
    aptext_face face = make_misuse_face();
    if (!face)
        return NULL;
    APTEXT_CHAIN_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APTEXT_CHAIN_DESC;
    desc.faces = &face;
    desc.face_count = 1;
    aptext_chain chain = NULL;
    APRESULT result = aptext_chain_create(&desc, &chain, NULL);
    if (result != APRESULT_OK)
        factory_failed("aptext_chain", "aptext_chain_create", result);
    aptext_chain_destroy(chain);
    aptext_face_destroy(face);
    return chain;
}

static aptext_layout dead_aptext_layout(void)
{
    aptext_layout layout = NULL;
    APRESULT result = aptext_layout_create(&layout);
    if (result != APRESULT_OK)
        factory_failed("aptext_layout", "aptext_layout_create", result);
    aptext_layout_destroy(layout);
    return layout;
}

static aptext_atlas dead_aptext_atlas(void)
{
    APTEXT_ATLAS_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APTEXT_ATLAS_DESC;
    desc.pixels_per_em = 16.0f;
    desc.distance_range = 2.0f;
    desc.page_width = 64;
    desc.page_height = 64;
    desc.max_pages = 1;
    aptext_atlas atlas = NULL;
    APRESULT result = aptext_atlas_create(&desc, &atlas, NULL);
    if (result != APRESULT_OK)
        factory_failed("aptext_atlas", "aptext_atlas_create", result);
    aptext_atlas_destroy(atlas);
    return atlas;
}

#endif // MISUSE_FACTORIES_H
