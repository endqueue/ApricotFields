#include "aptext_internal.hpp"
#include "../apristructbase.hpp"
#include <libdeflate.h>
#include <math.h>
#include <core/ShapeDistanceFinder.h>
#include <msdfgen.h>
#include <new>
#include <stdlib.h>
#include <string.h>
#include "../aprioverlap.hpp"

// ApText's glyph generation and atlases (APTEXT_TODO.md, item 3). A glyph's
// outline comes from HarfBuzz's draw callbacks into an msdfgen shape, in em
// units; msdfgen makes its MTSDF, which is quantised to RGBA8. An atlas packs
// the images into pages with a skyline packer, keeps a rectangle per page of
// what changed, and saves to and loads from bytes.

// ---- Outlines -------------------------------------------------------------------

// What the draw callbacks build: a shape in em units, y up.
struct outline_builder
{
    msdfgen::Shape *shape;
    msdfgen::Contour *contour;
    msdfgen::Point2 last;
    double scale; // em per font unit
    bool failed;  // out of memory in a callback, which can't throw through HarfBuzz
};

static msdfgen::Point2 em_point(const outline_builder *b, float x, float y)
{
    return msdfgen::Point2((double)x * b->scale, (double)y * b->scale);
}

static void move_to(hb_draw_funcs_t *, void *data, hb_draw_state_t *, float x, float y, void *)
{
    outline_builder *b = (outline_builder *)data;
    if (b->failed)
        return;
    try
    {
        b->contour = &b->shape->addContour();
        b->last = em_point(b, x, y);
    }
    catch (const std::bad_alloc &)
    {
        b->failed = true;
    }
}

static void line_to(hb_draw_funcs_t *, void *data, hb_draw_state_t *, float x, float y, void *)
{
    outline_builder *b = (outline_builder *)data;
    if (b->failed || !b->contour)
        return;
    msdfgen::Point2 to = em_point(b, x, y);
    if (to == b->last)
        return; // a zero-length edge has no direction
    try
    {
        b->contour->addEdge(msdfgen::EdgeHolder(b->last, to));
        b->last = to;
    }
    catch (const std::bad_alloc &)
    {
        b->failed = true;
    }
}

static void quadratic_to(hb_draw_funcs_t *, void *data, hb_draw_state_t *, float cx, float cy, float x, float y,
                         void *)
{
    outline_builder *b = (outline_builder *)data;
    if (b->failed || !b->contour)
        return;
    msdfgen::Point2 control = em_point(b, cx, cy);
    msdfgen::Point2 to = em_point(b, x, y);
    if (to == b->last && control == b->last)
        return;
    try
    {
        b->contour->addEdge(msdfgen::EdgeHolder(b->last, control, to));
        b->last = to;
    }
    catch (const std::bad_alloc &)
    {
        b->failed = true;
    }
}

static void cubic_to(hb_draw_funcs_t *, void *data, hb_draw_state_t *, float c1x, float c1y, float c2x, float c2y,
                     float x, float y, void *)
{
    outline_builder *b = (outline_builder *)data;
    if (b->failed || !b->contour)
        return;
    msdfgen::Point2 c1 = em_point(b, c1x, c1y);
    msdfgen::Point2 c2 = em_point(b, c2x, c2y);
    msdfgen::Point2 to = em_point(b, x, y);
    if (to == b->last && c1 == b->last && c2 == b->last)
        return;
    try
    {
        b->contour->addEdge(msdfgen::EdgeHolder(b->last, c1, c2, to));
        b->last = to;
    }
    catch (const std::bad_alloc &)
    {
        b->failed = true;
    }
}

// HarfBuzz closes a contour with a line back to its start itself.
static void close_path(hb_draw_funcs_t *, void *data, hb_draw_state_t *, void *)
{
    outline_builder *b = (outline_builder *)data;
    b->contour = nullptr;
}

hb_draw_funcs_t *aptext_create_draw_funcs(void)
{
    hb_draw_funcs_t *funcs = hb_draw_funcs_create();
    if (funcs == hb_draw_funcs_get_empty())
        return nullptr;
    hb_draw_funcs_set_move_to_func(funcs, move_to, nullptr, nullptr);
    hb_draw_funcs_set_line_to_func(funcs, line_to, nullptr, nullptr);
    hb_draw_funcs_set_quadratic_to_func(funcs, quadratic_to, nullptr, nullptr);
    hb_draw_funcs_set_cubic_to_func(funcs, cubic_to, nullptr, nullptr);
    hb_draw_funcs_set_close_path_func(funcs, close_path, nullptr, nullptr);
    hb_draw_funcs_make_immutable(funcs);
    return funcs;
}

// ---- Generation ---------------------------------------------------------------------

