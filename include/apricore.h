
#ifndef APRICORE_H
#define APRICORE_H

#include <stdint.h>

/*
 * Every named enum in ApricotFields' headers is declared
 * `typedef enum NAME APRI_ENUM_BASE {`. C sees a plain enum. C++ sees the
 * enum with a fixed underlying type, uint32_t, which makes every 32-bit value
 * a value of the type: a C caller may store any integer in an enum field, and
 * a C++ implementation then reads it and refuses it by its check, where an
 * enum with no fixed type would make reading a value outside its enumerators
 * undefined. The size is 4 bytes either way.
 */
#if __cplusplus
#define APRI_ENUM_BASE : uint32_t
#else
#define APRI_ENUM_BASE
#endif

#if __cplusplus
extern "C" {
#endif

/*
 * Every Apricot module's calls that can fail return an APRESULT: APRESULT_OK,
 * or a code from the module's own block (APRESULT_<MODULE>_*).
 */
typedef enum APRESULT APRI_ENUM_BASE {
	APRESULT_OK = 0,

	/* General: any module may return these. */

	/* A null pointer where one is required, an out-of-range value, a
	 * buffer of the wrong size, a call in the wrong state. */
	APRESULT_INVALID_ARGUMENT = 1,
	APRESULT_OUT_OF_MEMORY = 2,
	/* A caller-supplied I/O callback, or the I/O beneath it, failed. */
	APRESULT_IO = 3,
	APRESULT_NOT_FOUND = 4,
	APRESULT_NOT_IMPLEMENTED = 5,
	/* Over a limit the caller set. */
	APRESULT_LIMIT = 6,
	/* Valid input using a feature this module doesn't handle. */
	APRESULT_UNSUPPORTED = 7,
	/* Data that fails its integrity check: a checksum mismatch, or a
	 * decoded size other than the declared one. */
	APRESULT_CORRUPT_DATA = 8,
	/* A struct's base.type isn't the type the call takes there, 0
	 * included (see "Struct bases" below). */
	APRESULT_STRUCT_WRONG_TYPE = 9,
	/* A struct chained on that the call doesn't take there, or that this
	 * build doesn't know. */
	APRESULT_STRUCT_UNKNOWN_IN_CHAIN = 10,
	/* One type twice in a chain. */
	APRESULT_STRUCT_DUPLICATE_IN_CHAIN = 11,

	/* ApArchive (archive/aparchive.h) */

	/* No end of central directory record: not a ZIP archive. */
	APRESULT_APARCHIVE_NOT_AN_ARCHIVE = 1001,
	/* A ZIP archive with a broken structure, or one that fails a check:
	 * overlapping entries, a local header that disagrees with the central
	 * directory, duplicate or unsafe names. */
	APRESULT_APARCHIVE_MALFORMED = 1002,
	/* An entry compressed with a method that isn't in the codec table. */
	APRESULT_APARCHIVE_UNKNOWN_METHOD = 1003,

	/* ApSync (sync/apsyncnet.h) */

	/* Another listener already has one of the addresses and the port. */
	APRESULT_APSYNC_ADDRESS_IN_USE = 2001,
	/* A joined session that has no snapshot yet, or whose connection to
	 * the host has ended. */
	APRESULT_APSYNC_NOT_CONNECTED = 2002,
} APRESULT;

/*
 * Struct bases. A struct that may gain fields has as its first member an
 * APRI_STRUCT_BASE (one the caller fills in) or an APRI_STRUCT_BASE_OUT (one
 * ApricotFields fills in), named [base]. [base.type] says which struct it
 * is; [base.next] points to the next struct in its chain, or is NULL. A
 * struct's layout never changes once it is released: a new field comes as
 * a new struct with a base of its own, which the caller chains on. A
 * struct with no base is frozen and never changes at all; a new need gets
 * a new type. This is SpudLib's SPUD_STRUCT_BASE (spudcore.h), with
 * ApricotFields' own types.
 *
 *   - The caller sets every [base.type] in a chain, the root's included. A
 *     type other than the one the call takes is refused with
 *     APRESULT_STRUCT_WRONG_TYPE, before any other field is read.
 *   - A chained struct the call doesn't take there, or this build doesn't
 *     know, is refused with APRESULT_STRUCT_UNKNOWN_IN_CHAIN, never
 *     skipped: skipping it would decide for the caller that it didn't
 *     matter. One type twice in a chain is refused with
 *     APRESULT_STRUCT_DUPLICATE_IN_CHAIN.
 *   - Leaving a chained struct out means that addition isn't used. A
 *     chained struct's fields have no defaults, as no desc's do.
 *   - A based struct is never held by value inside another struct.
 *
 *     APARCHIVE_READER_DESC desc = {
 *         .base = { APRI_STRUCT_TYPE_APARCHIVE_READER_DESC, NULL },
 *         ...
 *     };
 *
 * The member is nested, not two loose fields, so that a pointer to any
 * based struct converts to a pointer to its base and back (C11 6.7.2.1p15):
 * a chain is walked through APRI_STRUCT_BASE pointers with defined
 * behaviour.
 *
 * Numbered in blocks per module: 1000+ ApArchive, 2000+ ApSync, 3000+
 * ApAudio, 4000+ ApMath, 5000+ Aprend. A value is never renumbered or
 * reused, a retired struct's included.
 */
typedef enum APRI_STRUCT_TYPE APRI_ENUM_BASE {
	/* Never valid: a struct left zeroed is refused, not misread. */
	APRI_STRUCT_TYPE_UNKNOWN = 0,

	/* ApArchive (archive/aparchive.h) */
	APRI_STRUCT_TYPE_APARCHIVE_CODEC = 1000,
	APRI_STRUCT_TYPE_APARCHIVE_WRITER_DESC = 1001,
	APRI_STRUCT_TYPE_APARCHIVE_ENTRY_DESC = 1002,
	APRI_STRUCT_TYPE_APARCHIVE_READER_DESC = 1003,
	APRI_STRUCT_TYPE_APARCHIVE_ENTRY_INFO = 1004, /* APRI_STRUCT_BASE_OUT */

	/* ApSync (sync/apsyncnet.h) */
	APRI_STRUCT_TYPE_APSYNC_SESSION_DESC = 2000,
	APRI_STRUCT_TYPE_APSYNC_HOST_DESC = 2001,
	APRI_STRUCT_TYPE_APSYNC_JOIN_DESC = 2002,
	APRI_STRUCT_TYPE_APSYNC_EVENT = 2003, /* APRI_STRUCT_BASE_OUT */

	/* ApAudio (audio/apaudio.h, audio/apaudiocues.h) */
	APRI_STRUCT_TYPE_APAUDIO_MIXER_DESC = 3000,
	APRI_STRUCT_TYPE_APAUDIO_BUS_DESC = 3001,
	APRI_STRUCT_TYPE_APAUDIO_MODIFIER_DESC = 3002,
	APRI_STRUCT_TYPE_APAUDIO_CONCURRENCY_DESC = 3003,
	APRI_STRUCT_TYPE_APAUDIO_SOUND_DESC = 3004,
	APRI_STRUCT_TYPE_APAUDIO_PLAY_DESC = 3005,
	APRI_STRUCT_TYPE_APAUDIO_CUES_DESC = 3006,
	APRI_STRUCT_TYPE_APAUDIO_CUE_INFO = 3007, /* APRI_STRUCT_BASE_OUT */
	APRI_STRUCT_TYPE_APAUDIO_CUE_PLAY_DESC = 3008,

	/* ApMath (math/apmathbvh.h, math/apmathpoly.h) */
	APRI_STRUCT_TYPE_APMATH_MESH_BVH_DESC = 4000,
	APRI_STRUCT_TYPE_APMATH_INSTANCE_BVH_DESC = 4001,
	APRI_STRUCT_TYPE_APMATH_CAST_DESC = 4002,
	APRI_STRUCT_TYPE_APMATH_POLY_BOOLEAN_DESC = 4003,
	APRI_STRUCT_TYPE_APMATH_POLY_OFFSET_DESC = 4004,
	APRI_STRUCT_TYPE_APMATH_POLY_SKELETON_DESC = 4005,

	/* Aprend (render/aprend*.h) */
	APRI_STRUCT_TYPE_APREND_INSTANCE_DESC = 5000,
	APRI_STRUCT_TYPE_APREND_VERTEX_POOL_DESC = 5001,
	APRI_STRUCT_TYPE_APREND_INDEX_POOL_DESC = 5002,
	APRI_STRUCT_TYPE_APREND_DRAW_LIST_DESC = 5003,
	APRI_STRUCT_TYPE_APREND_FRAMEBUFFER_DESC = 5004,
	APRI_STRUCT_TYPE_APREND_TEXTURE2D_DESC = 5005,
	APRI_STRUCT_TYPE_APREND_TEXTURE3D_DESC = 5006,
	APRI_STRUCT_TYPE_APREND_TEXTURE_VIEW_DESC = 5007,
	APRI_STRUCT_TYPE_APREND_SAMPLER_DESC = 5008,
	APRI_STRUCT_TYPE_APREND_MATERIAL_DESC = 5009,
	APRI_STRUCT_TYPE_APREND_RENDER_PASS_DESC = 5010,
	APRI_STRUCT_TYPE_APREND_COMPUTE_PASS_DESC = 5011,
	APRI_STRUCT_TYPE_APREND_GRAPHICS_PIPELINE_DESC = 5012,
	APRI_STRUCT_TYPE_APREND_COMPUTE_PIPELINE_DESC = 5013,
	APRI_STRUCT_TYPE_APREND_SWAP_CHAIN_DESC = 5014,
	APRI_STRUCT_TYPE_APREND_GPU_TIMER_DESC = 5015,
	APRI_STRUCT_TYPE_APREND_EXPOSURE_DESC = 5016,
	APRI_STRUCT_TYPE_APREND_LIT_ENVIRONMENT_DESC = 5017,
	APRI_STRUCT_TYPE_APREND_LIT_MATERIAL_VALUES = 5018,
	APRI_STRUCT_TYPE_APREND_LIT_VIEW = 5019,
	APRI_STRUCT_TYPE_APREND_LIT_SUN = 5020,
	APRI_STRUCT_TYPE_APREND_LIT_SUN_SHADOW = 5021,
	APRI_STRUCT_TYPE_APREND_LIT_CASCADE_FIT = 5022,
	APRI_STRUCT_TYPE_APREND_LIT_AMBIENT = 5023,

	/* Keeps the enum 32 bits wide on every compiler. Never a valid type. */
	APRI_STRUCT_TYPE_MAX_ENUM = 0x7FFFFFFF
} APRI_STRUCT_TYPE;

/* The first member, [base], of every extensible struct the caller fills
 * in. */
/* Frozen: the base every based struct starts with. It never changes; a new need gets a new type. */
typedef struct APRI_STRUCT_BASE {
	/* This struct's own type. */
	APRI_STRUCT_TYPE type;
	/* The next struct in the chain, or NULL. ApricotFields only reads it. */
	const struct APRI_STRUCT_BASE *next;
} APRI_STRUCT_BASE;

/* The first member, [base], of every extensible struct ApricotFields fills
 * in for the caller. The caller builds the chain and sets each [type];
 * ApricotFields writes the other fields and leaves [type] and [next] as
 * they are. */
/* Frozen: the base every based struct starts with. It never changes; a new need gets a new type. */
typedef struct APRI_STRUCT_BASE_OUT {
	/* This struct's own type. */
	APRI_STRUCT_TYPE type;
	/* The next struct in the chain, or NULL. */
	struct APRI_STRUCT_BASE_OUT *next;
} APRI_STRUCT_BASE_OUT;

/*
 * Every implementation of an Apricot opaque handle has as its first member a
 * pointer to a null-terminated string, the object's debug name, for
 * debugging and validation layers. This holds in every build configuration:
 * a debug name never changes a struct's layout or which functions exist.
 *
 * apri_debug_name_set stores [name] itself, not a copy: the string must stay
 * valid until the object is destroyed or named again. NULL removes the name.
 * apri_debug_name_get returns the stored pointer, or NULL for a NULL object
 * or one with no name.
 */
void apri_debug_name_set(void *object, const char *name);
#define APRI_SET_DEBUG_NAME(obj, name)                                         \
	apri_debug_name_set((void *)(obj), (name))
const char *apri_debug_name_get(void *object);
#define APRI_GET_DEBUG_NAME(obj) apri_debug_name_get((void *)(obj))

#if __cplusplus
}
#endif

#endif // APRICORE_H
