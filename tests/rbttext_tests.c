// Headless tests for Rabbittus Text (include/text/rbttext.h): the contract, as the
// implementation will have to meet it.
//
// Written in C on purpose: rbttext.h is a C front end, and this proves it
// compiles and links as one.
//
// Written before the implementation (RBTTEXT_TODO.md, the contract pass), and
// not to be edited by it. Two kinds of test:
//   - Argument checks, which every call makes before anything else and
//     which pass today.
//   - Behaviour: shaping, fallback, .notdef, wrapping, ligatures, kerning,
//     bidirectional runs, single-stroke faces, distance fields, packing,
//     dirty rectangles, saving and loading. Each is written against the
//     real behaviour. A call that returns RBT_RESULT_NOT_IMPLEMENTED marks
//     its test pending and ends it; anything else is checked. So the
//     implementation turns them on call by call, without touching this file.
//
// The fonts are built here, byte by byte (build_font()): two small TrueType
// fonts and a collection of both, with glyph ids, advances, outlines, a
// kerning pair and a ligature chosen so that every expected position is an
// exact binary fraction, and compared with ==. Run with --write-fonts <dir>
// to write them out for a font tool to check.

#include "text/rbttext.h"
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
// RBT_RESULT_NOT_IMPLEMENTED.
static void mark_pending(int line)
{
    if (!test_pending)
        printf("PENDING %s (%s:%d): not implemented yet\n", current_test, __FILE__, line);
    test_pending = true;
}

// True if [result] is from a built call; marks the test pending if not.
static bool built(RBT_RESULT result, int line)
{
    if (result == RBT_RESULT_NOT_IMPLEMENTED)
    {
        mark_pending(line);
        return false;
    }
    return true;
}

#define BUILT(result) built((result), __LINE__)

// ---- What a test made, destroyed when it ends ---------------------------------------

#define MAX_TRACKED 64

static rbttext_face tracked_faces[MAX_TRACKED];
static int tracked_face_count = 0;
static rbttext_chain tracked_chains[MAX_TRACKED];
static int tracked_chain_count = 0;
static rbttext_layout tracked_layouts[MAX_TRACKED];
static int tracked_layout_count = 0;
static rbttext_atlas tracked_atlases[MAX_TRACKED];
static int tracked_atlas_count = 0;
static void *tracked_memory[MAX_TRACKED];
static int tracked_memory_count = 0;

static void track_face(rbttext_face face)
{
    if (face && tracked_face_count < MAX_TRACKED)
        tracked_faces[tracked_face_count++] = face;
}

static void track_chain(rbttext_chain chain)
{
    if (chain && tracked_chain_count < MAX_TRACKED)
        tracked_chains[tracked_chain_count++] = chain;
}

static void track_layout(rbttext_layout layout)
{
    if (layout && tracked_layout_count < MAX_TRACKED)
        tracked_layouts[tracked_layout_count++] = layout;
}

static void track_atlas(rbttext_atlas atlas)
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
        rbttext_layout_destroy(tracked_layouts[i]);
    for (int i = 0; i < tracked_atlas_count; i++)
        rbttext_atlas_destroy(tracked_atlases[i]);
    for (int i = 0; i < tracked_chain_count; i++)
        rbttext_chain_destroy(tracked_chains[i]);
    for (int i = 0; i < tracked_face_count; i++)
        rbttext_face_destroy(tracked_faces[i]);
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
    const uint32_t at = b->size++;
    if (at < sizeof(b->data))
        b->data[at] = (uint8_t)v;
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
static rbttext_face open_face(const Bytes *bytes, uint32_t index)
{
    RBTTEXT_FACE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_TEXT_FACE_DESC;
    desc.data = bytes->data;
    desc.data_size = bytes->size;
    desc.collection_index = index;
    rbttext_face face = NULL;
    RBTTEXT_ERROR error;
    RBT_RESULT result = rbttext_face_create(&desc, &face, &error);
    if (!BUILT(result))
        return NULL;
    CHECK(result == RBT_RESULT_OK);
    if (result != RBT_RESULT_OK)
        printf("  %s\n", error.message);
    CHECK(error.message[0] == '\0' || result != RBT_RESULT_OK);
    track_face(face);
    return face;
}

// A chain of [count] faces, or NULL.
static rbttext_chain open_chain(const rbttext_face *faces, uint32_t count)
{
    RBTTEXT_CHAIN_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_TEXT_CHAIN_DESC;
    desc.faces = faces;
    desc.face_count = count;
    rbttext_chain chain = NULL;
    RBTTEXT_ERROR error;
    RBT_RESULT result = rbttext_chain_create(&desc, &chain, &error);
    if (!BUILT(result))
        return NULL;
    CHECK(result == RBT_RESULT_OK);
    if (result != RBT_RESULT_OK)
        printf("  %s\n", error.message);
    track_chain(chain);
    return chain;
}

