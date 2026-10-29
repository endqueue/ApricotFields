// aptext_fuzz - ApText's readers of outside bytes: font files
// (aptext_face_create(), through HarfBuzz), Hershey fonts
// (aptext_face_create_hershey()) and saved atlases (aptext_atlas_load()).
//
// The input's first byte says which; the rest is what it reads:
//   0: a font file. A face that opens is laid out, and the glyphs of the
//      layout are generated into an atlas, which is saved and loaded again.
//   1: a Hershey font, from U+0020. A face that opens is laid out, and its
//      strokes taken.
//   2: a font file's size (4 bytes, little-endian), the font, then a saved
//      atlas, loaded with that face at fixed settings (the corpus's).
// Beyond not crashing, it checks:
// - a failed create or load leaves no object and a terminated message;
// - a layout's glyphs, lines and runs index each other and the text;
// - a stroke layout's segment count is what get_strokes() fills;
// - an atlas saved and loaded again has the same glyphs and pages.

#include "aprifuzz.hpp"

#include "text/aptext.h"

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

bool message_is_terminated(const APTEXT_ERROR &error)
{
    return memchr(error.message, '\0', sizeof(error.message)) != nullptr;
}

APTEXT_ATLAS_DESC atlas_settings(float pixels_per_em, float distance_range)
{
    APTEXT_ATLAS_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APTEXT_ATLAS_DESC;
    desc.pixels_per_em = pixels_per_em;
    desc.distance_range = distance_range;
    desc.page_width = PAGE;
    desc.page_height = PAGE;
    desc.max_pages = MAX_PAGES;
    return desc;
}

// Lays [face] out in a chain of its own and checks the layout; the layout,
// or null if the build failed.
aptext_layout lay_out(aptext_face face, aptext_chain *out_chain)
{
    APTEXT_CHAIN_DESC chain_desc;
    memset(&chain_desc, 0, sizeof(chain_desc));
    chain_desc.base.type = APRI_STRUCT_TYPE_APTEXT_CHAIN_DESC;
    chain_desc.faces = &face;
    chain_desc.face_count = 1;
    APRIFUZZ_CHECK(aptext_chain_create(&chain_desc, out_chain, nullptr) == APRESULT_OK);

    aptext_layout layout = nullptr;
    APRIFUZZ_CHECK(aptext_layout_create(&layout) == APRESULT_OK);
    APTEXT_LAYOUT_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APTEXT_LAYOUT_DESC;
    desc.chain = *out_chain;
    desc.text = LAYOUT_TEXT;
    desc.text_size = (uint32_t)strlen(LAYOUT_TEXT);
    desc.direction = APTEXT_DIRECTION_AUTO;
    desc.features = APTEXT_FEATURE_KERNING | APTEXT_FEATURE_LIGATURES | APTEXT_FEATURE_CONTEXTUAL_ALTERNATES;
    desc.em_size = 16.0f;
    desc.max_width = 48.0f;
    desc.align = APTEXT_ALIGN_START;
    desc.line_spacing = 1.0f;
    desc.letter_spacing = 0.5f;
    APTEXT_ERROR error;
    APRESULT result = aptext_layout_build(layout, &desc, &error);
    APRIFUZZ_CHECK(message_is_terminated(error));
    if (result != APRESULT_OK)
    {
        APRIFUZZ_CHECK(result == APRESULT_OUT_OF_MEMORY);
        return layout;
    }

    const APTEXT_GLYPH *glyphs = nullptr;
    const APTEXT_LINE *lines = nullptr;
    const APTEXT_RUN *runs = nullptr;
    uint32_t glyph_count = 0, line_count = 0, run_count = 0;
    APRIFUZZ_CHECK(aptext_layout_get_glyphs(layout, &glyphs, &glyph_count) == APRESULT_OK);
    APRIFUZZ_CHECK(aptext_layout_get_lines(layout, &lines, &line_count) == APRESULT_OK);
    APRIFUZZ_CHECK(aptext_layout_get_runs(layout, &runs, &run_count) == APRESULT_OK);
    APRIFUZZ_CHECK(line_count >= 1);
    for (uint32_t i = 0; i < glyph_count; i++)
    {
        const APTEXT_GLYPH &g = glyphs[i];
        APRIFUZZ_CHECK(g.face_index == 0);
        APRIFUZZ_CHECK(g.line < line_count);
        APRIFUZZ_CHECK(g.run < run_count);
        APRIFUZZ_CHECK(g.text_size > 0);
        APRIFUZZ_CHECK(g.text_offset + g.text_size <= desc.text_size);
        APRIFUZZ_CHECK(g.advance >= 0.0f);
    }
    uint32_t next_glyph = 0;
    for (uint32_t l = 0; l < line_count; l++)
    {
        APRIFUZZ_CHECK(lines[l].first_glyph == next_glyph);
        next_glyph += lines[l].glyph_count;
        APRIFUZZ_CHECK(lines[l].text_offset + lines[l].text_size <= desc.text_size);
        APRIFUZZ_CHECK(lines[l].box_left <= lines[l].box_right);
    }
    APRIFUZZ_CHECK(next_glyph == glyph_count);
    for (uint32_t r = 0; r < run_count; r++)
    {
        APRIFUZZ_CHECK(runs[r].glyph_count > 0);
        APRIFUZZ_CHECK(runs[r].first_glyph + runs[r].glyph_count <= glyph_count);
        APRIFUZZ_CHECK(runs[r].line < line_count);
    }
    return layout;
}

