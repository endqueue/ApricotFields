// Headless tests for ApText (include/text/aptext.h): the contract, as the
// implementation will have to meet it.
//
// Written in C on purpose: aptext.h is a C front end, and this proves it
// compiles and links as one.
//
// Written before the implementation (APTEXT_TODO.md, the contract pass), and
// not to be edited by it. Two kinds of test:
//   - Argument checks, which every call makes before anything else and
//     which pass today.
//   - Behaviour: shaping, fallback, .notdef, wrapping, ligatures, kerning,
//     bidirectional runs, single-stroke faces, distance fields, packing,
//     dirty rectangles, saving and loading. Each is written against the
//     real behaviour. A call that returns APRESULT_NOT_IMPLEMENTED marks
//     its test pending and ends it; anything else is checked. So the
//     implementation turns them on call by call, without touching this file.
//
// The fonts are built here, byte by byte (build_font()): two small TrueType
// fonts and a collection of both, with glyph ids, advances, outlines, a
// kerning pair and a ligature chosen so that every expected position is an
// exact binary fraction, and compared with ==. Run with --write-fonts <dir>
// to write them out for a font tool to check.

#include "text/aptext.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static const char *current_test = "";

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            printf("FAIL %s (%s:%d): %s\n", current_test, __FILE__, __LINE__, #condition);                           \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// ---- Pending tests ----------------------------------------------------------------

static bool test_pending = false;

// Marks the current test pending: a call it needs returned
// APRESULT_NOT_IMPLEMENTED.
static void mark_pending(int line)
{
    if (!test_pending)
        printf("PENDING %s (%s:%d): not implemented yet\n", current_test, __FILE__, line);
    test_pending = true;
}

// True if [result] is from a built call; marks the test pending if not.
static bool built(APRESULT result, int line)
{
    if (result == APRESULT_NOT_IMPLEMENTED)
    {
        mark_pending(line);
        return false;
    }
    return true;
}

#define BUILT(result) built((result), __LINE__)

// ---- What a test made, destroyed when it ends ---------------------------------------

#define MAX_TRACKED 64

static aptext_face tracked_faces[MAX_TRACKED];
static int tracked_face_count = 0;
static aptext_chain tracked_chains[MAX_TRACKED];
static int tracked_chain_count = 0;
static aptext_layout tracked_layouts[MAX_TRACKED];
static int tracked_layout_count = 0;
static aptext_atlas tracked_atlases[MAX_TRACKED];
static int tracked_atlas_count = 0;
static void *tracked_memory[MAX_TRACKED];
static int tracked_memory_count = 0;

static void track_face(aptext_face face)
{
    if (face && tracked_face_count < MAX_TRACKED)
        tracked_faces[tracked_face_count++] = face;
}

static void track_chain(aptext_chain chain)
{
    if (chain && tracked_chain_count < MAX_TRACKED)
        tracked_chains[tracked_chain_count++] = chain;
}

static void track_layout(aptext_layout layout)
{
    if (layout && tracked_layout_count < MAX_TRACKED)
        tracked_layouts[tracked_layout_count++] = layout;
}

static void track_atlas(aptext_atlas atlas)
{
    if (atlas && tracked_atlas_count < MAX_TRACKED)
        tracked_atlases[tracked_atlas_count++] = atlas;
}

static void *track_memory(size_t size)
{
    void *memory = calloc(1, size ? size : 1);
    if (memory && tracked_memory_count < MAX_TRACKED)
        tracked_memory[tracked_memory_count++] = memory;
    return memory;
}

static void destroy_tracked(void)
{
    for (int i = 0; i < tracked_layout_count; i++)
        aptext_layout_destroy(tracked_layouts[i]);
    for (int i = 0; i < tracked_atlas_count; i++)
        aptext_atlas_destroy(tracked_atlases[i]);
    for (int i = 0; i < tracked_chain_count; i++)
        aptext_chain_destroy(tracked_chains[i]);
    for (int i = 0; i < tracked_face_count; i++)
        aptext_face_destroy(tracked_faces[i]);
    for (int i = 0; i < tracked_memory_count; i++)
        free(tracked_memory[i]);
    tracked_face_count = 0;
    tracked_chain_count = 0;
    tracked_layout_count = 0;
    tracked_atlas_count = 0;
    tracked_memory_count = 0;
}

// ---- Building fonts -------------------------------------------------------------------

typedef struct Bytes
{
    uint8_t data[16384];
    uint32_t size;
} Bytes;

static void put8(Bytes *b, uint32_t v)
{
    if (b->size < sizeof(b->data))
        b->data[b->size] = (uint8_t)v;
    b->size++;
}

static void put16(Bytes *b, uint32_t v)
{
    put8(b, v >> 8);
    put8(b, v);
}

static void put32(Bytes *b, uint32_t v)
{
    put16(b, v >> 16);
    put16(b, v);
}

static void put_tag(Bytes *b, const char *tag)
{
    for (int i = 0; i < 4; i++)
        put8(b, (uint8_t)tag[i]);
}

static void pad4(Bytes *b)
{
    while (b->size % 4)
        put8(b, 0);
}

static void set16(Bytes *b, uint32_t at, uint32_t v)
{
    b->data[at] = (uint8_t)(v >> 8);
    b->data[at + 1] = (uint8_t)v;
}

static void set32(Bytes *b, uint32_t at, uint32_t v)
{
    set16(b, at, v >> 16);
    set16(b, at + 2, v);
}

static uint32_t table_checksum(const uint8_t *data, uint32_t size)
{
    uint32_t sum = 0;
    for (uint32_t i = 0; i < size; i += 4)
    {
        uint32_t word = 0;
        for (uint32_t j = 0; j < 4; j++)
            word = (word << 8) | (i + j < size ? data[i + j] : 0);
        sum += word;
    }
    return sum;
}

// One glyph of a test font: a rectangle, or nothing.
typedef struct TestGlyph
{
    uint32_t codepoint; // 0: not in the character map
    uint16_t advance;
    bool empty;
    int16_t x0, y0, x1, y1;
} TestGlyph;

typedef struct TestFont
{
    const TestGlyph *glyphs;
    uint16_t glyph_count;
    uint32_t revision; // 16.16, part of the face's fingerprint
    bool kern;         // a kerning pair, kern_left then kern_right, by kern_value
    uint16_t kern_left, kern_right;
    int16_t kern_value;
    bool liga; // a 'liga' ligature, liga_first liga_second -> liga_glyph
    uint16_t liga_first, liga_second, liga_glyph;
} TestFont;

// Every test font: 1024 units per em, ascender 768, descender -256, no line
// gap, so that a line is exactly one em and every position below is an
// exact binary fraction.
#define UPEM 1024

// Font A, 9 glyphs.
#define A_NOTDEF 0
#define A_SPACE 1
#define A_LOWER_A 2
#define A_LOWER_B 3
#define A_LOWER_F 4
#define A_LOWER_I 5
#define A_FI 6
#define A_UPPER_A 7
#define A_UPPER_V 8
static const TestGlyph font_a_glyphs[] = {
    {0, 512, false, 64, 0, 448, 704},     // .notdef
    {0x20, 256, true, 0, 0, 0, 0},        // space
    {0x61, 512, false, 64, 0, 448, 384},  // a: a square, 0.375 em a side
    {0x62, 512, false, 64, 0, 448, 704},  // b
    {0x66, 320, false, 64, 0, 256, 704},  // f
    {0x69, 192, false, 64, 0, 128, 448},  // i
    {0, 512, false, 64, 0, 448, 704},     // f_i, reached only through 'liga'
    {0x41, 640, false, 32, 0, 608, 704},  // A
    {0x56, 640, false, 32, 0, 608, 704},  // V
};
static const TestFont font_a_spec = {font_a_glyphs, 9, 0x00010000, true, A_UPPER_A, A_UPPER_V, -128,
                                     true, A_LOWER_F, A_LOWER_I, A_FI};

// Font B, 6 glyphs: the fallback.
#define B_NOTDEF 0
#define B_LOWER_A 1
#define B_LOWER_Z 2
#define B_HAN_ONE 3
#define B_ALEF 4
#define B_BET 5
static const TestGlyph font_b_glyphs[] = {
    {0, 512, false, 64, 0, 448, 704},        // .notdef
    {0x61, 768, false, 64, 0, 704, 384},     // a, wider than font A's
    {0x7A, 384, false, 64, 0, 320, 384},     // z
    {0x4E00, 1024, false, -128, 256, 1152, 448}, // U+4E00, a bar 1.25 em wide: wider than an em
    {0x5D0, 512, false, 64, 0, 448, 448},    // U+05D0 HEBREW LETTER ALEF
    {0x5D1, 384, false, 64, 0, 320, 448},    // U+05D1 HEBREW LETTER BET
};
static const TestFont font_b_spec = {font_b_glyphs, 6, 0x00020000, false, 0, 0, 0, false, 0, 0, 0};

typedef struct Table
{
    const char *tag;
    Bytes bytes;
} Table;

static Table font_tables[12];

static void build_head(Bytes *b, const TestFont *f)
{
    put32(b, 0x00010000);
    put32(b, f->revision);
    put32(b, 0); // checkSumAdjustment, set once the font is whole
    put32(b, 0x5F0F3CF5);
    put16(b, 0x000B);
    put16(b, UPEM);
    put32(b, 0);
    put32(b, 0); // created
    put32(b, 0);
    put32(b, 0); // modified
    put16(b, (uint16_t)-128);
    put16(b, 0);
    put16(b, 1152);
    put16(b, 704);
    put16(b, 0);  // macStyle
    put16(b, 8);  // lowestRecPPEM
    put16(b, 2);  // fontDirectionHint
    put16(b, 1);  // indexToLocFormat: long
    put16(b, 0);  // glyphDataFormat
}

static void build_hhea(Bytes *b, const TestFont *f)
{
    uint16_t max_advance = 0;
    for (uint16_t i = 0; i < f->glyph_count; i++)
        if (f->glyphs[i].advance > max_advance)
            max_advance = f->glyphs[i].advance;
    put32(b, 0x00010000);
    put16(b, 768);
    put16(b, (uint16_t)-256);
    put16(b, 0);
    put16(b, max_advance);
    put16(b, (uint16_t)-128); // minLeftSideBearing
    put16(b, (uint16_t)-128); // minRightSideBearing
    put16(b, 1152);           // xMaxExtent
    put16(b, 1);              // caretSlopeRise
    put16(b, 0);
    put16(b, 0);
    for (int i = 0; i < 4; i++)
        put16(b, 0);
    put16(b, 0);
    put16(b, f->glyph_count);
}

static void build_maxp(Bytes *b, const TestFont *f)
{
    put32(b, 0x00010000);
    put16(b, f->glyph_count);
    put16(b, 4); // maxPoints
    put16(b, 1); // maxContours
    put16(b, 0);
    put16(b, 0);
    put16(b, 2); // maxZones
    for (int i = 0; i < 8; i++)
        put16(b, 0);
}

static void build_os2(Bytes *b, const TestFont *f)
{
    uint32_t first = 0xFFFF, last = 0;
    for (uint16_t i = 0; i < f->glyph_count; i++)
    {
        uint32_t c = f->glyphs[i].codepoint;
        if (!c)
            continue;
        if (c < first)
            first = c;
        if (c > last)
            last = c;
    }
    put16(b, 4);   // version
    put16(b, 512); // xAvgCharWidth
    put16(b, 400);
    put16(b, 5);
    put16(b, 0); // fsType
    for (int i = 0; i < 8; i++)
        put16(b, 0); // sub- and superscript sizes and offsets
    put16(b, 64);    // yStrikeoutSize
    put16(b, 320);   // yStrikeoutPosition
    put16(b, 0);     // sFamilyClass
    for (int i = 0; i < 10; i++)
        put8(b, 0); // panose
    put32(b, 1);
    put32(b, 0);
    put32(b, 0);
    put32(b, 0);
    put_tag(b, "NONE");
    put16(b, 0x00C0); // REGULAR | USE_TYPO_METRICS
    put16(b, first);
    put16(b, last);
    put16(b, 768);
    put16(b, (uint16_t)-256);
    put16(b, 0);
    put16(b, 768); // usWinAscent
    put16(b, 256); // usWinDescent
    put32(b, 1);
    put32(b, 0);
    put16(b, 448); // sxHeight
    put16(b, 704); // sCapHeight
    put16(b, 0);
    put16(b, 0x20);
    put16(b, 2);
}

static void build_post(Bytes *b)
{
    put32(b, 0x00030000);
    put32(b, 0);
    put16(b, (uint16_t)-128); // underlinePosition
    put16(b, 64);             // underlineThickness
    for (int i = 0; i < 5; i++)
        put32(b, 0);
}

static void build_name(Bytes *b)
{
    put16(b, 0);
    put16(b, 0);
    put16(b, 6);
}

static void build_cmap(Bytes *b, const TestFont *f)
{
    // Format 4, one segment per codepoint, sorted, then the 0xFFFF end.
    uint32_t codes[32];
    uint16_t gids[32];
    uint16_t n = 0;
    for (uint16_t g = 0; g < f->glyph_count; g++)
    {
        if (!f->glyphs[g].codepoint)
            continue;
        uint16_t at = n++;
        while (at > 0 && codes[at - 1] > f->glyphs[g].codepoint)
        {
            codes[at] = codes[at - 1];
            gids[at] = gids[at - 1];
            at--;
        }
        codes[at] = f->glyphs[g].codepoint;
        gids[at] = g;
    }
    uint16_t segments = (uint16_t)(n + 1);
    uint16_t search = 1, selector = 0;
    while (search * 2 <= segments)
    {
        search *= 2;
        selector++;
    }
    search *= 2;
    put16(b, 0);
    put16(b, 1);
    put16(b, 3);
    put16(b, 1);
    put32(b, 12);
    put16(b, 4);
    put16(b, 16 + 8 * segments);
    put16(b, 0);
    put16(b, segments * 2);
    put16(b, search);
    put16(b, selector);
    put16(b, segments * 2 - search);
    for (uint16_t i = 0; i < n; i++)
        put16(b, codes[i]);
    put16(b, 0xFFFF);
    put16(b, 0);
    for (uint16_t i = 0; i < n; i++)
        put16(b, codes[i]);
    put16(b, 0xFFFF);
    for (uint16_t i = 0; i < n; i++)
        put16(b, (uint16_t)(gids[i] - codes[i]));
    put16(b, 1);
    for (uint16_t i = 0; i < segments; i++)
        put16(b, 0);
}

static void build_glyf_loca_hmtx(Bytes *glyf, Bytes *loca, Bytes *hmtx, const TestFont *f)
{
    for (uint16_t i = 0; i < f->glyph_count; i++)
    {
        const TestGlyph *g = &f->glyphs[i];
        put32(loca, glyf->size);
        put16(hmtx, g->advance);
        put16(hmtx, (uint16_t)(g->empty ? 0 : g->x0));
        if (g->empty)
            continue;
        // One rectangle, clockwise (TrueType's outer direction), every
        // coordinate a 16-bit delta.
        put16(glyf, 1);
        put16(glyf, (uint16_t)g->x0);
        put16(glyf, (uint16_t)g->y0);
        put16(glyf, (uint16_t)g->x1);
        put16(glyf, (uint16_t)g->y1);
        put16(glyf, 3);
        put16(glyf, 0);
        for (int p = 0; p < 4; p++)
            put8(glyf, 0x01);
        put16(glyf, (uint16_t)g->x0);
        put16(glyf, 0);
        put16(glyf, (uint16_t)(g->x1 - g->x0));
        put16(glyf, 0);
        put16(glyf, (uint16_t)g->y0);
        put16(glyf, (uint16_t)(g->y1 - g->y0));
        put16(glyf, 0);
        put16(glyf, (uint16_t)(g->y0 - g->y1));
        pad4(glyf);
    }
    put32(loca, glyf->size);
}

static void build_kern(Bytes *b, const TestFont *f)
{
    put16(b, 0);
    put16(b, 1);
    put16(b, 0);
    put16(b, 20);
    put16(b, 0x0001);
    put16(b, 1);
    put16(b, 6);
    put16(b, 0);
    put16(b, 0);
    put16(b, f->kern_left);
    put16(b, f->kern_right);
    put16(b, (uint16_t)f->kern_value);
}

static void build_gsub(Bytes *b, const TestFont *f)
{
    // Header, then a ScriptList (DFLT, its default LangSys using feature
    // 0), a FeatureList ('liga', lookup 0) and a LookupList (one
    // LigatureSubst), at the offsets written out below.
    put16(b, 1);
    put16(b, 0);
    put16(b, 10);
    put16(b, 30);
    put16(b, 44);
    // ScriptList at 10.
    put16(b, 1);
    put_tag(b, "DFLT");
    put16(b, 8);
    put16(b, 4); // Script: defaultLangSys
    put16(b, 0);
    put16(b, 0); // LangSys
    put16(b, 0xFFFF);
    put16(b, 1);
    put16(b, 0);
    // FeatureList at 30.
    put16(b, 1);
    put_tag(b, "liga");
    put16(b, 8);
    put16(b, 0); // Feature
    put16(b, 1);
    put16(b, 0);
    // LookupList at 44.
    put16(b, 1);
    put16(b, 4);
    put16(b, 4); // Lookup: type 4, ligature substitution
    put16(b, 0);
    put16(b, 1);
    put16(b, 8);
    put16(b, 1); // LigatureSubstFormat1
    put16(b, 8);
    put16(b, 1);
    put16(b, 14);
    put16(b, 1); // Coverage format 1
    put16(b, 1);
    put16(b, f->liga_first);
    put16(b, 1); // LigatureSet
    put16(b, 4);
    put16(b, f->liga_glyph); // Ligature
    put16(b, 2);
    put16(b, f->liga_second);
}

// Builds [f] into [out] at [out]'s end, its table offsets counted from the
// start of [out] (so a collection's fonts can follow each other).
static void build_font(Bytes *out, const TestFont *f)
{
    int n = 0;
    memset(font_tables, 0, sizeof(font_tables));
    // Tags in ascending byte order, as the table directory needs them.
    if (f->liga)
    {
        font_tables[n].tag = "GSUB";
        build_gsub(&font_tables[n++].bytes, f);
    }
    font_tables[n].tag = "OS/2";
    build_os2(&font_tables[n++].bytes, f);
    font_tables[n].tag = "cmap";
    build_cmap(&font_tables[n++].bytes, f);
    int glyf = n;
    font_tables[n++].tag = "glyf";
    int head = n;
    font_tables[n].tag = "head";
    build_head(&font_tables[n++].bytes, f);
    font_tables[n].tag = "hhea";
    build_hhea(&font_tables[n++].bytes, f);
    int hmtx = n;
    font_tables[n++].tag = "hmtx";
    if (f->kern)
    {
        font_tables[n].tag = "kern";
        build_kern(&font_tables[n++].bytes, f);
    }
    int loca = n;
    font_tables[n++].tag = "loca";
    font_tables[n].tag = "maxp";
    build_maxp(&font_tables[n++].bytes, f);
    font_tables[n].tag = "name";
    build_name(&font_tables[n++].bytes);
    font_tables[n].tag = "post";
    build_post(&font_tables[n++].bytes);
    build_glyf_loca_hmtx(&font_tables[glyf].bytes, &font_tables[loca].bytes, &font_tables[hmtx].bytes, f);

    uint32_t start = out->size;
    uint16_t search = 1, selector = 0;
    while (search * 2 <= n)
    {
        search *= 2;
        selector++;
    }
    put32(out, 0x00010000);
    put16(out, (uint32_t)n);
    put16(out, search * 16u);
    put16(out, selector);
    put16(out, (uint32_t)n * 16u - search * 16u);
    uint32_t directory = out->size;
    for (int i = 0; i < n; i++)
    {
        put32(out, 0);
        put32(out, 0);
        put32(out, 0);
        put32(out, 0);
    }
    uint32_t head_at = 0;
    for (int i = 0; i < n; i++)
    {
        Bytes *t = &font_tables[i].bytes;
        uint32_t at = out->size;
        if (i == head)
            head_at = at;
        for (uint32_t j = 0; j < t->size; j++)
            put8(out, t->data[j]);
        pad4(out);
        uint32_t record = directory + (uint32_t)i * 16u;
        for (int c = 0; c < 4; c++)
            out->data[record + c] = (uint8_t)font_tables[i].tag[c];
        set32(out, record + 4, table_checksum(t->data, t->size));
        set32(out, record + 8, at);
        set32(out, record + 12, t->size);
    }
    set32(out, head_at + 8, 0xB1B0AFBAu - table_checksum(out->data + start, out->size - start));
}

static Bytes font_a;
static Bytes font_b;
static Bytes font_ab_collection;

static void build_fonts(void)
{
    font_a.size = 0;
    build_font(&font_a, &font_a_spec);
    font_b.size = 0;
    build_font(&font_b, &font_b_spec);
    // A collection: 'ttcf' 1.0 with two fonts, A then B.
    Bytes *c = &font_ab_collection;
    c->size = 0;
    put_tag(c, "ttcf");
    put32(c, 0x00010000);
    put32(c, 2);
    put32(c, 0);
    put32(c, 0);
    set32(c, 12, c->size);
    build_font(c, &font_a_spec);
    set32(c, 16, c->size);
    build_font(c, &font_b_spec);
}

static bool write_file(const char *dir, const char *name, const Bytes *b)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *file = fopen(path, "wb");
    if (!file)
        return false;
    bool ok = fwrite(b->data, 1, b->size, file) == b->size;
    return fclose(file) == 0 && ok;
}