APRESULT aptext_glyph_render(aptext_face face, uint32_t glyph, double pixels_per_em, double distance_range,
                             uint8_t *pixels, aptext_glyph_bounds *out_bounds, APTEXT_ERROR *error)
{
    memset(out_bounds, 0, sizeof(*out_bounds));
    msdfgen::Shape shape;
    outline_builder builder = {&shape, nullptr, msdfgen::Point2(), 1.0 / (double)face->metrics.units_per_em, false};
    hb_font_draw_glyph(face->font, glyph, face->draw, &builder);
    if (builder.failed)
        throw std::bad_alloc();
    shape.normalize();
    if (shape.edgeCount() == 0)
        return APRESULT_OK; // nothing to draw: a 0 by 0 image

    // The image covers the outline's bounds with half the range around
    // them and one pixel more, out to whole pixels, so its edges are at
    // pixel corners. The pixel more is for the colour channels: off a
    // convex corner they hold the distance to each edge's line, not to the
    // corner, so a texel within half the range of both lines reads as near
    // the outline, and bilinear sampling at the image's edge blends it in.
    double l = INFINITY;
    double b = INFINITY;
    double r = -INFINITY;
    double t = -INFINITY;
    shape.bound(l, b, r, t);
    const double margin = distance_range / 2.0 + 1.0;
    const double left = floor(l * pixels_per_em - margin);
    const double right = ceil(r * pixels_per_em + margin);
    const double bottom = floor(b * pixels_per_em - margin);
    const double top = ceil(t * pixels_per_em + margin);
    const double width = right - left;
    const double height = top - bottom;
    if (!(width <= (double)APTEXT_MAX_IMAGE_SIDE) || !(height <= (double)APTEXT_MAX_IMAGE_SIDE))
        return aptext_fail(error, APRESULT_INVALID_ARGUMENT,
                           "pixels_per_em %g makes glyph %u's image larger than %u pixels a side", pixels_per_em, glyph,
                           APTEXT_MAX_IMAGE_SIDE);
    out_bounds->width = (uint32_t)width;
    out_bounds->height = (uint32_t)height;
    out_bounds->plane_left = (float)(left / pixels_per_em);
    out_bounds->plane_right = (float)(right / pixels_per_em);
    out_bounds->plane_top = (float)(-top / pixels_per_em);
    out_bounds->plane_bottom = (float)(-bottom / pixels_per_em);
    if (!pixels)
        return APRESULT_OK;

    // An outline wound the other way (as CFF's are against TrueType's) is
    // reversed, found by the sign of the distance from a point outside it.
    msdfgen::Shape::Bounds bounds = shape.getBounds();
    msdfgen::Point2 outside(bounds.l - (bounds.r - bounds.l) - 1.0, bounds.b - (bounds.t - bounds.b) - 1.0);
    if (msdfgen::SimpleTrueShapeDistanceFinder::oneShotDistance(shape, outside) > 0.0)
    {
        for (msdfgen::Contour &contour : shape.contours)
            contour.reverse();
    }
    // A fixed seed, so the same glyph is colored the same every time.
    msdfgen::edgeColoringSimple(shape, 3.0, 0);
    const int w = (int)width;
    const int h = (int)height;
    msdfgen::Bitmap<float, 4> field(w, h, msdfgen::Y_UPWARD);
    msdfgen::Projection projection(msdfgen::Vector2(pixels_per_em, pixels_per_em),
                                   msdfgen::Vector2(-left / pixels_per_em, -bottom / pixels_per_em));
    msdfgen::DistanceMapping mapping(msdfgen::Range(distance_range / pixels_per_em));
    msdfgen::generateMTSDF(field, shape, msdfgen::SDFTransformation(projection, mapping),
                           msdfgen::MSDFGeneratorConfig(true));
    // Rows from the top; each channel round(255 * clamp(v)).
    for (int y = 0; y < h; y++)
    {
        uint8_t *row = pixels + (size_t)y * (size_t)w * 4;
        for (int x = 0; x < w; x++)
        {
            const float *texel = field(x, h - 1 - y);
            for (int ch = 0; ch < 4; ch++)
            {
                double v = (double)texel[ch];
                if (!(v > 0.0))
                    v = 0.0;
                if (v > 1.0)
                    v = 1.0;
                row[x * 4 + ch] = (uint8_t)floor(255.0 * v + 0.5);
            }
        }
    }
    return APRESULT_OK;
}

// The checks of an APTEXT_GLYPH_GENERATE_DESC's fields, its base checked.
static APRESULT check_generate_desc(const APTEXT_GLYPH_GENERATE_DESC &d, APTEXT_ERROR *error)
{
    if (!d.face)
        return aptext_fail(error, APRESULT_INVALID_ARGUMENT, "desc's face is null");
    if (!isfinite(d.pixels_per_em))
        return aptext_fail(error, APRESULT_INVALID_ARGUMENT, "desc's pixels_per_em is not finite");
    if (d.pixels_per_em <= 0.0f)
        return aptext_fail(error, APRESULT_INVALID_ARGUMENT, "desc's pixels_per_em is not above 0");
    if (!isfinite(d.distance_range))
        return aptext_fail(error, APRESULT_INVALID_ARGUMENT, "desc's distance_range is not finite");
    if (d.distance_range <= 0.0f)
        return aptext_fail(error, APRESULT_INVALID_ARGUMENT, "desc's distance_range is not above 0");
    return APRESULT_OK;
}

// The checks that need the face: an outline face, and a glyph it has.
static APRESULT check_generate_face(const APTEXT_GLYPH_GENERATE_DESC &d, APTEXT_ERROR *error)
{
    if (d.face->kind != APTEXT_FACE_KIND_OUTLINE)
        return aptext_fail(error, APRESULT_INVALID_ARGUMENT,
                           "desc's face is a stroke face, which has no distance field");
    if (d.glyph >= d.face->metrics.glyph_count)
        return aptext_fail(error, APRESULT_NOT_FOUND, "glyph %u is past the face's last glyph (%u)", d.glyph,
                           d.face->metrics.glyph_count - 1);
    return APRESULT_OK;
}

// ---- Atlases ------------------------------------------------------------------------

aptext_atlas_t::~aptext_atlas_t()
{
    for (aptext_page &page : pages)
        free(page.pixels);
}

static void delete_atlas(aptext_atlas_t *atlas)
{
    atlas->~aptext_atlas_t();
    free(atlas);
}

static aptext_atlas_t *new_atlas(const APTEXT_ATLAS_DESC &settings)
{
    aptext_atlas_t *atlas = (aptext_atlas_t *)malloc(sizeof(aptext_atlas_t));
    if (!atlas)
        return nullptr;
    atlas = new (atlas) aptext_atlas_t();
    atlas->settings = settings;
    atlas->settings.base = apri_struct_base(APRI_STRUCT_TYPE_APTEXT_ATLAS_DESC);
    return atlas;
}

static uint64_t entry_hash(aptext_face face, uint32_t glyph)
{
    return ((uint64_t)(uintptr_t)face * 0x9E3779B97F4A7C15ull) ^ glyph;
}

static const aptext_atlas_entry *find_entry(const aptext_atlas_t *atlas, aptext_face face, uint32_t glyph)
{
    const uint32_t *index = atlas->index.find(entry_hash(face, glyph), [&](uint32_t value) {
        const aptext_atlas_entry &e = atlas->entries[value];
        return e.face == face && e.glyph == glyph;
    });
    return index ? &atlas->entries[*index] : nullptr;
}

// Adds a page: zero pixels, and a skyline flat along its bottom... its top,
// since rows run from the top.
static APRESULT add_page(aptext_atlas_t *atlas, APTEXT_ERROR *error)
{
    const size_t size = (size_t)atlas->settings.page_width * atlas->settings.page_height * 4;
    aptext_page page;
    page.pixels = (uint8_t *)calloc(1, size);
    if (!page.pixels)
        return aptext_out_of_memory(error, "adding a page");
    aptext_skyline floor_span = {0, 0, atlas->settings.page_width};
    if (page.skyline.push(floor_span) != EQ_SUCCESS)
    {
        free(page.pixels);
        return aptext_out_of_memory(error, "adding a page");
    }
    if (atlas->pages.push(std::move(page)) != EQ_SUCCESS)
    {
        free(page.pixels);
        return aptext_out_of_memory(error, "adding a page");
    }
    return APRESULT_OK;
}

