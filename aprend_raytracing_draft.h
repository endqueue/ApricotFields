/*
 * aprend_raytracing_draft.h - Aprend's ray tracing over SPUDGPU_EXT_RAY_QUERY
 * (APREND_TODO.md item 24: card S12's Aprend half and card T16). For review:
 * prompts/ray_tracing.md, Phase 7.
 *
 * APPROVED 2026-10-31 by the user, the eleventh version as written here
 * (the workspace's DECISIONS.md, "Decided"). Built in Phase 7; where
 * building finds it wrong or incomplete, a card says so and this file is
 * not edited to match.
 *
 * DRAFT, 2026-10-31, eleventh version.
 *  - First: an rt scene and a path tracer object, turned down as a strap-on.
 *  - Second and third: the draw list's, turned down as duct tape (identity,
 *    lifetime, typed data and a mesh's nature inferred).
 *  - Fourth and fifth: BLAS and TLAS as resources, typed instances,
 *    commands; the fifth fixed the fourth's eight faults.
 *  - Sixth, with the user: named aprend_blas and aprend_tlas; and redone
 *    for the CPU and GPU work per frame (D1-D7 below), since the fifth
 *    walked handles and copied every instance every frame.
 *  - Seventh, with the user: the sixth's sets held the device's layout, so
 *    a BLAS reference carried over on the GPU went stale when that BLAS was
 *    compacted or rebuilt; the sets now hold Aprend's records, slots and
 *    all, and a GPU pass per build writes the device's layout (D3). Lit
 *    materials get a dense table of their own, as BLASes have, since the
 *    material arena's blocks sit in several buffers a megakernel can't
 *    index without bindless (D4). The 3x4 transform stays, with how to get
 *    it from a 4x4 (D2).
 *  - Eighth, with the user, from the seventh's critique: no CPU write into
 *    a table a frame in flight reads (the material table travels with each
 *    set, lit-material entries are staged into the submission); builds of
 *    one TLAS run in the order sent, checked at submit; leftover wording
 *    that contradicted D2-D4 corrected; what Aprend sets for culling and
 *    winding stated; written ranges tracked by a fixed bitset of blocks;
 *    the emitter table rebuilt only when something it reads changed; the
 *    accumulation's reset rule and a step's GPU time stated; a slot's
 *    generation wrap and the record's spare bytes stated.
 *  - Ninth, with the user: no setter silently takes another's place (a
 *    mask set while cascades are, or the reverse, is refused); the mask
 *    command reads the sun's size from the lit scene's sun, not a size of
 *    its own. The sun itself (Aprend's one directional light, apart from
 *    its point and spot lights) is card A35's question, open.
 *  - Tenth, after card A35 (decided with the user: directional lights a
 *    light type, shadows per light, the sky's discs named by its command,
 *    no light "the sun"): the shadow mask names the directional light it
 *    shadows, the path tracer samples every directional light, and the
 *    lit scene's mask setter is per light. Until A35 is built the lit
 *    scene has one directional light, today's sun, as directional light 0;
 *    nothing here changes when A35 lands.
 *  - Eleventh, with the user: a light has one shadow, set by one per-light
 *    call whose desc names its kind (aprend_lit_scene_set_light_shadow(),
 *    APREND_LIT_LIGHT_SHADOW), not a mask setter beside the cascades' with
 *    a rule refusing one while the other is set. This version gives it the
 *    mask kind; card A35 moves the cascaded kind into it, so nothing added
 *    here is thrown away.
 * Every declaration is marked [ADDED] or [CHANGED]; nothing is removed.
 * Ray-traced reflections wait on card A30; alpha testing and the path
 * tracer's material maps wait on card A15 (Metal's bindless).
 *
 * ---------------------------------------------------------------------------
 * The shape, in one paragraph
 * ---------------------------------------------------------------------------
 *
 * A BLAS (bottom-level acceleration structure) is one mesh's geometry in its
 * own space: one or more ranges, each a material slot, static or
 * deforming, built once and shared by every instance of the mesh. A TLAS
 * (top-level acceleration structure) is one placement of meshes: the
 * instances rays are traced against, rebuilt when the placement changes.
 * The caller creates an aprend_blas, sends APREND_COMMAND_BUILD_BLAS to
 * build, refit or compact it, and reads its slot once. It creates an
 * aprend_tlas, writes ranges of 64-byte instance records into the TLAS's
 * next instance set (each naming a BLAS by slot), and sends
 * APREND_COMMAND_BUILD_TLAS, which builds from the set as it stood when
 * sent; ranges it didn't write carry over from the last set on the GPU.
 * Two commands trace a TLAS: APREND_COMMAND_LIT_SHADOW_MASK (a directional
 * light's shadow as a mask the lit scene reads for that light instead of
 * cascades, the caller's explicit choice) and
 * APREND_COMMAND_LIT_PATH_TRACE (N samples per pixel into the caller's
 * accumulation, with albedo and normal guides). Every call refuses a device
 * without SPUDGPU_FEATURE_RAY_QUERY with APRESULT_UNSUPPORTED and a message
 * naming it, before SpudGPU would.
 *
 * ---------------------------------------------------------------------------
 * Data and work per frame (D1-D7)
 * ---------------------------------------------------------------------------
 *
 *  D1. A BLAS is named by slot in an instance, never by handle. The handle
 *      stays for what has a lifetime (create, build, info, destroy); the
 *      slot (aprend_blas_get_slot(), read once per BLAS, not per frame) is
 *      a uint32: a 24-bit index into the Aprend instance's dense BLAS table
 *      and an 8-bit generation, so a slot whose BLAS was destroyed and
 *      whose index was reused is refused, not silently another mesh (as
 *      generational entity ids are). Checking an instance reads two dense
 *      arrays (the table's generations and built flags), no pointer.
 *  D2. One record per instance, one cache line: APREND_RAY_INSTANCE is 64
 *      bytes, a row-major 3x4 transform (48 bytes, the form Vulkan's,
 *      D3D12's and SpudGPU's portable instances take), the slot, the
 *      material base, the mask, 4 reserved. Only the transform is in a
 *      device's order; the rest is Aprend's, translated by D3's GPU pass.
 *      The mask takes a uint32 and 4 bytes are reserved so the record is a
 *      whole cache line; they leave room for per-instance flags if a need
 *      comes (none now, D8). APRI_PLAIN_DATA, APRI_HOT_RECORD(64). The 3x4 row-major transform is
 *      the industry's instance form (DXR's D3D12_RAYTRACING_INSTANCE_DESC
 *      Transform[3][4], Vulkan's VkTransformMatrixKHR), not one backend's:
 *      Metal's column-major layout is absorbed below Aprend's API (D3's
 *      writer, per SpudGPU's layout kind). Not APREND_LIT_INSTANCE's
 *      column-major 4x4: 16 bytes longer, so a record would pass a cache
 *      line and every upload would carry a quarter more. From a
 *      column-major 4x4 m (m[column][row], glm's), row r of the 3x4 is
 *      (m[0][r], m[1][r], m[2][r], m[3][r]) for r = 0, 1, 2: its last row
 *      dropped, once, where the caller computes the transform.
 *  D3. The sets hold Aprend's records, and only what changed is written. A
 *      TLAS's sets are GPU-visible buffers of APREND_RAY_INSTANCE, slots
 *      and all: aprend_tlas_write_instances() copies a range of records into
 *      the set being prepared (a memcpy; the slots checked, T6), with no
 *      translation on the CPU. Which records a set wrote is a bitset over
 *      blocks of 64 instances, sized at creation (_max_instances / 64 bits
 *      a set), so tracking writes allocates nothing and a build's copy has
 *      at most one region per run of unwritten blocks. Sending BUILD_TLAS
 *      seals the set; at the build, the blocks it didn't write are copied
 *      from the last sealed set on the GPU (a buffer copy, about 6 MB for
 *      100,000 instances), which is why builds of one TLAS run in the order
 *      they were sent (T5),
 *      then a compute pass of Aprend's (SpudGPU's caller's GPU writer, card
 *      S14: one Slang shader per SpudGPU instance layout kind, so Metal's
 *      72-byte column-major form is the shader's, not the caller's) writes
 *      every instance in the device's layout, each slot resolved to its
 *      BLAS's current reference from the BLAS table on the GPU (D5). So a
 *      BLAS whose reference changes (compacted, or rebuilt full size, T4)
 *      changes one table entry, and every set, carried over or not, picks
 *      it up at its next build. A frame moving 1% of a static scene copies
 *      1% of it on the CPU; the GPU translates all of it each build (about
 *      0.24 ms for 100,000 instances, Phase 5's measured writer).
 *  D4. The material base is the instance's 24-bit custom index, so a hit
 *      reads it with the instance id the query already returns
 *      (CommittedInstanceID); no per-instance table on the side. Lit
 *      materials get a dense table of their own, as BLASes do: one storage
 *      buffer per Aprend instance holding every lit material's values, each
 *      material at a stable slot (a 24-bit index and an 8-bit generation)
 *      for its life. aprend_lit_material_set_values(), which already copies
 *      the material's block whenever it changes, also writes its table
 *      entry, as the arena writes a changed block: staged, and copied into
 *      the table in the next submission, ordered before what reads it, so
 *      no frame in flight sees a write it didn't submit after. The table
 *      grows by doubling, an allocation only when it fills, never in a
 *      frame that changes nothing. The material arena (item 19)
 *      never moves a block, but its blocks sit in several buffers at 256-byte
 *      alignment, which one megakernel can't index without bindless (Metal
 *      has none, card A15); the table is one buffer. A TLAS's material table
 *      (aprend_tlas_set_materials(), called when materials change) holds
 *      slots into it, and travels with the instance sets: each set holds its
 *      own copy of the table, written as instances are (a range into the set
 *      being prepared, carried over by block when not written), so a set a
 *      frame in flight reads is never written. A path trace binds the one
 *      lit-material buffer and its set's table; the CPU gathers nothing per
 *      frame.
 *  D5. The BLAS table is dense and on the GPU too: at a BLAS's slot, its
 *      current device reference (written by each build and compaction,
 *      read by D3's writer) and its geometries' buffer addresses and ranges
 *      (written once at creation: positions, normals and texture
 *      coordinates a path trace reads at a hit); nothing per frame. The
 *      CPU keeps the slots' generations and built flags beside it, for the
 *      checks (T6).
 *  D6. What every build needs is found without walking instances: Metal's
 *      residency (the BLASes a set names) is a bitset over slots each write
 *      sets, read at the seal. The emitters' table (T10) is built on the GPU
 *      by a compute pass over the set and the lit-material table, by the
 *      first path trace after anything it reads changed: a set with writes,
 *      a material table change, or a lit material's values changed (a
 *      changed flag each sets). A TLAS rebuilt every frame over an unchanged
 *      placement reuses its table, so a progressive render pays for it
 *      once.
 *  D7. The steady state allocates nothing and touches no handle: a frame
 *      that writes a range, seals, builds and traces makes no allocation of
 *      Aprend's and dereferences no aprend_blas (test_frame_allocations
 *      counts the first; the second is by construction, D1).
 *
 * ---------------------------------------------------------------------------
 * What this decides that no card has (each becomes a card if overturned)
 * ---------------------------------------------------------------------------
 *
 *  T1. Resources, not a manager: a BLAS and a TLAS are GPU objects with a
 *      lifetime, at the tier of aprend_buffer and aprend_texture (practice:
 *      NVRHI's and SpudGPU's acceleration structures; Unreal's ray tracing
 *      geometry and scene). Named by the industry's terms, each spelled out
 *      once in its comment.
 *  T2. A BLAS is one or more geometries, each an APREND_DRAW_MESH range and
 *      a material slot (practice: DXR's geometry index, Unreal's segments):
 *      positions vertex buffer 0's first three floats at its stride, 32-bit
 *      indices or none; for a path trace their layout is APREND_LIT_VERTEX's
 *      and the vertex buffers have storage usage (refused otherwise, naming
 *      the BLAS). Opaque to rays in this version (alpha testing waits on
 *      A15; its API is here: each slot's material carries `alpha_cutoff`).
 *  T3. Deforming is declared at creation (`_deforms`): built ALLOW_UPDATE
 *      and PREFER_FAST_BUILD, refitted (REFIT) after each rewrite, rebuilt
 *      (BUILD) when the caller wants the quality back, never compacted. A
 *      static one is built PREFER_FAST_TRACE and ALLOW_COMPACTION (practice).
 *      What is built in a frame is the caller's: it sends the commands.
 *  T4. Compaction is explicit: once aprend_blas_get_info() reports the
 *      compacted size, the caller sends BUILD_BLAS with COMPACT; both
 *      structures exist until that frame completes (the info shows it); a
 *      BUILD of a compacted BLAS makes full-size storage again (an
 *      allocation at that command, stated). The slot is unchanged; the
 *      device reference changes, written into the BLAS table by the
 *      command, so every TLAS built after it sees the new one (D3, D5).
 *  T5. Instance sets: a TLAS holds `_max_instance_sets` sets of
 *      `_max_instances`; writes go to the set being prepared; sending
 *      BUILD_TLAS seals it and builds from it, so a later write changes no
 *      build already sent. A sealed set is free again once the frame that
 *      built from it has completed; a write with no set free is refused
 *      (APRESULT_LIMIT, naming the count). `_instance_count`, the set's
 *      length, is the TLAS's until changed (aprend_tlas_set_instance_count()).
 *      Builds of one TLAS run in the order they were sent: a set's carried-
 *      over blocks come from the set sealed before it, which is complete only
 *      once its own build has run. A command list whose BUILD_TLAS was sent
 *      after another list's build of the same TLAS is refused at submit if
 *      that list hasn't been submitted (APRESULT_INVALID_ARGUMENT, naming
 *      both), as the narrowest rule all three APIs keep.
 *  T6. Lifetimes, checked where used: a BLAS outlives every set that names
 *      it until the builds from that set have completed, and has been built
 *      (or sent for building earlier in the same list) before a TLAS built
 *      from it runs; writing a record checks its slot's generation (stale:
 *      refused, naming the record), sealing checks every named slot is
 *      built or sent (the bitset against the built flags). A slot's
 *      generation is 8 bits: an index reused 256 times reads as its first
 *      BLAS again, so a slot kept that long past its BLAS's destruction can
 *      pass the check (as generational ids do); Debug keeps a 32-bit
 *      generation per index beside it and names such a slot.
 *  T7. A NULL material slot is one only shadows see: a path trace's
 *      `_cull_mask` must leave out every instance whose material base
 *      reaches a NULL slot, checked when the list compiles (refused, naming
 *      the instance).
 *  D8. Culling and winding are Aprend's, the same for every instance:
 *      triangle culling disabled (a mirrored instance's winding flips, and
 *      thin geometry must still stop a shadow ray), and the side a ray hit
 *      found from the geometric normal against the ray, not from winding.
 *      So a record carries no flags; its spare bytes (D2) are where flags
 *      would go if a need comes.
 *  T8. The TLAS is rebuilt, PREFER_FAST_BUILD, each time its command runs
 *      (practice: build rather than refit a TLAS); consecutive build
 *      commands in a list become one SpudGPU build call, with one barrier
 *      between the BLASes and the TLAS (practice). Scratch is the
 *      instance's: one buffer grown to the largest a compiled list needs.
 *  T9. A directional light's shadow may be a mask, as ambient occlusion is
 *      and as Unreal's ray-traced shadows are (a per-light choice there
 *      too): APREND_COMMAND_LIT_SHADOW_MASK traces the shadow of the
 *      directional light `_light` names (card A35; until A35 is built, 0,
 *      today's sun, is the only one) over the depth pre-pass into a mask,
 *      which the lit fragment shader reads for that light instead of its
 *      cascades. A light has one shadow, set with one per-light call
 *      (aprend_lit_scene_set_light_shadow()), whose desc names its kind:
 *      APREND_LIT_SHADOW_KIND_MASK here, _CASCADES once card A35 moves the
 *      cascaded shadow into it (practice: Unreal picks a shadow method per
 *      light; Hazel keeps shadow settings on each light). Setting a light's
 *      shadow replaces what it had, as any setter does; there is nothing to
 *      refuse. Until A35, directional light 0's shadow is also set by
 *      aprend_lit_scene_set_sun_shadow() (cascades): both calls set the one
 *      shadow light 0 has, whichever comes last is it, and each says so.
 *      The command reads
 *      the light's direction and angular radius from the lit scene it is
 *      given (today APREND_LIT_SUN::direction and ::disc_angle), so the
 *      sky's disc of that light and its shadow's penumbra are the same
 *      light (practice: Unreal's Source Angle is the light's own).
 *      `_ray_count` rays per texel (practice: 1), blue-noise directions, a
 *      depth- and normal-aware spatial filter. Approximate, and stated:
 *      normals from the depth's own slopes until cards A6 and A30's
 *      normal-and-roughness target exists; no temporal filter until Aprend
 *      has motion vectors. Point and spot lights' traced shadows are not
 *      here: their shadow maps stay, and a mask per point or spot light is
 *      the same command's later kind when a caller asks.
 *  T10. The path tracer, practice as Falcor's and Cycles': next-event
 *      estimation with MIS (power heuristic) between the BSDF, the
 *      environment (importance-sampled), every directional light of the
 *      lit scene (each over its own disc, from its angular radius; today
 *      the one, APREND_LIT_SUN's disc_angle) and emissive triangles; point and spot lights without MIS;
 *      Owen-scrambled Sobol and subpixel jitter; Russian roulette from half
 *      the bounces; an indirect `_clamp`; offsets by Ray Tracing Gems'
 *      chapter 6 method; materials' values from the lit-material table
 *      (D4), their maps waiting on A15. One compute megakernel per step (card T16), the
 *      material a Slang interface, stages as functions.
 *  T11. The accumulation is the caller's: an RGBA32F target holding the
 *      running mean, `_accumulated` the samples held before the step (0
 *      starts over); optional first-hit albedo and normal guides (practice:
 *      Unreal's and Falcor's, for Open Image Denoise). The same seed and
 *      counts give the same bits on one device. Accumulating is right only
 *      over an unchanged scene: `_accumulated` goes back to 0 after any
 *      change to the view, the instances, the materials, the environment or
 *      the lights, which is the caller's to track. A step is one dispatch:
 *      the caller sizes `_samples` so a step stays well under the platform's
 *      GPU time limit (Windows ends a dispatch at about two seconds; WARP
 *      took 0.4 s a frame for a simple scene), from the samples per second
 *      it measures.
 *  T12. Sizes are reported: aprend_blas_get_info() and aprend_tlas_get_info()
 *      give GPU bytes (a TLAS's sets, structure and emitter table), a BLAS's
 *      compacted size once known, and how many sets are in use.
 *
 * ---------------------------------------------------------------------------
 * Optional features (item 39): SPUDGPU_FEATURE_RAY_QUERY joins group 2.
 * ABI (abi.txt): additions only.
 * ---------------------------------------------------------------------------
 *
 * Tests (apricottests, every adapter; refused on the Iris Xe):
 *  - The mask against cascades where both are exact (a box on a plane,
 *    directional light 0 with an angular radius of 0), read back; a mask
 *    bit leaving the box out; set_light_shadow and set_sun_shadow each
 *    replacing light 0's one shadow, the lit picture following the last; a
 *    larger angular radius widening the penumbra; a `_light` naming no
 *    directional light refused, and the CASCADES kind refused until A35
 *    (naming set_sun_shadow); once A35 is built, the test runs over two
 *    directional lights.
 *  - A furnace with an environment of 1, and an emissive quad alone against
 *    its analytic irradiance.
 *  - Determinism; unbiased accumulation (N steps of 1 against 1 of N, and
 *    against a many-sample reference).
 *  - A two-geometry BLAS, each slot its own material, read back.
 *  - A deforming quad refitted each frame, its shadow read back at its new
 *    place; a static BLAS compacted (sizes before and after, the same
 *    shadow), then rebuilt full size.
 *  - Ordering (T5): a second list's BUILD_TLAS submitted before the first's
 *    refused, naming both; in order, both drawn right.
 *  - The emitter table (D6): a rebuilt TLAS over an unchanged set reusing it
 *    (counted), a written set or a material change rebuilding it.
 *  - Staged material writes (D4): values changed while a frame using the
 *    old ones is in flight, that frame reading the old, the next the new.
 *  - Partial writes (D3): a range of instances moved, the rest carried over,
 *    every hit where it should be; a build sent, then a write before
 *    compile, the build drawing the first; a BLAS compacted, then a TLAS
 *    built from a set carried over from before the compaction, its hits
 *    right (the reference resolved at the build, not stored in the set).
 *  - The lit-material table (D4): a material's values changed, the path
 *    trace reading the new ones; a destroyed material's slot reused, the
 *    old slot refused by set_materials.
 *  - Slots: a destroyed BLAS's slot reused by a new one, the old slot
 *    refused at a write, naming the record.
 *  - A path trace whose cull mask lets in a NULL material refused; with the
 *    mask leaving it out, drawn.
 *  - Every refusal, in order; a forbidden overlap of each new call
 *    (APREND_ENTER); test_frame_allocations with a write, a seal, both
 *    builds, the mask and a step: no allocation.
 *  - Measured, Release, the RTX A2000: a frame writing 1%, 10% and 100% of
 *    100,000 instances (CPU and GPU); the mask against the cascaded pass;
 *    the path tracer's samples per second at 1920x1080.
 *  - The misuse harness regenerated, with factories for both handles.
 *
 * Compared: Hazel's 2025 renderer has no ray-traced pass. NVIDIA's RTX best
 * practices (T3, T4, T8); Unreal's ray tracing geometry, shadows and path
 * tracer (T1, T2, T9, T11); DXR's geometry index and instance id (T2, D4);
 * generational entity ids in data-oriented engines (D1); Falcor's path
 * tracer (T10, T11); Ray Tracing Gems chapter 6 (T10).
 */

