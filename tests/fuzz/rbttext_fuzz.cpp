// rbttext_fuzz - Rabbittus Text's readers of outside bytes: font files
// (rbttext_face_create(), through HarfBuzz, with and without variations),
// Hershey, LFF and SHX fonts (rbttext_face_create_hershey(), _lff(), _shx())
// and saved atlases (rbttext_atlas_load()).
//
// The input's first byte says which; the rest is what it reads:
//   0: a font file. A face that opens is laid out, and the glyphs of the
//      layout are generated into an atlas, which is saved and loaded again.
//   1: a Hershey font, from U+0020. A face that opens is laid out, and its
//      strokes taken.
//   2: a font file's size (4 bytes, little-endian), the font, then a saved
//      atlas, loaded with that face at fixed settings (the corpus's).
//   3: an LFF font; 4: an SHX font (shape 128 as U+00E9). A face that
//      opens is laid out, and its strokes taken, as Hershey's.
//   5: a font file opened again with every variation axis it has at its
//      most, then laid out.
// Beyond not crashing, it checks:
// - a failed create or load leaves no object and a terminated message;
// - a layout's glyphs, lines and runs index each other and the text;
// - a stroke layout's segment count is what get_strokes() fills;
// - an atlas saved and loaded again has the same glyphs and pages.

#include "rbtfuzz.hpp"

#include "text/rbttext.h"

#include <string.h>
#include <vector>

