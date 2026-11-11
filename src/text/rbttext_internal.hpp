#ifndef RBTTEXT_INTERNAL_HPP
#define RBTTEXT_INTERNAL_HPP

#include "text/rbttext.h"
#include "../rbthashindex.hpp"
#include "../rbtoverlap.hpp"
#include "../rbtstale.hpp"
#include "../rbtplaindata.hpp"
#include <eqstd.hpp>
#include <hb-ot.h>
#include <hb.h>
#include <stdarg.h>
#include <stdio.h>

// Rabbittus Text's objects, and what its three sources share: rbttext.cpp (faces and
// chains), rbttext_layout.cpp (layouts) and rbttext_atlas.cpp (glyph
// generation and atlases). Nothing outside src/text includes this.
//
// Every handle struct holds eqstd containers, so it is made with malloc and
// constructed in place, and destroyed by its destructor and free
// (CLAUDE.md, "Code conventions"). eqstd never throws; the containers that
// do (rbt_hash_index, msdfgen's) are caught at each entry point.

// ---- Errors -------------------------------------------------------------------

static inline void rbttext_clear_error(RBTTEXT_ERROR *error)
{
    if (error)
        error->message[0] = '\0';
}

// Records [format] in [error] (if any) and returns [result].
static inline RBT_RESULT rbttext_fail(RBTTEXT_ERROR *error, RBT_RESULT result, const char *format, ...)
{
    if (error)
    {
        va_list args;
        va_start(args, format);
        vsnprintf(error->message, sizeof(error->message), format, args);
        va_end(args);
    }
    return result;
}

static inline RBT_RESULT rbttext_out_of_memory(RBTTEXT_ERROR *error, const char *what)
{
    return rbttext_fail(error, RBT_RESULT_OUT_OF_MEMORY, "out of memory while %s", what);
}

// An array the caller left NULL although it says it has elements.
static inline bool rbttext_missing(const void *array, uint64_t count)
{
    return count != 0 && array == nullptr;
}

static inline bool rbttext_is_valid_codepoint(uint32_t codepoint)
{
    if (codepoint > 0x10FFFFu)
        return false;
    return codepoint < 0xD800u || codepoint > 0xDFFFu;
}

// ---- Faces --------------------------------------------------------------------

// A stroke face's glyph, as kept: its points and polylines are its own
// blocks of the face's arrays, and each polyline's first_point counts from
// the glyph's first point, so rbttext_face_get_glyph_strokes() hands both
// blocks out as they are.
struct rbttext_stroke_glyph
{
    uint32_t codepoint;
    float advance; // em
    uint32_t first_point;
    uint32_t point_count;
    uint32_t first_polyline;
    uint32_t polyline_count;
};
RBT_PLAIN_DATA(rbttext_stroke_glyph);
RBT_PLAIN_DATA(RBTTEXT_STROKE_POLYLINE);
RBT_PLAIN_DATA(RbtVec2);

// A stroke face's character map: sorted by codepoint.
struct rbttext_cmap_entry
{
    uint32_t codepoint;
    uint32_t glyph;
};
RBT_PLAIN_DATA(rbttext_cmap_entry);

struct rbttext_face_t
{
    const char *debug_name = nullptr;
    RBT_STALE_TAG
    RBTTEXT_FACE_KIND kind = RBTTEXT_FACE_KIND_OUTLINE;
    // What rbttext_face_get_metrics() gives, its base aside.
    RBTTEXT_FACE_METRICS metrics = {};
    // What rbttext_atlas_save() writes to recognise the face again: an
    // outline face's head table revision and checksum adjustment, and its
    // glyph count; a stroke face's CRC-32 of its arrays, its glyph count
    // and 0.
    uint32_t fingerprint[3] = {0, 0, 0};

    // An outline face: HarfBuzz's objects, each immutable once made, so the
    // calls that only read a face may overlap.
    hb_blob_t *blob = nullptr;
    hb_face_t *hb_face = nullptr;
    hb_font_t *font = nullptr; // scaled to units_per_em: positions in font units
    hb_draw_funcs_t *draw = nullptr;