// ---- Helpers ------------------------------------------------------------------------

static int sentinel_object;
#define SENTINEL ((void *)&sentinel_object)

// A face from [bytes], or NULL: the test is then pending or has failed.
static aptext_face open_face(const Bytes *bytes, uint32_t index)
{
    APTEXT_FACE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APTEXT_FACE_DESC;
    desc.data = bytes->data;
    desc.data_size = bytes->size;
    desc.collection_index = index;
    aptext_face face = NULL;
    APTEXT_ERROR error;
    APRESULT result = aptext_face_create(&desc, &face, &error);
    if (!BUILT(result))
        return NULL;
    CHECK(result == APRESULT_OK);
    if (result != APRESULT_OK)
        printf("  %s\n", error.message);
    CHECK(error.message[0] == '\0' || result != APRESULT_OK);
    track_face(face);
    return face;
}

// A chain of [count] faces, or NULL.
static aptext_chain open_chain(const aptext_face *faces, uint32_t count)
{
    APTEXT_CHAIN_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APTEXT_CHAIN_DESC;
    desc.faces = faces;
    desc.face_count = count;
    aptext_chain chain = NULL;
    APTEXT_ERROR error;
    APRESULT result = aptext_chain_create(&desc, &chain, &error);
    if (!BUILT(result))
        return NULL;
    CHECK(result == APRESULT_OK);
    if (result != APRESULT_OK)
        printf("  %s\n", error.message);
    track_chain(chain);
    return chain;
}

// Font A then font B, or NULL.
static aptext_chain open_chain_ab(aptext_face *out_a, aptext_face *out_b)
{
    aptext_face faces[2];
    faces[0] = open_face(&font_a, 0);
    if (!faces[0])
        return NULL;
    faces[1] = open_face(&font_b, 0);
    if (!faces[1])
        return NULL;
    if (out_a)
        *out_a = faces[0];
    if (out_b)
        *out_b = faces[1];
    return open_chain(faces, 2);
}

// The desc most tests lay out with: em units, no wrapping, left aligned,
// the font's own line spacing, kerning and ligatures on.
static APTEXT_LAYOUT_DESC layout_desc(aptext_chain chain, const char *text)
{
    APTEXT_LAYOUT_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APTEXT_LAYOUT_DESC;
    desc.chain = chain;
    desc.text = text;
    desc.text_size = (uint32_t)strlen(text);
    desc.language = NULL;
    desc.direction = APTEXT_DIRECTION_LTR;
    desc.features = APTEXT_FEATURE_KERNING | APTEXT_FEATURE_LIGATURES;
    desc.em_size = 1.0f;
    desc.max_width = 0.0f;
    desc.align = APTEXT_ALIGN_LEFT;
    desc.line_spacing = 1.0f;
    desc.letter_spacing = 0.0f;
    return desc;
}

// A layout of [desc], or NULL.
static aptext_layout lay_out(const APTEXT_LAYOUT_DESC *desc)
{
    aptext_layout layout = NULL;
    APRESULT result = aptext_layout_create(&layout);
    if (!BUILT(result))
        return NULL;
    CHECK(result == APRESULT_OK);
    if (result != APRESULT_OK)
        return NULL;
    track_layout(layout);
    APTEXT_ERROR error;
    result = aptext_layout_build(layout, desc, &error);
    if (!BUILT(result))
        return NULL;
    CHECK(result == APRESULT_OK);
    if (result != APRESULT_OK)
    {
        printf("  %s\n", error.message);
        return NULL;
    }
    CHECK(error.message[0] == '\0');
    return layout;
}

// A layout's arrays and info, or false.
typedef struct LaidOut
{
    APTEXT_LAYOUT_INFO info;
    const APTEXT_GLYPH *glyphs;
    uint32_t glyph_count;
    const APTEXT_LINE *lines;
    uint32_t line_count;
    const APTEXT_RUN *runs;
    uint32_t run_count;
} LaidOut;

static bool read_layout(aptext_layout layout, LaidOut *out)
{
    memset(out, 0, sizeof(*out));
    out->info.base.type = APRI_STRUCT_TYPE_APTEXT_LAYOUT_INFO;
    APRESULT result = aptext_layout_get_info(layout, &out->info);
    if (!BUILT(result))
        return false;
    CHECK(result == APRESULT_OK);
    CHECK(out->info.base.type == APRI_STRUCT_TYPE_APTEXT_LAYOUT_INFO);
    CHECK(out->info.base.next == NULL);
    result = aptext_layout_get_glyphs(layout, &out->glyphs, &out->glyph_count);
    if (!BUILT(result))
        return false;
    CHECK(result == APRESULT_OK);
    result = aptext_layout_get_lines(layout, &out->lines, &out->line_count);
    if (!BUILT(result))
        return false;
    CHECK(result == APRESULT_OK);
    result = aptext_layout_get_runs(layout, &out->runs, &out->run_count);
    if (!BUILT(result))
        return false;
    CHECK(result == APRESULT_OK);
    CHECK(out->glyph_count == out->info.glyph_count);
    CHECK(out->line_count == out->info.line_count);
    CHECK(out->run_count == out->info.run_count);
    return true;
}

// Lays out [desc] and reads it, or false.
static bool lay_out_and_read(const APTEXT_LAYOUT_DESC *desc, LaidOut *out)
{
    aptext_layout layout = lay_out(desc);
    if (!layout)
        return false;
    return read_layout(layout, out);
}

// ---- Argument checks: they pass today --------------------------------------------------

