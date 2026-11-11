#include "rbttext_internal.hpp"
#include "../rbtstructbase.hpp"
#include <algorithm>
#include <libdeflate.h>
#include <math.h>
#include <new>
#include <stdlib.h>
#include <string.h>

// Rabbittus Text's faces and fallback chains (RBTTEXT_TODO.md). An outline face is
// HarfBuzz's blob, face and font over the caller's bytes; a stroke face is
// arrays of its own. Layouts are in rbttext_layout.cpp, glyph generation and
// atlases in rbttext_atlas.cpp.

// ---- Objects ------------------------------------------------------------------

rbttext_face_t::~rbttext_face_t()
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

static rbttext_face_t *new_face(void)
{
    rbttext_face_t *face = (rbttext_face_t *)malloc(sizeof(rbttext_face_t));
    if (!face)
        return nullptr;
    return new (face) rbttext_face_t();
}

static void delete_face(rbttext_face_t *face)
{
    face->~rbttext_face_t();
    free(face);
}

bool rbttext_face_lookup(const rbttext_face_t *face, uint32_t codepoint, uint32_t *out_glyph)
{
    *out_glyph = 0;
    if (face->kind == RBTTEXT_FACE_KIND_OUTLINE)
    {
        hb_codepoint_t glyph = 0;
        if (!hb_font_get_nominal_glyph(face->font, codepoint, &glyph))
            return false;
        *out_glyph = glyph;
        return true;
    }
    const rbttext_cmap_entry *begin = face->cmap.begin();
    const rbttext_cmap_entry *end = face->cmap.end();
    const rbttext_cmap_entry *at = std::lower_bound(
        begin, end, codepoint, [](const rbttext_cmap_entry &entry, uint32_t value) { return entry.codepoint < value; });
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

// HarfBuzz's draw callbacks, which rbttext_atlas.cpp gives a shape to.
hb_draw_funcs_t *rbttext_create_draw_funcs(void);

// Fills an outline face from its blob: HarfBuzz's face and font, metrics
// and fingerprint. [face]'s blob is set.
// Sets [face]'s font on [variations] (checked to be finite by the caller):
// each a distinct axis of the font, within its range. The fingerprint's
// third word takes a CRC-32 of them in tag order, so a saved atlas is
// never taken for another instance; with none it stays the glyph count.
static RBT_RESULT set_variations(rbttext_face_t *face, const RBTTEXT_VARIATION *variations, uint32_t count, RBTTEXT_ERROR *error)
{
    if (count == 0)
        return RBT_RESULT_OK;
    unsigned int axis_count = hb_ot_var_get_axis_count(face->hb_face);
    eq::array<hb_ot_var_axis_info_t> axes;
    eq::array<hb_variation_t> set;
    if (axes.resize(axis_count) != EQ_SUCCESS)
        return rbttext_out_of_memory(error, "reading the font's axes");
    if (set.reserve(count) != EQ_SUCCESS)
        return rbttext_out_of_memory(error, "setting the variations");
    unsigned int got = axis_count;
    if (axis_count)
        hb_ot_var_get_axis_infos(face->hb_face, 0, &got, axes.data);
    for (uint32_t i = 0; i < count; i++)
    {
        const RBTTEXT_VARIATION &v = variations[i];
        const hb_ot_var_axis_info_t *axis = nullptr;
        for (unsigned int a = 0; a < got; a++)
            if (axes[a].tag == v.tag)
                axis = &axes[a];
        if (!axis)
            return rbttext_fail(error, RBT_RESULT_INVALID_ARGUMENT, "variations[%u]: the font has no axis '%c%c%c%c'", i,
                               (char)(v.tag >> 24), (char)(v.tag >> 16), (char)(v.tag >> 8), (char)v.tag);
        for (uint32_t k = 0; k < i; k++)
            if (variations[k].tag == v.tag)
                return rbttext_fail(error, RBT_RESULT_INVALID_ARGUMENT, "variations[%u] and [%u] set one axis", k, i);
        if (v.value < axis->min_value || v.value > axis->max_value)
            return rbttext_fail(error, RBT_RESULT_INVALID_ARGUMENT, "variations[%u]: %g is outside the axis's %g to %g", i,
                               (double)v.value, (double)axis->min_value, (double)axis->max_value);
        hb_variation_t h = {v.tag, v.value};
        if (set.push(h) != EQ_SUCCESS)
            return rbttext_out_of_memory(error, "setting the variations");
    }
    hb_font_set_variations(face->font, set.data, (unsigned int)set.count);
    std::sort(set.begin(), set.end(), [](const hb_variation_t &a, const hb_variation_t &b) { return a.tag < b.tag; });
    uint32_t crc = 0;
    for (const hb_variation_t &h : set)
    {
        uint32_t value_bits;
        memcpy(&value_bits, &h.value, sizeof(value_bits));
        const uint32_t words[2] = {h.tag, value_bits};
        for (uint32_t w : words)
        {
            uint8_t bytes[4] = {(uint8_t)w, (uint8_t)(w >> 8), (uint8_t)(w >> 16), (uint8_t)(w >> 24)};
            crc = libdeflate_crc32(crc, bytes, 4);
        }
    }
    face->fingerprint[2] ^= crc | 0x80000000u;
    return RBT_RESULT_OK;
}

static RBT_RESULT open_outline(rbttext_face_t *face, uint32_t collection_index, const RBTTEXT_VARIATION *variations,
                             uint32_t variation_count, RBTTEXT_ERROR *error)
{
    unsigned int count = hb_face_count(face->blob);
    if (count == 0)
        return rbttext_fail(error, RBT_RESULT_TEXT_NOT_A_FONT,
                           "the bytes are not an OpenType or TrueType font, or a collection of them");
    if (collection_index >= count)
        return rbttext_fail(error, RBT_RESULT_NOT_FOUND, "collection_index %u is past the file's last font (%u)",
                           collection_index, count - 1);
    face->hb_face = hb_face_create(face->blob, collection_index);
    if (face->hb_face == hb_face_get_empty())
    {
        face->hb_face = nullptr;
        return rbttext_out_of_memory(error, "reading the font");
    }
    hb_face_make_immutable(face->hb_face);

    hb_blob_t *head = hb_face_reference_table(face->hb_face, HB_TAG('h', 'e', 'a', 'd'));
    unsigned int head_size = 0;
    const uint8_t *head_data = (const uint8_t *)hb_blob_get_data(head, &head_size);
    if (head_size < 54)
    {
        hb_blob_destroy(head);
        return rbttext_fail(error, RBT_RESULT_TEXT_NOT_A_FONT, "the font has no head table, or one cut short");
    }
    face->fingerprint[0] = read_u32(head_data, 4); // fontRevision
    face->fingerprint[1] = read_u32(head_data, 8); // checkSumAdjustment
    hb_blob_destroy(head);

    uint32_t glyph_count = hb_face_get_glyph_count(face->hb_face);
    if (glyph_count == 0)
        return rbttext_fail(error, RBT_RESULT_TEXT_NOT_A_FONT, "the font has no glyphs (no maxp table, or one cut short)");
    uint32_t units_per_em = hb_face_get_upem(face->hb_face);
    face->fingerprint[2] = glyph_count;

    face->font = hb_font_create(face->hb_face);
    if (face->font == hb_font_get_empty())
    {
        face->font = nullptr;
        return rbttext_out_of_memory(error, "reading the font");
    }
    hb_font_set_scale(face->font, (int)units_per_em, (int)units_per_em);
    if (RBT_RESULT result = set_variations(face, variations, variation_count, error))
        return result;
    hb_font_make_immutable(face->font);
    face->draw = rbttext_create_draw_funcs();
    if (!face->draw)
        return rbttext_out_of_memory(error, "reading the font");

    hb_font_extents_t extents = {};
    hb_font_get_h_extents(face->font, &extents);
    RBTTEXT_FACE_METRICS &m = face->metrics;
    m.kind = RBTTEXT_FACE_KIND_OUTLINE;
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
    return RBT_RESULT_OK;
}

// ---- Stroke faces -------------------------------------------------------------

// One glyph of a stroke face as a reader has it: polylines indexing points
// anywhere in the reader's arrays.
struct stroke_source
{
    const RBTTEXT_STROKE_GLYPH *glyphs;
    uint32_t glyph_count;
    const RBTTEXT_STROKE_POLYLINE *polylines;
    const RbtVec2 *points; // em, y up
};

static void stroke_metrics(rbttext_face_t *face, const RBTTEXT_STROKE_METRICS &metrics, uint32_t glyph_count)
{
    RBTTEXT_FACE_METRICS &m = face->metrics;
    m.kind = RBTTEXT_FACE_KIND_STROKE;
    m.units_per_em = 0;
    m.glyph_count = glyph_count + 1;
    m.ascender = metrics.ascender;
    m.descender = metrics.descender;
    m.line_gap = metrics.line_gap;
}

// Copies [source] into [face]'s arrays, each glyph's points and polylines
// a block of their own, with y turned down; then sorts the character map
// and refuses two glyphs with one codepoint.
static RBT_RESULT fill_stroke(rbttext_face_t *face, const stroke_source &source, RBTTEXT_ERROR *error)
{
    face->kind = RBTTEXT_FACE_KIND_STROKE;
    if (face->glyphs.reserve(source.glyph_count) != EQ_SUCCESS)
        return rbttext_out_of_memory(error, "copying the glyphs");
    if (face->cmap.reserve(source.glyph_count) != EQ_SUCCESS)
        return rbttext_out_of_memory(error, "copying the glyphs");
    for (uint32_t i = 0; i < source.glyph_count; i++)
    {
        const RBTTEXT_STROKE_GLYPH &g = source.glyphs[i];
        rbttext_stroke_glyph kept;
        kept.codepoint = g.codepoint;
        kept.advance = g.advance;
        kept.first_point = (uint32_t)face->points.count;
        kept.point_count = 0;
        kept.first_polyline = (uint32_t)face->polylines.count;
        kept.polyline_count = g.polyline_count;
        for (uint32_t p = 0; p < g.polyline_count; p++)
        {
            const RBTTEXT_STROKE_POLYLINE &line = source.polylines[g.first_polyline + p];
            RBTTEXT_STROKE_POLYLINE copy;
            copy.first_point = kept.point_count;
            copy.point_count = line.point_count;
            if (face->polylines.push(copy) != EQ_SUCCESS)
                return rbttext_out_of_memory(error, "copying the polylines");
            for (uint32_t q = 0; q < line.point_count; q++)
            {
                RbtVec2 point = source.points[line.first_point + q];
                point.y = -point.y;
                if (face->points.push(point) != EQ_SUCCESS)
                    return rbttext_out_of_memory(error, "copying the points");
            }
            kept.point_count += line.point_count;
        }
        if (face->glyphs.push(kept) != EQ_SUCCESS)
            return rbttext_out_of_memory(error, "copying the glyphs");
        rbttext_cmap_entry entry = {g.codepoint, i + 1};
        if (face->cmap.push(entry) != EQ_SUCCESS)
            return rbttext_out_of_memory(error, "copying the glyphs");
    }
    std::sort(face->cmap.begin(), face->cmap.end(), [](const rbttext_cmap_entry &a, const rbttext_cmap_entry &b) {
        if (a.codepoint != b.codepoint)
            return a.codepoint < b.codepoint;
        return a.glyph < b.glyph;
    });
    for (size_t i = 1; i < face->cmap.count; i++)
    {
        if (face->cmap[i].codepoint == face->cmap[i - 1].codepoint)
            return rbttext_fail(error, RBT_RESULT_INVALID_ARGUMENT, "glyphs[%u] and glyphs[%u] both draw U+%04X",
                               face->cmap[i - 1].glyph - 1, face->cmap[i].glyph - 1, face->cmap[i].codepoint);
    }
    // The fingerprint: a CRC-32 of what was kept, which is the same bytes
    // on every platform (floats as their bits, little-endian or not, since
    // the CRC runs over each value's bytes in a fixed order).
    uint32_t crc = 0;
    for (const rbttext_stroke_glyph &g : face->glyphs)
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
    for (const RbtVec2 &point : face->points)
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
    return RBT_RESULT_OK;
}

// ---- Hershey fonts --------------------------------------------------------------

// A Hershey file read: every glyph's advance and polylines, in em, y up.
typedef rbttext_stroke_font hershey_font;
RBT_PLAIN_DATA(RBTTEXT_STROKE_GLYPH);

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
        RbtVec2 dot = font.points[first_point];
        if (EQ_RESULT result = font.points.push(dot))
            return result;
        count = 2;
    }
    RBTTEXT_STROKE_POLYLINE line = {first_point, count};
    return font.polylines.push(line);
}

