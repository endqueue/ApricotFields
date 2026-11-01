#ifndef APREND_RAYTRACING_INTERNAL_HPP
#define APREND_RAYTRACING_INTERNAL_HPP

/* Internal header: never included outside ApricotFields/src/render/. The
 * structs behind aprend_blas and aprend_tlas (aprendraytracing.h), the
 * instance's tables they and the lit materials take slots in, and what the
 * four ray tracing commands' compile functions (aprendcommands.cpp) call.
 *
 * Who writes each GPU-visible buffer, when, and what keeps a frame in flight
 * from seeing the write:
 * - The BLAS table (aprend_rt_state::blas_table): an entry is written by the
 *   GPU, by a copy recorded in place of the BUILD_BLAS that builds or
 *   compacts its BLAS, from the current frame's transient memory. Queue
 *   order keeps every earlier build's reading of the old entry before it.
 * - The geometry arena (card A41): a BLAS's range is written by the copies a
 *   BUILD or REFIT of it records, from its geometries' buffers, in the
 *   command's place on the queue.
 * - The lit-material table (D4): an entry is staged in the CPU's copy by
 *   aprend_lit_material_set_values() and copied from transient memory into
 *   the table by the next submission of a list that reads the table, ahead
 *   of that list's commands; a list submitted before reads the values it
 *   was submitted after.
 * - A TLAS's instance sets (D3, T5): UPLOAD memory, written by the CPU only
 *   while the set is being prepared, read by the GPU only by the build that
 *   sealed it; a sealed set is written again only once that build's
 *   submission has completed (aprend_rt_set::serial).
 * - A TLAS's current records and current material table: device memory the
 *   GPU writes at each of its builds, copied from the set's written blocks;
 *   the blocks it didn't write are those of the builds before it, in place
 *   (carried over). Builds of one TLAS run in the order they were sent (T5),
 *   so each build's copy lands on the one before it.
 * - A TLAS's device instances: written by Aprend's instance writer at each
 *   build, read by that build; its emitter table by the emitter pass of the
 *   first path trace after anything it reads changed.
 * Every one of those GPU writes is ordered before what reads it by a barrier
 * the compile records. */

#include "aprend_internal.hpp"
#include "aprendranges_internal.hpp"
#include "render/aprendraytracing.h"

/* ---- Fixed values of the design (aprend_raytracing_draft.h) ------------ */

/* Records, and material table entries, a block of a set's written bitset
 * (D3). */
#define APREND_RT_BLOCK 64u
/* A slot's low 24 bits are its index, its high 8 its generation (D1, D4). */
#define APREND_RT_SLOT_INDEX_MASK 0xFFFFFFu
#define APREND_RT_SLOT_GENERATION_SHIFT 24u
/* A record's `_material_base` is below this (D4). */
#define APREND_RT_MATERIAL_BASE_LIMIT (1u << 24)
/* A record's `_mask` is at most this. */
#define APREND_RT_MASK_LIMIT 255u
/* LIT_SHADOW_MASK's `_filter_radius` is at most this. */
#define APREND_RT_MAX_FILTER_RADIUS 8u
/* Bytes from one BLAS vertex to the next: an APREND_LIT_VERTEX's (card A42). */
#define APREND_RT_VERTEX_STRIDE 48u
/* The geometry arena is handed out in units of this many bytes (a uint4). */
#define APREND_RT_ARENA_UNIT 16u
/* What the tables and the arena start at; each doubles when it fills. */
#define APREND_RT_FIRST_TABLE_ENTRIES 256u
#define APREND_RT_FIRST_ARENA_UNITS 65536u
/* The arena's most: 4 GiB, what the shaders' 32-bit byte offsets reach. */
#define APREND_RT_ARENA_MAX_UNITS (1u << 28)
/* The groups of Aprend's ray tracing compute shaders: as their numthreads. */
#define APREND_RT_WRITER_GROUP 64u
#define APREND_RT_TRACE_GROUP 8u
#define APREND_RT_EMITTER_GROUP 256u

