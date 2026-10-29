#include "aptext_internal.hpp"
#include "../apristructbase.hpp"
#include <algorithm>
#include <libdeflate.h>
#include <math.h>
#include <new>
#include <stdlib.h>
#include <string.h>

// ApText's faces and fallback chains (APTEXT_TODO.md). An outline face is
// HarfBuzz's blob, face and font over the caller's bytes; a stroke face is
// arrays of its own. Layouts are in aptext_layout.cpp, glyph generation and
// atlases in aptext_atlas.cpp.

// ---- Objects ------------------------------------------------------------------

aptext_face_t::~aptext_face_t()
{
    if (draw)
        hb_draw_funcs_destroy(draw);
    if (font)
        hb_font_destroy(font);
    if (hb_face)
        hb_face_destroy(hb_face);
    if (blob)
        hb_blob_destroy(blob);
}

static aptext_face_t *new_face(void)
{
    aptext_face_t *face = (aptext_face_t *)malloc(sizeof(aptext_face_t));
    if (!face)
        return nullptr;
    return new (face) aptext_face_t();
}

static void delete_face(aptext_face_t *face)
{
    face->~aptext_face_t();
    free(face);
}

bool aptext_face_lookup(const aptext_face_t *face, uint32_t codepoint, uint32_t *out_glyph)
{
    *out_glyph = 0;
    if (face->kind == APTEXT_FACE_KIND_OUTLINE)
    {
        hb_codepoint_t glyph = 0;
        if (!hb_font_get_nominal_glyph(face->font, codepoint, &glyph))
            return false;
        *out_glyph = glyph;
        return true;
    }
    const aptext_cmap_entry *begin = face->cmap.begin();
    const aptext_cmap_entry *end = face->cmap.end();
    const aptext_cmap_entry *at = std::lower_bound(
        begin, end, codepoint, [](const aptext_cmap_entry &entry, uint32_t value) { return entry.codepoint < value; });
    if (at == end)
        return false;
    if (at->codepoint != codepoint)
        return false;
    *out_glyph = at->glyph;
    return true;
}

// ---- Outline faces ------------------------------------------------------------

static uint32_t read_u32(const uint8_t *data, uint32_t at)
{
    return ((uint32_t)data[at] << 24) | ((uint32_t)data[at + 1] << 16) | ((uint32_t)data[at + 2] << 8) |
           (uint32_t)data[at + 3];
}

// [value] font units as em, through double so a binary fraction stays exact.
static float to_em(double value, uint32_t units_per_em)
{
    return (float)(value / (double)units_per_em);
}

static float metric_or_zero(hb_font_t *font, hb_ot_metrics_tag_t tag, uint32_t units_per_em)
{
    hb_position_t position = 0;
    if (!hb_ot_metrics_get_position(font, tag, &position))
        return 0.0f;
    return to_em(position, units_per_em);
}

// HarfBuzz's draw callbacks, which aptext_atlas.cpp gives a shape to.
hb_draw_funcs_t *aptext_create_draw_funcs(void);