static RBT_RESULT read_hershey(const RBTTEXT_HERSHEY_FACE_DESC &d, hershey_font &font, RBTTEXT_ERROR *error)
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
            return rbttext_fail(error, RBT_RESULT_TEXT_NOT_A_FONT, "line %u is too short for a Hershey glyph",
                               line_number);
        for (uint32_t column = 0; column < 5; column++)
        {
            uint8_t c = data[at + column];
            if (c == ' ')
                continue;
            if (c < '0' || c > '9')
                return rbttext_fail(error, RBT_RESULT_TEXT_NOT_A_FONT,
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
                return rbttext_fail(error, RBT_RESULT_TEXT_NOT_A_FONT,
                                   "line %u: column %u is not a digit of a Hershey glyph's vertex count", line_number,
                                   column + 1);
            vertex_count = vertex_count * 10 + (uint32_t)(c - '0');
            digits = true;
        }
        if (vertex_count == 0)
            return rbttext_fail(error, RBT_RESULT_TEXT_NOT_A_FONT, "line %u: a Hershey glyph with no vertices",
                               line_number);
        at += 8;

        // The pairs, which may run on over several lines.
        RBTTEXT_STROKE_GLYPH glyph = {};
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
                return rbttext_fail(error, RBT_RESULT_TEXT_NOT_A_FONT,
                                   "line %u: the text ends inside a Hershey glyph's coordinates", line_number);
            uint8_t cx = data[at];
            uint8_t cy = data[at + 1];
            at += 2;
            if (cx < ' ' || cx > '~' || cy < ' ' || cy > '~')
                return rbttext_fail(error, RBT_RESULT_TEXT_NOT_A_FONT,
                                   "line %u: a Hershey coordinate that isn't a printable character", line_number);
            int x = (int)cx - 'R';
            int y = (int)cy - 'R';
            if (v == 0)
            {
                left = x;
                glyph.advance = (float)((double)(y - x) * scale);
                if (glyph.advance < 0.0f)
                    return rbttext_fail(error, RBT_RESULT_TEXT_NOT_A_FONT,
                                       "line %u: a Hershey glyph whose right bound is left of its left", line_number);
                continue;
            }
            if (cx == ' ' && cy == 'R')
            {
                if (end_hershey_polyline(font, polyline_start) != EQ_SUCCESS)
                    return rbttext_out_of_memory(error, "reading the Hershey font");
                polyline_start = (uint32_t)font.points.count;
                continue;
            }
            RbtVec2 point;
            point.x = (float)((double)(x - left) * scale);
            point.y = (float)(((double)d.baseline - (double)y) * scale);
            if (font.points.push(point) != EQ_SUCCESS)
                return rbttext_out_of_memory(error, "reading the Hershey font");
        }
        if (end_hershey_polyline(font, polyline_start) != EQ_SUCCESS)
            return rbttext_out_of_memory(error, "reading the Hershey font");
        glyph.polyline_count = (uint32_t)font.polylines.count - glyph.first_polyline;
        // What follows the last pair: trailing spaces, then the line's end.
        while (at < size && data[at] == ' ')
            at++;
        if (at < size && !is_line_end(data[at]))
            return rbttext_fail(error, RBT_RESULT_TEXT_NOT_A_FONT,
                               "line %u: more than the glyph's vertex count of coordinates", line_number);
        at = skip_line_end(data, size, at);
        line_number++;
        if (font.glyphs.push(glyph) != EQ_SUCCESS)
            return rbttext_out_of_memory(error, "reading the Hershey font");
    }
    if (font.glyphs.count == 0)
        return rbttext_fail(error, RBT_RESULT_TEXT_NOT_A_FONT, "the text holds no Hershey glyph");
    return RBT_RESULT_OK;
}