// Where a [width] by [height] rectangle fits lowest (then leftmost) on
// [page]'s skyline, starting at span [*out_span]; false if nowhere.
static bool skyline_fit(const aptext_page &page, uint32_t page_width, uint32_t page_height, uint32_t width,
                        uint32_t height, uint32_t *out_span, uint32_t *out_y)
{
    bool found = false;
    uint32_t best_y = 0;
    uint32_t best_x = 0;
    for (uint32_t i = 0; i < page.skyline.count; i++)
    {
        const uint32_t x = page.skyline[i].x;
        if (x + width > page_width)
            break;
        uint32_t y = 0;
        uint32_t covered = 0;
        for (uint32_t j = i; j < page.skyline.count && covered < width; j++)
        {
            if (page.skyline[j].y > y)
                y = page.skyline[j].y;
            covered = page.skyline[j].x + page.skyline[j].width - x;
        }
        if (y + height > page_height)
            continue;
        if (!found || y < best_y || (y == best_y && x < best_x))
        {
            found = true;
            best_y = y;
            best_x = x;
            *out_span = i;
        }
    }
    *out_y = best_y;
    return found;
}

// Raises [page]'s skyline over the rectangle placed at span [span].
static EQ_RESULT skyline_place(aptext_page &page, uint32_t span, uint32_t width, uint32_t height, uint32_t y)
{
    const uint32_t x = page.skyline[span].x;
    const uint32_t end = x + width;
    aptext_skyline placed = {x, y + height, width};
    // Spans wholly under the rectangle go; one it covers in part is cut.
    uint32_t i = span;
    while (i < page.skyline.count && page.skyline[i].x + page.skyline[i].width <= end)
        i++;
    if (i < page.skyline.count && page.skyline[i].x < end)
    {
        page.skyline[i].width -= end - page.skyline[i].x;
        page.skyline[i].x = end;
    }
    if (EQ_RESULT result = page.skyline.remove(span, i - span))
        return result;
    if (EQ_RESULT result = page.skyline.insert(span, placed))
        return result;
    // Neighbours at one height become one span.
    for (uint32_t k = 0; k + 1 < page.skyline.count;)
    {
        if (page.skyline[k].y == page.skyline[k + 1].y)
        {
            page.skyline[k].width += page.skyline[k + 1].width;
            if (EQ_RESULT result = page.skyline.remove(k + 1))
                return result;
        }
        else
            k++;
    }
    return EQ_SUCCESS;
}

static void mark_dirty(aptext_page &page, uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    if (!page.dirty)
    {
        page.dirty = 1;
        page.x0 = x;
        page.y0 = y;
        page.x1 = x + width;
        page.y1 = y + height;
        return;
    }
    if (x < page.x0)
        page.x0 = x;
    if (y < page.y0)
        page.y0 = y;
    if (x + width > page.x1)
        page.x1 = x + width;
    if (y + height > page.y1)
        page.y1 = y + height;
}

static void fill_atlas_glyph(const aptext_atlas_entry &e, APTEXT_ATLAS_GLYPH *out)
{
    APRI_STRUCT_BASE_OUT base = out->base;
    memset(out, 0, sizeof(*out));
    out->base = base;
    out->page = e.page;
    out->x = e.x;
    out->y = e.y;
    out->width = e.width;
    out->height = e.height;
    out->plane_left = e.plane_left;
    out->plane_top = e.plane_top;
    out->plane_right = e.plane_right;
    out->plane_bottom = e.plane_bottom;
}

// Packs an image of [bounds] into [atlas], arguments checked and the glyph
// not held yet. Throws std::bad_alloc (the index).
static APRESULT place_glyph(aptext_atlas_t *atlas, aptext_face face, uint32_t glyph, const aptext_glyph_bounds &bounds,
                            const uint8_t *pixels, const aptext_atlas_entry **out_entry, APTEXT_ERROR *error)
{
    const APTEXT_ATLAS_DESC &s = atlas->settings;
    aptext_atlas_entry entry = {};
    entry.face = face;
    entry.glyph = glyph;
    entry.width = bounds.width;
    entry.height = bounds.height;
    entry.plane_left = bounds.plane_left;
    entry.plane_top = bounds.plane_top;
    entry.plane_right = bounds.plane_right;
    entry.plane_bottom = bounds.plane_bottom;
    if (bounds.width && bounds.height)
    {
        if (bounds.width > s.page_width || bounds.height > s.page_height)
            return aptext_fail(error, APRESULT_LIMIT, "glyph %u's image is %ux%u pixels, larger than a %ux%u page", glyph,
                               bounds.width, bounds.height, s.page_width, s.page_height);
        uint32_t page = 0;
        uint32_t span = 0;
        uint32_t y = 0;
        bool fits = false;
        for (; page < atlas->pages.count && !fits; page++)
            fits = skyline_fit(atlas->pages[page], s.page_width, s.page_height, bounds.width, bounds.height, &span, &y);
        if (fits)
            page--;
        else
        {
            if (atlas->pages.count >= s.max_pages)
                return aptext_fail(error, APRESULT_LIMIT, "the atlas is full at its max_pages of %u", s.max_pages);
            if (APRESULT result = add_page(atlas, error))
                return result;
            page = (uint32_t)atlas->pages.count - 1;
            fits = skyline_fit(atlas->pages[page], s.page_width, s.page_height, bounds.width, bounds.height, &span, &y);
        }
        aptext_page &target = atlas->pages[page];
        entry.page = page;
        entry.x = target.skyline[span].x;
        entry.y = y;
        if (skyline_place(target, span, bounds.width, bounds.height, y) != EQ_SUCCESS)
            return aptext_out_of_memory(error, "packing the glyph");
        for (uint32_t row = 0; row < bounds.height; row++)
            memcpy(target.pixels + ((size_t)(entry.y + row) * s.page_width + entry.x) * 4,
                   pixels + (size_t)row * bounds.width * 4, (size_t)bounds.width * 4);
        mark_dirty(target, entry.x, entry.y, bounds.width, bounds.height);
    }
    if (atlas->entries.push(entry) != EQ_SUCCESS)
        return aptext_out_of_memory(error, "adding the glyph");
    atlas->index.add(entry_hash(face, glyph), (uint32_t)atlas->entries.count - 1);
    *out_entry = &atlas->entries[atlas->entries.count - 1];
    return APRESULT_OK;
}

