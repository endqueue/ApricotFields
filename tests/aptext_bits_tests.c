// ApText's "same bits on every platform" (aptext.h): one layout and one
// saved atlas, built from the two fonts tests/aptext_tests.c makes
// (written out to tests/data/aptext with its --write-fonts), are compared
// by CRC-32 with the values first recorded on macOS. The sizes, spacing and
// width are deliberately not binary fractions, so rounding shows.
//
// A failure on one platform is a determinism bug, or a dependency that
// computes differently there (APTEXT_TODO.md): the values are never
// re-recorded to make one pass.
//
// Written in C, like aptext_tests.c. Takes the data folder as its argument.

#include "text/aptext.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            printf("FAIL (%s:%d): %s\n", __FILE__, __LINE__, #condition);                                            \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// Recorded on macOS (Apple clang 21, arm64), Debug and Release, 2026-10-29.
#define EXPECTED_LAYOUT_CRC 0x8F07D913u
#define EXPECTED_ATLAS_CRC 0x2144DF1Cu
#define EXPECTED_ATLAS_SIZE 66192u

static uint32_t crc32_of(uint32_t crc, const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    crc = ~crc;
    while (size--)
    {
        crc ^= *bytes++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static uint8_t *read_file(const char *dir, const char *name, size_t *out_size)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *file = fopen(path, "rb");
    if (!file)
        return NULL;
    uint8_t *data = NULL;
    if (fseek(file, 0, SEEK_END) == 0)
    {
        long size = ftell(file);
        if (size > 0 && fseek(file, 0, SEEK_SET) == 0)
        {
            data = (uint8_t *)malloc((size_t)size);
            if (data && fread(data, 1, (size_t)size, file) != (size_t)size)
            {
                free(data);
                data = NULL;
            }
            *out_size = (size_t)size;
        }
    }
    fclose(file);
    return data;
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        printf("aptext_bits_tests <data folder>\n");
        return 1;
    }
    size_t size_a = 0, size_b = 0;
    uint8_t *font_a = read_file(argv[1], "aptext_test_a.ttf", &size_a);
    uint8_t *font_b = read_file(argv[1], "aptext_test_b.ttf", &size_b);
    CHECK(font_a != NULL);
    CHECK(font_b != NULL);
    if (!font_a || !font_b)
        return 1;

    aptext_face faces[2] = {NULL, NULL};
    APTEXT_FACE_DESC face_desc;
    memset(&face_desc, 0, sizeof(face_desc));
    face_desc.base.type = APRI_STRUCT_TYPE_APTEXT_FACE_DESC;
    face_desc.data = font_a;
    face_desc.data_size = size_a;
    CHECK(aptext_face_create(&face_desc, &faces[0], NULL) == APRESULT_OK);
    face_desc.data = font_b;
    face_desc.data_size = size_b;
    CHECK(aptext_face_create(&face_desc, &faces[1], NULL) == APRESULT_OK);

    APTEXT_CHAIN_DESC chain_desc;
    memset(&chain_desc, 0, sizeof(chain_desc));
    chain_desc.base.type = APRI_STRUCT_TYPE_APTEXT_CHAIN_DESC;
    chain_desc.faces = faces;
    chain_desc.face_count = 2;
    aptext_chain chain = NULL;
    CHECK(aptext_chain_create(&chain_desc, &chain, NULL) == APRESULT_OK);

    // Ligature, kerning, fallback, .notdef, Hebrew, Han, wrapping, CR LF.
    const char *text = "fi AV az\naQ a\xD7\x90\xD7\x91 aa aa aa \xE4\xB8\x80 ab\r\nba";
    APTEXT_LAYOUT_DESC layout_desc;
    memset(&layout_desc, 0, sizeof(layout_desc));
    layout_desc.base.type = APRI_STRUCT_TYPE_APTEXT_LAYOUT_DESC;
    layout_desc.chain = chain;
    layout_desc.text = text;
    layout_desc.text_size = (uint32_t)strlen(text);
    layout_desc.direction = APTEXT_DIRECTION_AUTO;
    layout_desc.features = APTEXT_FEATURE_KERNING | APTEXT_FEATURE_LIGATURES;
    layout_desc.em_size = 13.7f;
    layout_desc.max_width = 41.3f;
    layout_desc.align = APTEXT_ALIGN_CENTER;
    layout_desc.line_spacing = 1.15f;
    layout_desc.letter_spacing = 0.3f;
    aptext_layout layout = NULL;
    CHECK(aptext_layout_create(&layout) == APRESULT_OK);
    CHECK(aptext_layout_build(layout, &layout_desc, NULL) == APRESULT_OK);
    const APTEXT_GLYPH *glyphs = NULL;
    const APTEXT_LINE *lines = NULL;
    const APTEXT_RUN *runs = NULL;
    uint32_t glyph_count = 0, line_count = 0, run_count = 0;
    CHECK(aptext_layout_get_glyphs(layout, &glyphs, &glyph_count) == APRESULT_OK);
    CHECK(aptext_layout_get_lines(layout, &lines, &line_count) == APRESULT_OK);
    CHECK(aptext_layout_get_runs(layout, &runs, &run_count) == APRESULT_OK);
    uint32_t layout_crc = crc32_of(0, glyphs, glyph_count * sizeof(APTEXT_GLYPH));
    layout_crc = crc32_of(layout_crc, lines, line_count * sizeof(APTEXT_LINE));
    layout_crc = crc32_of(layout_crc, runs, run_count * sizeof(APTEXT_RUN));

    APTEXT_ATLAS_DESC atlas_desc;
    memset(&atlas_desc, 0, sizeof(atlas_desc));
    atlas_desc.base.type = APRI_STRUCT_TYPE_APTEXT_ATLAS_DESC;
    atlas_desc.pixels_per_em = 37.5f;
    atlas_desc.distance_range = 6.0f;
    atlas_desc.page_width = 128;
    atlas_desc.page_height = 128;
    atlas_desc.max_pages = 8;
    aptext_atlas atlas = NULL;
    CHECK(aptext_atlas_create(&atlas_desc, &atlas, NULL) == APRESULT_OK);
    CHECK(aptext_atlas_add_layout(atlas, layout, NULL) == APRESULT_OK);
    uint64_t saved_size = 0;
    CHECK(aptext_atlas_save(atlas, faces, 2, NULL, 0, &saved_size, NULL) == APRESULT_OK);
    uint8_t *saved = (uint8_t *)malloc(saved_size ? (size_t)saved_size : 1);
    CHECK(saved != NULL);
    uint32_t atlas_crc = 0;
    if (saved)
    {
        CHECK(aptext_atlas_save(atlas, faces, 2, saved, saved_size, &saved_size, NULL) == APRESULT_OK);
        atlas_crc = crc32_of(0, saved, (size_t)saved_size);
    }

    printf("aptext_bits_tests: layout of %u glyphs, %u lines, %u runs: CRC-32 %08X (expected %08X)\n", glyph_count,
           line_count, run_count, layout_crc, EXPECTED_LAYOUT_CRC);
    printf("aptext_bits_tests: saved atlas of %llu bytes: CRC-32 %08X (expected %08X)\n",
           (unsigned long long)saved_size, atlas_crc, EXPECTED_ATLAS_CRC);
    CHECK(layout_crc == EXPECTED_LAYOUT_CRC);
    CHECK(saved_size == EXPECTED_ATLAS_SIZE);
    CHECK(atlas_crc == EXPECTED_ATLAS_CRC);

    free(saved);
    aptext_atlas_destroy(atlas);
    aptext_layout_destroy(layout);
    aptext_chain_destroy(chain);
    aptext_face_destroy(faces[0]);
    aptext_face_destroy(faces[1]);
    free(font_a);
    free(font_b);
    return failures ? 1 : 0;
}
