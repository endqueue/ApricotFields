#ifndef RBTREND_BATCH_H
#define RBTREND_BATCH_H

#include "rbtrendcommands.h"

#if __cplusplus
extern "C" {
#endif // __cplusplus

/**
 * @file rbtrendbatch.h
 * @brief The batcher: quads, lines, circles and glyphs, one record a
 * shape, drawn in a few instanced draws (card A3, decided 2026-10-28).
 *
 * A batch collects shapes for one frame and draws them, in the order they
 * were added, into the pass it is drawn in (RBTREND_COMMAND_DRAW_BATCH,
 * rbtrendcommands.h). Each shape is one record in the instance's transient
 * memory (rbtrendtransient.h), which the vertex shader expands into a quad:
 * a quarter of the vertex traffic of four vertices a shape. Consecutive
 * shapes of one kind are one instanced draw.
 *
 * - **Quads**, solid or sampling the frame's texture.
 * - **Lines**: polylines of a width in pixels or in paper millimetres (a
 *   pen at a scale), with butt, square or round caps, mitre, bevel or
 *   round joins, and dash patterns measured along the line from its start,
 *   so dashes stay even at every zoom and carry on across corners. Each
 *   segment's coverage is worked out per fragment from its distance to the
 *   segment and the corners at its ends: no tessellation, antialiased, and
 *   no texel drawn twice at a corner.
 * - **Circles**, rings and arcs, antialiased by their distance in the
 *   fragment shader, exact at any size.
 * - **Glyphs**: positioned glyphs from a multi-channel signed distance
 *   field atlas (MTSDF, as Rabbittus Text makes one), with an outline. The batcher
 *   never sees a string, a font or a codepoint: the caller places each
 *   glyph's quad and names its rectangle in the atlas.
 *
 * A frame's shapes are in screen space (pixels of the target, y down,
 * origin at the top left: interface, labels) or in world space (through a
 * view_projection: annotation and debug drawing in a scene), chosen per
 * frame; world-space shapes are depth-tested against the pass's depth or
 * drawn over it, also per frame. Widths and dash lengths are screen
 * measures in both spaces: a world-space line stays its width in pixels or
 * millimetres however far away it is.
 *
 * The debug calls (rbtrend_batch_debug_box() and its kin) add lines for a
 * box, a sphere, an arrow and axes: debug drawing is lines in a world-space
 * batch, nothing more.
 *
 * Not here: text layout (Rabbittus Text), fills of arbitrary polygons (Rabbittus Math's
 * triangulation and the caller's mesh), and anything kept from frame to
 * frame.
 *
 * @par Threads
 * The rule is in rbtrendcontext.h ("Threads"): a batch is used through its
 * instance, one caller at a time.
 */

/** @brief Opaque handle to a batch. */
typedef struct rbtrend_batch_t *rbtrend_batch;

/** @brief Where a frame's shapes are. */
typedef enum RBTREND_BATCH_SPACE RBT_ENUM_BASE {
	/** Refused: a frame says which. */
	RBTREND_BATCH_SPACE_UNSPECIFIED = 0,
	/** Pixels of the target: x right, y down, (0, 0) its top-left corner;
	 * z is not read. */
	RBTREND_BATCH_SPACE_SCREEN = 1,
	/** The scene's coordinates, through RBTREND_BATCH_FRAME's
	 * `view_projection` (Carrottus GPU's clip space). */
	RBTREND_BATCH_SPACE_WORLD = 2,
} RBTREND_BATCH_SPACE;

/** @brief Whether a world-space frame's shapes are hidden behind the scene. */
typedef enum RBTREND_BATCH_DEPTH RBT_ENUM_BASE {
	/** Refused: a frame says which. */
	RBTREND_BATCH_DEPTH_UNSPECIFIED = 0,
	/** Drawn over everything; the pass needs no depth. */
	RBTREND_BATCH_DEPTH_OVER = 1,
	/** Tested against the pass's depth (less or equal, or greater or equal
	 * for depth reversed), never written. A screen-space frame refuses it. */
	RBTREND_BATCH_DEPTH_TESTED = 2,
	/** As #RBTREND_BATCH_DEPTH_TESTED, for depth rendered near at 1. */
	RBTREND_BATCH_DEPTH_TESTED_REVERSED = 3,
} RBTREND_BATCH_DEPTH;

/** @brief The unit of a line's width and dash lengths. */
typedef enum RBTREND_BATCH_UNIT RBT_ENUM_BASE {
	/** Refused: a line says which. */
	RBTREND_BATCH_UNIT_UNSPECIFIED = 0,
	/** Pixels of the target. */
	RBTREND_BATCH_UNIT_PIXELS = 1,
	/** Millimetres of paper, at the frame's `pixels_per_mm`: a pen's width
	 * at a drawing's scale. */
	RBTREND_BATCH_UNIT_PAPER_MM = 2,
} RBTREND_BATCH_UNIT;

/** @brief How an open line's two ends are finished. */
typedef enum RBTREND_BATCH_CAP RBT_ENUM_BASE {
	/** Refused: a line says which. */
	RBTREND_BATCH_CAP_UNSPECIFIED = 0,
	/** Cut square at the end point. */
	RBTREND_BATCH_CAP_BUTT = 1,
	/** Cut square half the width past the end point. */
	RBTREND_BATCH_CAP_SQUARE = 2,
	/** A half disc round the end point. */
	RBTREND_BATCH_CAP_ROUND = 3,
} RBTREND_BATCH_CAP;

/** @brief How a line's segments meet at its corners. */
typedef enum RBTREND_BATCH_JOIN RBT_ENUM_BASE {
	/** Refused: a line says which. */
	RBTREND_BATCH_JOIN_UNSPECIFIED = 0,
	/** The outer edges carried on to where they meet, unless that is
	 * further from the corner than `miter_limit` half widths: then bevelled. */
	RBTREND_BATCH_JOIN_MITER = 1,
	/** The outer corners joined by a straight edge. */
	RBTREND_BATCH_JOIN_BEVEL = 2,
	/** A disc round the corner. */
	RBTREND_BATCH_JOIN_ROUND = 3,
} RBTREND_BATCH_JOIN;

/**
 * @brief Configuration descriptor for a batch: what its pipelines are made
 * for. No field has a default.
 */
typedef struct RBTREND_BATCH_DESC {
	/** This struct's type, RBT_STRUCT_TYPE_REND_BATCH_DESC, and its chain (rbtcore.h, "Struct bases"). */
	RBT_STRUCT_BASE base;
	/** The format of the color target it is drawn into: one a shader
	 * writes floats to. */
	CRGPU_FORMAT color_format;
	/** The format of the pass's depth, or CRGPU_FORMAT_UNKNOWN for a
	 * pass with none (then no frame may be depth-tested). Single-sampled,
	 * as every Rend pipeline is until MSAA (`RBTREND_TODO.md` item 27). */
	CRGPU_FORMAT depth_format;
	/** The most shapes a frame holds (a line holds one a segment, and the
	 * debug calls several): its records are taken from transient memory at
	 * rbtrend_batch_begin(), 128 bytes each. Above 0. */
	uint32_t max_shapes;
	/** For debuggers and messages; may be NULL. Copied. */
	const char *debug_name;
} RBTREND_BATCH_DESC;

/**
 * @brief Creates a batch and the pipelines it draws with.
 *
 * @param[in]  instance  The instance.
 * @param[in]  desc      What it is drawn into. Only read during the call.
 * @param[out] out_batch The new batch; NULL on failure.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return RBT_RESULT_OK; RBT_RESULT_INVALID_ARGUMENT for a NULL argument, a
 *         color format a shader doesn't write floats to, a depth format that
 *         isn't one, or `max_shapes` 0; a struct-base
 *         code for a wrong `base`; RBT_RESULT_OUT_OF_MEMORY; or
 *         RBT_RESULT_REND_GPU_FAILURE if a pipeline can't be made.
 *
 * @see rbtrend_batch_destroy()
 */
RBT_RESULT rbtrend_batch_create(rbtrend_instance instance, const RBTREND_BATCH_DESC *desc, rbtrend_batch *out_batch,
                             RBTREND_ERROR *out_error);

/**
 * @brief Destroys a batch, after every list that draws it has finished on
 * the GPU (as for a pipeline).
 *
 * @param[in] batch The batch. NULL is accepted and does nothing.
 */
void rbtrend_batch_destroy(rbtrend_batch batch);

/**
 * @brief What one frame of a batch is drawn with. No field has a default.
 */
typedef struct RBTREND_BATCH_FRAME {
	/** This struct's type, RBT_STRUCT_TYPE_REND_BATCH_FRAME, and its chain (rbtcore.h, "Struct bases"). */
	RBT_STRUCT_BASE base;
	/** Screen or world. */
	RBTREND_BATCH_SPACE space;
	/** Over the scene or tested against its depth; screen space is always
	 * #RBTREND_BATCH_DEPTH_OVER. */
	RBTREND_BATCH_DEPTH depth;
	/** World space's view_projection, column-major (Carrottus GPU's clip space:
	 * depth 0 to 1, +Y up the target); not read in screen space. Every
	 * value finite. */
	float view_projection[16];
	/** The target's size in pixels, which screen measures (widths, dashes)
	 * are taken against. Above 0. */
	float viewport_size[2];
	/** Pixels to a millimetre of paper, for lines in
	 * RBTREND_BATCH_UNIT_PAPER_MM; 0 for a frame with none. */
	float pixels_per_mm;
	/** What a textured quad samples: a SHADER_RESOURCE 2D view of color,
	 * or NULL for a frame of solid quads only. It must outlive the frame's
	 * draws. */
	rbtrend_texture_view texture;
	/** What glyphs sample: a SHADER_RESOURCE 2D_ARRAY view of an MTSDF
	 * atlas (RGB the multi-channel distance, A the true one; a page a
	 * layer), or NULL for a frame with no glyphs. */
	rbtrend_texture_view atlas;
	/** The atlas's distance range in its texels (msdfgen's pxRange, the
	 * range Rabbittus Text generated it with); above 0 with an atlas. */
	float atlas_distance_range;
} RBTREND_BATCH_FRAME;

/**
 * @brief Starts a frame of a batch: forgets the last frame's shapes and
 * takes room for #RBTREND_BATCH_DESC::max_shapes records of transient memory.
 *
 * Called once a frame before the shapes are added, in the frame the batch
 * is drawn (the transient memory's rule, rbtrendtransient.h).
 *
 * @param[in]  batch     The batch.
 * @param[in]  frame     The frame's settings. Only read during the call.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return RBT_RESULT_OK; RBT_RESULT_INVALID_ARGUMENT for a NULL argument, an
 *         unspecified or unknown space or depth, a depth test in screen
 *         space or with no depth format, a value not finite, a viewport not
 *         above 0, a negative `pixels_per_mm`, a texture or atlas of another
 *         instance or of the wrong type, shape or aspect, or an atlas with
 *         a distance range not above 0; a struct-base code for a wrong
 *         `base`; or a code of rbtrend_transient_allocate() (RBT_RESULT_LIMIT
 *         with no transient memory).
 */
RBT_RESULT rbtrend_batch_begin(rbtrend_batch batch, const RBTREND_BATCH_FRAME *frame, RBTREND_ERROR *out_error);

/**
 * @brief A quad: a parallelogram from a corner and two edges, solid or
 * textured.
 */
/* Frozen: an element of the array rbtrend_batch_add_quads() takes. It never changes; a new need gets a new type. */
typedef struct RBTREND_BATCH_QUAD {
	/** The corner texture coordinate (u0, v0) lands on. */
	float origin[3];
	/** The edge to the corner (u1, v0): x and y in screen space (z not
	 * read), all three in world space. */
	float edge_x[3];
	/** The edge to the corner (u0, v1). */
	float edge_y[3];
	/** Linear RGBA, alpha not premultiplied; times the texture where it
	 * is textured. */
	float color[4];
	/** u0, v0, u1, v1 of the frame's texture; not read for a solid quad. */
	float uv_rect[4];
	/** Samples the frame's texture. */
	bool textured;
} RBTREND_BATCH_QUAD;

/**
 * @brief A circle, a ring or an arc of one, in the plane of two radii.
 */
/* Frozen: an element of the array rbtrend_batch_add_circles() takes. It never changes; a new need gets a new type. */
typedef struct RBTREND_BATCH_CIRCLE {
	/** The centre. */
	float center[3];
	/** The radius along angle 0: its length is the radius (unequal radii
	 * make an ellipse). In screen space x and y (z not read). */
	float radius_x[3];
	/** The radius at a quarter turn from #radius_x. */
	float radius_y[3];
	/** Linear RGBA, alpha not premultiplied. */
	float color[4];
	/** The hole, as a share of the radius: 0 for a disc, up to 1 (a ring
	 * of no width). */
	float inner;
	/** Where the arc starts, in radians from #radius_x towards #radius_y. */
	float start_angle;
	/** How far it turns, in radians: 0, or 2 pi and beyond, for the whole
	 * circle; negative turns the other way. */
	float sweep_angle;
} RBTREND_BATCH_CIRCLE;

/**
 * @brief A polyline: its points, its pen and its dashes.
 */
typedef struct RBTREND_BATCH_LINE {
	/** This struct's type, RBT_STRUCT_TYPE_REND_BATCH_LINE, and its chain (rbtcore.h, "Struct bases"). */
	RBT_STRUCT_BASE base;
	/** Three floats a point, #point_count of them: x and y in screen
	 * space (z not read), all three in world space. Only read during the
	 * call. */
	const float *points;
	/** At least 2. Two equal points in a row are one. */
	uint32_t point_count;
	/** Joins the last point to the first, with a join there and no caps. */
	bool closed;
	/** The width, in #unit; above 0. */
	float width;
	/** The unit of #width, #dashes and #dash_offset. */
	RBTREND_BATCH_UNIT unit;
	/** The ends of an open line. */
	RBTREND_BATCH_CAP cap;
	/** The corners. */
	RBTREND_BATCH_JOIN join;
	/** How far a mitre may reach, in half widths from the corner; 1 or
	 * above. Read only for #RBTREND_BATCH_JOIN_MITER. */
	float miter_limit;
	/** Linear RGBA, alpha not premultiplied. */
	float color[4];
	/** Dash and gap lengths in turn, dash first, in #unit, measured along
	 * the line on the screen; NULL for a solid line. Each dash takes the
	 * caps (a round cap makes round dashes). Only read during the call. */
	const float *dashes;
	/** An even count, up to 8: 0 for a solid line. */
	uint32_t dash_count;
	/** How far into the pattern the line starts, in #unit. */
	float dash_offset;
} RBTREND_BATCH_LINE;

/**
 * @brief A glyph: its quad, where it is in the atlas, and its colors.
 */
/* Frozen: an element of the array rbtrend_batch_add_glyphs() takes. It never changes; a new need gets a new type. */
typedef struct RBTREND_BATCH_GLYPH {
	/** The quad's top-left corner, the glyph's plane bounds placed by the
	 * caller (Rabbittus Text's `plane_left` and `plane_top` from the pen position,
	 * at its size). */
	float origin[3];
	/** The edge to the top-right corner. */
	float edge_x[3];
	/** The edge to the bottom-left corner. */
	float edge_y[3];
	/** Linear RGBA of the glyph, alpha not premultiplied. */
	float color[4];
	/** Linear RGBA of its outline. */
	float outline_color[4];
	/** The outline's width, as a share of the atlas's distance range: 0
	 * for none, up to 1 (as far as the field reaches). */
	float outline_width;
	/** u0, v0, u1, v1 of its rectangle in the atlas page, as shares of the
	 * page's size. */
	float uv_rect[4];
	/** The atlas page: the layer of the frame's atlas. */
	uint32_t layer;
} RBTREND_BATCH_GLYPH;

/**
 * @brief Adds quads to the frame.
 *
 * @param[in]  batch     The batch, begun this frame.
 * @param[in]  quads     @p count quads. Only read during the call.
 * @param[in]  count     How many.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return RBT_RESULT_OK; RBT_RESULT_INVALID_ARGUMENT for a NULL argument, a
 *         batch not begun, a value not finite, or a textured quad in a frame
 *         with no texture; or RBT_RESULT_LIMIT past `max_shapes` (nothing is
 *         added).
 */
RBT_RESULT rbtrend_batch_add_quads(rbtrend_batch batch, const RBTREND_BATCH_QUAD *quads, uint32_t count,
                                RBTREND_ERROR *out_error);

/**
 * @brief Adds circles, rings and arcs to the frame.
 *
 * @param[in]  batch     The batch, begun this frame.
 * @param[in]  circles   @p count circles. Only read during the call.
 * @param[in]  count     How many.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return RBT_RESULT_OK; RBT_RESULT_INVALID_ARGUMENT for a NULL argument, a
 *         batch not begun, a value not finite or an `inner` outside 0 to 1;
 *         or RBT_RESULT_LIMIT past `max_shapes` (nothing is added).
 */
RBT_RESULT rbtrend_batch_add_circles(rbtrend_batch batch, const RBTREND_BATCH_CIRCLE *circles, uint32_t count,
                                  RBTREND_ERROR *out_error);

/**
 * @brief Adds a polyline to the frame, a record a segment.
 *
 * In world space, the dashes are laid out along the line as it is
 * projected now (the frame's view_projection and viewport), and a segment
 * passing behind the eye is cut at the near plane.
 *
 * @param[in]  batch     The batch, begun this frame.
 * @param[in]  line      The line. Only read during the call.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return RBT_RESULT_OK; RBT_RESULT_INVALID_ARGUMENT for a NULL argument, a
 *         batch not begun, fewer than 2 points, a value not finite, a width
 *         not above 0, an unspecified or unknown unit, cap or join, a mitre
 *         limit under 1, paper millimetres in a frame with no
 *         `pixels_per_mm`, an odd dash count or one over 8, or a dash or
 *         gap that is negative or a pattern of no length; a struct-base
 *         code for a wrong `base`; or RBT_RESULT_LIMIT past `max_shapes`
 *         (nothing is added).
 */
RBT_RESULT rbtrend_batch_add_line(rbtrend_batch batch, const RBTREND_BATCH_LINE *line, RBTREND_ERROR *out_error);

/**
 * @brief Adds glyphs to the frame.
 *
 * @param[in]  batch     The batch, begun this frame.
 * @param[in]  glyphs    @p count glyphs. Only read during the call.
 * @param[in]  count     How many.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return RBT_RESULT_OK; RBT_RESULT_INVALID_ARGUMENT for a NULL argument, a
 *         batch not begun, a frame with no atlas, a value not finite, an
 *         outline width outside 0 to 1, or a layer past the atlas's; or
 *         RBT_RESULT_LIMIT past `max_shapes` (nothing is added).
 */
RBT_RESULT rbtrend_batch_add_glyphs(rbtrend_batch batch, const RBTREND_BATCH_GLYPH *glyphs, uint32_t count,
                                 RBTREND_ERROR *out_error);

/**
 * @brief How many records the frame holds so far.
 *
 * @param[in] batch The batch.
 *
 * @return The count, or 0 if @p batch is NULL.
 */
uint32_t rbtrend_batch_get_shape_count(rbtrend_batch batch);

/* ---- Debug drawing ------------------------------------------------------ */

/**
 * @brief Adds a box's twelve edges, in world space.
 *
 * @param[in]  batch     The batch, begun this frame in world space.
 * @param[in]  center    The box's centre.
 * @param[in]  half_x    Half its edge along its first axis (length and
 *                       direction).
 * @param[in]  half_y    Half its edge along its second axis.
 * @param[in]  half_z    Half its edge along its third axis.
 * @param[in]  color     Linear RGBA, alpha not premultiplied.
 * @param[in]  width     In pixels; above 0.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return As rbtrend_batch_add_line(); RBT_RESULT_INVALID_ARGUMENT also for a
 *         frame in screen space.
 */
RBT_RESULT rbtrend_batch_debug_box(rbtrend_batch batch, const float center[3], const float half_x[3], const float half_y[3],
                                const float half_z[3], const float color[4], float width, RBTREND_ERROR *out_error);

/**
 * @brief Adds a sphere as three great circles, round its own axes, in
 * world space.
 *
 * @param[in]  batch     The batch, begun this frame in world space.
 * @param[in]  center    The centre.
 * @param[in]  radius    Above 0.
 * @param[in]  color     Linear RGBA, alpha not premultiplied.
 * @param[in]  width     In pixels; above 0.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return As rbtrend_batch_debug_box().
 */
RBT_RESULT rbtrend_batch_debug_sphere(rbtrend_batch batch, const float center[3], float radius, const float color[4],
                                   float width, RBTREND_ERROR *out_error);

/**
 * @brief Adds an arrow: a line and a head of two strokes, in world space.
 *
 * @param[in]  batch     The batch, begun this frame in world space.
 * @param[in]  from      The tail.
 * @param[in]  to        The point; not @p from.
 * @param[in]  head      The head's length, in the scene's units; above 0.
 * @param[in]  color     Linear RGBA, alpha not premultiplied.
 * @param[in]  width     In pixels; above 0.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return As rbtrend_batch_debug_box().
 */
RBT_RESULT rbtrend_batch_debug_arrow(rbtrend_batch batch, const float from[3], const float to[3], float head,
                                  const float color[4], float width, RBTREND_ERROR *out_error);

/**
 * @brief Adds three axes from a point: x red, y green, z blue, in world
 * space.
 *
 * @param[in]  batch     The batch, begun this frame in world space.
 * @param[in]  origin    Where they start.
 * @param[in]  length    Each axis's length, in the scene's units; above 0.
 * @param[in]  width     In pixels; above 0.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return As rbtrend_batch_debug_box().
 */
RBT_RESULT rbtrend_batch_debug_axes(rbtrend_batch batch, const float origin[3], float length, float width,
                                 RBTREND_ERROR *out_error);

#if __cplusplus
} // Extern "C"
#endif // __cplusplus

#endif // RBTREND_BATCH_H