/* ---- Handles ------------------------------------------------------------ */
/** [ADDED] A bottom-level acceleration structure (BLAS): one mesh's geometry. */
typedef struct aprend_blas_t *aprend_blas;
/** [ADDED] A top-level acceleration structure (TLAS): one placement of meshes. */
typedef struct aprend_tlas_t *aprend_tlas;

/* ---- BLAS (T1-T4, D1, D5) ------------------------------------------------ */

/** @brief [ADDED] What a BLAS is made from. */
typedef struct APREND_BLAS_DESC {
	APRI_STRUCT_BASE base;               /**< APRI_STRUCT_TYPE_APREND_BLAS_DESC. */
	/** Its geometries, one per material slot, each a mesh's buffers and
	 * range as a draw names them. Copied; the buffers outlive the BLAS's
	 * use. */
	const APREND_DRAW_MESH *_geometries;
	uint32_t _geometry_count;            /**< 1 to the device's limit. */
	bool _deforms;                       /**< Rewritten while it lives: refitted, never compacted (T3). */
	const char *debug_name;              /**< Copied, or NULL. */
} APREND_BLAS_DESC;

APRESULT aprend_blas_create(aprend_instance instance, const APREND_BLAS_DESC *desc, aprend_blas *out_blas,
                            APREND_ERROR *out_error);