// The checks of an APTEXT_ATLAS_DESC, for aptext_atlas_create() and
// aptext_atlas_load(); [name] is what the message calls it.
static APRESULT check_atlas_desc(const APTEXT_ATLAS_DESC *desc, const char *name, APTEXT_ERROR *error)
{
    if (APRESULT result = apri_struct_check(&desc->base, APRI_STRUCT_TYPE_APTEXT_ATLAS_DESC))
        return aptext_fail(error, result, "%s %s", name, apri_struct_check_message(result));
    const APTEXT_ATLAS_DESC &d = *desc;
    if (!isfinite(d.pixels_per_em))
        return aptext_fail(error, APRESULT_INVALID_ARGUMENT, "%s's pixels_per_em is not finite", name);
    if (d.pixels_per_em <= 0.0f)
        return aptext_fail(error, APRESULT_INVALID_ARGUMENT, "%s's pixels_per_em is not above 0", name);
    if (!isfinite(d.distance_range))
        return aptext_fail(error, APRESULT_INVALID_ARGUMENT, "%s's distance_range is not finite", name);
    if (d.distance_range <= 0.0f)
        return aptext_fail(error, APRESULT_INVALID_ARGUMENT, "%s's distance_range is not above 0", name);
    if (d.page_width == 0)
        return aptext_fail(error, APRESULT_INVALID_ARGUMENT, "%s's page_width is 0", name);
    if (d.page_height == 0)
        return aptext_fail(error, APRESULT_INVALID_ARGUMENT, "%s's page_height is 0", name);
    double cell = (double)d.pixels_per_em + (double)d.distance_range;
    if ((double)d.page_width < cell)
        return aptext_fail(error, APRESULT_INVALID_ARGUMENT,
                           "%s's page_width is less than pixels_per_em plus distance_range", name);
    if ((double)d.page_height < cell)
        return aptext_fail(error, APRESULT_INVALID_ARGUMENT,
                           "%s's page_height is less than pixels_per_em plus distance_range", name);
    if (d.max_pages == 0)
        return aptext_fail(error, APRESULT_INVALID_ARGUMENT, "%s's max_pages is 0", name);
    return APRESULT_OK;
}

// Generates [glyph] of [face] at the atlas's settings and packs it, unless
// it is held already. Throws std::bad_alloc.
static APRESULT add_glyph(aptext_atlas_t *atlas, aptext_face face, uint32_t glyph, eq::array<uint8_t> &scratch,
                          APTEXT_ERROR *error)
{
    if (find_entry(atlas, face, glyph))
        return APRESULT_OK;
    if (glyph >= face->metrics.glyph_count)
        return aptext_fail(error, APRESULT_NOT_FOUND, "glyph %u is past the face's last glyph (%u)", glyph,
                           face->metrics.glyph_count - 1);
    const double ppem = (double)atlas->settings.pixels_per_em;
    const double range = (double)atlas->settings.distance_range;
    aptext_glyph_bounds bounds;
    if (APRESULT result = aptext_glyph_render(face, glyph, ppem, range, nullptr, &bounds, error))
        return result;
    // Refused before it is generated, not after.
    if (bounds.width > atlas->settings.page_width || bounds.height > atlas->settings.page_height)
        return aptext_fail(error, APRESULT_LIMIT, "glyph %u's image is %ux%u pixels, larger than a %ux%u page", glyph,
                           bounds.width, bounds.height, atlas->settings.page_width, atlas->settings.page_height);
    if (scratch.resize((size_t)bounds.width * bounds.height * 4) != EQ_SUCCESS)
        return aptext_out_of_memory(error, "generating the glyph");
    if (bounds.width && bounds.height)
    {
        if (APRESULT result = aptext_glyph_render(face, glyph, ppem, range, scratch.data, &bounds, error))
            return result;
    }
    const aptext_atlas_entry *entry = nullptr;
    return place_glyph(atlas, face, glyph, bounds, scratch.data, &entry, error);
}

// ---- Saving and loading -------------------------------------------------------------

// The bytes: little-endian throughout, floats as their bits.
//   "APTXATLS", format version, generator version,
//   pixels_per_em, distance_range, page_width, page_height, max_pages,
//   face count, then per face its kind and fingerprint (3 values),
//   page count, then per page its skyline (span count, then x, y, width
//   each) and its pixels,
//   glyph count, then per glyph its face's place in the list, glyph id,
//   page, x, y, width, height and plane bounds (4 floats),
//   and last the CRC-32 of everything before it.
static const uint8_t ATLAS_MAGIC[8] = {'A', 'P', 'T', 'X', 'A', 'T', 'L', 'S'};
static const uint32_t ATLAS_FORMAT_VERSION = 1;

struct writer
{
    uint8_t *data; // null while only counting
    uint64_t size;
};

static void put32(writer &w, uint32_t v)
{
    if (w.data)
    {
        w.data[w.size] = (uint8_t)v;
        w.data[w.size + 1] = (uint8_t)(v >> 8);
        w.data[w.size + 2] = (uint8_t)(v >> 16);
        w.data[w.size + 3] = (uint8_t)(v >> 24);
    }
    w.size += 4;
}

static void put_float(writer &w, float f)
{
    uint32_t v;
    memcpy(&v, &f, sizeof(v));
    put32(w, v);
}

static void put_bytes(writer &w, const void *bytes, uint64_t size)
{
    if (w.data)
        memcpy(w.data + w.size, bytes, (size_t)size);
    w.size += size;
}

