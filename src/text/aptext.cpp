#include "aptext_internal.hpp"
#include "../apristructbase.hpp"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

// ApText's contract pass (APTEXT_TODO.md): every entry point checks its
// arguments, line by line, and its struct bases, then returns
// APRESULT_NOT_IMPLEMENTED. No dependency is vendored yet (HarfBuzz,
// msdfgen, SheenBidi, libunibreak). The implementation pass fills each call
// in behind the same checks and leaves tests/aptext_tests.c as it is.

// ---- Errors -------------------------------------------------------------------

static void clear_error(APTEXT_ERROR *error)
{
    if (error)
        error->message[0] = '\0';
}

// Records [format] in [error] (if any) and returns [result].
static APRESULT fail(APTEXT_ERROR *error, APRESULT result, const char *format, ...)
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

// What every call returns once its arguments pass, until it is built.
static APRESULT not_implemented(APTEXT_ERROR *error, const char *call)
{
    return fail(error, APRESULT_NOT_IMPLEMENTED,
                "%s is not implemented yet: ApText is a contract only, with no dependency vendored (APTEXT_TODO.md)",
                call);
}

// ---- Argument helpers -----------------------------------------------------------

// An array the caller left NULL although it says it has elements.
static bool missing(const void *array, uint64_t count)
{
    return count != 0 && array == nullptr;
}

static bool is_valid_codepoint(uint32_t codepoint)
{
    if (codepoint > 0x10FFFFu)
        return false;
    return codepoint < 0xD800u || codepoint > 0xDFFFu;
}

// The checks of APTEXT_STROKE_METRICS, for both stroke face descs.
static APRESULT check_stroke_metrics(const APTEXT_STROKE_METRICS &m, APTEXT_ERROR *error)
{
    if (!isfinite(m.ascender))
        return fail(error, APRESULT_INVALID_ARGUMENT, "metrics.ascender is not finite");
    if (!isfinite(m.descender))
        return fail(error, APRESULT_INVALID_ARGUMENT, "metrics.descender is not finite");
    if (m.ascender <= m.descender)
        return fail(error, APRESULT_INVALID_ARGUMENT, "metrics.ascender is not above metrics.descender");
    if (!isfinite(m.line_gap))
        return fail(error, APRESULT_INVALID_ARGUMENT, "metrics.line_gap is not finite");
    if (m.line_gap < 0.0f)
        return fail(error, APRESULT_INVALID_ARGUMENT, "metrics.line_gap is negative");
    return APRESULT_OK;
}

// The checks of an APTEXT_ATLAS_DESC, for aptext_atlas_create() and
// aptext_atlas_load(); [name] is what the message calls it.
static APRESULT check_atlas_desc(const APTEXT_ATLAS_DESC *desc, const char *name, APTEXT_ERROR *error)
{
    if (APRESULT result = apri_struct_check(&desc->base, APRI_STRUCT_TYPE_APTEXT_ATLAS_DESC))
        return fail(error, result, "%s %s", name, apri_struct_check_message(result));
    const APTEXT_ATLAS_DESC &d = *desc;
    if (!isfinite(d.pixels_per_em))
        return fail(error, APRESULT_INVALID_ARGUMENT, "%s's pixels_per_em is not finite", name);
    if (d.pixels_per_em <= 0.0f)
        return fail(error, APRESULT_INVALID_ARGUMENT, "%s's pixels_per_em is not above 0", name);
    if (!isfinite(d.distance_range))
        return fail(error, APRESULT_INVALID_ARGUMENT, "%s's distance_range is not finite", name);
    if (d.distance_range <= 0.0f)
        return fail(error, APRESULT_INVALID_ARGUMENT, "%s's distance_range is not above 0", name);
    if (d.page_width == 0)
        return fail(error, APRESULT_INVALID_ARGUMENT, "%s's page_width is 0", name);
    if (d.page_height == 0)
        return fail(error, APRESULT_INVALID_ARGUMENT, "%s's page_height is 0", name);
    double cell = (double)d.pixels_per_em + (double)d.distance_range;
    if ((double)d.page_width < cell)
        return fail(error, APRESULT_INVALID_ARGUMENT, "%s's page_width is less than pixels_per_em plus distance_range",
                    name);
    if ((double)d.page_height < cell)
        return fail(error, APRESULT_INVALID_ARGUMENT,
                    "%s's page_height is less than pixels_per_em plus distance_range", name);
    if (d.max_pages == 0)
        return fail(error, APRESULT_INVALID_ARGUMENT, "%s's max_pages is 0", name);
    return APRESULT_OK;
}

