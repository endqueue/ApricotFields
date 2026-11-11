/*
 * rbtrend_lights_draft.h - directional lights as a light type, one shadow
 * setter for every light, and one shadow atlas per scene (cards A35, b,
 * decided 2026-10-31; A36, a, and A37, b, decided 2026-11-01 with the
 * user; the workspace's DECISIONS.md, "Decided"; RBTREND_TODO.md item 24).
 *
 * APPROVED 2026-11-01 by the user, as written here. To be built into
 * rbtrendlit.h, rbtrendcommands.h and rbtcore.h; where building finds this
 * draft wrong or incomplete, a card says so and this file is not edited to
 * match.
 *
 * DRAFT, 2026-11-01. Not built, not in any
 * target. Not compilable: it repeats declarations of rbtrendlit.h,
 * rbtrendcommands.h and rbtcore.h to show them changed. Every change
 * below is marked [ADDED], [CHANGED] or [REMOVED]; nothing unmarked
 * changes.
 *
 * Nothing in Rend is the sun after it: a scene holds up to four
 * directional lights beside its point and spot lights, in one list; any
 * light has at most one shadow, set by naming the light; every shadow
 * drawn from maps reads one depth atlas, in which each of its views (a
 * spot's one, a point light's six faces, a directional light's cascades)
 * names its rectangle.
 *
 * ---------------------------------------------------------------------------
 * What it settles that the cards left open
 * ---------------------------------------------------------------------------
 *
 * 1. One list for every light, and indices into it. A shadow, the traced
 *    mask command and the path trace name a light by its place in the
 *    list rbtrend_lit_scene_set_lights() was given, whatever its type.
 *    Directional lights aren't culled per tile; point and spot lights
 *    are, as today.
 * 2. The sky's discs. A35's record says RBTREND_COMMAND_LIT_SKY takes a
 *    mask of directional lights and draws a disc for each. That command
 *    fills an environment and draws no disc (its header says why: a disc
 *    would only be blurred into the rougher mips); the disc is drawn by
 *    RBTREND_LIT_SHADER_SKY_FRAGMENT from the scene. So the choice moves
 *    to the scene's data: each directional light says whether the sky
 *    draws its disc (`_sky_disc`). The shape is the same (none, a sun, a
 *    sun and a moon); the place is where the disc is drawn.
 * 3. A light's size. One field, `_radius`: a directional light's angular
 *    radius (its disc, and the softness of a traced shadow and the path
 *    trace's light), read today. For point and spot lights it is the
 *    source radius card A38 asks about (option b, open, recommended:
 *    the room made in this struct, read by nothing until a caller asks);
 *    until A38 is decided it must be 0 for them, and is refused
 *    otherwise, so deciding it later changes no layout.
 * 4. A cap on the atlas's views, not on shadows per type: a scene draws
 *    at most RBTREND_LIT_MAX_SHADOW_VIEWS (64) views (a cascade, a spot or
 *    a point face each), a constant beside the code that checks it, since
 *    the shadow table the surface reads is sized by it. Today's caps add
 *    to 32 (4 cascades, 4 spots, 24 faces); 64 holds four directional
 *    lights of four cascades and eight point lights.
 * 5. A shadow outlives a change of the light list where it still fits:
 *    rbtrend_lit_scene_set_lights() keeps each shadow whose index still
 *    names a light of a type its kind fits, and takes the rest away, so a
 *    list resent each frame keeps its shadows.
 */

/* =====================================================================
 * rbtcore.h: struct types
 * ===================================================================== */

/*
 * [REMOVED] RBT_STRUCT_TYPE_REND_LIT_SUN (5020) and
 * RBT_STRUCT_TYPE_REND_LIT_SUN_SHADOW (5021): their numbers are not
 * reused.
 * [ADDED] RBT_STRUCT_TYPE_REND_LIT_SHADOW_MAPS, the next free number in
 * Rend's block.
 * RBT_STRUCT_TYPE_REND_LIT_CASCADE_FIT (5022) and
 * RBT_STRUCT_TYPE_REND_LIT_LIGHT_SHADOW (5034) are kept.
 */

/* =====================================================================
 * rbtrendlit.h
 * ===================================================================== */