static void test_face_create_arguments(void)
{
    static const uint8_t bytes[16] = {0};
    APTEXT_FACE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APTEXT_FACE_DESC;
    desc.data = bytes;
    desc.data_size = sizeof(bytes);
    aptext_face face;
    APTEXT_ERROR error;

    CHECK(aptext_face_create(&desc, NULL, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(error.message[0] != '\0');

    face = (aptext_face)SENTINEL;
    CHECK(aptext_face_create(NULL, &face, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(face == NULL);
    CHECK(error.message[0] != '\0');
    CHECK(aptext_face_create(NULL, &face, NULL) == APRESULT_INVALID_ARGUMENT);

    APTEXT_FACE_DESC bad = desc;
    bad.base.type = APRI_STRUCT_TYPE_APTEXT_CHAIN_DESC;
    face = (aptext_face)SENTINEL;
    CHECK(aptext_face_create(&bad, &face, &error) == APRESULT_STRUCT_WRONG_TYPE);
    CHECK(face == NULL);
    bad.base.type = APRI_STRUCT_TYPE_UNKNOWN;
    CHECK(aptext_face_create(&bad, &face, &error) == APRESULT_STRUCT_WRONG_TYPE);

    APRI_STRUCT_BASE chained = {APRI_STRUCT_TYPE_APTEXT_FACE_DESC, NULL};
    bad = desc;
    bad.base.next = &chained;
    CHECK(aptext_face_create(&bad, &face, &error) == APRESULT_STRUCT_UNKNOWN_IN_CHAIN);
    CHECK(error.message[0] != '\0');

    bad = desc;
    bad.data = NULL;
    CHECK(aptext_face_create(&bad, &face, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(error.message[0] != '\0');

    bad = desc;
    bad.data_size = 0;
    CHECK(aptext_face_create(&bad, &face, &error) == APRESULT_INVALID_ARGUMENT);

    bad = desc;
    bad.data_size = 0x100000000ull;
    CHECK(aptext_face_create(&bad, &face, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(face == NULL);
}

// A stroke face desc with two glyphs, 'L' and a space, that passes every
// check; the argument tests break one field at a time.
static const ApriVec2 stroke_points[3] = {{0.125f, 0.75f}, {0.125f, 0.0f}, {0.5f, 0.0f}};
static const APTEXT_STROKE_POLYLINE stroke_polylines[1] = {{0, 3}};
static const APTEXT_STROKE_GLYPH stroke_glyphs[2] = {{0x4C, 0.625f, 0, 1}, {0x20, 0.25f, 1, 0}};

static APTEXT_STROKE_FACE_DESC stroke_desc(void)
{
    APTEXT_STROKE_FACE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APTEXT_STROKE_FACE_DESC;
    desc.glyphs = stroke_glyphs;
    desc.glyph_count = 2;
    desc.polylines = stroke_polylines;
    desc.polyline_count = 1;
    desc.points = stroke_points;
    desc.point_count = 3;
    desc.metrics.ascender = 0.75f;
    desc.metrics.descender = -0.25f;
    desc.metrics.line_gap = 0.0f;
    return desc;
}

static void expect_stroke_refused(const APTEXT_STROKE_FACE_DESC *desc, int line)
{
    aptext_face face = (aptext_face)SENTINEL;
    APTEXT_ERROR error;
    error.message[0] = '\0';
    APRESULT result = aptext_face_create_stroke(desc, &face, &error);
    if (result != APRESULT_INVALID_ARGUMENT)
        printf("FAIL %s (%s:%d): expected APRESULT_INVALID_ARGUMENT, got %d\n", current_test, __FILE__, line,
               (int)result);
    if (result != APRESULT_INVALID_ARGUMENT)
        failures++;
    CHECK(face == NULL);
    CHECK(error.message[0] != '\0');
}

static void test_stroke_face_arguments(void)
{
    APTEXT_STROKE_FACE_DESC desc = stroke_desc();
    APTEXT_STROKE_FACE_DESC bad;
    aptext_face face;
    APTEXT_ERROR error;

    CHECK(aptext_face_create_stroke(&desc, NULL, &error) == APRESULT_INVALID_ARGUMENT);
    face = (aptext_face)SENTINEL;
    CHECK(aptext_face_create_stroke(NULL, &face, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(face == NULL);

    bad = desc;
    bad.base.type = APRI_STRUCT_TYPE_APTEXT_FACE_DESC;
    CHECK(aptext_face_create_stroke(&bad, &face, &error) == APRESULT_STRUCT_WRONG_TYPE);
    APRI_STRUCT_BASE chained = {APRI_STRUCT_TYPE_APTEXT_STROKE_FACE_DESC, NULL};
    bad = desc;
    bad.base.next = &chained;
    CHECK(aptext_face_create_stroke(&bad, &face, &error) == APRESULT_STRUCT_UNKNOWN_IN_CHAIN);

    bad = desc;
    bad.glyphs = NULL;
    expect_stroke_refused(&bad, __LINE__);
    bad = desc;
    bad.glyph_count = 0;
    expect_stroke_refused(&bad, __LINE__);
    bad = desc;
    bad.polylines = NULL;
    expect_stroke_refused(&bad, __LINE__);
    bad = desc;
    bad.points = NULL;
    expect_stroke_refused(&bad, __LINE__);

    APTEXT_STROKE_GLYPH glyphs[2];
    memcpy(glyphs, stroke_glyphs, sizeof(glyphs));
    bad = desc;
    bad.glyphs = glyphs;
    glyphs[0].codepoint = 0x110000;
    expect_stroke_refused(&bad, __LINE__);
    glyphs[0].codepoint = 0xD800;
    expect_stroke_refused(&bad, __LINE__);
    glyphs[0] = stroke_glyphs[0];
    glyphs[0].advance = NAN;
    expect_stroke_refused(&bad, __LINE__);
    glyphs[0].advance = -0.5f;
    expect_stroke_refused(&bad, __LINE__);
    glyphs[0] = stroke_glyphs[0];
    glyphs[0].first_polyline = 2;
    expect_stroke_refused(&bad, __LINE__);
    glyphs[0] = stroke_glyphs[0];
    glyphs[0].polyline_count = 2;
    expect_stroke_refused(&bad, __LINE__);

    APTEXT_STROKE_POLYLINE polylines[1] = {{0, 1}};
    bad = desc;
    bad.polylines = polylines;
    expect_stroke_refused(&bad, __LINE__);
    polylines[0].point_count = 4;
    expect_stroke_refused(&bad, __LINE__);
    polylines[0].first_point = 2;
    polylines[0].point_count = 2;
    expect_stroke_refused(&bad, __LINE__);

    ApriVec2 points[3];
    memcpy(points, stroke_points, sizeof(points));
    bad = desc;
    bad.points = points;
    points[1].x = INFINITY;
    expect_stroke_refused(&bad, __LINE__);
    points[1].x = 0.0f;
    points[2].y = NAN;
    expect_stroke_refused(&bad, __LINE__);

    bad = desc;
    bad.metrics.ascender = -0.25f;
    expect_stroke_refused(&bad, __LINE__);
    bad = desc;
    bad.metrics.descender = NAN;
    expect_stroke_refused(&bad, __LINE__);
    bad = desc;
    bad.metrics.line_gap = -0.125f;
    expect_stroke_refused(&bad, __LINE__);
}

// A small Hershey font: a space, an 'I' (one stroke) and an 'H' (two,
// with a pen-up between them), each 8 units wide, 32 units to the em.
static const char hershey_text[] = "    1  1NV\n"
                                   "    2  3NVRNRV\n"
                                   "    3  6NVPNPV RTNTV\n";
static const uint32_t hershey_codepoints[3] = {0x20, 0x49, 0x48};

static APTEXT_HERSHEY_FACE_DESC hershey_desc(void)
{
    APTEXT_HERSHEY_FACE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APTEXT_HERSHEY_FACE_DESC;
    desc.data = hershey_text;
    desc.data_size = sizeof(hershey_text) - 1;
    desc.codepoints = hershey_codepoints;
    desc.codepoint_count = 3;
    desc.first_codepoint = 0;
    desc.units_per_em = 32.0f;
    desc.baseline = 4.0f;
    desc.metrics.ascender = 0.75f;
    desc.metrics.descender = -0.25f;
    desc.metrics.line_gap = 0.0f;
    return desc;
}

static void expect_hershey_refused(const APTEXT_HERSHEY_FACE_DESC *desc, int line)
{
    aptext_face face = (aptext_face)SENTINEL;
    APTEXT_ERROR error;
    error.message[0] = '\0';
    APRESULT result = aptext_face_create_hershey(desc, &face, &error);
    if (result != APRESULT_INVALID_ARGUMENT)
        printf("FAIL %s (%s:%d): expected APRESULT_INVALID_ARGUMENT, got %d\n", current_test, __FILE__, line,
               (int)result);
    if (result != APRESULT_INVALID_ARGUMENT)
        failures++;
    CHECK(face == NULL);
    CHECK(error.message[0] != '\0');
}

static void test_hershey_face_arguments(void)
{
    APTEXT_HERSHEY_FACE_DESC desc = hershey_desc();
    APTEXT_HERSHEY_FACE_DESC bad;
    aptext_face face;
    APTEXT_ERROR error;

    CHECK(aptext_face_create_hershey(&desc, NULL, &error) == APRESULT_INVALID_ARGUMENT);
    face = (aptext_face)SENTINEL;
    CHECK(aptext_face_create_hershey(NULL, &face, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(face == NULL);
    bad = desc;
    bad.base.type = APRI_STRUCT_TYPE_APTEXT_STROKE_FACE_DESC;
    CHECK(aptext_face_create_hershey(&bad, &face, &error) == APRESULT_STRUCT_WRONG_TYPE);
    APRI_STRUCT_BASE chained = {APRI_STRUCT_TYPE_APTEXT_HERSHEY_FACE_DESC, NULL};
    bad = desc;
    bad.base.next = &chained;
    CHECK(aptext_face_create_hershey(&bad, &face, &error) == APRESULT_STRUCT_UNKNOWN_IN_CHAIN);

    bad = desc;
    bad.data = NULL;
    expect_hershey_refused(&bad, __LINE__);
    bad = desc;
    bad.data_size = 0;
    expect_hershey_refused(&bad, __LINE__);
    bad = desc;
    bad.codepoints = NULL;
    expect_hershey_refused(&bad, __LINE__); // a count of 3 with no codepoints
    bad = desc;
    bad.codepoint_count = 0;
    expect_hershey_refused(&bad, __LINE__);
    uint32_t codepoints[3] = {0x20, 0xDFFF, 0x48};
    bad = desc;
    bad.codepoints = codepoints;
    expect_hershey_refused(&bad, __LINE__);
    bad = desc;
    bad.codepoints = NULL;
    bad.codepoint_count = 0;
    bad.first_codepoint = 0x110000;
    expect_hershey_refused(&bad, __LINE__);
    bad = desc;
    bad.units_per_em = 0.0f;
    expect_hershey_refused(&bad, __LINE__);
    bad = desc;
    bad.units_per_em = NAN;
    expect_hershey_refused(&bad, __LINE__);
    bad = desc;
    bad.baseline = INFINITY;
    expect_hershey_refused(&bad, __LINE__);
    bad = desc;
    bad.metrics.ascender = bad.metrics.descender;
    expect_hershey_refused(&bad, __LINE__);
}

static void test_face_getter_arguments(void)
{
    APTEXT_FACE_METRICS metrics;
    memset(&metrics, 0, sizeof(metrics));
    metrics.base.type = APRI_STRUCT_TYPE_APTEXT_FACE_METRICS;
    CHECK(aptext_face_get_metrics(NULL, &metrics) == APRESULT_INVALID_ARGUMENT);
    uint32_t glyph = 7;
    CHECK(aptext_face_find_glyph(NULL, 0x61, &glyph) == APRESULT_INVALID_ARGUMENT);
    const ApriVec2 *points;
    const APTEXT_STROKE_POLYLINE *polylines;
    uint32_t point_count, polyline_count;
    CHECK(aptext_face_get_glyph_strokes(NULL, 0, &points, &point_count, &polylines, &polyline_count) ==
          APRESULT_INVALID_ARGUMENT);
    aptext_face_destroy(NULL);
}

static void test_chain_arguments(void)
{
    aptext_face none[1] = {NULL};
    APTEXT_CHAIN_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APTEXT_CHAIN_DESC;
    desc.faces = none;
    desc.face_count = 1;
    aptext_chain chain;
    APTEXT_ERROR error;

    CHECK(aptext_chain_create(&desc, NULL, &error) == APRESULT_INVALID_ARGUMENT);
    chain = (aptext_chain)SENTINEL;
    CHECK(aptext_chain_create(NULL, &chain, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(chain == NULL);
    APTEXT_CHAIN_DESC bad = desc;
    bad.base.type = APRI_STRUCT_TYPE_APTEXT_LAYOUT_DESC;
    CHECK(aptext_chain_create(&bad, &chain, &error) == APRESULT_STRUCT_WRONG_TYPE);
    APRI_STRUCT_BASE chained = {APRI_STRUCT_TYPE_APTEXT_CHAIN_DESC, NULL};
    bad = desc;
    bad.base.next = &chained;
    CHECK(aptext_chain_create(&bad, &chain, &error) == APRESULT_STRUCT_UNKNOWN_IN_CHAIN);
    bad = desc;
    bad.faces = NULL;
    CHECK(aptext_chain_create(&bad, &chain, &error) == APRESULT_INVALID_ARGUMENT);
    bad = desc;
    bad.face_count = 0;
    CHECK(aptext_chain_create(&bad, &chain, &error) == APRESULT_INVALID_ARGUMENT);
    error.message[0] = '\0';
    chain = (aptext_chain)SENTINEL;
    CHECK(aptext_chain_create(&desc, &chain, &error) == APRESULT_INVALID_ARGUMENT); // faces[0] is NULL
    CHECK(chain == NULL);
    CHECK(error.message[0] != '\0');

    CHECK(aptext_chain_get_face(NULL, 0) == NULL);
    CHECK(aptext_chain_get_face_count(NULL) == 0);
    uint32_t face_index, glyph;
    CHECK(aptext_chain_find_glyph(NULL, 0x61, &face_index, &glyph) == APRESULT_INVALID_ARGUMENT);
    aptext_chain_destroy(NULL);
}

static void test_layout_arguments(void)
{
    CHECK(aptext_layout_create(NULL) == APRESULT_INVALID_ARGUMENT);
    APTEXT_LAYOUT_DESC desc = layout_desc(NULL, "a");
    APTEXT_ERROR error;
    error.message[0] = '\0';
    CHECK(aptext_layout_build(NULL, &desc, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(error.message[0] != '\0');
    APTEXT_LAYOUT_INFO info;
    memset(&info, 0, sizeof(info));
    info.base.type = APRI_STRUCT_TYPE_APTEXT_LAYOUT_INFO;
    CHECK(aptext_layout_get_info(NULL, &info) == APRESULT_INVALID_ARGUMENT);
    const APTEXT_GLYPH *glyphs;
    const APTEXT_LINE *lines;
    const APTEXT_RUN *runs;
    uint32_t count;
    CHECK(aptext_layout_get_glyphs(NULL, &glyphs, &count) == APRESULT_INVALID_ARGUMENT);
    CHECK(aptext_layout_get_lines(NULL, &lines, &count) == APRESULT_INVALID_ARGUMENT);
    CHECK(aptext_layout_get_runs(NULL, &runs, &count) == APRESULT_INVALID_ARGUMENT);
    CHECK(aptext_layout_get_strokes(NULL, NULL, 0, &count) == APRESULT_INVALID_ARGUMENT);
    aptext_layout_destroy(NULL);
}

static void test_generate_arguments(void)
{
    APTEXT_GLYPH_GENERATE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APTEXT_GLYPH_GENERATE_DESC;
    desc.face = NULL;
    desc.glyph = 0;
    desc.pixels_per_em = 32.0f;
    desc.distance_range = 4.0f;
    APTEXT_GLYPH_IMAGE image;
    memset(&image, 0, sizeof(image));
    image.base.type = APRI_STRUCT_TYPE_APTEXT_GLYPH_IMAGE;
    APTEXT_ERROR error;

    CHECK(aptext_generate_glyph(NULL, NULL, 0, &image, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(error.message[0] != '\0');
    APTEXT_GLYPH_GENERATE_DESC bad = desc;
    bad.base.type = APRI_STRUCT_TYPE_APTEXT_ATLAS_DESC;
    CHECK(aptext_generate_glyph(&bad, NULL, 0, &image, &error) == APRESULT_STRUCT_WRONG_TYPE);
    APRI_STRUCT_BASE chained = {APRI_STRUCT_TYPE_APTEXT_GLYPH_GENERATE_DESC, NULL};
    bad = desc;
    bad.base.next = &chained;
    CHECK(aptext_generate_glyph(&bad, NULL, 0, &image, &error) == APRESULT_STRUCT_UNKNOWN_IN_CHAIN);
    error.message[0] = '\0';
    CHECK(aptext_generate_glyph(&desc, NULL, 0, &image, &error) == APRESULT_INVALID_ARGUMENT); // no face
    CHECK(error.message[0] != '\0');
}

static APTEXT_ATLAS_DESC atlas_desc(float pixels_per_em, float distance_range, uint32_t page, uint32_t max_pages)
{
    APTEXT_ATLAS_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APTEXT_ATLAS_DESC;
    desc.pixels_per_em = pixels_per_em;
    desc.distance_range = distance_range;
    desc.page_width = page;
    desc.page_height = page;
    desc.max_pages = max_pages;
    return desc;
}

static void expect_atlas_refused(const APTEXT_ATLAS_DESC *desc, int line)
{
    aptext_atlas atlas = (aptext_atlas)SENTINEL;
    APTEXT_ERROR error;
    error.message[0] = '\0';
    APRESULT result = aptext_atlas_create(desc, &atlas, &error);
    if (result != APRESULT_INVALID_ARGUMENT)
        printf("FAIL %s (%s:%d): expected APRESULT_INVALID_ARGUMENT, got %d\n", current_test, __FILE__, line,
               (int)result);
    if (result != APRESULT_INVALID_ARGUMENT)
        failures++;
    CHECK(atlas == NULL);
    CHECK(error.message[0] != '\0');
}

static void test_atlas_arguments(void)
{
    APTEXT_ATLAS_DESC desc = atlas_desc(32.0f, 4.0f, 256, 4);
    APTEXT_ATLAS_DESC bad;
    aptext_atlas atlas;
    APTEXT_ERROR error;

    CHECK(aptext_atlas_create(&desc, NULL, &error) == APRESULT_INVALID_ARGUMENT);
    atlas = (aptext_atlas)SENTINEL;
    CHECK(aptext_atlas_create(NULL, &atlas, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(atlas == NULL);
    bad = desc;
    bad.base.type = APRI_STRUCT_TYPE_APTEXT_ATLAS_LOAD_DESC;
    CHECK(aptext_atlas_create(&bad, &atlas, &error) == APRESULT_STRUCT_WRONG_TYPE);
    APRI_STRUCT_BASE chained = {APRI_STRUCT_TYPE_APTEXT_ATLAS_DESC, NULL};
    bad = desc;
    bad.base.next = &chained;
    CHECK(aptext_atlas_create(&bad, &atlas, &error) == APRESULT_STRUCT_UNKNOWN_IN_CHAIN);

    bad = desc;
    bad.pixels_per_em = 0.0f;
    expect_atlas_refused(&bad, __LINE__);
    bad.pixels_per_em = -32.0f;
    expect_atlas_refused(&bad, __LINE__);
    bad.pixels_per_em = NAN;
    expect_atlas_refused(&bad, __LINE__);
    bad = desc;
    bad.distance_range = 0.0f;
    expect_atlas_refused(&bad, __LINE__);
    bad.distance_range = INFINITY;
    expect_atlas_refused(&bad, __LINE__);
    bad = desc;
    bad.page_width = 0;
    expect_atlas_refused(&bad, __LINE__);
    bad = desc;
    bad.page_height = 0;
    expect_atlas_refused(&bad, __LINE__);
    bad = desc;
    bad.page_width = 35; // under 32 + 4
    expect_atlas_refused(&bad, __LINE__);
    bad = desc;
    bad.page_height = 35;
    expect_atlas_refused(&bad, __LINE__);
    bad = desc;
    bad.max_pages = 0;
    expect_atlas_refused(&bad, __LINE__);

    APTEXT_ATLAS_INFO info;
    memset(&info, 0, sizeof(info));
    info.base.type = APRI_STRUCT_TYPE_APTEXT_ATLAS_INFO;
    CHECK(aptext_atlas_get_info(NULL, &info) == APRESULT_INVALID_ARGUMENT);
    APTEXT_GLYPH_GENERATE_DESC generate;
    memset(&generate, 0, sizeof(generate));
    generate.base.type = APRI_STRUCT_TYPE_APTEXT_GLYPH_GENERATE_DESC;
    CHECK(aptext_atlas_insert(NULL, &generate, NULL, 0, NULL, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(aptext_atlas_add_layout(NULL, NULL, &error) == APRESULT_INVALID_ARGUMENT);
    APTEXT_ATLAS_GLYPH glyph;
    memset(&glyph, 0, sizeof(glyph));
    glyph.base.type = APRI_STRUCT_TYPE_APTEXT_ATLAS_GLYPH;
    CHECK(aptext_atlas_find_glyph(NULL, NULL, 0, &glyph) == APRESULT_INVALID_ARGUMENT);
    const void *pixels;
    CHECK(aptext_atlas_get_page(NULL, 0, &pixels) == APRESULT_INVALID_ARGUMENT);
    uint32_t count;
    CHECK(aptext_atlas_take_dirty(NULL, NULL, 0, &count) == APRESULT_INVALID_ARGUMENT);
    uint64_t size;
    CHECK(aptext_atlas_save(NULL, NULL, 0, NULL, 0, &size, &error) == APRESULT_INVALID_ARGUMENT);
    aptext_atlas_destroy(NULL);
}

static void test_atlas_load_arguments(void)
{
    static const uint8_t bytes[16] = {0};
    APTEXT_ATLAS_DESC settings = atlas_desc(32.0f, 4.0f, 256, 4);
    aptext_face none[1] = {NULL};
    APTEXT_ATLAS_LOAD_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APTEXT_ATLAS_LOAD_DESC;
    desc.atlas = &settings;
    desc.data = bytes;
    desc.data_size = sizeof(bytes);
    desc.faces = NULL;
    desc.face_count = 0;
    aptext_atlas atlas;
    APTEXT_ERROR error;

    CHECK(aptext_atlas_load(&desc, NULL, &error) == APRESULT_INVALID_ARGUMENT);
    atlas = (aptext_atlas)SENTINEL;
    CHECK(aptext_atlas_load(NULL, &atlas, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(atlas == NULL);
    APTEXT_ATLAS_LOAD_DESC bad = desc;
    bad.base.type = APRI_STRUCT_TYPE_APTEXT_ATLAS_DESC;
    CHECK(aptext_atlas_load(&bad, &atlas, &error) == APRESULT_STRUCT_WRONG_TYPE);
    bad = desc;
    bad.atlas = NULL;
    CHECK(aptext_atlas_load(&bad, &atlas, &error) == APRESULT_INVALID_ARGUMENT);
    APTEXT_ATLAS_DESC bad_settings = settings;
    bad_settings.base.type = APRI_STRUCT_TYPE_APTEXT_ATLAS_LOAD_DESC;
    bad = desc;
    bad.atlas = &bad_settings;
    CHECK(aptext_atlas_load(&bad, &atlas, &error) == APRESULT_STRUCT_WRONG_TYPE);
    bad_settings = settings;
    bad_settings.pixels_per_em = 0.0f;
    CHECK(aptext_atlas_load(&bad, &atlas, &error) == APRESULT_INVALID_ARGUMENT);
    bad = desc;
    bad.data = NULL;
    CHECK(aptext_atlas_load(&bad, &atlas, &error) == APRESULT_INVALID_ARGUMENT);
    bad = desc;
    bad.data_size = 0;
    CHECK(aptext_atlas_load(&bad, &atlas, &error) == APRESULT_INVALID_ARGUMENT);
    bad = desc;
    bad.face_count = 1;
    CHECK(aptext_atlas_load(&bad, &atlas, &error) == APRESULT_INVALID_ARGUMENT);
    bad = desc;
    bad.faces = none;
    bad.face_count = 1;
    error.message[0] = '\0';
    CHECK(aptext_atlas_load(&bad, &atlas, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(atlas == NULL);
    CHECK(error.message[0] != '\0');
}

// ---- Faces ------------------------------------------------------------------------

static void test_face_metrics(void)
{
    aptext_face a = open_face(&font_a, 0);
    if (!a)
        return;
    APTEXT_FACE_METRICS m;
    memset(&m, 0, sizeof(m));
    m.base.type = APRI_STRUCT_TYPE_APTEXT_FACE_METRICS;
    APRESULT result = aptext_face_get_metrics(a, &m);
    if (!BUILT(result))
        return;
    CHECK(result == APRESULT_OK);
    CHECK(m.base.type == APRI_STRUCT_TYPE_APTEXT_FACE_METRICS);
    CHECK(m.base.next == NULL);
    CHECK(m.kind == APTEXT_FACE_KIND_OUTLINE);
    CHECK(m.units_per_em == UPEM);
    CHECK(m.glyph_count == 9);
    CHECK(m.ascender == 0.75f);
    CHECK(m.descender == -0.25f);
    CHECK(m.line_gap == 0.0f);
    CHECK(m.underline_position == -0.125f);
    CHECK(m.underline_thickness == 0.0625f);
    CHECK(m.strikeout_position == 0.3125f);
    CHECK(m.strikeout_thickness == 0.0625f);
    CHECK(m.cap_height == 0.6875f);
    CHECK(m.x_height == 0.4375f);

    APTEXT_FACE_METRICS wrong = m;
    wrong.base.type = APRI_STRUCT_TYPE_APTEXT_LAYOUT_INFO;
    CHECK(aptext_face_get_metrics(a, &wrong) == APRESULT_STRUCT_WRONG_TYPE);
    CHECK(aptext_face_get_metrics(a, NULL) == APRESULT_INVALID_ARGUMENT);
}

static void test_face_find_glyph(void)
{
    aptext_face a = open_face(&font_a, 0);
    if (!a)
        return;
    uint32_t glyph = 99;
    APRESULT result = aptext_face_find_glyph(a, 0x61, &glyph);
    if (!BUILT(result))
        return;
    CHECK(result == APRESULT_OK);
    CHECK(glyph == A_LOWER_A);
    CHECK(aptext_face_find_glyph(a, 0x56, &glyph) == APRESULT_OK);
    CHECK(glyph == A_UPPER_V);
    glyph = 99;
    CHECK(aptext_face_find_glyph(a, 0x7A, &glyph) == APRESULT_NOT_FOUND);
    CHECK(glyph == 0);
    CHECK(aptext_face_find_glyph(a, 0x61, NULL) == APRESULT_INVALID_ARGUMENT);
}

static void test_face_not_a_font(void)
{
    uint8_t junk[256];
    for (int i = 0; i < 256; i++)
        junk[i] = (uint8_t)(i * 37 + 11);
    APTEXT_FACE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APTEXT_FACE_DESC;
    desc.data = junk;
    desc.data_size = sizeof(junk);
    aptext_face face = (aptext_face)SENTINEL;
    APTEXT_ERROR error;
    APRESULT result = aptext_face_create(&desc, &face, &error);
    if (!BUILT(result))
        return;
    CHECK(result == APRESULT_APTEXT_NOT_A_FONT);
    CHECK(face == NULL);
    CHECK(error.message[0] != '\0');
    track_face(result == APRESULT_OK ? face : NULL);

    // A font cut short.
    desc.data = font_a.data;
    desc.data_size = 40;
    face = (aptext_face)SENTINEL;
    result = aptext_face_create(&desc, &face, &error);
    if (!BUILT(result))
        return;
    CHECK(result == APRESULT_APTEXT_NOT_A_FONT);
    CHECK(face == NULL);
    track_face(result == APRESULT_OK ? face : NULL);
}

static void test_face_collection(void)
{
    aptext_face first = open_face(&font_ab_collection, 0);
    if (!first)
        return;
    aptext_face second = open_face(&font_ab_collection, 1);
    if (!second)
        return;
    APTEXT_FACE_METRICS m;
    memset(&m, 0, sizeof(m));
    m.base.type = APRI_STRUCT_TYPE_APTEXT_FACE_METRICS;
    APRESULT result = aptext_face_get_metrics(first, &m);
    if (!BUILT(result))
        return;
    CHECK(m.glyph_count == 9);
    CHECK(aptext_face_get_metrics(second, &m) == APRESULT_OK);
    CHECK(m.glyph_count == 6);

    APTEXT_FACE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APTEXT_FACE_DESC;
    desc.data = font_ab_collection.data;
    desc.data_size = font_ab_collection.size;
    desc.collection_index = 2;
    aptext_face face = (aptext_face)SENTINEL;
    APTEXT_ERROR error;
    CHECK(aptext_face_create(&desc, &face, &error) == APRESULT_NOT_FOUND);
    CHECK(face == NULL);
    // One font, asked for its second.
    desc.data = font_a.data;
    desc.data_size = font_a.size;
    desc.collection_index = 1;
    face = (aptext_face)SENTINEL;
    CHECK(aptext_face_create(&desc, &face, &error) == APRESULT_NOT_FOUND);
    CHECK(face == NULL);
}

static void test_face_copies_data(void)
{
    uint8_t *copy = (uint8_t *)track_memory(font_a.size);
    if (!copy)
        return;
    memcpy(copy, font_a.data, font_a.size);
    APTEXT_FACE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APTEXT_FACE_DESC;
    desc.data = copy;
    desc.data_size = font_a.size;
    desc.copy_data = true;
    aptext_face face = NULL;
    APTEXT_ERROR error;
    APRESULT result = aptext_face_create(&desc, &face, &error);
    if (!BUILT(result))
        return;
    CHECK(result == APRESULT_OK);
    track_face(face);
    if (result != APRESULT_OK)
        return;
    memset(copy, 0, font_a.size); // the caller's bytes are gone
    uint32_t glyph = 0;
    result = aptext_face_find_glyph(face, 0x62, &glyph);
    if (!BUILT(result))
        return;
    CHECK(result == APRESULT_OK);
    CHECK(glyph == A_LOWER_B);
    aptext_chain chain = open_chain(&face, 1);
    if (!chain)
        return;
    APTEXT_LAYOUT_DESC layout = layout_desc(chain, "ab");
    LaidOut out;
    if (!lay_out_and_read(&layout, &out))
        return;
    CHECK(out.glyph_count == 2);
    if (out.glyph_count == 2)
        CHECK(out.glyphs[1].x == 0.5f);
}

// ---- Fallback chains ------------------------------------------------------------------

static void test_chain_lookup(void)
{
    aptext_face a = NULL, b = NULL;
    aptext_chain chain = open_chain_ab(&a, &b);
    if (!chain)
        return;
    CHECK(aptext_chain_get_face_count(chain) == 2);
    CHECK(aptext_chain_get_face(chain, 0) == a);
    CHECK(aptext_chain_get_face(chain, 1) == b);
    CHECK(aptext_chain_get_face(chain, 2) == NULL);

    uint32_t face_index = 99, glyph = 99;
    APRESULT result = aptext_chain_find_glyph(chain, 0x61, &face_index, &glyph);
    if (!BUILT(result))
        return;
    CHECK(result == APRESULT_OK);
    CHECK(face_index == 0);
    CHECK(glyph == A_LOWER_A);
    CHECK(aptext_chain_find_glyph(chain, 0x7A, &face_index, &glyph) == APRESULT_OK);
    CHECK(face_index == 1);
    CHECK(glyph == B_LOWER_Z);
    face_index = 99;
    glyph = 99;
    CHECK(aptext_chain_find_glyph(chain, 0x51, &face_index, &glyph) == APRESULT_NOT_FOUND);
    CHECK(face_index == 0);
    CHECK(glyph == 0);
}

static void test_chain_refusals(void)
{
    aptext_face a = open_face(&font_a, 0);
    if (!a)
        return;
    aptext_face twice[2] = {a, a};
    APTEXT_CHAIN_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APTEXT_CHAIN_DESC;
    desc.faces = twice;
    desc.face_count = 2;
    aptext_chain chain = (aptext_chain)SENTINEL;
    APTEXT_ERROR error;
    APRESULT result = aptext_chain_create(&desc, &chain, &error);
    if (!BUILT(result))
        return;
    CHECK(result == APRESULT_INVALID_ARGUMENT);
    CHECK(chain == NULL);

    // Outline and stroke faces don't mix.
    APTEXT_STROKE_FACE_DESC stroke = stroke_desc();
    aptext_face s = NULL;
    result = aptext_face_create_stroke(&stroke, &s, &error);
    if (!BUILT(result))
        return;
    CHECK(result == APRESULT_OK);
    track_face(s);
    aptext_face mixed[2] = {a, s};
    desc.faces = mixed;
    chain = (aptext_chain)SENTINEL;
    result = aptext_chain_create(&desc, &chain, &error);
    if (!BUILT(result))
        return;
    CHECK(result == APRESULT_INVALID_ARGUMENT);
    CHECK(chain == NULL);
    CHECK(error.message[0] != '\0');
}

// ---- Shaping and layout --------------------------------------------------------------

static void test_layout_simple(void)
{
    aptext_face a = open_face(&font_a, 0);
    if (!a)
        return;
    aptext_chain chain = open_chain(&a, 1);
    if (!chain)
        return;
    APTEXT_LAYOUT_DESC desc = layout_desc(chain, "ab");
    LaidOut out;
    if (!lay_out_and_read(&desc, &out))
        return;
    CHECK(out.glyph_count == 2);
    CHECK(out.line_count == 1);
    CHECK(out.run_count == 1);
    CHECK(out.info.missing_codepoint_count == 0);
    CHECK(out.info.replaced_sequence_count == 0);
    CHECK(out.info.width == 1.0f);
    CHECK(out.info.height == 1.0f);
    if (out.glyph_count != 2 || out.line_count != 1 || out.run_count != 1)
        return;
    const APTEXT_GLYPH *g = out.glyphs;
    CHECK(g[0].glyph == A_LOWER_A);
    CHECK(g[0].face_index == 0);
    CHECK(g[0].x == 0.0f);
    CHECK(g[0].y == 0.75f);
    CHECK(g[0].advance == 0.5f);
    CHECK(g[0].text_offset == 0);
    CHECK(g[0].text_size == 1);
    CHECK(g[0].line == 0);
    CHECK(g[0].run == 0);
    CHECK(g[0].box_left == 0.0f);
    CHECK(g[0].box_top == 0.0f);
    CHECK(g[0].box_right == 0.5f);
    CHECK(g[0].box_bottom == 1.0f);
    CHECK(g[1].glyph == A_LOWER_B);
    CHECK(g[1].x == 0.5f);
    CHECK(g[1].y == 0.75f);
    CHECK(g[1].text_offset == 1);
    CHECK(g[1].text_size == 1);
    CHECK(g[1].box_right == 1.0f);

    const APTEXT_LINE *l = out.lines;
    CHECK(l[0].first_glyph == 0);
    CHECK(l[0].glyph_count == 2);
    CHECK(l[0].first_run == 0);
    CHECK(l[0].run_count == 1);
    CHECK(l[0].text_offset == 0);
    CHECK(l[0].text_size == 2);
    CHECK(l[0].baseline == 0.75f);
    CHECK(l[0].box_left == 0.0f);
    CHECK(l[0].box_top == 0.0f);
    CHECK(l[0].box_right == 1.0f);
    CHECK(l[0].box_bottom == 1.0f);

    const APTEXT_RUN *r = out.runs;
    CHECK(r[0].first_glyph == 0);
    CHECK(r[0].glyph_count == 2);
    CHECK(r[0].text_offset == 0);
    CHECK(r[0].text_size == 2);
    CHECK(r[0].face_index == 0);
    CHECK(r[0].script == APTEXT_TAG('L', 'a', 't', 'n'));
    CHECK(r[0].bidi_level == 0);
    CHECK(r[0].direction == APTEXT_DIRECTION_LTR);
    CHECK(r[0].line == 0);
}

static void test_layout_em_size(void)
{
    aptext_face a = open_face(&font_a, 0);
    if (!a)
        return;
    aptext_chain chain = open_chain(&a, 1);
    if (!chain)
        return;
    APTEXT_LAYOUT_DESC desc = layout_desc(chain, "ab");
    desc.em_size = 32.0f;
    LaidOut out;
    if (!lay_out_and_read(&desc, &out))
        return;
    CHECK(out.glyph_count == 2);
    if (out.glyph_count != 2)
        return;
    CHECK(out.glyphs[1].x == 16.0f);
    CHECK(out.glyphs[1].y == 24.0f);
    CHECK(out.glyphs[1].advance == 16.0f);
    CHECK(out.info.width == 32.0f);
    CHECK(out.info.height == 32.0f);
}

static void test_layout_kerning(void)
{
    aptext_face a = open_face(&font_a, 0);
    if (!a)
        return;
    aptext_chain chain = open_chain(&a, 1);
    if (!chain)
        return;
    APTEXT_LAYOUT_DESC desc = layout_desc(chain, "AV");
    LaidOut out;
    if (!lay_out_and_read(&desc, &out))
        return;
    CHECK(out.glyph_count == 2);
    // Kerned by -0.125 em. How a shaper shares a pair's kerning between
    // the two glyphs' advances and offsets is its own (HarfBuzz splits a
    // legacy kern table's value), so only where V lands and the line's
    // width are checked.
    if (out.glyph_count == 2)
    {
        CHECK(out.glyphs[0].glyph == A_UPPER_A);
        CHECK(out.glyphs[1].glyph == A_UPPER_V);
        CHECK(out.glyphs[1].x == 0.5f);
    }
    CHECK(out.info.width == 1.125f);
    desc.features = APTEXT_FEATURE_LIGATURES;
    if (!lay_out_and_read(&desc, &out))
        return;
    CHECK(out.glyph_count == 2);
    if (out.glyph_count == 2)
    {
        CHECK(out.glyphs[1].x == 0.625f);
        CHECK(out.glyphs[0].advance == 0.625f);
    }
    CHECK(out.info.width == 1.25f);
}

static void test_layout_ligature(void)
{
    aptext_face a = open_face(&font_a, 0);
    if (!a)
        return;
    aptext_chain chain = open_chain(&a, 1);
    if (!chain)
        return;
    APTEXT_LAYOUT_DESC desc = layout_desc(chain, "fib");
    LaidOut out;
    if (!lay_out_and_read(&desc, &out))
        return;
    CHECK(out.glyph_count == 2);
    if (out.glyph_count == 2)
    {
        CHECK(out.glyphs[0].glyph == A_FI);
        CHECK(out.glyphs[0].text_offset == 0);
        CHECK(out.glyphs[0].text_size == 2);
        CHECK(out.glyphs[0].advance == 0.5f);
        CHECK(out.glyphs[1].glyph == A_LOWER_B);
        CHECK(out.glyphs[1].text_offset == 2);
        CHECK(out.glyphs[1].x == 0.5f);
    }
    desc.features = APTEXT_FEATURE_KERNING;
    if (!lay_out_and_read(&desc, &out))
        return;
    CHECK(out.glyph_count == 3);
    if (out.glyph_count == 3)
    {
        CHECK(out.glyphs[0].glyph == A_LOWER_F);
        CHECK(out.glyphs[1].glyph == A_LOWER_I);
        CHECK(out.glyphs[1].x == 0.3125f);
        CHECK(out.glyphs[2].x == 0.5f);
    }
    // The same ligature switched off through an extra feature is refused:
    // 'liga' is APTEXT_FEATURE_LIGATURES'.
    APTEXT_FEATURE liga = {APTEXT_TAG('l', 'i', 'g', 'a'), 0};
    desc.extra_features = &liga;
    desc.extra_feature_count = 1;
    aptext_layout layout = NULL;
    if (!BUILT(aptext_layout_create(&layout)))
        return;
    track_layout(layout);
    APTEXT_ERROR error;
    APRESULT result = aptext_layout_build(layout, &desc, &error);
    if (!BUILT(result))
        return;
    CHECK(result == APRESULT_INVALID_ARGUMENT);
}

static void test_layout_fallback(void)
{
    aptext_chain chain = open_chain_ab(NULL, NULL);
    if (!chain)
        return;
    APTEXT_LAYOUT_DESC desc = layout_desc(chain, "az");
    LaidOut out;
    if (!lay_out_and_read(&desc, &out))
        return;
    CHECK(out.glyph_count == 2);
    CHECK(out.run_count == 2);
    CHECK(out.info.missing_codepoint_count == 0);
    if (out.glyph_count == 2)
    {
        CHECK(out.glyphs[0].face_index == 0);
        CHECK(out.glyphs[0].glyph == A_LOWER_A);
        CHECK(out.glyphs[1].face_index == 1);
        CHECK(out.glyphs[1].glyph == B_LOWER_Z);
        CHECK(out.glyphs[1].x == 0.5f);
        CHECK(out.glyphs[1].advance == 0.375f);
        CHECK(out.glyphs[1].run == 1);
    }
    if (out.run_count == 2)
    {
        CHECK(out.runs[0].face_index == 0);
        CHECK(out.runs[1].face_index == 1);
        CHECK(out.runs[1].first_glyph == 1);
        CHECK(out.runs[1].text_offset == 1);
    }

    // The first face with the glyph wins: B before A.
    aptext_face reversed[2];
    reversed[0] = aptext_chain_get_face(chain, 1);
    reversed[1] = aptext_chain_get_face(chain, 0);
    aptext_chain chain_ba = open_chain(reversed, 2);
    if (!chain_ba)
        return;
    desc = layout_desc(chain_ba, "a");
    if (!lay_out_and_read(&desc, &out))
        return;
    CHECK(out.glyph_count == 1);
    if (out.glyph_count == 1)
    {
        CHECK(out.glyphs[0].face_index == 0);
        CHECK(out.glyphs[0].glyph == B_LOWER_A);
        CHECK(out.glyphs[0].advance == 0.75f);
    }
}

static void test_layout_notdef(void)
{
    aptext_chain chain = open_chain_ab(NULL, NULL);
    if (!chain)
        return;
    APTEXT_LAYOUT_DESC desc = layout_desc(chain, "aQb");
    LaidOut out;
    if (!lay_out_and_read(&desc, &out))
        return;
    CHECK(out.glyph_count == 3); // never skipped
    CHECK(out.info.missing_codepoint_count == 1);
    if (out.glyph_count != 3)
        return;
    CHECK(out.glyphs[1].face_index == 0);
    CHECK(out.glyphs[1].glyph == 0);
    CHECK(out.glyphs[1].text_offset == 1);
    CHECK(out.glyphs[1].advance == 0.5f);
    CHECK(out.glyphs[2].x == 1.0f);
}

static void test_layout_invalid_utf8(void)
{
    aptext_face a = open_face(&font_a, 0);
    if (!a)
        return;
    aptext_chain chain = open_chain(&a, 1);
    if (!chain)
        return;
    APTEXT_LAYOUT_DESC desc = layout_desc(chain, "a\xFF"
                                                 "b");
    LaidOut out;
    if (!lay_out_and_read(&desc, &out))
        return;
    CHECK(out.glyph_count == 3);
    CHECK(out.info.replaced_sequence_count == 1);
    CHECK(out.info.missing_codepoint_count == 1); // U+FFFD, which neither font has
    if (out.glyph_count != 3)
        return;
    CHECK(out.glyphs[0].glyph == A_LOWER_A);
    CHECK(out.glyphs[1].glyph == 0);
    CHECK(out.glyphs[1].text_offset == 1);
    CHECK(out.glyphs[1].text_size == 1);
    CHECK(out.glyphs[2].glyph == A_LOWER_B);
    CHECK(out.glyphs[2].text_offset == 2);

    // A sequence cut short at the end of the text.
    desc = layout_desc(chain, "a\xE4\xB8");
    if (!lay_out_and_read(&desc, &out))
        return;
    CHECK(out.glyph_count == 2);
    CHECK(out.info.replaced_sequence_count == 1);
}

static void test_layout_default_ignorable(void)
{
    aptext_face a = open_face(&font_a, 0);
    if (!a)
        return;
    aptext_chain chain = open_chain(&a, 1);
    if (!chain)
        return;
    // U+200D ZERO WIDTH JOINER between two letters: no glyph, not missing.
    APTEXT_LAYOUT_DESC desc = layout_desc(chain, "a\xE2\x80\x8D"
                                                 "b");
    LaidOut out;
    if (!lay_out_and_read(&desc, &out))
        return;
    CHECK(out.glyph_count == 2);
    CHECK(out.info.missing_codepoint_count == 0);
    if (out.glyph_count == 2)
        CHECK(out.glyphs[1].x == 0.5f);
}

static void test_layout_wrapping(void)
{
    aptext_face a = open_face(&font_a, 0);
    if (!a)
        return;
    aptext_chain chain = open_chain(&a, 1);
    if (!chain)
        return;
    // "aa aa " is 2.25 em with its space hanging, "aa aa aa" 3.5.
    APTEXT_LAYOUT_DESC desc = layout_desc(chain, "aa aa aa");
    desc.max_width = 2.5f;
    LaidOut out;
    if (!lay_out_and_read(&desc, &out))
        return;
    CHECK(out.line_count == 2);
    CHECK(out.glyph_count == 8);
    CHECK(out.info.width == 2.25f);
    CHECK(out.info.height == 2.0f);
    if (out.line_count != 2 || out.glyph_count != 8)
        return;
    CHECK(out.lines[0].first_glyph == 0);
    CHECK(out.lines[0].glyph_count == 6);
    CHECK(out.lines[0].text_offset == 0);
    CHECK(out.lines[0].text_size == 6);
    CHECK(out.lines[0].box_right == 2.25f);
    CHECK(out.lines[1].first_glyph == 6);
    CHECK(out.lines[1].glyph_count == 2);
    CHECK(out.lines[1].text_offset == 6);
    CHECK(out.lines[1].text_size == 2);
    CHECK(out.lines[1].baseline == 1.75f);
    CHECK(out.lines[1].box_top == 1.0f);
    CHECK(out.lines[1].box_bottom == 2.0f);
    CHECK(out.glyphs[5].glyph == A_SPACE);
    CHECK(out.glyphs[5].line == 0);
    CHECK(out.glyphs[5].x == 2.25f); // the hanging space, after "aa aa"
    CHECK(out.glyphs[6].line == 1);
    CHECK(out.glyphs[6].x == 0.0f);
    CHECK(out.glyphs[6].y == 1.75f);
    CHECK(out.glyphs[7].x == 0.5f);
    CHECK(out.glyphs[7].box_top == 1.0f);

    // A word wider than the line wraps between clusters.
    desc = layout_desc(chain, "aaaaa");
    desc.max_width = 1.2f;
    if (!lay_out_and_read(&desc, &out))
        return;
    CHECK(out.line_count == 3);
    if (out.line_count == 3)
    {
        CHECK(out.lines[0].glyph_count == 2);
        CHECK(out.lines[1].glyph_count == 2);
        CHECK(out.lines[2].glyph_count == 1);
        CHECK(out.lines[2].text_offset == 4);
    }
}

static void test_layout_newlines(void)
{
    aptext_face a = open_face(&font_a, 0);
    if (!a)
        return;
    aptext_chain chain = open_chain(&a, 1);
    if (!chain)
        return;
    APTEXT_LAYOUT_DESC desc = layout_desc(chain, "a\nb");
    LaidOut out;
    if (!lay_out_and_read(&desc, &out))
        return;
    CHECK(out.line_count == 2);
    CHECK(out.glyph_count == 2); // the break gives no glyph
    CHECK(out.info.missing_codepoint_count == 0);
    if (out.line_count == 2 && out.glyph_count == 2)
    {
        CHECK(out.lines[0].text_offset == 0);
        CHECK(out.lines[0].text_size == 2);
        CHECK(out.lines[0].glyph_count == 1);
        CHECK(out.lines[1].text_offset == 2);
        CHECK(out.lines[1].text_size == 1);
        CHECK(out.lines[1].first_glyph == 1);
        CHECK(out.glyphs[1].line == 1);
        CHECK(out.glyphs[1].x == 0.0f);
        CHECK(out.glyphs[1].y == 1.75f);
    }

    desc = layout_desc(chain, "a\r\nb");
    if (!lay_out_and_read(&desc, &out))
        return;
    CHECK(out.line_count == 2);
    CHECK(out.glyph_count == 2);
    if (out.line_count == 2)
        CHECK(out.lines[0].text_size == 3);

    // A break at the end leaves a last, empty line.
    desc = layout_desc(chain, "a\n");
    if (!lay_out_and_read(&desc, &out))
        return;
    CHECK(out.line_count == 2);
    if (out.line_count == 2)
        CHECK(out.lines[1].glyph_count == 0);
    CHECK(out.info.height == 2.0f);

    // U+2028 LINE SEPARATOR is a mandatory break too.
    desc = layout_desc(chain, "a\xE2\x80\xA8"
                              "b");
    if (!lay_out_and_read(&desc, &out))
        return;
    CHECK(out.line_count == 2);
    CHECK(out.glyph_count == 2);
}

static void test_layout_empty(void)
{
    aptext_face a = open_face(&font_a, 0);
    if (!a)
        return;
    aptext_chain chain = open_chain(&a, 1);
    if (!chain)
        return;
    APTEXT_LAYOUT_DESC desc = layout_desc(chain, "");
    desc.text = NULL;
    LaidOut out;
    if (!lay_out_and_read(&desc, &out))
        return;
    CHECK(out.glyph_count == 0);
    CHECK(out.glyphs == NULL);
    CHECK(out.line_count == 1);
    CHECK(out.info.width == 0.0f);
    CHECK(out.info.height == 1.0f);
    if (out.line_count == 1)
    {
        CHECK(out.lines[0].glyph_count == 0);
        CHECK(out.lines[0].baseline == 0.75f);
    }
}

static void test_layout_alignment(void)
{
    aptext_face a = open_face(&font_a, 0);
    if (!a)
        return;
    aptext_chain chain = open_chain(&a, 1);
    if (!chain)
        return;
    APTEXT_LAYOUT_DESC desc = layout_desc(chain, "ab");
    desc.max_width = 4.0f;
    desc.align = APTEXT_ALIGN_CENTER;
    LaidOut out;
    if (!lay_out_and_read(&desc, &out))
        return;
    if (out.line_count == 1 && out.glyph_count == 2)
    {
        CHECK(out.lines[0].box_left == 1.5f);
        CHECK(out.lines[0].box_right == 2.5f);
        CHECK(out.glyphs[0].x == 1.5f);
    }
    desc.align = APTEXT_ALIGN_RIGHT;
    if (!lay_out_and_read(&desc, &out))
        return;
    if (out.glyph_count == 2)
        CHECK(out.glyphs[0].x == 3.0f);
    desc.align = APTEXT_ALIGN_END; // a left-to-right paragraph ends on the right
    if (!lay_out_and_read(&desc, &out))
        return;
    if (out.glyph_count == 2)
        CHECK(out.glyphs[0].x == 3.0f);
    desc.align = APTEXT_ALIGN_START;
    if (!lay_out_and_read(&desc, &out))
        return;
    if (out.glyph_count == 2)
        CHECK(out.glyphs[0].x == 0.0f);

    // With no max_width, lines align within the widest.
    desc = layout_desc(chain, "ab\na");
    desc.align = APTEXT_ALIGN_CENTER;
    if (!lay_out_and_read(&desc, &out))
        return;
    CHECK(out.line_count == 2);
    if (out.line_count == 2)
    {
        CHECK(out.lines[0].box_left == 0.0f);
        CHECK(out.lines[1].box_left == 0.25f);
    }
    CHECK(out.info.width == 1.0f);
}

static void test_layout_spacing(void)
{
    aptext_face a = open_face(&font_a, 0);
    if (!a)
        return;
    aptext_chain chain = open_chain(&a, 1);
    if (!chain)
        return;
    APTEXT_LAYOUT_DESC desc = layout_desc(chain, "ab");
    desc.letter_spacing = 0.25f;
    LaidOut out;
    if (!lay_out_and_read(&desc, &out))
        return;
    CHECK(out.glyph_count == 2);
    if (out.glyph_count == 2)
    {
        CHECK(out.glyphs[0].advance == 0.75f);
        CHECK(out.glyphs[1].x == 0.75f);
        CHECK(out.glyphs[1].advance == 0.5f); // none after the last cluster
    }
    CHECK(out.info.width == 1.25f);

    desc = layout_desc(chain, "a\na");
    desc.line_spacing = 2.0f;
    if (!lay_out_and_read(&desc, &out))
        return;
    CHECK(out.line_count == 2);
    if (out.line_count == 2)
    {
        CHECK(out.lines[0].baseline == 1.25f);
        CHECK(out.lines[1].baseline == 3.25f);
        CHECK(out.lines[1].box_top == 2.0f);
        CHECK(out.lines[1].box_bottom == 4.0f);
    }
    CHECK(out.info.height == 4.0f);
}

static void test_layout_scripts_and_bidi(void)
{
    aptext_chain chain = open_chain_ab(NULL, NULL);
    if (!chain)
        return;
    // 'a' then U+4E00: a Latin run from A, a Han run from B.
    APTEXT_LAYOUT_DESC desc = layout_desc(chain, "a\xE4\xB8\x80");
    LaidOut out;
    if (!lay_out_and_read(&desc, &out))
        return;
    CHECK(out.run_count == 2);
    if (out.run_count == 2)
    {
        CHECK(out.runs[0].script == APTEXT_TAG('L', 'a', 't', 'n'));
        CHECK(out.runs[1].script == APTEXT_TAG('H', 'a', 'n', 'i'));
        CHECK(out.runs[1].face_index == 1);
        CHECK(out.runs[1].text_offset == 1);
        CHECK(out.runs[1].text_size == 3);
    }
    CHECK(out.info.width == 1.5f);

    // 'a', alef, bet in a left-to-right paragraph: the Hebrew run is
    // reversed, so visual order is a, bet, alef.
    desc = layout_desc(chain, "a\xD7\x90\xD7\x91");
    if (!lay_out_and_read(&desc, &out))
        return;
    CHECK(out.glyph_count == 3);
    CHECK(out.run_count == 2);
    if (out.glyph_count == 3)
    {
        CHECK(out.glyphs[0].glyph == A_LOWER_A);
        CHECK(out.glyphs[1].glyph == B_BET);
        CHECK(out.glyphs[1].text_offset == 3);
        CHECK(out.glyphs[1].x == 0.5f);
        CHECK(out.glyphs[2].glyph == B_ALEF);
        CHECK(out.glyphs[2].text_offset == 1);
        CHECK(out.glyphs[2].x == 0.875f);
    }
    if (out.run_count == 2)
    {
        CHECK(out.runs[0].direction == APTEXT_DIRECTION_LTR);
        CHECK(out.runs[0].bidi_level == 0);
        CHECK(out.runs[1].direction == APTEXT_DIRECTION_RTL);
        CHECK(out.runs[1].bidi_level == 1);
        CHECK(out.runs[1].script == APTEXT_TAG('H', 'e', 'b', 'r'));
    }

    // Latin in a right-to-left paragraph keeps its order, at level 2, and
    // starts from the right.
    desc = layout_desc(chain, "ab");
    desc.direction = APTEXT_DIRECTION_RTL;
    desc.max_width = 4.0f;
    desc.align = APTEXT_ALIGN_START;
    if (!lay_out_and_read(&desc, &out))
        return;
    CHECK(out.glyph_count == 2);
    if (out.glyph_count == 2)
    {
        CHECK(out.glyphs[0].glyph == A_LOWER_A);
        CHECK(out.glyphs[0].x == 3.0f);
    }
    if (out.run_count == 1)
    {
        CHECK(out.runs[0].bidi_level == 2);
        CHECK(out.runs[0].direction == APTEXT_DIRECTION_LTR);
    }

    // AUTO takes the paragraph's direction from its first strong character.
    desc = layout_desc(chain, "\xD7\x90"
                              "a");
    desc.direction = APTEXT_DIRECTION_AUTO;
    if (!lay_out_and_read(&desc, &out))
        return;
    CHECK(out.glyph_count == 2);
    if (out.glyph_count == 2)
    {
        CHECK(out.glyphs[0].glyph == A_LOWER_A); // rightmost in logical order, leftmost on screen
        CHECK(out.glyphs[1].glyph == B_ALEF);
    }
}

static void test_layout_deterministic(void)
{
    aptext_chain chain = open_chain_ab(NULL, NULL);
    if (!chain)
        return;
    APTEXT_LAYOUT_DESC desc = layout_desc(chain, "fi AV az\naQ a\xD7\x90\xD7\x91 aa aa aa");
    desc.max_width = 3.0f;
    desc.em_size = 16.0f;
    LaidOut first, second;
    if (!lay_out_and_read(&desc, &first))
        return;
    aptext_layout other = lay_out(&desc);
    if (!other)
        return;
    if (!read_layout(other, &second))
        return;
    CHECK(first.glyph_count == second.glyph_count);
    CHECK(first.line_count == second.line_count);
    CHECK(first.run_count == second.run_count);
    if (first.glyph_count == second.glyph_count && first.glyph_count)
        CHECK(memcmp(first.glyphs, second.glyphs, first.glyph_count * sizeof(APTEXT_GLYPH)) == 0);
    if (first.line_count == second.line_count && first.line_count)
        CHECK(memcmp(first.lines, second.lines, first.line_count * sizeof(APTEXT_LINE)) == 0);
    if (first.run_count == second.run_count && first.run_count)
        CHECK(memcmp(first.runs, second.runs, first.run_count * sizeof(APTEXT_RUN)) == 0);
}

static void test_layout_rebuild_and_failure(void)
{
    aptext_face a = open_face(&font_a, 0);
    if (!a)
        return;
    aptext_chain chain = open_chain(&a, 1);
    if (!chain)
        return;
    aptext_layout layout = NULL;
    APRESULT result = aptext_layout_create(&layout);
    if (!BUILT(result))
        return;
    CHECK(result == APRESULT_OK);
    track_layout(layout);
    LaidOut out;
    if (!read_layout(layout, &out))
        return;
    CHECK(out.glyph_count == 0); // never built
    CHECK(out.line_count == 0);
    CHECK(out.glyphs == NULL);

    APTEXT_LAYOUT_DESC desc = layout_desc(chain, "ab");
    APTEXT_ERROR error;
    result = aptext_layout_build(layout, &desc, &error);
    if (!BUILT(result))
        return;
    CHECK(result == APRESULT_OK);
    desc = layout_desc(chain, "b");
    CHECK(aptext_layout_build(layout, &desc, &error) == APRESULT_OK);
    if (!read_layout(layout, &out))
        return;
    CHECK(out.glyph_count == 1);

    // The argument checks a layout makes, each on its own.
    APTEXT_LAYOUT_DESC bad;
    bad = desc;
    bad.base.type = APRI_STRUCT_TYPE_APTEXT_CHAIN_DESC;
    CHECK(aptext_layout_build(layout, &bad, &error) == APRESULT_STRUCT_WRONG_TYPE);
    CHECK(aptext_layout_build(layout, NULL, &error) == APRESULT_INVALID_ARGUMENT);
    bad = desc;
    bad.chain = NULL;
    CHECK(aptext_layout_build(layout, &bad, &error) == APRESULT_INVALID_ARGUMENT);
    bad = desc;
    bad.text = NULL;
    CHECK(aptext_layout_build(layout, &bad, &error) == APRESULT_INVALID_ARGUMENT);
    bad = desc;
    bad.direction = (APTEXT_DIRECTION)0;
    CHECK(aptext_layout_build(layout, &bad, &error) == APRESULT_INVALID_ARGUMENT);
    bad.direction = (APTEXT_DIRECTION)4;
    CHECK(aptext_layout_build(layout, &bad, &error) == APRESULT_INVALID_ARGUMENT);
    bad = desc;
    bad.features = 0x80;
    CHECK(aptext_layout_build(layout, &bad, &error) == APRESULT_INVALID_ARGUMENT);
    APTEXT_FEATURE twice[2] = {{APTEXT_TAG('s', 'm', 'c', 'p'), 1}, {APTEXT_TAG('s', 'm', 'c', 'p'), 0}};
    bad = desc;
    bad.extra_features = twice;
    bad.extra_feature_count = 2;
    CHECK(aptext_layout_build(layout, &bad, &error) == APRESULT_INVALID_ARGUMENT);
    bad.extra_features = NULL;
    CHECK(aptext_layout_build(layout, &bad, &error) == APRESULT_INVALID_ARGUMENT);
    bad = desc;
    bad.em_size = 0.0f;
    CHECK(aptext_layout_build(layout, &bad, &error) == APRESULT_INVALID_ARGUMENT);
    bad.em_size = NAN;
    CHECK(aptext_layout_build(layout, &bad, &error) == APRESULT_INVALID_ARGUMENT);
    bad = desc;
    bad.max_width = -1.0f;
    CHECK(aptext_layout_build(layout, &bad, &error) == APRESULT_INVALID_ARGUMENT);
    bad.max_width = INFINITY;
    CHECK(aptext_layout_build(layout, &bad, &error) == APRESULT_INVALID_ARGUMENT);
    bad = desc;
    bad.align = (APTEXT_ALIGN)0;
    CHECK(aptext_layout_build(layout, &bad, &error) == APRESULT_INVALID_ARGUMENT);
    bad.align = (APTEXT_ALIGN)6;
    CHECK(aptext_layout_build(layout, &bad, &error) == APRESULT_INVALID_ARGUMENT);
    bad = desc;
    bad.line_spacing = 0.0f;
    CHECK(aptext_layout_build(layout, &bad, &error) == APRESULT_INVALID_ARGUMENT);
    bad = desc;
    bad.letter_spacing = NAN;
    error.message[0] = '\0';
    CHECK(aptext_layout_build(layout, &bad, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(error.message[0] != '\0');

    // A failed build leaves the layout empty.
    if (!read_layout(layout, &out))
        return;
    CHECK(out.glyph_count == 0);
    CHECK(out.line_count == 0);

    const APTEXT_GLYPH *glyphs;
    uint32_t count;
    CHECK(aptext_layout_get_glyphs(layout, NULL, &count) == APRESULT_INVALID_ARGUMENT);
    CHECK(aptext_layout_get_glyphs(layout, &glyphs, NULL) == APRESULT_INVALID_ARGUMENT);
    APTEXT_LAYOUT_INFO info;
    memset(&info, 0, sizeof(info));
    info.base.type = APRI_STRUCT_TYPE_APTEXT_FACE_METRICS;
    CHECK(aptext_layout_get_info(layout, &info) == APRESULT_STRUCT_WRONG_TYPE);
}

// ---- Single-stroke faces ----------------------------------------------------------------

static void test_stroke_face(void)
{
    APTEXT_STROKE_FACE_DESC desc = stroke_desc();
    aptext_face face = NULL;
    APTEXT_ERROR error;
    APRESULT result = aptext_face_create_stroke(&desc, &face, &error);
    if (!BUILT(result))
        return;
    CHECK(result == APRESULT_OK);
    track_face(face);
    if (result != APRESULT_OK)
        return;

    APTEXT_FACE_METRICS m;
    memset(&m, 0, sizeof(m));
    m.base.type = APRI_STRUCT_TYPE_APTEXT_FACE_METRICS;
    result = aptext_face_get_metrics(face, &m);
    if (!BUILT(result))
        return;
    CHECK(m.kind == APTEXT_FACE_KIND_STROKE);
    CHECK(m.units_per_em == 0);
    CHECK(m.glyph_count == 3); // .notdef and two
    CHECK(m.ascender == 0.75f);
    CHECK(m.descender == -0.25f);

    uint32_t glyph = 0;
    result = aptext_face_find_glyph(face, 0x4C, &glyph);
    if (!BUILT(result))
        return;
    CHECK(result == APRESULT_OK);
    CHECK(glyph == 1); // glyphs[0]

    const ApriVec2 *points = NULL;
    const APTEXT_STROKE_POLYLINE *polylines = NULL;
    uint32_t point_count = 0, polyline_count = 0;
    result = aptext_face_get_glyph_strokes(face, 1, &points, &point_count, &polylines, &polyline_count);
    if (!BUILT(result))
        return;
    CHECK(result == APRESULT_OK);
    CHECK(point_count == 3);
    CHECK(polyline_count == 1);
    if (point_count == 3 && polyline_count == 1)
    {
        CHECK(polylines[0].first_point == 0);
        CHECK(polylines[0].point_count == 3);
        CHECK(points[0].x == 0.125f);
        CHECK(points[0].y == -0.75f); // y down
        CHECK(points[1].y == 0.0f);
        CHECK(points[2].x == 0.5f);
    }
    CHECK(aptext_face_get_glyph_strokes(face, 3, &points, &point_count, &polylines, &polyline_count) ==
          APRESULT_NOT_FOUND);

    aptext_chain chain = open_chain(&face, 1);
    if (!chain)
        return;
    APTEXT_LAYOUT_DESC layout_d = layout_desc(chain, "L L");
    aptext_layout layout = lay_out(&layout_d);
    if (!layout)
        return;
    LaidOut out;
    if (!read_layout(layout, &out))
        return;
    CHECK(out.glyph_count == 3);
    if (out.glyph_count == 3)
    {
        CHECK(out.glyphs[1].x == 0.625f);
        CHECK(out.glyphs[2].x == 0.875f);
    }
    uint32_t count = 0;
    result = aptext_layout_get_strokes(layout, NULL, 0, &count);
    if (!BUILT(result))
        return;
    CHECK(result == APRESULT_OK);
    CHECK(count == 4);
    APTEXT_SEGMENT segments[4];
    CHECK(aptext_layout_get_strokes(layout, segments, 2, &count) == APRESULT_LIMIT);
    CHECK(aptext_layout_get_strokes(layout, segments, 4, &count) == APRESULT_OK);
    CHECK(count == 4);
    if (count == 4)
    {
        CHECK(segments[0].x0 == 0.125f);
        CHECK(segments[0].y0 == 0.0f);
        CHECK(segments[0].x1 == 0.125f);
        CHECK(segments[0].y1 == 0.75f);
        CHECK(segments[0].glyph_index == 0);
        CHECK(segments[1].x1 == 0.5f);
        CHECK(segments[1].y1 == 0.75f);
        CHECK(segments[2].x0 == 1.0f);
        CHECK(segments[2].glyph_index == 2);
        CHECK(segments[3].x1 == 1.375f);
    }

    // A stroke face has no distance field, and a stroke layout no atlas
    // glyphs.
    APTEXT_GLYPH_GENERATE_DESC generate;
    memset(&generate, 0, sizeof(generate));
    generate.base.type = APRI_STRUCT_TYPE_APTEXT_GLYPH_GENERATE_DESC;
    generate.face = face;
    generate.glyph = 1;
    generate.pixels_per_em = 32.0f;
    generate.distance_range = 4.0f;
    APTEXT_GLYPH_IMAGE image;
    memset(&image, 0, sizeof(image));
    image.base.type = APRI_STRUCT_TYPE_APTEXT_GLYPH_IMAGE;
    result = aptext_generate_glyph(&generate, NULL, 0, &image, &error);
    if (!BUILT(result))
        return;
    CHECK(result == APRESULT_INVALID_ARGUMENT);
    APTEXT_ATLAS_DESC settings = atlas_desc(32.0f, 4.0f, 64, 4);
    aptext_atlas atlas = NULL;
    result = aptext_atlas_create(&settings, &atlas, &error);
    if (!BUILT(result))
        return;
    track_atlas(atlas);
    result = aptext_atlas_add_layout(atlas, layout, &error);
    if (!BUILT(result))
        return;
    CHECK(result == APRESULT_INVALID_ARGUMENT);
}

static void test_stroke_face_duplicates(void)
{
    APTEXT_STROKE_GLYPH glyphs[2];
    memcpy(glyphs, stroke_glyphs, sizeof(glyphs));
    glyphs[1].codepoint = glyphs[0].codepoint;
    APTEXT_STROKE_FACE_DESC desc = stroke_desc();
    desc.glyphs = glyphs;
    aptext_face face = (aptext_face)SENTINEL;
    APTEXT_ERROR error;
    APRESULT result = aptext_face_create_stroke(&desc, &face, &error);
    if (!BUILT(result))
        return;
    CHECK(result == APRESULT_INVALID_ARGUMENT);
    CHECK(face == NULL);
    track_face(result == APRESULT_OK ? face : NULL);
}

static void test_hershey_face(void)
{
    APTEXT_HERSHEY_FACE_DESC desc = hershey_desc();
    aptext_face face = NULL;
    APTEXT_ERROR error;
    APRESULT result = aptext_face_create_hershey(&desc, &face, &error);
    if (!BUILT(result))
        return;
    CHECK(result == APRESULT_OK);
    track_face(face);
    if (result != APRESULT_OK)
        return;

    uint32_t glyph_i = 0, glyph_h = 0;
    result = aptext_face_find_glyph(face, 0x49, &glyph_i);
    if (!BUILT(result))
        return;
    CHECK(result == APRESULT_OK);
    CHECK(aptext_face_find_glyph(face, 0x48, &glyph_h) == APRESULT_OK);

    const ApriVec2 *points = NULL;
    const APTEXT_STROKE_POLYLINE *polylines = NULL;
    uint32_t point_count = 0, polyline_count = 0;
    result = aptext_face_get_glyph_strokes(face, glyph_i, &points, &point_count, &polylines, &polyline_count);
    if (!BUILT(result))
        return;
    CHECK(point_count == 2);
    CHECK(polyline_count == 1);
    if (point_count == 2)
    {
        CHECK(points[0].x == 0.125f); // (0 - left -4) / 32
        CHECK(points[0].y == -0.25f); // file y -4, baseline 4: a quarter em up
        CHECK(points[1].y == 0.0f);
    }
    // The pen lifts between the H's two strokes.
    CHECK(aptext_face_get_glyph_strokes(face, glyph_h, &points, &point_count, &polylines, &polyline_count) ==
          APRESULT_OK);
    CHECK(polyline_count == 2);
    CHECK(point_count == 4);
    if (point_count == 4)
    {
        CHECK(points[0].x == 0.0625f);
        CHECK(points[3].x == 0.1875f);
    }

    aptext_chain chain = open_chain(&face, 1);
    if (!chain)
        return;
    APTEXT_LAYOUT_DESC layout_d = layout_desc(chain, "I I");
    aptext_layout layout = lay_out(&layout_d);
    if (!layout)
        return;
    LaidOut out;
    if (!read_layout(layout, &out))
        return;
    CHECK(out.glyph_count == 3);
    if (out.glyph_count == 3)
    {
        CHECK(out.glyphs[1].x == 0.25f); // the space, 8 units
        CHECK(out.glyphs[2].x == 0.5f);
    }
    APTEXT_SEGMENT segments[2];
    uint32_t count = 0;
    result = aptext_layout_get_strokes(layout, segments, 2, &count);
    if (!BUILT(result))
        return;
    CHECK(result == APRESULT_OK);
    CHECK(count == 2);
    if (count == 2)
    {
        CHECK(segments[0].x0 == 0.125f);
        CHECK(segments[0].y0 == 0.5f);
        CHECK(segments[0].y1 == 0.75f);
        CHECK(segments[1].x0 == 0.625f);
    }

    // Not a Hershey font, and a codepoint list that doesn't match.
    APTEXT_HERSHEY_FACE_DESC bad = desc;
    bad.data = "hello, world";
    bad.data_size = 12;
    bad.codepoints = NULL;
    bad.codepoint_count = 0;
    bad.first_codepoint = 0x20;
    aptext_face other = (aptext_face)SENTINEL;
    result = aptext_face_create_hershey(&bad, &other, &error);
    CHECK(result == APRESULT_APTEXT_NOT_A_FONT);
    CHECK(other == NULL);
    track_face(result == APRESULT_OK ? other : NULL);
    bad = desc;
    bad.codepoint_count = 2;
    other = (aptext_face)SENTINEL;
    result = aptext_face_create_hershey(&bad, &other, &error);
    CHECK(result == APRESULT_INVALID_ARGUMENT);
    track_face(result == APRESULT_OK ? other : NULL);
}

// ---- Distance fields ------------------------------------------------------------------

static APTEXT_GLYPH_GENERATE_DESC generate_desc(aptext_face face, uint32_t glyph, float pixels_per_em,
                                                float distance_range)
{
    APTEXT_GLYPH_GENERATE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APTEXT_GLYPH_GENERATE_DESC;
    desc.face = face;
    desc.glyph = glyph;
    desc.pixels_per_em = pixels_per_em;
    desc.distance_range = distance_range;
    return desc;
}

static APTEXT_GLYPH_IMAGE empty_image(void)
{
    APTEXT_GLYPH_IMAGE image;
    memset(&image, 0, sizeof(image));
    image.base.type = APRI_STRUCT_TYPE_APTEXT_GLYPH_IMAGE;
    return image;
}

// Generates a glyph into tracked memory, or returns NULL.
static uint8_t *generate(const APTEXT_GLYPH_GENERATE_DESC *desc, APTEXT_GLYPH_IMAGE *image, uint64_t *out_size)
{
    *image = empty_image();
    APTEXT_ERROR error;
    APRESULT result = aptext_generate_glyph(desc, NULL, 0, image, &error);
    if (!BUILT(result))
        return NULL;
    CHECK(result == APRESULT_OK);
    if (result != APRESULT_OK)
        return NULL;
    uint64_t size = (uint64_t)image->width * image->height * 4;
    uint8_t *pixels = (uint8_t *)track_memory((size_t)size);
    if (!pixels)
        return NULL;
    APTEXT_GLYPH_IMAGE again = empty_image();
    result = aptext_generate_glyph(desc, size ? pixels : NULL, size, &again, &error);
    if (!BUILT(result))
        return NULL;
    CHECK(result == APRESULT_OK);
    CHECK(again.width == image->width);
    CHECK(again.height == image->height);
    CHECK(again.plane_left == image->plane_left);
    CHECK(again.plane_bottom == image->plane_bottom);
    *out_size = size;
    return result == APRESULT_OK ? pixels : NULL;
}

// A channel of [pixels] sampled bilinearly at the point (ex, ey), in em
// units from the glyph's origin with y down, through the image's plane
// bounds; 0 to 255.
static double sample(const uint8_t *pixels, const APTEXT_GLYPH_IMAGE *image, int channel, double ex, double ey)
{
    double u = (ex - image->plane_left) / (image->plane_right - image->plane_left) * image->width - 0.5;
    double v = (ey - image->plane_top) / (image->plane_bottom - image->plane_top) * image->height - 0.5;
    int x0 = (int)floor(u), y0 = (int)floor(v);
    double fx = u - x0, fy = v - y0;
    double value = 0.0;
    for (int dy = 0; dy < 2; dy++)
        for (int dx = 0; dx < 2; dx++)
        {
            int x = x0 + dx, y = y0 + dy;
            if (x < 0)
                x = 0;
            if (y < 0)
                y = 0;
            if (x >= (int)image->width)
                x = (int)image->width - 1;
            if (y >= (int)image->height)
                y = (int)image->height - 1;
            double weight = (dx ? fx : 1.0 - fx) * (dy ? fy : 1.0 - fy);
            value += weight * pixels[((size_t)y * image->width + x) * 4 + channel];
        }
    return value;
}

static double median_at(const uint8_t *pixels, const APTEXT_GLYPH_IMAGE *image, double ex, double ey)
{
    double r = sample(pixels, image, 0, ex, ey);
    double g = sample(pixels, image, 1, ex, ey);
    double b = sample(pixels, image, 2, ex, ey);
    double lo = r < g ? r : g, hi = r < g ? g : r;
    return b < lo ? lo : (b > hi ? hi : b);
}

static void test_distance_field(void)
{
    aptext_face a = open_face(&font_a, 0);
    if (!a)
        return;
    // Glyph 'a' is the square [0.0625, 0.4375] by [0, 0.375] em, y up: at
    // 64 pixels per em, 24 pixels a side; the range is 4 pixels.
    APTEXT_GLYPH_GENERATE_DESC desc = generate_desc(a, A_LOWER_A, 64.0f, 4.0f);
    APTEXT_GLYPH_IMAGE image;
    uint64_t size = 0;
    uint8_t *pixels = generate(&desc, &image, &size);
    if (!pixels)
        return;
    CHECK(image.base.type == APRI_STRUCT_TYPE_APTEXT_GLYPH_IMAGE);
    CHECK(image.base.next == NULL);
    const double margin = 2.0 / 64.0; // half the range, in em
    CHECK(image.plane_left <= 0.0625 - margin);
    CHECK(image.plane_right >= 0.4375 + margin);
    CHECK(image.plane_top <= -0.375 - margin);
    CHECK(image.plane_bottom >= 0.0 + margin);
    CHECK(fabs((image.plane_right - image.plane_left) * 64.0 - image.width) < 1e-3);
    CHECK(fabs((image.plane_bottom - image.plane_top) * 64.0 - image.height) < 1e-3);

    // Alpha is the true signed distance: 0.5 on the outline, a quarter
    // more per pixel inside, clamped.
    const double mid_y = -0.1875;
    CHECK(sample(pixels, &image, 3, 0.25, mid_y) >= 250.0);
    CHECK(fabs(sample(pixels, &image, 3, 0.0625, mid_y) - 127.5) <= 20.0);
    CHECK(fabs(sample(pixels, &image, 3, 0.0625 + 1.0 / 64.0, mid_y) - 191.25) <= 20.0);
    CHECK(fabs(sample(pixels, &image, 3, 0.0625 - 1.0 / 64.0, mid_y) - 63.75) <= 20.0);
    CHECK(fabs(sample(pixels, &image, 3, 0.25, -0.375) - 127.5) <= 20.0); // the top edge
    CHECK(pixels[3] <= 8);                                               // the corner pixel, outside
    // The median of RGB agrees near the edge and inside.
    CHECK(median_at(pixels, &image, 0.25, mid_y) >= 250.0);
    CHECK(fabs(median_at(pixels, &image, 0.0625, mid_y) - 127.5) <= 24.0);
    CHECK(median_at(pixels, &image, image.plane_left + 0.5 / 64.0, image.plane_top + 0.5 / 64.0) <= 8.0);

    // The same glyph again gives the same bytes.
    APTEXT_GLYPH_IMAGE image2;
    uint64_t size2 = 0;
    uint8_t *pixels2 = generate(&desc, &image2, &size2);
    if (!pixels2)
        return;
    CHECK(size2 == size);
    if (size2 == size)
        CHECK(memcmp(pixels, pixels2, (size_t)size) == 0);

    // Too little memory, a glyph past the last, and a glyph with no outline.
    APTEXT_GLYPH_IMAGE out = empty_image();
    APTEXT_ERROR error;
    CHECK(aptext_generate_glyph(&desc, pixels, size - 1, &out, &error) == APRESULT_INVALID_ARGUMENT);
    APTEXT_GLYPH_GENERATE_DESC past = generate_desc(a, 9, 64.0f, 4.0f);
    out = empty_image();
    CHECK(aptext_generate_glyph(&past, NULL, 0, &out, &error) == APRESULT_NOT_FOUND);
    APTEXT_GLYPH_GENERATE_DESC space = generate_desc(a, A_SPACE, 64.0f, 4.0f);
    out = empty_image();
    CHECK(aptext_generate_glyph(&space, NULL, 0, &out, &error) == APRESULT_OK);
    CHECK(out.width == 0);
    CHECK(out.height == 0);
    CHECK(out.plane_left == 0.0f);
    CHECK(out.plane_right == 0.0f);

    // The argument checks that need a face.
    APTEXT_GLYPH_GENERATE_DESC bad = desc;
    bad.pixels_per_em = 0.0f;
    out = empty_image();
    CHECK(aptext_generate_glyph(&bad, NULL, 0, &out, &error) == APRESULT_INVALID_ARGUMENT);
    bad = desc;
    bad.distance_range = NAN;
    CHECK(aptext_generate_glyph(&bad, NULL, 0, &out, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(aptext_generate_glyph(&desc, NULL, 16, &out, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(aptext_generate_glyph(&desc, NULL, 0, NULL, &error) == APRESULT_INVALID_ARGUMENT);
    out.base.type = APRI_STRUCT_TYPE_APTEXT_ATLAS_GLYPH;
    CHECK(aptext_generate_glyph(&desc, NULL, 0, &out, &error) == APRESULT_STRUCT_WRONG_TYPE);
}

// ---- The atlas ------------------------------------------------------------------------

static aptext_atlas open_atlas(float pixels_per_em, float distance_range, uint32_t page, uint32_t max_pages)
{
    APTEXT_ATLAS_DESC desc = atlas_desc(pixels_per_em, distance_range, page, max_pages);
    aptext_atlas atlas = NULL;
    APTEXT_ERROR error;
    APRESULT result = aptext_atlas_create(&desc, &atlas, &error);
    if (!BUILT(result))
        return NULL;
    CHECK(result == APRESULT_OK);
    track_atlas(atlas);
    return atlas;
}

static bool read_atlas_info(aptext_atlas atlas, APTEXT_ATLAS_INFO *info)
{
    memset(info, 0, sizeof(*info));
    info->base.type = APRI_STRUCT_TYPE_APTEXT_ATLAS_INFO;
    APRESULT result = aptext_atlas_get_info(atlas, info);
    if (!BUILT(result))
        return false;
    CHECK(result == APRESULT_OK);
    return result == APRESULT_OK;
}

static APTEXT_ATLAS_GLYPH empty_atlas_glyph(void)
{
    APTEXT_ATLAS_GLYPH glyph;
    memset(&glyph, 0, sizeof(glyph));
    glyph.base.type = APRI_STRUCT_TYPE_APTEXT_ATLAS_GLYPH;
    return glyph;
}

// Generates and inserts one glyph; false if pending or failed.
static bool insert(aptext_atlas atlas, aptext_face face, uint32_t glyph, APTEXT_ATLAS_GLYPH *out_glyph,
                   const uint8_t **out_pixels, APTEXT_GLYPH_IMAGE *out_image)
{
    APTEXT_ATLAS_INFO info;
    if (!read_atlas_info(atlas, &info))
        return false;
    APTEXT_GLYPH_GENERATE_DESC desc = generate_desc(face, glyph, info.pixels_per_em, info.distance_range);
    uint64_t size = 0;
    uint8_t *pixels = generate(&desc, out_image, &size);
    if (!pixels)
        return false; // pending, or failed
    *out_glyph = empty_atlas_glyph();
    APTEXT_ERROR error;
    APRESULT result = aptext_atlas_insert(atlas, &desc, size ? pixels : NULL, size, out_glyph, &error);
    if (!BUILT(result))
        return false;
    CHECK(result == APRESULT_OK);
    if (result != APRESULT_OK)
        printf("  %s\n", error.message);
    if (out_pixels)
        *out_pixels = pixels;
    return result == APRESULT_OK;
}

static bool rects_overlap(const APTEXT_ATLAS_GLYPH *a, const APTEXT_ATLAS_GLYPH *b)
{
    if (a->page != b->page)
        return false;
    if (a->width == 0 || a->height == 0 || b->width == 0 || b->height == 0)
        return false;
    if (a->x + a->width <= b->x || b->x + b->width <= a->x)
        return false;
    return !(a->y + a->height <= b->y || b->y + b->height <= a->y);
}

static void test_atlas_packing(void)
{
    aptext_face a = NULL, b = NULL;
    aptext_chain chain = open_chain_ab(&a, &b);
    if (!chain)
        return;
    // Pages of 48 pixels hold a couple of glyphs each, so this spreads
    // over several.
    aptext_atlas atlas = open_atlas(32.0f, 4.0f, 48, 64);
    if (!atlas)
        return;
    APTEXT_ATLAS_INFO info;
    if (!read_atlas_info(atlas, &info))
        return;
    CHECK(info.page_count == 0);
    CHECK(info.glyph_count == 0);
    CHECK(info.pixels_per_em == 32.0f);
    CHECK(info.distance_range == 4.0f);
    CHECK(info.page_width == 48);
    CHECK(info.page_height == 48);

    enum { COUNT = 13 };
    aptext_face faces[COUNT];
    uint32_t ids[COUNT];
    APTEXT_ATLAS_GLYPH placed[COUNT];
    const uint8_t *images[COUNT];
    APTEXT_GLYPH_IMAGE sizes[COUNT];
    int n = 0;
    for (uint32_t g = 0; g < 9; g++)
    {
        faces[n] = a;
        ids[n++] = g;
    }
    for (uint32_t g = 0; g < 5; g++)
    {
        if (g == B_HAN_ONE)
            continue; // wider than a 48-pixel page at 32 pixels per em
        faces[n] = b;
        ids[n++] = g;
    }
    for (int i = 0; i < n; i++)
        if (!insert(atlas, faces[i], ids[i], &placed[i], &images[i], &sizes[i]))
            return;
    if (!read_atlas_info(atlas, &info))
        return;
    CHECK(info.glyph_count == (uint32_t)n);
    CHECK(info.page_count > 1);

    for (int i = 0; i < n; i++)
    {
        const APTEXT_ATLAS_GLYPH *p = &placed[i];
        CHECK(p->base.type == APRI_STRUCT_TYPE_APTEXT_ATLAS_GLYPH);
        CHECK(p->page < info.page_count);
        CHECK(p->x + p->width <= info.page_width);
        CHECK(p->y + p->height <= info.page_height);
        CHECK(p->width == sizes[i].width);
        CHECK(p->height == sizes[i].height);
        CHECK(p->plane_left == sizes[i].plane_left);
        CHECK(p->plane_top == sizes[i].plane_top);
        CHECK(p->plane_right == sizes[i].plane_right);
        CHECK(p->plane_bottom == sizes[i].plane_bottom);
        for (int j = 0; j < i; j++)
            CHECK(!rects_overlap(p, &placed[j]));
        // The page holds the image as generated.
        const void *page = NULL;
        CHECK(aptext_atlas_get_page(atlas, p->page, &page) == APRESULT_OK);
        if (page && images[i])
        {
            const uint8_t *row = (const uint8_t *)page;
            for (uint32_t y = 0; y < p->height; y++)
                CHECK(memcmp(row + ((size_t)(p->y + y) * info.page_width + p->x) * 4,
                             images[i] + (size_t)y * p->width * 4, (size_t)p->width * 4) == 0);
        }
        // Found where it was put.
        APTEXT_ATLAS_GLYPH found = empty_atlas_glyph();
        CHECK(aptext_atlas_find_glyph(atlas, faces[i], ids[i], &found) == APRESULT_OK);
        CHECK(found.page == p->page);
        CHECK(found.x == p->x);
        CHECK(found.y == p->y);
    }
    const void *page = NULL;
    CHECK(aptext_atlas_get_page(atlas, info.page_count, &page) == APRESULT_NOT_FOUND);
    APTEXT_ATLAS_GLYPH found = empty_atlas_glyph();
    CHECK(aptext_atlas_find_glyph(atlas, b, B_HAN_ONE, &found) == APRESULT_NOT_FOUND);

    // Inserting a glyph again leaves it where it is.
    APTEXT_ATLAS_GLYPH again;
    const uint8_t *image_again;
    APTEXT_GLYPH_IMAGE size_again;
    if (!insert(atlas, a, A_LOWER_B, &again, &image_again, &size_again))
        return;
    CHECK(again.page == placed[A_LOWER_B].page);
    CHECK(again.x == placed[A_LOWER_B].x);
    CHECK(again.y == placed[A_LOWER_B].y);
    APTEXT_ATLAS_INFO after;
    if (!read_atlas_info(atlas, &after))
        return;
    CHECK(after.glyph_count == info.glyph_count);
}

static void test_atlas_dirty(void)
{
    aptext_face a = open_face(&font_a, 0);
    if (!a)
        return;
    aptext_atlas atlas = open_atlas(32.0f, 4.0f, 48, 64);
    if (!atlas)
        return;
    APTEXT_ATLAS_GLYPH placed[4];
    APTEXT_GLYPH_IMAGE sizes[4];
    const uint32_t ids[4] = {A_LOWER_A, A_LOWER_B, A_LOWER_F, A_UPPER_V};
    for (int i = 0; i < 4; i++)
        if (!insert(atlas, a, ids[i], &placed[i], NULL, &sizes[i]))
            return;
    APTEXT_ATLAS_INFO info;
    if (!read_atlas_info(atlas, &info))
        return;
    CHECK(info.dirty_rect_count >= 1);
    CHECK(info.dirty_rect_count <= info.page_count);

    APTEXT_DIRTY_RECT rects[64];
    uint32_t count = 99;
    APRESULT result = aptext_atlas_take_dirty(atlas, NULL, 0, &count);
    if (!BUILT(result))
        return;
    CHECK(result == APRESULT_OK);
    CHECK(count == 0); // nothing taken
    // Take one, then the rest.
    CHECK(aptext_atlas_take_dirty(atlas, rects, 1, &count) == APRESULT_OK);
    CHECK(count == 1);
    uint32_t taken = count;
    CHECK(aptext_atlas_take_dirty(atlas, rects + 1, 63, &count) == APRESULT_OK);
    taken += count;
    CHECK(taken == info.dirty_rect_count);
    for (uint32_t i = 0; i < taken; i++)
    {
        CHECK(rects[i].page < info.page_count);
        CHECK(rects[i].width > 0);
        CHECK(rects[i].height > 0);
        CHECK(rects[i].x + rects[i].width <= info.page_width);
        CHECK(rects[i].y + rects[i].height <= info.page_height);
        if (i > 0)
            CHECK(rects[i].page > rects[i - 1].page);
    }
    // Every glyph lies inside its page's rectangle.
    for (int g = 0; g < 4; g++)
    {
        bool covered = false;
        for (uint32_t i = 0; i < taken; i++)
        {
            const APTEXT_DIRTY_RECT *r = &rects[i];
            if (r->page == placed[g].page && r->x <= placed[g].x && r->y <= placed[g].y &&
                placed[g].x + placed[g].width <= r->x + r->width && placed[g].y + placed[g].height <= r->y + r->height)
                covered = true;
        }
        CHECK(covered);
    }
    if (!read_atlas_info(atlas, &info))
        return;
    CHECK(info.dirty_rect_count == 0);
    CHECK(aptext_atlas_take_dirty(atlas, rects, 64, &count) == APRESULT_OK);
    CHECK(count == 0);

    // One more glyph: one rectangle, around it.
    APTEXT_ATLAS_GLYPH more;
    APTEXT_GLYPH_IMAGE more_size;
    if (!insert(atlas, a, A_UPPER_A, &more, NULL, &more_size))
        return;
    CHECK(aptext_atlas_take_dirty(atlas, rects, 64, &count) == APRESULT_OK);
    CHECK(count == 1);
    if (count == 1)
    {
        CHECK(rects[0].page == more.page);
        CHECK(rects[0].x <= more.x);
        CHECK(rects[0].y <= more.y);
        CHECK(more.x + more.width <= rects[0].x + rects[0].width);
        CHECK(more.y + more.height <= rects[0].y + rects[0].height);
    }
    CHECK(aptext_atlas_take_dirty(atlas, NULL, 4, &count) == APRESULT_INVALID_ARGUMENT);
    CHECK(aptext_atlas_take_dirty(atlas, rects, 4, NULL) == APRESULT_INVALID_ARGUMENT);
}

static void test_atlas_limits(void)
{
    aptext_face a = NULL, b = NULL;
    aptext_chain chain = open_chain_ab(&a, &b);
    if (!chain)
        return;
    // A glyph wider than a page: U+4E00 in font B is 1.25 em.
    aptext_atlas small = open_atlas(32.0f, 4.0f, 36, 8);
    if (!small)
        return;
    APTEXT_GLYPH_GENERATE_DESC wide = generate_desc(b, B_HAN_ONE, 32.0f, 4.0f);
    APTEXT_GLYPH_IMAGE image;
    uint64_t size = 0;
    uint8_t *pixels = generate(&wide, &image, &size);
    if (!pixels)
        return;
    APTEXT_ATLAS_GLYPH placed = empty_atlas_glyph();
    APTEXT_ERROR error;
    APRESULT result = aptext_atlas_insert(small, &wide, pixels, size, &placed, &error);
    if (!BUILT(result))
        return;
    CHECK(result == APRESULT_LIMIT);

    // One page only: full sooner or later.
    aptext_atlas one = open_atlas(32.0f, 4.0f, 48, 1);
    if (!one)
        return;
    bool limited = false;
    for (uint32_t g = 0; g < 9 && !limited; g++)
    {
        APTEXT_GLYPH_GENERATE_DESC desc = generate_desc(a, g, 32.0f, 4.0f);
        pixels = generate(&desc, &image, &size);
        if (!pixels)
            return;
        placed = empty_atlas_glyph();
        result = aptext_atlas_insert(one, &desc, size ? pixels : NULL, size, &placed, &error);
        CHECK(result == APRESULT_OK || result == APRESULT_LIMIT);
        limited = result == APRESULT_LIMIT;
    }
    CHECK(limited);
    APTEXT_ATLAS_INFO info;
    if (!read_atlas_info(one, &info))
        return;
    CHECK(info.page_count == 1);

    // Settings other than the atlas's, and pixels of the wrong size.
    APTEXT_GLYPH_GENERATE_DESC other = generate_desc(a, A_LOWER_A, 48.0f, 4.0f);
    pixels = generate(&other, &image, &size);
    if (!pixels)
        return;
    result = aptext_atlas_insert(small, &other, pixels, size, NULL, &error);
    CHECK(result == APRESULT_INVALID_ARGUMENT);
    APTEXT_GLYPH_GENERATE_DESC right = generate_desc(a, A_LOWER_A, 32.0f, 4.0f);
    pixels = generate(&right, &image, &size);
    if (!pixels)
        return;
    CHECK(aptext_atlas_insert(small, &right, pixels, size - 4, NULL, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(aptext_atlas_insert(small, &right, NULL, size, NULL, &error) == APRESULT_INVALID_ARGUMENT);
    APTEXT_ATLAS_GLYPH wrong = empty_atlas_glyph();
    wrong.base.type = APRI_STRUCT_TYPE_APTEXT_GLYPH_IMAGE;
    CHECK(aptext_atlas_insert(small, &right, pixels, size, &wrong, &error) == APRESULT_STRUCT_WRONG_TYPE);
    CHECK(aptext_atlas_insert(small, &right, pixels, size, NULL, &error) == APRESULT_OK);
}

static void test_atlas_add_layout(void)
{
    aptext_chain chain = open_chain_ab(NULL, NULL);
    if (!chain)
        return;
    APTEXT_LAYOUT_DESC desc = layout_desc(chain, "fib azQ");
    aptext_layout layout = lay_out(&desc);
    if (!layout)
        return;
    aptext_atlas atlas = open_atlas(32.0f, 4.0f, 128, 16);
    if (!atlas)
        return;
    APTEXT_ERROR error;
    APRESULT result = aptext_atlas_add_layout(atlas, layout, &error);
    if (!BUILT(result))
        return;
    CHECK(result == APRESULT_OK);
    LaidOut out;
    if (!read_layout(layout, &out))
        return;
    for (uint32_t i = 0; i < out.glyph_count; i++)
    {
        aptext_face face = aptext_chain_get_face(chain, out.glyphs[i].face_index);
        APTEXT_ATLAS_GLYPH found = empty_atlas_glyph();
        CHECK(aptext_atlas_find_glyph(atlas, face, out.glyphs[i].glyph, &found) == APRESULT_OK);
    }
    // Adding it again adds nothing.
    APTEXT_ATLAS_INFO before, after;
    if (!read_atlas_info(atlas, &before))
        return;
    CHECK(aptext_atlas_add_layout(atlas, layout, &error) == APRESULT_OK);
    if (!read_atlas_info(atlas, &after))
        return;
    CHECK(after.glyph_count == before.glyph_count);
}

static void test_atlas_save_load(void)
{
    aptext_face a = NULL, b = NULL;
    aptext_chain chain = open_chain_ab(&a, &b);
    if (!chain)
        return;
    APTEXT_LAYOUT_DESC layout_d = layout_desc(chain, "ab fi AV z");
    aptext_layout layout = lay_out(&layout_d);
    if (!layout)
        return;
    APTEXT_ATLAS_DESC settings = atlas_desc(32.0f, 4.0f, 64, 16);
    aptext_atlas atlas = NULL;
    APTEXT_ERROR error;
    APRESULT result = aptext_atlas_create(&settings, &atlas, &error);
    if (!BUILT(result))
        return;
    track_atlas(atlas);
    result = aptext_atlas_add_layout(atlas, layout, &error);
    if (!BUILT(result))
        return;
    CHECK(result == APRESULT_OK);

    aptext_face faces[2] = {a, b};
    uint64_t size = 0;
    result = aptext_atlas_save(atlas, faces, 2, NULL, 0, &size, &error);
    if (!BUILT(result))
        return;
    CHECK(result == APRESULT_OK);
    CHECK(size > 0);
    uint8_t *saved = (uint8_t *)track_memory((size_t)size);
    uint8_t *saved_again = (uint8_t *)track_memory((size_t)size);
    if (!saved || !saved_again)
        return;
    uint64_t written = 0;
    CHECK(aptext_atlas_save(atlas, faces, 2, saved, size - 1, &written, &error) == APRESULT_LIMIT);
    CHECK(aptext_atlas_save(atlas, faces, 2, saved, size, &written, &error) == APRESULT_OK);
    CHECK(written == size);
    CHECK(aptext_atlas_save(atlas, faces, 2, saved_again, size, &written, &error) == APRESULT_OK);
    CHECK(memcmp(saved, saved_again, (size_t)size) == 0);
    // A face the atlas holds glyphs of must be listed.
    CHECK(aptext_atlas_save(atlas, faces, 1, saved_again, size, &written, &error) == APRESULT_INVALID_ARGUMENT);

    APTEXT_ATLAS_LOAD_DESC load;
    memset(&load, 0, sizeof(load));
    load.base.type = APRI_STRUCT_TYPE_APTEXT_ATLAS_LOAD_DESC;
    load.atlas = &settings;
    load.data = saved;
    load.data_size = size;
    load.faces = faces;
    load.face_count = 2;
    aptext_atlas loaded = NULL;
    result = aptext_atlas_load(&load, &loaded, &error);
    if (!BUILT(result))
        return;
    CHECK(result == APRESULT_OK);
    track_atlas(loaded);
    if (result != APRESULT_OK)
        return;

    APTEXT_ATLAS_INFO original, copy;
    if (!read_atlas_info(atlas, &original))
        return;
    if (!read_atlas_info(loaded, &copy))
        return;
    CHECK(copy.page_count == original.page_count);
    CHECK(copy.glyph_count == original.glyph_count);
    CHECK(copy.dirty_rect_count == copy.page_count); // every page, in full
    LaidOut out;
    if (!read_layout(layout, &out))
        return;
    for (uint32_t i = 0; i < out.glyph_count; i++)
    {
        aptext_face face = aptext_chain_get_face(chain, out.glyphs[i].face_index);
        APTEXT_ATLAS_GLYPH x = empty_atlas_glyph(), y = empty_atlas_glyph();
        CHECK(aptext_atlas_find_glyph(atlas, face, out.glyphs[i].glyph, &x) == APRESULT_OK);
        CHECK(aptext_atlas_find_glyph(loaded, face, out.glyphs[i].glyph, &y) == APRESULT_OK);
        CHECK(x.page == y.page);
        CHECK(x.x == y.x);
        CHECK(x.y == y.y);
        CHECK(x.width == y.width);
        CHECK(x.plane_top == y.plane_top);
    }
    for (uint32_t p = 0; p < copy.page_count; p++)
    {
        const void *x = NULL, *y = NULL;
        CHECK(aptext_atlas_get_page(atlas, p, &x) == APRESULT_OK);
        CHECK(aptext_atlas_get_page(loaded, p, &y) == APRESULT_OK);
        if (x && y)
            CHECK(memcmp(x, y, (size_t)copy.page_width * copy.page_height * 4) == 0);
    }
    APTEXT_DIRTY_RECT rects[16];
    uint32_t count = 0;
    CHECK(aptext_atlas_take_dirty(loaded, rects, 16, &count) == APRESULT_OK);
    for (uint32_t i = 0; i < count; i++)
    {
        CHECK(rects[i].x == 0);
        CHECK(rects[i].y == 0);
        CHECK(rects[i].width == copy.page_width);
        CHECK(rects[i].height == copy.page_height);
    }

    // Other settings, other faces, fewer faces: a mismatch.
    aptext_atlas refused = (aptext_atlas)SENTINEL;
    APTEXT_ATLAS_DESC other_settings = atlas_desc(48.0f, 4.0f, 64, 16);
    APTEXT_ATLAS_LOAD_DESC bad = load;
    bad.atlas = &other_settings;
    CHECK(aptext_atlas_load(&bad, &refused, &error) == APRESULT_APTEXT_ATLAS_MISMATCH);
    CHECK(refused == NULL);
    aptext_face swapped[2] = {b, a};
    bad = load;
    bad.faces = swapped;
    CHECK(aptext_atlas_load(&bad, &refused, &error) == APRESULT_APTEXT_ATLAS_MISMATCH);
    bad = load;
    bad.face_count = 1;
    CHECK(aptext_atlas_load(&bad, &refused, &error) == APRESULT_APTEXT_ATLAS_MISMATCH);

    // Damaged bytes: corrupt.
    memcpy(saved_again, saved, (size_t)size);
    saved_again[size / 2] ^= 0x5A;
    bad = load;
    bad.data = saved_again;
    CHECK(aptext_atlas_load(&bad, &refused, &error) == APRESULT_CORRUPT_DATA);
    CHECK(refused == NULL);
    bad = load;
    bad.data_size = size - 1;
    refused = (aptext_atlas)SENTINEL;
    CHECK(aptext_atlas_load(&bad, &refused, &error) == APRESULT_CORRUPT_DATA);
    CHECK(refused == NULL);
}

// ---- Running ------------------------------------------------------------------------

static int tests_run = 0;
static int tests_passed = 0;
static int tests_pending = 0;
static int tests_failed = 0;

static void run(const char *name, void (*test)(void))
{
    current_test = name;
    test_pending = false;
    int before = failures;
    test();
    destroy_tracked();
    tests_run++;
    if (failures != before)
        tests_failed++;
    else if (test_pending)
        tests_pending++;
    else
        tests_passed++;
}

#define RUN(test) run(#test, test)

int main(int argc, char **argv)
{
    build_fonts();
    if (argc == 3 && strcmp(argv[1], "--write-fonts") == 0)
    {
        bool ok = write_file(argv[2], "aptext_test_a.ttf", &font_a);
        ok = write_file(argv[2], "aptext_test_b.ttf", &font_b) && ok;
        ok = write_file(argv[2], "aptext_test_ab.ttc", &font_ab_collection) && ok;
        printf(ok ? "aptext_tests: fonts written to %s\n" : "aptext_tests: couldn't write fonts to %s\n", argv[2]);
        return ok ? 0 : 1;
    }

    // Argument checks.
    RUN(test_face_create_arguments);
    RUN(test_stroke_face_arguments);
    RUN(test_hershey_face_arguments);
    RUN(test_face_getter_arguments);
    RUN(test_chain_arguments);
    RUN(test_layout_arguments);
    RUN(test_generate_arguments);
    RUN(test_atlas_arguments);
    RUN(test_atlas_load_arguments);

    // Faces and fallback.
    RUN(test_face_metrics);
    RUN(test_face_find_glyph);
    RUN(test_face_not_a_font);
    RUN(test_face_collection);
    RUN(test_face_copies_data);
    RUN(test_chain_lookup);
    RUN(test_chain_refusals);

    // Shaping and layout.
    RUN(test_layout_simple);
    RUN(test_layout_em_size);
    RUN(test_layout_kerning);
    RUN(test_layout_ligature);
    RUN(test_layout_fallback);
    RUN(test_layout_notdef);
    RUN(test_layout_invalid_utf8);
    RUN(test_layout_default_ignorable);
    RUN(test_layout_wrapping);
    RUN(test_layout_newlines);
    RUN(test_layout_empty);
    RUN(test_layout_alignment);
    RUN(test_layout_spacing);
    RUN(test_layout_scripts_and_bidi);
    RUN(test_layout_deterministic);
    RUN(test_layout_rebuild_and_failure);

    // Single-stroke faces.
    RUN(test_stroke_face);
    RUN(test_stroke_face_duplicates);
    RUN(test_hershey_face);

    // The atlas.
    RUN(test_distance_field);
    RUN(test_atlas_packing);
    RUN(test_atlas_dirty);
    RUN(test_atlas_limits);
    RUN(test_atlas_add_layout);
    RUN(test_atlas_save_load);

    printf("aptext_tests: %d tests, %d passed, %d pending (not implemented yet), %d failed\n", tests_run,
           tests_passed, tests_pending, tests_failed);
    if (failures)
    {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
