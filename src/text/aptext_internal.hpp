#ifndef APTEXT_INTERNAL_HPP
#define APTEXT_INTERNAL_HPP

#include "text/aptext.h"
#include "../aprihashindex.hpp"
#include "../apriplaindata.hpp"
#include <eqstd.hpp>
#include <hb-ot.h>
#include <hb.h>
#include <stdarg.h>
#include <stdio.h>

// ApText's objects, and what its three sources share: aptext.cpp (faces and
// chains), aptext_layout.cpp (layouts) and aptext_atlas.cpp (glyph
// generation and atlases). Nothing outside src/text includes this.
//
// Every handle struct holds eqstd containers, so it is made with malloc and
// constructed in place, and destroyed by its destructor and free
// (CLAUDE.md, "Code conventions"). eqstd never throws; the containers that
// do (apri_hash_index, msdfgen's) are caught at each entry point.

// ---- Errors -------------------------------------------------------------------

static inline void aptext_clear_error(APTEXT_ERROR *error)
{
    if (error)
        error->message[0] = '\0';
}

// Records [format] in [error] (if any) and returns [result].
static inline APRESULT aptext_fail(APTEXT_ERROR *error, APRESULT result, const char *format, ...)
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

static inline APRESULT aptext_out_of_memory(APTEXT_ERROR *error, const char *what)
{
    return aptext_fail(error, APRESULT_OUT_OF_MEMORY, "out of memory while %s", what);
}

// An array the caller left NULL although it says it has elements.
static inline bool aptext_missing(const void *array, uint64_t count)
{
    return count != 0 && array == nullptr;
}

static inline bool aptext_is_valid_codepoint(uint32_t codepoint)
{
    if (codepoint > 0x10FFFFu)
        return false;
    return codepoint < 0xD800u || codepoint > 0xDFFFu;
}

// ---- Faces --------------------------------------------------------------------

// A stroke face's glyph, as kept: its points and polylines are its own
// blocks of the face's arrays, and each polyline's first_point counts from
// the glyph's first point, so aptext_face_get_glyph_strokes() hands both
// blocks out as they are.
struct aptext_stroke_glyph
{
    uint32_t codepoint;
    float advance; // em
    uint32_t first_point;
    uint32_t point_count;
    uint32_t first_polyline;
    uint32_t polyline_count;
};
APRI_PLAIN_DATA(aptext_stroke_glyph);
APRI_PLAIN_DATA(APTEXT_STROKE_POLYLINE);
APRI_PLAIN_DATA(ApriVec2);

// A stroke face's character map: sorted by codepoint.
struct aptext_cmap_entry
{
    uint32_t codepoint;
    uint32_t glyph;
};
APRI_PLAIN_DATA(aptext_cmap_entry);

struct aptext_face_t
{
    const char *debug_name = nullptr;
    APTEXT_FACE_KIND kind = APTEXT_FACE_KIND_OUTLINE;
    // What aptext_face_get_metrics() gives, its base aside.
    APTEXT_FACE_METRICS metrics = {};
    // What aptext_atlas_save() writes to recognise the face again: an
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
    eq::array<aptext_stroke_glyph> glyphs;
    eq::array<APTEXT_STROKE_POLYLINE> polylines;
    eq::array<ApriVec2> points; // em, y down
    eq::array<aptext_cmap_entry> cmap;

    ~aptext_face_t();
};

// The glyph [face] maps [codepoint] to through its character map alone,
// or 0 (with false) when it has none.
bool aptext_face_lookup(const aptext_face_t *face, uint32_t codepoint, uint32_t *out_glyph);

// A stroke face's .notdef advance, in em.
static constexpr float APTEXT_STROKE_NOTDEF_ADVANCE = 0.5f;

// ---- Chains -------------------------------------------------------------------

struct aptext_chain_t
{
    const char *debug_name = nullptr;
    APTEXT_FACE_KIND kind = APTEXT_FACE_KIND_OUTLINE;
    eq::array<aptext_face> faces;
};

// ---- Layouts ------------------------------------------------------------------

// One glyph as a shaper gives it, before it is placed on a line: em units,
// y up for the offset, as HarfBuzz gives them.
struct aptext_shaped
{
    uint32_t glyph;
    uint32_t cluster;     // the index of its cluster's first codepoint in the text
    uint32_t cluster_end; // the index just past its cluster's last
    uint32_t run;         // the layout's run it goes in
    double advance;
    double x_offset;
    double y_offset;
};
APRI_PLAIN_DATA(aptext_shaped);