/** [CHANGED] The most point and spot lights a scene holds: 128, as before. */
#define RBTREND_LIT_MAX_LIGHTS 128u
/** [ADDED] The most directional lights a scene holds, beside them (Hazel's 4). */
#define RBTREND_LIT_MAX_DIRECTIONAL_LIGHTS 4u
/** The most cascades a directional light's shadow has. Unchanged. */
#define RBTREND_LIT_MAX_CASCADES 4u
/** [ADDED] The most views a scene's shadows draw from its atlas, together: a
 * cascade, a spot's map and each of a point light's six faces are one each. */
#define RBTREND_LIT_MAX_SHADOW_VIEWS 64u
/* [REMOVED] RBTREND_LIT_MAX_SPOT_SHADOWS and RBTREND_LIT_MAX_POINT_SHADOWS:
 * spots and points are capped by the views their shadows draw. */

/*
 * [CHANGED] The file comment's first line: "under directional lights with
 * cascaded shadows, point and spot lights, each with a shadow of its own",
 * for "under a sun with cascaded shadows, point and spot lights (spots
 * with shadows of their own)". "@par Sets": a pass that renders a shadow
 * binds, at set 0, the material rbtrend_lit_scene_get_shadow_pass_material()
 * gives for the light and view it renders.
 */

/**
 * [CHANGED] The kinds of light.
 */
typedef uint32_t RBTREND_LIT_LIGHT_TYPE;
enum {
	/** Light from one point, every way. */
	RBTREND_LIT_LIGHT_TYPE_POINT = 0,
	/** Light from one point, in a cone. */
	RBTREND_LIT_LIGHT_TYPE_SPOT = 1,
	/** [ADDED] Light from one direction, everywhere alike: a sun, a moon. */
	RBTREND_LIT_LIGHT_TYPE_DIRECTIONAL = 2,
};

/*
 * [REMOVED] RBTREND_LIT_LIGHT (frozen, so not changed: replaced by
 * RBTREND_LIT_SCENE_LIGHT below, which every caller moves to).
 */

/**
 * [ADDED] One light of a scene, of any type.
 *
 * Why: A35's light type. A frozen struct can't gain the directional
 * light's fields, so a new one replaces RBTREND_LIT_LIGHT, with the size
 * A38 may need.
 */
/* Frozen: an array element, set every frame. It never changes; a new need gets a new type. */
typedef struct RBTREND_LIT_SCENE_LIGHT {
	/** Which type. */
	RBTREND_LIT_LIGHT_TYPE _type;
	/** Where a point or spot light is, in the scene's space. Not read for a
	 * directional light. */
	float _position[3];
	/**
	 * A spot's axis, from the light outwards; a directional light's
	 * direction, from the surface towards the light. Any length but 0. Not
	 * read for a point light.
	 */
	float _direction[3];
	/**
	 * Its color times its strength, linear: what a white matte face square
	 * to it reads (a point or spot light at distance 1). 0 is a light that
	 * gives nothing.
	 */
	float _radiance[3];
	/** Where a point or spot light's light has fallen to 0, smoothly: above
	 * 0. What tiled culling bounds it by. Not read for a directional light. */
	float _range;
	/** A spot's full light within this angle of its axis, in radians... */
	float _inner_angle;
	/** ...fading to none at this one: _inner_angle to pi / 2. */
	float _outer_angle;
	/**
	 * A directional light's angular radius, in radians, 0 to 0.5: the
	 * angle from its disc's centre to its edge (0.00465 for the sun seen
	 * from the earth). Its light is spread over the disc, so it lights
	 * nothing more than `_radiance`; a traced shadow and the path trace
	 * read it as the light's size. A point or spot light's: 0 (card A38).
	 */
	float _radius;
	/**
	 * A directional light only: whether RBTREND_LIT_SHADER_SKY_FRAGMENT
	 * draws its disc, at `_radius`. Each texel of a disc reads pi times
	 * `_radiance` over its solid angle, 2 pi (1 - cos(_radius)). Must be
	 * false for the other types.
	 */
	bool _sky_disc;
} RBTREND_LIT_SCENE_LIGHT;