/* What every ray tracing call and command checks first: the device was
 * created with SPUDGPU_FEATURE_RAY_QUERY. */
static inline bool aprend_rt_supported(aprend_instance instance) {
	return (instance->features & SPUDGPU_FEATURE_RAY_QUERY) != 0;
}

/* The message of every refusal for a device without ray queries, after the
 * call's name. */
#define APREND_RT_UNSUPPORTED_MESSAGE "the device was not created with SPUDGPU_FEATURE_RAY_QUERY"

/* The name of one of ray tracing's four commands, for a message. */
static inline const char *aprend_rt_command_name(APREND_COMMAND_TYPE type) {
	switch (type) {
	case APREND_COMMAND_BUILD_BLAS:
		return "BUILD_BLAS";
	case APREND_COMMAND_BUILD_TLAS:
		return "BUILD_TLAS";
	case APREND_COMMAND_LIT_SHADOW_MASK:
		return "LIT_SHADOW_MASK";
	case APREND_COMMAND_LIT_PATH_TRACE:
		return "LIT_PATH_TRACE";
	default:
		return "a command";
	}
}

static inline uint32_t aprend_rt_slot(uint32_t index, uint32_t generation) {
	return index | (generation << APREND_RT_SLOT_GENERATION_SHIFT);
}
static inline uint32_t aprend_rt_slot_index(uint32_t slot) { return slot & APREND_RT_SLOT_INDEX_MASK; }
static inline uint32_t aprend_rt_slot_generation(uint32_t slot) { return slot >> APREND_RT_SLOT_GENERATION_SHIFT; }

/* ---- What the GPU reads (each matched in shaders/aprend_rt_common.slang) -- */

/* One entry of the BLAS table, at a BLAS's index (D5): its current
 * structure's reference (written by each build and compaction), and where
 * its geometries' headers start in the arena, in 32-bit words. 16 bytes. */
struct aprend_rt_blas_entry {
	uint32_t reference[2];
	uint32_t arena_word;
	uint32_t geometry_count;
};
APRI_PLAIN_DATA(aprend_rt_blas_entry);
static_assert(sizeof(aprend_rt_blas_entry) == 16, "BlasEntry in aprend_rt_common.slang");

/* One geometry of a BLAS in the arena, ahead of its vertices: where its
 * vertices (APREND_LIT_VERTEX, 12 words each) and its 32-bit indices start,
 * in words; its triangles; whether it has indices. 16 bytes. */
struct aprend_rt_geometry_header {
	uint32_t vertex_word;
	uint32_t index_word;
	uint32_t triangle_count;
	uint32_t indexed;
};
APRI_PLAIN_DATA(aprend_rt_geometry_header);
static_assert(sizeof(aprend_rt_geometry_header) == 16, "GeometryHeader in aprend_rt_common.slang");

/* One entry of the lit-material table (D4): the values a path trace reads
 * (aprendcommands.h, LIT_PATH_TRACE), from the material's block: base
 * color, emission, then roughness, metalness and specular. 48 bytes. */
struct aprend_rt_lit_entry {
	float base_color[4];
	float emission[4];
	float surface[4];
};
APRI_PLAIN_DATA(aprend_rt_lit_entry);
static_assert(sizeof(aprend_rt_lit_entry) == 48, "LitEntry in aprend_rt_common.slang");

/* The push constants of the instance writers (aprend_rt_instances_*_comp). */
struct aprend_rt_writer_params {
	uint32_t count;
	uint32_t instance_size;
	uint32_t padding[2];
};
static_assert(sizeof(aprend_rt_writer_params) == 16, "WriterParams in aprend_rt_common.slang");

/* The push constants of the emitter pass (aprend_rt_emitters_comp): the
 * instances of the TLAS's last build, its material table's length. */
struct aprend_rt_emitter_params {
	uint32_t count;
	uint32_t material_count;
	uint32_t padding[2];
};
static_assert(sizeof(aprend_rt_emitter_params) == 16, "EmitterParams in aprend_rt_emitters_comp.slang");