// The checks of an APTEXT_GLYPH_GENERATE_DESC's fields, its base checked.
static APRESULT check_generate_desc(const APTEXT_GLYPH_GENERATE_DESC &d, APTEXT_ERROR *error)
{
    if (!d.face)
        return fail(error, APRESULT_INVALID_ARGUMENT, "desc's face is null");
    if (!isfinite(d.pixels_per_em))
        return fail(error, APRESULT_INVALID_ARGUMENT, "desc's pixels_per_em is not finite");
    if (d.pixels_per_em <= 0.0f)
        return fail(error, APRESULT_INVALID_ARGUMENT, "desc's pixels_per_em is not above 0");
    if (!isfinite(d.distance_range))
        return fail(error, APRESULT_INVALID_ARGUMENT, "desc's distance_range is not finite");
    if (d.distance_range <= 0.0f)
        return fail(error, APRESULT_INVALID_ARGUMENT, "desc's distance_range is not above 0");
    return APRESULT_OK;
}

// Whether [tag] is a feature APTEXT_FEATURE_FLAGS controls.
static bool is_flag_feature(uint32_t tag)
{
    if (tag == APTEXT_TAG('k', 'e', 'r', 'n'))
        return true;
    if (tag == APTEXT_TAG('l', 'i', 'g', 'a'))
        return true;
    if (tag == APTEXT_TAG('c', 'l', 'i', 'g'))
        return true;
    return tag == APTEXT_TAG('c', 'a', 'l', 't');
}

static const uint32_t ALL_FEATURE_FLAGS =
    APTEXT_FEATURE_KERNING | APTEXT_FEATURE_LIGATURES | APTEXT_FEATURE_CONTEXTUAL_ALTERNATES;