void aprend_blas_destroy(aprend_blas blas);

/** @brief [ADDED] The BLAS's slot (D1): a 24-bit index and an 8-bit
 * generation, constant for its life. 0 is never a slot. */
uint32_t aprend_blas_get_slot(aprend_blas blas);

/** @brief [ADDED] What a BLAS holds (T4, T12). */
typedef struct APREND_BLAS_INFO {
	APRI_STRUCT_BASE_OUT base;           /**< APRI_STRUCT_TYPE_APREND_BLAS_INFO. */
	uint64_t _storage_bytes;             /**< GPU bytes now (both structures while a compaction's frame runs). */
	uint64_t _compacted_bytes;           /**< Once known; 0 before, and for a deforming one. */
	bool _built;                         /**< A build has completed. */
	bool _compacted;                     /**< Its storage is compacted. */
} APREND_BLAS_INFO;

APRESULT aprend_blas_get_info(aprend_blas blas, APREND_BLAS_INFO *out_info, APREND_ERROR *out_error);

/* ---- TLAS (T5-T8, T12, D2-D4, D6) ---------------------------------------- */

/** @brief [ADDED] Creates a TLAS. */
typedef struct APREND_TLAS_DESC {
	APRI_STRUCT_BASE base;               /**< APRI_STRUCT_TYPE_APREND_TLAS_DESC. */
	uint32_t _max_instances;             /**< Not 0, within the device's limit. */
	uint32_t _max_materials;             /**< The material table's length: not 0. */
	uint32_t _max_instance_sets;         /**< At least 2; frames_in_flight + 1 for one build a frame (T5). */
	const char *debug_name;              /**< Copied, or NULL. */
} APREND_TLAS_DESC;