/**
 * [CHANGED] Sets a scene's lights, every type, replacing the ones it had.
 * Shadows, the traced mask command and the path trace name a light by its
 * place in this list.
 *
 * A shadow set for a light stays while its index names a light of a type
 * its kind fits; the call takes away the rest (an index past the new
 * count, or a kind the new light's type can't have).
 *
 * @param[in] scene  Scene to change.
 * @param[in] lights The lights; NULL with a count of 0 for none. Only read
 *                   during the call.
 * @param[in] count  How many: up to RBTREND_LIT_MAX_LIGHTS plus
 *                   RBTREND_LIT_MAX_DIRECTIONAL_LIGHTS.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return RBT_RESULT_OK. Otherwise, with nothing changed:
 *         RBT_RESULT_INVALID_ARGUMENT if @p scene is NULL, @p lights is NULL
 *         with a count above 0, or a light (named by its index) has a type
 *         that is not an RBTREND_LIT_LIGHT_TYPE, a value not finite, a
 *         negative radiance, a direction of 0 where it is read, a range not
 *         above 0 (point, spot), angles out of order or past pi / 2
 *         (spot), a radius out of 0 to 0.5 (directional) or not 0 (point,
 *         spot), or `_sky_disc` true on a point or spot light;
 *         RBT_RESULT_LIMIT if more than RBTREND_LIT_MAX_LIGHTS are point and
 *         spot lights, or more than RBTREND_LIT_MAX_DIRECTIONAL_LIGHTS are
 *         directional.
 */
RBT_RESULT rbtrend_lit_scene_set_lights(rbtrend_lit_scene scene, const RBTREND_LIT_SCENE_LIGHT *lights, uint32_t count,
                                     RBTREND_ERROR *out_error);

/*
 * [REMOVED] RBTREND_LIT_SUN, rbtrend_lit_scene_set_sun(): the sun is a
 * directional light of the list.
 * [REMOVED] RBTREND_LIT_SUN_SHADOW, rbtrend_lit_scene_set_sun_shadow(),
 * rbtrend_lit_scene_get_sun_shadow_material(): a directional light's
 * cascades are its shadow, set by rbtrend_lit_scene_set_light_shadow().
 * [REMOVED] RBTREND_LIT_SPOT_SHADOW, rbtrend_lit_scene_set_spot_shadows(),
 * rbtrend_lit_scene_get_spot_shadow_material(), RBTREND_LIT_POINT_SHADOW,
 * rbtrend_lit_scene_set_point_shadows(),
 * rbtrend_lit_scene_get_point_shadow_material(): the same (card A36).
 */

/**
 * [ADDED] Sets the depth atlas every shadow drawn from maps reads, or
 * takes it away (card A37).
 *
 * The caller renders each view of each shadow into its rectangle of the
 * atlas (RBTREND_LIT_SHADOW_MAPS::_rects): a depth-only pass over the
 * atlas, its viewport and scissor the rectangle
 * (RBTREND_COMMAND_SET_VIEWPORTS, RBTREND_COMMAND_SET_SCISSOR_RECTS), binding
 * rbtrend_lit_scene_get_shadow_pass_material() for that light and view at
 * set 0. Which rectangle each view has, and how large, is the caller's:
 * rectangles may differ in size, and nothing checks two don't overlap.
 * Every view's depth is rendered with the same direction of depth.
 *
 * Why: one binding for any number of shadows of any size, the caps then
 * the view table's; a small or distant light's shadow costs what the
 * caller gives it (Unreal's, Godot's and Unity's atlases).
 *
 * @param[in] scene          Scene to change.
 * @param[in] atlas          A SHADER_RESOURCE view, DEPTH aspect, shaped 2D,
 *                           of a depth texture; NULL for none, which every
 *                           map shadow then reads as lit. Borrowed: it
 *                           outlives its use by the scene, until set again
 *                           or the scene goes.
 * @param[in] reversed_depth The views are rendered near at 1, far at 0.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return RBT_RESULT_OK. Otherwise, with nothing changed:
 *         RBT_RESULT_INVALID_ARGUMENT if @p scene is NULL, or @p atlas is of
 *         another instance or not a depth view shaped as above; or
 *         RBT_RESULT_INVALID_ARGUMENT, naming the light, if a shadow already
 *         set has a rectangle past the atlas's size.
 */