void fuzz_font(const uint8_t *data, size_t size)
{
    APTEXT_FACE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APTEXT_FACE_DESC;
    desc.data = data;
    desc.data_size = size;
    aptext_face face = (aptext_face)&desc; // must be overwritten
    APTEXT_ERROR error;
    APRESULT result = aptext_face_create(&desc, &face, &error);
    APRIFUZZ_CHECK(message_is_terminated(error));
    if (result != APRESULT_OK)
    {
        APRIFUZZ_CHECK(face == nullptr);
        return;
    }
    APTEXT_FACE_METRICS metrics;
    memset(&metrics, 0, sizeof(metrics));
    metrics.base.type = APRI_STRUCT_TYPE_APTEXT_FACE_METRICS;
    APRIFUZZ_CHECK(aptext_face_get_metrics(face, &metrics) == APRESULT_OK);
    APRIFUZZ_CHECK(metrics.glyph_count > 0);

    aptext_chain chain = nullptr;
    aptext_layout layout = lay_out(face, &chain);

    APTEXT_ATLAS_DESC settings = atlas_settings(PIXELS_PER_EM, DISTANCE_RANGE);
    aptext_atlas atlas = nullptr;
    APRIFUZZ_CHECK(aptext_atlas_create(&settings, &atlas, nullptr) == APRESULT_OK);
    result = aptext_atlas_add_layout(atlas, layout, &error);
    APRIFUZZ_CHECK(message_is_terminated(error));
    if (result == APRESULT_OK)
    {
        uint64_t saved_size = 0;
        APRIFUZZ_CHECK(aptext_atlas_save(atlas, &face, 1, nullptr, 0, &saved_size, nullptr) == APRESULT_OK);
        std::vector<uint8_t> saved((size_t)saved_size);
        APRIFUZZ_CHECK(aptext_atlas_save(atlas, &face, 1, saved.data(), saved_size, &saved_size, nullptr) ==
                       APRESULT_OK);
        APTEXT_ATLAS_LOAD_DESC load;
        memset(&load, 0, sizeof(load));
        load.base.type = APRI_STRUCT_TYPE_APTEXT_ATLAS_LOAD_DESC;
        load.atlas = &settings;
        load.data = saved.data();
        load.data_size = saved_size;
        load.faces = &face;
        load.face_count = 1;
        aptext_atlas loaded = nullptr;
        result = aptext_atlas_load(&load, &loaded, &error);
        if (result != APRESULT_OK)
            fprintf(stderr, "aptext_fuzz: an atlas saved doesn't load: %s\n", error.message);
        APRIFUZZ_CHECK(result == APRESULT_OK);
        APTEXT_ATLAS_INFO a, b;
        memset(&a, 0, sizeof(a));
        a.base.type = APRI_STRUCT_TYPE_APTEXT_ATLAS_INFO;
        b = a;
        APRIFUZZ_CHECK(aptext_atlas_get_info(atlas, &a) == APRESULT_OK);
        APRIFUZZ_CHECK(aptext_atlas_get_info(loaded, &b) == APRESULT_OK);
        APRIFUZZ_CHECK(a.glyph_count == b.glyph_count);
        APRIFUZZ_CHECK(a.page_count == b.page_count);
        for (uint32_t p = 0; p < a.page_count; p++)
        {
            const void *x = nullptr;
            const void *y = nullptr;
            APRIFUZZ_CHECK(aptext_atlas_get_page(atlas, p, &x) == APRESULT_OK);
            APRIFUZZ_CHECK(aptext_atlas_get_page(loaded, p, &y) == APRESULT_OK);
            APRIFUZZ_CHECK(memcmp(x, y, (size_t)PAGE * PAGE * 4) == 0);
        }
        aptext_atlas_destroy(loaded);
    }
    else
    {
        APRIFUZZ_CHECK(result == APRESULT_LIMIT || result == APRESULT_INVALID_ARGUMENT ||
                       result == APRESULT_OUT_OF_MEMORY);
    }
    aptext_atlas_destroy(atlas);
    aptext_layout_destroy(layout);
    aptext_chain_destroy(chain);
    aptext_face_destroy(face);
}