    // A stroke face. Glyph id i is glyphs[i - 1]; id 0 is .notdef.
    eq::array<rbttext_stroke_glyph> glyphs;
    eq::array<RBTTEXT_STROKE_POLYLINE> polylines;
    eq::array<RbtVec2> points; // em, y down
    eq::array<rbttext_cmap_entry> cmap;

    ~rbttext_face_t();
};

// A single-stroke font as a reader has it: every glyph's advance and
// polylines, in em units, y up, before it becomes a face.
struct rbttext_stroke_font
{
    eq::array<RBTTEXT_STROKE_GLYPH> glyphs;
    eq::array<RBTTEXT_STROKE_POLYLINE> polylines;
    eq::array<RbtVec2> points;
};

// The CAD formats' readers (rbttext_cadfonts.cpp), on descs checked by the
// callers. The SHX reader gives the descender its file sets.
RBT_RESULT rbttext_read_lff(const RBTTEXT_LFF_FACE_DESC &desc, rbttext_stroke_font &font, RBTTEXT_ERROR *error);
RBT_RESULT rbttext_read_shx(const RBTTEXT_SHX_FACE_DESC &desc, rbttext_stroke_font &font, float *out_descender,
                         RBTTEXT_ERROR *error);

// The glyph [face] maps [codepoint] to through its character map alone,
// or 0 (with false) when it has none.
bool rbttext_face_lookup(const rbttext_face_t *face, uint32_t codepoint, uint32_t *out_glyph);

// A stroke face's .notdef advance, in em.
static constexpr float RBTTEXT_STROKE_NOTDEF_ADVANCE = 0.5f;

// ---- Chains -------------------------------------------------------------------

struct rbttext_chain_t
{
    const char *debug_name = nullptr;
    RBT_STALE_TAG
    RBTTEXT_FACE_KIND kind = RBTTEXT_FACE_KIND_OUTLINE;
    eq::array<rbttext_face> faces;
};

// ---- Layouts ------------------------------------------------------------------

// One glyph as a shaper gives it, before it is placed on a line: em units,
// y up for the offset, as HarfBuzz gives them.
struct rbttext_shaped
{
    uint32_t glyph;
    uint32_t cluster;     // the index of its cluster's first codepoint in the text
    uint32_t cluster_end; // the index just past its cluster's last
    uint32_t run;         // the layout's run it goes in
    double advance;
    double x_offset;
    double y_offset;
};
RBT_PLAIN_DATA(rbttext_shaped);

// A cluster of a paragraph, for breaking lines: codepoints [start, end).
struct rbttext_cluster
{
    uint32_t start;
    uint32_t end;
    double advance; // em, its glyphs' advances
    uint32_t whitespace; // 1 if every codepoint in it is a space that hangs
};
RBT_PLAIN_DATA(rbttext_cluster);

RBT_PLAIN_DATA(RBTTEXT_GLYPH);
RBT_PLAIN_DATA(RBTTEXT_LINE);
RBT_PLAIN_DATA(RBTTEXT_RUN);
RBT_PLAIN_DATA(hb_feature_t);

struct rbttext_layout_t
{
    const char *debug_name = nullptr;
    RBT_STALE_TAG
    // The chain of the last successful build; null while empty.
    rbttext_chain chain = nullptr;
    float em_size = 0.0f;
    uint32_t missing_codepoint_count = 0;
    uint32_t replaced_sequence_count = 0;
    float width = 0.0f;
    float height = 0.0f;

    eq::array<RBTTEXT_GLYPH> glyphs;
    eq::array<RBTTEXT_LINE> lines;
    eq::array<RBTTEXT_RUN> runs;

