#ifndef APTEXT_H
#define APTEXT_H

#include "../apricore.h"
#include "../aprimath.h"
#include <stdbool.h>
#include <stdint.h>

#if __cplusplus
extern "C" {
#endif

/**
 * @file aptext.h
 * @brief ApText - text into positioned glyphs, and the glyphs' distance-field
 * atlas.
 *
 * General purpose: it doesn't know what the text is for. A string, a style
 * and a width go in; glyph ids, positions, source byte ranges and boxes come
 * out, and for the glyphs a multi-channel signed distance field atlas
 * (MTSDF) to draw them from, or for a single-stroke face the strokes as line
 * segments.
 *
 * What it holds, each an object the caller creates and passes in:
 *   - Faces (#aptext_face): an OpenType or TrueType font made from bytes the
 *     caller supplies (aptext_face_create()), or a single-stroke face made
 *     from the caller's polylines (aptext_face_create_stroke()) or read from
 *     a Hershey font (aptext_face_create_hershey()).
 *   - Fallback chains (#aptext_chain): an ordered list of faces; for each
 *     codepoint the first face that has a glyph for it wins.
 *   - Layouts (#aptext_layout): UTF-8 text shaped and broken into lines
 *     with one chain.
 *   - Atlases (#aptext_atlas): the MTSDF of every glyph a caller has asked
 *     for, packed on demand into pages of a size the caller chose.
 *
 * What it deliberately does not do:
 *   - No file I/O. A face is made from bytes in memory; an atlas is saved
 *     to and loaded from bytes in memory. No font search path, no system
 *     font lookup, no disk cache: where fonts and cached atlases live is
 *     the caller's.
 *   - No GPU. An atlas page is RGBA8 pixels in memory and the glyphs are
 *     positions and rectangles; uploading and drawing them is the caller's.
 *   - No default font and no global state.
 *   - No platform text engine. Shaping, bidirectional reordering and line
 *     breaking are one implementation on every platform, so the same input
 *     gives the same output, bit for bit, everywhere: a layout can be
 *     cached, compared or sent to another machine by its caller.
 *
 * @par Coordinates
 * Two spaces, and every field says which it is in:
 *   - Font space, for what describes a face (APTEXT_FACE_METRICS, and the
 *     polylines a caller gives aptext_face_create_stroke()): em units (1 is
 *     the em), y up, the origin on the baseline at the glyph's pen
 *     position. A descender is negative.
 *   - Layout space, for everything ApText produces: x right, y down, the
 *     origin at the top-left of the layout, in output units, which are em
 *     units times APTEXT_LAYOUT_DESC's `em_size` (an `em_size` of 1 gives em
 *     units, a pixel size gives pixels). An atlas glyph's plane bounds are
 *     in em units with y down, relative to the glyph's origin, so they
 *     scale by `em_size` straight into layout space.
 *
 * @par Not built yet
 * (APTEXT_TODO.md) Variable font axes, a caller-set script per run, tab
 * stops, hit-testing and caret calls, ellipsis truncation, LFF and SHX
 * single-stroke fonts, an atlas that evicts. Each is a new call or a struct
 * chained onto a desc; none changes what today's calls produce.
 *
 * @par Threads
 * apricore.h's rule ("Threads"): a layout and an atlas are each used by one
 * caller at a time. What is particular here: a face and a chain never
 * change after they are created, so the calls that only read them may
 * overlap each other, from any number of threads - building layouts (each
 * on its own layout), generating glyphs with aptext_generate_glyph(),
 * inserting into atlases (each its own) and the face and chain getters.
 * None of them may overlap the destroy of a face or chain they read. Each
 * call that may is marked. aptext_generate_glyph() is the expensive call
 * and takes no layout or atlas, so a caller can spread generation over its
 * own threads and insert the results one at a time. ApText starts no thread
 * and never calls back.
 *
 * @par Results
 * Calls return APRESULT (apricore.h): APRESULT_OK, a general code, or an
 * APRESULT_APTEXT_* one. The general codes mean, here:
 *   - APRESULT_INVALID_ARGUMENT: also a desc field left at a value its
 *     comment refuses, a stroke face where a call needs an outline face,
 *     and a face or chain of the wrong kind.
 *   - APRESULT_NOT_FOUND: no such glyph, codepoint, page or collection
 *     index.
 *   - APRESULT_LIMIT: an atlas over its `max_pages`, or a glyph larger
 *     than one page.
 *   - APRESULT_CORRUPT_DATA: saved atlas bytes that fail their checksum.
 *   - APRESULT_UNSUPPORTED: saved atlas bytes in a format version this
 *     build doesn't read.
 *   - APRESULT_NOT_IMPLEMENTED: the call isn't built yet. Every call
 *     returns it today, after checking its arguments (APTEXT_TODO.md).
 *
 * @par Error messages
 * Every call that can fail for a reason worth a sentence takes an
 * APTEXT_ERROR pointer (NULL to skip it), as aparchive.h's do: on failure
 * it holds a description naming the argument or the part that failed; on
 * success its message is empty. Being per call, not on an object, it lets
 * the calls that read one face overlap (see "Threads").
 *
 * @par Struct bases
 * Every desc and every struct ApText fills in for one call starts with a
 * `base` (apricore.h, "Struct bases"): the caller sets `base.type` to the
 * struct's own APRI_STRUCT_TYPE_APTEXT_* and `base.next` to NULL, on the
 * output structs too. A wrong type is APRESULT_STRUCT_WRONG_TYPE and a
 * chained struct APRESULT_STRUCT_UNKNOWN_IN_CHAIN: nothing chains onto any
 * of them yet. The array elements ApText reads or hands out (APTEXT_GLYPH,
 * APTEXT_LINE, APTEXT_RUN and the others marked frozen) and APTEXT_ERROR
 * have none.
 *
 * @par Defaults
 * Nothing in a desc has a default. A size, a range or a limit left at 0 is
 * refused unless its comment says what 0 means; which OpenType features
 * apply is the caller's to say, kerning and ligatures included.
 */

/* ---- Conventions ----------------------------------------------------- */

/** @brief The size of APTEXT_ERROR's `message`, its NUL included. */
#define APTEXT_ERROR_MESSAGE_SIZE 256

/**
 * @brief Why a call failed, filled per call (aptext.h, "Error messages").
 */
/* Frozen: filled per call. It never changes; a new need gets a new type. */
typedef struct APTEXT_ERROR {
	char message[APTEXT_ERROR_MESSAGE_SIZE]; /**< NUL-terminated, truncated to fit; empty on success. */
} APTEXT_ERROR;

/**
 * @brief Makes an OpenType tag (a feature or a script) from its four
 * characters, as OpenType stores it: the first character in the high byte.
 *
 * @param a The first character.
 * @param b The second character.
 * @param c The third character.
 * @param d The fourth character.
 */
#define APTEXT_TAG(a, b, c, d)                                                                                         \
	((((uint32_t)(uint8_t)(a)) << 24U) | (((uint32_t)(uint8_t)(b)) << 16U) | (((uint32_t)(uint8_t)(c)) << 8U) |        \
	 ((uint32_t)(uint8_t)(d)))

/* ---- Faces ----------------------------------------------------------- */

/**
 * @brief Opaque handle to a face: one font, of either kind
 * (APTEXT_FACE_KIND).
 *
 * A face never changes after it is created.
 *
 * Thread: the calls that only read a face (marked on each) may overlap each
 * other, from any number of threads; not its destroy.
 */
typedef struct aptext_face_t *aptext_face;

/** @brief What a face's glyphs are. */
typedef enum APTEXT_FACE_KIND APRI_ENUM_BASE {
	/** Filled outlines (OpenType or TrueType), drawn from an atlas's
	 * distance fields. */
	APTEXT_FACE_KIND_OUTLINE = 1,
	/** Open polylines, drawn as lines; no distance field. */
	APTEXT_FACE_KIND_STROKE = 2,
} APTEXT_FACE_KIND;

/**
 * @brief Configuration descriptor for an outline face.
 *
 * The bytes are borrowed by default, as HarfBuzz's and FreeType's memory
 * faces borrow: a memory-mapped or embedded font, which can be tens of
 * megabytes, isn't held twice. `copy_data` has ApText copy them instead, for
 * bytes from a network or a temporary buffer.
 */
typedef struct APTEXT_FACE_DESC {
	/** This struct's type, APRI_STRUCT_TYPE_APTEXT_FACE_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** The font file's bytes: an OpenType (CFF or TrueType outlines) or
	 * TrueType font, or a collection of them (TTC/OTC). Never NULL. Unless
	 * #copy_data is set, they must stay valid and unchanged until the face
	 * is destroyed. */
	const void *data;
	/** The size of #data in bytes. Neither 0 nor over 4 GiB, which a font's
	 * 32-bit offsets can't address. */
	uint64_t data_size;
	/** Which font of a collection; 0 for a file holding one font. */
	uint32_t collection_index;
	/** Copy #data into the face, so the caller's bytes need only last the
	 * call. */
	bool copy_data;
} APTEXT_FACE_DESC;

/**
 * @brief Creates an outline face from font bytes.
 *
 * Reads only the font's header and the tables it needs to answer for
 * metrics and its character map; glyph outlines are read when first used.
 *
 * @param[in]  desc      Face configuration. Only read during the call.
 * @param[out] out_face  The new face; NULL on failure.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT for a NULL argument, a
 *         NULL `data`, or a `data_size` of 0 or over 4 GiB; a struct-base
 *         code for a wrong `base`; APRESULT_APTEXT_NOT_A_FONT if the bytes
 *         aren't a font ApText reads; APRESULT_NOT_FOUND if
 *         `collection_index` is past the last font in the file (any index
 *         but 0 for a file holding one font); or APRESULT_OUT_OF_MEMORY.
 *
 * @see aptext_face_destroy()
 */
APRESULT aptext_face_create(const APTEXT_FACE_DESC *desc, aptext_face *out_face, APTEXT_ERROR *out_error);

/**
 * @brief One polyline of a single-stroke glyph: a range of
 * APTEXT_STROKE_FACE_DESC's `points`.
 */
/* Frozen: an element of a caller's array, and of the arrays a face hands out. It never changes; a new need gets a new type. */
typedef struct APTEXT_STROKE_POLYLINE {
	uint32_t first_point; /**< The index of its first point. */
	uint32_t point_count; /**< How many points it has: at least 2. */
} APTEXT_STROKE_POLYLINE;

/**
 * @brief One glyph of a single-stroke face, as the caller gives it: a
 * codepoint, an advance and a range of APTEXT_STROKE_FACE_DESC's
 * `polylines`.
 */
/* Frozen: an element of a caller's array. It never changes; a new need gets a new type. */
typedef struct APTEXT_STROKE_GLYPH {
	uint32_t codepoint;      /**< The Unicode codepoint it draws; no two glyphs share one. Not a surrogate, not over U+10FFFF. */
	float advance;           /**< How far the pen moves after it, in em units. Finite and not negative. */
	uint32_t first_polyline; /**< The index of its first polyline. */
	uint32_t polyline_count; /**< How many polylines it has; 0 for a glyph that draws nothing, such as a space. */
} APTEXT_STROKE_GLYPH;

/**
 * @brief A single-stroke face's vertical metrics, in em units, font space
 * (y up).
 */
/* Frozen: held by value in the stroke face descs. It never changes; a new need gets a new type. */
typedef struct APTEXT_STROKE_METRICS {
	float ascender;  /**< The top of the line above the baseline: finite, above #descender. */
	float descender; /**< The bottom of the line, negative below the baseline: finite. */
	float line_gap;  /**< Extra space between lines: finite and not negative. */
} APTEXT_STROKE_METRICS;

/**
 * @brief Configuration descriptor for a single-stroke face made from the
 * caller's polylines.
 *
 * Every array is copied, so none need outlive the call. Where the strokes
 * came from is the caller's.
 */
typedef struct APTEXT_STROKE_FACE_DESC {
	/** This struct's type, APRI_STRUCT_TYPE_APTEXT_STROKE_FACE_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** The glyphs, in any order. Never NULL. Glyph id i of the face is
	 * #glyphs [i - 1]: id 0 is the face's `.notdef`, which draws nothing and
	 * advances by half an em. */
	const APTEXT_STROKE_GLYPH *glyphs;
	/** How many #glyphs there are: not 0. */
	uint32_t glyph_count;
	/** Every glyph's polylines. NULL only when #polyline_count is 0. */
	const APTEXT_STROKE_POLYLINE *polylines;
	/** How many #polylines there are. */
	uint32_t polyline_count;
	/** Every polyline's points, in em units, font space (y up, the origin
	 * at the glyph's pen position on the baseline). Finite. NULL only when
	 * #point_count is 0. */
	const ApriVec2 *points;
	/** How many #points there are. */
	uint32_t point_count;
	/** The face's vertical metrics. */
	APTEXT_STROKE_METRICS metrics;
} APTEXT_STROKE_FACE_DESC;

/**
 * @brief Creates a single-stroke face from the caller's polylines.
 *
 * @param[in]  desc      Face configuration. Only read during the call.
 * @param[out] out_face  The new face; NULL on failure.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT for a NULL argument, an
 *         array that is NULL with a count above 0, a glyph or polyline
 *         whose range runs past its array, a polyline of fewer than 2
 *         points, a non-finite or negative value where the field refuses
 *         one, an invalid codepoint, or two glyphs with one codepoint; a
 *         struct-base code for a wrong `base`; or APRESULT_OUT_OF_MEMORY.
 *
 * @see aptext_face_destroy()
 */
APRESULT aptext_face_create_stroke(const APTEXT_STROKE_FACE_DESC *desc, aptext_face *out_face,
                                   APTEXT_ERROR *out_error);

/**
 * @brief Configuration descriptor for a single-stroke face read from a
 * Hershey font.
 *
 * The Hershey fonts (1967) are in the public domain. The format read is
 * the common ".jhf" distribution: one glyph per line (a line may be
 * continued over several, and a line feed or CR LF ends it); columns 1-5
 * the glyph's number, ignored; columns 6-8 its vertex count, the bounds pair
 * included; then pairs of characters, each a coordinate as its character
 * code minus 'R' (82), y down. The first pair is the glyph's left and right
 * bounds; " R" lifts the pen. Glyph i of the file (from 0) draws
 * `codepoints[i]`, or `first_codepoint + i` when `codepoints` is NULL.
 *
 * The format has no scale and no baseline, so the caller gives both. Its
 * units become em units by dividing by `units_per_em`; a glyph's pen origin
 * is its left bound on the baseline, and it advances by its right bound
 * minus its left.
 */
typedef struct APTEXT_HERSHEY_FACE_DESC {
	/** This struct's type, APRI_STRUCT_TYPE_APTEXT_HERSHEY_FACE_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** The file's bytes, ASCII. Never NULL. Only read during the call. */
	const void *data;
	/** The size of #data in bytes; not 0. */
	uint64_t data_size;
	/** The codepoint each glyph of the file draws, in file order, or NULL
	 * for consecutive codepoints from #first_codepoint. */
	const uint32_t *codepoints;
	/** How many #codepoints there are: the file's glyph count exactly, or
	 * 0 when #codepoints is NULL. */
	uint32_t codepoint_count;
	/** The codepoint of the file's first glyph when #codepoints is NULL
	 * (32 for the usual files, which start at the space); ignored
	 * otherwise. */
	uint32_t first_codepoint;
	/** How many of the file's units make an em: finite and above 0. */
	float units_per_em;
	/** The baseline's y in the file's coordinates (y down; 9 for the usual
	 * Roman fonts, whose capitals run from -12 to 9). Finite. */
	float baseline;
	/** The face's vertical metrics, in em units. */
	APTEXT_STROKE_METRICS metrics;
} APTEXT_HERSHEY_FACE_DESC;

/**
 * @brief Creates a single-stroke face from a Hershey font.
 *
 * @param[in]  desc      Face configuration. Only read during the call.
 * @param[out] out_face  The new face; NULL on failure.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT for a NULL argument, a
 *         NULL `data`, a `data_size` of 0, a `codepoint_count` that doesn't
 *         match the file or #APTEXT_HERSHEY_FACE_DESC::codepoints, a value a
 *         field refuses, or two glyphs with one codepoint; a struct-base
 *         code for a wrong `base`; APRESULT_APTEXT_NOT_A_FONT if a line isn't
 *         a Hershey glyph; or APRESULT_OUT_OF_MEMORY.
 *
 * @see aptext_face_destroy()
 */
APRESULT aptext_face_create_hershey(const APTEXT_HERSHEY_FACE_DESC *desc, aptext_face *out_face,
                                    APTEXT_ERROR *out_error);

/**
 * @brief What a face says about itself. Metrics are in em units, font
 * space (y up).
 */
typedef struct APTEXT_FACE_METRICS {
	/** This struct's type, APRI_STRUCT_TYPE_APTEXT_FACE_METRICS, and its chain (apricore.h, "Struct bases"). The caller sets it. */
	APRI_STRUCT_BASE_OUT base;
	/** Outline or stroke. */
	APTEXT_FACE_KIND kind;
	/** The font's design units per em; 0 for a stroke face. */
	uint32_t units_per_em;
	/** How many glyphs it has, `.notdef` included. */
	uint32_t glyph_count;
	/** The top of the line above the baseline. For an outline face, the
	 * OS/2 table's typographic ascender where the font sets
	 * USE_TYPO_METRICS, else the hhea table's (as HarfBuzz reads them). */
	float ascender;
	/** The bottom of the line, negative below the baseline; from the same
	 * table as #ascender. */
	float descender;
	/** Extra space between lines; from the same table as #ascender. */
	float line_gap;
	/** The underline's position, negative below the baseline, as the
	 * font gives it (an outline face's post table); 0 when the face
	 * doesn't say. */
	float underline_position;
	/** The underline's thickness; 0 when the face doesn't say. */
	float underline_thickness;
	/** The strikeout's position above the baseline, as the font gives it
	 * (an outline face's OS/2 table); 0 when the face doesn't say. */
	float strikeout_position;
	/** The strikeout's thickness; 0 when the face doesn't say. */
	float strikeout_thickness;
	/** The height of flat capitals; 0 when the face doesn't say. */
	float cap_height;
	/** The height of flat lowercase letters; 0 when the face doesn't say. */
	float x_height;
} APTEXT_FACE_METRICS;

/**
 * @brief Reads a face's metrics.
 *
 * Fills @p out_metrics, whose base the caller sets, and leaves its base as
 * it was.
 *
 * Thread: only reads the face; may overlap any other call that only reads
 * it, from any thread.
 *
 * @param[in]     face        The face.
 * @param[in,out] out_metrics The face's metrics.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT for a NULL argument; or a
 *         struct-base code for a wrong `base`.
 */
APRESULT aptext_face_get_metrics(aptext_face face, APTEXT_FACE_METRICS *out_metrics);

/**
 * @brief Finds the glyph a face maps a codepoint to, through its character
 * map alone (no shaping).
 *
 * Thread: only reads the face; may overlap any other call that only reads
 * it, from any thread.
 *
 * @param[in]  face      The face.
 * @param[in]  codepoint A Unicode codepoint.
 * @param[out] out_glyph The glyph id; 0 when there is none.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT for a NULL argument; or
 *         APRESULT_NOT_FOUND if the face has no glyph for @p codepoint.
 */
APRESULT aptext_face_find_glyph(aptext_face face, uint32_t codepoint, uint32_t *out_glyph);

/**
 * @brief A stroke face's polylines for one glyph, in em units, relative to
 * the glyph's origin, with y down (layout space's direction, unscaled).
 *
 * The arrays belong to the face and stay valid until it is destroyed.
 * Each polyline indexes @p out_points, as in APTEXT_STROKE_FACE_DESC.
 *
 * Thread: only reads the face; may overlap any other call that only reads
 * it, from any thread.
 *
 * @param[in]  face               A stroke face.
 * @param[in]  glyph              A glyph id of the face.
 * @param[out] out_points         The glyph's points; NULL when it has none.
 * @param[out] out_point_count    How many points.
 * @param[out] out_polylines      The glyph's polylines; NULL when it has
 *                                none.
 * @param[out] out_polyline_count How many polylines.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT for a NULL argument or an
 *         outline face; or APRESULT_NOT_FOUND if @p glyph is past the face's
 *         last glyph.
 */
APRESULT aptext_face_get_glyph_strokes(aptext_face face, uint32_t glyph, const ApriVec2 **out_points,
                                       uint32_t *out_point_count, const APTEXT_STROKE_POLYLINE **out_polylines,
                                       uint32_t *out_polyline_count);

/**
 * @brief Destroys a face.
 *
 * @warning Destroy every chain holding the face first, and every atlas
 * holding a glyph of it.
 *
 * @param[in] face The face. NULL is accepted and does nothing.
 */
void aptext_face_destroy(aptext_face face);

/* ---- Fallback chains ------------------------------------------------- */

/**
 * @brief Opaque handle to a fallback chain: an ordered list of faces of one
 * kind.
 *
 * For each codepoint the first face with a glyph for it wins; a codepoint
 * no face has becomes the first face's `.notdef` (glyph 0), never a
 * skipped glyph. A chain never changes after it is created.
 *
 * Thread: the calls that only read a chain (marked on each) may overlap
 * each other, from any number of threads; not its destroy.
 */
typedef struct aptext_chain_t *aptext_chain;

/** @brief Configuration descriptor for a fallback chain. */
typedef struct APTEXT_CHAIN_DESC {
	/** This struct's type, APRI_STRUCT_TYPE_APTEXT_CHAIN_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** The faces, first choice first. Never NULL, no NULL entry, no face
	 * twice, all of one APTEXT_FACE_KIND. The array is copied; the faces
	 * must outlive the chain. */
	const aptext_face *faces;
	/** How many #faces there are: not 0. */
	uint32_t face_count;
} APTEXT_CHAIN_DESC;

/**
 * @brief Creates a fallback chain.
 *
 * @param[in]  desc      Chain configuration. Only read during the call.
 * @param[out] out_chain The new chain; NULL on failure.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT for a NULL argument, a
 *         NULL `faces` or entry in it, a `face_count` of 0, a face twice,
 *         or faces of more than one kind; a struct-base code for a wrong
 *         `base`; or APRESULT_OUT_OF_MEMORY.
 *
 * Thread: only reads the faces; may overlap any other call that only reads
 * them, from any thread.
 *
 * @see aptext_chain_destroy()
 */
APRESULT aptext_chain_create(const APTEXT_CHAIN_DESC *desc, aptext_chain *out_chain, APTEXT_ERROR *out_error);

/**
 * @brief One of a chain's faces.
 *
 * Thread: only reads the chain; may overlap any other call that only reads
 * it, from any thread.
 *
 * @param[in] chain The chain.
 * @param[in] index The face's place in the chain, from 0.
 *
 * @return The face, or NULL if @p chain is NULL or @p index is past its last
 *         face.
 */
aptext_face aptext_chain_get_face(aptext_chain chain, uint32_t index);

/**
 * @brief How many faces a chain has.
 *
 * Thread: only reads the chain; may overlap any other call that only reads
 * it, from any thread.
 *
 * @param[in] chain The chain.
 *
 * @return The count, or 0 if @p chain is NULL.
 */
uint32_t aptext_chain_get_face_count(aptext_chain chain);

/**
 * @brief Which face of a chain draws a codepoint, and with which glyph,
 * through each face's character map alone (no shaping).
 *
 * Thread: only reads the chain and its faces; may overlap any other call
 * that only reads them, from any thread.
 *
 * @param[in]  chain          The chain.
 * @param[in]  codepoint      A Unicode codepoint.
 * @param[out] out_face_index The first face with a glyph for it; 0 when
 *                            none has one.
 * @param[out] out_glyph      Its glyph id; 0 (the first face's `.notdef`)
 *                            when no face has one.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT for a NULL argument; or
 *         APRESULT_NOT_FOUND if no face has a glyph for @p codepoint, with
 *         the outputs set to the `.notdef` that stands in for it.
 */
APRESULT aptext_chain_find_glyph(aptext_chain chain, uint32_t codepoint, uint32_t *out_face_index,
                                 uint32_t *out_glyph);

/**
 * @brief Destroys a chain. Its faces are the caller's and stay.
 *
 * @warning Build no layout with the chain, and destroy or rebuild every
 * layout built with it, first.
 *
 * @param[in] chain The chain. NULL is accepted and does nothing.
 */
void aptext_chain_destroy(aptext_chain chain);

/* ---- Layout ---------------------------------------------------------- */

/** @brief A direction of text. */
typedef enum APTEXT_DIRECTION APRI_ENUM_BASE {
	/** Left to right. */
	APTEXT_DIRECTION_LTR = 1,
	/** Right to left. */
	APTEXT_DIRECTION_RTL = 2,
	/** A paragraph's direction from its first strongly directional
	 * character (Unicode's bidirectional algorithm, rules P2 and P3), left
	 * to right when it has none. Only for APTEXT_LAYOUT_DESC's `direction`. */
	APTEXT_DIRECTION_AUTO = 3,
} APTEXT_DIRECTION;

/**
 * @brief Where a line sits across the layout's width: `max_width`, or the
 * widest line's width when `max_width` is 0.
 */
typedef enum APTEXT_ALIGN APRI_ENUM_BASE {
	/** Against the left edge. */
	APTEXT_ALIGN_LEFT = 1,
	/** Centred. */
	APTEXT_ALIGN_CENTER = 2,
	/** Against the right edge. */
	APTEXT_ALIGN_RIGHT = 3,
	/** Against the edge the paragraph starts from: left for a left-to-right
	 * paragraph, right for a right-to-left one. */
	APTEXT_ALIGN_START = 4,
	/** Against the edge the paragraph ends at. */
	APTEXT_ALIGN_END = 5,
} APTEXT_ALIGN;

/**
 * @brief The OpenType features most text switches, as bits of
 * APTEXT_LAYOUT_DESC's `features`.
 *
 * A bit set turns its features on for the whole text, clear turns them
 * off. The features a script needs to be correct at all (`ccmp`, `locl`,
 * mark positioning, and the script's own shaping features, Arabic's joining
 * forms among them) are always on and aren't listed here. Any other feature
 * goes in APTEXT_LAYOUT_DESC's `extra_features`.
 */
typedef enum APTEXT_FEATURE_FLAGS APRI_ENUM_BASE {
	/** Kerning: `kern`, and the font's legacy kern table where it has no
	 * GPOS. */
	APTEXT_FEATURE_KERNING = 0x1,
	/** Standard and contextual ligatures: `liga` and `clig`. */
	APTEXT_FEATURE_LIGATURES = 0x2,
	/** Contextual alternates: `calt`. */
	APTEXT_FEATURE_CONTEXTUAL_ALTERNATES = 0x4,
} APTEXT_FEATURE_FLAGS;

/** @brief One more OpenType feature for a layout, over its whole text. */
/* Frozen: an element of a caller's array. It never changes; a new need gets a new type. */
typedef struct APTEXT_FEATURE {
	uint32_t tag;   /**< The feature's tag (APTEXT_TAG()); not one APTEXT_FEATURE_FLAGS controls, and not twice in one layout. */
	uint32_t value; /**< 0 off, 1 on, or for an alternates feature the alternate to use, from 1. */
} APTEXT_FEATURE;

/**
 * @brief Opaque handle to a layout: text shaped and broken into lines.
 *
 * Made empty and filled by aptext_layout_build(), which can be called again
 * to lay out other text into the same memory.
 */
typedef struct aptext_layout_t *aptext_layout;

/**
 * @brief What to lay out, and how.
 *
 * How lines are measured, in output units, with `asc`, `desc` and `gap`
 * the first face of `chain`'s ascender, descender and line gap
 * (APTEXT_FACE_METRICS), the same for every line:
 *   - a line's natural height is (asc - desc + gap) * `em_size`;
 *   - its advance, from one line's top to the next's, is the natural height
 *     times `line_spacing`;
 *   - line i's top is i times the advance, and its baseline is its top plus
 *     (advance - (asc - desc) * `em_size`) / 2 plus asc * `em_size`, so that
 *     the space beyond the ascender and descender is shared equally above
 *     and below.
 */
typedef struct APTEXT_LAYOUT_DESC {
	/** This struct's type, APRI_STRUCT_TYPE_APTEXT_LAYOUT_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** The faces to lay out with. Never NULL. The layout keeps it: it must
	 * outlive the layout, or the layout's next build. */
	aptext_chain chain;
	/** The text, UTF-8, need not be NUL-terminated; only read during the
	 * call. An ill-formed sequence is laid out as U+FFFD, never refused:
	 * one U+FFFD for each maximal subpart, Unicode's recommended practice
	 * (so a sequence cut short is one). NULL only when #text_size is 0. */
	const char *text;
	/** The size of #text in bytes. 0 gives one empty line. */
	uint32_t text_size;
	/** A BCP 47 language tag ("en", "sr-Latn"), NUL-terminated, for the
	 * forms that differ between languages; NULL for none. Only read during
	 * the call. */
	const char *language;
	/** The paragraphs' base direction. Each paragraph is reordered by
	 * Unicode's bidirectional algorithm within it. */
	APTEXT_DIRECTION direction;
	/** APTEXT_FEATURE_FLAGS bits; no others. 0 turns them all off. */
	uint32_t features;
	/** More features, or NULL for none. Only read during the call. */
	const APTEXT_FEATURE *extra_features;
	/** How many #extra_features there are. */
	uint32_t extra_feature_count;
	/** Output units per em: 1 for em units, a pixel size for pixels. Finite
	 * and above 0. */
	float em_size;
	/** The widest a line may be, in output units, beyond which it wraps at
	 * a line break opportunity (Unicode's line breaking algorithm); a word
	 * wider than this alone wraps between grapheme clusters. 0 for no limit:
	 * lines end only at mandatory breaks. Finite and not negative. */
	float max_width;
	/** Where each line sits across the width. */
	APTEXT_ALIGN align;
	/** The line advance as a multiple of the natural height (see above): 1
	 * for the font's own spacing. Finite and above 0. */
	float line_spacing;
	/** Space added after every grapheme cluster but the last on its line,
	 * in output units; 0 for none. Finite. */
	float letter_spacing;
} APTEXT_LAYOUT_DESC;

/**
 * @brief One positioned glyph of a layout, in layout space.
 *
 * A layout's glyphs are in visual order: line by line, and within a line
 * from left to right, whatever the text's direction.
 */
/* Frozen: an element of the layout's glyph array. It never changes; a new need gets a new type. */
typedef struct APTEXT_GLYPH {
	uint32_t glyph;       /**< The glyph id in its face. */
	uint32_t face_index;  /**< Its face's place in the layout's chain. */
	float x;              /**< Its origin's x: the pen position on the baseline, offsets from shaping included. */
	float y;              /**< Its origin's y: the line's baseline, plus any vertical offset from shaping. */
	float advance;        /**< How far it moves the pen, letter spacing and kerning included; never negative. */
	uint32_t text_offset; /**< The first byte of the cluster it came from in the desc's `text`. */
	uint32_t text_size;   /**< The cluster's size in bytes; glyphs of one cluster share the range (a ligature covers several characters, a decomposed character gives several glyphs). */
	uint32_t line;        /**< The line it is on, from 0. */
	uint32_t run;         /**< The run it belongs to, from 0. */
	float box_left;       /**< Its box's left edge, for selection: the box spans its advance across and its line's box down. */
	float box_top;        /**< Its box's top edge: its line's top. */
	float box_right;      /**< Its box's right edge: #box_left plus #advance. */
	float box_bottom;     /**< Its box's bottom edge: its line's bottom. */
} APTEXT_GLYPH;

/**
 * @brief One line of a layout, in layout space.
 *
 * Lines are in order from the top. Each mandatory break (line feed, CR,
 * CR LF, NEL, U+2028, U+2029) ends a line and gives no glyph; text that
 * ends with one has a last, empty line after it.
 */
/* Frozen: an element of the layout's line array. It never changes; a new need gets a new type. */
typedef struct APTEXT_LINE {
	uint32_t first_glyph; /**< Its first glyph in the layout's glyph array. */
	uint32_t glyph_count; /**< How many glyphs it has, trailing spaces included. */
	uint32_t first_run;   /**< Its first run in the layout's run array. */
	uint32_t run_count;   /**< How many runs it has. */
	uint32_t text_offset; /**< The first byte of its text, in logical order. */
	uint32_t text_size;   /**< Its text's size in bytes, trailing spaces and its break included. */
	float baseline;       /**< Its baseline's y (APTEXT_LAYOUT_DESC has the formula). */
	float box_left;       /**< Its box's left edge: its first glyph's, after alignment. */
	float box_top;        /**< Its box's top edge: the line's top. */
	float box_right;      /**< Its box's right edge: its last glyph's, trailing spaces left out. */
	float box_bottom;     /**< Its box's bottom edge: #box_top plus the line advance. */
} APTEXT_LINE;

/**
 * @brief One run of a layout: glyphs on one line from one face, in one
 * script and one direction, shaped together.
 *
 * Runs are in visual order, as glyphs are.
 */
/* Frozen: an element of the layout's run array. It never changes; a new need gets a new type. */
typedef struct APTEXT_RUN {
	uint32_t first_glyph;       /**< Its first glyph in the layout's glyph array. */
	uint32_t glyph_count;       /**< How many glyphs it has. */
	uint32_t text_offset;       /**< The first byte of its text. */
	uint32_t text_size;         /**< Its text's size in bytes. */
	uint32_t face_index;        /**< Its face's place in the layout's chain. */
	uint32_t script;            /**< Its ISO 15924 script as a tag (APTEXT_TAG('L','a','t','n')); 'Zyyy' where it has only common characters. */
	uint32_t bidi_level;        /**< Its embedding level from the bidirectional algorithm: even left to right, odd right to left. */
	APTEXT_DIRECTION direction; /**< APTEXT_DIRECTION_LTR or APTEXT_DIRECTION_RTL, from #bidi_level. */
	uint32_t line;              /**< The line it is on. */
} APTEXT_RUN;

/** @brief The sizes and counts of a built layout. */
typedef struct APTEXT_LAYOUT_INFO {
	/** This struct's type, APRI_STRUCT_TYPE_APTEXT_LAYOUT_INFO, and its chain (apricore.h, "Struct bases"). The caller sets it. */
	APRI_STRUCT_BASE_OUT base;
	/** How many glyphs. */
	uint32_t glyph_count;
	/** How many lines; at least 1 once built. */
	uint32_t line_count;
	/** How many runs. */
	uint32_t run_count;
	/** How many codepoints no face of the chain had, each laid out as the
	 * first face's `.notdef`. Default-ignorable codepoints (zero-width
	 * joiners, variation selectors and the like) a face lacks give no glyph
	 * and aren't counted. */
	uint32_t missing_codepoint_count;
	/** How many invalid UTF-8 sequences were laid out as U+FFFD. */
	uint32_t replaced_sequence_count;
	/** The widest line's box width, trailing spaces left out. */
	float width;
	/** The line advance times the line count. */
	float height;
} APTEXT_LAYOUT_INFO;

/**
 * @brief Creates an empty layout.
 *
 * @param[out] out_layout The new layout; NULL on failure.
 *
 * @return APRESULT_OK, APRESULT_INVALID_ARGUMENT if @p out_layout is NULL,
 *         or APRESULT_OUT_OF_MEMORY.
 *
 * @see aptext_layout_destroy()
 */
APRESULT aptext_layout_create(aptext_layout *out_layout);

/**
 * @brief Lays text out, replacing what the layout held.
 *
 * Decodes the text, splits it into paragraphs at mandatory breaks, finds
 * each paragraph's bidirectional runs, splits those by script and by the
 * face of the chain that has each character's glyph, shapes each run with
 * the desc's features, breaks lines, reorders each line's runs visually
 * and aligns it. U+0009 (tab) is laid out as a space in this version.
 *
 * The output is deterministic: the same desc, text and faces give the same
 * glyphs, lines and runs, bit for bit, on every platform.
 *
 * On failure the layout is left empty (no glyph, no line), and usable.
 *
 * Thread: reads the desc's chain and its faces only; may overlap any other
 * call that only reads them (another layout's build among them), from any
 * thread. The layout itself is one caller's at a time.
 *
 * @param[in]  layout    The layout.
 * @param[in]  desc      What to lay out. Only read during the call, but
 *                       for its `chain`.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT for a NULL argument, a
 *         NULL `chain`, a NULL `text` with a `text_size` above 0, a value a
 *         field refuses, an unknown `features` bit, or an extra feature
 *         that is a flag's or is listed twice; a struct-base code for a
 *         wrong `base`; or APRESULT_OUT_OF_MEMORY.
 */
APRESULT aptext_layout_build(aptext_layout layout, const APTEXT_LAYOUT_DESC *desc, APTEXT_ERROR *out_error);

/**
 * @brief Reads a layout's sizes and counts.
 *
 * Fills @p out_info, whose base the caller sets, and leaves its base as it
 * was. An empty layout (never built, or after a failed build) has every
 * count and size 0.
 *
 * @param[in]     layout   The layout.
 * @param[in,out] out_info Its sizes and counts.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT for a NULL argument; or a
 *         struct-base code for a wrong `base`.
 */
APRESULT aptext_layout_get_info(aptext_layout layout, APTEXT_LAYOUT_INFO *out_info);

/**
 * @brief A layout's glyphs, in visual order.
 *
 * The array belongs to the layout and stays valid until it is built again
 * or destroyed.
 *
 * @param[in]  layout     The layout.
 * @param[out] out_glyphs The glyphs; NULL when there are none.
 * @param[out] out_count  How many.
 *
 * @return APRESULT_OK, or APRESULT_INVALID_ARGUMENT for a NULL argument.
 */
APRESULT aptext_layout_get_glyphs(aptext_layout layout, const APTEXT_GLYPH **out_glyphs, uint32_t *out_count);

/**
 * @brief A layout's lines, from the top.
 *
 * The array belongs to the layout and stays valid until it is built again
 * or destroyed.
 *
 * @param[in]  layout    The layout.
 * @param[out] out_lines The lines; NULL when there are none.
 * @param[out] out_count How many.
 *
 * @return APRESULT_OK, or APRESULT_INVALID_ARGUMENT for a NULL argument.
 */
APRESULT aptext_layout_get_lines(aptext_layout layout, const APTEXT_LINE **out_lines, uint32_t *out_count);

/**
 * @brief A layout's runs, in visual order.
 *
 * The array belongs to the layout and stays valid until it is built again
 * or destroyed.
 *
 * @param[in]  layout    The layout.
 * @param[out] out_runs  The runs; NULL when there are none.
 * @param[out] out_count How many.
 *
 * @return APRESULT_OK, or APRESULT_INVALID_ARGUMENT for a NULL argument.
 */
APRESULT aptext_layout_get_runs(aptext_layout layout, const APTEXT_RUN **out_runs, uint32_t *out_count);

/** @brief One straight piece of a laid-out stroke glyph, in layout space. */
/* Frozen: an element of a caller's array that aptext_layout_get_strokes() fills. It never changes; a new need gets a new type. */
typedef struct APTEXT_SEGMENT {
	float x0;             /**< Its start's x. */
	float y0;             /**< Its start's y. */
	float x1;             /**< Its end's x. */
	float y1;             /**< Its end's y. */
	uint32_t glyph_index; /**< The glyph it belongs to, in the layout's glyph array. */
} APTEXT_SEGMENT;

/**
 * @brief A stroke layout's glyphs as line segments, placed and scaled into
 * layout space: each polyline of n points gives n - 1 segments, in glyph
 * order, then polyline order.
 *
 * With @p segments NULL and @p capacity 0, only counts them.
 *
 * Thread: also reads the layout's chain and its faces; may overlap any
 * other call that only reads them, from any thread.
 *
 * @param[in]  layout    A layout built with a chain of stroke faces.
 * @param[out] segments  Receives the segments. May be NULL when
 *                       @p capacity is 0.
 * @param[in]  capacity  How many @p segments can hold.
 * @param[out] out_count How many segments the layout has.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT for a NULL @p layout or
 *         @p out_count, a NULL @p segments with a @p capacity above 0, or a
 *         layout of outline faces; or APRESULT_LIMIT if @p capacity is
 *         above 0 and below the count, with nothing written.
 */
APRESULT aptext_layout_get_strokes(aptext_layout layout, APTEXT_SEGMENT *segments, uint32_t capacity,
                                   uint32_t *out_count);

/**
 * @brief Destroys a layout.
 *
 * @param[in] layout The layout. NULL is accepted and does nothing.
 */
void aptext_layout_destroy(aptext_layout layout);

/* ---- Glyph generation ------------------------------------------------ */

/**
 * @brief Which glyph to generate, and at what scale: the settings an atlas
 * must have to take it.
 *
 * A glyph's image is its multi-channel signed distance field with a true
 * signed distance in alpha (MTSDF), RGBA8, 4 bytes per pixel, rows from the
 * top, each `width` * 4 bytes. With d a pixel's signed distance to the
 * outline in pixels, positive inside, alpha is round(255 * clamp(0.5 + d /
 * `distance_range`, 0, 1)); the median of red, green and blue is the same
 * near the outline and keeps its corners sharp. Each image has a margin of
 * at least `distance_range` / 2 pixels around the outline, so a shader can
 * draw an outline or halo that wide.
 */
typedef struct APTEXT_GLYPH_GENERATE_DESC {
	/** This struct's type, APRI_STRUCT_TYPE_APTEXT_GLYPH_GENERATE_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** An outline face. Never NULL. */
	aptext_face face;
	/** A glyph id of #face. */
	uint32_t glyph;
	/** The em's size in the image, in pixels. Finite and above 0. */
	float pixels_per_em;
	/** The distance in pixels over which alpha goes from 0 to 255 across
	 * the outline (msdfgen's pxRange): 4 to 8 leaves room for outlines and
	 * halos. Finite and above 0. */
	float distance_range;
} APTEXT_GLYPH_GENERATE_DESC;

/**
 * @brief A generated glyph's size, and where it sits.
 *
 * The image covers the glyph's plane bounds: the top-left corner of pixel
 * (0, 0) is at (`plane_left`, `plane_top`) and the bottom-right corner of
 * pixel (width - 1, height - 1) at (`plane_right`, `plane_bottom`), in em
 * units relative to the glyph's origin, y down. A glyph with no outline
 * (a space) has a 0 by 0 image and plane bounds all 0.
 */
typedef struct APTEXT_GLYPH_IMAGE {
	/** This struct's type, APRI_STRUCT_TYPE_APTEXT_GLYPH_IMAGE, and its chain (apricore.h, "Struct bases"). The caller sets it. */
	APRI_STRUCT_BASE_OUT base;
	/** The image's width in pixels. */
	uint32_t width;
	/** The image's height in pixels. */
	uint32_t height;
	/** The image's left edge, in em units from the origin. */
	float plane_left;
	/** The image's top edge, in em units from the baseline, y down (above
	 * the baseline is negative). */
	float plane_top;
	/** The image's right edge. */
	float plane_right;
	/** The image's bottom edge. */
	float plane_bottom;
} APTEXT_GLYPH_IMAGE;

/**
 * @brief Generates one glyph's MTSDF into the caller's memory.
 *
 * This is the expensive part of text: a caller with many glyphs to
 * generate runs this as jobs on its own threads, then gives each result to
 * aptext_atlas_insert() in turn. With @p pixels NULL and @p pixels_size 0,
 * it only fills @p out_image, so the caller knows how much memory to give.
 *
 * The pixels are deterministic: the same face, glyph and settings give the
 * same bytes on every platform.
 *
 * Thread: only reads the face; may overlap any other call that only reads
 * it, from any thread, this one included.
 *
 * @param[in]     desc        The glyph and the scale. Only read during the
 *                            call.
 * @param[out]    pixels      Receives the image. May be NULL when
 *                            @p pixels_size is 0.
 * @param[in]     pixels_size The size of @p pixels in bytes: 0, or at least
 *                            `width` * `height` * 4.
 * @param[in,out] out_image   The image's size and plane bounds; the caller
 *                            sets its base.
 * @param[out]    out_error   Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT for a NULL @p desc or
 *         @p out_image, a NULL `face`, a stroke face, a value a field
 *         refuses, a NULL @p pixels with a @p pixels_size above 0, or a
 *         @p pixels_size above 0 but too small; a struct-base code for a
 *         wrong `base`; APRESULT_NOT_FOUND if `glyph` is past the face's
 *         last glyph; or APRESULT_OUT_OF_MEMORY.
 */
APRESULT aptext_generate_glyph(const APTEXT_GLYPH_GENERATE_DESC *desc, void *pixels, uint64_t pixels_size,
                               APTEXT_GLYPH_IMAGE *out_image, APTEXT_ERROR *out_error);

/* ---- Atlas ----------------------------------------------------------- */

/**
 * @brief Opaque handle to an atlas: generated glyphs packed into pages.
 *
 * Glyphs are added on demand, packed into the first page with room (a
 * skyline packer), and a glyph no page has room for starts a new page.
 * Nothing is evicted. Each page is RGBA8, 4 bytes per pixel, rows from the
 * top, each `page_width` * 4 bytes, as APTEXT_GLYPH_GENERATE_DESC
 * describes; a page no glyph has reached yet is zero. A caller uploads the
 * pages as the layers of one 2D array texture, sampled linearly with no
 * mips, and after each change only the rectangles aptext_atlas_take_dirty()
 * gives.
 *
 * An atlas keys its glyphs by face and glyph id; it holds glyphs of any
 * number of outline faces.
 */
typedef struct aptext_atlas_t *aptext_atlas;

/** @brief Configuration descriptor for an atlas. */
typedef struct APTEXT_ATLAS_DESC {
	/** This struct's type, APRI_STRUCT_TYPE_APTEXT_ATLAS_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** Every glyph's em size in pixels (APTEXT_GLYPH_GENERATE_DESC). Finite
	 * and above 0. */
	float pixels_per_em;
	/** Every glyph's distance range in pixels (APTEXT_GLYPH_GENERATE_DESC),
	 * reported back for the shader. Finite and above 0. */
	float distance_range;
	/** Each page's width in pixels: at least #pixels_per_em plus
	 * #distance_range. */
	uint32_t page_width;
	/** Each page's height in pixels: at least #pixels_per_em plus
	 * #distance_range. */
	uint32_t page_height;
	/** The most pages it may have: not 0; UINT32_MAX for no limit. */
	uint32_t max_pages;
} APTEXT_ATLAS_DESC;

/**
 * @brief Creates an empty atlas: no glyph, no page.
 *
 * @param[in]  desc      Atlas configuration. Only read during the call.
 * @param[out] out_atlas The new atlas; NULL on failure.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT for a NULL argument or a
 *         value a field refuses; a struct-base code for a wrong `base`; or
 *         APRESULT_OUT_OF_MEMORY.
 *
 * @see aptext_atlas_destroy(), aptext_atlas_load()
 */
APRESULT aptext_atlas_create(const APTEXT_ATLAS_DESC *desc, aptext_atlas *out_atlas, APTEXT_ERROR *out_error);

/** @brief An atlas's settings and counts. */
typedef struct APTEXT_ATLAS_INFO {
	/** This struct's type, APRI_STRUCT_TYPE_APTEXT_ATLAS_INFO, and its chain (apricore.h, "Struct bases"). The caller sets it. */
	APRI_STRUCT_BASE_OUT base;
	/** As created. */
	float pixels_per_em;
	/** As created: the value a shader's distance range must be. */
	float distance_range;
	/** As created. */
	uint32_t page_width;
	/** As created. */
	uint32_t page_height;
	/** How many pages it has now. */
	uint32_t page_count;
	/** How many glyphs it holds, those with no outline included. */
	uint32_t glyph_count;
	/** How many rectangles aptext_atlas_take_dirty() has to give. */
	uint32_t dirty_rect_count;
} APTEXT_ATLAS_INFO;

/**
 * @brief Reads an atlas's settings and counts.
 *
 * Fills @p out_info, whose base the caller sets, and leaves its base as it
 * was.
 *
 * @param[in]     atlas    The atlas.
 * @param[in,out] out_info Its settings and counts.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT for a NULL argument; or a
 *         struct-base code for a wrong `base`.
 */
APRESULT aptext_atlas_get_info(aptext_atlas atlas, APTEXT_ATLAS_INFO *out_info);

/**
 * @brief Where an atlas holds one glyph.
 *
 * A glyph with no outline (a space) has a 0 by 0 rectangle on page 0 and
 * plane bounds all 0: there is nothing to draw.
 */
typedef struct APTEXT_ATLAS_GLYPH {
	/** This struct's type, APRI_STRUCT_TYPE_APTEXT_ATLAS_GLYPH, and its chain (apricore.h, "Struct bases"). The caller sets it. */
	APRI_STRUCT_BASE_OUT base;
	/** The page it is on, from 0. */
	uint32_t page;
	/** Its rectangle's left edge in the page, in pixels. */
	uint32_t x;
	/** Its rectangle's top edge in the page, in pixels. */
	uint32_t y;
	/** Its rectangle's width in pixels: its image's (APTEXT_GLYPH_IMAGE). */
	uint32_t width;
	/** Its rectangle's height in pixels. */
	uint32_t height;
	/** Its image's plane bounds, as APTEXT_GLYPH_IMAGE's: the rectangle's
	 * left edge, in em units from the glyph's origin. */
	float plane_left;
	/** The rectangle's top edge, y down. */
	float plane_top;
	/** The rectangle's right edge. */
	float plane_right;
	/** The rectangle's bottom edge. */
	float plane_bottom;
} APTEXT_ATLAS_GLYPH;

/**
 * @brief Packs a generated glyph into an atlas.
 *
 * Copies the image into a page and marks its rectangle dirty. A glyph the
 * atlas already holds is left where it is, and its place returned: two
 * jobs that generated the same glyph both insert it safely.
 *
 * Thread: also reads the desc's face; may overlap any other call that only
 * reads it, from any thread. The atlas itself is one caller's at a time.
 *
 * @param[in]     atlas       The atlas.
 * @param[in]     desc        The desc the pixels were generated with: its
 *                            `pixels_per_em` and `distance_range` must be
 *                            the atlas's.
 * @param[in]     pixels      The image from aptext_generate_glyph(). May be
 *                            NULL for a glyph with no outline.
 * @param[in]     pixels_size The size of @p pixels in bytes: the image's
 *                            `width` * `height` * 4 exactly.
 * @param[in,out] out_glyph   Where the glyph is; the caller sets its base.
 *                            May be NULL.
 * @param[out]    out_error   Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT for a NULL @p atlas or
 *         @p desc, settings other than the atlas's, a stroke face, or a
 *         @p pixels or @p pixels_size that isn't the image's; a struct-base
 *         code for a wrong `base`; APRESULT_NOT_FOUND if `glyph` is past
 *         the face's last glyph; APRESULT_LIMIT if the glyph is larger than
 *         a page, or needs a page past `max_pages`; or
 *         APRESULT_OUT_OF_MEMORY.
 */
APRESULT aptext_atlas_insert(aptext_atlas atlas, const APTEXT_GLYPH_GENERATE_DESC *desc, const void *pixels,
                             uint64_t pixels_size, APTEXT_ATLAS_GLYPH *out_glyph, APTEXT_ERROR *out_error);

/**
 * @brief Generates and inserts every glyph of a layout the atlas doesn't
 * hold yet, one after another, on the caller's thread.
 *
 * The simple way to keep an atlas up to date: call it after each build.
 * Glyphs inserted before a failure stay.
 *
 * Thread: also reads the layout's chain and its faces; may overlap any
 * other call that only reads them, from any thread.
 *
 * @param[in]  atlas     The atlas.
 * @param[in]  layout    A layout built with a chain of outline faces.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT for a NULL argument or a
 *         layout of stroke faces; APRESULT_LIMIT as
 *         aptext_atlas_insert(); or APRESULT_OUT_OF_MEMORY.
 */
APRESULT aptext_atlas_add_layout(aptext_atlas atlas, aptext_layout layout, APTEXT_ERROR *out_error);

/**
 * @brief Finds where an atlas holds a glyph.
 *
 * @param[in]     atlas     The atlas.
 * @param[in]     face      The glyph's face.
 * @param[in]     glyph     The glyph id.
 * @param[in,out] out_glyph Where it is; the caller sets its base.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT for a NULL argument; a
 *         struct-base code for a wrong `base`; or APRESULT_NOT_FOUND if the
 *         atlas doesn't hold it.
 */
APRESULT aptext_atlas_find_glyph(aptext_atlas atlas, aptext_face face, uint32_t glyph,
                                 APTEXT_ATLAS_GLYPH *out_glyph);

/**
 * @brief One page's pixels.
 *
 * The memory belongs to the atlas and stays at the same address until the
 * atlas is destroyed; its contents change as glyphs are inserted.
 *
 * @param[in]  atlas      The atlas.
 * @param[in]  page       The page, from 0.
 * @param[out] out_pixels The page: `page_height` rows of `page_width` * 4
 *                        bytes.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT for a NULL argument; or
 *         APRESULT_NOT_FOUND if @p page is past the last page.
 */
APRESULT aptext_atlas_get_page(aptext_atlas atlas, uint32_t page, const void **out_pixels);

/** @brief A rectangle of a page that changed, in pixels. */
/* Frozen: an element of a caller's array that aptext_atlas_take_dirty() fills. It never changes; a new need gets a new type. */
typedef struct APTEXT_DIRTY_RECT {
	uint32_t page;   /**< The page. */
	uint32_t x;      /**< The left edge. */
	uint32_t y;      /**< The top edge. */
	uint32_t width;  /**< The width; never 0. */
	uint32_t height; /**< The height; never 0. */
} APTEXT_DIRTY_RECT;

/**
 * @brief Takes the rectangles that changed since they were last taken: at
 * most one per page, covering everything that changed on it, in page
 * order.
 *
 * Takes up to @p capacity of them; those not taken stay for the next call.
 * APTEXT_ATLAS_INFO's `dirty_rect_count` says how many there are. A new
 * page's first rectangle covers only what was written to it: the rest of
 * the page is zero.
 *
 * @param[in]  atlas     The atlas.
 * @param[out] rects     Receives the rectangles. May be NULL when
 *                       @p capacity is 0.
 * @param[in]  capacity  How many @p rects can hold.
 * @param[out] out_count How many were taken.
 *
 * @return APRESULT_OK, or APRESULT_INVALID_ARGUMENT for a NULL @p atlas or
 *         @p out_count, or a NULL @p rects with a @p capacity above 0.
 */
APRESULT aptext_atlas_take_dirty(aptext_atlas atlas, APTEXT_DIRTY_RECT *rects, uint32_t capacity,
                                 uint32_t *out_count);

/**
 * @brief Saves an atlas as bytes, for the caller to keep wherever it keeps
 * files.
 *
 * The bytes hold the settings, every glyph's place and plane bounds, every
 * page, a checksum, and for each face a fingerprint (from the font's own
 * header: its revision, checksum adjustment and glyph count; for a stroke
 * face, its data), so that loading them with other faces fails rather than
 * draws the wrong glyphs. They also name the generator's version, since
 * another version may generate other pixels. The same atlas gives the same
 * bytes on every platform.
 *
 * With @p buffer NULL and @p capacity 0, only gives the size.
 *
 * @param[in]  atlas      The atlas.
 * @param[in]  faces      Every face the atlas holds a glyph of, in the
 *                        order aptext_atlas_load() will be given them.
 * @param[in]  face_count How many @p faces there are.
 * @param[out] buffer     Receives the bytes. May be NULL when @p capacity
 *                        is 0.
 * @param[in]  capacity   The size of @p buffer in bytes.
 * @param[out] out_size   The size of the saved bytes.
 * @param[out] out_error  Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT for a NULL @p atlas or
 *         @p out_size, a NULL @p faces with a @p face_count above 0, a NULL
 *         entry or a face twice, a face the atlas holds a glyph of missing
 *         from @p faces, or a NULL @p buffer with a @p capacity above 0; or
 *         APRESULT_LIMIT if @p capacity is above 0 and below the size, with
 *         nothing written.
 */
APRESULT aptext_atlas_save(aptext_atlas atlas, const aptext_face *faces, uint32_t face_count, void *buffer,
                           uint64_t capacity, uint64_t *out_size, APTEXT_ERROR *out_error);

/** @brief What aptext_atlas_load() loads, and what it must match. */
typedef struct APTEXT_ATLAS_LOAD_DESC {
	/** This struct's type, APRI_STRUCT_TYPE_APTEXT_ATLAS_LOAD_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** The settings the caller wants; the saved atlas's must be the same.
	 * Never NULL. */
	const APTEXT_ATLAS_DESC *atlas;
	/** The saved bytes. Never NULL. Only read during the call. */
	const void *data;
	/** The size of #data in bytes; not 0. */
	uint64_t data_size;
	/** The faces, in the order they were given to aptext_atlas_save(). NULL
	 * only when #face_count is 0. The faces must outlive the atlas. */
	const aptext_face *faces;
	/** How many #faces there are. */
	uint32_t face_count;
} APTEXT_ATLAS_LOAD_DESC;

/**
 * @brief Creates an atlas from saved bytes.
 *
 * Every loaded page is dirty in full, so the caller uploads it all.
 *
 * Thread: also reads the faces; may overlap any other call that only reads
 * them, from any thread.
 *
 * @param[in]  desc      What to load. Only read during the call.
 * @param[out] out_atlas The new atlas; NULL on failure.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT for a NULL argument, a
 *         NULL `atlas` or `data`, a `data_size` of 0, a value
 *         APTEXT_ATLAS_DESC refuses, or a NULL `faces` with a `face_count`
 *         above 0 or a NULL entry; a struct-base code for a wrong `base`,
 *         the atlas desc's included; APRESULT_APTEXT_ATLAS_MISMATCH if the
 *         bytes were saved with other settings, other faces, another face
 *         count or another generator version; APRESULT_CORRUPT_DATA if they
 *         fail their checksum or their structure; APRESULT_UNSUPPORTED if
 *         they are in a format version this build doesn't read; or
 *         APRESULT_OUT_OF_MEMORY.
 *
 * @see aptext_atlas_save()
 */
APRESULT aptext_atlas_load(const APTEXT_ATLAS_LOAD_DESC *desc, aptext_atlas *out_atlas, APTEXT_ERROR *out_error);

/**
 * @brief Destroys an atlas, and every page with it.
 *
 * @param[in] atlas The atlas. NULL is accepted and does nothing.
 */
void aptext_atlas_destroy(aptext_atlas atlas);

#if __cplusplus
}
#endif

#endif // APTEXT_H