// Fills an outline face from its blob: HarfBuzz's face and font, metrics
// and fingerprint. [face]'s blob is set.
static APRESULT open_outline(aptext_face_t *face, uint32_t collection_index, APTEXT_ERROR *error)
{
    unsigned int count = hb_face_count(face->blob);
    if (count == 0)
        return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT,
                           "the bytes are not an OpenType or TrueType font, or a collection of them");
    if (collection_index >= count)
        return aptext_fail(error, APRESULT_NOT_FOUND, "collection_index %u is past the file's last font (%u)",
                           collection_index, count - 1);
    face->hb_face = hb_face_create(face->blob, collection_index);
    if (face->hb_face == hb_face_get_empty())
    {
        face->hb_face = nullptr;
        return aptext_out_of_memory(error, "reading the font");
    }
    hb_face_make_immutable(face->hb_face);

    hb_blob_t *head = hb_face_reference_table(face->hb_face, HB_TAG('h', 'e', 'a', 'd'));
    unsigned int head_size = 0;
    const uint8_t *head_data = (const uint8_t *)hb_blob_get_data(head, &head_size);
    if (head_size < 54)
    {
        hb_blob_destroy(head);
        return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "the font has no head table, or one cut short");
    }
    face->fingerprint[0] = read_u32(head_data, 4); // fontRevision
    face->fingerprint[1] = read_u32(head_data, 8); // checkSumAdjustment
    hb_blob_destroy(head);

    uint32_t glyph_count = hb_face_get_glyph_count(face->hb_face);
    if (glyph_count == 0)
        return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "the font has no glyphs (no maxp table, or one cut short)");
    uint32_t units_per_em = hb_face_get_upem(face->hb_face);
    face->fingerprint[2] = glyph_count;

    face->font = hb_font_create(face->hb_face);
    if (face->font == hb_font_get_empty())
    {
        face->font = nullptr;
        return aptext_out_of_memory(error, "reading the font");
    }
    hb_font_set_scale(face->font, (int)units_per_em, (int)units_per_em);
    hb_font_make_immutable(face->font);
    face->draw = aptext_create_draw_funcs();
    if (!face->draw)
        return aptext_out_of_memory(error, "reading the font");

    hb_font_extents_t extents = {};
    hb_font_get_h_extents(face->font, &extents);
    APTEXT_FACE_METRICS &m = face->metrics;
    m.kind = APTEXT_FACE_KIND_OUTLINE;
    m.units_per_em = units_per_em;
    m.glyph_count = glyph_count;
    m.ascender = to_em(extents.ascender, units_per_em);
    m.descender = to_em(extents.descender, units_per_em);
    m.line_gap = to_em(extents.line_gap, units_per_em);
    m.underline_position = metric_or_zero(face->font, HB_OT_METRICS_TAG_UNDERLINE_OFFSET, units_per_em);
    m.underline_thickness = metric_or_zero(face->font, HB_OT_METRICS_TAG_UNDERLINE_SIZE, units_per_em);
    m.strikeout_position = metric_or_zero(face->font, HB_OT_METRICS_TAG_STRIKEOUT_OFFSET, units_per_em);
    m.strikeout_thickness = metric_or_zero(face->font, HB_OT_METRICS_TAG_STRIKEOUT_SIZE, units_per_em);
    m.cap_height = metric_or_zero(face->font, HB_OT_METRICS_TAG_CAP_HEIGHT, units_per_em);
    m.x_height = metric_or_zero(face->font, HB_OT_METRICS_TAG_X_HEIGHT, units_per_em);
    return APRESULT_OK;
}

// ---- Stroke faces -------------------------------------------------------------

// One glyph of a stroke face as a reader has it: polylines indexing points
// anywhere in the reader's arrays.
struct stroke_source
{
    const APTEXT_STROKE_GLYPH *glyphs;
    uint32_t glyph_count;
    const APTEXT_STROKE_POLYLINE *polylines;
    const ApriVec2 *points; // em, y up
};

static void stroke_metrics(aptext_face_t *face, const APTEXT_STROKE_METRICS &metrics, uint32_t glyph_count)
{
    APTEXT_FACE_METRICS &m = face->metrics;
    m.kind = APTEXT_FACE_KIND_STROKE;
    m.units_per_em = 0;
    m.glyph_count = glyph_count + 1;
    m.ascender = metrics.ascender;
    m.descender = metrics.descender;
    m.line_gap = metrics.line_gap;
}