APRESULT aprend_tlas_create(aprend_instance instance, const APREND_TLAS_DESC *desc, aprend_tlas *out_tlas,
                            APREND_ERROR *out_error);
void aprend_tlas_destroy(aprend_tlas tlas);

/** @brief [ADDED] One instance (D2): 64 bytes, one cache line. */
/* Frozen: an array element of aprend_tlas_write_instances(); its transform in the devices' order, the rest Aprend's. */
typedef struct APREND_RAY_INSTANCE {
	float _transform[12];                /**< Object to world, row-major 3x4: rows of (x, y, z, translation). */
	uint32_t _blas;                      /**< aprend_blas_get_slot() of a BLAS of the same instance (D1). */
	uint32_t _material_base;             /**< Below 2^24: its geometry g uses the material table's [_material_base + g] (D4). */
	uint32_t _mask;                      /**< 8 bits: which rays see it (each ray command's cull mask). */
	uint32_t _reserved;                  /**< 0. */
} APREND_RAY_INSTANCE;

/** @brief [ADDED] Copies @p count records into the set being prepared, from
 * instance @p first (D3): copied as they are, the slots checked (T6), the
 * blocks marked written. Blocks this set doesn't write carry over from the
 * last sealed set at its build. APRESULT_LIMIT with every set in use. */