/* The push constants of the shadow mask (aprend_rt_shadow_mask_*_comp). */
struct aprend_rt_mask_params {
	uint32_t size[2];
	uint32_t ray_count;
	uint32_t filter_radius;
	uint32_t cull_mask;
	float normal_offset;
	float max_distance;
	uint32_t light;
};
static_assert(sizeof(aprend_rt_mask_params) == 32, "MaskParams in aprend_rt_shadow_mask.slang");

/* The push constants of the path tracer (aprend_rt_path_trace_comp). */
struct aprend_rt_trace_params {
	uint32_t size[2];
	uint32_t samples;
	uint32_t accumulated;
	uint32_t seed;
	uint32_t max_bounces;
	uint32_t cull_mask;
	float clamp;
	uint32_t guides;
	uint32_t instance_count;
	uint32_t material_count;
	uint32_t padding;
};
static_assert(sizeof(aprend_rt_trace_params) == 48, "TraceParams in aprend_rt_path_trace_comp.slang");

/* ---- The instance's tables -------------------------------------------- */

/* What an instance with SPUDGPU_FEATURE_RAY_QUERY keeps for ray tracing,
 * made by its first BLAS, TLAS or lit material (aprend_rt_state_get) and
 * destroyed with it. Each table is a structure of arrays on the CPU side:
 * a check of a record reads two dense arrays of bytes (D1), and nothing a
 * hot loop reads sits beside a pointer or a name. Index 0 of each is never
 * handed out, so 0 is never a slot. */
struct aprend_rt_state {
	/* The device's limits and its instance layout (card S14), read once. */
	SPUDGPU_RAY_QUERY_LIMITS limits{};
	spudgpu_acceleration_structure_instance_layout instance_layout{};

	/* The BLAS table (D1, D5): per index, its 8-bit generation, whether a
	 * BLAS holds it, whether a build of it has been sent (T6), and the
	 * BLAS's current structure (what a TLAS build names for residency). The
	 * Debug build keeps a 32-bit generation beside it, to name a slot whose
	 * 8-bit generation has wrapped (T6). Grown by doubling, with the GPU
	 * table, by aprend_blas_create() and only then. */
	eq::array<uint8_t> blas_generation{};
	eq::array<uint8_t> blas_live{};
	eq::array<uint8_t> blas_built{};
	eq::array<uint32_t> blas_generation_debug{};
	eq::array<spudgpu_acceleration_structure> blas_structure{};
	/* Per index, its BLAS's geometry count: how far a record's range of a
	 * material table reaches (T7). */
	eq::array<uint32_t> blas_geometry_count{};
	eq::array<uint32_t> blas_free{};
	/* Indices whose BLAS was destroyed, from any thread, not yet taken into
	 * the table: a destroy only adds to this, under the instance's
	 * release_mutex, and the calls on the instance (create, write, seal)
	 * take them in first (aprend_rt_drain), so the table is changed only by
	 * the instance's one caller. Room for every index. */
	eq::array<uint32_t> blas_freed{};
	aprend_buffer blas_table{nullptr};

	/* The geometry arena (card A41): one storage buffer, handed out in
	 * APREND_RT_ARENA_UNIT units, a range a BLAS for its life. */
	aprend_buffer arena{nullptr};
	aprend_range_list arena_ranges{};

	/* The lit-material table (D4): per index, the generation and whether a
	 * material holds it; the entry as last set, and whether it changed since
	 * it was last copied, with the list of those that did (room for every
	 * index, so marking one never allocates). */
	eq::array<uint8_t> lit_generation{};
	eq::array<uint8_t> lit_live{};
	eq::array<uint32_t> lit_free{};
	eq::array<aprend_rt_lit_entry> lit_entries{};
	eq::array<uint8_t> lit_dirty{};
	eq::array<uint32_t> lit_dirty_list{};
	aprend_buffer lit_table{nullptr};
	/* Bumped by each change of a lit material's values: what a TLAS's
	 * emitter table was built against (D6). */
	uint64_t lit_version{1};
	/* What a staged entry's copy clears once submitted (aprend_upload_copy's
	 * dirty): the list itself is emptied then (aprend_rt_submitted). */
	bool lit_copied{false};