// Copies [source] into [face]'s arrays, each glyph's points and polylines
// a block of their own, with y turned down; then sorts the character map
// and refuses two glyphs with one codepoint.
static APRESULT fill_stroke(aptext_face_t *face, const stroke_source &source, APTEXT_ERROR *error)
{
    face->kind = APTEXT_FACE_KIND_STROKE;
    if (face->glyphs.reserve(source.glyph_count) != EQ_SUCCESS)
        return aptext_out_of_memory(error, "copying the glyphs");
    if (face->cmap.reserve(source.glyph_count) != EQ_SUCCESS)
        return aptext_out_of_memory(error, "copying the glyphs");
    for (uint32_t i = 0; i < source.glyph_count; i++)
    {
        const APTEXT_STROKE_GLYPH &g = source.glyphs[i];
        aptext_stroke_glyph kept;
        kept.codepoint = g.codepoint;
        kept.advance = g.advance;
        kept.first_point = (uint32_t)face->points.count;
        kept.point_count = 0;
        kept.first_polyline = (uint32_t)face->polylines.count;
        kept.polyline_count = g.polyline_count;
        for (uint32_t p = 0; p < g.polyline_count; p++)
        {
            const APTEXT_STROKE_POLYLINE &line = source.polylines[g.first_polyline + p];
            APTEXT_STROKE_POLYLINE copy;
            copy.first_point = kept.point_count;
            copy.point_count = line.point_count;
            if (face->polylines.push(copy) != EQ_SUCCESS)
                return aptext_out_of_memory(error, "copying the polylines");
            for (uint32_t q = 0; q < line.point_count; q++)
            {
                ApriVec2 point = source.points[line.first_point + q];
                point.y = -point.y;
                if (face->points.push(point) != EQ_SUCCESS)
                    return aptext_out_of_memory(error, "copying the points");
            }
            kept.point_count += line.point_count;
        }
        if (face->glyphs.push(kept) != EQ_SUCCESS)
            return aptext_out_of_memory(error, "copying the glyphs");
        aptext_cmap_entry entry = {g.codepoint, i + 1};
        if (face->cmap.push(entry) != EQ_SUCCESS)
            return aptext_out_of_memory(error, "copying the glyphs");
    }
    std::sort(face->cmap.begin(), face->cmap.end(), [](const aptext_cmap_entry &a, const aptext_cmap_entry &b) {
        if (a.codepoint != b.codepoint)
            return a.codepoint < b.codepoint;
        return a.glyph < b.glyph;
    });
    for (size_t i = 1; i < face->cmap.count; i++)
    {
        if (face->cmap[i].codepoint == face->cmap[i - 1].codepoint)
            return aptext_fail(error, APRESULT_INVALID_ARGUMENT, "glyphs[%u] and glyphs[%u] both draw U+%04X",
                               face->cmap[i - 1].glyph - 1, face->cmap[i].glyph - 1, face->cmap[i].codepoint);
    }
    // The fingerprint: a CRC-32 of what was kept, which is the same bytes
    // on every platform (floats as their bits, little-endian or not, since
    // the CRC runs over each value's bytes in a fixed order).
    uint32_t crc = 0;
    for (const aptext_stroke_glyph &g : face->glyphs)
    {
        uint32_t values[6];
        memcpy(&values[1], &g.advance, sizeof(float));
        values[0] = g.codepoint;
        values[2] = g.first_point;
        values[3] = g.point_count;
        values[4] = g.first_polyline;
        values[5] = g.polyline_count;
        for (uint32_t v : values)
        {
            uint8_t bytes[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
            crc = libdeflate_crc32(crc, bytes, 4);
        }
    }
    for (const ApriVec2 &point : face->points)
    {
        uint32_t values[2];
        memcpy(&values[0], &point.x, sizeof(float));
        memcpy(&values[1], &point.y, sizeof(float));
        for (uint32_t v : values)
        {
            uint8_t bytes[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
            crc = libdeflate_crc32(crc, bytes, 4);
        }
    }
    face->fingerprint[0] = crc;
    face->fingerprint[1] = source.glyph_count + 1;
    face->fingerprint[2] = 0;
    return APRESULT_OK;
}

// ---- Hershey fonts --------------------------------------------------------------

// A Hershey file read: every glyph's advance and polylines, in em, y up.
struct hershey_font
{
    eq::array<APTEXT_STROKE_GLYPH> glyphs;
    eq::array<APTEXT_STROKE_POLYLINE> polylines;
    eq::array<ApriVec2> points;
};
APRI_PLAIN_DATA(APTEXT_STROKE_GLYPH);

static bool is_line_end(uint8_t c)
{
    return c == '\n' || c == '\r';
}

// Skips one line end (LF, or CR LF, or a CR alone) at [at].
static uint64_t skip_line_end(const uint8_t *data, uint64_t size, uint64_t at)
{
    if (at < size && data[at] == '\r')
        at++;
    if (at < size && data[at] == '\n')
        at++;
    return at;
}

// Ends the polyline being read, if any: one of a single point is made a
// zero-length line, so a dot still draws.
static EQ_RESULT end_hershey_polyline(hershey_font &font, uint32_t first_point)
{
    uint32_t count = (uint32_t)font.points.count - first_point;
    if (count == 0)
        return EQ_SUCCESS;
    if (count == 1)
    {
        ApriVec2 dot = font.points[first_point];
        if (EQ_RESULT result = font.points.push(dot))
            return result;
        count = 2;
    }
    APTEXT_STROKE_POLYLINE line = {first_point, count};
    return font.polylines.push(line);
}

static APRESULT read_hershey(const APTEXT_HERSHEY_FACE_DESC &d, hershey_font &font, APTEXT_ERROR *error)
{
    const uint8_t *data = (const uint8_t *)d.data;
    const uint64_t size = d.data_size;
    const double scale = 1.0 / (double)d.units_per_em;
    uint64_t at = 0;
    uint32_t line_number = 1;
    while (at < size)
    {
        // Blank lines between and after glyphs are skipped.
        uint64_t blank = at;
        while (blank < size && (data[blank] == ' ' || data[blank] == '\t'))
            blank++;
        if (blank == size)
            break;
        if (is_line_end(data[blank]))
        {
            at = skip_line_end(data, size, blank);
            line_number++;
            continue;
        }
        // Columns 1-5, the glyph's number, and 6-8, its vertex count:
        // digits after spaces.
        if (size - at < 8)
            return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "line %u is too short for a Hershey glyph",
                               line_number);
        for (uint32_t column = 0; column < 5; column++)
        {
            uint8_t c = data[at + column];
            if (c == ' ')
                continue;
            if (c < '0' || c > '9')
                return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT,
                                   "line %u: column %u is not a digit of a Hershey glyph's number", line_number,
                                   column + 1);
        }
        uint32_t vertex_count = 0;
        bool digits = false;
        for (uint32_t column = 5; column < 8; column++)
        {
            uint8_t c = data[at + column];
            if (c == ' ' && !digits)
                continue;
            if (c < '0' || c > '9')
                return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT,
                                   "line %u: column %u is not a digit of a Hershey glyph's vertex count", line_number,
                                   column + 1);
            vertex_count = vertex_count * 10 + (uint32_t)(c - '0');
            digits = true;
        }
        if (vertex_count == 0)
            return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "line %u: a Hershey glyph with no vertices",
                               line_number);
        at += 8;

        // The pairs, which may run on over several lines.
        APTEXT_STROKE_GLYPH glyph = {};
        glyph.first_polyline = (uint32_t)font.polylines.count;
        int left = 0;
        uint32_t polyline_start = (uint32_t)font.points.count;
        for (uint32_t v = 0; v < vertex_count; v++)
        {
            while (at < size && is_line_end(data[at]))
            {
                at = skip_line_end(data, size, at);
                line_number++;
            }
            if (size - at < 2)
                return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT,
                                   "line %u: the text ends inside a Hershey glyph's coordinates", line_number);
            uint8_t cx = data[at];
            uint8_t cy = data[at + 1];
            at += 2;
            if (cx < ' ' || cx > '~' || cy < ' ' || cy > '~')
                return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT,
                                   "line %u: a Hershey coordinate that isn't a printable character", line_number);
            int x = (int)cx - 'R';
            int y = (int)cy - 'R';
            if (v == 0)
            {
                left = x;
                glyph.advance = (float)((double)(y - x) * scale);
                if (glyph.advance < 0.0f)
                    return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT,
                                       "line %u: a Hershey glyph whose right bound is left of its left", line_number);
                continue;
            }
            if (cx == ' ' && cy == 'R')
            {
                if (end_hershey_polyline(font, polyline_start) != EQ_SUCCESS)
                    return aptext_out_of_memory(error, "reading the Hershey font");
                polyline_start = (uint32_t)font.points.count;
                continue;
            }
            ApriVec2 point;
            point.x = (float)((double)(x - left) * scale);
            point.y = (float)(((double)d.baseline - (double)y) * scale);
            if (font.points.push(point) != EQ_SUCCESS)
                return aptext_out_of_memory(error, "reading the Hershey font");
        }
        if (end_hershey_polyline(font, polyline_start) != EQ_SUCCESS)
            return aptext_out_of_memory(error, "reading the Hershey font");
        glyph.polyline_count = (uint32_t)font.polylines.count - glyph.first_polyline;
        // What follows the last pair: trailing spaces, then the line's end.
        while (at < size && data[at] == ' ')
            at++;
        if (at < size && !is_line_end(data[at]))
            return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT,
                               "line %u: more than the glyph's vertex count of coordinates", line_number);
        at = skip_line_end(data, size, at);
        line_number++;
        if (font.glyphs.push(glyph) != EQ_SUCCESS)
            return aptext_out_of_memory(error, "reading the Hershey font");
    }
    if (font.glyphs.count == 0)
        return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "the text holds no Hershey glyph");
    return APRESULT_OK;
}