RBT_RESULT rbtrend_lit_scene_set_shadow_atlas(rbtrend_lit_scene scene, rbtrend_texture_view atlas, bool reversed_depth,
                                           RBTREND_ERROR *out_error);

/**
 * [CHANGED] What kind of shadow a light has.
 */
typedef enum RBTREND_LIT_SHADOW_KIND RBT_ENUM_BASE {
	/** Not a kind: refused. */
	RBTREND_LIT_SHADOW_KIND_NONE      = 0,
	/** A traced mask, a directional light's, as before. */
	RBTREND_LIT_SHADOW_KIND_MASK      = 1,
	/** [CHANGED] A directional light's cascades, from the atlas: taken now
	 * (refused until card A35, before). Its fields are an
	 * RBTREND_LIT_SHADOW_MAPS chained on the shadow's base. */
	RBTREND_LIT_SHADOW_KIND_CASCADES  = 2,
	/** [ADDED] A spot's map, one view in the atlas; an RBTREND_LIT_SHADOW_MAPS
	 * chained on. */
	RBTREND_LIT_SHADOW_KIND_SPOT_MAP  = 3,
	/** [ADDED] A point light's six faces, +X, -X, +Y, -Y, +Z, -Z, each a
	 * view in the atlas; an RBTREND_LIT_SHADOW_MAPS chained on. A fragment is
	 * looked up in the face its offset from the light is largest along. */
	RBTREND_LIT_SHADOW_KIND_POINT_MAP = 4,
} RBTREND_LIT_SHADOW_KIND;

/**
 * [CHANGED] One light's shadow: its kind, its strength and, for a mask,
 * the mask. A map kind's fields are an RBTREND_LIT_SHADOW_MAPS chained on
 * `base` (its `next`); a mask has none chained. Layout unchanged.
 */
typedef struct RBTREND_LIT_LIGHT_SHADOW {
	RBT_STRUCT_BASE base;
	RBTREND_LIT_SHADOW_KIND _kind;
	float _strength;
	rbtrend_texture_view _mask;
} RBTREND_LIT_LIGHT_SHADOW;

/** [ADDED] A rectangle of a scene's shadow atlas, in texels. */
/* Frozen: an array element of RBTREND_LIT_SHADOW_MAPS. It never changes; a new need gets a new type. */
typedef struct RBTREND_LIT_SHADOW_RECT {
	/** Its left column. */
	uint32_t _x;
	/** Its top row. */
	uint32_t _y;
	/** Its width: above 0. */
	uint32_t _width;
	/** Its height: above 0. */
	uint32_t _height;
} RBTREND_LIT_SHADOW_RECT;

/**
 * [ADDED] A map shadow's views: each one's matrix and rectangle of the
 * atlas, and the filtering. Chained on RBTREND_LIT_LIGHT_SHADOW's base.
 *
 * Why: the three map kinds' fields are the same but for how many views
 * and what a cascade adds (its split), so one struct (a spot's
 * RBTREND_LIT_SPOT_SHADOW, a point's RBTREND_LIT_POINT_SHADOW and
 * RBTREND_LIT_SUN_SHADOW's map fields, folded together).
 */
typedef struct RBTREND_LIT_SHADOW_MAPS {
	/** This struct's type, RBT_STRUCT_TYPE_REND_LIT_SHADOW_MAPS, and its chain (rbtcore.h, "Struct bases"). */
	RBT_STRUCT_BASE base;
	/** How many views: 1 to RBTREND_LIT_MAX_CASCADES for CASCADES, 1 for a
	 * SPOT_MAP, 6 for a POINT_MAP. */
	uint32_t _view_count;
	/**
	 * Each view's matrix from the scene's space to its clip space, column
	 * after column, as rendered into its rectangle (the rectangle is the
	 * whole of that clip space). rbtrend_lit_fit_cascades(),
	 * rbtrend_lit_spot_shadow_view_projection() and
	 * rbtrend_lit_point_shadow_view_projection() frame them.
	 */
	float _view_projection[6][16];
	/** Each view's rectangle of the atlas, inside it. Filtering reads within
	 * a rectangle only, clamped at its edge. */
	RBTREND_LIT_SHADOW_RECT _rects[6];
	/**
	 * CASCADES only: how far along the view direction from the eye each
	 * cascade reaches, increasing and above 0, blended as the sun's were
	 * (the last tenth of each into the next; the last fades out). Not read
	 * for the other kinds.
	 */
	float _split_depths[RBTREND_LIT_MAX_CASCADES];
	/** Depth added in the light's favour before the test, in the map's
	 * depth units: against acne. */
	float _depth_bias;
	/** How far a surface's position is moved along its normal before it is
	 * looked up, in the scene's units: against acne on slopes. */
	float _normal_offset;
	/** The edge, in texels: 0 for the hardware's own filter alone, up to 4. */
	float _softness;
} RBTREND_LIT_SHADOW_MAPS;