// A cluster of a paragraph, for breaking lines: codepoints [start, end).
struct aptext_cluster
{
    uint32_t start;
    uint32_t end;
    double advance; // em, its glyphs' advances
    uint32_t whitespace; // 1 if every codepoint in it is a space that hangs
};
APRI_PLAIN_DATA(aptext_cluster);

APRI_PLAIN_DATA(APTEXT_GLYPH);
APRI_PLAIN_DATA(APTEXT_LINE);
APRI_PLAIN_DATA(APTEXT_RUN);
APRI_PLAIN_DATA(hb_feature_t);

struct aptext_layout_t
{
    const char *debug_name = nullptr;
    // The chain of the last successful build; null while empty.
    aptext_chain chain = nullptr;
    float em_size = 0.0f;
    uint32_t missing_codepoint_count = 0;
    uint32_t replaced_sequence_count = 0;
    float width = 0.0f;
    float height = 0.0f;

    eq::array<APTEXT_GLYPH> glyphs;
    eq::array<APTEXT_LINE> lines;
    eq::array<APTEXT_RUN> runs;

    // Scratch, kept between builds so a layout rebuilt every frame stops
    // allocating once it has grown. Indexed by codepoint unless said.
    eq::array<uint32_t> codepoints;
    eq::array<uint32_t> shaping_codepoints; // tab as a space
    eq::array<uint32_t> bidi_codepoints;    // see aptext_layout.cpp
    eq::array<uint32_t> byte_offsets;       // codepoints + 1 entries
    eq::array<uint32_t> faces;              // the chain face each codepoint is drawn from
    eq::array<uint32_t> scripts;            // ISO 15924 tags
    eq::array<uint8_t> levels;              // the paragraph's bidi levels
    eq::array<char> breaks;                 // libunibreak's, the paragraph's
    eq::array<double> advances;             // per cluster start
    eq::array<uint8_t> cluster_starts;
    eq::array<aptext_cluster> clusters;
    eq::array<aptext_shaped> shaped;
    eq::array<hb_feature_t> features;
    eq::array<uint8_t> line_rtl; // per line: its paragraph is right to left
    eq::array<uint32_t> line_starts; // a paragraph's lines, as cluster indices
    hb_buffer_t *buffer = nullptr;

    ~aptext_layout_t();
};

// ---- Atlases ------------------------------------------------------------------

// The generator's version, saved with an atlas: another version may make
// other pixels from the same glyph. Raised whenever what
// aptext_generate_glyph() writes changes (msdfgen 1.13, edge coloring,
// error correction, quantising).
static constexpr uint32_t APTEXT_GENERATOR_VERSION = 1;

// The largest image side aptext_generate_glyph() makes, in pixels.
static constexpr uint32_t APTEXT_MAX_IMAGE_SIDE = 16384;

// One span of a page's skyline: the lowest free row over [x, x + width).
struct aptext_skyline
{
    uint32_t x;
    uint32_t y;
    uint32_t width;
};
APRI_PLAIN_DATA(aptext_skyline);

struct aptext_page
{
    uint8_t *pixels = nullptr; // freed by the atlas
    eq::array<aptext_skyline> skyline;
    // What changed since aptext_atlas_take_dirty() last took it: [x0, x1)
    // by [y0, y1), when dirty is 1.
    uint32_t dirty = 0;
    uint32_t x0 = 0;
    uint32_t y0 = 0;
    uint32_t x1 = 0;
    uint32_t y1 = 0;
};

struct aptext_atlas_entry
{
    aptext_face face;
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
APRI_PLAIN_DATA(aptext_atlas_entry);

struct aptext_atlas_t
{
    const char *debug_name = nullptr;
    APTEXT_ATLAS_DESC settings = {};
    eq::vector<aptext_page> pages;
    eq::array<aptext_atlas_entry> entries; // in the order they were inserted
    apri_hash_index index;                 // face and glyph to entries' index

    ~aptext_atlas_t();
};

// The bounds of one glyph's image, as aptext_generate_glyph() makes it.
struct aptext_glyph_bounds
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
APRESULT aptext_glyph_render(aptext_face face, uint32_t glyph, double pixels_per_em, double distance_range,
                             uint8_t *pixels, aptext_glyph_bounds *out_bounds, APTEXT_ERROR *error);

#endif // APTEXT_INTERNAL_HPP