APRESULT aprend_tlas_write_instances(aprend_tlas tlas, uint32_t first, const APREND_RAY_INSTANCE *instances,
                                     uint32_t count, APREND_ERROR *out_error);

/** @brief [ADDED] How many instances the sets hold from the next set on
 * (T5): at most _max_instances. */
APRESULT aprend_tlas_set_instance_count(aprend_tlas tlas, uint32_t count, APREND_ERROR *out_error);

/** @brief [ADDED] The material table (D4): @p count lit materials (or NULL
 * for slots only shadows see, T7) from @p first, written as their slots
 * into the set being prepared (carried over by block when not written, as
 * instances are). Seen by the build that seals that set and every trace of
 * it. APRESULT_LIMIT with every set in use. */
APRESULT aprend_tlas_set_materials(aprend_tlas tlas, uint32_t first, const aprend_material *materials, uint32_t count,
                                   APREND_ERROR *out_error);

/** @brief [ADDED] What a TLAS holds (T12). */
typedef struct APREND_TLAS_INFO {
	APRI_STRUCT_BASE_OUT base;           /**< APRI_STRUCT_TYPE_APREND_TLAS_INFO. */
	uint64_t _storage_bytes;             /**< GPU bytes: the structure, its sets, its emitter table. */
	uint32_t _sets_in_use;               /**< Sealed sets whose builds have not completed. */
} APREND_TLAS_INFO;