void fuzz_hershey(const uint8_t *data, size_t size)
{
    APTEXT_HERSHEY_FACE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APTEXT_HERSHEY_FACE_DESC;
    desc.data = data;
    desc.data_size = size;
    desc.first_codepoint = 0x20;
    desc.units_per_em = 32.0f;
    desc.baseline = 9.0f;
    desc.metrics.ascender = 0.75f;
    desc.metrics.descender = -0.25f;
    aptext_face face = (aptext_face)&desc; // must be overwritten
    APTEXT_ERROR error;
    APRESULT result = aptext_face_create_hershey(&desc, &face, &error);
    APRIFUZZ_CHECK(message_is_terminated(error));
    if (result != APRESULT_OK)
    {
        APRIFUZZ_CHECK(face == nullptr);
        return;
    }
    aptext_chain chain = nullptr;
    aptext_layout layout = lay_out(face, &chain);
    uint32_t count = 0;
    if (aptext_layout_get_strokes(layout, nullptr, 0, &count) == APRESULT_OK && count < (1u << 20))
    {
        std::vector<APTEXT_SEGMENT> segments(count ? count : 1);
        uint32_t filled = 0;
        APRIFUZZ_CHECK(aptext_layout_get_strokes(layout, count ? segments.data() : nullptr, count, &filled) ==
                       APRESULT_OK);
        APRIFUZZ_CHECK(filled == count);
    }
    aptext_layout_destroy(layout);
    aptext_chain_destroy(chain);
    aptext_face_destroy(face);
}

void fuzz_atlas(const uint8_t *data, size_t size)
{
    if (size < 4)
        return;
    const uint32_t font_size = (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) |
                               ((uint32_t)data[3] << 24);
    if (font_size == 0 || font_size > size - 4)
        return;
    APTEXT_FACE_DESC face_desc;
    memset(&face_desc, 0, sizeof(face_desc));
    face_desc.base.type = APRI_STRUCT_TYPE_APTEXT_FACE_DESC;
    face_desc.data = data + 4;
    face_desc.data_size = font_size;
    aptext_face face = nullptr;
    if (aptext_face_create(&face_desc, &face, nullptr) != APRESULT_OK)
        return;
    const size_t atlas_size = size - 4 - font_size;
    if (atlas_size > 0)
    {
        APTEXT_ATLAS_DESC settings = atlas_settings(32.0f, 4.0f);
        APTEXT_ATLAS_LOAD_DESC load;
        memset(&load, 0, sizeof(load));
        load.base.type = APRI_STRUCT_TYPE_APTEXT_ATLAS_LOAD_DESC;
        load.atlas = &settings;
        load.data = data + 4 + font_size;
        load.data_size = atlas_size;
        load.faces = &face;
        load.face_count = 1;
        aptext_atlas atlas = (aptext_atlas)&load; // must be overwritten
        APTEXT_ERROR error;
        APRESULT result = aptext_atlas_load(&load, &atlas, &error);
        APRIFUZZ_CHECK(message_is_terminated(error));
        if (result == APRESULT_OK)
        {
            APTEXT_ATLAS_INFO info;
            memset(&info, 0, sizeof(info));
            info.base.type = APRI_STRUCT_TYPE_APTEXT_ATLAS_INFO;
            APRIFUZZ_CHECK(aptext_atlas_get_info(atlas, &info) == APRESULT_OK);
            APRIFUZZ_CHECK(info.page_count <= MAX_PAGES);
            APRIFUZZ_CHECK(info.dirty_rect_count == info.page_count);
            aptext_atlas_destroy(atlas);
        }
        else
            APRIFUZZ_CHECK(atlas == nullptr);
    }
    aptext_face_destroy(face);
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
    default:
        break;
    }
    return 0;
}