static void write_atlas(const aptext_atlas_t *atlas, const aptext_face *faces, uint32_t face_count,
                        const uint32_t *entry_faces, writer &w)
{
    const APTEXT_ATLAS_DESC &s = atlas->settings;
    put_bytes(w, ATLAS_MAGIC, sizeof(ATLAS_MAGIC));
    put32(w, ATLAS_FORMAT_VERSION);
    put32(w, APTEXT_GENERATOR_VERSION);
    put_float(w, s.pixels_per_em);
    put_float(w, s.distance_range);
    put32(w, s.page_width);
    put32(w, s.page_height);
    put32(w, s.max_pages);
    put32(w, face_count);
    for (uint32_t i = 0; i < face_count; i++)
    {
        put32(w, (uint32_t)faces[i]->kind);
        for (uint32_t v : faces[i]->fingerprint)
            put32(w, v);
    }
    put32(w, (uint32_t)atlas->pages.count);
    const uint64_t page_size = (uint64_t)s.page_width * s.page_height * 4;
    for (const aptext_page &page : atlas->pages)
    {
        put32(w, (uint32_t)page.skyline.count);
        for (const aptext_skyline &span : page.skyline)
        {
            put32(w, span.x);
            put32(w, span.y);
            put32(w, span.width);
        }
        put_bytes(w, page.pixels, page_size);
    }
    put32(w, (uint32_t)atlas->entries.count);
    for (uint32_t i = 0; i < atlas->entries.count; i++)
    {
        const aptext_atlas_entry &e = atlas->entries[i];
        put32(w, entry_faces ? entry_faces[i] : 0);
        put32(w, e.glyph);
        put32(w, e.page);
        put32(w, e.x);
        put32(w, e.y);
        put32(w, e.width);
        put32(w, e.height);
        put_float(w, e.plane_left);
        put_float(w, e.plane_top);
        put_float(w, e.plane_right);
        put_float(w, e.plane_bottom);
    }
    uint32_t crc = w.data ? libdeflate_crc32(0, w.data, (size_t)w.size) : 0;
    put32(w, crc);
}

struct reader
{
    const uint8_t *data;
    uint64_t size;
    uint64_t at;
    bool short_read;
};