APRESULT aprend_tlas_get_info(aprend_tlas tlas, APREND_TLAS_INFO *out_info, APREND_ERROR *out_error);

/* ---- Commands (aprendcommands.h), each outside a pass -------------------- */

/* [ADDED] APREND_BUILD_MODE: _BUILD = 1; _REFIT = 2 (a deforming BLAS, built
 * once); _COMPACT = 3 (a static BLAS whose compacted size is known).
 * [ADDED] APREND_COMMAND_BUILD_BLAS = 40 (T3, T4, T8):
 *   struct { aprend_blas _blas; APREND_BUILD_MODE _mode; } _build_blas;
 * [ADDED] APREND_COMMAND_BUILD_TLAS = 41 (T5, T6, T8, D3): seals the set
 * being prepared when sent, copies its carried-over ranges, builds.
 *   struct { aprend_tlas _tlas; } _build_tlas;
 * [ADDED] APREND_COMMAND_LIT_SHADOW_MASK = 42 (T9):
 *   struct {
 *       aprend_lit_scene _scene;      aprend_tlas _tlas;            uint32_t _light;  (a directional light's index)
 *       aprend_texture_view _depth;   aprend_texture_view _target;  (STORAGE, R8_UNORM or R16_FLOAT)
 *       uint32_t _ray_count;          uint32_t _filter_radius;      (the sun's size: the scene's disc_angle, T9)
 *       uint32_t _cull_mask;          float _normal_offset;         float _max_distance;
 *   } _lit_shadow_mask;
 * [ADDED] APREND_COMMAND_LIT_PATH_TRACE = 43 (T7, T10, T11):
 *   struct {
 *       aprend_lit_scene _scene;      aprend_tlas _tlas;
 *       aprend_texture_view _target;  aprend_texture_view _albedo;  aprend_texture_view _normal;
 *       uint32_t _samples;            uint32_t _accumulated;        uint32_t _seed;
 *       uint32_t _max_bounces;        uint32_t _cull_mask;          float _clamp;
 *   } _lit_path_trace;
 * [CHANGED] aprend_command_list_send(): a BUILD_TLAS record seals its TLAS's
 * set being prepared into the record's padding (T5), and checks T6. */