/**
 * [CHANGED] Sets light @p light's one shadow, of any kind its type can
 * have, replacing what it had, or takes it away (card A36).
 *
 * A directional light takes a MASK or CASCADES; a spot a SPOT_MAP; a point
 * light a POINT_MAP. A map shadow's rectangles must lie inside the scene's
 * atlas (rbtrend_lit_scene_set_shadow_atlas()), and the scene's views
 * together stay within RBTREND_LIT_MAX_SHADOW_VIEWS. The shadow pass
 * materials of its views get their matrices here.
 *
 * Why the one setter: a light has one shadow, set by naming the light; a
 * change to one light's shadow touches that light alone (Unreal, Godot and
 * Unity set shadows on each light), and a later kind (a traced spot or
 * point shadow, card A38) is one more kind, not a setter.
 *
 * @param[in] scene     Scene to change.
 * @param[in] light     Which light: its place in the scene's lights.
 * @param[in] shadow    The shadow, or NULL for none. Only read during the
 *                      call; a mask view it names is borrowed, as before.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return RBT_RESULT_OK. Otherwise, checked in this order, with nothing
 *         changed: RBT_RESULT_INVALID_ARGUMENT if @p scene is NULL;
 *         RBT_RESULT_INVALID_ARGUMENT if @p light names no light of the
 *         scene; the struct codes for `shadow->base`;
 *         RBT_RESULT_INVALID_ARGUMENT if `_kind` is NONE or not an
 *         RBTREND_LIT_SHADOW_KIND, or a kind the light's type can't have, or
 *         `_strength` is not finite or out of 0 to 1; for a mask, as before;
 *         for a map kind, the struct codes for the chained
 *         RBTREND_LIT_SHADOW_MAPS (missing: RBT_RESULT_INVALID_ARGUMENT), then
 *         RBT_RESULT_INVALID_ARGUMENT if `_view_count` is wrong for the kind,
 *         a value is not finite, a rectangle is empty or past the atlas (or
 *         there is no atlas), the splits are not increasing above 0
 *         (CASCADES), the softness is out of 0 to 4 or a bias negative;
 *         RBT_RESULT_LIMIT if the scene's views would pass
 *         RBTREND_LIT_MAX_SHADOW_VIEWS.
 */
RBT_RESULT rbtrend_lit_scene_set_light_shadow(rbtrend_lit_scene scene, uint32_t light, const RBTREND_LIT_LIGHT_SHADOW *shadow,
                                           RBTREND_ERROR *out_error);

/**
 * [ADDED] The material a pass that renders view @p view of light
 * @p light's map shadow binds at set 0: of rbtrend_lit_get_shadow_layout(),
 * holding that view's matrix. The scene's: don't destroy it. It changes
 * with the light's shadow; one taken before a later
 * rbtrend_lit_scene_set_light_shadow() for the light holds the new matrix.
 *
 * The scene makes RBTREND_LIT_MAX_SHADOW_VIEWS such materials when it is
 * created (rbtrend_lit_scene_create(), which says so) and hands them out as
 * shadows take views: nothing is allocated here or by
 * rbtrend_lit_scene_set_light_shadow().
 *
 * Why: replaces the sun's, the spots' and the points' three getters with
 * one named as the shadow is, by light.
 *
 * @param[in] scene Scene to read.
 * @param[in] light Which light.
 * @param[in] view  Which view of its shadow: a cascade, or a face.
 *
 * @return The material, or NULL if @p scene is NULL, @p light has no map
 *         shadow, or @p view is not below its view count.
 */