// Font A then font B, or NULL.
static rbttext_chain open_chain_ab(rbttext_face *out_a, rbttext_face *out_b)
{
    rbttext_face faces[2];
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
static RBTTEXT_LAYOUT_DESC layout_desc(rbttext_chain chain, const char *text)
{
    RBTTEXT_LAYOUT_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_TEXT_LAYOUT_DESC;
    desc.chain = chain;
    desc.text = text;
    desc.text_size = (uint32_t)strlen(text);
    desc.language = NULL;
    desc.direction = RBTTEXT_DIRECTION_LTR;
    desc.features = RBTTEXT_FEATURE_KERNING | RBTTEXT_FEATURE_LIGATURES;
    desc.em_size = 1.0f;
    desc.max_width = 0.0f;
    desc.align = RBTTEXT_ALIGN_LEFT;
    desc.line_spacing = 1.0f;
    desc.letter_spacing = 0.0f;
    return desc;
}

// A layout of [desc], or NULL.
static rbttext_layout lay_out(const RBTTEXT_LAYOUT_DESC *desc)
{
    rbttext_layout layout = NULL;
    RBT_RESULT result = rbttext_layout_create(&layout);
    if (!BUILT(result))
        return NULL;
    CHECK(result == RBT_RESULT_OK);
    if (result != RBT_RESULT_OK)
        return NULL;
    track_layout(layout);
    RBTTEXT_ERROR error;
    result = rbttext_layout_build(layout, desc, &error);
    if (!BUILT(result))
        return NULL;
    CHECK(result == RBT_RESULT_OK);
    if (result != RBT_RESULT_OK)
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
    RBTTEXT_LAYOUT_INFO info;
    const RBTTEXT_GLYPH *glyphs;
    uint32_t glyph_count;
    const RBTTEXT_LINE *lines;
    uint32_t line_count;
    const RBTTEXT_RUN *runs;
    uint32_t run_count;
} LaidOut;

static bool read_layout(rbttext_layout layout, LaidOut *out)
{
    memset(out, 0, sizeof(*out));
    out->info.base.type = RBT_STRUCT_TYPE_TEXT_LAYOUT_INFO;
    RBT_RESULT result = rbttext_layout_get_info(layout, &out->info);
    if (!BUILT(result))
        return false;
    CHECK(result == RBT_RESULT_OK);
    CHECK(out->info.base.type == RBT_STRUCT_TYPE_TEXT_LAYOUT_INFO);
    CHECK(out->info.base.next == NULL);
    result = rbttext_layout_get_glyphs(layout, &out->glyphs, &out->glyph_count);
    if (!BUILT(result))
        return false;
    CHECK(result == RBT_RESULT_OK);
    result = rbttext_layout_get_lines(layout, &out->lines, &out->line_count);
    if (!BUILT(result))
        return false;
    CHECK(result == RBT_RESULT_OK);
    result = rbttext_layout_get_runs(layout, &out->runs, &out->run_count);
    if (!BUILT(result))
        return false;
    CHECK(result == RBT_RESULT_OK);
    CHECK(out->glyph_count == out->info.glyph_count);
    CHECK(out->line_count == out->info.line_count);
    CHECK(out->run_count == out->info.run_count);
    return true;
}

// Lays out [desc] and reads it, or false.
static bool lay_out_and_read(const RBTTEXT_LAYOUT_DESC *desc, LaidOut *out)
{
    rbttext_layout layout = lay_out(desc);
    if (!layout)
        return false;
    return read_layout(layout, out);
}

// ---- Argument checks: they pass today --------------------------------------------------

static void test_face_create_arguments(void)
{
    static const uint8_t bytes[16] = {0};
    RBTTEXT_FACE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_TEXT_FACE_DESC;
    desc.data = bytes;
    desc.data_size = sizeof(bytes);
    rbttext_face face;
    RBTTEXT_ERROR error;

    CHECK(rbttext_face_create(&desc, NULL, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(error.message[0] != '\0');

    face = (rbttext_face)SENTINEL;
    CHECK(rbttext_face_create(NULL, &face, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(face == NULL);
    CHECK(error.message[0] != '\0');
    CHECK(rbttext_face_create(NULL, &face, NULL) == RBT_RESULT_INVALID_ARGUMENT);

    RBTTEXT_FACE_DESC bad = desc;
    bad.base.type = RBT_STRUCT_TYPE_TEXT_CHAIN_DESC;
    face = (rbttext_face)SENTINEL;
    CHECK(rbttext_face_create(&bad, &face, &error) == RBT_RESULT_STRUCT_WRONG_TYPE);
    CHECK(face == NULL);
    bad.base.type = RBT_STRUCT_TYPE_UNKNOWN;
    CHECK(rbttext_face_create(&bad, &face, &error) == RBT_RESULT_STRUCT_WRONG_TYPE);

    RBT_STRUCT_BASE chained = {RBT_STRUCT_TYPE_TEXT_FACE_DESC, NULL};
    bad = desc;
    bad.base.next = &chained;
    CHECK(rbttext_face_create(&bad, &face, &error) == RBT_RESULT_STRUCT_UNKNOWN_IN_CHAIN);
    CHECK(error.message[0] != '\0');

    bad = desc;
    bad.data = NULL;
    CHECK(rbttext_face_create(&bad, &face, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(error.message[0] != '\0');

    bad = desc;
    bad.data_size = 0;
    CHECK(rbttext_face_create(&bad, &face, &error) == RBT_RESULT_INVALID_ARGUMENT);

    bad = desc;
    bad.data_size = 0x100000000ull;
    CHECK(rbttext_face_create(&bad, &face, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(face == NULL);
}

// A stroke face desc with two glyphs, 'L' and a space, that passes every
// check; the argument tests break one field at a time.
static const RbtVec2 stroke_points[3] = {{0.125f, 0.75f}, {0.125f, 0.0f}, {0.5f, 0.0f}};
static const RBTTEXT_STROKE_POLYLINE stroke_polylines[1] = {{0, 3}};
static const RBTTEXT_STROKE_GLYPH stroke_glyphs[2] = {{0x4C, 0.625f, 0, 1}, {0x20, 0.25f, 1, 0}};

static RBTTEXT_STROKE_FACE_DESC stroke_desc(void)
{
    RBTTEXT_STROKE_FACE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_TEXT_STROKE_FACE_DESC;
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

static void expect_stroke_refused(const RBTTEXT_STROKE_FACE_DESC *desc, int line)
{
    rbttext_face face = (rbttext_face)SENTINEL;
    RBTTEXT_ERROR error;
    error.message[0] = '\0';
    RBT_RESULT result = rbttext_face_create_stroke(desc, &face, &error);
    if (result != RBT_RESULT_INVALID_ARGUMENT)
        printf("FAIL %s (%s:%d): expected RBT_RESULT_INVALID_ARGUMENT, got %d\n", current_test, __FILE__, line,
               (int)result);
    if (result != RBT_RESULT_INVALID_ARGUMENT)
        failures++;
    CHECK(face == NULL);
    CHECK(error.message[0] != '\0');
}

static void test_stroke_face_arguments(void)
{
    RBTTEXT_STROKE_FACE_DESC desc = stroke_desc();
    RBTTEXT_STROKE_FACE_DESC bad;
    rbttext_face face;
    RBTTEXT_ERROR error;

    CHECK(rbttext_face_create_stroke(&desc, NULL, &error) == RBT_RESULT_INVALID_ARGUMENT);
    face = (rbttext_face)SENTINEL;
    CHECK(rbttext_face_create_stroke(NULL, &face, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(face == NULL);

    bad = desc;
    bad.base.type = RBT_STRUCT_TYPE_TEXT_FACE_DESC;
    CHECK(rbttext_face_create_stroke(&bad, &face, &error) == RBT_RESULT_STRUCT_WRONG_TYPE);
    RBT_STRUCT_BASE chained = {RBT_STRUCT_TYPE_TEXT_STROKE_FACE_DESC, NULL};
    bad = desc;
    bad.base.next = &chained;
    CHECK(rbttext_face_create_stroke(&bad, &face, &error) == RBT_RESULT_STRUCT_UNKNOWN_IN_CHAIN);

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

    RBTTEXT_STROKE_GLYPH glyphs[2];
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

    RBTTEXT_STROKE_POLYLINE polylines[1] = {{0, 1}};
    bad = desc;
    bad.polylines = polylines;
    expect_stroke_refused(&bad, __LINE__);
    polylines[0].point_count = 4;
    expect_stroke_refused(&bad, __LINE__);
    polylines[0].first_point = 2;
    polylines[0].point_count = 2;
    expect_stroke_refused(&bad, __LINE__);

    RbtVec2 points[3];
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

static RBTTEXT_HERSHEY_FACE_DESC hershey_desc(void)
{
    RBTTEXT_HERSHEY_FACE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_TEXT_HERSHEY_FACE_DESC;
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

static void expect_hershey_refused(const RBTTEXT_HERSHEY_FACE_DESC *desc, int line)
{
    rbttext_face face = (rbttext_face)SENTINEL;
    RBTTEXT_ERROR error;
    error.message[0] = '\0';
    RBT_RESULT result = rbttext_face_create_hershey(desc, &face, &error);
    if (result != RBT_RESULT_INVALID_ARGUMENT)
        printf("FAIL %s (%s:%d): expected RBT_RESULT_INVALID_ARGUMENT, got %d\n", current_test, __FILE__, line,
               (int)result);
    if (result != RBT_RESULT_INVALID_ARGUMENT)
        failures++;
    CHECK(face == NULL);
    CHECK(error.message[0] != '\0');
}

static void test_hershey_face_arguments(void)
{
    RBTTEXT_HERSHEY_FACE_DESC desc = hershey_desc();
    RBTTEXT_HERSHEY_FACE_DESC bad;
    rbttext_face face;
    RBTTEXT_ERROR error;

    CHECK(rbttext_face_create_hershey(&desc, NULL, &error) == RBT_RESULT_INVALID_ARGUMENT);
    face = (rbttext_face)SENTINEL;
    CHECK(rbttext_face_create_hershey(NULL, &face, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(face == NULL);
    bad = desc;
    bad.base.type = RBT_STRUCT_TYPE_TEXT_STROKE_FACE_DESC;
    CHECK(rbttext_face_create_hershey(&bad, &face, &error) == RBT_RESULT_STRUCT_WRONG_TYPE);
    RBT_STRUCT_BASE chained = {RBT_STRUCT_TYPE_TEXT_HERSHEY_FACE_DESC, NULL};
    bad = desc;
    bad.base.next = &chained;
    CHECK(rbttext_face_create_hershey(&bad, &face, &error) == RBT_RESULT_STRUCT_UNKNOWN_IN_CHAIN);

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
    RBTTEXT_FACE_METRICS metrics;
    memset(&metrics, 0, sizeof(metrics));
    metrics.base.type = RBT_STRUCT_TYPE_TEXT_FACE_METRICS;
    CHECK(rbttext_face_get_metrics(NULL, &metrics) == RBT_RESULT_INVALID_ARGUMENT);
    uint32_t glyph = 7;
    CHECK(rbttext_face_find_glyph(NULL, 0x61, &glyph) == RBT_RESULT_INVALID_ARGUMENT);
    const RbtVec2 *points;
    const RBTTEXT_STROKE_POLYLINE *polylines;
    uint32_t point_count, polyline_count;
    CHECK(rbttext_face_get_glyph_strokes(NULL, 0, &points, &point_count, &polylines, &polyline_count) ==
          RBT_RESULT_INVALID_ARGUMENT);
    rbttext_face_destroy(NULL);
}

static void test_chain_arguments(void)
{
    rbttext_face none[1] = {NULL};
    RBTTEXT_CHAIN_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_TEXT_CHAIN_DESC;
    desc.faces = none;
    desc.face_count = 1;
    rbttext_chain chain;
    RBTTEXT_ERROR error;

    CHECK(rbttext_chain_create(&desc, NULL, &error) == RBT_RESULT_INVALID_ARGUMENT);
    chain = (rbttext_chain)SENTINEL;
    CHECK(rbttext_chain_create(NULL, &chain, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(chain == NULL);
    RBTTEXT_CHAIN_DESC bad = desc;
    bad.base.type = RBT_STRUCT_TYPE_TEXT_LAYOUT_DESC;
    CHECK(rbttext_chain_create(&bad, &chain, &error) == RBT_RESULT_STRUCT_WRONG_TYPE);
    RBT_STRUCT_BASE chained = {RBT_STRUCT_TYPE_TEXT_CHAIN_DESC, NULL};
    bad = desc;
    bad.base.next = &chained;
    CHECK(rbttext_chain_create(&bad, &chain, &error) == RBT_RESULT_STRUCT_UNKNOWN_IN_CHAIN);
    bad = desc;
    bad.faces = NULL;
    CHECK(rbttext_chain_create(&bad, &chain, &error) == RBT_RESULT_INVALID_ARGUMENT);
    bad = desc;
    bad.face_count = 0;
    CHECK(rbttext_chain_create(&bad, &chain, &error) == RBT_RESULT_INVALID_ARGUMENT);
    error.message[0] = '\0';
    chain = (rbttext_chain)SENTINEL;
    CHECK(rbttext_chain_create(&desc, &chain, &error) == RBT_RESULT_INVALID_ARGUMENT); // faces[0] is NULL
    CHECK(chain == NULL);
    CHECK(error.message[0] != '\0');

    CHECK(rbttext_chain_get_face(NULL, 0) == NULL);
    CHECK(rbttext_chain_get_face_count(NULL) == 0);
    uint32_t face_index, glyph;
    CHECK(rbttext_chain_find_glyph(NULL, 0x61, &face_index, &glyph) == RBT_RESULT_INVALID_ARGUMENT);
    rbttext_chain_destroy(NULL);
}

static void test_layout_arguments(void)
{
    CHECK(rbttext_layout_create(NULL) == RBT_RESULT_INVALID_ARGUMENT);
    RBTTEXT_LAYOUT_DESC desc = layout_desc(NULL, "a");
    RBTTEXT_ERROR error;
    error.message[0] = '\0';
    CHECK(rbttext_layout_build(NULL, &desc, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(error.message[0] != '\0');
    RBTTEXT_LAYOUT_INFO info;
    memset(&info, 0, sizeof(info));
    info.base.type = RBT_STRUCT_TYPE_TEXT_LAYOUT_INFO;
    CHECK(rbttext_layout_get_info(NULL, &info) == RBT_RESULT_INVALID_ARGUMENT);
    const RBTTEXT_GLYPH *glyphs;
    const RBTTEXT_LINE *lines;
    const RBTTEXT_RUN *runs;
    uint32_t count;
    CHECK(rbttext_layout_get_glyphs(NULL, &glyphs, &count) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbttext_layout_get_lines(NULL, &lines, &count) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbttext_layout_get_runs(NULL, &runs, &count) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbttext_layout_get_strokes(NULL, NULL, 0, &count) == RBT_RESULT_INVALID_ARGUMENT);
    rbttext_layout_destroy(NULL);
}

static void test_generate_arguments(void)
{
    RBTTEXT_GLYPH_GENERATE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_TEXT_GLYPH_GENERATE_DESC;
    desc.face = NULL;
    desc.glyph = 0;
    desc.pixels_per_em = 32.0f;
    desc.distance_range = 4.0f;
    RBTTEXT_GLYPH_IMAGE image;
    memset(&image, 0, sizeof(image));
    image.base.type = RBT_STRUCT_TYPE_TEXT_GLYPH_IMAGE;
    RBTTEXT_ERROR error;

    CHECK(rbttext_generate_glyph(NULL, NULL, 0, &image, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(error.message[0] != '\0');
    RBTTEXT_GLYPH_GENERATE_DESC bad = desc;
    bad.base.type = RBT_STRUCT_TYPE_TEXT_ATLAS_DESC;
    CHECK(rbttext_generate_glyph(&bad, NULL, 0, &image, &error) == RBT_RESULT_STRUCT_WRONG_TYPE);
    RBT_STRUCT_BASE chained = {RBT_STRUCT_TYPE_TEXT_GLYPH_GENERATE_DESC, NULL};
    bad = desc;
    bad.base.next = &chained;
    CHECK(rbttext_generate_glyph(&bad, NULL, 0, &image, &error) == RBT_RESULT_STRUCT_UNKNOWN_IN_CHAIN);
    error.message[0] = '\0';
    CHECK(rbttext_generate_glyph(&desc, NULL, 0, &image, &error) == RBT_RESULT_INVALID_ARGUMENT); // no face
    CHECK(error.message[0] != '\0');
}

static RBTTEXT_ATLAS_DESC atlas_desc(float pixels_per_em, float distance_range, uint32_t page, uint32_t max_pages)
{
    RBTTEXT_ATLAS_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_TEXT_ATLAS_DESC;
    desc.pixels_per_em = pixels_per_em;
    desc.distance_range = distance_range;
    desc.page_width = page;
    desc.page_height = page;
    desc.max_pages = max_pages;
    return desc;
}

static void expect_atlas_refused(const RBTTEXT_ATLAS_DESC *desc, int line)
{
    rbttext_atlas atlas = (rbttext_atlas)SENTINEL;
    RBTTEXT_ERROR error;
    error.message[0] = '\0';
    RBT_RESULT result = rbttext_atlas_create(desc, &atlas, &error);
    if (result != RBT_RESULT_INVALID_ARGUMENT)
        printf("FAIL %s (%s:%d): expected RBT_RESULT_INVALID_ARGUMENT, got %d\n", current_test, __FILE__, line,
               (int)result);
    if (result != RBT_RESULT_INVALID_ARGUMENT)
        failures++;
    CHECK(atlas == NULL);
    CHECK(error.message[0] != '\0');
}

static void test_atlas_arguments(void)
{
    RBTTEXT_ATLAS_DESC desc = atlas_desc(32.0f, 4.0f, 256, 4);
    RBTTEXT_ATLAS_DESC bad;
    rbttext_atlas atlas;
    RBTTEXT_ERROR error;

    CHECK(rbttext_atlas_create(&desc, NULL, &error) == RBT_RESULT_INVALID_ARGUMENT);
    atlas = (rbttext_atlas)SENTINEL;
    CHECK(rbttext_atlas_create(NULL, &atlas, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(atlas == NULL);
    bad = desc;
    bad.base.type = RBT_STRUCT_TYPE_TEXT_ATLAS_LOAD_DESC;
    CHECK(rbttext_atlas_create(&bad, &atlas, &error) == RBT_RESULT_STRUCT_WRONG_TYPE);
    RBT_STRUCT_BASE chained = {RBT_STRUCT_TYPE_TEXT_ATLAS_DESC, NULL};
    bad = desc;
    bad.base.next = &chained;
    CHECK(rbttext_atlas_create(&bad, &atlas, &error) == RBT_RESULT_STRUCT_UNKNOWN_IN_CHAIN);

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

    RBTTEXT_ATLAS_INFO info;
    memset(&info, 0, sizeof(info));
    info.base.type = RBT_STRUCT_TYPE_TEXT_ATLAS_INFO;
    CHECK(rbttext_atlas_get_info(NULL, &info) == RBT_RESULT_INVALID_ARGUMENT);
    RBTTEXT_GLYPH_GENERATE_DESC generate;
    memset(&generate, 0, sizeof(generate));
    generate.base.type = RBT_STRUCT_TYPE_TEXT_GLYPH_GENERATE_DESC;
    CHECK(rbttext_atlas_insert(NULL, &generate, NULL, 0, NULL, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbttext_atlas_add_layout(NULL, NULL, &error) == RBT_RESULT_INVALID_ARGUMENT);
    RBTTEXT_ATLAS_GLYPH glyph;
    memset(&glyph, 0, sizeof(glyph));
    glyph.base.type = RBT_STRUCT_TYPE_TEXT_ATLAS_GLYPH;
    CHECK(rbttext_atlas_find_glyph(NULL, NULL, 0, &glyph) == RBT_RESULT_INVALID_ARGUMENT);
    const void *pixels;
    CHECK(rbttext_atlas_get_page(NULL, 0, &pixels) == RBT_RESULT_INVALID_ARGUMENT);
    uint32_t count;
    CHECK(rbttext_atlas_take_dirty(NULL, NULL, 0, &count) == RBT_RESULT_INVALID_ARGUMENT);
    uint64_t size;
    CHECK(rbttext_atlas_save(NULL, NULL, 0, NULL, 0, &size, &error) == RBT_RESULT_INVALID_ARGUMENT);
    rbttext_atlas_destroy(NULL);
}

static void test_atlas_load_arguments(void)
{
    static const uint8_t bytes[16] = {0};
    RBTTEXT_ATLAS_DESC settings = atlas_desc(32.0f, 4.0f, 256, 4);
    rbttext_face none[1] = {NULL};
    RBTTEXT_ATLAS_LOAD_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_TEXT_ATLAS_LOAD_DESC;
    desc.atlas = &settings;
    desc.data = bytes;
    desc.data_size = sizeof(bytes);
    desc.faces = NULL;
    desc.face_count = 0;
    rbttext_atlas atlas;
    RBTTEXT_ERROR error;

    CHECK(rbttext_atlas_load(&desc, NULL, &error) == RBT_RESULT_INVALID_ARGUMENT);
    atlas = (rbttext_atlas)SENTINEL;
    CHECK(rbttext_atlas_load(NULL, &atlas, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(atlas == NULL);
    RBTTEXT_ATLAS_LOAD_DESC bad = desc;
    bad.base.type = RBT_STRUCT_TYPE_TEXT_ATLAS_DESC;
    CHECK(rbttext_atlas_load(&bad, &atlas, &error) == RBT_RESULT_STRUCT_WRONG_TYPE);
    bad = desc;
    bad.atlas = NULL;
    CHECK(rbttext_atlas_load(&bad, &atlas, &error) == RBT_RESULT_INVALID_ARGUMENT);
    RBTTEXT_ATLAS_DESC bad_settings = settings;
    bad_settings.base.type = RBT_STRUCT_TYPE_TEXT_ATLAS_LOAD_DESC;
    bad = desc;
    bad.atlas = &bad_settings;
    CHECK(rbttext_atlas_load(&bad, &atlas, &error) == RBT_RESULT_STRUCT_WRONG_TYPE);
    bad_settings = settings;
    bad_settings.pixels_per_em = 0.0f;
    CHECK(rbttext_atlas_load(&bad, &atlas, &error) == RBT_RESULT_INVALID_ARGUMENT);
    bad = desc;
    bad.data = NULL;
    CHECK(rbttext_atlas_load(&bad, &atlas, &error) == RBT_RESULT_INVALID_ARGUMENT);
    bad = desc;
    bad.data_size = 0;
    CHECK(rbttext_atlas_load(&bad, &atlas, &error) == RBT_RESULT_INVALID_ARGUMENT);
    bad = desc;
    bad.face_count = 1;
    CHECK(rbttext_atlas_load(&bad, &atlas, &error) == RBT_RESULT_INVALID_ARGUMENT);
    bad = desc;
    bad.faces = none;
    bad.face_count = 1;
    error.message[0] = '\0';
    CHECK(rbttext_atlas_load(&bad, &atlas, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(atlas == NULL);
    CHECK(error.message[0] != '\0');
}

// ---- Faces ------------------------------------------------------------------------

static void test_face_metrics(void)
{
    rbttext_face a = open_face(&font_a, 0);
    if (!a)
        return;
    RBTTEXT_FACE_METRICS m;
    memset(&m, 0, sizeof(m));
    m.base.type = RBT_STRUCT_TYPE_TEXT_FACE_METRICS;
    RBT_RESULT result = rbttext_face_get_metrics(a, &m);
    if (!BUILT(result))
        return;
    CHECK(result == RBT_RESULT_OK);
    CHECK(m.base.type == RBT_STRUCT_TYPE_TEXT_FACE_METRICS);
    CHECK(m.base.next == NULL);
    CHECK(m.kind == RBTTEXT_FACE_KIND_OUTLINE);
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

    RBTTEXT_FACE_METRICS wrong = m;
    wrong.base.type = RBT_STRUCT_TYPE_TEXT_LAYOUT_INFO;
    CHECK(rbttext_face_get_metrics(a, &wrong) == RBT_RESULT_STRUCT_WRONG_TYPE);
    CHECK(rbttext_face_get_metrics(a, NULL) == RBT_RESULT_INVALID_ARGUMENT);
}

static void test_face_find_glyph(void)
{
    rbttext_face a = open_face(&font_a, 0);
    if (!a)
        return;
    uint32_t glyph = 99;
    RBT_RESULT result = rbttext_face_find_glyph(a, 0x61, &glyph);
    if (!BUILT(result))
        return;
    CHECK(result == RBT_RESULT_OK);
    CHECK(glyph == A_LOWER_A);
    CHECK(rbttext_face_find_glyph(a, 0x56, &glyph) == RBT_RESULT_OK);
    CHECK(glyph == A_UPPER_V);
    glyph = 99;
    CHECK(rbttext_face_find_glyph(a, 0x7A, &glyph) == RBT_RESULT_NOT_FOUND);
    CHECK(glyph == 0);
    CHECK(rbttext_face_find_glyph(a, 0x61, NULL) == RBT_RESULT_INVALID_ARGUMENT);
}

static void test_face_not_a_font(void)
{
    uint8_t junk[256];
    for (int i = 0; i < 256; i++)
        junk[i] = (uint8_t)(i * 37 + 11);
    RBTTEXT_FACE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_TEXT_FACE_DESC;
    desc.data = junk;
    desc.data_size = sizeof(junk);
    rbttext_face face = (rbttext_face)SENTINEL;
    RBTTEXT_ERROR error;
    RBT_RESULT result = rbttext_face_create(&desc, &face, &error);
    if (!BUILT(result))
        return;
    CHECK(result == RBT_RESULT_TEXT_NOT_A_FONT);
    CHECK(face == NULL);
    CHECK(error.message[0] != '\0');
    track_face(result == RBT_RESULT_OK ? face : NULL);

    // A font cut short.
    desc.data = font_a.data;
    desc.data_size = 40;
    face = (rbttext_face)SENTINEL;
    result = rbttext_face_create(&desc, &face, &error);
    if (!BUILT(result))
        return;
    CHECK(result == RBT_RESULT_TEXT_NOT_A_FONT);
    CHECK(face == NULL);
    track_face(result == RBT_RESULT_OK ? face : NULL);
}

static void test_face_collection(void)
{
    rbttext_face first = open_face(&font_ab_collection, 0);
    if (!first)
        return;
    rbttext_face second = open_face(&font_ab_collection, 1);
    if (!second)
        return;
    RBTTEXT_FACE_METRICS m;
    memset(&m, 0, sizeof(m));
    m.base.type = RBT_STRUCT_TYPE_TEXT_FACE_METRICS;
    RBT_RESULT result = rbttext_face_get_metrics(first, &m);
    if (!BUILT(result))
        return;
    CHECK(m.glyph_count == 9);
    CHECK(rbttext_face_get_metrics(second, &m) == RBT_RESULT_OK);
    CHECK(m.glyph_count == 6);

    RBTTEXT_FACE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_TEXT_FACE_DESC;
    desc.data = font_ab_collection.data;
    desc.data_size = font_ab_collection.size;
    desc.collection_index = 2;
    rbttext_face face = (rbttext_face)SENTINEL;
    RBTTEXT_ERROR error;
    CHECK(rbttext_face_create(&desc, &face, &error) == RBT_RESULT_NOT_FOUND);
    CHECK(face == NULL);
    // One font, asked for its second.
    desc.data = font_a.data;
    desc.data_size = font_a.size;
    desc.collection_index = 1;
    face = (rbttext_face)SENTINEL;
    CHECK(rbttext_face_create(&desc, &face, &error) == RBT_RESULT_NOT_FOUND);
    CHECK(face == NULL);
}

static void test_face_copies_data(void)
{
    uint8_t *copy = (uint8_t *)track_memory(font_a.size);
    if (!copy)
        return;
    memcpy(copy, font_a.data, font_a.size);
    RBTTEXT_FACE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_TEXT_FACE_DESC;
    desc.data = copy;
    desc.data_size = font_a.size;
    desc.copy_data = true;
    rbttext_face face = NULL;
    RBTTEXT_ERROR error;
    RBT_RESULT result = rbttext_face_create(&desc, &face, &error);
    if (!BUILT(result))
        return;
    CHECK(result == RBT_RESULT_OK);
    track_face(face);
    if (result != RBT_RESULT_OK)
        return;
    memset(copy, 0, font_a.size); // the caller's bytes are gone
    uint32_t glyph = 0;
    result = rbttext_face_find_glyph(face, 0x62, &glyph);
    if (!BUILT(result))
        return;
    CHECK(result == RBT_RESULT_OK);
    CHECK(glyph == A_LOWER_B);
    rbttext_chain chain = open_chain(&face, 1);
    if (!chain)
        return;
    RBTTEXT_LAYOUT_DESC layout = layout_desc(chain, "ab");
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
    rbttext_face a = NULL, b = NULL;
    rbttext_chain chain = open_chain_ab(&a, &b);
    if (!chain)
        return;
    CHECK(rbttext_chain_get_face_count(chain) == 2);
    CHECK(rbttext_chain_get_face(chain, 0) == a);
    CHECK(rbttext_chain_get_face(chain, 1) == b);
    CHECK(rbttext_chain_get_face(chain, 2) == NULL);

    uint32_t face_index = 99, glyph = 99;
    RBT_RESULT result = rbttext_chain_find_glyph(chain, 0x61, &face_index, &glyph);
    if (!BUILT(result))
        return;
    CHECK(result == RBT_RESULT_OK);
    CHECK(face_index == 0);
    CHECK(glyph == A_LOWER_A);
    CHECK(rbttext_chain_find_glyph(chain, 0x7A, &face_index, &glyph) == RBT_RESULT_OK);
    CHECK(face_index == 1);
    CHECK(glyph == B_LOWER_Z);
    face_index = 99;
    glyph = 99;
    CHECK(rbttext_chain_find_glyph(chain, 0x51, &face_index, &glyph) == RBT_RESULT_NOT_FOUND);
    CHECK(face_index == 0);
    CHECK(glyph == 0);
}

static void test_chain_refusals(void)
{
    rbttext_face a = open_face(&font_a, 0);
    if (!a)
        return;
    rbttext_face twice[2] = {a, a};
    RBTTEXT_CHAIN_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_TEXT_CHAIN_DESC;
    desc.faces = twice;
    desc.face_count = 2;
    rbttext_chain chain = (rbttext_chain)SENTINEL;
    RBTTEXT_ERROR error;
    RBT_RESULT result = rbttext_chain_create(&desc, &chain, &error);
    if (!BUILT(result))
        return;
    CHECK(result == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(chain == NULL);

    // Outline and stroke faces don't mix.
    RBTTEXT_STROKE_FACE_DESC stroke = stroke_desc();
    rbttext_face s = NULL;
    result = rbttext_face_create_stroke(&stroke, &s, &error);
    if (!BUILT(result))
        return;
    CHECK(result == RBT_RESULT_OK);
    track_face(s);
    rbttext_face mixed[2] = {a, s};
    desc.faces = mixed;
    chain = (rbttext_chain)SENTINEL;
    result = rbttext_chain_create(&desc, &chain, &error);
    if (!BUILT(result))
        return;
    CHECK(result == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(chain == NULL);
    CHECK(error.message[0] != '\0');
}

// ---- Shaping and layout --------------------------------------------------------------

static void test_layout_simple(void)
{
    rbttext_face a = open_face(&font_a, 0);
    if (!a)
        return;
    rbttext_chain chain = open_chain(&a, 1);
    if (!chain)
        return;
    RBTTEXT_LAYOUT_DESC desc = layout_desc(chain, "ab");
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
    const RBTTEXT_GLYPH *g = out.glyphs;
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

    const RBTTEXT_LINE *l = out.lines;
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

    const RBTTEXT_RUN *r = out.runs;
    CHECK(r[0].first_glyph == 0);
    CHECK(r[0].glyph_count == 2);
    CHECK(r[0].text_offset == 0);
    CHECK(r[0].text_size == 2);
    CHECK(r[0].face_index == 0);
    CHECK(r[0].script == RBTTEXT_TAG('L', 'a', 't', 'n'));
    CHECK(r[0].bidi_level == 0);
    CHECK(r[0].direction == RBTTEXT_DIRECTION_LTR);
    CHECK(r[0].line == 0);
}

static void test_layout_em_size(void)
{
    rbttext_face a = open_face(&font_a, 0);
    if (!a)
        return;
    rbttext_chain chain = open_chain(&a, 1);
    if (!chain)
        return;
    RBTTEXT_LAYOUT_DESC desc = layout_desc(chain, "ab");
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
    rbttext_face a = open_face(&font_a, 0);
    if (!a)
        return;
    rbttext_chain chain = open_chain(&a, 1);
    if (!chain)
        return;
    RBTTEXT_LAYOUT_DESC desc = layout_desc(chain, "AV");
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
    desc.features = RBTTEXT_FEATURE_LIGATURES;
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
    rbttext_face a = open_face(&font_a, 0);
    if (!a)
        return;
    rbttext_chain chain = open_chain(&a, 1);
    if (!chain)
        return;
    RBTTEXT_LAYOUT_DESC desc = layout_desc(chain, "fib");
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
    desc.features = RBTTEXT_FEATURE_KERNING;
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
    // 'liga' is RBTTEXT_FEATURE_LIGATURES'.
    RBTTEXT_FEATURE liga = {RBTTEXT_TAG('l', 'i', 'g', 'a'), 0};
    desc.extra_features = &liga;
    desc.extra_feature_count = 1;
    rbttext_layout layout = NULL;
    if (!BUILT(rbttext_layout_create(&layout)))
        return;
    track_layout(layout);
    RBTTEXT_ERROR error;
    RBT_RESULT result = rbttext_layout_build(layout, &desc, &error);
    if (!BUILT(result))
        return;
    CHECK(result == RBT_RESULT_INVALID_ARGUMENT);
}

static void test_layout_fallback(void)
{
    rbttext_chain chain = open_chain_ab(NULL, NULL);
    if (!chain)
        return;
    RBTTEXT_LAYOUT_DESC desc = layout_desc(chain, "az");
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
    rbttext_face reversed[2];
    reversed[0] = rbttext_chain_get_face(chain, 1);
    reversed[1] = rbttext_chain_get_face(chain, 0);
    rbttext_chain chain_ba = open_chain(reversed, 2);
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
    rbttext_chain chain = open_chain_ab(NULL, NULL);
    if (!chain)
        return;
    RBTTEXT_LAYOUT_DESC desc = layout_desc(chain, "aQb");
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
    rbttext_face a = open_face(&font_a, 0);
    if (!a)
        return;
    rbttext_chain chain = open_chain(&a, 1);
    if (!chain)
        return;
    RBTTEXT_LAYOUT_DESC desc = layout_desc(chain, "a\xFF"
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
    rbttext_face a = open_face(&font_a, 0);
    if (!a)
        return;
    rbttext_chain chain = open_chain(&a, 1);
    if (!chain)
        return;
    // U+200D ZERO WIDTH JOINER between two letters: no glyph, not missing.
    RBTTEXT_LAYOUT_DESC desc = layout_desc(chain, "a\xE2\x80\x8D"
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
    rbttext_face a = open_face(&font_a, 0);
    if (!a)
        return;
    rbttext_chain chain = open_chain(&a, 1);
    if (!chain)
        return;
    // "aa aa " is 2.25 em with its space hanging, "aa aa aa" 3.5.
    RBTTEXT_LAYOUT_DESC desc = layout_desc(chain, "aa aa aa");
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
    rbttext_face a = open_face(&font_a, 0);
    if (!a)
        return;
    rbttext_chain chain = open_chain(&a, 1);
    if (!chain)
        return;
    RBTTEXT_LAYOUT_DESC desc = layout_desc(chain, "a\nb");
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
    rbttext_face a = open_face(&font_a, 0);
    if (!a)
        return;
    rbttext_chain chain = open_chain(&a, 1);
    if (!chain)
        return;
    RBTTEXT_LAYOUT_DESC desc = layout_desc(chain, "");
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
    rbttext_face a = open_face(&font_a, 0);
    if (!a)
        return;
    rbttext_chain chain = open_chain(&a, 1);
    if (!chain)
        return;
    RBTTEXT_LAYOUT_DESC desc = layout_desc(chain, "ab");
    desc.max_width = 4.0f;
    desc.align = RBTTEXT_ALIGN_CENTER;
    LaidOut out;
    if (!lay_out_and_read(&desc, &out))
        return;
    if (out.line_count == 1 && out.glyph_count == 2)
    {
        CHECK(out.lines[0].box_left == 1.5f);
        CHECK(out.lines[0].box_right == 2.5f);
        CHECK(out.glyphs[0].x == 1.5f);
    }
    desc.align = RBTTEXT_ALIGN_RIGHT;
    if (!lay_out_and_read(&desc, &out))
        return;
    if (out.glyph_count == 2)
        CHECK(out.glyphs[0].x == 3.0f);
    desc.align = RBTTEXT_ALIGN_END; // a left-to-right paragraph ends on the right
    if (!lay_out_and_read(&desc, &out))
        return;
    if (out.glyph_count == 2)
        CHECK(out.glyphs[0].x == 3.0f);
    desc.align = RBTTEXT_ALIGN_START;
    if (!lay_out_and_read(&desc, &out))
        return;
    if (out.glyph_count == 2)
        CHECK(out.glyphs[0].x == 0.0f);

    // With no max_width, lines align within the widest.
    desc = layout_desc(chain, "ab\na");
    desc.align = RBTTEXT_ALIGN_CENTER;
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
    rbttext_face a = open_face(&font_a, 0);
    if (!a)
        return;
    rbttext_chain chain = open_chain(&a, 1);
    if (!chain)
        return;
    RBTTEXT_LAYOUT_DESC desc = layout_desc(chain, "ab");
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
    rbttext_chain chain = open_chain_ab(NULL, NULL);
    if (!chain)
        return;
    // 'a' then U+4E00: a Latin run from A, a Han run from B.
    RBTTEXT_LAYOUT_DESC desc = layout_desc(chain, "a\xE4\xB8\x80");
    LaidOut out;
    if (!lay_out_and_read(&desc, &out))
        return;
    CHECK(out.run_count == 2);
    if (out.run_count == 2)
    {
        CHECK(out.runs[0].script == RBTTEXT_TAG('L', 'a', 't', 'n'));
        CHECK(out.runs[1].script == RBTTEXT_TAG('H', 'a', 'n', 'i'));
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
        CHECK(out.runs[0].direction == RBTTEXT_DIRECTION_LTR);
        CHECK(out.runs[0].bidi_level == 0);
        CHECK(out.runs[1].direction == RBTTEXT_DIRECTION_RTL);
        CHECK(out.runs[1].bidi_level == 1);
        CHECK(out.runs[1].script == RBTTEXT_TAG('H', 'e', 'b', 'r'));
    }

    // Latin in a right-to-left paragraph keeps its order, at level 2, and
    // starts from the right.
    desc = layout_desc(chain, "ab");
    desc.direction = RBTTEXT_DIRECTION_RTL;
    desc.max_width = 4.0f;
    desc.align = RBTTEXT_ALIGN_START;
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
        CHECK(out.runs[0].direction == RBTTEXT_DIRECTION_LTR);
    }

    // AUTO takes the paragraph's direction from its first strong character.
    desc = layout_desc(chain, "\xD7\x90"
                              "a");
    desc.direction = RBTTEXT_DIRECTION_AUTO;
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
    rbttext_chain chain = open_chain_ab(NULL, NULL);
    if (!chain)
        return;
    RBTTEXT_LAYOUT_DESC desc = layout_desc(chain, "fi AV az\naQ a\xD7\x90\xD7\x91 aa aa aa");
    desc.max_width = 3.0f;
    desc.em_size = 16.0f;
    LaidOut first, second;
    if (!lay_out_and_read(&desc, &first))
        return;
    rbttext_layout other = lay_out(&desc);
    if (!other)
        return;
    if (!read_layout(other, &second))
        return;
    CHECK(first.glyph_count == second.glyph_count);
    CHECK(first.line_count == second.line_count);
    CHECK(first.run_count == second.run_count);
    if (first.glyph_count == second.glyph_count && first.glyph_count)
        CHECK(memcmp(first.glyphs, second.glyphs, first.glyph_count * sizeof(RBTTEXT_GLYPH)) == 0);
    if (first.line_count == second.line_count && first.line_count)
        CHECK(memcmp(first.lines, second.lines, first.line_count * sizeof(RBTTEXT_LINE)) == 0);
    if (first.run_count == second.run_count && first.run_count)
        CHECK(memcmp(first.runs, second.runs, first.run_count * sizeof(RBTTEXT_RUN)) == 0);
}

static void test_layout_rebuild_and_failure(void)
{
    rbttext_face a = open_face(&font_a, 0);
    if (!a)
        return;
    rbttext_chain chain = open_chain(&a, 1);
    if (!chain)
        return;
    rbttext_layout layout = NULL;
    RBT_RESULT result = rbttext_layout_create(&layout);
    if (!BUILT(result))
        return;
    CHECK(result == RBT_RESULT_OK);
    track_layout(layout);
    LaidOut out;
    if (!read_layout(layout, &out))
        return;
    CHECK(out.glyph_count == 0); // never built
    CHECK(out.line_count == 0);
    CHECK(out.glyphs == NULL);

    RBTTEXT_LAYOUT_DESC desc = layout_desc(chain, "ab");
    RBTTEXT_ERROR error;
    result = rbttext_layout_build(layout, &desc, &error);
    if (!BUILT(result))
        return;
    CHECK(result == RBT_RESULT_OK);
    desc = layout_desc(chain, "b");
    CHECK(rbttext_layout_build(layout, &desc, &error) == RBT_RESULT_OK);
    if (!read_layout(layout, &out))
        return;
    CHECK(out.glyph_count == 1);

    // The argument checks a layout makes, each on its own.
    RBTTEXT_LAYOUT_DESC bad;
    bad = desc;
    bad.base.type = RBT_STRUCT_TYPE_TEXT_CHAIN_DESC;
    CHECK(rbttext_layout_build(layout, &bad, &error) == RBT_RESULT_STRUCT_WRONG_TYPE);
    CHECK(rbttext_layout_build(layout, NULL, &error) == RBT_RESULT_INVALID_ARGUMENT);
    bad = desc;
    bad.chain = NULL;
    CHECK(rbttext_layout_build(layout, &bad, &error) == RBT_RESULT_INVALID_ARGUMENT);
    bad = desc;
    bad.text = NULL;
    CHECK(rbttext_layout_build(layout, &bad, &error) == RBT_RESULT_INVALID_ARGUMENT);
    bad = desc;
    bad.direction = (RBTTEXT_DIRECTION)0;
    CHECK(rbttext_layout_build(layout, &bad, &error) == RBT_RESULT_INVALID_ARGUMENT);
    bad.direction = (RBTTEXT_DIRECTION)4;
    CHECK(rbttext_layout_build(layout, &bad, &error) == RBT_RESULT_INVALID_ARGUMENT);
    bad = desc;
    bad.features = 0x80;
    CHECK(rbttext_layout_build(layout, &bad, &error) == RBT_RESULT_INVALID_ARGUMENT);
    RBTTEXT_FEATURE twice[2] = {{RBTTEXT_TAG('s', 'm', 'c', 'p'), 1}, {RBTTEXT_TAG('s', 'm', 'c', 'p'), 0}};
    bad = desc;
    bad.extra_features = twice;
    bad.extra_feature_count = 2;
    CHECK(rbttext_layout_build(layout, &bad, &error) == RBT_RESULT_INVALID_ARGUMENT);
    bad.extra_features = NULL;
    CHECK(rbttext_layout_build(layout, &bad, &error) == RBT_RESULT_INVALID_ARGUMENT);
    bad = desc;
    bad.em_size = 0.0f;
    CHECK(rbttext_layout_build(layout, &bad, &error) == RBT_RESULT_INVALID_ARGUMENT);
    bad.em_size = NAN;
    CHECK(rbttext_layout_build(layout, &bad, &error) == RBT_RESULT_INVALID_ARGUMENT);
    bad = desc;
    bad.max_width = -1.0f;
    CHECK(rbttext_layout_build(layout, &bad, &error) == RBT_RESULT_INVALID_ARGUMENT);
    bad.max_width = INFINITY;
    CHECK(rbttext_layout_build(layout, &bad, &error) == RBT_RESULT_INVALID_ARGUMENT);
    bad = desc;
    bad.align = (RBTTEXT_ALIGN)0;
    CHECK(rbttext_layout_build(layout, &bad, &error) == RBT_RESULT_INVALID_ARGUMENT);
    bad.align = (RBTTEXT_ALIGN)6;
    CHECK(rbttext_layout_build(layout, &bad, &error) == RBT_RESULT_INVALID_ARGUMENT);
    bad = desc;
    bad.line_spacing = 0.0f;
    CHECK(rbttext_layout_build(layout, &bad, &error) == RBT_RESULT_INVALID_ARGUMENT);
    bad = desc;
    bad.letter_spacing = NAN;
    error.message[0] = '\0';
    CHECK(rbttext_layout_build(layout, &bad, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(error.message[0] != '\0');

    // A failed build leaves the layout empty.
    if (!read_layout(layout, &out))
        return;
    CHECK(out.glyph_count == 0);
    CHECK(out.line_count == 0);

    const RBTTEXT_GLYPH *glyphs;
    uint32_t count;
    CHECK(rbttext_layout_get_glyphs(layout, NULL, &count) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbttext_layout_get_glyphs(layout, &glyphs, NULL) == RBT_RESULT_INVALID_ARGUMENT);
    RBTTEXT_LAYOUT_INFO info;
    memset(&info, 0, sizeof(info));
    info.base.type = RBT_STRUCT_TYPE_TEXT_FACE_METRICS;
    CHECK(rbttext_layout_get_info(layout, &info) == RBT_RESULT_STRUCT_WRONG_TYPE);
}

// ---- Single-stroke faces ----------------------------------------------------------------

static void test_stroke_face(void)
{
    RBTTEXT_STROKE_FACE_DESC desc = stroke_desc();
    rbttext_face face = NULL;
    RBTTEXT_ERROR error;
    RBT_RESULT result = rbttext_face_create_stroke(&desc, &face, &error);
    if (!BUILT(result))
        return;
    CHECK(result == RBT_RESULT_OK);
    track_face(face);
    if (result != RBT_RESULT_OK)
        return;

    RBTTEXT_FACE_METRICS m;
    memset(&m, 0, sizeof(m));
    m.base.type = RBT_STRUCT_TYPE_TEXT_FACE_METRICS;
    result = rbttext_face_get_metrics(face, &m);
    if (!BUILT(result))
        return;
    CHECK(m.kind == RBTTEXT_FACE_KIND_STROKE);
    CHECK(m.units_per_em == 0);
    CHECK(m.glyph_count == 3); // .notdef and two
    CHECK(m.ascender == 0.75f);
    CHECK(m.descender == -0.25f);

    uint32_t glyph = 0;
    result = rbttext_face_find_glyph(face, 0x4C, &glyph);
    if (!BUILT(result))
        return;
    CHECK(result == RBT_RESULT_OK);
    CHECK(glyph == 1); // glyphs[0]

    const RbtVec2 *points = NULL;
    const RBTTEXT_STROKE_POLYLINE *polylines = NULL;
    uint32_t point_count = 0, polyline_count = 0;
    result = rbttext_face_get_glyph_strokes(face, 1, &points, &point_count, &polylines, &polyline_count);
    if (!BUILT(result))
        return;
    CHECK(result == RBT_RESULT_OK);
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
    CHECK(rbttext_face_get_glyph_strokes(face, 3, &points, &point_count, &polylines, &polyline_count) ==
          RBT_RESULT_NOT_FOUND);

    rbttext_chain chain = open_chain(&face, 1);
    if (!chain)
        return;
    RBTTEXT_LAYOUT_DESC layout_d = layout_desc(chain, "L L");
    rbttext_layout layout = lay_out(&layout_d);
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
    result = rbttext_layout_get_strokes(layout, NULL, 0, &count);
    if (!BUILT(result))
        return;
    CHECK(result == RBT_RESULT_OK);
    CHECK(count == 4);
    RBTTEXT_SEGMENT segments[4];
    CHECK(rbttext_layout_get_strokes(layout, segments, 2, &count) == RBT_RESULT_LIMIT);
    CHECK(rbttext_layout_get_strokes(layout, segments, 4, &count) == RBT_RESULT_OK);
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
    RBTTEXT_GLYPH_GENERATE_DESC generate;
    memset(&generate, 0, sizeof(generate));
    generate.base.type = RBT_STRUCT_TYPE_TEXT_GLYPH_GENERATE_DESC;
    generate.face = face;
    generate.glyph = 1;
    generate.pixels_per_em = 32.0f;
    generate.distance_range = 4.0f;
    RBTTEXT_GLYPH_IMAGE image;
    memset(&image, 0, sizeof(image));
    image.base.type = RBT_STRUCT_TYPE_TEXT_GLYPH_IMAGE;
    result = rbttext_generate_glyph(&generate, NULL, 0, &image, &error);
    if (!BUILT(result))
        return;
    CHECK(result == RBT_RESULT_INVALID_ARGUMENT);
    RBTTEXT_ATLAS_DESC settings = atlas_desc(32.0f, 4.0f, 64, 4);
    rbttext_atlas atlas = NULL;
    result = rbttext_atlas_create(&settings, &atlas, &error);
    if (!BUILT(result))
        return;
    track_atlas(atlas);
    result = rbttext_atlas_add_layout(atlas, layout, &error);
    if (!BUILT(result))
        return;
    CHECK(result == RBT_RESULT_INVALID_ARGUMENT);
}

static void test_stroke_face_duplicates(void)
{
    RBTTEXT_STROKE_GLYPH glyphs[2];
    memcpy(glyphs, stroke_glyphs, sizeof(glyphs));
    glyphs[1].codepoint = glyphs[0].codepoint;
    RBTTEXT_STROKE_FACE_DESC desc = stroke_desc();
    desc.glyphs = glyphs;
    rbttext_face face = (rbttext_face)SENTINEL;
    RBTTEXT_ERROR error;
    RBT_RESULT result = rbttext_face_create_stroke(&desc, &face, &error);
    if (!BUILT(result))
        return;
    CHECK(result == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(face == NULL);
    track_face(result == RBT_RESULT_OK ? face : NULL);
}

static void test_hershey_face(void)
{
    RBTTEXT_HERSHEY_FACE_DESC desc = hershey_desc();
    rbttext_face face = NULL;
    RBTTEXT_ERROR error;
    RBT_RESULT result = rbttext_face_create_hershey(&desc, &face, &error);
    if (!BUILT(result))
        return;
    CHECK(result == RBT_RESULT_OK);
    track_face(face);
    if (result != RBT_RESULT_OK)
        return;

    uint32_t glyph_i = 0, glyph_h = 0;
    result = rbttext_face_find_glyph(face, 0x49, &glyph_i);
    if (!BUILT(result))
        return;
    CHECK(result == RBT_RESULT_OK);
    CHECK(rbttext_face_find_glyph(face, 0x48, &glyph_h) == RBT_RESULT_OK);

    const RbtVec2 *points = NULL;
    const RBTTEXT_STROKE_POLYLINE *polylines = NULL;
    uint32_t point_count = 0, polyline_count = 0;
    result = rbttext_face_get_glyph_strokes(face, glyph_i, &points, &point_count, &polylines, &polyline_count);
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
    CHECK(rbttext_face_get_glyph_strokes(face, glyph_h, &points, &point_count, &polylines, &polyline_count) ==
          RBT_RESULT_OK);
    CHECK(polyline_count == 2);
    CHECK(point_count == 4);
    if (point_count == 4)
    {
        CHECK(points[0].x == 0.0625f);
        CHECK(points[3].x == 0.1875f);
    }

    rbttext_chain chain = open_chain(&face, 1);
    if (!chain)
        return;
    RBTTEXT_LAYOUT_DESC layout_d = layout_desc(chain, "I I");
    rbttext_layout layout = lay_out(&layout_d);
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
    RBTTEXT_SEGMENT segments[2];
    uint32_t count = 0;
    result = rbttext_layout_get_strokes(layout, segments, 2, &count);
    if (!BUILT(result))
        return;
    CHECK(result == RBT_RESULT_OK);
    CHECK(count == 2);
    if (count == 2)
    {
        CHECK(segments[0].x0 == 0.125f);
        CHECK(segments[0].y0 == 0.5f);
        CHECK(segments[0].y1 == 0.75f);
        CHECK(segments[1].x0 == 0.625f);
    }

    // Not a Hershey font, and a codepoint list that doesn't match.
    RBTTEXT_HERSHEY_FACE_DESC bad = desc;
    bad.data = "hello, world";
    bad.data_size = 12;
    bad.codepoints = NULL;
    bad.codepoint_count = 0;
    bad.first_codepoint = 0x20;
    rbttext_face other = (rbttext_face)SENTINEL;
    result = rbttext_face_create_hershey(&bad, &other, &error);
    CHECK(result == RBT_RESULT_TEXT_NOT_A_FONT);
    CHECK(other == NULL);
    track_face(result == RBT_RESULT_OK ? other : NULL);
    bad = desc;
    bad.codepoint_count = 2;
    other = (rbttext_face)SENTINEL;
    result = rbttext_face_create_hershey(&bad, &other, &error);
    CHECK(result == RBT_RESULT_INVALID_ARGUMENT);
    track_face(result == RBT_RESULT_OK ? other : NULL);
}

// ---- Distance fields ------------------------------------------------------------------

static RBTTEXT_GLYPH_GENERATE_DESC generate_desc(rbttext_face face, uint32_t glyph, float pixels_per_em,
                                                float distance_range)
{
    RBTTEXT_GLYPH_GENERATE_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_TEXT_GLYPH_GENERATE_DESC;
    desc.face = face;
    desc.glyph = glyph;
    desc.pixels_per_em = pixels_per_em;
    desc.distance_range = distance_range;
    return desc;
}

static RBTTEXT_GLYPH_IMAGE empty_image(void)
{
    RBTTEXT_GLYPH_IMAGE image;
    memset(&image, 0, sizeof(image));
    image.base.type = RBT_STRUCT_TYPE_TEXT_GLYPH_IMAGE;
    return image;
}

// Generates a glyph into tracked memory, or returns NULL.
static uint8_t *generate(const RBTTEXT_GLYPH_GENERATE_DESC *desc, RBTTEXT_GLYPH_IMAGE *image, uint64_t *out_size)
{
    *image = empty_image();
    RBTTEXT_ERROR error;
    RBT_RESULT result = rbttext_generate_glyph(desc, NULL, 0, image, &error);
    if (!BUILT(result))
        return NULL;
    CHECK(result == RBT_RESULT_OK);
    if (result != RBT_RESULT_OK)
        return NULL;
    uint64_t size = (uint64_t)image->width * image->height * 4;
    uint8_t *pixels = (uint8_t *)track_memory((size_t)size);
    if (!pixels)
        return NULL;
    RBTTEXT_GLYPH_IMAGE again = empty_image();
    result = rbttext_generate_glyph(desc, size ? pixels : NULL, size, &again, &error);
    if (!BUILT(result))
        return NULL;
    CHECK(result == RBT_RESULT_OK);
    CHECK(again.width == image->width);
    CHECK(again.height == image->height);
    CHECK(again.plane_left == image->plane_left);
    CHECK(again.plane_bottom == image->plane_bottom);
    *out_size = size;
    return result == RBT_RESULT_OK ? pixels : NULL;
}

// A channel of [pixels] sampled bilinearly at the point (ex, ey), in em
// units from the glyph's origin with y down, through the image's plane
// bounds; 0 to 255.
static double sample(const uint8_t *pixels, const RBTTEXT_GLYPH_IMAGE *image, int channel, double ex, double ey)
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

static double median_at(const uint8_t *pixels, const RBTTEXT_GLYPH_IMAGE *image, double ex, double ey)
{
    double r = sample(pixels, image, 0, ex, ey);
    double g = sample(pixels, image, 1, ex, ey);
    double b = sample(pixels, image, 2, ex, ey);
    double lo = r < g ? r : g, hi = r < g ? g : r;
    return b < lo ? lo : (b > hi ? hi : b);
}

static void test_distance_field(void)
{
    rbttext_face a = open_face(&font_a, 0);
    if (!a)
        return;
    // Glyph 'a' is the square [0.0625, 0.4375] by [0, 0.375] em, y up: at
    // 64 pixels per em, 24 pixels a side; the range is 4 pixels.
    RBTTEXT_GLYPH_GENERATE_DESC desc = generate_desc(a, A_LOWER_A, 64.0f, 4.0f);
    RBTTEXT_GLYPH_IMAGE image;
    uint64_t size = 0;
    uint8_t *pixels = generate(&desc, &image, &size);
    if (!pixels)
        return;
    CHECK(image.base.type == RBT_STRUCT_TYPE_TEXT_GLYPH_IMAGE);
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
    RBTTEXT_GLYPH_IMAGE image2;
    uint64_t size2 = 0;
    uint8_t *pixels2 = generate(&desc, &image2, &size2);
    if (!pixels2)
        return;
    CHECK(size2 == size);
    if (size2 == size)
        CHECK(memcmp(pixels, pixels2, (size_t)size) == 0);

    // Too little memory, a glyph past the last, and a glyph with no outline.
    RBTTEXT_GLYPH_IMAGE out = empty_image();
    RBTTEXT_ERROR error;
    CHECK(rbttext_generate_glyph(&desc, pixels, size - 1, &out, &error) == RBT_RESULT_INVALID_ARGUMENT);
    RBTTEXT_GLYPH_GENERATE_DESC past = generate_desc(a, 9, 64.0f, 4.0f);
    out = empty_image();
    CHECK(rbttext_generate_glyph(&past, NULL, 0, &out, &error) == RBT_RESULT_NOT_FOUND);
    RBTTEXT_GLYPH_GENERATE_DESC space = generate_desc(a, A_SPACE, 64.0f, 4.0f);
    out = empty_image();
    CHECK(rbttext_generate_glyph(&space, NULL, 0, &out, &error) == RBT_RESULT_OK);
    CHECK(out.width == 0);
    CHECK(out.height == 0);
    CHECK(out.plane_left == 0.0f);
    CHECK(out.plane_right == 0.0f);

    // The argument checks that need a face.
    RBTTEXT_GLYPH_GENERATE_DESC bad = desc;
    bad.pixels_per_em = 0.0f;
    out = empty_image();
    CHECK(rbttext_generate_glyph(&bad, NULL, 0, &out, &error) == RBT_RESULT_INVALID_ARGUMENT);
    bad = desc;
    bad.distance_range = NAN;
    CHECK(rbttext_generate_glyph(&bad, NULL, 0, &out, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbttext_generate_glyph(&desc, NULL, 16, &out, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbttext_generate_glyph(&desc, NULL, 0, NULL, &error) == RBT_RESULT_INVALID_ARGUMENT);
    out.base.type = RBT_STRUCT_TYPE_TEXT_ATLAS_GLYPH;
    CHECK(rbttext_generate_glyph(&desc, NULL, 0, &out, &error) == RBT_RESULT_STRUCT_WRONG_TYPE);
}

// ---- The atlas ------------------------------------------------------------------------

static rbttext_atlas open_atlas(float pixels_per_em, float distance_range, uint32_t page, uint32_t max_pages)
{
    RBTTEXT_ATLAS_DESC desc = atlas_desc(pixels_per_em, distance_range, page, max_pages);
    rbttext_atlas atlas = NULL;
    RBTTEXT_ERROR error;
    RBT_RESULT result = rbttext_atlas_create(&desc, &atlas, &error);
    if (!BUILT(result))
        return NULL;
    CHECK(result == RBT_RESULT_OK);
    track_atlas(atlas);
    return atlas;
}

static bool read_atlas_info(rbttext_atlas atlas, RBTTEXT_ATLAS_INFO *info)
{
    memset(info, 0, sizeof(*info));
    info->base.type = RBT_STRUCT_TYPE_TEXT_ATLAS_INFO;
    RBT_RESULT result = rbttext_atlas_get_info(atlas, info);
    if (!BUILT(result))
        return false;
    CHECK(result == RBT_RESULT_OK);
    return result == RBT_RESULT_OK;
}

static RBTTEXT_ATLAS_GLYPH empty_atlas_glyph(void)
{
    RBTTEXT_ATLAS_GLYPH glyph;
    memset(&glyph, 0, sizeof(glyph));
    glyph.base.type = RBT_STRUCT_TYPE_TEXT_ATLAS_GLYPH;
    return glyph;
}

// Generates and inserts one glyph; false if pending or failed.
static bool insert(rbttext_atlas atlas, rbttext_face face, uint32_t glyph, RBTTEXT_ATLAS_GLYPH *out_glyph,
                   const uint8_t **out_pixels, RBTTEXT_GLYPH_IMAGE *out_image)
{
    RBTTEXT_ATLAS_INFO info;
    if (!read_atlas_info(atlas, &info))
        return false;
    RBTTEXT_GLYPH_GENERATE_DESC desc = generate_desc(face, glyph, info.pixels_per_em, info.distance_range);
    uint64_t size = 0;
    uint8_t *pixels = generate(&desc, out_image, &size);
    if (!pixels)
        return false; // pending, or failed
    *out_glyph = empty_atlas_glyph();
    RBTTEXT_ERROR error;
    RBT_RESULT result = rbttext_atlas_insert(atlas, &desc, size ? pixels : NULL, size, out_glyph, &error);
    if (!BUILT(result))
        return false;
    CHECK(result == RBT_RESULT_OK);
    if (result != RBT_RESULT_OK)
        printf("  %s\n", error.message);
    if (out_pixels)
        *out_pixels = pixels;
    return result == RBT_RESULT_OK;
}

static bool rects_overlap(const RBTTEXT_ATLAS_GLYPH *a, const RBTTEXT_ATLAS_GLYPH *b)
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
    rbttext_face a = NULL, b = NULL;
    rbttext_chain chain = open_chain_ab(&a, &b);
    if (!chain)
        return;
    // Pages of 48 pixels hold a couple of glyphs each, so this spreads
    // over several.
    rbttext_atlas atlas = open_atlas(32.0f, 4.0f, 48, 64);
    if (!atlas)
        return;
    RBTTEXT_ATLAS_INFO info;
    if (!read_atlas_info(atlas, &info))
        return;
    CHECK(info.page_count == 0);
    CHECK(info.glyph_count == 0);
    CHECK(info.pixels_per_em == 32.0f);
    CHECK(info.distance_range == 4.0f);
    CHECK(info.page_width == 48);
    CHECK(info.page_height == 48);

    enum { COUNT = 13 };
    rbttext_face faces[COUNT];
    uint32_t ids[COUNT];
    RBTTEXT_ATLAS_GLYPH placed[COUNT];
    const uint8_t *images[COUNT];
    RBTTEXT_GLYPH_IMAGE sizes[COUNT];
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
        const RBTTEXT_ATLAS_GLYPH *p = &placed[i];
        CHECK(p->base.type == RBT_STRUCT_TYPE_TEXT_ATLAS_GLYPH);
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
        CHECK(rbttext_atlas_get_page(atlas, p->page, &page) == RBT_RESULT_OK);
        if (page && images[i])
        {
            const uint8_t *row = (const uint8_t *)page;
            for (uint32_t y = 0; y < p->height; y++)
                CHECK(memcmp(row + ((size_t)(p->y + y) * info.page_width + p->x) * 4,
                             images[i] + (size_t)y * p->width * 4, (size_t)p->width * 4) == 0);
        }
        // Found where it was put.
        RBTTEXT_ATLAS_GLYPH found = empty_atlas_glyph();
        CHECK(rbttext_atlas_find_glyph(atlas, faces[i], ids[i], &found) == RBT_RESULT_OK);
        CHECK(found.page == p->page);
        CHECK(found.x == p->x);
        CHECK(found.y == p->y);
    }
    const void *page = NULL;
    CHECK(rbttext_atlas_get_page(atlas, info.page_count, &page) == RBT_RESULT_NOT_FOUND);
    RBTTEXT_ATLAS_GLYPH found = empty_atlas_glyph();
    CHECK(rbttext_atlas_find_glyph(atlas, b, B_HAN_ONE, &found) == RBT_RESULT_NOT_FOUND);

    // Inserting a glyph again leaves it where it is.
    RBTTEXT_ATLAS_GLYPH again;
    const uint8_t *image_again;
    RBTTEXT_GLYPH_IMAGE size_again;
    if (!insert(atlas, a, A_LOWER_B, &again, &image_again, &size_again))
        return;
    CHECK(again.page == placed[A_LOWER_B].page);
    CHECK(again.x == placed[A_LOWER_B].x);
    CHECK(again.y == placed[A_LOWER_B].y);
    RBTTEXT_ATLAS_INFO after;
    if (!read_atlas_info(atlas, &after))
        return;
    CHECK(after.glyph_count == info.glyph_count);
}

static void test_atlas_dirty(void)
{
    rbttext_face a = open_face(&font_a, 0);
    if (!a)
        return;
    rbttext_atlas atlas = open_atlas(32.0f, 4.0f, 48, 64);
    if (!atlas)
        return;
    RBTTEXT_ATLAS_GLYPH placed[4];
    RBTTEXT_GLYPH_IMAGE sizes[4];
    const uint32_t ids[4] = {A_LOWER_A, A_LOWER_B, A_LOWER_F, A_UPPER_V};
    for (int i = 0; i < 4; i++)
        if (!insert(atlas, a, ids[i], &placed[i], NULL, &sizes[i]))
            return;
    RBTTEXT_ATLAS_INFO info;
    if (!read_atlas_info(atlas, &info))
        return;
    CHECK(info.dirty_rect_count >= 1);
    CHECK(info.dirty_rect_count <= info.page_count);

    RBTTEXT_DIRTY_RECT rects[64];
    uint32_t count = 99;
    RBT_RESULT result = rbttext_atlas_take_dirty(atlas, NULL, 0, &count);
    if (!BUILT(result))
        return;
    CHECK(result == RBT_RESULT_OK);
    CHECK(count == 0); // nothing taken
    // Take one, then the rest.
    CHECK(rbttext_atlas_take_dirty(atlas, rects, 1, &count) == RBT_RESULT_OK);
    CHECK(count == 1);
    uint32_t taken = count;
    CHECK(rbttext_atlas_take_dirty(atlas, rects + 1, 63, &count) == RBT_RESULT_OK);
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
            const RBTTEXT_DIRTY_RECT *r = &rects[i];
            if (r->page == placed[g].page && r->x <= placed[g].x && r->y <= placed[g].y &&
                placed[g].x + placed[g].width <= r->x + r->width && placed[g].y + placed[g].height <= r->y + r->height)
                covered = true;
        }
        CHECK(covered);
    }
    if (!read_atlas_info(atlas, &info))
        return;
    CHECK(info.dirty_rect_count == 0);
    CHECK(rbttext_atlas_take_dirty(atlas, rects, 64, &count) == RBT_RESULT_OK);
    CHECK(count == 0);

    // One more glyph: one rectangle, around it.
    RBTTEXT_ATLAS_GLYPH more;
    RBTTEXT_GLYPH_IMAGE more_size;
    if (!insert(atlas, a, A_UPPER_A, &more, NULL, &more_size))
        return;
    CHECK(rbttext_atlas_take_dirty(atlas, rects, 64, &count) == RBT_RESULT_OK);
    CHECK(count == 1);
    if (count == 1)
    {
        CHECK(rects[0].page == more.page);
        CHECK(rects[0].x <= more.x);
        CHECK(rects[0].y <= more.y);
        CHECK(more.x + more.width <= rects[0].x + rects[0].width);
        CHECK(more.y + more.height <= rects[0].y + rects[0].height);
    }
    CHECK(rbttext_atlas_take_dirty(atlas, NULL, 4, &count) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbttext_atlas_take_dirty(atlas, rects, 4, NULL) == RBT_RESULT_INVALID_ARGUMENT);
}

static void test_atlas_limits(void)
{
    rbttext_face a = NULL, b = NULL;
    rbttext_chain chain = open_chain_ab(&a, &b);
    if (!chain)
        return;
    // A glyph wider than a page: U+4E00 in font B is 1.25 em.
    rbttext_atlas small = open_atlas(32.0f, 4.0f, 36, 8);
    if (!small)
        return;
    RBTTEXT_GLYPH_GENERATE_DESC wide = generate_desc(b, B_HAN_ONE, 32.0f, 4.0f);
    RBTTEXT_GLYPH_IMAGE image;
    uint64_t size = 0;
    uint8_t *pixels = generate(&wide, &image, &size);
    if (!pixels)
        return;
    RBTTEXT_ATLAS_GLYPH placed = empty_atlas_glyph();
    RBTTEXT_ERROR error;
    RBT_RESULT result = rbttext_atlas_insert(small, &wide, pixels, size, &placed, &error);
    if (!BUILT(result))
        return;
    CHECK(result == RBT_RESULT_LIMIT);

    // One page only: full sooner or later.
    rbttext_atlas one = open_atlas(32.0f, 4.0f, 48, 1);
    if (!one)
        return;
    bool limited = false;
    for (uint32_t g = 0; g < 9 && !limited; g++)
    {
        RBTTEXT_GLYPH_GENERATE_DESC desc = generate_desc(a, g, 32.0f, 4.0f);
        pixels = generate(&desc, &image, &size);
        if (!pixels)
            return;
        placed = empty_atlas_glyph();
        result = rbttext_atlas_insert(one, &desc, size ? pixels : NULL, size, &placed, &error);
        CHECK(result == RBT_RESULT_OK || result == RBT_RESULT_LIMIT);
        limited = result == RBT_RESULT_LIMIT;
    }
    CHECK(limited);
    RBTTEXT_ATLAS_INFO info;
    if (!read_atlas_info(one, &info))
        return;
    CHECK(info.page_count == 1);

    // Settings other than the atlas's, and pixels of the wrong size.
    RBTTEXT_GLYPH_GENERATE_DESC other = generate_desc(a, A_LOWER_A, 48.0f, 4.0f);
    pixels = generate(&other, &image, &size);
    if (!pixels)
        return;
    result = rbttext_atlas_insert(small, &other, pixels, size, NULL, &error);
    CHECK(result == RBT_RESULT_INVALID_ARGUMENT);
    RBTTEXT_GLYPH_GENERATE_DESC right = generate_desc(a, A_LOWER_A, 32.0f, 4.0f);
    pixels = generate(&right, &image, &size);
    if (!pixels)
        return;
    CHECK(rbttext_atlas_insert(small, &right, pixels, size - 4, NULL, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbttext_atlas_insert(small, &right, NULL, size, NULL, &error) == RBT_RESULT_INVALID_ARGUMENT);
    RBTTEXT_ATLAS_GLYPH wrong = empty_atlas_glyph();
    wrong.base.type = RBT_STRUCT_TYPE_TEXT_GLYPH_IMAGE;
    CHECK(rbttext_atlas_insert(small, &right, pixels, size, &wrong, &error) == RBT_RESULT_STRUCT_WRONG_TYPE);
    CHECK(rbttext_atlas_insert(small, &right, pixels, size, NULL, &error) == RBT_RESULT_OK);
}

static void test_atlas_add_layout(void)
{
    rbttext_chain chain = open_chain_ab(NULL, NULL);
    if (!chain)
        return;
    RBTTEXT_LAYOUT_DESC desc = layout_desc(chain, "fib azQ");
    rbttext_layout layout = lay_out(&desc);
    if (!layout)
        return;
    rbttext_atlas atlas = open_atlas(32.0f, 4.0f, 128, 16);
    if (!atlas)
        return;
    RBTTEXT_ERROR error;
    RBT_RESULT result = rbttext_atlas_add_layout(atlas, layout, &error);
    if (!BUILT(result))
        return;
    CHECK(result == RBT_RESULT_OK);
    LaidOut out;
    if (!read_layout(layout, &out))
        return;
    for (uint32_t i = 0; i < out.glyph_count; i++)
    {
        rbttext_face face = rbttext_chain_get_face(chain, out.glyphs[i].face_index);
        RBTTEXT_ATLAS_GLYPH found = empty_atlas_glyph();
        CHECK(rbttext_atlas_find_glyph(atlas, face, out.glyphs[i].glyph, &found) == RBT_RESULT_OK);
    }
    // Adding it again adds nothing.
    RBTTEXT_ATLAS_INFO before, after;
    if (!read_atlas_info(atlas, &before))
        return;
    CHECK(rbttext_atlas_add_layout(atlas, layout, &error) == RBT_RESULT_OK);
    if (!read_atlas_info(atlas, &after))
        return;
    CHECK(after.glyph_count == before.glyph_count);
}

static void test_atlas_save_load(void)
{
    rbttext_face a = NULL, b = NULL;
    rbttext_chain chain = open_chain_ab(&a, &b);
    if (!chain)
        return;
    RBTTEXT_LAYOUT_DESC layout_d = layout_desc(chain, "ab fi AV z");
    rbttext_layout layout = lay_out(&layout_d);
    if (!layout)
        return;
    RBTTEXT_ATLAS_DESC settings = atlas_desc(32.0f, 4.0f, 64, 16);
    rbttext_atlas atlas = NULL;
    RBTTEXT_ERROR error;
    RBT_RESULT result = rbttext_atlas_create(&settings, &atlas, &error);
    if (!BUILT(result))
        return;
    track_atlas(atlas);
    result = rbttext_atlas_add_layout(atlas, layout, &error);
    if (!BUILT(result))
        return;
    CHECK(result == RBT_RESULT_OK);

    rbttext_face faces[2] = {a, b};
    uint64_t size = 0;
    result = rbttext_atlas_save(atlas, faces, 2, NULL, 0, &size, &error);
    if (!BUILT(result))
        return;
    CHECK(result == RBT_RESULT_OK);
    CHECK(size > 0);
    uint8_t *saved = (uint8_t *)track_memory((size_t)size);
    uint8_t *saved_again = (uint8_t *)track_memory((size_t)size);
    if (!saved || !saved_again)
        return;
    uint64_t written = 0;
    CHECK(rbttext_atlas_save(atlas, faces, 2, saved, size - 1, &written, &error) == RBT_RESULT_LIMIT);
    CHECK(rbttext_atlas_save(atlas, faces, 2, saved, size, &written, &error) == RBT_RESULT_OK);
    CHECK(written == size);
    CHECK(rbttext_atlas_save(atlas, faces, 2, saved_again, size, &written, &error) == RBT_RESULT_OK);
    CHECK(memcmp(saved, saved_again, (size_t)size) == 0);
    // A face the atlas holds glyphs of must be listed.
    CHECK(rbttext_atlas_save(atlas, faces, 1, saved_again, size, &written, &error) == RBT_RESULT_INVALID_ARGUMENT);

    RBTTEXT_ATLAS_LOAD_DESC load;
    memset(&load, 0, sizeof(load));
    load.base.type = RBT_STRUCT_TYPE_TEXT_ATLAS_LOAD_DESC;
    load.atlas = &settings;
    load.data = saved;
    load.data_size = size;
    load.faces = faces;
    load.face_count = 2;
    rbttext_atlas loaded = NULL;
    result = rbttext_atlas_load(&load, &loaded, &error);
    if (!BUILT(result))
        return;
    CHECK(result == RBT_RESULT_OK);
    track_atlas(loaded);
    if (result != RBT_RESULT_OK)
        return;

    RBTTEXT_ATLAS_INFO original, copy;
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
        rbttext_face face = rbttext_chain_get_face(chain, out.glyphs[i].face_index);
        RBTTEXT_ATLAS_GLYPH x = empty_atlas_glyph(), y = empty_atlas_glyph();
        CHECK(rbttext_atlas_find_glyph(atlas, face, out.glyphs[i].glyph, &x) == RBT_RESULT_OK);
        CHECK(rbttext_atlas_find_glyph(loaded, face, out.glyphs[i].glyph, &y) == RBT_RESULT_OK);
        CHECK(x.page == y.page);
        CHECK(x.x == y.x);
        CHECK(x.y == y.y);
        CHECK(x.width == y.width);
        CHECK(x.plane_top == y.plane_top);
    }
    for (uint32_t p = 0; p < copy.page_count; p++)
    {
        const void *x = NULL, *y = NULL;
        CHECK(rbttext_atlas_get_page(atlas, p, &x) == RBT_RESULT_OK);
        CHECK(rbttext_atlas_get_page(loaded, p, &y) == RBT_RESULT_OK);
        if (x && y)
            CHECK(memcmp(x, y, (size_t)copy.page_width * copy.page_height * 4) == 0);
    }
    RBTTEXT_DIRTY_RECT rects[16];
    uint32_t count = 0;
    CHECK(rbttext_atlas_take_dirty(loaded, rects, 16, &count) == RBT_RESULT_OK);
    for (uint32_t i = 0; i < count; i++)
    {
        CHECK(rects[i].x == 0);
        CHECK(rects[i].y == 0);
        CHECK(rects[i].width == copy.page_width);
        CHECK(rects[i].height == copy.page_height);
    }

    // Other settings, other faces, fewer faces: a mismatch.
    rbttext_atlas refused = (rbttext_atlas)SENTINEL;
    RBTTEXT_ATLAS_DESC other_settings = atlas_desc(48.0f, 4.0f, 64, 16);
    RBTTEXT_ATLAS_LOAD_DESC bad = load;
    bad.atlas = &other_settings;
    CHECK(rbttext_atlas_load(&bad, &refused, &error) == RBT_RESULT_TEXT_ATLAS_MISMATCH);
    CHECK(refused == NULL);
    rbttext_face swapped[2] = {b, a};
    bad = load;
    bad.faces = swapped;
    CHECK(rbttext_atlas_load(&bad, &refused, &error) == RBT_RESULT_TEXT_ATLAS_MISMATCH);
    bad = load;
    bad.face_count = 1;
    CHECK(rbttext_atlas_load(&bad, &refused, &error) == RBT_RESULT_TEXT_ATLAS_MISMATCH);

    // Damaged bytes: corrupt.
    memcpy(saved_again, saved, (size_t)size);
    saved_again[size / 2] ^= 0x5A;
    bad = load;
    bad.data = saved_again;
    CHECK(rbttext_atlas_load(&bad, &refused, &error) == RBT_RESULT_CORRUPT_DATA);
    CHECK(refused == NULL);
    bad = load;
    bad.data_size = size - 1;
    refused = (rbttext_atlas)SENTINEL;
    CHECK(rbttext_atlas_load(&bad, &refused, &error) == RBT_RESULT_CORRUPT_DATA);
    CHECK(refused == NULL);
}

// ---- Running ------------------------------------------------------------------------

static int tests_run = 0;
static int tests_passed = 0;
static int tests_pending = 0;
static int tests_failed = 0;

// ---- CAD single-stroke formats and variable fonts (2026-10-30) ----------------

static const char *data_dir = NULL; // the tests' data folder, argv[1]

static RBTTEXT_STROKE_METRICS cad_metrics(void)
{
    RBTTEXT_STROKE_METRICS m;
    m.ascender = 1.0f;
    m.descender = -0.25f;
    m.line_gap = 0.0f;
    return m;
}

static rbttext_face lff_face(const char *text, RBT_RESULT *out_result)
{
    RBTTEXT_LFF_FACE_DESC d;
    memset(&d, 0, sizeof(d));
    d.base.type = RBT_STRUCT_TYPE_TEXT_LFF_FACE_DESC;
    d.data = text;
    d.data_size = strlen(text);
    d.units_per_em = 9.0f;
    d.arc_tolerance = 0.01f;
    d.metrics = cad_metrics();
    rbttext_face face = NULL;
    *out_result = rbttext_face_create_lff(&d, &face, NULL);
    if (face)
        track_face(face);
    return face;
}

static bool strokes_of(rbttext_face face, uint32_t codepoint, const RbtVec2 **points, uint32_t *point_count,
                       const RBTTEXT_STROKE_POLYLINE **polylines, uint32_t *polyline_count)
{
    uint32_t glyph = 0;
    if (rbttext_face_find_glyph(face, codepoint, &glyph) != RBT_RESULT_OK)
        return false;
    return rbttext_face_get_glyph_strokes(face, glyph, points, point_count, polylines, polyline_count) == RBT_RESULT_OK;
}

// LFF: an I, an O-like half circle by a bulge, an L referring to the I, the
// space LibreCAD's fonts leave out; then every refusal.
static void test_lff_face(void)
{
    const char *text = "# Format: LibreCAD Font 1\n"
                       "# LetterSpacing: 2\n"
                       "# WordSpacing: 5\n"
                       "\n"
                       "[0049] I\n"
                       "0,0;0,9\n"
                       "\n"
                       "[004F] O\n"
                       "4,0;4,8,A1\n"
                       "\n"
                       "[004C] L\n"
                       "C0049\n"
                       "0,0;5,0\n";
    RBT_RESULT result;
    rbttext_face face = lff_face(text, &result);
    CHECK(result == RBT_RESULT_OK);
    if (!face)
        return;
    const RbtVec2 *points = NULL;
    const RBTTEXT_STROKE_POLYLINE *lines = NULL;
    uint32_t point_count = 0, line_count = 0;
    CHECK(strokes_of(face, 0x49, &points, &point_count, &lines, &line_count));
    CHECK(point_count == 2 && line_count == 1);
    if (point_count == 2)
        CHECK(points[0].y == 0.0f && points[1].y == -1.0f); // 9 up, y down in em
    // The arc: every point on the circle round (4, 4), radius 4, to the
    // right of the chord (anticlockwise from the bottom), and no chord's
    // middle further inside than the tolerance.
    CHECK(strokes_of(face, 0x4F, &points, &point_count, &lines, &line_count));
    CHECK(line_count == 1 && point_count > 8);
    float rightmost = 0.0f;
    for (uint32_t i = 0; i < point_count; i++)
    {
        const float x = points[i].x * 9.0f - 4.0f, y = -points[i].y * 9.0f - 4.0f;
        CHECK(fabsf(sqrtf(x * x + y * y) - 4.0f) < 1e-4f);
        CHECK(x >= -1e-4f);
        if (points[i].x > rightmost)
            rightmost = points[i].x;
        if (i > 0)
        {
            const float mx = (points[i].x + points[i - 1].x) * 4.5f - 4.0f;
            const float my = -(points[i].y + points[i - 1].y) * 4.5f - 4.0f;
            CHECK(4.0f - sqrtf(mx * mx + my * my) <= 0.01f * 9.0f + 1e-4f);
        }
    }
    CHECK(fabsf(rightmost - 8.0f / 9.0f) < 1e-4f);
    // L: the I's stroke copied, then its foot.
    CHECK(strokes_of(face, 0x4C, &points, &point_count, &lines, &line_count));
    CHECK(line_count == 2 && point_count == 4);
    // Advances: the rightmost x plus the letter spacing; the space, the
    // word spacing.
    rbttext_chain chain = open_chain(&face, 1);
    if (chain)
    {
        RBTTEXT_LAYOUT_DESC ld = layout_desc(chain, "I O");
        rbttext_layout layout = lay_out(&ld);
        LaidOut out;
        if (layout && read_layout(layout, &out) && out.glyph_count == 3)
        {
            CHECK(fabsf(out.glyphs[1].x - 2.0f / 9.0f) < 1e-6f);
            CHECK(fabsf(out.glyphs[2].x - 7.0f / 9.0f) < 1e-6f);
        }
        else
            CHECK(!"the LFF face lays out");
    }
    // The same text, the same face, bit for bit.
    rbttext_face again = lff_face(text, &result);
    const RbtVec2 *points2 = NULL;
    uint32_t point_count2 = 0;
    CHECK(again && strokes_of(face, 0x4F, &points, &point_count, &lines, &line_count) &&
          strokes_of(again, 0x4F, &points2, &point_count2, &lines, &line_count) && point_count == point_count2 &&
          memcmp(points, points2, sizeof(RbtVec2) * point_count) == 0);

    const char *bad[] = {"[0049] I\n0,0;0,9\n\n[0049] I\n0,0;1,1\n", // twice
                         "[0049] I\nC0041\n",                        // a reference to nothing
                         "[0049] I\n0,0;0\n",                         // not x,y
                         "[0049] I\n0,0,B2;1,1\n",                    // not Ab
                         "0,0;1,1\n",                                 // strokes outside a glyph
                         "[ZZ] I\n",                                  // not hex
                         "# only a header\n"};                        // no glyph
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
    {
        rbttext_face refused = lff_face(bad[i], &result);
        CHECK(result == RBT_RESULT_TEXT_NOT_A_FONT && refused == NULL);
    }
    RBTTEXT_LFF_FACE_DESC d;
    memset(&d, 0, sizeof(d));
    d.base.type = RBT_STRUCT_TYPE_TEXT_LFF_FACE_DESC;
    d.data = text;
    d.data_size = strlen(text);
    d.units_per_em = 9.0f;
    d.metrics = cad_metrics();
    rbttext_face refused = NULL;
    CHECK(rbttext_face_create_lff(&d, &refused, NULL) == RBT_RESULT_INVALID_ARGUMENT); // no tolerance
    d.arc_tolerance = 0.01f;
    d.units_per_em = 0.0f;
    CHECK(rbttext_face_create_lff(&d, &refused, NULL) == RBT_RESULT_INVALID_ARGUMENT);
}

// An SHX file built here.
typedef struct ShxFile
{
    uint8_t bytes[1024];
    uint32_t size;
} ShxFile;

static void shx_put(ShxFile *f, const void *data, uint32_t size)
{
    memcpy(f->bytes + f->size, data, size);
    f->size += size;
}

static void shx_u16(ShxFile *f, uint32_t v)
{
    uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    shx_put(f, b, 2);
}

// A shapes 1.0 file of [count] shapes: numbers, names and commands.
static void shx_shapes(ShxFile *f, uint32_t count, const uint32_t *numbers, const char *const *names,
                       const uint8_t *const *specs, const uint32_t *spec_sizes)
{
    f->size = 0;
    shx_put(f, "AutoCAD-86 shapes 1.0\r\n\x1a", 24);
    shx_u16(f, 0);
    shx_u16(f, 255);
    shx_u16(f, count);
    for (uint32_t i = 0; i < count; i++)
    {
        shx_u16(f, numbers[i]);
        shx_u16(f, (uint32_t)strlen(names[i]) + 1 + spec_sizes[i]);
    }
    for (uint32_t i = 0; i < count; i++)
    {
        shx_put(f, names[i], (uint32_t)strlen(names[i]) + 1);
        shx_put(f, specs[i], spec_sizes[i]);
    }
}

static rbttext_face shx_face(const ShxFile *f, const uint32_t *high, uint32_t high_count, RBT_RESULT *out_result)
{
    RBTTEXT_SHX_FACE_DESC d;
    memset(&d, 0, sizeof(d));
    d.base.type = RBT_STRUCT_TYPE_TEXT_SHX_FACE_DESC;
    d.data = f->bytes;
    d.data_size = f->size;
    d.high_codepoints = high;
    d.high_codepoint_count = high_count;
    d.arc_tolerance = 0.001f;
    d.line_gap = 0.2f;
    rbttext_face face = NULL;
    *out_result = rbttext_face_create_shx(&d, &face, NULL);
    if (face)
        track_face(face);
    return face;
}

static bool near2(RbtVec2 p, float x, float y)
{
    return fabsf(p.x - x) < 1e-4f && fabsf(p.y - y) < 1e-4f;
}

// SHX: vectors, pen up and down, an octant circle, a subshape, push and
// pop, scale, a bulge arc, the vertical skip, a displacement list, a
// fractional arc, a high shape through the caller's code page, a unifont;
// then refusals. "above" is 10, so a unit is a tenth of an em.
static void test_shx_face(void)
{
    static const uint8_t info[] = {10, 2, 0, 0};
    static const uint8_t shape_i[] = {0xA4, 2, 0x50, 0};                     // up 10, pen up, east 5
    static const uint8_t shape_o[] = {2, 8, 10, 5, 1, 10, 5, 0x00, 2, 8, 2, 0xFB, 0}; // a circle round (5, 5), to (12, 0)
    static const uint8_t shape_s[] = {5, 7, 0x49, 6, 1, 4, 2, 0x20, 12, 0, 4, 127, 14, 0x40, 9, 1, 0, 0, 1, 0, 0, 0};
    static const uint8_t shape_c[] = {2, 8, 10, 0, 1, 11, 0, 128, 0, 10, 0x02, 0}; // radius 10 from 0 to 67.5 degrees
    static const uint8_t shape_e[] = {0x34, 0};                              // shape 128: up 3
    const uint32_t numbers[] = {0, 0x49, 0x4F, 0x53, 0x43, 128};
    const char *names[] = {"test", "I", "O", "S", "C", "E"};
    const uint8_t *specs[] = {info, shape_i, shape_o, shape_s, shape_c, shape_e};
    const uint32_t sizes[] = {sizeof(info), sizeof(shape_i), sizeof(shape_o), sizeof(shape_s), sizeof(shape_c), sizeof(shape_e)};
    ShxFile f;
    shx_shapes(&f, 6, numbers, names, specs, sizes);
    const uint32_t high[] = {0xE9};
    RBT_RESULT result;
    rbttext_face face = shx_face(&f, high, 1, &result);
    CHECK(result == RBT_RESULT_OK);
    if (!face)
        return;
    RBTTEXT_FACE_METRICS m;
    memset(&m, 0, sizeof(m));
    m.base.type = RBT_STRUCT_TYPE_TEXT_FACE_METRICS;
    CHECK(rbttext_face_get_metrics(face, &m) == RBT_RESULT_OK && m.ascender == 1.0f && fabsf(m.descender + 0.2f) < 1e-6f);
    const RbtVec2 *p = NULL;
    const RBTTEXT_STROKE_POLYLINE *lines = NULL;
    uint32_t n = 0, ln = 0;
    CHECK(strokes_of(face, 0x49, &p, &n, &lines, &ln) && n == 2 && ln == 1 && near2(p[0], 0, 0) && near2(p[1], 0, -1));
    // O: a closed circle of radius 0.5 em round (0.5, 0.5) up.
    CHECK(strokes_of(face, 0x4F, &p, &n, &lines, &ln) && ln == 1 && n > 16);
    for (uint32_t i = 0; i < n; i++)
    {
        const float x = p[i].x - 0.5f, y = -p[i].y - 0.5f;
        CHECK(fabsf(sqrtf(x * x + y * y) - 0.5f) < 1e-4f);
    }
    CHECK(n > 1 && near2(p[0], 1.0f, -0.5f) && near2(p[n - 1], 1.0f, -0.5f));
    // S: the I's stroke (the subshape), then from (0, 0) after the pop: two
    // units scaled by 2 east, a half circle to (4, 8) bulging east, the
    // skipped north vector, then (6, 8) and (6, 10).
    CHECK(strokes_of(face, 0x53, &p, &n, &lines, &ln) && ln == 2);
    if (ln == 2 && n > 6)
    {
        CHECK(near2(p[0], 0, 0) && near2(p[1], 0, -1));
        CHECK(near2(p[2], 0, 0) && near2(p[3], 0.4f, 0));
        CHECK(near2(p[n - 3], 0.4f, -0.8f) && near2(p[n - 2], 0.6f, -0.8f) && near2(p[n - 1], 0.6f, -1.0f));
        float east = 0.0f;
        for (uint32_t i = 3; i < n - 2; i++)
            east = fmaxf(east, p[i].x);
        CHECK(fabsf(east - 0.8f) < 1e-3f); // the half circle reaches x 8
    }
    // C: from (10, 0) round the origin to 67.5 degrees.
    CHECK(strokes_of(face, 0x43, &p, &n, &lines, &ln) && ln == 1 && n > 2);
    if (n > 2)
    {
        CHECK(near2(p[0], 1.0f, 0.0f));
        CHECK(near2(p[n - 1], 0.38268343f, -0.92387953f));
    }
    // Shape 128, through the caller's code page.
    CHECK(strokes_of(face, 0xE9, &p, &n, &lines, &ln) && n == 2);
    // Advances: where the pen ends.
    uint32_t glyph = 0;
    CHECK(rbttext_face_find_glyph(face, 0x4F, &glyph) == RBT_RESULT_OK);

    // A unifont: a shape drawing another by its two-byte number.
    {
        ShxFile u;
        u.size = 0;
        shx_put(&u, "AutoCAD-86 unifont 1.0\r\n\x1a", 25);
        const uint8_t count[4] = {3, 0, 0, 0};
        shx_put(&u, count, 4);
        static const uint8_t uinfo[] = {'u', 0, 10, 2, 0, 0, 0, 0};
        shx_u16(&u, sizeof(uinfo));
        shx_put(&u, uinfo, sizeof(uinfo));
        static const uint8_t a[] = {'A', 0, 0x14, 0};                 // U+0041: up 1
        static const uint8_t cyr[] = {0, 7, 0x00, 0x41, 0x10, 0};      // U+0410: the A, then east 1
        shx_u16(&u, 0x41);
        shx_u16(&u, sizeof(a));
        shx_put(&u, a, sizeof(a));
        shx_u16(&u, 0x410);
        shx_u16(&u, sizeof(cyr));
        shx_put(&u, cyr, sizeof(cyr));
        rbttext_face uface = shx_face(&u, NULL, 0, &result);
        CHECK(result == RBT_RESULT_OK);
        CHECK(uface && strokes_of(uface, 0x410, &p, &n, &lines, &ln) && n == 3 && near2(p[2], 0.1f, -0.1f));
    }

    // Refusals.
    ShxFile bad = f;
    memcpy(bad.bytes, "AutoCAD-86 bigfont 1.0\r\n\x1a", 25);
    rbttext_face refused = shx_face(&bad, NULL, 0, &result);
    CHECK(result == RBT_RESULT_UNSUPPORTED && refused == NULL);
    bad = f;
    bad.bytes[0] = 'X';
    refused = shx_face(&bad, NULL, 0, &result);
    CHECK(result == RBT_RESULT_TEXT_NOT_A_FONT);
    static const uint8_t unknown[] = {15, 0};
    static const uint8_t loop[] = {7, 0x52, 0};
    static const uint8_t pushes[] = {5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 0};
    static const uint8_t unended[] = {0x14};
    const uint8_t *refusing[] = {unknown, loop, pushes, unended};
    const uint32_t refusing_sizes[] = {sizeof(unknown), sizeof(loop), sizeof(pushes), sizeof(unended)};
    for (int i = 0; i < 4; i++)
    {
        const uint32_t nums[] = {0, 0x52};
        const char *nm[] = {"t", "R"};
        const uint8_t *sp[] = {info, refusing[i]};
        const uint32_t sz[] = {sizeof(info), refusing_sizes[i]};
        shx_shapes(&bad, 2, nums, nm, sp, sz);
        refused = shx_face(&bad, NULL, 0, &result);
        CHECK(result == RBT_RESULT_TEXT_NOT_A_FONT && refused == NULL);
    }
    RBTTEXT_SHX_FACE_DESC d;
    memset(&d, 0, sizeof(d));
    d.base.type = RBT_STRUCT_TYPE_TEXT_SHX_FACE_DESC;
    d.data = f.bytes;
    d.data_size = f.size;
    d.line_gap = 0.0f;
    CHECK(rbttext_face_create_shx(&d, &refused, NULL) == RBT_RESULT_INVALID_ARGUMENT); // no tolerance
}

static uint8_t *read_data(const char *name, size_t *out_size)
{
    if (!data_dir)
        return NULL;
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", data_dir, name);
    FILE *file = fopen(path, "rb");
    if (!file)
        return NULL;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)malloc((size_t)size);
    if (data && fread(data, 1, (size_t)size, file) != (size_t)size)
    {
        free(data);
        data = NULL;
    }
    fclose(file);
    *out_size = (size_t)size;
    return data;
}

// Variable fonts: Roboto's a, b and c. Its axes are read; at its heaviest
// weight 'a' advances by another width and draws other pixels than at the
// default; an
// unknown axis, a value out of range, an axis twice and NaN are refused.
static void test_variable_face(void)
{
    size_t size = 0;
    uint8_t *font = read_data("rbttext_variable_abc.ttf", &size);
    CHECK(font != NULL);
    if (!font)
        return;
    RBTTEXT_FACE_DESC fd;
    memset(&fd, 0, sizeof(fd));
    fd.base.type = RBT_STRUCT_TYPE_TEXT_FACE_DESC;
    fd.data = font;
    fd.data_size = size;
    fd.copy_data = true;
    rbttext_face plain = NULL, bold = NULL, refused = NULL;
    CHECK(rbttext_face_create(&fd, &plain, NULL) == RBT_RESULT_OK);
    const uint32_t wght = 0x77676874u;
    uint32_t axes = rbttext_face_get_axis_count(plain);
    CHECK(axes >= 1);
    RBTTEXT_AXIS weight;
    memset(&weight, 0, sizeof(weight));
    bool found = false;
    for (uint32_t i = 0; i < axes; i++)
    {
        RBTTEXT_AXIS axis;
        CHECK(rbttext_face_get_axis(plain, i, &axis) == RBT_RESULT_OK);
        if (axis.tag == wght)
        {
            weight = axis;
            found = true;
        }
    }
    CHECK(found && weight.min_value < weight.default_value && weight.default_value < weight.max_value);
    RBTTEXT_AXIS none;
    CHECK(rbttext_face_get_axis(plain, axes, &none) == RBT_RESULT_NOT_FOUND);
    CHECK(rbttext_face_get_axis_count(NULL) == 0);

    RBTTEXT_VARIATION v[2] = {{wght, weight.max_value}, {wght, weight.min_value}};
    fd.variations = v;
    fd.variation_count = 1;
    CHECK(rbttext_face_create(&fd, &bold, NULL) == RBT_RESULT_OK);
    if (plain && bold)
    {
        track_face(plain);
        track_face(bold);
        // Advances, laid out at one em.
        float advances[2] = {0, 0};
        rbttext_face faces[2] = {plain, bold};
        for (int i = 0; i < 2; i++)
        {
            rbttext_chain chain = open_chain(&faces[i], 1);
            RBTTEXT_LAYOUT_DESC ld = layout_desc(chain, "a");
            rbttext_layout layout = chain ? lay_out(&ld) : NULL;
            LaidOut out;
            if (layout && read_layout(layout, &out) && out.glyph_count == 1)
                advances[i] = out.glyphs[0].advance;
        }
        // Roboto's bold 'a' is a little narrower than its default (0.531 em
        // against 0.544): it moves, which way is the font's.
        CHECK(advances[0] > 0.0f && advances[1] > 0.0f && advances[1] != advances[0]);
        // Pixels.
        uint32_t glyph = 0;
        CHECK(rbttext_face_find_glyph(plain, 0x61, &glyph) == RBT_RESULT_OK);
        uint8_t *images[2] = {NULL, NULL};
        uint32_t sizes[2] = {0, 0};
        for (int i = 0; i < 2; i++)
        {
            RBTTEXT_GLYPH_GENERATE_DESC gd;
            memset(&gd, 0, sizeof(gd));
            gd.base.type = RBT_STRUCT_TYPE_TEXT_GLYPH_GENERATE_DESC;
            gd.face = faces[i];
            gd.glyph = glyph;
            gd.pixels_per_em = 32.0f;
            gd.distance_range = 4.0f;
            RBTTEXT_GLYPH_IMAGE image;
            memset(&image, 0, sizeof(image));
            image.base.type = RBT_STRUCT_TYPE_TEXT_GLYPH_IMAGE;
            if (rbttext_generate_glyph(&gd, NULL, 0, &image, NULL) == RBT_RESULT_OK)
            {
                sizes[i] = image.width * image.height * 4;
                images[i] = (uint8_t *)malloc(sizes[i]);
                CHECK(rbttext_generate_glyph(&gd, images[i], sizes[i], &image, NULL) == RBT_RESULT_OK);
            }
        }
        CHECK(images[0] && images[1] && (sizes[0] != sizes[1] || memcmp(images[0], images[1], sizes[0]) != 0));
        free(images[0]);
        free(images[1]);
    }
    else
    {
        rbttext_face_destroy(plain);
        rbttext_face_destroy(bold);
    }
    fd.variation_count = 2; // one axis twice
    CHECK(rbttext_face_create(&fd, &refused, NULL) == RBT_RESULT_INVALID_ARGUMENT && refused == NULL);
    RBTTEXT_VARIATION out_of_range = {wght, weight.max_value + 1.0f};
    RBTTEXT_VARIATION unknown = {0x7A7A7A7Au, 1.0f};
    RBTTEXT_VARIATION nan = {wght, NAN};
    const RBTTEXT_VARIATION *bad[3] = {&out_of_range, &unknown, &nan};
    for (int i = 0; i < 3; i++)
    {
        fd.variations = bad[i];
        fd.variation_count = 1;
        CHECK(rbttext_face_create(&fd, &refused, NULL) == RBT_RESULT_INVALID_ARGUMENT && refused == NULL);
    }
    fd.variations = NULL;
    CHECK(rbttext_face_create(&fd, &refused, NULL) == RBT_RESULT_INVALID_ARGUMENT);
    free(font);
}


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
        bool ok = write_file(argv[2], "rbttext_test_a.ttf", &font_a);
        ok = write_file(argv[2], "rbttext_test_b.ttf", &font_b) && ok;
        ok = write_file(argv[2], "rbttext_test_ab.ttc", &font_ab_collection) && ok;
        printf(ok ? "rbttext_tests: fonts written to %s\n" : "rbttext_tests: couldn't write fonts to %s\n", argv[2]);
        return ok ? 0 : 1;
    }

    if (argc == 2)
        data_dir = argv[1];

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
    RUN(test_lff_face);
    RUN(test_shx_face);
    RUN(test_variable_face);

    // The atlas.
    RUN(test_distance_field);
    RUN(test_atlas_packing);
    RUN(test_atlas_dirty);
    RUN(test_atlas_limits);
    RUN(test_atlas_add_layout);
    RUN(test_atlas_save_load);

    printf("rbttext_tests: %d tests, %d passed, %d pending (not implemented yet), %d failed\n", tests_run,
           tests_passed, tests_pending, tests_failed);
    if (failures)
    {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
