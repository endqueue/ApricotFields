
#ifndef RBTCORE_H
#define RBTCORE_H

#include <stdint.h>

/**
 * @file rbtcore.h
 * @brief What every Rabbittus module shares: the version, the RBT_RESULT
 * codes, the enum base, struct bases and their types, and debug names.
 */

/**
 * @brief The underlying type of every named Rabbittus enum.
 *
 * Every named enum in Rabbittus' headers is declared
 * `typedef enum NAME RBT_ENUM_BASE {`. C sees a plain enum. C++ sees the
 * enum with a fixed underlying type, uint32_t, which makes every 32-bit value
 * a value of the type: a C caller may store any integer in an enum field, and
 * a C++ implementation then reads it and refuses it by its check, where an
 * enum with no fixed type would make reading a value outside its enumerators
 * undefined. The size is 4 bytes either way.
 */
#if __cplusplus
#define RBT_ENUM_BASE : uint32_t
#else
#define RBT_ENUM_BASE
#endif

#if __cplusplus
extern "C" {
#endif

/* ---- Version ---- */

/**
 * @defgroup rbt_version Version
 * @brief Which Rabbittus a caller was built against, and which it runs
 * on.
 *
 * One version for the whole library: Rabbittus is built and linked as
 * one library, so its modules can't be mixed from different versions.
 *
 * What a version promises, from 1.0.0 on:
 *
 * | Change in | May add | May change or remove |
 * |-----------|---------|----------------------|
 * | patch     | nothing | nothing: fixes only, no header change but comments |
 * | minor     | functions, RBT_RESULT values, RBT_STRUCT_TYPE values, chained structs, enumerators | nothing |
 * | major     | anything | anything |
 *
 * "Released" means a commit tagged vMAJOR.MINOR.PATCH. Before 1.0.0, a
 * minor may change anything, and every change to the public API is listed
 * against the workspace's ABI snapshot (`abi.txt`).
 *
 * A struct that can be chained says in its comment which version first
 * takes it ("Since 1.2.0"), so that a caller can ask rbt_get_version()
 * before chaining it. A caller that doesn't ask loses nothing but the
 * question: an Rabbittus that doesn't take a struct refuses the chain
 * with RBT_RESULT_STRUCT_UNKNOWN_IN_CHAIN.
 * @{
 */

/** @brief Packs a version into one number that compares with < and >:
 * 10 bits of major, 10 of minor, 12 of patch (Carrottus's layout). */
#define RABBITTUS_MAKE_VERSION(major, minor, patch) \
	((((uint32_t)(major)) << 22U) | (((uint32_t)(minor)) << 12U) | ((uint32_t)(patch)))
/** @brief The major part of a packed version. */
#define RABBITTUS_VERSION_GET_MAJOR(version) ((uint32_t)(version) >> 22U)
/** @brief The minor part of a packed version. */
#define RABBITTUS_VERSION_GET_MINOR(version) (((uint32_t)(version) >> 12U) & 0x3FFU)
/** @brief The patch part of a packed version. */
#define RABBITTUS_VERSION_GET_PATCH(version) ((uint32_t)(version) & 0xFFFU)

/* The one place the version is written. CMakeLists.txt reads these three
 * lines, so each stays "#define NAME <number>" and nothing else. */
/** @brief The major version of these headers. */
#define RABBITTUS_VERSION_MAJOR 0
/** @brief The minor version of these headers. */
#define RABBITTUS_VERSION_MINOR 1
/** @brief The patch version of these headers. */
#define RABBITTUS_VERSION_PATCH 0

/** @brief The version of the headers the caller is compiled against. */
#define RABBITTUS_VERSION \
	RABBITTUS_MAKE_VERSION(RABBITTUS_VERSION_MAJOR, RABBITTUS_VERSION_MINOR, RABBITTUS_VERSION_PATCH)

/**
 * @brief Returns the version of the Rabbittus the caller is running on:
 * the RABBITTUS_VERSION it was compiled with.
 *
 * It can differ from the caller's own RABBITTUS_VERSION only when
 * Rabbittus was built separately from the caller. Which one to accept
 * is the caller's decision; Rabbittus refuses no caller for its
 * version.
 *
 * @return The packed version (RABBITTUS_MAKE_VERSION). Never 0.
 */
uint32_t rbt_get_version(void);

/** @} */

/* ---- Threads ---- */

/**
 * @defgroup rbt_threads Threads
 * @brief Which calls may overlap: the rule every Rabbittus header rests
 * on.
 *
 * Every object, in every module, is used by one caller at a time: no two
 * calls that take the same object may overlap. Keeping it so is the
 * caller's, as with Vulkan's "externally synchronized" objects;
 * Rabbittus takes no lock to make it so.
 *
 * The rule is serialization, not thread identity. A caller may call from
 * any thread, and from a different thread each time - a Swift actor, a C#
 * continuation, a job system - as long as no two of its calls on one
 * object overlap. Different objects are independent: two callers may each
 * use their own at the same time. A module whose objects share state says
 * so in its header, and names its unit: Rend's is the instance with
 * everything created from it (rbtrendcontext.h, "Threads").
 *
 * A function that takes no object, only its arguments, is safe from any
 * number of callers at once.
 *
 * Exceptions are few, and each is marked on the call itself, with a line
 * beginning "Thread:" that says exactly what may overlap what: a call that
 * may come from any thread at any time, reads of an object that no longer
 * changes, a value another thread reads, a cancel or wake, or code that
 * runs on a thread other than the caller's. A module with more to say than
 * one call's worth has a "Threads" paragraph at the top of its header, for
 * what is particular to it alone.
 *
 * Two overlapping calls on one object are a data race, not a failure a
 * call reports. A Debug build looks for it, in every module: each call
 * the rule covers - every call on an object but its destroy and the calls
 * marked "Thread:" - opens by noting it is inside, and one that finds a
 * call from another thread still inside the same object prints both
 * calls' names on stderr and goes on, as Vulkan's validation layer reports
 * an object used by two threads at once. A module whose objects share a
 * unit checks the unit (Rend's instance; Rabbittus Audio's roles, a mixer's
 * control calls and its render each). It checks overlap, not which thread
 * calls: a call from another thread once the last has returned is no
 * overlap, nor is a call made from inside another on the same thread. No
 * Release build checks anything.
 *
 * Where Rabbittus itself runs code on a thread of its own today, the
 * header says so on the call: Rabbittus Sync starts a network thread for each
 * session, and a joining session one more, to look its host up
 * (rbtsyncnet.h). Rabbittus Audio starts none; its render call runs on whatever
 * thread the caller's audio stream calls it from (rbtaudio.h). Rabbittus Jobs'
 * reference pool, which a caller creates only if it wants one, starts the
 * threads its desc asks for and runs tasks on them (rbtjobs.h). Nothing else
 * in Rabbittus starts a thread or calls back from one.
 */

/**
 * @brief The result of every Apricot call that can fail.
 *
 * Every Apricot module's calls that can fail return an RBT_RESULT: RBT_RESULT_OK,
 * or a code from the module's own block (RBT_RESULT_<MODULE>_*).
 */
typedef enum RBT_RESULT RBT_ENUM_BASE {
	/** The call succeeded. */
	RBT_RESULT_OK = 0,

	/* General: any module may return these. */

	/** A null pointer where one is required, an out-of-range value, a
	 * buffer of the wrong size, a call in the wrong state. */
	RBT_RESULT_INVALID_ARGUMENT = 1,
	/** An allocation failed. */
	RBT_RESULT_OUT_OF_MEMORY = 2,
	/** A caller-supplied I/O callback, or the I/O beneath it, failed. */
	RBT_RESULT_IO = 3,
	/** What the call was asked for doesn't exist, such as an entry, an
	 * index or a handle. */
	RBT_RESULT_NOT_FOUND = 4,
	/** The call, or the part of it asked for, isn't built yet. */
	RBT_RESULT_NOT_IMPLEMENTED = 5,
	/** Over a limit the caller set. */
	RBT_RESULT_LIMIT = 6,
	/** Valid input using a feature this module doesn't handle. */
	RBT_RESULT_UNSUPPORTED = 7,
	/** Data that fails its integrity check: a checksum mismatch, or a
	 * decoded size other than the declared one. */
	RBT_RESULT_CORRUPT_DATA = 8,
	/** A struct's `base.type` isn't the type the call takes there, 0
	 * included (see "Struct bases" at RBT_STRUCT_TYPE). */
	RBT_RESULT_STRUCT_WRONG_TYPE = 9,
	/** A struct chained on that the call doesn't take there, or that this
	 * build doesn't know. */
	RBT_RESULT_STRUCT_UNKNOWN_IN_CHAIN = 10,
	/** One type twice in a chain. */
	RBT_RESULT_STRUCT_DUPLICATE_IN_CHAIN = 11,
	/** A wait's time limit ran out first. Nothing the call would have
	 * done is done. */
	RBT_RESULT_TIMEOUT = 12,

	/* Rabbittus Archive (archive/rbtarchive.h) */

	/** No end of central directory record: not a ZIP archive. */
	RBT_RESULT_ARCHIVE_NOT_AN_ARCHIVE = 1001,
	/** A ZIP archive with a broken structure, or one that fails a check:
	 * overlapping entries, a local header that disagrees with the central
	 * directory, duplicate or unsafe names. */
	RBT_RESULT_ARCHIVE_MALFORMED = 1002,
	/** An entry compressed with a method that isn't in the codec table. */
	RBT_RESULT_ARCHIVE_UNKNOWN_METHOD = 1003,

	/* Rabbittus Sync (sync/rbtsyncnet.h) */

	/** Another listener already has one of the addresses and the port. */
	RBT_RESULT_SYNC_ADDRESS_IN_USE = 2001,
	/** A joined session that has no snapshot yet, or whose connection to
	 * the host has ended. */
	RBT_RESULT_SYNC_NOT_CONNECTED = 2002,

	/* Rend (render/rbtrend*.h) */

	/** Carrottus GPU refused something Rend asked of it, for a reason no other
	 * code names; the call's RBTREND_ERROR names Carrottus GPU's result. */
	RBT_RESULT_REND_GPU_FAILURE = 5001,
	/** A command list compiled against something that has changed since:
	 * destroyed, resized, updated, or an earlier frame. Compile it again. */
	RBT_RESULT_REND_STALE = 5002,
	/** The device is lost (removed, or reset after a hang): nothing more
	 * the instance submits runs. Destroy what was made of it, the instance
	 * last, and the device; a caller that goes on makes them again. */
	RBT_RESULT_REND_DEVICE_LOST = 5003,
	/** A shader package's reflection was written by tools/rbtrend_reflect
	 * of another format version than this library reads. Rebuilding the
	 * caller's shaders with this version's tool fixes it. */
	RBT_RESULT_REND_REFLECTION_MISMATCH = 5004,

	/* Rabbittus Text (text/rbttext.h) */

	/** Bytes that aren't a font in the format the call reads (OpenType or
	 * TrueType, or a Hershey font), or one too broken to read. */
	RBT_RESULT_TEXT_NOT_A_FONT = 6001,
	/** Saved atlas bytes made with other settings, other faces or another
	 * version of the glyph generator: well formed, but not this atlas. */
	RBT_RESULT_TEXT_ATLAS_MISMATCH = 6002,
} RBT_RESULT;

/**
 * @brief Which struct a based struct is: the value of its `base.type`.
 *
 * @par Struct bases
 * A struct that may gain fields has as its first member an
 * RBT_STRUCT_BASE (one the caller fills in) or an RBT_STRUCT_BASE_OUT (one
 * Rabbittus fills in), named `base`. `base.type` says which struct it
 * is; `base.next` points to the next struct in its chain, or is NULL. A
 * struct's layout never changes once it is released: a new field comes as
 * a new struct with a base of its own, which the caller chains on. A
 * struct with no base is frozen and never changes at all; a new need gets
 * a new type. This is Carrottus's CR_STRUCT_BASE (crcore.h), with
 * Rabbittus' own types.
 *
 *   - The caller sets every `base.type` in a chain, the root's included. A
 *     type other than the one the call takes is refused with
 *     RBT_RESULT_STRUCT_WRONG_TYPE, before any other field is read.
 *   - A chained struct the call doesn't take there, or this build doesn't
 *     know, is refused with RBT_RESULT_STRUCT_UNKNOWN_IN_CHAIN, never
 *     skipped: skipping it would decide for the caller that it didn't
 *     matter. One type twice in a chain is refused with
 *     RBT_RESULT_STRUCT_DUPLICATE_IN_CHAIN.
 *   - Leaving a chained struct out means that addition isn't used. A
 *     chained struct's fields have no defaults, as no desc's do.
 *   - A based struct is never held by value inside another struct.
 *
 * @code
 *     RBTARCHIVE_READER_DESC desc = {
 *         .base = { RBT_STRUCT_TYPE_ARCHIVE_READER_DESC, NULL },
 *         ...
 *     };
 * @endcode
 *
 * The member is nested, not two loose fields, so that a pointer to any
 * based struct converts to a pointer to its base and back (C11 6.7.2.1p15):
 * a chain is walked through RBT_STRUCT_BASE pointers with defined
 * behaviour.
 *
 * Numbered in blocks per module: 1000+ Rabbittus Archive, 2000+ Rabbittus Sync, 3000+
 * Rabbittus Audio, 4000+ Rabbittus Math, 5000+ Rend, 6000+ Rabbittus Text. A value is never renumbered or
 * reused, a retired struct's included. Each enumerator names its struct;
 * the ones marked RBT_STRUCT_BASE_OUT are structs Rabbittus fills in.
 */
typedef enum RBT_STRUCT_TYPE RBT_ENUM_BASE {
	/** Never valid: a struct left zeroed is refused, not misread. */
	RBT_STRUCT_TYPE_UNKNOWN = 0,

	/* Rabbittus Archive (archive/rbtarchive.h) */
	RBT_STRUCT_TYPE_ARCHIVE_CODEC = 1000, /**< RBTARCHIVE_CODEC. */
	RBT_STRUCT_TYPE_ARCHIVE_WRITER_DESC = 1001, /**< RBTARCHIVE_WRITER_DESC. */
	RBT_STRUCT_TYPE_ARCHIVE_ENTRY_DESC = 1002, /**< RBTARCHIVE_ENTRY_DESC. */
	RBT_STRUCT_TYPE_ARCHIVE_READER_DESC = 1003, /**< RBTARCHIVE_READER_DESC. */
	RBT_STRUCT_TYPE_ARCHIVE_ENTRY_INFO = 1004, /**< RBTARCHIVE_ENTRY_INFO, an RBT_STRUCT_BASE_OUT. */
	RBT_STRUCT_TYPE_ARCHIVE_UPDATE_DESC = 1005, /**< RBTARCHIVE_UPDATE_DESC. */

	/* Rabbittus Sync (sync/rbtsyncnet.h) */
	RBT_STRUCT_TYPE_SYNC_SESSION_DESC = 2000, /**< RBTSYNC_SESSION_DESC. */
	RBT_STRUCT_TYPE_SYNC_HOST_DESC = 2001, /**< RBTSYNC_HOST_DESC. */
	RBT_STRUCT_TYPE_SYNC_JOIN_DESC = 2002, /**< RBTSYNC_JOIN_DESC. */
	RBT_STRUCT_TYPE_SYNC_EVENT = 2003, /**< RBTSYNC_EVENT, an RBT_STRUCT_BASE_OUT. */

	/* Rabbittus Audio (audio/rbtaudio.h, audio/rbtaudiocues.h) */
	RBT_STRUCT_TYPE_AUDIO_MIXER_DESC = 3000, /**< RBTAUDIO_MIXER_DESC. */
	RBT_STRUCT_TYPE_AUDIO_BUS_DESC = 3001, /**< RBTAUDIO_BUS_DESC. */
	RBT_STRUCT_TYPE_AUDIO_MODIFIER_DESC = 3002, /**< RBTAUDIO_MODIFIER_DESC. */
	RBT_STRUCT_TYPE_AUDIO_CONCURRENCY_DESC = 3003, /**< RBTAUDIO_CONCURRENCY_DESC. */
	RBT_STRUCT_TYPE_AUDIO_SOUND_DESC = 3004, /**< RBTAUDIO_SOUND_DESC. */
	RBT_STRUCT_TYPE_AUDIO_PLAY_DESC = 3005, /**< RBTAUDIO_PLAY_DESC. */
	RBT_STRUCT_TYPE_AUDIO_CUES_DESC = 3006, /**< RBTAUDIO_CUES_DESC. */
	RBT_STRUCT_TYPE_AUDIO_CUE_INFO = 3007, /**< RBTAUDIO_CUE_INFO, an RBT_STRUCT_BASE_OUT. */
	RBT_STRUCT_TYPE_AUDIO_CUE_PLAY_DESC = 3008, /**< RBTAUDIO_CUE_PLAY_DESC. */
	RBT_STRUCT_TYPE_AUDIO_OPUS_ENCODE_DESC = 3009, /**< RBTAUDIO_OPUS_ENCODE_DESC. */
	RBT_STRUCT_TYPE_AUDIO_SPATIAL_DESC = 3010, /**< RBTAUDIO_SPATIAL_DESC, chained onto RBTAUDIO_MIXER_DESC. */
	RBT_STRUCT_TYPE_AUDIO_ATTENUATION_DESC = 3011, /**< RBTAUDIO_ATTENUATION_DESC. */

	/* Rabbittus Math (math/rbtmathbvh.h, math/rbtmathpoly.h) */
	RBT_STRUCT_TYPE_MATH_MESH_BVH_DESC = 4000, /**< RBTMATH_MESH_BVH_DESC. */
	RBT_STRUCT_TYPE_MATH_INSTANCE_BVH_DESC = 4001, /**< RBTMATH_INSTANCE_BVH_DESC. */
	RBT_STRUCT_TYPE_MATH_CAST_DESC = 4002, /**< RBTMATH_CAST_DESC. */
	RBT_STRUCT_TYPE_MATH_POLY_BOOLEAN_DESC = 4003, /**< RBTMATH_POLY_BOOLEAN_DESC. */
	RBT_STRUCT_TYPE_MATH_POLY_OFFSET_DESC = 4004, /**< RBTMATH_POLY_OFFSET_DESC. */
	RBT_STRUCT_TYPE_MATH_POLY_SKELETON_DESC = 4005, /**< RBTMATH_POLY_SKELETON_DESC. */
	RBT_STRUCT_TYPE_MATH_BOX_BVH_DESC = 4006, /**< RBTMATH_BOX_BVH_DESC. */

	/* Rend (render/rbtrend*.h) */
	RBT_STRUCT_TYPE_REND_INSTANCE_DESC = 5000, /**< RBTREND_INSTANCE_DESC. */
	RBT_STRUCT_TYPE_REND_VERTEX_POOL_DESC = 5001, /**< RBTREND_VERTEX_POOL_DESC. */
	RBT_STRUCT_TYPE_REND_INDEX_POOL_DESC = 5002, /**< RBTREND_INDEX_POOL_DESC. */
	RBT_STRUCT_TYPE_REND_DRAW_LIST_DESC = 5003, /**< RBTREND_DRAW_LIST_DESC. */
	RBT_STRUCT_TYPE_REND_FRAMEBUFFER_DESC = 5004, /**< RBTREND_FRAMEBUFFER_DESC. */
	RBT_STRUCT_TYPE_REND_TEXTURE2D_DESC = 5005, /**< RBTREND_TEXTURE2D_DESC. */
	RBT_STRUCT_TYPE_REND_TEXTURE3D_DESC = 5006, /**< RBTREND_TEXTURE3D_DESC. */
	RBT_STRUCT_TYPE_REND_TEXTURE_VIEW_DESC = 5007, /**< RBTREND_TEXTURE_VIEW_DESC. */
	RBT_STRUCT_TYPE_REND_SAMPLER_DESC = 5008, /**< RBTREND_SAMPLER_DESC. */
	RBT_STRUCT_TYPE_REND_MATERIAL_DESC = 5009, /**< RBTREND_MATERIAL_DESC. */
	RBT_STRUCT_TYPE_REND_RENDER_PASS_DESC = 5010, /**< RBTREND_RENDER_PASS_DESC. */
	RBT_STRUCT_TYPE_REND_COMPUTE_PASS_DESC = 5011, /**< RBTREND_COMPUTE_PASS_DESC. */
	RBT_STRUCT_TYPE_REND_GRAPHICS_PIPELINE_DESC = 5012, /**< RBTREND_GRAPHICS_PIPELINE_DESC. */
	RBT_STRUCT_TYPE_REND_COMPUTE_PIPELINE_DESC = 5013, /**< RBTREND_COMPUTE_PIPELINE_DESC. */
	RBT_STRUCT_TYPE_REND_SWAP_CHAIN_DESC = 5014, /**< RBTREND_SWAP_CHAIN_DESC. */
	RBT_STRUCT_TYPE_REND_GPU_TIMER_DESC = 5015, /**< RBTREND_GPU_TIMER_DESC. */
	RBT_STRUCT_TYPE_REND_EXPOSURE_DESC = 5016, /**< RBTREND_EXPOSURE_DESC. */
	RBT_STRUCT_TYPE_REND_LIT_ENVIRONMENT_DESC = 5017, /**< RBTREND_LIT_ENVIRONMENT_DESC. */
	RBT_STRUCT_TYPE_REND_LIT_MATERIAL_VALUES = 5018, /**< RBTREND_LIT_MATERIAL_VALUES. */
	RBT_STRUCT_TYPE_REND_LIT_VIEW = 5019, /**< RBTREND_LIT_VIEW. */
	/* 5020 and 5021 were RBTREND_LIT_SUN and RBTREND_LIT_SUN_SHADOW, gone with
	 * card A35: never reused. */
	RBT_STRUCT_TYPE_REND_LIT_CASCADE_FIT = 5022, /**< RBTREND_LIT_CASCADE_FIT. */
	RBT_STRUCT_TYPE_REND_LIT_AMBIENT = 5023, /**< RBTREND_LIT_AMBIENT. */
	RBT_STRUCT_TYPE_REND_RENDER_TARGET_POOL_INFO = 5024, /**< RBTREND_RENDER_TARGET_POOL_INFO, an RBT_STRUCT_BASE_OUT. */
	RBT_STRUCT_TYPE_REND_BUFFER_DESC = 5025, /**< RBTREND_BUFFER_DESC. */
	RBT_STRUCT_TYPE_REND_BATCH_DESC = 5026, /**< RBTREND_BATCH_DESC. */
	RBT_STRUCT_TYPE_REND_BATCH_FRAME = 5027, /**< RBTREND_BATCH_FRAME. */
	RBT_STRUCT_TYPE_REND_BATCH_LINE = 5028, /**< RBTREND_BATCH_LINE. */
	RBT_STRUCT_TYPE_REND_SHADER_PACKAGE = 5029, /**< RBTREND_SHADER_PACKAGE. */
	RBT_STRUCT_TYPE_REND_BLAS_DESC = 5030, /**< RBTREND_BLAS_DESC. */
	RBT_STRUCT_TYPE_REND_BLAS_INFO = 5031, /**< RBTREND_BLAS_INFO, an RBT_STRUCT_BASE_OUT. */
	RBT_STRUCT_TYPE_REND_TLAS_DESC = 5032, /**< RBTREND_TLAS_DESC. */
	RBT_STRUCT_TYPE_REND_TLAS_INFO = 5033, /**< RBTREND_TLAS_INFO, an RBT_STRUCT_BASE_OUT. */
	RBT_STRUCT_TYPE_REND_LIT_LIGHT_SHADOW = 5034, /**< RBTREND_LIT_LIGHT_SHADOW. */
	RBT_STRUCT_TYPE_REND_LIT_SHADOW_MAPS = 5035, /**< RBTREND_LIT_SHADOW_MAPS. */

	/* Rabbittus Text (text/rbttext.h) */
	RBT_STRUCT_TYPE_TEXT_FACE_DESC = 6000, /**< RBTTEXT_FACE_DESC. */
	RBT_STRUCT_TYPE_TEXT_STROKE_FACE_DESC = 6001, /**< RBTTEXT_STROKE_FACE_DESC. */
	RBT_STRUCT_TYPE_TEXT_HERSHEY_FACE_DESC = 6002, /**< RBTTEXT_HERSHEY_FACE_DESC. */
	RBT_STRUCT_TYPE_TEXT_FACE_METRICS = 6003, /**< RBTTEXT_FACE_METRICS, an RBT_STRUCT_BASE_OUT. */
	RBT_STRUCT_TYPE_TEXT_CHAIN_DESC = 6004, /**< RBTTEXT_CHAIN_DESC. */
	RBT_STRUCT_TYPE_TEXT_LAYOUT_DESC = 6005, /**< RBTTEXT_LAYOUT_DESC. */
	RBT_STRUCT_TYPE_TEXT_LAYOUT_INFO = 6006, /**< RBTTEXT_LAYOUT_INFO, an RBT_STRUCT_BASE_OUT. */
	RBT_STRUCT_TYPE_TEXT_GLYPH_GENERATE_DESC = 6007, /**< RBTTEXT_GLYPH_GENERATE_DESC. */
	RBT_STRUCT_TYPE_TEXT_GLYPH_IMAGE = 6008, /**< RBTTEXT_GLYPH_IMAGE, an RBT_STRUCT_BASE_OUT. */
	RBT_STRUCT_TYPE_TEXT_ATLAS_DESC = 6009, /**< RBTTEXT_ATLAS_DESC. */
	RBT_STRUCT_TYPE_TEXT_ATLAS_INFO = 6010, /**< RBTTEXT_ATLAS_INFO, an RBT_STRUCT_BASE_OUT. */
	RBT_STRUCT_TYPE_TEXT_ATLAS_GLYPH = 6011, /**< RBTTEXT_ATLAS_GLYPH, an RBT_STRUCT_BASE_OUT. */
	RBT_STRUCT_TYPE_TEXT_ATLAS_LOAD_DESC = 6012, /**< RBTTEXT_ATLAS_LOAD_DESC. */
	RBT_STRUCT_TYPE_TEXT_LFF_FACE_DESC = 6013, /**< RBTTEXT_LFF_FACE_DESC. */
	RBT_STRUCT_TYPE_TEXT_SHX_FACE_DESC = 6014, /**< RBTTEXT_SHX_FACE_DESC. */

	/* Rabbittus Jobs (jobs/rbtjobs.h) */
	RBT_STRUCT_TYPE_JOBS_SCHEDULER = 7000, /**< RBTJOBS_SCHEDULER. */
	RBT_STRUCT_TYPE_JOBS_POOL_DESC = 7001, /**< RBTJOBS_POOL_DESC. */
	RBT_STRUCT_TYPE_JOBS_SCHEDULER_GROUPS = 7002, /**< RBTJOBS_SCHEDULER_GROUPS, chained onto RBTJOBS_SCHEDULER. */

	/** Keeps the enum 32 bits wide on every compiler. Never a valid type. */
	RBT_STRUCT_TYPE_MAX_ENUM = 0x7FFFFFFF
} RBT_STRUCT_TYPE;

/**
 * @brief The first member, `base`, of every extensible struct the caller
 * fills in.
 *
 * @see RBT_STRUCT_TYPE ("Struct bases")
 */
/* Frozen: the base every based struct starts with. It never changes; a new need gets a new type. */
typedef struct RBT_STRUCT_BASE {
	/** This struct's own type. */
	RBT_STRUCT_TYPE type;
	/** The next struct in the chain, or NULL. Rabbittus only reads it. */
	const struct RBT_STRUCT_BASE *next;
} RBT_STRUCT_BASE;

/**
 * @brief The first member, `base`, of every extensible struct Rabbittus
 * fills in for the caller.
 *
 * The caller builds the chain and sets each `type`; Rabbittus writes the
 * other fields and leaves `type` and `next` as they are.
 *
 * @see RBT_STRUCT_TYPE ("Struct bases")
 */
/* Frozen: the base every based struct starts with. It never changes; a new need gets a new type. */
typedef struct RBT_STRUCT_BASE_OUT {
	/** This struct's own type. */
	RBT_STRUCT_TYPE type;
	/** The next struct in the chain, or NULL. */
	struct RBT_STRUCT_BASE_OUT *next;
} RBT_STRUCT_BASE_OUT;

/* ---- Debug names ---- */

/**
 * @brief Sets an object's debug name.
 *
 * Every implementation of an Apricot opaque handle has as its first member a
 * pointer to a null-terminated string, the object's debug name, for
 * debugging and validation layers. This holds in every build configuration:
 * a debug name never changes a struct's layout or which functions exist.
 *
 * Stores @p name itself, not a copy: the string must stay valid until the
 * object is destroyed or named again.
 *
 * @param[in] object Any Apricot opaque handle. NULL is accepted and does
 *                   nothing.
 * @param[in] name   The debug name. NULL removes the name.
 *
 * @see rbt_debug_name_get(), RBT_SET_DEBUG_NAME
 */
void rbt_debug_name_set(void *object, const char *name);
/**
 * @brief rbt_debug_name_set() on any handle, cast to `void *`.
 *
 * @param obj  The handle.
 * @param name The debug name, or NULL.
 */
#define RBT_SET_DEBUG_NAME(obj, name)                                         \
	rbt_debug_name_set((void *)(obj), (name))
/**
 * @brief Gets an object's debug name.
 *
 * @param[in] object Any Apricot opaque handle, or NULL.
 *
 * @return The stored pointer, or NULL for a NULL object or one with no name.
 *
 * @see rbt_debug_name_set(), RBT_GET_DEBUG_NAME
 */
const char *rbt_debug_name_get(void *object);
/**
 * @brief rbt_debug_name_get() on any handle, cast to `void *`.
 *
 * @param obj The handle.
 */
#define RBT_GET_DEBUG_NAME(obj) rbt_debug_name_get((void *)(obj))

#if __cplusplus
}
#endif

#endif // RBTCORE_H