extern "C"
{
    // ---- Faces ------------------------------------------------------------------

    APRESULT aptext_face_create(const APTEXT_FACE_DESC *desc, aptext_face *out_face, APTEXT_ERROR *out_error)
    {
        aptext_clear_error(out_error);
        if (!out_face)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_face is null");
        *out_face = nullptr;
        if (!desc)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is null");
        if (APRESULT result = apri_struct_check(&desc->base, APRI_STRUCT_TYPE_APTEXT_FACE_DESC))
            return aptext_fail(out_error, result, "desc %s", apri_struct_check_message(result));
        const APTEXT_FACE_DESC &d = *desc;
        if (!d.data)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's data is null");
        if (d.data_size == 0)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's data_size is 0");
        if (d.data_size > 0xFFFFFFFFull)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT,
                               "desc's data_size is over 4 GiB, more than a font's 32-bit offsets address");

        aptext_face_t *face = new_face();
        if (!face)
            return aptext_out_of_memory(out_error, "creating the face");
        // HarfBuzz borrows the bytes, or copies them (copy_data); a blob it
        // couldn't make is its empty one.
        hb_memory_mode_t mode = d.copy_data ? HB_MEMORY_MODE_DUPLICATE : HB_MEMORY_MODE_READONLY;
        face->blob = hb_blob_create((const char *)d.data, (unsigned int)d.data_size, mode, nullptr, nullptr);
        if (face->blob == hb_blob_get_empty())
        {
            face->blob = nullptr;
            delete_face(face);
            return aptext_out_of_memory(out_error, "creating the face");
        }
        hb_blob_make_immutable(face->blob);
        if (APRESULT result = open_outline(face, d.collection_index, out_error))
        {
            delete_face(face);
            return result;
        }
        *out_face = face;
        return APRESULT_OK;
    }

    APRESULT aptext_face_create_stroke(const APTEXT_STROKE_FACE_DESC *desc, aptext_face *out_face,
                                       APTEXT_ERROR *out_error)
    {
        aptext_clear_error(out_error);
        if (!out_face)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_face is null");
        *out_face = nullptr;
        if (!desc)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is null");
        if (APRESULT result = apri_struct_check(&desc->base, APRI_STRUCT_TYPE_APTEXT_STROKE_FACE_DESC))
            return aptext_fail(out_error, result, "desc %s", apri_struct_check_message(result));
        const APTEXT_STROKE_FACE_DESC &d = *desc;
        if (!d.glyphs)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's glyphs is null");
        if (d.glyph_count == 0)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's glyph_count is 0");
        if (aptext_missing(d.polylines, d.polyline_count))
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT,
                               "desc's polylines is null with a polyline_count of %u", d.polyline_count);
        if (aptext_missing(d.points, d.point_count))
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's points is null with a point_count of %u",
                               d.point_count);
        for (uint32_t i = 0; i < d.glyph_count; i++)
        {
            const APTEXT_STROKE_GLYPH &g = d.glyphs[i];
            if (!aptext_is_valid_codepoint(g.codepoint))
                return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "glyphs[%u].codepoint 0x%X is not a codepoint",
                                   i, g.codepoint);
            if (!isfinite(g.advance))
                return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "glyphs[%u].advance is not finite", i);
            if (g.advance < 0.0f)
                return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "glyphs[%u].advance is negative", i);
            if (g.first_polyline > d.polyline_count)
                return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "glyphs[%u].first_polyline is past polylines",
                                   i);
            if (g.polyline_count > d.polyline_count - g.first_polyline)
                return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "glyphs[%u]'s polylines run past polylines",
                                   i);
        }
        for (uint32_t i = 0; i < d.polyline_count; i++)
        {
            const APTEXT_STROKE_POLYLINE &p = d.polylines[i];
            if (p.point_count < 2)
                return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "polylines[%u] has fewer than 2 points", i);
            if (p.first_point > d.point_count)
                return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "polylines[%u].first_point is past points", i);
            if (p.point_count > d.point_count - p.first_point)
                return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "polylines[%u]'s points run past points", i);
        }
        for (uint32_t i = 0; i < d.point_count; i++)
        {
            if (!isfinite(d.points[i].x))
                return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "points[%u].x is not finite", i);
            if (!isfinite(d.points[i].y))
                return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "points[%u].y is not finite", i);
        }
        if (!isfinite(d.metrics.ascender))
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "metrics.ascender is not finite");
        if (!isfinite(d.metrics.descender))
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "metrics.descender is not finite");
        if (d.metrics.ascender <= d.metrics.descender)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "metrics.ascender is not above metrics.descender");
        if (!isfinite(d.metrics.line_gap))
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "metrics.line_gap is not finite");
        if (d.metrics.line_gap < 0.0f)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "metrics.line_gap is negative");

        aptext_face_t *face = new_face();
        if (!face)
            return aptext_out_of_memory(out_error, "creating the face");
        stroke_source source = {d.glyphs, d.glyph_count, d.polylines, d.points};
        if (APRESULT result = fill_stroke(face, source, out_error))
        {
            delete_face(face);
            return result;
        }
        stroke_metrics(face, d.metrics, d.glyph_count);
        *out_face = face;
        return APRESULT_OK;
    }

    APRESULT aptext_face_create_hershey(const APTEXT_HERSHEY_FACE_DESC *desc, aptext_face *out_face,
                                        APTEXT_ERROR *out_error)
    {
        aptext_clear_error(out_error);
        if (!out_face)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_face is null");
        *out_face = nullptr;
        if (!desc)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is null");
        if (APRESULT result = apri_struct_check(&desc->base, APRI_STRUCT_TYPE_APTEXT_HERSHEY_FACE_DESC))
            return aptext_fail(out_error, result, "desc %s", apri_struct_check_message(result));
        const APTEXT_HERSHEY_FACE_DESC &d = *desc;
        if (!d.data)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's data is null");
        if (d.data_size == 0)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's data_size is 0");
        if (aptext_missing(d.codepoints, d.codepoint_count))
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT,
                               "desc's codepoints is null with a codepoint_count of %u", d.codepoint_count);
        if (d.codepoints)
        {
            if (d.codepoint_count == 0)
                return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT,
                                   "desc's codepoint_count is 0 with codepoints given");
            for (uint32_t i = 0; i < d.codepoint_count; i++)
            {
                if (!aptext_is_valid_codepoint(d.codepoints[i]))
                    return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "codepoints[%u] 0x%X is not a codepoint",
                                       i, d.codepoints[i]);
            }
        }
        else
        {
            if (!aptext_is_valid_codepoint(d.first_codepoint))
                return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT,
                                   "desc's first_codepoint 0x%X is not a codepoint", d.first_codepoint);
        }
        if (!isfinite(d.units_per_em))
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's units_per_em is not finite");
        if (d.units_per_em <= 0.0f)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's units_per_em is not above 0");
        if (!isfinite(d.baseline))
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's baseline is not finite");
        if (!isfinite(d.metrics.ascender))
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "metrics.ascender is not finite");
        if (!isfinite(d.metrics.descender))
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "metrics.descender is not finite");
        if (d.metrics.ascender <= d.metrics.descender)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "metrics.ascender is not above metrics.descender");
        if (!isfinite(d.metrics.line_gap))
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "metrics.line_gap is not finite");
        if (d.metrics.line_gap < 0.0f)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "metrics.line_gap is negative");

        hershey_font font;
        if (APRESULT result = read_hershey(d, font, out_error))
            return result;
        uint32_t glyph_count = (uint32_t)font.glyphs.count;
        if (d.codepoints && d.codepoint_count != glyph_count)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT,
                               "desc's codepoint_count is %u, and the file has %u glyphs", d.codepoint_count,
                               glyph_count);
        for (uint32_t i = 0; i < glyph_count; i++)
        {
            uint64_t codepoint = d.codepoints ? d.codepoints[i] : (uint64_t)d.first_codepoint + i;
            if (codepoint > 0x10FFFFu || !aptext_is_valid_codepoint((uint32_t)codepoint))
                return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT,
                                   "the file's glyph %u would draw 0x%llX, which is not a codepoint", i,
                                   (unsigned long long)codepoint);
            font.glyphs[i].codepoint = (uint32_t)codepoint;
        }

        aptext_face_t *face = new_face();
        if (!face)
            return aptext_out_of_memory(out_error, "creating the face");
        stroke_source source = {font.glyphs.data, glyph_count, font.polylines.data, font.points.data};
        if (APRESULT result = fill_stroke(face, source, out_error))
        {
            delete_face(face);
            return result;
        }
        stroke_metrics(face, d.metrics, glyph_count);
        *out_face = face;
        return APRESULT_OK;
    }

    APRESULT aptext_face_get_metrics(aptext_face face, APTEXT_FACE_METRICS *out_metrics)
    {
        if (!face)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_metrics)
            return APRESULT_INVALID_ARGUMENT;
        if (APRESULT result = apri_struct_out_check(&out_metrics->base, APRI_STRUCT_TYPE_APTEXT_FACE_METRICS))
            return result;
        APRI_STRUCT_BASE_OUT base = out_metrics->base;
        *out_metrics = face->metrics;
        out_metrics->base = base;
        return APRESULT_OK;
    }

    APRESULT aptext_face_find_glyph(aptext_face face, uint32_t codepoint, uint32_t *out_glyph)
    {
        if (!face)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_glyph)
            return APRESULT_INVALID_ARGUMENT;
        if (!aptext_face_lookup(face, codepoint, out_glyph))
            return APRESULT_NOT_FOUND;
        return APRESULT_OK;
    }

    APRESULT aptext_face_get_glyph_strokes(aptext_face face, uint32_t glyph, const ApriVec2 **out_points,
                                           uint32_t *out_point_count, const APTEXT_STROKE_POLYLINE **out_polylines,
                                           uint32_t *out_polyline_count)
    {
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
        if (face->kind != APTEXT_FACE_KIND_STROKE)
            return APRESULT_INVALID_ARGUMENT;
        if (glyph > face->glyphs.count)
            return APRESULT_NOT_FOUND;
        if (glyph == 0)
            return APRESULT_OK; // .notdef draws nothing
        const aptext_stroke_glyph &g = face->glyphs[glyph - 1];
        if (g.point_count)
            *out_points = face->points.data + g.first_point;
        *out_point_count = g.point_count;
        if (g.polyline_count)
            *out_polylines = face->polylines.data + g.first_polyline;
        *out_polyline_count = g.polyline_count;
        return APRESULT_OK;
    }

    void aptext_face_destroy(aptext_face face)
    {
        if (!face)
            return;
        delete_face(face);
    }

    // ---- Fallback chains --------------------------------------------------------

    APRESULT aptext_chain_create(const APTEXT_CHAIN_DESC *desc, aptext_chain *out_chain, APTEXT_ERROR *out_error)
    {
        aptext_clear_error(out_error);
        if (!out_chain)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_chain is null");
        *out_chain = nullptr;
        if (!desc)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is null");
        if (APRESULT result = apri_struct_check(&desc->base, APRI_STRUCT_TYPE_APTEXT_CHAIN_DESC))
            return aptext_fail(out_error, result, "desc %s", apri_struct_check_message(result));
        const APTEXT_CHAIN_DESC &d = *desc;
        if (!d.faces)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's faces is null");
        if (d.face_count == 0)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's face_count is 0");
        for (uint32_t i = 0; i < d.face_count; i++)
        {
            if (!d.faces[i])
                return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "faces[%u] is null", i);
        }
        // A chain is a handful of faces: comparing each pair is cheaper
        // than anything that allocates.
        for (uint32_t i = 1; i < d.face_count; i++)
        {
            for (uint32_t j = 0; j < i; j++)
            {
                if (d.faces[i] == d.faces[j])
                    return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "faces[%u] is faces[%u] again", i, j);
            }
        }
        for (uint32_t i = 1; i < d.face_count; i++)
        {
            if (d.faces[i]->kind != d.faces[0]->kind)
                return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT,
                                   "faces[%u] is a %s face and faces[0] a %s one: a chain's faces are of one kind", i,
                                   d.faces[i]->kind == APTEXT_FACE_KIND_STROKE ? "stroke" : "outline",
                                   d.faces[0]->kind == APTEXT_FACE_KIND_STROKE ? "stroke" : "outline");
        }

        aptext_chain_t *chain = (aptext_chain_t *)malloc(sizeof(aptext_chain_t));
        if (!chain)
            return aptext_out_of_memory(out_error, "creating the chain");
        chain = new (chain) aptext_chain_t();
        chain->kind = d.faces[0]->kind;
        if (chain->faces.append(d.faces, d.face_count) != EQ_SUCCESS)
        {
            chain->~aptext_chain_t();
            free(chain);
            return aptext_out_of_memory(out_error, "creating the chain");
        }
        *out_chain = chain;
        return APRESULT_OK;
    }

    aptext_face aptext_chain_get_face(aptext_chain chain, uint32_t index)
    {
        if (!chain)
            return nullptr;
        if (index >= chain->faces.count)
            return nullptr;
        return chain->faces[index];
    }

    uint32_t aptext_chain_get_face_count(aptext_chain chain)
    {
        if (!chain)
            return 0;
        return (uint32_t)chain->faces.count;
    }

    APRESULT aptext_chain_find_glyph(aptext_chain chain, uint32_t codepoint, uint32_t *out_face_index,
                                     uint32_t *out_glyph)
    {
        if (!chain)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_face_index)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_glyph)
            return APRESULT_INVALID_ARGUMENT;
        *out_face_index = 0;
        *out_glyph = 0;
        for (uint32_t i = 0; i < chain->faces.count; i++)
        {
            uint32_t glyph = 0;
            if (aptext_face_lookup(chain->faces[i], codepoint, &glyph))
            {
                *out_face_index = i;
                *out_glyph = glyph;
                return APRESULT_OK;
            }
        }
        return APRESULT_NOT_FOUND;
    }

    void aptext_chain_destroy(aptext_chain chain)
    {
        if (!chain)
            return;
        chain->~aptext_chain_t();
        free(chain);
    }
}