/* ---- The lit scene (T9) --------------------------------------------------- */

/** @brief [ADDED] What kind of shadow a light has (T9). */
typedef enum APREND_LIT_SHADOW_KIND APRI_ENUM_BASE {
	APREND_LIT_SHADOW_KIND_NONE = 0,     /**< Refused: a desc that has not chosen. */
	APREND_LIT_SHADOW_KIND_MASK = 1,     /**< A traced mask, from APREND_COMMAND_LIT_SHADOW_MASK. */
	APREND_LIT_SHADOW_KIND_CASCADES = 2  /**< Cascaded shadow maps: refused until card A35 moves them here
	                                      *   (use aprend_lit_scene_set_sun_shadow() until then). */
} APREND_LIT_SHADOW_KIND;

/** @brief [ADDED] One light's shadow (T9). A kind's own fields: a mask
 * here; card A35 adds the cascades' as a struct chained on #base. */
typedef struct APREND_LIT_LIGHT_SHADOW {
	APRI_STRUCT_BASE base;               /**< APRI_STRUCT_TYPE_APREND_LIT_LIGHT_SHADOW. */
	APREND_LIT_SHADOW_KIND _kind;
	/** How dark the shadow is: 1 takes all the light away, 0 none of it. */
	float _strength;
	/** _MASK: a SHADER_RESOURCE 2D view of the mask a LIT_SHADOW_MASK wrote
	 * for this light, at the view's target size; outlives its use by the
	 * scene. */
	aprend_texture_view _mask;
} APREND_LIT_LIGHT_SHADOW;