extern "C"
{
    // ---- Faces ------------------------------------------------------------------

    APRESULT aptext_face_create(const APTEXT_FACE_DESC *desc, aptext_face *out_face, APTEXT_ERROR *out_error)
    {
        clear_error(out_error);
        if (!out_face)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "out_face is null");
        *out_face = nullptr;
        if (!desc)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is null");
        if (APRESULT result = apri_struct_check(&desc->base, APRI_STRUCT_TYPE_APTEXT_FACE_DESC))
            return fail(out_error, result, "desc %s", apri_struct_check_message(result));
        const APTEXT_FACE_DESC &d = *desc;
        if (!d.data)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's data is null");
        if (d.data_size == 0)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's data_size is 0");
        if (d.data_size > 0xFFFFFFFFull)
            return fail(out_error, APRESULT_INVALID_ARGUMENT,
                        "desc's data_size is over 4 GiB, more than a font's 32-bit offsets address");
        return not_implemented(out_error, "aptext_face_create");
    }

    APRESULT aptext_face_create_stroke(const APTEXT_STROKE_FACE_DESC *desc, aptext_face *out_face,
                                       APTEXT_ERROR *out_error)
    {
        clear_error(out_error);
        if (!out_face)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "out_face is null");
        *out_face = nullptr;
        if (!desc)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is null");
        if (APRESULT result = apri_struct_check(&desc->base, APRI_STRUCT_TYPE_APTEXT_STROKE_FACE_DESC))
            return fail(out_error, result, "desc %s", apri_struct_check_message(result));
        const APTEXT_STROKE_FACE_DESC &d = *desc;
        if (!d.glyphs)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's glyphs is null");
        if (d.glyph_count == 0)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's glyph_count is 0");
        if (missing(d.polylines, d.polyline_count))
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's polylines is null with a polyline_count of %u",
                        d.polyline_count);
        if (missing(d.points, d.point_count))
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's points is null with a point_count of %u",
                        d.point_count);
        for (uint32_t i = 0; i < d.glyph_count; i++)
        {
            const APTEXT_STROKE_GLYPH &g = d.glyphs[i];
            if (!is_valid_codepoint(g.codepoint))
                return fail(out_error, APRESULT_INVALID_ARGUMENT, "glyphs[%u].codepoint 0x%X is not a codepoint", i,
                            g.codepoint);
            if (!isfinite(g.advance))
                return fail(out_error, APRESULT_INVALID_ARGUMENT, "glyphs[%u].advance is not finite", i);
            if (g.advance < 0.0f)
                return fail(out_error, APRESULT_INVALID_ARGUMENT, "glyphs[%u].advance is negative", i);
            if (g.first_polyline > d.polyline_count)
                return fail(out_error, APRESULT_INVALID_ARGUMENT, "glyphs[%u].first_polyline is past polylines", i);
            if (g.polyline_count > d.polyline_count - g.first_polyline)
                return fail(out_error, APRESULT_INVALID_ARGUMENT, "glyphs[%u]'s polylines run past polylines", i);
        }
        for (uint32_t i = 0; i < d.polyline_count; i++)
        {
            const APTEXT_STROKE_POLYLINE &p = d.polylines[i];
            if (p.point_count < 2)
                return fail(out_error, APRESULT_INVALID_ARGUMENT, "polylines[%u] has fewer than 2 points", i);
            if (p.first_point > d.point_count)
                return fail(out_error, APRESULT_INVALID_ARGUMENT, "polylines[%u].first_point is past points", i);
            if (p.point_count > d.point_count - p.first_point)
                return fail(out_error, APRESULT_INVALID_ARGUMENT, "polylines[%u]'s points run past points", i);
        }
        for (uint32_t i = 0; i < d.point_count; i++)
        {
            if (!isfinite(d.points[i].x))
                return fail(out_error, APRESULT_INVALID_ARGUMENT, "points[%u].x is not finite", i);
            if (!isfinite(d.points[i].y))
                return fail(out_error, APRESULT_INVALID_ARGUMENT, "points[%u].y is not finite", i);
        }
        if (APRESULT result = check_stroke_metrics(d.metrics, out_error))
            return result;
        // Two glyphs with one codepoint are refused by the implementation,
        // which sorts a copy of the glyphs anyway.
        return not_implemented(out_error, "aptext_face_create_stroke");
    }

    APRESULT aptext_face_create_hershey(const APTEXT_HERSHEY_FACE_DESC *desc, aptext_face *out_face,
                                        APTEXT_ERROR *out_error)
    {
        clear_error(out_error);
        if (!out_face)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "out_face is null");
        *out_face = nullptr;
        if (!desc)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is null");
        if (APRESULT result = apri_struct_check(&desc->base, APRI_STRUCT_TYPE_APTEXT_HERSHEY_FACE_DESC))
            return fail(out_error, result, "desc %s", apri_struct_check_message(result));
        const APTEXT_HERSHEY_FACE_DESC &d = *desc;
        if (!d.data)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's data is null");
        if (d.data_size == 0)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's data_size is 0");
        if (missing(d.codepoints, d.codepoint_count))
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's codepoints is null with a codepoint_count of %u",
                        d.codepoint_count);
        if (d.codepoints)
        {
            if (d.codepoint_count == 0)
                return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's codepoint_count is 0 with codepoints given");
            for (uint32_t i = 0; i < d.codepoint_count; i++)
            {
                if (!is_valid_codepoint(d.codepoints[i]))
                    return fail(out_error, APRESULT_INVALID_ARGUMENT, "codepoints[%u] 0x%X is not a codepoint", i,
                                d.codepoints[i]);
            }
        }
        else
        {
            if (!is_valid_codepoint(d.first_codepoint))
                return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's first_codepoint 0x%X is not a codepoint",
                            d.first_codepoint);
        }
        if (!isfinite(d.units_per_em))
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's units_per_em is not finite");
        if (d.units_per_em <= 0.0f)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's units_per_em is not above 0");
        if (!isfinite(d.baseline))
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's baseline is not finite");
        if (APRESULT result = check_stroke_metrics(d.metrics, out_error))
            return result;
        return not_implemented(out_error, "aptext_face_create_hershey");
    }

    APRESULT aptext_face_get_metrics(aptext_face face, APTEXT_FACE_METRICS *out_metrics)
    {
        if (!face)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_metrics)
            return APRESULT_INVALID_ARGUMENT;
        if (APRESULT result = apri_struct_out_check(&out_metrics->base, APRI_STRUCT_TYPE_APTEXT_FACE_METRICS))
            return result;
        return APRESULT_NOT_IMPLEMENTED;
    }

    APRESULT aptext_face_find_glyph(aptext_face face, uint32_t codepoint, uint32_t *out_glyph)
    {
        (void)codepoint;
        if (!face)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_glyph)
            return APRESULT_INVALID_ARGUMENT;
        *out_glyph = 0;
        return APRESULT_NOT_IMPLEMENTED;
    }

    APRESULT aptext_face_get_glyph_strokes(aptext_face face, uint32_t glyph, const ApriVec2 **out_points,
                                           uint32_t *out_point_count, const APTEXT_STROKE_POLYLINE **out_polylines,
                                           uint32_t *out_polyline_count)
    {
        (void)glyph;
        if (!face)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_points)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_point_count)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_polylines)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_polyline_count)
            return APRESULT_INVALID_ARGUMENT;
        *out_points = nullptr;
        *out_point_count = 0;
        *out_polylines = nullptr;
        *out_polyline_count = 0;
        return APRESULT_NOT_IMPLEMENTED;
    }

    void aptext_face_destroy(aptext_face face)
    {
        if (!face)
            return;
        free(face);
    }

    // ---- Fallback chains --------------------------------------------------------

    APRESULT aptext_chain_create(const APTEXT_CHAIN_DESC *desc, aptext_chain *out_chain, APTEXT_ERROR *out_error)
    {
        clear_error(out_error);
        if (!out_chain)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "out_chain is null");
        *out_chain = nullptr;
        if (!desc)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is null");
        if (APRESULT result = apri_struct_check(&desc->base, APRI_STRUCT_TYPE_APTEXT_CHAIN_DESC))
            return fail(out_error, result, "desc %s", apri_struct_check_message(result));
        const APTEXT_CHAIN_DESC &d = *desc;
        if (!d.faces)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's faces is null");
        if (d.face_count == 0)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's face_count is 0");
        for (uint32_t i = 0; i < d.face_count; i++)
        {
            if (!d.faces[i])
                return fail(out_error, APRESULT_INVALID_ARGUMENT, "faces[%u] is null", i);
        }
        // A chain is a handful of faces: comparing each pair is cheaper
        // than anything that allocates.
        for (uint32_t i = 1; i < d.face_count; i++)
        {
            for (uint32_t j = 0; j < i; j++)
            {
                if (d.faces[i] == d.faces[j])
                    return fail(out_error, APRESULT_INVALID_ARGUMENT, "faces[%u] is faces[%u] again", i, j);
            }
        }
        // Faces of more than one kind are refused by the implementation,
        // which reads each face's kind.
        return not_implemented(out_error, "aptext_chain_create");
    }

    aptext_face aptext_chain_get_face(aptext_chain chain, uint32_t index)
    {
        // No chain can exist yet, so there is no face to return.
        (void)chain;
        (void)index;
        return nullptr;
    }

    uint32_t aptext_chain_get_face_count(aptext_chain chain)
    {
        (void)chain;
        return 0;
    }

    APRESULT aptext_chain_find_glyph(aptext_chain chain, uint32_t codepoint, uint32_t *out_face_index,
                                     uint32_t *out_glyph)
    {
        (void)codepoint;
        if (!chain)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_face_index)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_glyph)
            return APRESULT_INVALID_ARGUMENT;
        *out_face_index = 0;
        *out_glyph = 0;
        return APRESULT_NOT_IMPLEMENTED;
    }

    void aptext_chain_destroy(aptext_chain chain)
    {
        if (!chain)
            return;
        free(chain);
    }

    // ---- Layout -----------------------------------------------------------------

    APRESULT aptext_layout_create(aptext_layout *out_layout)
    {
        if (!out_layout)
            return APRESULT_INVALID_ARGUMENT;
        *out_layout = nullptr;
        return APRESULT_NOT_IMPLEMENTED;
    }

    APRESULT aptext_layout_build(aptext_layout layout, const APTEXT_LAYOUT_DESC *desc, APTEXT_ERROR *out_error)
    {
        clear_error(out_error);
        if (!layout)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "layout is null");
        if (!desc)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is null");
        if (APRESULT result = apri_struct_check(&desc->base, APRI_STRUCT_TYPE_APTEXT_LAYOUT_DESC))
            return fail(out_error, result, "desc %s", apri_struct_check_message(result));
        const APTEXT_LAYOUT_DESC &d = *desc;
        if (!d.chain)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's chain is null");
        if (missing(d.text, d.text_size))
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's text is null with a text_size of %u",
                        d.text_size);
        if (d.direction < APTEXT_DIRECTION_LTR)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's direction %u is not an APTEXT_DIRECTION",
                        (unsigned)d.direction);
        if (d.direction > APTEXT_DIRECTION_AUTO)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's direction %u is not an APTEXT_DIRECTION",
                        (unsigned)d.direction);
        if ((d.features & ~ALL_FEATURE_FLAGS) != 0)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's features has bits 0x%X, which are no feature",
                        d.features & ~ALL_FEATURE_FLAGS);
        if (missing(d.extra_features, d.extra_feature_count))
            return fail(out_error, APRESULT_INVALID_ARGUMENT,
                        "desc's extra_features is null with an extra_feature_count of %u", d.extra_feature_count);
        for (uint32_t i = 0; i < d.extra_feature_count; i++)
        {
            if (is_flag_feature(d.extra_features[i].tag))
                return fail(out_error, APRESULT_INVALID_ARGUMENT,
                            "extra_features[%u] is a feature desc's features controls", i);
            for (uint32_t j = 0; j < i; j++)
            {
                if (d.extra_features[i].tag == d.extra_features[j].tag)
                    return fail(out_error, APRESULT_INVALID_ARGUMENT,
                                "extra_features[%u] has the tag of extra_features[%u]", i, j);
            }
        }
        if (!isfinite(d.em_size))
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's em_size is not finite");
        if (d.em_size <= 0.0f)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's em_size is not above 0");
        if (!isfinite(d.max_width))
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's max_width is not finite");
        if (d.max_width < 0.0f)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's max_width is negative");
        if (d.align < APTEXT_ALIGN_LEFT)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's align %u is not an APTEXT_ALIGN",
                        (unsigned)d.align);
        if (d.align > APTEXT_ALIGN_END)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's align %u is not an APTEXT_ALIGN",
                        (unsigned)d.align);
        if (!isfinite(d.line_spacing))
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's line_spacing is not finite");
        if (d.line_spacing <= 0.0f)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's line_spacing is not above 0");
        if (!isfinite(d.letter_spacing))
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's letter_spacing is not finite");
        return not_implemented(out_error, "aptext_layout_build");
    }

    APRESULT aptext_layout_get_info(aptext_layout layout, APTEXT_LAYOUT_INFO *out_info)
    {
        if (!layout)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_info)
            return APRESULT_INVALID_ARGUMENT;
        if (APRESULT result = apri_struct_out_check(&out_info->base, APRI_STRUCT_TYPE_APTEXT_LAYOUT_INFO))
            return result;
        return APRESULT_NOT_IMPLEMENTED;
    }

    APRESULT aptext_layout_get_glyphs(aptext_layout layout, const APTEXT_GLYPH **out_glyphs, uint32_t *out_count)
    {
        if (!layout)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_glyphs)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_count)
            return APRESULT_INVALID_ARGUMENT;
        *out_glyphs = nullptr;
        *out_count = 0;
        return APRESULT_NOT_IMPLEMENTED;
    }

    APRESULT aptext_layout_get_lines(aptext_layout layout, const APTEXT_LINE **out_lines, uint32_t *out_count)
    {
        if (!layout)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_lines)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_count)
            return APRESULT_INVALID_ARGUMENT;
        *out_lines = nullptr;
        *out_count = 0;
        return APRESULT_NOT_IMPLEMENTED;
    }

    APRESULT aptext_layout_get_runs(aptext_layout layout, const APTEXT_RUN **out_runs, uint32_t *out_count)
    {
        if (!layout)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_runs)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_count)
            return APRESULT_INVALID_ARGUMENT;
        *out_runs = nullptr;
        *out_count = 0;
        return APRESULT_NOT_IMPLEMENTED;
    }

    APRESULT aptext_layout_get_strokes(aptext_layout layout, APTEXT_SEGMENT *segments, uint32_t capacity,
                                       uint32_t *out_count)
    {
        if (!layout)
            return APRESULT_INVALID_ARGUMENT;
        if (missing(segments, capacity))
            return APRESULT_INVALID_ARGUMENT;
        if (!out_count)
            return APRESULT_INVALID_ARGUMENT;
        *out_count = 0;
        return APRESULT_NOT_IMPLEMENTED;
    }

    void aptext_layout_destroy(aptext_layout layout)
    {
        if (!layout)
            return;
        free(layout);
    }

    // ---- Glyph generation -------------------------------------------------------

    APRESULT aptext_generate_glyph(const APTEXT_GLYPH_GENERATE_DESC *desc, void *pixels, uint64_t pixels_size,
                                   APTEXT_GLYPH_IMAGE *out_image, APTEXT_ERROR *out_error)
    {
        clear_error(out_error);
        if (!desc)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is null");
        if (APRESULT result = apri_struct_check(&desc->base, APRI_STRUCT_TYPE_APTEXT_GLYPH_GENERATE_DESC))
            return fail(out_error, result, "desc %s", apri_struct_check_message(result));
        if (APRESULT result = check_generate_desc(*desc, out_error))
            return result;
        if (missing(pixels, pixels_size))
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "pixels is null with a pixels_size of %llu",
                        (unsigned long long)pixels_size);
        if (!out_image)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "out_image is null");
        if (APRESULT result = apri_struct_out_check(&out_image->base, APRI_STRUCT_TYPE_APTEXT_GLYPH_IMAGE))
            return fail(out_error, result, "out_image %s", apri_struct_check_message(result));
        return not_implemented(out_error, "aptext_generate_glyph");
    }

    // ---- Atlas ------------------------------------------------------------------

    APRESULT aptext_atlas_create(const APTEXT_ATLAS_DESC *desc, aptext_atlas *out_atlas, APTEXT_ERROR *out_error)
    {
        clear_error(out_error);
        if (!out_atlas)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "out_atlas is null");
        *out_atlas = nullptr;
        if (!desc)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is null");
        if (APRESULT result = check_atlas_desc(desc, "desc", out_error))
            return result;
        return not_implemented(out_error, "aptext_atlas_create");
    }

    APRESULT aptext_atlas_get_info(aptext_atlas atlas, APTEXT_ATLAS_INFO *out_info)
    {
        if (!atlas)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_info)
            return APRESULT_INVALID_ARGUMENT;
        if (APRESULT result = apri_struct_out_check(&out_info->base, APRI_STRUCT_TYPE_APTEXT_ATLAS_INFO))
            return result;
        return APRESULT_NOT_IMPLEMENTED;
    }

    APRESULT aptext_atlas_insert(aptext_atlas atlas, const APTEXT_GLYPH_GENERATE_DESC *desc, const void *pixels,
                                 uint64_t pixels_size, APTEXT_ATLAS_GLYPH *out_glyph, APTEXT_ERROR *out_error)
    {
        clear_error(out_error);
        if (!atlas)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "atlas is null");
        if (!desc)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is null");
        if (APRESULT result = apri_struct_check(&desc->base, APRI_STRUCT_TYPE_APTEXT_GLYPH_GENERATE_DESC))
            return fail(out_error, result, "desc %s", apri_struct_check_message(result));
        if (APRESULT result = check_generate_desc(*desc, out_error))
            return result;
        if (missing(pixels, pixels_size))
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "pixels is null with a pixels_size of %llu",
                        (unsigned long long)pixels_size);
        if (out_glyph)
        {
            if (APRESULT result = apri_struct_out_check(&out_glyph->base, APRI_STRUCT_TYPE_APTEXT_ATLAS_GLYPH))
                return fail(out_error, result, "out_glyph %s", apri_struct_check_message(result));
        }
        return not_implemented(out_error, "aptext_atlas_insert");
    }

    APRESULT aptext_atlas_add_layout(aptext_atlas atlas, aptext_layout layout, APTEXT_ERROR *out_error)
    {
        clear_error(out_error);
        if (!atlas)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "atlas is null");
        if (!layout)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "layout is null");
        return not_implemented(out_error, "aptext_atlas_add_layout");
    }

    APRESULT aptext_atlas_find_glyph(aptext_atlas atlas, aptext_face face, uint32_t glyph,
                                     APTEXT_ATLAS_GLYPH *out_glyph)
    {
        (void)glyph;
        if (!atlas)
            return APRESULT_INVALID_ARGUMENT;
        if (!face)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_glyph)
            return APRESULT_INVALID_ARGUMENT;
        if (APRESULT result = apri_struct_out_check(&out_glyph->base, APRI_STRUCT_TYPE_APTEXT_ATLAS_GLYPH))
            return result;
        return APRESULT_NOT_IMPLEMENTED;
    }

    APRESULT aptext_atlas_get_page(aptext_atlas atlas, uint32_t page, const void **out_pixels)
    {
        (void)page;
        if (!atlas)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_pixels)
            return APRESULT_INVALID_ARGUMENT;
        *out_pixels = nullptr;
        return APRESULT_NOT_IMPLEMENTED;
    }

    APRESULT aptext_atlas_take_dirty(aptext_atlas atlas, APTEXT_DIRTY_RECT *rects, uint32_t capacity,
                                     uint32_t *out_count)
    {
        if (!atlas)
            return APRESULT_INVALID_ARGUMENT;
        if (missing(rects, capacity))
            return APRESULT_INVALID_ARGUMENT;
        if (!out_count)
            return APRESULT_INVALID_ARGUMENT;
        *out_count = 0;
        return APRESULT_NOT_IMPLEMENTED;
    }

    APRESULT aptext_atlas_save(aptext_atlas atlas, const aptext_face *faces, uint32_t face_count, void *buffer,
                               uint64_t capacity, uint64_t *out_size, APTEXT_ERROR *out_error)
    {
        clear_error(out_error);
        if (!atlas)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "atlas is null");
        if (missing(faces, face_count))
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "faces is null with a face_count of %u", face_count);
        for (uint32_t i = 0; i < face_count; i++)
        {
            if (!faces[i])
                return fail(out_error, APRESULT_INVALID_ARGUMENT, "faces[%u] is null", i);
            for (uint32_t j = 0; j < i; j++)
            {
                if (faces[i] == faces[j])
                    return fail(out_error, APRESULT_INVALID_ARGUMENT, "faces[%u] is faces[%u] again", i, j);
            }
        }
        if (missing(buffer, capacity))
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "buffer is null with a capacity of %llu",
                        (unsigned long long)capacity);
        if (!out_size)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "out_size is null");
        *out_size = 0;
        return not_implemented(out_error, "aptext_atlas_save");
    }

    APRESULT aptext_atlas_load(const APTEXT_ATLAS_LOAD_DESC *desc, aptext_atlas *out_atlas, APTEXT_ERROR *out_error)
    {
        clear_error(out_error);
        if (!out_atlas)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "out_atlas is null");
        *out_atlas = nullptr;
        if (!desc)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is null");
        if (APRESULT result = apri_struct_check(&desc->base, APRI_STRUCT_TYPE_APTEXT_ATLAS_LOAD_DESC))
            return fail(out_error, result, "desc %s", apri_struct_check_message(result));
        const APTEXT_ATLAS_LOAD_DESC &d = *desc;
        if (!d.atlas)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's atlas is null");
        if (APRESULT result = check_atlas_desc(d.atlas, "desc's atlas", out_error))
            return result;
        if (!d.data)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's data is null");
        if (d.data_size == 0)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's data_size is 0");
        if (missing(d.faces, d.face_count))
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's faces is null with a face_count of %u",
                        d.face_count);
        for (uint32_t i = 0; i < d.face_count; i++)
        {
            if (!d.faces[i])
                return fail(out_error, APRESULT_INVALID_ARGUMENT, "faces[%u] is null", i);
        }
        return not_implemented(out_error, "aptext_atlas_load");
    }

    void aptext_atlas_destroy(aptext_atlas atlas)
    {
        if (!atlas)
            return;
        free(atlas);
    }
}