	/* Bumped whenever a table or the arena is replaced by a larger one: a
	 * set made over the old one is made again. */
	uint64_t buffers_version{1};

	/* The builds' scratch (T8): one buffer, grown to the largest a compiled
	 * list needs. */
	aprend_buffer scratch{nullptr};

	/* Aprend's ray tracing shaders, layouts and pipelines, each made as first
	 * needed. The writers' set 0 (the TLAS's buffers), the emitter pass's,
	 * the traces' set 1 (a TLAS's set, aprend_tlas_t::trace_set), and the
	 * output layouts of the mask and the path tracer. */
	aprend_shader writer_row64{nullptr};
	aprend_shader writer_col72{nullptr};
	aprend_binding_layout writer_layout{nullptr};
	aprend_compute_pipeline writer_pipeline{nullptr};
	aprend_shader emitter_comp{nullptr};
	aprend_binding_layout emitter_layout{nullptr};
	aprend_compute_pipeline emitter_pipeline{nullptr};
	aprend_shader mask_comps[2]{};
	aprend_binding_layout scene_layout{nullptr};
	aprend_binding_layout trace_layout{nullptr};
	aprend_binding_layout mask_output_layout{nullptr};
	aprend_compute_pipeline mask_pipelines[2]{};
	/* The path tracer's four variants, by its guides' formats (bit 1 an
	 * albedo of R16G16B16A16_FLOAT, bit 0 a normal of it). */
	aprend_shader path_comps[4]{};
	aprend_binding_layout path_output_layout{nullptr};
	aprend_compute_pipeline path_pipelines[4]{};
	/* Every TLAS alive, by address and serial: what a list's reset reads
	 * before it reaches a TLAS through a seal, which may name one destroyed
	 * since. Under the instance's release_mutex. */
	struct live_tlas {
		const void *tlas;
		uint64_t serial;
	};
	eq::array<live_tlas> live_tlases{};
	/* What an absent guide writes into: a 1x1 RGBA32F storage texture. */
	aprend_texture2d no_guide{nullptr};
	aprend_texture_view no_guide_view{nullptr};
};

/* The instance's ray tracing state, made if it isn't. NULL, [error] saying
 * why, if it can't be. */
aprend_rt_state *aprend_rt_state_get(aprend_instance instance, APREND_ERROR *error);

/* From the instance's destructor: the handles before its builtins go
 * (destroy), the tables once the release queue has run (free), since a lit
 * material released then gives its slot back. */
void aprend_rt_state_destroy(aprend_instance instance);
void aprend_rt_state_free(aprend_instance instance);

/* ---- Lit materials (D4) ------------------------------------------------- */

/* A slot in [instance]'s lit-material table for a new lit material, its
 * entry from [block]. 0, with nothing taken, on a device without ray
 * queries. APRESULT_OUT_OF_MEMORY with nothing taken. */
APRESULT aprend_rt_lit_take(aprend_instance instance, const struct aprend_lit_material_block *block, uint32_t *out_slot, APREND_ERROR *error);
/* The material at [slot] changed its values to [block]: staged for the next
 * submission that reads the table. */
void aprend_rt_lit_stage(aprend_instance instance, uint32_t slot, const struct aprend_lit_material_block *block);
/* Gives [slot] back, its generation advanced: from the material's release,
 * once the GPU has finished with what was submitted while it lived. */
void aprend_rt_lit_give(aprend_instance instance, uint32_t slot);
/* Whether [slot] names a lit material alive now. */
bool aprend_rt_lit_slot_live(aprend_instance instance, uint32_t slot);

/* ---- BLAS ---------------------------------------------------------------- */