    // Scratch, kept between builds so a layout rebuilt every frame stops
    // allocating once it has grown. Indexed by codepoint unless said.
    eq::array<uint32_t> codepoints;
    eq::array<uint32_t> shaping_codepoints; // tab as a space
    eq::array<uint32_t> bidi_codepoints;    // see rbttext_layout.cpp
    eq::array<uint32_t> byte_offsets;       // codepoints + 1 entries
    eq::array<uint32_t> faces;              // the chain face each codepoint is drawn from
    eq::array<uint32_t> scripts;            // ISO 15924 tags
    eq::array<uint8_t> levels;              // the paragraph's bidi levels
    eq::array<char> breaks;                 // libunibreak's, the paragraph's
    eq::array<double> advances;             // per cluster start
    eq::array<uint8_t> cluster_starts;
    eq::array<rbttext_cluster> clusters;
    eq::array<rbttext_shaped> shaped;
    eq::array<hb_feature_t> features;
    eq::array<uint8_t> line_rtl; // per line: its paragraph is right to left
    eq::array<uint32_t> line_starts; // a paragraph's lines, as cluster indices
    hb_buffer_t *buffer = nullptr;
    // SheenBidi's script locator, made on first use and kept: a rebuild
    // allocates nothing for it (rbttext_layout.cpp).
    struct _SBScriptLocator *script_locator = nullptr;

    ~rbttext_layout_t();
    // Debug builds' check of rbtcore.h's "Threads" rule (../rbtoverlap.hpp).
    rbt_overlap overlap{};
};

// ---- Atlases ------------------------------------------------------------------

// The generator's version, saved with an atlas: another version may make
// other pixels from the same glyph. Raised whenever what
// rbttext_generate_glyph() writes changes (msdfgen 1.13, edge coloring,
// error correction, quantising).
static constexpr uint32_t RBTTEXT_GENERATOR_VERSION = 1;

// The largest image side rbttext_generate_glyph() makes, in pixels.
static constexpr uint32_t RBTTEXT_MAX_IMAGE_SIDE = 16384;

// One span of a page's skyline: the lowest free row over [x, x + width).
struct rbttext_skyline
{
    uint32_t x;
    uint32_t y;
    uint32_t width;
};
RBT_PLAIN_DATA(rbttext_skyline);

struct rbttext_page
{
    uint8_t *pixels = nullptr; // freed by the atlas
    eq::array<rbttext_skyline> skyline;
    // What changed since rbttext_atlas_take_dirty() last took it: [x0, x1)
    // by [y0, y1), when dirty is 1.
    uint32_t dirty = 0;
    uint32_t x0 = 0;
    uint32_t y0 = 0;
    uint32_t x1 = 0;
    uint32_t y1 = 0;
};

struct rbttext_atlas_entry
{
    rbttext_face face;
    uint32_t glyph;
    uint32_t page;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    float plane_left;
    float plane_top;
    float plane_right;
    float plane_bottom;
};
RBT_PLAIN_DATA(rbttext_atlas_entry);

struct rbttext_atlas_t
{
    const char *debug_name = nullptr;
    RBT_STALE_TAG
    RBTTEXT_ATLAS_DESC settings = {};
    eq::vector<rbttext_page> pages;
    eq::array<rbttext_atlas_entry> entries; // in the order they were inserted
    rbt_hash_index index;                 // face and glyph to entries' index

    ~rbttext_atlas_t();
    // Debug builds' check of rbtcore.h's "Threads" rule (../rbtoverlap.hpp).
    rbt_overlap overlap{};
};

// The bounds of one glyph's image, as rbttext_generate_glyph() makes it.
struct rbttext_glyph_bounds
{
    uint32_t width;
    uint32_t height;
    float plane_left;
    float plane_top;
    float plane_right;
    float plane_bottom;
};

// Measures (and, with [pixels], generates) one outline glyph whose
// arguments are checked. Throws std::bad_alloc.
RBT_RESULT rbttext_glyph_render(rbttext_face face, uint32_t glyph, double pixels_per_em, double distance_range,
                             uint8_t *pixels, rbttext_glyph_bounds *out_bounds, RBTTEXT_ERROR *error);

#endif // RBTTEXT_INTERNAL_HPP