static RBT_RESULT check_stroke_metrics(const RBTTEXT_STROKE_METRICS &m, RBTTEXT_ERROR *error)
{
    if (!isfinite(m.ascender))
        return rbttext_fail(error, RBT_RESULT_INVALID_ARGUMENT, "metrics.ascender is not finite");
    if (!isfinite(m.descender))
        return rbttext_fail(error, RBT_RESULT_INVALID_ARGUMENT, "metrics.descender is not finite");
    if (m.ascender <= m.descender)
        return rbttext_fail(error, RBT_RESULT_INVALID_ARGUMENT, "metrics.ascender is not above metrics.descender");
    if (!isfinite(m.line_gap))
        return rbttext_fail(error, RBT_RESULT_INVALID_ARGUMENT, "metrics.line_gap is not finite");
    if (m.line_gap < 0.0f)
        return rbttext_fail(error, RBT_RESULT_INVALID_ARGUMENT, "metrics.line_gap is negative");
    return RBT_RESULT_OK;
}

// A read font, made a face.
static RBT_RESULT make_stroke_face(rbttext_stroke_font &font, const RBTTEXT_STROKE_METRICS &metrics, rbttext_face *out_face,
                                 RBTTEXT_ERROR *error)
{
    rbttext_face_t *face = new_face();
    if (!face)
        return rbttext_out_of_memory(error, "creating the face");
    const uint32_t glyph_count = (uint32_t)font.glyphs.count;
    stroke_source source = {font.glyphs.data, glyph_count, font.polylines.data, font.points.data};
    if (RBT_RESULT result = fill_stroke(face, source, error))
    {
        delete_face(face);
        return result;
    }
    stroke_metrics(face, metrics, glyph_count);
    *out_face = face;
    return RBT_RESULT_OK;
}