/* One geometry of a BLAS, as the build reads it and the arena holds it. */
struct aprend_rt_geometry {
	aprend_buffer vertices;
	aprend_buffer indices;
	/* The first vertex the build reads, and how many; its first index and
	 * the triangle count. */
	uint64_t vertex_offset;
	uint32_t vertex_count;
	uint64_t index_offset;
	uint32_t index_count;
	uint32_t triangle_count;
	/* Where its vertices and indices go in the arena, in bytes. */
	uint64_t arena_vertices;
	uint64_t arena_indices;
};
APRI_PLAIN_DATA(aprend_rt_geometry);

typedef struct aprend_blas_t {
	char *debug_name{nullptr};
	APRI_STALE_TAG
	aprend_blas_t() = default;
	~aprend_blas_t();
	aprend_instance instance{nullptr};
	/* Its slot (D1): constant for its life. */
	uint32_t slot{0};
	bool deforms{false};
	/* The geometries, copied at creation. */
	aprend_rt_geometry *geometries{nullptr};
	uint32_t geometry_count{0};
	uint64_t primitive_count{0};
	/* Its range of the arena, in units, for its life. */
	uint32_t arena_offset{0};
	uint32_t arena_units{0};
	/* The full-size structure, NULL while compacted; and the compacted one,
	 * NULL until a compaction. While a compaction's frame runs both are
	 * held; the one given up is released once that frame has completed. */
	spudgpu_acceleration_structure full{nullptr};
	spudgpu_acceleration_structure compact{nullptr};
	uint64_t full_size{0};
	uint64_t build_scratch{0};
	uint64_t update_scratch{0};
	/* The submission serial and frame number of the last build (0: none
	 * pending); and the structure a compaction or a rebuild of a compacted
	 * BLAS gave up, released once its submission has completed and the
	 * frame has moved on (T4: both exist while that frame runs). */
	uint64_t built_serial{0};
	uint64_t built_frame{0};
	spudgpu_acceleration_structure retiring{nullptr};
	uint64_t retiring_serial{0};
	uint64_t retiring_frame{0};
	/* Where the compacted size is written after a static BLAS's build (a
	 * storage buffer the device writes it into), copied into READBACK memory
	 * mapped for life, and read once that build has completed. */
	aprend_buffer size_device{nullptr};
	spudgpu_buffer size_readback{nullptr};
	void *size_mapped{nullptr};
	uint64_t compacted_size{0};
	bool built{false};
	/* Debug builds' check of the rule in aprendcontext.h ("Threads"), one
	 * caller to the BLAS (APREND_ENTER_BLAS). In every build, so the struct
	 * is the same in each. */
	apri_overlap entered{};
} aprend_blas_t;

/* Takes the BLASes destroyed since into the table: each index free, its
 * generation advanced (D1). From the instance's one caller. */
void aprend_rt_drain(aprend_instance instance);
/* The same, with the instance's release_mutex held by the caller: what a
 * send's seal and a write call, which read the table under it. */
void aprend_rt_drain_locked(aprend_rt_state *rt);

/* A static BLAS's flags (T3): fast tracing and compaction; a deforming
 * one's: fast building and refits. */
SPUDGPU_ACCELERATION_STRUCTURE_BUILD_FLAGS aprend_rt_blas_flags(bool deforms);
/* [blas]'s inputs as its builds read them: [geometries] has room for its
 * geometry_count. */
void aprend_rt_blas_describe(const aprend_blas_t *blas, spudgpu_acceleration_structure_geometry *geometries, spudgpu_acceleration_structure_inputs *inputs);
/* Takes in what has completed since [blas] was last read: a build (built,
 * and a static one's compacted size read back) and a compaction (the
 * full-size structure released). */
void aprend_rt_blas_settle(aprend_blas blas);

/* ---- TLAS ---------------------------------------------------------------- */

/* Per BLAS index, the records below a TLAS's instance count that name it:
 * those of one generation, and those of any other, which a seal refuses
 * (the BLAS they named is destroyed). */
struct aprend_rt_slot_count {
	uint32_t current;
	uint32_t other;
	uint8_t generation;
};
APRI_PLAIN_DATA(aprend_rt_slot_count);