/** @brief [ADDED] Sets directional light @p light's one shadow (T9),
 * replacing what it had, or takes it away (@p shadow NULL). Refused
 * (APRESULT_INVALID_ARGUMENT, naming why) when @p light is no directional
 * light of the scene (today only 0, the sun, until card A35 is built), the
 * kind is NONE, or a field doesn't fit the kind; CASCADES until A35
 * (naming aprend_lit_scene_set_sun_shadow()). */
APRESULT aprend_lit_scene_set_light_shadow(aprend_lit_scene scene, uint32_t light, const APREND_LIT_LIGHT_SHADOW *shadow,
                                           APREND_ERROR *out_error);
/* [CHANGED] aprend_lit_scene_set_sun_shadow(): until card A35, it sets
 * directional light 0's one shadow to cascades (replacing a mask set by
 * aprend_lit_scene_set_light_shadow()), and its comment says so; A35 moves
 * cascades into APREND_LIT_LIGHT_SHADOW and retires it. */
/* [CHANGED] aprend_lit_material_create() takes a slot in the instance's
 * lit-material table (D4), and aprend_lit_material_set_values() stages its
 * entry beside its arena block, copied in the next submission; destroying
 * the material frees the slot, its generation advanced, once the frames
 * using it have completed. No public declaration changes: the table is
 * Aprend's.
 * [CHANGED] aprend_command_list_submit(): refuses a list with a BUILD_TLAS
 * sent after another unsubmitted list's build of the same TLAS (T5). */
/* [ADDED] apricore.h: APRI_STRUCT_TYPE_APREND_BLAS_DESC, _APREND_BLAS_INFO,
 * _APREND_TLAS_DESC, _APREND_TLAS_INFO, _APREND_LIT_LIGHT_SHADOW
 * (5030-5034). */