extern "C"
{
    // ---- Faces ------------------------------------------------------------------

    RBT_RESULT rbttext_face_create(const RBTTEXT_FACE_DESC *desc, rbttext_face *out_face, RBTTEXT_ERROR *out_error)
    {
        rbttext_clear_error(out_error);
        if (!out_face)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "out_face is null");
        *out_face = nullptr;
        if (!desc)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc is null");
        if (RBT_RESULT result = rbt_struct_check(&desc->base, RBT_STRUCT_TYPE_TEXT_FACE_DESC))
            return rbttext_fail(out_error, result, "desc %s", rbt_struct_check_message(result));
        const RBTTEXT_FACE_DESC &d = *desc;
        if (!d.data)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc's data is null");
        if (d.data_size == 0)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc's data_size is 0");
        if (d.data_size > 0xFFFFFFFFull)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT,
                               "desc's data_size is over 4 GiB, more than a font's 32-bit offsets address");
        if (rbttext_missing(d.variations, d.variation_count))
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT,
                               "desc's variations is null with a variation_count of %u", d.variation_count);
        for (uint32_t i = 0; i < d.variation_count; i++)
        {
            if (!isfinite(d.variations[i].value))
                return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "variations[%u]'s value is not finite", i);
        }

        rbttext_face_t *face = new_face();
        if (!face)
            return rbttext_out_of_memory(out_error, "creating the face");
        // HarfBuzz borrows the bytes, or copies them (copy_data); a blob it
        // couldn't make is its empty one.
        hb_memory_mode_t mode = d.copy_data ? HB_MEMORY_MODE_DUPLICATE : HB_MEMORY_MODE_READONLY;
        face->blob = hb_blob_create((const char *)d.data, (unsigned int)d.data_size, mode, nullptr, nullptr);
        if (face->blob == hb_blob_get_empty())
        {
            face->blob = nullptr;
            delete_face(face);
            return rbttext_out_of_memory(out_error, "creating the face");
        }
        hb_blob_make_immutable(face->blob);
        if (RBT_RESULT result = open_outline(face, d.collection_index, d.variations, d.variation_count, out_error))
        {
            delete_face(face);
            return result;
        }
        *out_face = face;
        return RBT_RESULT_OK;
    }

    RBT_RESULT rbttext_face_create_stroke(const RBTTEXT_STROKE_FACE_DESC *desc, rbttext_face *out_face,
                                       RBTTEXT_ERROR *out_error)
    {
        rbttext_clear_error(out_error);
        if (!out_face)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "out_face is null");
        *out_face = nullptr;
        if (!desc)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc is null");
        if (RBT_RESULT result = rbt_struct_check(&desc->base, RBT_STRUCT_TYPE_TEXT_STROKE_FACE_DESC))
            return rbttext_fail(out_error, result, "desc %s", rbt_struct_check_message(result));
        const RBTTEXT_STROKE_FACE_DESC &d = *desc;
        if (!d.glyphs)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc's glyphs is null");
        if (d.glyph_count == 0)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc's glyph_count is 0");
        if (rbttext_missing(d.polylines, d.polyline_count))
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT,
                               "desc's polylines is null with a polyline_count of %u", d.polyline_count);
        if (rbttext_missing(d.points, d.point_count))
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc's points is null with a point_count of %u",
                               d.point_count);
        for (uint32_t i = 0; i < d.glyph_count; i++)
        {
            const RBTTEXT_STROKE_GLYPH &g = d.glyphs[i];
            if (!rbttext_is_valid_codepoint(g.codepoint))
                return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "glyphs[%u].codepoint 0x%X is not a codepoint",
                                   i, g.codepoint);
            if (!isfinite(g.advance))
                return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "glyphs[%u].advance is not finite", i);
            if (g.advance < 0.0f)
                return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "glyphs[%u].advance is negative", i);
            if (g.first_polyline > d.polyline_count)
                return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "glyphs[%u].first_polyline is past polylines",
                                   i);
            if (g.polyline_count > d.polyline_count - g.first_polyline)
                return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "glyphs[%u]'s polylines run past polylines",
                                   i);
        }
        for (uint32_t i = 0; i < d.polyline_count; i++)
        {
            const RBTTEXT_STROKE_POLYLINE &p = d.polylines[i];
            if (p.point_count < 2)
                return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "polylines[%u] has fewer than 2 points", i);
            if (p.first_point > d.point_count)
                return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "polylines[%u].first_point is past points", i);
            if (p.point_count > d.point_count - p.first_point)
                return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "polylines[%u]'s points run past points", i);
        }
        for (uint32_t i = 0; i < d.point_count; i++)
        {
            if (!isfinite(d.points[i].x))
                return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "points[%u].x is not finite", i);
            if (!isfinite(d.points[i].y))
                return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "points[%u].y is not finite", i);
        }
        if (!isfinite(d.metrics.ascender))
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "metrics.ascender is not finite");
        if (!isfinite(d.metrics.descender))
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "metrics.descender is not finite");
        if (d.metrics.ascender <= d.metrics.descender)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "metrics.ascender is not above metrics.descender");
        if (!isfinite(d.metrics.line_gap))
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "metrics.line_gap is not finite");
        if (d.metrics.line_gap < 0.0f)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "metrics.line_gap is negative");

        rbttext_face_t *face = new_face();
        if (!face)
            return rbttext_out_of_memory(out_error, "creating the face");
        stroke_source source = {d.glyphs, d.glyph_count, d.polylines, d.points};
        if (RBT_RESULT result = fill_stroke(face, source, out_error))
        {
            delete_face(face);
            return result;
        }
        stroke_metrics(face, d.metrics, d.glyph_count);
        *out_face = face;
        return RBT_RESULT_OK;
    }

    RBT_RESULT rbttext_face_create_hershey(const RBTTEXT_HERSHEY_FACE_DESC *desc, rbttext_face *out_face,
                                        RBTTEXT_ERROR *out_error)
    {
        rbttext_clear_error(out_error);
        if (!out_face)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "out_face is null");
        *out_face = nullptr;
        if (!desc)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc is null");
        if (RBT_RESULT result = rbt_struct_check(&desc->base, RBT_STRUCT_TYPE_TEXT_HERSHEY_FACE_DESC))
            return rbttext_fail(out_error, result, "desc %s", rbt_struct_check_message(result));
        const RBTTEXT_HERSHEY_FACE_DESC &d = *desc;
        if (!d.data)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc's data is null");
        if (d.data_size == 0)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc's data_size is 0");
        if (rbttext_missing(d.codepoints, d.codepoint_count))
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT,
                               "desc's codepoints is null with a codepoint_count of %u", d.codepoint_count);
        if (d.codepoints)
        {
            if (d.codepoint_count == 0)
                return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT,
                                   "desc's codepoint_count is 0 with codepoints given");
            for (uint32_t i = 0; i < d.codepoint_count; i++)
            {
                if (!rbttext_is_valid_codepoint(d.codepoints[i]))
                    return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "codepoints[%u] 0x%X is not a codepoint",
                                       i, d.codepoints[i]);
            }
        }
        else
        {
            if (!rbttext_is_valid_codepoint(d.first_codepoint))
                return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT,
                                   "desc's first_codepoint 0x%X is not a codepoint", d.first_codepoint);
        }
        if (!isfinite(d.units_per_em))
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc's units_per_em is not finite");
        if (d.units_per_em <= 0.0f)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc's units_per_em is not above 0");
        if (!isfinite(d.baseline))
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc's baseline is not finite");
        if (!isfinite(d.metrics.ascender))
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "metrics.ascender is not finite");
        if (!isfinite(d.metrics.descender))
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "metrics.descender is not finite");
        if (d.metrics.ascender <= d.metrics.descender)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "metrics.ascender is not above metrics.descender");
        if (!isfinite(d.metrics.line_gap))
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "metrics.line_gap is not finite");
        if (d.metrics.line_gap < 0.0f)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "metrics.line_gap is negative");

        hershey_font font;
        if (RBT_RESULT result = read_hershey(d, font, out_error))
            return result;
        uint32_t glyph_count = (uint32_t)font.glyphs.count;
        if (d.codepoints && d.codepoint_count != glyph_count)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT,
                               "desc's codepoint_count is %u, and the file has %u glyphs", d.codepoint_count,
                               glyph_count);
        for (uint32_t i = 0; i < glyph_count; i++)
        {
            uint64_t codepoint = d.codepoints ? d.codepoints[i] : (uint64_t)d.first_codepoint + i;
            if (codepoint > 0x10FFFFu || !rbttext_is_valid_codepoint((uint32_t)codepoint))
                return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT,
                                   "the file's glyph %u would draw 0x%llX, which is not a codepoint", i,
                                   (unsigned long long)codepoint);
            font.glyphs[i].codepoint = (uint32_t)codepoint;
        }

        rbttext_face_t *face = new_face();
        if (!face)
            return rbttext_out_of_memory(out_error, "creating the face");
        stroke_source source = {font.glyphs.data, glyph_count, font.polylines.data, font.points.data};
        if (RBT_RESULT result = fill_stroke(face, source, out_error))
        {
            delete_face(face);
            return result;
        }
        stroke_metrics(face, d.metrics, glyph_count);
        *out_face = face;
        return RBT_RESULT_OK;
    }

    RBT_RESULT rbttext_face_create_lff(const RBTTEXT_LFF_FACE_DESC *desc, rbttext_face *out_face, RBTTEXT_ERROR *out_error)
    {
        rbttext_clear_error(out_error);
        if (!out_face)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "out_face is null");
        *out_face = nullptr;
        if (!desc)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc is null");
        if (RBT_RESULT result = rbt_struct_check(&desc->base, RBT_STRUCT_TYPE_TEXT_LFF_FACE_DESC))
            return rbttext_fail(out_error, result, "desc %s", rbt_struct_check_message(result));
        const RBTTEXT_LFF_FACE_DESC &d = *desc;
        if (!d.data)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc's data is null");
        if (d.data_size == 0)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc's data_size is 0");
        if (!isfinite(d.units_per_em))
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc's units_per_em is not finite");
        if (d.units_per_em <= 0.0f)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc's units_per_em is not above 0");
        if (!isfinite(d.arc_tolerance))
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc's arc_tolerance is not finite");
        if (d.arc_tolerance <= 0.0f)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc's arc_tolerance is not above 0");
        if (RBT_RESULT result = check_stroke_metrics(d.metrics, out_error))
            return result;
        rbttext_stroke_font font;
        if (RBT_RESULT result = rbttext_read_lff(d, font, out_error))
            return result;
        return make_stroke_face(font, d.metrics, out_face, out_error);
    }

    RBT_RESULT rbttext_face_create_shx(const RBTTEXT_SHX_FACE_DESC *desc, rbttext_face *out_face, RBTTEXT_ERROR *out_error)
    {
        rbttext_clear_error(out_error);
        if (!out_face)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "out_face is null");
        *out_face = nullptr;
        if (!desc)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc is null");
        if (RBT_RESULT result = rbt_struct_check(&desc->base, RBT_STRUCT_TYPE_TEXT_SHX_FACE_DESC))
            return rbttext_fail(out_error, result, "desc %s", rbt_struct_check_message(result));
        const RBTTEXT_SHX_FACE_DESC &d = *desc;
        if (!d.data)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc's data is null");
        if (d.data_size == 0)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc's data_size is 0");
        if (rbttext_missing(d.high_codepoints, d.high_codepoint_count))
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT,
                               "desc's high_codepoints is null with a high_codepoint_count of %u", d.high_codepoint_count);
        if (d.high_codepoint_count > 65408)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc's high_codepoint_count is over 65,408");
        for (uint32_t i = 0; i < d.high_codepoint_count; i++)
        {
            if (!rbttext_is_valid_codepoint(d.high_codepoints[i]))
                return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "high_codepoints[%u] 0x%X is not a codepoint",
                                   i, d.high_codepoints[i]);
        }
        if (!isfinite(d.arc_tolerance))
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc's arc_tolerance is not finite");
        if (d.arc_tolerance <= 0.0f)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc's arc_tolerance is not above 0");
        if (!isfinite(d.line_gap))
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc's line_gap is not finite");
        if (d.line_gap < 0.0f)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc's line_gap is negative");
        rbttext_stroke_font font;
        float descender = 0.0f;
        if (RBT_RESULT result = rbttext_read_shx(d, font, &descender, out_error))
            return result;
        RBTTEXT_STROKE_METRICS metrics;
        metrics.ascender = 1.0f;
        metrics.descender = descender;
        metrics.line_gap = d.line_gap;
        return make_stroke_face(font, metrics, out_face, out_error);
    }

    RBT_RESULT rbttext_face_get_metrics(rbttext_face face, RBTTEXT_FACE_METRICS *out_metrics)
    {
        if (!face)
            return RBT_RESULT_INVALID_ARGUMENT;
        if (RBT_STALE(face, "rbttext_face_get_metrics", "face"))
            return RBT_RESULT_INVALID_ARGUMENT;
        if (!out_metrics)
            return RBT_RESULT_INVALID_ARGUMENT;
        if (RBT_RESULT result = rbt_struct_out_check(&out_metrics->base, RBT_STRUCT_TYPE_TEXT_FACE_METRICS))
            return result;
        RBT_STRUCT_BASE_OUT base = out_metrics->base;
        *out_metrics = face->metrics;
        out_metrics->base = base;
        return RBT_RESULT_OK;
    }

    uint32_t rbttext_face_get_axis_count(rbttext_face face)
    {
        if (!face)
            return 0;
        if (RBT_STALE(face, "rbttext_face_get_axis_count", "face"))
            return 0;
        if (face->kind != RBTTEXT_FACE_KIND_OUTLINE)
            return 0;
        return hb_ot_var_get_axis_count(face->hb_face);
    }

    RBT_RESULT rbttext_face_get_axis(rbttext_face face, uint32_t index, RBTTEXT_AXIS *out_axis)
    {
        if (!face)
            return RBT_RESULT_INVALID_ARGUMENT;
        if (RBT_STALE(face, "rbttext_face_get_axis", "face"))
            return RBT_RESULT_INVALID_ARGUMENT;
        if (!out_axis)
            return RBT_RESULT_INVALID_ARGUMENT;
        if (index >= rbttext_face_get_axis_count(face))
            return RBT_RESULT_NOT_FOUND;
        hb_ot_var_axis_info_t info;
        unsigned int one = 1;
        hb_ot_var_get_axis_infos(face->hb_face, index, &one, &info);
        out_axis->tag = info.tag;
        out_axis->min_value = info.min_value;
        out_axis->default_value = info.default_value;
        out_axis->max_value = info.max_value;
        out_axis->hidden = (info.flags & HB_OT_VAR_AXIS_FLAG_HIDDEN) != 0;
        return RBT_RESULT_OK;
    }

    RBT_RESULT rbttext_face_find_glyph(rbttext_face face, uint32_t codepoint, uint32_t *out_glyph)
    {
        if (!face)
            return RBT_RESULT_INVALID_ARGUMENT;
        if (RBT_STALE(face, "rbttext_face_find_glyph", "face"))
            return RBT_RESULT_INVALID_ARGUMENT;
        if (!out_glyph)
            return RBT_RESULT_INVALID_ARGUMENT;
        if (!rbttext_face_lookup(face, codepoint, out_glyph))
            return RBT_RESULT_NOT_FOUND;
        return RBT_RESULT_OK;
    }

    RBT_RESULT rbttext_face_get_glyph_strokes(rbttext_face face, uint32_t glyph, const RbtVec2 **out_points,
                                           uint32_t *out_point_count, const RBTTEXT_STROKE_POLYLINE **out_polylines,
                                           uint32_t *out_polyline_count)
    {
        if (!face)
            return RBT_RESULT_INVALID_ARGUMENT;
        if (RBT_STALE(face, "rbttext_face_get_glyph_strokes", "face"))
            return RBT_RESULT_INVALID_ARGUMENT;
        if (!out_points)
            return RBT_RESULT_INVALID_ARGUMENT;
        if (!out_point_count)
            return RBT_RESULT_INVALID_ARGUMENT;
        if (!out_polylines)
            return RBT_RESULT_INVALID_ARGUMENT;
        if (!out_polyline_count)
            return RBT_RESULT_INVALID_ARGUMENT;
        *out_points = nullptr;
        *out_point_count = 0;
        *out_polylines = nullptr;
        *out_polyline_count = 0;
        if (face->kind != RBTTEXT_FACE_KIND_STROKE)
            return RBT_RESULT_INVALID_ARGUMENT;
        if (glyph > face->glyphs.count)
            return RBT_RESULT_NOT_FOUND;
        if (glyph == 0)
            return RBT_RESULT_OK; // .notdef draws nothing
        const rbttext_stroke_glyph &g = face->glyphs[glyph - 1];
        if (g.point_count)
            *out_points = face->points.data + g.first_point;
        *out_point_count = g.point_count;
        if (g.polyline_count)
            *out_polylines = face->polylines.data + g.first_polyline;
        *out_polyline_count = g.polyline_count;
        return RBT_RESULT_OK;
    }

    void rbttext_face_destroy(rbttext_face face)
    {
        if (!face)
            return;
        if (RBT_STALE(face, "rbttext_face_destroy", "face"))
            return;
        delete_face(face);
    }

    // ---- Fallback chains --------------------------------------------------------

    RBT_RESULT rbttext_chain_create(const RBTTEXT_CHAIN_DESC *desc, rbttext_chain *out_chain, RBTTEXT_ERROR *out_error)
    {
        rbttext_clear_error(out_error);
        if (!out_chain)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "out_chain is null");
        *out_chain = nullptr;
        if (!desc)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc is null");
        if (RBT_RESULT result = rbt_struct_check(&desc->base, RBT_STRUCT_TYPE_TEXT_CHAIN_DESC))
            return rbttext_fail(out_error, result, "desc %s", rbt_struct_check_message(result));
        const RBTTEXT_CHAIN_DESC &d = *desc;
        if (!d.faces)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc's faces is null");
        if (d.face_count == 0)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc's face_count is 0");
        for (uint32_t i = 0; i < d.face_count; i++)
        {
            if (!d.faces[i])
                return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "faces[%u] is null", i);
        }
        // A chain is a handful of faces: comparing each pair is cheaper
        // than anything that allocates.
        for (uint32_t i = 1; i < d.face_count; i++)
        {
            for (uint32_t j = 0; j < i; j++)
            {
                if (d.faces[i] == d.faces[j])
                    return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "faces[%u] is faces[%u] again", i, j);
            }
        }
        for (uint32_t i = 1; i < d.face_count; i++)
        {
            if (d.faces[i]->kind != d.faces[0]->kind)
                return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT,
                                   "faces[%u] is a %s face and faces[0] a %s one: a chain's faces are of one kind", i,
                                   d.faces[i]->kind == RBTTEXT_FACE_KIND_STROKE ? "stroke" : "outline",
                                   d.faces[0]->kind == RBTTEXT_FACE_KIND_STROKE ? "stroke" : "outline");
        }

        rbttext_chain_t *chain = (rbttext_chain_t *)malloc(sizeof(rbttext_chain_t));
        if (!chain)
            return rbttext_out_of_memory(out_error, "creating the chain");
        chain = new (chain) rbttext_chain_t();
        chain->kind = d.faces[0]->kind;
        if (chain->faces.append(d.faces, d.face_count) != EQ_SUCCESS)
        {
            chain->~rbttext_chain_t();
            free(chain);
            return rbttext_out_of_memory(out_error, "creating the chain");
        }
        *out_chain = chain;
        return RBT_RESULT_OK;
    }

    rbttext_face rbttext_chain_get_face(rbttext_chain chain, uint32_t index)
    {
        if (!chain)
            return nullptr;
        if (RBT_STALE(chain, "rbttext_chain_get_face", "chain"))
            return nullptr;
        if (index >= chain->faces.count)
            return nullptr;
        return chain->faces[index];
    }

    uint32_t rbttext_chain_get_face_count(rbttext_chain chain)
    {
        if (!chain)
            return 0;
        if (RBT_STALE(chain, "rbttext_chain_get_face_count", "chain"))
            return 0;
        return (uint32_t)chain->faces.count;
    }

    RBT_RESULT rbttext_chain_find_glyph(rbttext_chain chain, uint32_t codepoint, uint32_t *out_face_index,
                                     uint32_t *out_glyph)
    {
        if (!chain)
            return RBT_RESULT_INVALID_ARGUMENT;
        if (RBT_STALE(chain, "rbttext_chain_find_glyph", "chain"))
            return RBT_RESULT_INVALID_ARGUMENT;
        if (!out_face_index)
            return RBT_RESULT_INVALID_ARGUMENT;
        if (!out_glyph)
            return RBT_RESULT_INVALID_ARGUMENT;
        *out_face_index = 0;
        *out_glyph = 0;
        for (uint32_t i = 0; i < chain->faces.count; i++)
        {
            uint32_t glyph = 0;
            if (rbttext_face_lookup(chain->faces[i], codepoint, &glyph))
            {
                *out_face_index = i;
                *out_glyph = glyph;
                return RBT_RESULT_OK;
            }
        }
        return RBT_RESULT_NOT_FOUND;
    }

    void rbttext_chain_destroy(rbttext_chain chain)
    {
        if (!chain)
            return;
        if (RBT_STALE(chain, "rbttext_chain_destroy", "chain"))
            return;
        chain->~rbttext_chain_t();
        free(chain);
    }
}