/* Where an instance set is in its life (T5). */
enum : uint32_t {
	APREND_RT_SET_FREE      = 0,
	APREND_RT_SET_PREPARING = 1,
	APREND_RT_SET_SEALED    = 2, /* sent, its list not yet submitted */
	APREND_RT_SET_SUBMITTED = 3, /* its build's submission not yet complete */
};

/* One instance set (D3, T5): its records and its material table, in UPLOAD
 * memory mapped for life; what its seal took; where it is in its life. Read
 * by the CPU only through its own fields, never through the mapping. */
struct aprend_rt_set {
	spudgpu_buffer records;
	APREND_RAY_INSTANCE *records_mapped;
	spudgpu_buffer materials;
	uint32_t *materials_mapped;
	uint32_t state;
	/* At the seal: the instance count built, and the order the seal was made
	 * in, which builds run in (T5). */
	uint32_t instance_count;
	uint64_t sequence;
	/* The list whose BUILD_TLAS sealed it, for a message: never read
	 * through. */
	const void *sealed_by;
	/* The submission that built it, while SUBMITTED. */
	uint64_t serial;
	/* The BLAS indices its records name at the seal, for the build's
	 * bottom-level list (residency, D6): first in bottom_levels. */
	uint32_t bottom_level_first;
	uint32_t bottom_level_count;
};
APRI_PLAIN_DATA(aprend_rt_set);

typedef struct aprend_tlas_t {
	char *debug_name{nullptr};
	APRI_STALE_TAG
	aprend_tlas_t() = default;
	~aprend_tlas_t();
	aprend_instance instance{nullptr};
	/* aprend_instance_next_serial()'s at creation: what a record names it by
	 * besides its address. */
	uint64_t serial{0};
	uint32_t max_instances{0};
	uint32_t max_materials{0};
	uint32_t set_count{0};

	spudgpu_acceleration_structure structure{nullptr};
	uint64_t structure_size{0};
	uint64_t build_scratch{0};
	/* What builds and traces read on the GPU: the current records and
	 * material table (written blocks copied in at each build), the device's
	 * instances, and the emitter table (per instance, the emissive
	 * triangles before it, prefix-summed). */
	aprend_buffer records{nullptr};
	aprend_buffer materials{nullptr};
	aprend_buffer device_instances{nullptr};
	aprend_buffer emitters{nullptr};

	/* The sets, set_count of them; their written blocks, record_words and
	 * material_words 64-bit words a set; and every set's bottom levels,
	 * max_instances indices a set. */
	aprend_rt_set *sets{nullptr};
	uint64_t *record_blocks{nullptr};
	uint64_t *material_blocks{nullptr};
	uint32_t record_words{0};
	uint32_t material_words{0};
	uint32_t *bottom_levels{nullptr};
	/* The set being prepared, or UINT32_MAX for none yet. */
	uint32_t preparing{UINT32_MAX};
	uint64_t next_sequence{1};

	/* The records and material table as the writes left them: what a newly
	 * prepared set's touched blocks are filled from, and what the checks
	 * read (T6, T7). max_instances and max_materials entries. */
	APREND_RAY_INSTANCE *latest{nullptr};
	uint32_t *latest_materials{nullptr};
	uint32_t instance_count{0};
	/* Per BLAS index, the records below instance_count that name it (T6,
	 * residency); grown with the BLAS table. */
	eq::array<aprend_rt_slot_count> slot_counts{};
	/* Per record, whether its range of the material table reaches a NULL
	 * entry; and per mask bit, how many records below instance_count with
	 * that bit do (T7). */
	uint8_t *reaches_null{nullptr};
	uint32_t null_by_bit[8]{};
	/* The Debug build's per record: its BLAS index's 32-bit generation when
	 * it was written, which a seal compares with the index's now, to name a
	 * slot whose 8-bit generation has wrapped (T6). NULL in Release. */
	uint32_t *debug_generations{nullptr};

	/* A build has been sent: a trace has something to read. */
	bool sent_build{false};
	/* D6: what the emitter table was last built against, and what it reads
	 * now: bumped by a seal with writes or a material table change. */
	uint64_t content_version{1};
	uint64_t emitters_content{0};
	uint64_t emitters_lit{0};
	/* What a compiled, unsubmitted path trace built it against, and its
	 * list: current once that list is submitted. */
	uint64_t emitters_pending_content{0};
	uint64_t emitters_pending_lit{0};
	const void *emitters_pending_list{nullptr};
	/* The instance count of the last set sealed: what the GPU's current
	 * records hold, once its build has run, for the traces after it. */
	uint32_t built_count{0};
	/* The traces' set 1 over this TLAS, and the buffers_version it was made
	 * against. */
	aprend_binding_set trace_set{nullptr};
	uint64_t trace_set_version{0};
	/* The writer's and the emitter pass's sets over this TLAS, likewise. */
	aprend_binding_set writer_set{nullptr};
	uint64_t writer_set_version{0};
	aprend_binding_set emitter_set{nullptr};
	uint64_t emitter_set_version{0};
	/* The bottom levels of one build, gathered at its compile: room for
	 * max_instances, made at creation. */
	spudgpu_acceleration_structure *build_levels{nullptr};
	/* As aprend_blas_t's. */
	apri_overlap entered{};
} aprend_tlas_t;