rbtrend_material rbtrend_lit_scene_get_shadow_pass_material(rbtrend_lit_scene scene, uint32_t light, uint32_t view);

/**
 * [CHANGED] RBTREND_LIT_CASCADE_FIT: `sun_direction` becomes
 * `light_direction` (the directional light's `_direction`), and
 * `map_size` the width in texels of each cascade's rectangle. Layout and
 * type number unchanged; the rename breaks source only.
 *
 * [CHANGED] rbtrend_lit_fit_sun_cascades() becomes rbtrend_lit_fit_cascades():
 * the same framing, into an RBTREND_LIT_SHADOW_MAPS (its _view_count,
 * _view_projection and _split_depths; its rectangles, biases and softness
 * left as they are, the caller's).
 */
RBT_RESULT rbtrend_lit_fit_cascades(const RBTREND_LIT_CASCADE_FIT *fit, RBTREND_LIT_SHADOW_MAPS *out_maps, RBTREND_ERROR *out_error);

/**
 * [CHANGED] rbtrend_lit_point_shadow_view_projection() and
 * rbtrend_lit_spot_shadow_view_projection() take an RBTREND_LIT_SCENE_LIGHT
 * (of type POINT, or SPOT), otherwise unchanged.
 */
RBT_RESULT rbtrend_lit_point_shadow_view_projection(const RBTREND_LIT_SCENE_LIGHT *light, uint32_t face, float near_depth, bool reversed_depth,
                                                 float out_view_projection[16], RBTREND_ERROR *out_error);
RBT_RESULT rbtrend_lit_spot_shadow_view_projection(const RBTREND_LIT_SCENE_LIGHT *light, float near_depth, bool reversed_depth,
                                                float out_view_projection[16], RBTREND_ERROR *out_error);

/*
 * [CHANGED] RBTREND_LIT_SHADER_SKY_FRAGMENT: "With directional lights
 * whose `_sky_disc` is true, each disc is drawn over it at its `_radius`,
 * its edge softened over a texel", for the sun's disc.
 *
 * [CHANGED] RBTREND_LIT_VIEW::view_direction: "what directional lights'
 * cascades are measured along. Any length; 0 only for a scene with no
 * cascades."
 */

/* =====================================================================
 * rbtrendcommands.h
 * ===================================================================== */

/*
 * [CHANGED] RBTREND_COMMAND_LIT_SKY: "The sun's disc is not drawn here: a
 * directional light's disc is the scene's (RBTREND_LIT_SCENE_LIGHT::_sky_disc),
 * drawn by the sky shader." `_sun_direction` stays: which way the sky's
 * own model has its sun, the caller's (one of its directional lights,
 * usually). Layout unchanged.
 *
 * [CHANGED] RBTREND_COMMAND_LIT_SHADOW_MASK: `_light` names a directional
 * light of the scene by its place in the scene's lights ("today only 0,
 * the sun" goes); its disc is its `_direction` and `_radius`. A `_light`
 * that names no light, or a light that isn't directional, is refused at
 * send. Layout unchanged.
 *
 * [CHANGED] RBTREND_COMMAND_LIT_PATH_TRACE: "every directional light over
 * its disc" reads each directional light's `_radius`. Unchanged otherwise.
 */