namespace
{
const char LAYOUT_TEXT[] = "fi AV az\n\xD7\x90\xD7\x91 a\xE4\xB8\x80 aa\xE2\x80\x8D"
                           "aa aa";

// Small, so a fuzzed font's glyphs generate quickly.
constexpr float PIXELS_PER_EM = 8.0f;
constexpr float DISTANCE_RANGE = 2.0f;
constexpr uint32_t PAGE = 64;
constexpr uint32_t MAX_PAGES = 16;

bool message_is_terminated(const RBTTEXT_ERROR &error)
{
    return memchr(error.message, '\0', sizeof(error.message)) != nullptr;
}

RBTTEXT_ATLAS_DESC atlas_settings(float pixels_per_em, float distance_range)
{
    RBTTEXT_ATLAS_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_TEXT_ATLAS_DESC;
    desc.pixels_per_em = pixels_per_em;
    desc.distance_range = distance_range;
    desc.page_width = PAGE;
    desc.page_height = PAGE;
    desc.max_pages = MAX_PAGES;
    return desc;
}

// Lays [face] out in a chain of its own and checks the layout; the layout,
// or null if the build failed.
rbttext_layout lay_out(rbttext_face face, rbttext_chain *out_chain)
{
    RBTTEXT_CHAIN_DESC chain_desc;
    memset(&chain_desc, 0, sizeof(chain_desc));
    chain_desc.base.type = RBT_STRUCT_TYPE_TEXT_CHAIN_DESC;
    chain_desc.faces = &face;
    chain_desc.face_count = 1;
    RBTFUZZ_CHECK(rbttext_chain_create(&chain_desc, out_chain, nullptr) == RBT_RESULT_OK);

    rbttext_layout layout = nullptr;
    RBTFUZZ_CHECK(rbttext_layout_create(&layout) == RBT_RESULT_OK);
    RBTTEXT_LAYOUT_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_TEXT_LAYOUT_DESC;
    desc.chain = *out_chain;
    desc.text = LAYOUT_TEXT;
    desc.text_size = (uint32_t)strlen(LAYOUT_TEXT);
    desc.direction = RBTTEXT_DIRECTION_AUTO;
    desc.features = RBTTEXT_FEATURE_KERNING | RBTTEXT_FEATURE_LIGATURES | RBTTEXT_FEATURE_CONTEXTUAL_ALTERNATES;
    desc.em_size = 16.0f;
    desc.max_width = 48.0f;
    desc.align = RBTTEXT_ALIGN_START;
    desc.line_spacing = 1.0f;
    desc.letter_spacing = 0.5f;
    RBTTEXT_ERROR error;
    RBT_RESULT result = rbttext_layout_build(layout, &desc, &error);
    RBTFUZZ_CHECK(message_is_terminated(error));
    if (result != RBT_RESULT_OK)
    {
        RBTFUZZ_CHECK(result == RBT_RESULT_OUT_OF_MEMORY);
        return layout;
    }

    const RBTTEXT_GLYPH *glyphs = nullptr;
    const RBTTEXT_LINE *lines = nullptr;
    const RBTTEXT_RUN *runs = nullptr;
    uint32_t glyph_count = 0, line_count = 0, run_count = 0;
    RBTFUZZ_CHECK(rbttext_layout_get_glyphs(layout, &glyphs, &glyph_count) == RBT_RESULT_OK);
    RBTFUZZ_CHECK(rbttext_layout_get_lines(layout, &lines, &line_count) == RBT_RESULT_OK);
    RBTFUZZ_CHECK(rbttext_layout_get_runs(layout, &runs, &run_count) == RBT_RESULT_OK);
    RBTFUZZ_CHECK(line_count >= 1);
    for (uint32_t i = 0; i < glyph_count; i++)
    {
        const RBTTEXT_GLYPH &g = glyphs[i];
        RBTFUZZ_CHECK(g.face_index == 0);
        RBTFUZZ_CHECK(g.line < line_count);
        RBTFUZZ_CHECK(g.run < run_count);
        RBTFUZZ_CHECK(g.text_size > 0);
        RBTFUZZ_CHECK(g.text_offset + g.text_size <= desc.text_size);
        RBTFUZZ_CHECK(g.advance >= 0.0f);
    }
    uint32_t next_glyph = 0;
    for (uint32_t l = 0; l < line_count; l++)
    {
        RBTFUZZ_CHECK(lines[l].first_glyph == next_glyph);
        next_glyph += lines[l].glyph_count;
        RBTFUZZ_CHECK(lines[l].text_offset + lines[l].text_size <= desc.text_size);
        RBTFUZZ_CHECK(lines[l].box_left <= lines[l].box_right);
    }
    RBTFUZZ_CHECK(next_glyph == glyph_count);
    for (uint32_t r = 0; r < run_count; r++)
    {
        RBTFUZZ_CHECK(runs[r].glyph_count > 0);
        RBTFUZZ_CHECK(runs[r].first_glyph + runs[r].glyph_count <= glyph_count);
        RBTFUZZ_CHECK(runs[r].line < line_count);
    }
    return layout;
}

void fuzz_font(const uint8_t *data, size_t size)
{
    RBTTEXT_FACE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_TEXT_FACE_DESC;
    desc.data = data;
    desc.data_size = size;
    rbttext_face face = (rbttext_face)&desc; // must be overwritten
    RBTTEXT_ERROR error;
    RBT_RESULT result = rbttext_face_create(&desc, &face, &error);
    RBTFUZZ_CHECK(message_is_terminated(error));
    if (result != RBT_RESULT_OK)
    {
        RBTFUZZ_CHECK(face == nullptr);
        return;
    }
    RBTTEXT_FACE_METRICS metrics;
    memset(&metrics, 0, sizeof(metrics));
    metrics.base.type = RBT_STRUCT_TYPE_TEXT_FACE_METRICS;
    RBTFUZZ_CHECK(rbttext_face_get_metrics(face, &metrics) == RBT_RESULT_OK);
    RBTFUZZ_CHECK(metrics.glyph_count > 0);

    rbttext_chain chain = nullptr;
    rbttext_layout layout = lay_out(face, &chain);

    RBTTEXT_ATLAS_DESC settings = atlas_settings(PIXELS_PER_EM, DISTANCE_RANGE);
    rbttext_atlas atlas = nullptr;
    RBTFUZZ_CHECK(rbttext_atlas_create(&settings, &atlas, nullptr) == RBT_RESULT_OK);
    result = rbttext_atlas_add_layout(atlas, layout, &error);
    RBTFUZZ_CHECK(message_is_terminated(error));
    if (result == RBT_RESULT_OK)
    {
        uint64_t saved_size = 0;
        RBTFUZZ_CHECK(rbttext_atlas_save(atlas, &face, 1, nullptr, 0, &saved_size, nullptr) == RBT_RESULT_OK);
        std::vector<uint8_t> saved((size_t)saved_size);
        RBTFUZZ_CHECK(rbttext_atlas_save(atlas, &face, 1, saved.data(), saved_size, &saved_size, nullptr) ==
                       RBT_RESULT_OK);
        RBTTEXT_ATLAS_LOAD_DESC load;
        memset(&load, 0, sizeof(load));
        load.base.type = RBT_STRUCT_TYPE_TEXT_ATLAS_LOAD_DESC;
        load.atlas = &settings;
        load.data = saved.data();
        load.data_size = saved_size;
        load.faces = &face;
        load.face_count = 1;
        rbttext_atlas loaded = nullptr;
        result = rbttext_atlas_load(&load, &loaded, &error);
        if (result != RBT_RESULT_OK)
            fprintf(stderr, "rbttext_fuzz: an atlas saved doesn't load: %s\n", error.message);
        RBTFUZZ_CHECK(result == RBT_RESULT_OK);
        RBTTEXT_ATLAS_INFO a, b;
        memset(&a, 0, sizeof(a));
        a.base.type = RBT_STRUCT_TYPE_TEXT_ATLAS_INFO;
        b = a;
        RBTFUZZ_CHECK(rbttext_atlas_get_info(atlas, &a) == RBT_RESULT_OK);
        RBTFUZZ_CHECK(rbttext_atlas_get_info(loaded, &b) == RBT_RESULT_OK);
        RBTFUZZ_CHECK(a.glyph_count == b.glyph_count);
        RBTFUZZ_CHECK(a.page_count == b.page_count);
        for (uint32_t p = 0; p < a.page_count; p++)
        {
            const void *x = nullptr;
            const void *y = nullptr;
            RBTFUZZ_CHECK(rbttext_atlas_get_page(atlas, p, &x) == RBT_RESULT_OK);
            RBTFUZZ_CHECK(rbttext_atlas_get_page(loaded, p, &y) == RBT_RESULT_OK);
            RBTFUZZ_CHECK(memcmp(x, y, (size_t)PAGE * PAGE * 4) == 0);
        }
        rbttext_atlas_destroy(loaded);
    }
    else
    {
        RBTFUZZ_CHECK(result == RBT_RESULT_LIMIT || result == RBT_RESULT_INVALID_ARGUMENT ||
                       result == RBT_RESULT_OUT_OF_MEMORY);
    }
    rbttext_atlas_destroy(atlas);
    rbttext_layout_destroy(layout);
    rbttext_chain_destroy(chain);
    rbttext_face_destroy(face);
}

void fuzz_hershey(const uint8_t *data, size_t size)
{
    RBTTEXT_HERSHEY_FACE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_TEXT_HERSHEY_FACE_DESC;
    desc.data = data;
    desc.data_size = size;
    desc.first_codepoint = 0x20;
    desc.units_per_em = 32.0f;
    desc.baseline = 9.0f;
    desc.metrics.ascender = 0.75f;
    desc.metrics.descender = -0.25f;
    rbttext_face face = (rbttext_face)&desc; // must be overwritten
    RBTTEXT_ERROR error;
    RBT_RESULT result = rbttext_face_create_hershey(&desc, &face, &error);
    RBTFUZZ_CHECK(message_is_terminated(error));
    if (result != RBT_RESULT_OK)
    {
        RBTFUZZ_CHECK(face == nullptr);
        return;
    }
    rbttext_chain chain = nullptr;
    rbttext_layout layout = lay_out(face, &chain);
    uint32_t count = 0;
    if (rbttext_layout_get_strokes(layout, nullptr, 0, &count) == RBT_RESULT_OK && count < (1u << 20))
    {
        std::vector<RBTTEXT_SEGMENT> segments(count ? count : 1);
        uint32_t filled = 0;
        RBTFUZZ_CHECK(rbttext_layout_get_strokes(layout, count ? segments.data() : nullptr, count, &filled) ==
                       RBT_RESULT_OK);
        RBTFUZZ_CHECK(filled == count);
    }
    rbttext_layout_destroy(layout);
    rbttext_chain_destroy(chain);
    rbttext_face_destroy(face);
}

// A stroke face that opened: laid out, and its strokes taken.
void lay_out_strokes(rbttext_face face)
{
    rbttext_chain chain = nullptr;
    rbttext_layout layout = lay_out(face, &chain);
    uint32_t count = 0;
    if (rbttext_layout_get_strokes(layout, nullptr, 0, &count) == RBT_RESULT_OK && count < (1u << 20))
    {
        std::vector<RBTTEXT_SEGMENT> segments(count ? count : 1);
        uint32_t filled = 0;
        RBTFUZZ_CHECK(rbttext_layout_get_strokes(layout, count ? segments.data() : nullptr, count, &filled) ==
                       RBT_RESULT_OK);
        RBTFUZZ_CHECK(filled == count);
    }
    rbttext_layout_destroy(layout);
    rbttext_chain_destroy(chain);
    rbttext_face_destroy(face);
}

void fuzz_lff(const uint8_t *data, size_t size)
{
    RBTTEXT_LFF_FACE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_TEXT_LFF_FACE_DESC;
    desc.data = data;
    desc.data_size = size;
    desc.units_per_em = 9.0f;
    desc.arc_tolerance = 0.01f;
    desc.metrics.ascender = 1.0f;
    desc.metrics.descender = -0.25f;
    rbttext_face face = (rbttext_face)&desc; // must be overwritten
    RBTTEXT_ERROR error;
    RBT_RESULT result = rbttext_face_create_lff(&desc, &face, &error);
    RBTFUZZ_CHECK(message_is_terminated(error));
    if (result != RBT_RESULT_OK)
    {
        RBTFUZZ_CHECK(face == nullptr);
        return;
    }
    lay_out_strokes(face);
}

void fuzz_shx(const uint8_t *data, size_t size)
{
    static const uint32_t high[1] = {0xE9};
    RBTTEXT_SHX_FACE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_TEXT_SHX_FACE_DESC;
    desc.data = data;
    desc.data_size = size;
    desc.high_codepoints = high;
    desc.high_codepoint_count = 1;
    desc.arc_tolerance = 0.01f;
    rbttext_face face = (rbttext_face)&desc; // must be overwritten
    RBTTEXT_ERROR error;
    RBT_RESULT result = rbttext_face_create_shx(&desc, &face, &error);
    RBTFUZZ_CHECK(message_is_terminated(error));
    if (result != RBT_RESULT_OK)
    {
        RBTFUZZ_CHECK(face == nullptr);
        return;
    }
    lay_out_strokes(face);
}

void fuzz_variable(const uint8_t *data, size_t size)
{
    RBTTEXT_FACE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_TEXT_FACE_DESC;
    desc.data = data;
    desc.data_size = size;
    rbttext_face face = nullptr;
    if (rbttext_face_create(&desc, &face, nullptr) != RBT_RESULT_OK)
        return;
    const uint32_t count = rbttext_face_get_axis_count(face);
    std::vector<RBTTEXT_VARIATION> variations;
    for (uint32_t i = 0; i < count && i < 64; i++)
    {
        RBTTEXT_AXIS axis;
        RBTFUZZ_CHECK(rbttext_face_get_axis(face, i, &axis) == RBT_RESULT_OK);
        RBTFUZZ_CHECK(!(axis.min_value > axis.max_value));
        bool seen = false;
        for (const RBTTEXT_VARIATION &v : variations)
            seen = seen || v.tag == axis.tag;
        if (!seen && axis.max_value == axis.max_value)
            variations.push_back({axis.tag, axis.max_value});
    }
    rbttext_face_destroy(face);
    if (variations.empty())
        return;
    desc.variations = variations.data();
    desc.variation_count = (uint32_t)variations.size();
    face = nullptr;
    RBTTEXT_ERROR error;
    if (rbttext_face_create(&desc, &face, &error) != RBT_RESULT_OK)
    {
        RBTFUZZ_CHECK(face == nullptr);
        RBTFUZZ_CHECK(message_is_terminated(error));
        return;
    }
    rbttext_chain chain = nullptr;
    rbttext_layout layout = lay_out(face, &chain);
    rbttext_layout_destroy(layout);
    rbttext_chain_destroy(chain);
    rbttext_face_destroy(face);
}

void fuzz_atlas(const uint8_t *data, size_t size)
{
    if (size < 4)
        return;
    const uint32_t font_size = (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) |
                               ((uint32_t)data[3] << 24);
    if (font_size == 0 || font_size > size - 4)
        return;
    RBTTEXT_FACE_DESC face_desc;
    memset(&face_desc, 0, sizeof(face_desc));
    face_desc.base.type = RBT_STRUCT_TYPE_TEXT_FACE_DESC;
    face_desc.data = data + 4;
    face_desc.data_size = font_size;
    rbttext_face face = nullptr;
    if (rbttext_face_create(&face_desc, &face, nullptr) != RBT_RESULT_OK)
        return;
    const size_t atlas_size = size - 4 - font_size;
    if (atlas_size > 0)
    {
        RBTTEXT_ATLAS_DESC settings = atlas_settings(32.0f, 4.0f);
        RBTTEXT_ATLAS_LOAD_DESC load;
        memset(&load, 0, sizeof(load));
        load.base.type = RBT_STRUCT_TYPE_TEXT_ATLAS_LOAD_DESC;
        load.atlas = &settings;
        load.data = data + 4 + font_size;
        load.data_size = atlas_size;
        load.faces = &face;
        load.face_count = 1;
        rbttext_atlas atlas = (rbttext_atlas)&load; // must be overwritten
        RBTTEXT_ERROR error;
        RBT_RESULT result = rbttext_atlas_load(&load, &atlas, &error);
        RBTFUZZ_CHECK(message_is_terminated(error));
        if (result == RBT_RESULT_OK)
        {
            RBTTEXT_ATLAS_INFO info;
            memset(&info, 0, sizeof(info));
            info.base.type = RBT_STRUCT_TYPE_TEXT_ATLAS_INFO;
            RBTFUZZ_CHECK(rbttext_atlas_get_info(atlas, &info) == RBT_RESULT_OK);
            RBTFUZZ_CHECK(info.page_count <= MAX_PAGES);
            RBTFUZZ_CHECK(info.dirty_rect_count == info.page_count);
            rbttext_atlas_destroy(atlas);
        }
        else
            RBTFUZZ_CHECK(atlas == nullptr);
    }
    rbttext_face_destroy(face);
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 2)
        return 0;
    switch (data[0])
    {
    case 0:
        fuzz_font(data + 1, size - 1);
        break;
    case 1:
        fuzz_hershey(data + 1, size - 1);
        break;
    case 2:
        fuzz_atlas(data + 1, size - 1);
        break;
    case 3:
        fuzz_lff(data + 1, size - 1);
        break;
    case 4:
        fuzz_shx(data + 1, size - 1);
        break;
    case 5:
        fuzz_variable(data + 1, size - 1);
        break;
    default:
        break;
    }
    return 0;
}