#define APREND_ENTER_BLAS(blas, who) APRI_ENTER((blas)->entered, "aprend", who, "blas")
#define APREND_ENTER_TLAS(tlas, who) APRI_ENTER((tlas)->entered, "aprend", who, "tlas")

/* What a BUILD_TLAS record keeps in its padding, after `_tlas`: the set its
 * send sealed and the seal's order (aprendcommands.h, the send change of
 * aprend_raytracing_draft.h). Read and written with memcpy at
 * APREND_RT_SEAL_OFFSET of the command's `_params`. */
struct aprend_rt_seal {
	uint32_t set;
	uint32_t padding;
	uint64_t sequence;
	uint64_t tlas_serial;
};
#define APREND_RT_SEAL_OFFSET 8u

static inline aprend_rt_seal aprend_rt_seal_of(const APREND_COMMAND &command) {
	aprend_rt_seal seal;
	memcpy(&seal, command._params._padding + APREND_RT_SEAL_OFFSET, sizeof(seal));
	return seal;
}

/* ---- What the commands' send and compile call ------------------------- */

/* aprend_command_list_send's checks of the four commands, in the order
 * aprendcommands.h lists them: false, reported, for a command refused. */
bool aprend_rt_send_checks(aprend_command_list list, const APREND_COMMAND &command);
/* What a checked command changes as it is recorded, once room for it has
 * been made: a BUILD_TLAS seals its set (aprend_rt_seal_set), a BUILD_BLAS
 * of APREND_BUILD_MODE_BUILD marks its BLAS sent for building (T6). False,
 * reported, with nothing changed. */
bool aprend_rt_send_commit(aprend_command_list list, APREND_COMMAND *command);

/* Sends a BUILD_TLAS: checks the set being prepared (T6) and seals it into
 * [command]'s padding, [list] its sealer. False, reported, with nothing
 * sealed. */
bool aprend_rt_seal_set(aprend_command_list list, APREND_COMMAND *command);
/* A list's sealed, unsubmitted BUILD_TLAS records are abandoned (the list
 * reset or destroyed): each set's written blocks are carried into the set
 * after it, and the set is free. */
void aprend_rt_abandon(aprend_command_list list);
/* T5 at submit: false, reported naming both lists, if [list] holds a
 * BUILD_TLAS whose TLAS has an earlier seal another list holds unsubmitted. */
bool aprend_rt_submit_order_ok(aprend_command_list list);
/* After [list]'s submission took [serial]: its seals are submitted, its
 * BLASes' builds are this submission's, and the lit-material entries it
 * copied are clean. */
void aprend_rt_submitted(aprend_command_list list, uint64_t serial);
/* For the submission of a list that reads the lit-material table: each
 * entry staged since the last copy, through the current frame's transient
 * memory, added to [copies]. False, reported, if the memory can't be
 * taken. Throws std::bad_alloc from [copies]. */