/*
 * ---------------------------------------------------------------------------
 * ABI breaks (before 1.0; check.sh abi lists them; every caller changes in
 * the same change, as A35 decided)
 * ---------------------------------------------------------------------------
 *
 * - Removed: RBTREND_LIT_SUN, RBTREND_LIT_SUN_SHADOW, RBTREND_LIT_SPOT_SHADOW,
 *   RBTREND_LIT_POINT_SHADOW, RBTREND_LIT_LIGHT, rbtrend_lit_scene_set_sun(),
 *   _set_sun_shadow(), _get_sun_shadow_material(), _set_spot_shadows(),
 *   _get_spot_shadow_material(), _set_point_shadows(),
 *   _get_point_shadow_material(), rbtrend_lit_fit_sun_cascades(),
 *   RBTREND_LIT_MAX_SPOT_SHADOWS, RBTREND_LIT_MAX_POINT_SHADOWS, two struct
 *   type numbers (not reused).
 * - Changed: rbtrend_lit_scene_set_lights() and the two view-projection
 *   helpers take RBTREND_LIT_SCENE_LIGHT; RBTREND_LIT_CASCADE_FIT's two
 *   fields renamed; RBTREND_LIT_SHADOW_KIND_CASCADES taken; the shadow mask
 *   command's `_light` an index into the one list.
 * - Added: RBTREND_LIT_LIGHT_TYPE_DIRECTIONAL, two kinds,
 *   RBTREND_LIT_SCENE_LIGHT, RBTREND_LIT_SHADOW_RECT, RBTREND_LIT_SHADOW_MAPS
 *   (and its struct type), rbtrend_lit_scene_set_shadow_atlas(),
 *   rbtrend_lit_scene_get_shadow_pass_material(), rbtrend_lit_fit_cascades(),
 *   RBTREND_LIT_MAX_DIRECTIONAL_LIGHTS, RBTREND_LIT_MAX_SHADOW_VIEWS.
 * - Behaviour: a scene starts with no lights, as before, so no sun: a
 *   caller that set none draws as before. A caller's shadow maps move
 *   into one atlas: the sun's 2D array, the spots' and the points' become
 *   rectangles of one 2D depth texture.
 * - Callers in the workspace that change with it: Rend's own tests,
 *   rabbittustests' lit tests (rbtrend_gpu_tests.c), its destroyed-handle
 *   list, the misuse harness (regenerated); no other library calls these
 *   yet.
 *
 * ---------------------------------------------------------------------------
 * Tests
 * ---------------------------------------------------------------------------
 *
 * Rabbittus (no device): set_lights' refusals each by name (a radius
 * on a point light, a disc on a spot, a fifth directional light, a 129th
 * point or spot light); set_light_shadow's refusals in their order (a kind
 * the type can't have, a missing chained struct, a view count wrong for
 * the kind, a rectangle past the atlas, the 65th view); a shadow kept and
 * one taken away when the lights are resent; fit_cascades' framing the
 * same bits as fit_sun_cascades' today for the same inputs.
 *
 * rabbittustests (a device; Metal here under validation with warnings
 * fatal, D3D12 and Vulkan in the handoff):
 * - test_lit_mesh_shadow and every lit shadow test moved to the atlas,
 *   reading what they read today: a directional light's four cascades,
 *   spot and point shadows, now rectangles of one atlas.
 * - Two directional lights, each with cascades in its own rectangles, a
 *   box on a floor: two shadows on the floor, each where its light puts
 *   it, and the floor lit by both outside them.
 * - Rectangles of different sizes for two spots: each shadow where it
 *   belongs; a soft shadow's filter near a rectangle's edge reading
 *   nothing of its neighbour (a neighbour filled with depth 0).
 * - The sky with two discs (`_sky_disc` on both, different radii) and one
 *   with none.
 * - The traced mask and the path trace naming a directional light other
 *   than the first (on the device with ray queries).
 * - test_frame_allocations with four shadowed directional lights: none of
 *   Rend's.
 *
 * ---------------------------------------------------------------------------
 * Compared
 * ---------------------------------------------------------------------------
 *
 * - Hazel: four directional lights (Scene::MaxDirectionalLights), each
 *   with CastsShadows; its cascades renderer-wide, one 2D array.
 * - Unreal: directional lights are ordinary lights, any number, each with
 *   its own cascades (or virtual shadow map); one or two flagged as the
 *   sky atmosphere's sun. Shadows placed in an atlas per light.
 * - Godot 4: up to 8 directional lights, their shadows in one directional
 *   shadow atlas, point and spot shadows in a second atlas of quadrants.
 * - Unity: URP's main light (one directional with cascades) and
 *   additional lights, their shadows in an atlas; HDRP any number of
 *   shadowed directional lights, one cascaded.
 * - Filament: one directional light (its sun), with cascades.
 * - The platform APIs: a viewport and scissor per rectangle on all three;
 *   one depth texture sampled with comparison, clamped in the shader.
 */