static uint32_t get32(reader &r)
{
    if (r.size - r.at < 4)
    {
        r.short_read = true;
        r.at = r.size;
        return 0;
    }
    const uint8_t *p = r.data + r.at;
    r.at += 4;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static float get_float(reader &r)
{
    uint32_t v = get32(r);
    float f;
    memcpy(&f, &v, sizeof(f));
    return f;
}

static APRESULT corrupt(APTEXT_ERROR *error, const char *what)
{
    return aptext_fail(error, APRESULT_CORRUPT_DATA, "the saved atlas is damaged: %s", what);
}

static APRESULT read_atlas(const APTEXT_ATLAS_LOAD_DESC &d, aptext_atlas_t *atlas, APTEXT_ERROR *error)
{
    const uint8_t *data = (const uint8_t *)d.data;
    if (d.data_size < sizeof(ATLAS_MAGIC) + 8)
        return corrupt(error, "too short to be one");
    if (memcmp(data, ATLAS_MAGIC, sizeof(ATLAS_MAGIC)) != 0)
        return corrupt(error, "it doesn't start as a saved atlas does");
    reader r = {data, d.data_size, sizeof(ATLAS_MAGIC), false};
    const uint32_t format = get32(r);
    if (format != ATLAS_FORMAT_VERSION)
        return aptext_fail(error, APRESULT_UNSUPPORTED, "the atlas was saved in format version %u; this build reads %u",
                           format, ATLAS_FORMAT_VERSION);
    if (d.data_size < r.at + 8)
        return corrupt(error, "too short to be one");
    const uint64_t body = d.data_size - 4;
    const uint32_t stored_crc = (uint32_t)data[body] | ((uint32_t)data[body + 1] << 8) |
                                ((uint32_t)data[body + 2] << 16) | ((uint32_t)data[body + 3] << 24);
    if (libdeflate_crc32(0, data, (size_t)body) != stored_crc)
        return corrupt(error, "its checksum doesn't match");
    r.size = body;

    const uint32_t generator = get32(r);
    if (generator != APTEXT_GENERATOR_VERSION)
        return aptext_fail(error, APRESULT_APTEXT_ATLAS_MISMATCH,
                           "the atlas was generated by generator version %u; this build is %u", generator,
                           APTEXT_GENERATOR_VERSION);
    const APTEXT_ATLAS_DESC &want = *d.atlas;
    const float ppem = get_float(r);
    const float range = get_float(r);
    const uint32_t page_width = get32(r);
    const uint32_t page_height = get32(r);
    const uint32_t max_pages = get32(r);
    if (r.short_read)
        return corrupt(error, "it ends in its settings");
    if (memcmp(&ppem, &want.pixels_per_em, sizeof(float)) != 0)
        return aptext_fail(error, APRESULT_APTEXT_ATLAS_MISMATCH, "the atlas was saved at %g pixels per em, not %g",
                           (double)ppem, (double)want.pixels_per_em);
    if (memcmp(&range, &want.distance_range, sizeof(float)) != 0)
        return aptext_fail(error, APRESULT_APTEXT_ATLAS_MISMATCH, "the atlas was saved with a distance range of %g, not %g",
                           (double)range, (double)want.distance_range);
    if (page_width != want.page_width)
        return aptext_fail(error, APRESULT_APTEXT_ATLAS_MISMATCH, "the atlas was saved with pages %u wide, not %u",
                           page_width, want.page_width);
    if (page_height != want.page_height)
        return aptext_fail(error, APRESULT_APTEXT_ATLAS_MISMATCH, "the atlas was saved with pages %u high, not %u",
                           page_height, want.page_height);
    if (max_pages != want.max_pages)
        return aptext_fail(error, APRESULT_APTEXT_ATLAS_MISMATCH, "the atlas was saved with max_pages %u, not %u",
                           max_pages, want.max_pages);

    const uint32_t face_count = get32(r);
    if (r.short_read)
        return corrupt(error, "it ends before its faces");
    if (face_count != d.face_count)
        return aptext_fail(error, APRESULT_APTEXT_ATLAS_MISMATCH, "the atlas was saved with %u faces, not %u", face_count,
                           d.face_count);
    for (uint32_t i = 0; i < face_count; i++)
    {
        const uint32_t kind = get32(r);
        uint32_t fingerprint[3];
        for (uint32_t &v : fingerprint)
            v = get32(r);
        if (r.short_read)
            return corrupt(error, "it ends in its faces");
        const aptext_face face = d.faces[i];
        if (kind != (uint32_t)face->kind || memcmp(fingerprint, face->fingerprint, sizeof(fingerprint)) != 0)
            return aptext_fail(error, APRESULT_APTEXT_ATLAS_MISMATCH,
                               "faces[%u] is not the face the atlas was saved with in its place", i);
    }

    const uint32_t page_count = get32(r);
    if (r.short_read)
        return corrupt(error, "it ends before its pages");
    if (page_count > max_pages)
        return corrupt(error, "it has more pages than its max_pages");
    const uint64_t page_size = (uint64_t)page_width * page_height * 4;
    for (uint32_t p = 0; p < page_count; p++)
    {
        const uint32_t spans = get32(r);
        if (r.short_read)
            return corrupt(error, "it ends in its pages");
        if (spans == 0 || spans > page_width)
            return corrupt(error, "a page's skyline is not one");
        if ((r.size - r.at) / 12 < spans)
            return corrupt(error, "it ends in a page's skyline");
        if (APRESULT result = add_page(atlas, error))
            return result;
        aptext_page &page = atlas->pages[p];
        if (page.skyline.resize(spans) != EQ_SUCCESS)
            return aptext_out_of_memory(error, "loading the atlas");
        uint32_t expected_x = 0;
        for (uint32_t i = 0; i < spans; i++)
        {
            aptext_skyline &span = page.skyline[i];
            span.x = get32(r);
            span.y = get32(r);
            span.width = get32(r);
            if (span.x != expected_x || span.width == 0 || span.width > page_width - span.x || span.y > page_height)
                return corrupt(error, "a page's skyline is not one");
            expected_x = span.x + span.width;
        }
        if (expected_x != page_width)
            return corrupt(error, "a page's skyline doesn't span it");
        if (r.size - r.at < page_size)
            return corrupt(error, "it ends in a page's pixels");
        memcpy(page.pixels, data + r.at, (size_t)page_size);
        r.at += page_size;
        mark_dirty(page, 0, 0, page_width, page_height);
    }

    const uint32_t glyph_count = get32(r);
    if (r.short_read)
        return corrupt(error, "it ends before its glyphs");
    if ((r.size - r.at) / 44 < glyph_count)
        return corrupt(error, "it ends in its glyphs");
    if (atlas->entries.reserve(glyph_count) != EQ_SUCCESS)
        return aptext_out_of_memory(error, "loading the atlas");
    for (uint32_t i = 0; i < glyph_count; i++)
    {
        aptext_atlas_entry e = {};
        const uint32_t face_index = get32(r);
        e.glyph = get32(r);
        e.page = get32(r);
        e.x = get32(r);
        e.y = get32(r);
        e.width = get32(r);
        e.height = get32(r);
        e.plane_left = get_float(r);
        e.plane_top = get_float(r);
        e.plane_right = get_float(r);
        e.plane_bottom = get_float(r);
        if (face_index >= face_count)
            return corrupt(error, "a glyph names a face past the last");
        e.face = d.faces[face_index];
        if (e.face->kind != APTEXT_FACE_KIND_OUTLINE || e.glyph >= e.face->metrics.glyph_count)
            return corrupt(error, "a glyph isn't one of its face's");
        if (e.width || e.height)
        {
            if (e.page >= page_count)
                return corrupt(error, "a glyph is on a page past the last");
            if (e.width > page_width || e.x > page_width - e.width || e.height > page_height ||
                e.y > page_height - e.height)
                return corrupt(error, "a glyph lies outside its page");
        }
        if (find_entry(atlas, e.face, e.glyph))
            return corrupt(error, "a glyph is listed twice");
        (void)atlas->entries.push(e);
        atlas->index.add(entry_hash(e.face, e.glyph), i);
    }
    if (r.at != r.size)
        return corrupt(error, "it has bytes after its glyphs");
    return APRESULT_OK;
}

// ---- Entry points -------------------------------------------------------------------

extern "C"
{
    APRESULT aptext_generate_glyph(const APTEXT_GLYPH_GENERATE_DESC *desc, void *pixels, uint64_t pixels_size,
                                   APTEXT_GLYPH_IMAGE *out_image, APTEXT_ERROR *out_error)
    {
        aptext_clear_error(out_error);
        if (!desc)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is null");
        if (APRESULT result = apri_struct_check(&desc->base, APRI_STRUCT_TYPE_APTEXT_GLYPH_GENERATE_DESC))
            return aptext_fail(out_error, result, "desc %s", apri_struct_check_message(result));
        if (APRESULT result = check_generate_desc(*desc, out_error))
            return result;
        if (aptext_missing(pixels, pixels_size))
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "pixels is null with a pixels_size of %llu",
                               (unsigned long long)pixels_size);
        if (!out_image)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_image is null");
        if (APRESULT result = apri_struct_out_check(&out_image->base, APRI_STRUCT_TYPE_APTEXT_GLYPH_IMAGE))
            return aptext_fail(out_error, result, "out_image %s", apri_struct_check_message(result));
        if (APRESULT result = check_generate_face(*desc, out_error))
            return result;
        try
        {
            const double ppem = (double)desc->pixels_per_em;
            const double range = (double)desc->distance_range;
            aptext_glyph_bounds bounds;
            if (APRESULT result = aptext_glyph_render(desc->face, desc->glyph, ppem, range, nullptr, &bounds, out_error))
                return result;
            const uint64_t needed = (uint64_t)bounds.width * bounds.height * 4;
            if (pixels_size != 0 && pixels_size < needed)
                return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT,
                                   "pixels_size is %llu bytes, and the %ux%u image needs %llu",
                                   (unsigned long long)pixels_size, bounds.width, bounds.height,
                                   (unsigned long long)needed);
            if (pixels_size != 0 && needed != 0)
            {
                if (APRESULT result =
                        aptext_glyph_render(desc->face, desc->glyph, ppem, range, (uint8_t *)pixels, &bounds, out_error))
                    return result;
            }
            out_image->width = bounds.width;
            out_image->height = bounds.height;
            out_image->plane_left = bounds.plane_left;
            out_image->plane_top = bounds.plane_top;
            out_image->plane_right = bounds.plane_right;
            out_image->plane_bottom = bounds.plane_bottom;
            return APRESULT_OK;
        }
        catch (const std::bad_alloc &)
        {
            return aptext_out_of_memory(out_error, "generating the glyph");
        }
    }

    APRESULT aptext_atlas_create(const APTEXT_ATLAS_DESC *desc, aptext_atlas *out_atlas, APTEXT_ERROR *out_error)
    {
        aptext_clear_error(out_error);
        if (!out_atlas)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_atlas is null");
        *out_atlas = nullptr;
        if (!desc)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is null");
        if (APRESULT result = check_atlas_desc(desc, "desc", out_error))
            return result;
        aptext_atlas_t *atlas = new_atlas(*desc);
        if (!atlas)
            return aptext_out_of_memory(out_error, "creating the atlas");
        *out_atlas = atlas;
        return APRESULT_OK;
    }

    APRESULT aptext_atlas_get_info(aptext_atlas atlas, APTEXT_ATLAS_INFO *out_info)
    {
        if (!atlas)
            return APRESULT_INVALID_ARGUMENT;
        if (APRI_STALE(atlas, "aptext_atlas_get_info", "atlas"))
            return APRESULT_INVALID_ARGUMENT;
        APRI_ENTER(atlas->overlap, "aptext", "aptext_atlas_get_info", "atlas");
        if (!out_info)
            return APRESULT_INVALID_ARGUMENT;
        if (APRESULT result = apri_struct_out_check(&out_info->base, APRI_STRUCT_TYPE_APTEXT_ATLAS_INFO))
            return result;
        out_info->pixels_per_em = atlas->settings.pixels_per_em;
        out_info->distance_range = atlas->settings.distance_range;
        out_info->page_width = atlas->settings.page_width;
        out_info->page_height = atlas->settings.page_height;
        out_info->page_count = (uint32_t)atlas->pages.count;
        out_info->glyph_count = (uint32_t)atlas->entries.count;
        uint32_t dirty = 0;
        for (const aptext_page &page : atlas->pages)
            dirty += page.dirty;
        out_info->dirty_rect_count = dirty;
        return APRESULT_OK;
    }

    APRESULT aptext_atlas_insert(aptext_atlas atlas, const APTEXT_GLYPH_GENERATE_DESC *desc, const void *pixels,
                                 uint64_t pixels_size, APTEXT_ATLAS_GLYPH *out_glyph, APTEXT_ERROR *out_error)
    {
        aptext_clear_error(out_error);
        if (!atlas)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "atlas is null");
        if (APRI_STALE(atlas, "aptext_atlas_insert", "atlas"))
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "atlas is destroyed");
        if (!desc)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is null");
        if (APRESULT result = apri_struct_check(&desc->base, APRI_STRUCT_TYPE_APTEXT_GLYPH_GENERATE_DESC))
            return aptext_fail(out_error, result, "desc %s", apri_struct_check_message(result));
        if (APRESULT result = check_generate_desc(*desc, out_error))
            return result;
        if (aptext_missing(pixels, pixels_size))
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "pixels is null with a pixels_size of %llu",
                               (unsigned long long)pixels_size);
        if (out_glyph)
        {
            if (APRESULT result = apri_struct_out_check(&out_glyph->base, APRI_STRUCT_TYPE_APTEXT_ATLAS_GLYPH))
                return aptext_fail(out_error, result, "out_glyph %s", apri_struct_check_message(result));
        }
        if (desc->pixels_per_em != atlas->settings.pixels_per_em)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT,
                               "desc's pixels_per_em is %g, and the atlas's %g", (double)desc->pixels_per_em,
                               (double)atlas->settings.pixels_per_em);
        if (desc->distance_range != atlas->settings.distance_range)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT,
                               "desc's distance_range is %g, and the atlas's %g", (double)desc->distance_range,
                               (double)atlas->settings.distance_range);
        if (APRESULT result = check_generate_face(*desc, out_error))
            return result;
        try
        {
            aptext_glyph_bounds bounds;
            if (APRESULT result = aptext_glyph_render(desc->face, desc->glyph, (double)desc->pixels_per_em,
                                                      (double)desc->distance_range, nullptr, &bounds, out_error))
                return result;
            const uint64_t needed = (uint64_t)bounds.width * bounds.height * 4;
            if (pixels_size != needed)
                return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT,
                                   "pixels_size is %llu bytes, and the glyph's %ux%u image is %llu",
                                   (unsigned long long)pixels_size, bounds.width, bounds.height,
                                   (unsigned long long)needed);
            const aptext_atlas_entry *entry = find_entry(atlas, desc->face, desc->glyph);
            if (!entry)
            {
                if (APRESULT result = place_glyph(atlas, desc->face, desc->glyph, bounds, (const uint8_t *)pixels,
                                                  &entry, out_error))
                    return result;
            }
            if (out_glyph)
                fill_atlas_glyph(*entry, out_glyph);
            return APRESULT_OK;
        }
        catch (const std::bad_alloc &)
        {
            return aptext_out_of_memory(out_error, "inserting the glyph");
        }
    }

    APRESULT aptext_atlas_add_layout(aptext_atlas atlas, aptext_layout layout, APTEXT_ERROR *out_error)
    {
        aptext_clear_error(out_error);
        if (!atlas)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "atlas is null");
        if (APRI_STALE(atlas, "aptext_atlas_add_layout", "atlas"))
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "atlas is destroyed");
        if (!layout)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "layout is null");
        if (APRI_STALE(layout, "aptext_atlas_add_layout", "layout"))
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "layout is destroyed");
        if (!layout->chain)
            return APRESULT_OK; // empty: nothing to add
        if (layout->chain->kind != APTEXT_FACE_KIND_OUTLINE)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT,
                               "layout was built with stroke faces, which have no distance fields");
        try
        {
            eq::array<uint8_t> scratch;
            for (const APTEXT_GLYPH &g : layout->glyphs)
            {
                if (APRESULT result = add_glyph(atlas, layout->chain->faces[g.face_index], g.glyph, scratch, out_error))
                    return result;
            }
            return APRESULT_OK;
        }
        catch (const std::bad_alloc &)
        {
            return aptext_out_of_memory(out_error, "adding the layout's glyphs");
        }
    }

    APRESULT aptext_atlas_find_glyph(aptext_atlas atlas, aptext_face face, uint32_t glyph,
                                     APTEXT_ATLAS_GLYPH *out_glyph)
    {
        if (!atlas)
            return APRESULT_INVALID_ARGUMENT;
        if (APRI_STALE(atlas, "aptext_atlas_find_glyph", "atlas"))
            return APRESULT_INVALID_ARGUMENT;
        APRI_ENTER(atlas->overlap, "aptext", "aptext_atlas_find_glyph", "atlas");
        if (!face)
            return APRESULT_INVALID_ARGUMENT;
        if (APRI_STALE(face, "aptext_atlas_find_glyph", "face"))
            return APRESULT_INVALID_ARGUMENT;
        if (!out_glyph)
            return APRESULT_INVALID_ARGUMENT;
        if (APRESULT result = apri_struct_out_check(&out_glyph->base, APRI_STRUCT_TYPE_APTEXT_ATLAS_GLYPH))
            return result;
        const aptext_atlas_entry *entry = find_entry(atlas, face, glyph);
        if (!entry)
            return APRESULT_NOT_FOUND;
        fill_atlas_glyph(*entry, out_glyph);
        return APRESULT_OK;
    }

    APRESULT aptext_atlas_get_page(aptext_atlas atlas, uint32_t page, const void **out_pixels)
    {
        if (!atlas)
            return APRESULT_INVALID_ARGUMENT;
        if (APRI_STALE(atlas, "aptext_atlas_get_page", "atlas"))
            return APRESULT_INVALID_ARGUMENT;
        APRI_ENTER(atlas->overlap, "aptext", "aptext_atlas_get_page", "atlas");
        if (!out_pixels)
            return APRESULT_INVALID_ARGUMENT;
        *out_pixels = nullptr;
        if (page >= atlas->pages.count)
            return APRESULT_NOT_FOUND;
        *out_pixels = atlas->pages[page].pixels;
        return APRESULT_OK;
    }

    APRESULT aptext_atlas_take_dirty(aptext_atlas atlas, APTEXT_DIRTY_RECT *rects, uint32_t capacity,
                                     uint32_t *out_count)
    {
        if (!atlas)
            return APRESULT_INVALID_ARGUMENT;
        if (APRI_STALE(atlas, "aptext_atlas_take_dirty", "atlas"))
            return APRESULT_INVALID_ARGUMENT;
        APRI_ENTER(atlas->overlap, "aptext", "aptext_atlas_take_dirty", "atlas");
        if (aptext_missing(rects, capacity))
            return APRESULT_INVALID_ARGUMENT;
        if (!out_count)
            return APRESULT_INVALID_ARGUMENT;
        uint32_t taken = 0;
        for (uint32_t p = 0; p < atlas->pages.count && taken < capacity; p++)
        {
            aptext_page &page = atlas->pages[p];
            if (!page.dirty)
                continue;
            APTEXT_DIRTY_RECT &rect = rects[taken++];
            rect.page = p;
            rect.x = page.x0;
            rect.y = page.y0;
            rect.width = page.x1 - page.x0;
            rect.height = page.y1 - page.y0;
            page.dirty = 0;
        }
        *out_count = taken;
        return APRESULT_OK;
    }

    APRESULT aptext_atlas_save(aptext_atlas atlas, const aptext_face *faces, uint32_t face_count, void *buffer,
                               uint64_t capacity, uint64_t *out_size, APTEXT_ERROR *out_error)
    {
        aptext_clear_error(out_error);
        if (!atlas)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "atlas is null");
        if (APRI_STALE(atlas, "aptext_atlas_save", "atlas"))
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "atlas is destroyed");
        APRI_ENTER(atlas->overlap, "aptext", "aptext_atlas_save", "atlas");
        if (aptext_missing(faces, face_count))
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "faces is null with a face_count of %u", face_count);
        for (uint32_t i = 0; i < face_count; i++)
        {
            if (!faces[i])
                return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "faces[%u] is null", i);
            for (uint32_t j = 0; j < i; j++)
            {
                if (faces[i] == faces[j])
                    return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "faces[%u] is faces[%u] again", i, j);
            }
        }
        if (aptext_missing(buffer, capacity))
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "buffer is null with a capacity of %llu",
                               (unsigned long long)capacity);
        if (!out_size)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_size is null");
        *out_size = 0;
        // Each glyph's face, as its place in [faces].
        eq::array<uint32_t> entry_faces;
        if (entry_faces.resize(atlas->entries.count) != EQ_SUCCESS)
            return aptext_out_of_memory(out_error, "saving the atlas");
        for (uint32_t i = 0; i < atlas->entries.count; i++)
        {
            const aptext_face face = atlas->entries[i].face;
            uint32_t place = face_count;
            for (uint32_t f = 0; f < face_count && place == face_count; f++)
            {
                if (faces[f] == face)
                    place = f;
            }
            if (place == face_count)
                return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT,
                                   "the atlas holds glyph %u of a face that isn't in faces", atlas->entries[i].glyph);
            entry_faces[i] = place;
        }
        writer counter = {nullptr, 0};
        write_atlas(atlas, faces, face_count, entry_faces.data, counter);
        *out_size = counter.size;
        if (capacity == 0)
            return APRESULT_OK;
        if (capacity < counter.size)
            return aptext_fail(out_error, APRESULT_LIMIT, "capacity is %llu bytes, and the atlas needs %llu",
                               (unsigned long long)capacity, (unsigned long long)counter.size);
        writer out = {(uint8_t *)buffer, 0};
        write_atlas(atlas, faces, face_count, entry_faces.data, out);
        return APRESULT_OK;
    }

    APRESULT aptext_atlas_load(const APTEXT_ATLAS_LOAD_DESC *desc, aptext_atlas *out_atlas, APTEXT_ERROR *out_error)
    {
        aptext_clear_error(out_error);
        if (!out_atlas)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_atlas is null");
        *out_atlas = nullptr;
        if (!desc)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is null");
        if (APRESULT result = apri_struct_check(&desc->base, APRI_STRUCT_TYPE_APTEXT_ATLAS_LOAD_DESC))
            return aptext_fail(out_error, result, "desc %s", apri_struct_check_message(result));
        const APTEXT_ATLAS_LOAD_DESC &d = *desc;
        if (!d.atlas)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's atlas is null");
        if (APRESULT result = check_atlas_desc(d.atlas, "desc's atlas", out_error))
            return result;
        if (!d.data)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's data is null");
        if (d.data_size == 0)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's data_size is 0");
        if (aptext_missing(d.faces, d.face_count))
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's faces is null with a face_count of %u",
                               d.face_count);
        for (uint32_t i = 0; i < d.face_count; i++)
        {
            if (!d.faces[i])
                return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "faces[%u] is null", i);
        }
        aptext_atlas_t *atlas = new_atlas(*d.atlas);
        if (!atlas)
            return aptext_out_of_memory(out_error, "loading the atlas");
        APRESULT result;
        try
        {
            result = read_atlas(d, atlas, out_error);
        }
        catch (const std::bad_alloc &)
        {
            result = aptext_out_of_memory(out_error, "loading the atlas");
        }
        if (result != APRESULT_OK)
        {
            delete_atlas(atlas);
            return result;
        }
        *out_atlas = atlas;
        return APRESULT_OK;
    }

    void aptext_atlas_destroy(aptext_atlas atlas)
    {
        if (!atlas)
            return;
        if (APRI_STALE(atlas, "aptext_atlas_destroy", "atlas"))
            return;
        delete_atlas(atlas);
    }
}