bool aprend_rt_stage_lit(aprend_instance instance, eq::array<aprend_upload_copy> &copies);

/* [scene]'s block alone, as the traces' set 0 of [layout] reads it, kept by
 * the scene (aprend_lit_scene_t::rt_set). NULL, reported, if it can't be
 * made. In aprendlit.cpp. */
aprend_binding_set aprend_lit_scene_rt_set(aprend_lit_scene scene, aprend_binding_layout layout);

/* The traces' compile (aprendcommands.cpp) reads these. */
/* The shadow mask's pipeline for a target of [format] (R8_UNORM or
 * R16_FLOAT), made if it isn't, with the traces' layouts. NULL, reported,
 * if it can't be. */
aprend_compute_pipeline aprend_rt_mask_pipeline(aprend_instance instance, SPUDGPU_FORMAT format);
/* [tlas]'s set of the traces' set 1: the TLAS and its tables, made again if
 * the instance's tables were replaced since. NULL, reported. */
aprend_binding_set aprend_rt_tlas_trace_set(aprend_tlas tlas);
/* The output set of a trace into [target], of [layout], from [entries]:
 * kept by the target and made again only when [others] (two views, or
 * NULL) differ from what it was made with. NULL, reported. */
aprend_binding_set aprend_rt_output_set(aprend_texture_view target, aprend_binding_layout layout, const aprend_texture_view *others,
                                        const APREND_BINDING_SET_ENTRY *entries, uint32_t count);

/* The traces' layouts of sets 0 to 2 (the scene's block, the TLAS, the path
 * tracer's outputs), from the path tracer, made if they aren't. False,
 * reported. */
bool aprend_rt_trace_layouts(aprend_instance instance);
/* The path tracer's pipeline for guides of [albedo] and [normal] (each
 * R32G32B32A32_FLOAT or R16G16B16A16_FLOAT), made if it isn't. NULL,
 * reported. */
aprend_compute_pipeline aprend_rt_path_pipeline(aprend_instance instance, SPUDGPU_FORMAT albedo, SPUDGPU_FORMAT normal);
/* The emitter pass's pipeline, and [tlas]'s set of it, made if they aren't.
 * NULL, reported. */
aprend_compute_pipeline aprend_rt_emitter_pipeline(aprend_instance instance);
aprend_binding_set aprend_rt_tlas_emitter_set(aprend_tlas tlas);
/* What an absent guide is written into: the instance's 1x1 RGBA32F storage
 * view, made if it isn't. NULL, reported. */
aprend_texture_view aprend_rt_no_guide(aprend_instance instance);
/* T7: the first record below [tlas]'s instance count whose range reaches a
 * NULL material table entry and whose mask [cull_mask] lets in, or
 * UINT32_MAX for none. */
uint32_t aprend_rt_null_instance(aprend_tlas tlas, uint32_t cull_mask);

/* The builds' compile (aprendcommands.cpp) reads these. */
/* The instance writer of the device's instance layout (card S14), made if
 * it isn't. NULL, reported, if it can't be. */
aprend_compute_pipeline aprend_rt_writer_pipeline(aprend_instance instance);
/* [tlas]'s set of the writer's layout, made again if the tables were
 * replaced since. NULL, reported, if it can't be. */
aprend_binding_set aprend_rt_tlas_writer_set(aprend_tlas tlas);
/* The builds' scratch, grown to [size] if it is smaller. False, reported. */
bool aprend_rt_prepare_scratch(aprend_instance instance, uint64_t size);
/* A COMPACT's compacted structure, and a BUILD's full-size one again for a
 * compacted BLAS (T4): made, or false, reported. */
bool aprend_rt_blas_make_compact(aprend_blas blas);
/* [blas]'s structure given up by a compaction or a rebuild still waiting
 * for its frame, handed to the release queue before another takes its
 * place. False, reported, with no memory to. */
bool aprend_rt_blas_retire_pending(aprend_blas blas);
bool aprend_rt_blas_make_full(aprend_blas blas);

#endif // APREND_RAYTRACING_INTERNAL_HPP
