#include "rbttext_internal.hpp"
#include "../rbtstructbase.hpp"
#include <SheenBidi/SheenBidi.h>
#include <linebreak.h>
#include <math.h>
#include <new>
#include <stdlib.h>
#include <string.h>
#include "../rbtoverlap.hpp"

// Rabbittus Text's layouts (RBTTEXT_TODO.md, item 2). A build runs, for each
// paragraph (the text between mandatory breaks):
//   1. Unicode's bidirectional algorithm (SheenBidi), for each codepoint's
//      embedding level;
//   2. script runs (SheenBidi's script locator, Unicode's Script property
//      with common and inherited characters resolved to their neighbours');
//   3. the chain face each codepoint is drawn from;
//   4. shaping the paragraph in pieces of one face, script and level, only
//      to measure its clusters;
//   5. breaking it into lines at Unicode's line break opportunities
//      (libunibreak), greedily, with trailing spaces hanging;
//   6. for each line, its runs in visual order (SheenBidi's line, which
//      applies rules L1 and L2), each split where the face or script changes
//      and shaped again, so shaping at a line's ends sees the line's text;
//   7. placing the glyphs along the line.
// Then every line is aligned within the layout's width.
//
// All arithmetic is in doubles, from integer font units, with
// floating-point contraction off (CMakeLists.txt): the same text gives the
// same bits on every platform.

// ---- Characters -------------------------------------------------------------------

static bool is_mandatory_break(uint32_t c)
{
    if (c == 0x0A)
        return true;
    if (c == 0x0D)
        return true;
    if (c == 0x85)
        return true;
    return c == 0x2028 || c == 0x2029;
}

// The spaces that hang at a line's end: Unicode's space separators that
// break, and the tab.
static bool is_hanging_space(uint32_t c)
{
    if (c == 0x20 || c == 0x09 || c == 0x1680 || c == 0x205F || c == 0x3000)
        return true;
    if (c >= 0x2000 && c <= 0x2006)
        return true;
    return c >= 0x2008 && c <= 0x200A;
}

// Unicode's Default_Ignorable_Code_Point (DerivedCoreProperties.txt):
// format controls, variation selectors and the like, which give no glyph
// where a face has none.
static bool is_default_ignorable(uint32_t c)
{
    if (c < 0xAD)
        return false;
    if (c == 0xAD || c == 0x34F || c == 0x61C || c == 0x3164 || c == 0xFEFF || c == 0xFFA0)
        return true;
    if (c >= 0x115F && c <= 0x1160)
        return true;
    if (c >= 0x17B4 && c <= 0x17B5)
        return true;
    if (c >= 0x180B && c <= 0x180F)
        return true;
    if (c >= 0x200B && c <= 0x200F)
        return true;
    if (c >= 0x202A && c <= 0x202E)
        return true;
    if (c >= 0x2060 && c <= 0x206F)
        return true;
    if (c >= 0xFE00 && c <= 0xFE0F)
        return true;
    if (c >= 0xFFF0 && c <= 0xFFF8)
        return true;
    if (c >= 0x1BCA0 && c <= 0x1BCA3)
        return true;
    if (c >= 0x1D173 && c <= 0x1D17A)
        return true;
    return c >= 0xE0000 && c <= 0xE0FFF;
}

static bool is_mark(uint32_t c)
{
    hb_unicode_general_category_t category = hb_unicode_general_category(hb_unicode_funcs_get_default(), c);
    if (category == HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK)
        return true;
    if (category == HB_UNICODE_GENERAL_CATEGORY_SPACING_MARK)
        return true;
    return category == HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK;
}

// ---- Decoding -----------------------------------------------------------------------

// Decodes UTF-8 into the layout's codepoints and byte offsets: an
// ill-formed sequence is one U+FFFD per maximal subpart (Unicode's
// recommended practice, section 3.9).
static EQ_RESULT decode(rbttext_layout_t *layout, const uint8_t *text, uint32_t size)
{
    layout->codepoints.clear();
    layout->byte_offsets.clear();
    if (EQ_RESULT result = layout->codepoints.reserve(size))
        return result;
    if (EQ_RESULT result = layout->byte_offsets.reserve((size_t)size + 1))
        return result;
    uint32_t at = 0;
    while (at < size)
    {
        uint32_t b0 = text[at];
        uint32_t need = 0;
        uint32_t codepoint = 0;
        uint8_t low = 0x80;
        uint8_t high = 0xBF;
        if (b0 < 0x80)
            codepoint = b0;
        else if (b0 >= 0xC2 && b0 <= 0xDF)
        {
            need = 1;
            codepoint = b0 & 0x1F;
        }
        else if (b0 >= 0xE0 && b0 <= 0xEF)
        {
            need = 2;
            codepoint = b0 & 0x0F;
            if (b0 == 0xE0)
                low = 0xA0;
            if (b0 == 0xED)
                high = 0x9F;
        }
        else if (b0 >= 0xF0 && b0 <= 0xF4)
        {
            need = 3;
            codepoint = b0 & 0x07;
            if (b0 == 0xF0)
                low = 0x90;
            if (b0 == 0xF4)
                high = 0x8F;
        }
        else
            need = UINT32_MAX; // a byte that starts nothing
        // The lead byte and the continuation bytes that fit: the whole
        // sequence, or its maximal subpart.
        uint32_t length = 1;
        bool valid = need != UINT32_MAX;
        for (uint32_t k = 1; valid && k <= need; k++)
        {
            if (at + k >= size)
            {
                valid = false;
                break;
            }
            uint8_t b = text[at + k];
            if (b < low || b > high)
            {
                valid = false;
                break;
            }
            codepoint = (codepoint << 6) | (b & 0x3F);
            low = 0x80;
            high = 0xBF;
            length = k + 1;
        }
        if (!valid)
        {
            codepoint = 0xFFFD;
            layout->replaced_sequence_count++;
        }
        (void)layout->codepoints.push(codepoint);
        (void)layout->byte_offsets.push(at);
        at += length;
    }
    (void)layout->byte_offsets.push(size);
    return EQ_SUCCESS;
}

// ---- Shaping --------------------------------------------------------------------------

// What every paragraph of one build shares.
struct build_context
{
    rbttext_layout_t *layout;
    const RBTTEXT_LAYOUT_DESC *desc;
    rbttext_chain_t *chain;
    hb_language_t language;
    double em_size;
    double letter_spacing;
    double line_advance;
    double baseline_offset; // from a line's top to its baseline
    RBTTEXT_ERROR *error;
};

// Shapes codepoints [s0, s1) of the paragraph [p0, p1) with one face, into
// the layout's shaped glyphs, in visual order, each given [run].
static RBT_RESULT shape_piece(build_context &c, uint32_t p0, uint32_t p1, uint32_t s0, uint32_t s1, uint32_t face_index,
                            uint32_t script, bool rtl, uint32_t run)
{
    rbttext_layout_t *layout = c.layout;
    rbttext_face_t *face = c.chain->faces[face_index];
    const size_t first = layout->shaped.count;
    if (face->kind == RBTTEXT_FACE_KIND_OUTLINE)
    {
        hb_buffer_t *buffer = layout->buffer;
        hb_buffer_clear_contents(buffer);
        hb_buffer_set_direction(buffer, rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
        hb_buffer_set_script(buffer, hb_script_from_iso15924_tag(script));
        hb_buffer_set_language(buffer, c.language);
        unsigned int flags = HB_BUFFER_FLAG_REMOVE_DEFAULT_IGNORABLES;
        if (s0 == p0)
            flags |= HB_BUFFER_FLAG_BOT;
        if (s1 == p1)
            flags |= HB_BUFFER_FLAG_EOT;
        hb_buffer_set_flags(buffer, (hb_buffer_flags_t)flags);
        hb_buffer_set_cluster_level(buffer, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_GRAPHEMES);
        hb_buffer_add_codepoints(buffer, layout->shaping_codepoints.data + p0, (int)(p1 - p0), s0 - p0,
                                 (int)(s1 - s0));
        if (!hb_buffer_allocation_successful(buffer))
            return rbttext_out_of_memory(c.error, "shaping");
        hb_shape(face->font, buffer, layout->features.data, (unsigned int)layout->features.count);
        if (!hb_buffer_allocation_successful(buffer))
            return rbttext_out_of_memory(c.error, "shaping");
        unsigned int count = 0;
        const hb_glyph_info_t *infos = hb_buffer_get_glyph_infos(buffer, &count);
        const hb_glyph_position_t *positions = hb_buffer_get_glyph_positions(buffer, &count);
        const double units = (double)face->metrics.units_per_em;
        if (layout->shaped.reserve(first + count) != EQ_SUCCESS)
            return rbttext_out_of_memory(c.error, "shaping");
        for (unsigned int i = 0; i < count; i++)
        {
            rbttext_shaped s;
            // A damaged font's substitutions can name a glyph past its
            // last, which HarfBuzz passes on: a layout never does.
            s.glyph = infos[i].codepoint < face->metrics.glyph_count ? infos[i].codepoint : 0;
            s.cluster = infos[i].cluster + p0;
            s.cluster_end = s1;
            s.run = run;
            s.advance = (double)positions[i].x_advance / units;
            s.x_offset = (double)positions[i].x_offset / units;
            s.y_offset = (double)positions[i].y_offset / units;
            (void)layout->shaped.push(s);
        }
    }
    else
    {
        // A stroke face has no shaping rules: a glyph per codepoint, by its
        // character map.
        if (layout->shaped.reserve(first + (s1 - s0)) != EQ_SUCCESS)
            return rbttext_out_of_memory(c.error, "laying out");
        for (uint32_t i = s0; i < s1; i++)
        {
            uint32_t glyph = 0;
            bool found = rbttext_face_lookup(face, layout->shaping_codepoints[i], &glyph);
            if (!found && is_default_ignorable(layout->codepoints[i]))
                continue;
            rbttext_shaped s;
            s.glyph = glyph;
            s.cluster = i;
            s.cluster_end = s1;
            s.run = run;
            s.advance = glyph ? (double)face->glyphs[glyph - 1].advance : (double)RBTTEXT_STROKE_NOTDEF_ADVANCE;
            s.x_offset = 0.0;
            s.y_offset = 0.0;
            (void)layout->shaped.push(s);
        }
        if (rtl)
        {
            rbttext_shaped *begin = layout->shaped.data + first;
            rbttext_shaped *end = layout->shaped.data + layout->shaped.count;
            for (; begin + 1 < end; begin++, end--)
            {
                rbttext_shaped swap = *begin;
                *begin = end[-1];
                end[-1] = swap;
            }
        }
    }
    // Where each glyph's cluster ends: the next cluster in logical order,
    // which is to the right in a left-to-right piece and to the left in a
    // right-to-left one; the piece's end for the last.
    rbttext_shaped *glyphs = layout->shaped.data + first;
    const size_t count = layout->shaped.count - first;
    uint32_t next = s1;
    if (!rtl)
    {
        for (size_t i = count; i-- > 0;)
        {
            if (i + 1 < count && glyphs[i + 1].cluster != glyphs[i].cluster)
                next = glyphs[i + 1].cluster;
            glyphs[i].cluster_end = next;
        }
    }
    else
    {
        for (size_t i = 0; i < count; i++)
        {
            if (i > 0 && glyphs[i - 1].cluster != glyphs[i].cluster)
                next = glyphs[i - 1].cluster;
            glyphs[i].cluster_end = next;
        }
    }
    return RBT_RESULT_OK;
}

// ---- Paragraphs ---------------------------------------------------------------------

// Releases SheenBidi's objects when a paragraph is done.
struct bidi_objects
{
    SBAlgorithmRef algorithm = nullptr;
    SBParagraphRef paragraph = nullptr;
    SBLineRef line = nullptr;

    ~bidi_objects()
    {
        if (line)
            SBLineRelease(line);
        if (paragraph)
            SBParagraphRelease(paragraph);
        if (algorithm)
            SBAlgorithmRelease(algorithm);
    }
};

// Adds a line to the layout, its glyphs and box not yet aligned: the box
// spans the glyphs left of [content_left] and right of [content_right] out.
static RBT_RESULT add_line(build_context &c, uint32_t text_start, uint32_t text_end, uint32_t first_glyph,
                         uint32_t first_run, double content_left, double content_right, bool rtl)
{
    rbttext_layout_t *layout = c.layout;
    const uint32_t index = (uint32_t)layout->lines.count;
    RBTTEXT_LINE line;
    line.first_glyph = first_glyph;
    line.glyph_count = (uint32_t)layout->glyphs.count - first_glyph;
    line.first_run = first_run;
    line.run_count = (uint32_t)layout->runs.count - first_run;
    line.text_offset = layout->byte_offsets[text_start];
    line.text_size = layout->byte_offsets[text_end] - line.text_offset;
    const double top = (double)index * c.line_advance;
    line.baseline = (float)(top + c.baseline_offset);
    line.box_left = (float)content_left;
    line.box_top = (float)top;
    line.box_right = (float)content_right;
    line.box_bottom = (float)(top + c.line_advance);
    if (layout->lines.push(line) != EQ_SUCCESS)
        return rbttext_out_of_memory(c.error, "laying out");
    if (layout->line_rtl.push(rtl ? 1 : 0) != EQ_SUCCESS)
        return rbttext_out_of_memory(c.error, "laying out");
    return RBT_RESULT_OK;
}

// Places a paragraph's line [l0, l1), whose text runs on to [text_end]
// (its break, on the paragraph's last line), and whose trailing spaces
// start at [trailing].
static RBT_RESULT place_line(build_context &c, bidi_objects &bidi, uint32_t p0, uint32_t p1, uint32_t l0, uint32_t l1,
                           uint32_t text_end, uint32_t trailing, bool rtl)
{
    rbttext_layout_t *layout = c.layout;
    const uint32_t first_glyph = (uint32_t)layout->glyphs.count;
    const uint32_t first_run = (uint32_t)layout->runs.count;
    const uint32_t line_index = (uint32_t)layout->lines.count;
    const double baseline = (double)line_index * c.line_advance + c.baseline_offset;

    // A paragraph with no bidi pass (all level 0, see lay_out_paragraph)
    // is one run a line.
    SBRun whole;
    whole.offset = l0 - p0;
    whole.length = l1 - l0;
    whole.level = 0;
    SBUInteger run_count = 1;
    const SBRun *bidi_runs = &whole;
    if (bidi.paragraph)
    {
        if (bidi.line)
        {
            SBLineRelease(bidi.line);
            bidi.line = nullptr;
        }
        bidi.line = SBParagraphCreateLine(bidi.paragraph, l0 - p0, l1 - l0);
        if (!bidi.line)
            return rbttext_out_of_memory(c.error, "reordering a line");
        run_count = SBLineGetRunCount(bidi.line);
        bidi_runs = SBLineGetRunsPtr(bidi.line);
    }

    // The line's glyphs, run by run in visual order.
    layout->shaped.clear();
    for (SBUInteger r = 0; r < run_count; r++)
    {
        const uint32_t r0 = p0 + (uint32_t)bidi_runs[r].offset;
        const uint32_t r1 = r0 + (uint32_t)bidi_runs[r].length;
        const uint8_t level = bidi_runs[r].level;
        const bool odd = (level & 1) != 0;
        // Its pieces of one face and script, from the logical end when it
        // runs right to left.
        uint32_t at = odd ? r1 : r0;
        while (odd ? at > r0 : at < r1)
        {
            uint32_t s0;
            uint32_t s1;
            if (!odd)
            {
                s0 = at;
                s1 = at + 1;
                while (s1 < r1 && layout->faces[s1] == layout->faces[s0] && layout->scripts[s1] == layout->scripts[s0])
                    s1++;
                at = s1;
            }
            else
            {
                s1 = at;
                s0 = at - 1;
                while (s0 > r0 && layout->faces[s0 - 1] == layout->faces[s1 - 1] &&
                       layout->scripts[s0 - 1] == layout->scripts[s1 - 1])
                    s0--;
                at = s0;
            }
            const size_t before = layout->shaped.count;
            const uint32_t run_index = (uint32_t)layout->runs.count;
            if (RBT_RESULT result = shape_piece(c, p0, p1, s0, s1, layout->faces[s0], layout->scripts[s0], odd, run_index))
                return result;
            const size_t shaped = layout->shaped.count - before;
            if (shaped == 0)
                continue; // nothing but codepoints that give no glyph
            RBTTEXT_RUN run;
            run.first_glyph = first_glyph + (uint32_t)before;
            run.glyph_count = (uint32_t)shaped;
            run.text_offset = layout->byte_offsets[s0];
            run.text_size = layout->byte_offsets[s1] - run.text_offset;
            run.face_index = layout->faces[s0];
            run.script = layout->scripts[s0];
            run.bidi_level = level;
            run.direction = odd ? RBTTEXT_DIRECTION_RTL : RBTTEXT_DIRECTION_LTR;
            run.line = line_index;
            if (layout->runs.push(run) != EQ_SUCCESS)
                return rbttext_out_of_memory(c.error, "laying out");
        }
    }

    // Placed along the line from 0: letter spacing after each cluster but
    // the line's last, the box over everything but the trailing spaces.
    const size_t count = layout->shaped.count;
    if (layout->glyphs.reserve(first_glyph + count) != EQ_SUCCESS)
        return rbttext_out_of_memory(c.error, "laying out");
    double pen = 0.0;
    double content_left = INFINITY;
    double content_right = -INFINITY;
    for (size_t i = 0; i < count; i++)
    {
        const rbttext_shaped &s = layout->shaped[i];
        const bool cluster_ends = i + 1 == count || layout->shaped[i + 1].cluster != s.cluster;
        bool last_cluster = true;
        for (size_t j = i + 1; j < count; j++)
        {
            if (layout->shaped[j].cluster != s.cluster)
            {
                last_cluster = false;
                break;
            }
        }
        double advance = s.advance * c.em_size;
        if (cluster_ends && !last_cluster)
            advance += c.letter_spacing;
        if (advance < 0.0)
            advance = 0.0;
        RBTTEXT_GLYPH g;
        g.glyph = s.glyph;
        g.face_index = layout->runs[s.run].face_index;
        g.x = (float)(pen + s.x_offset * c.em_size);
        g.y = (float)(baseline - s.y_offset * c.em_size);
        g.advance = (float)advance;
        g.text_offset = layout->byte_offsets[s.cluster];
        g.text_size = layout->byte_offsets[s.cluster_end] - g.text_offset;
        g.line = line_index;
        g.run = s.run;
        g.box_left = (float)pen;
        g.box_top = (float)(baseline - c.baseline_offset);
        g.box_right = (float)(pen + advance);
        g.box_bottom = (float)(baseline - c.baseline_offset + c.line_advance);
        (void)layout->glyphs.push(g);
        if (s.cluster < trailing)
        {
            if (pen < content_left)
                content_left = pen;
            if (pen + advance > content_right)
                content_right = pen + advance;
        }
        pen += advance;
    }
    if (content_left > content_right)
    {
        content_left = 0.0;
        content_right = 0.0;
    }
    return add_line(c, l0, text_end, first_glyph, first_run, content_left, content_right, rtl);
}

// Lays out the paragraph [p0, p1), whose break runs to [p_end].
static RBT_RESULT lay_out_paragraph(build_context &c, uint32_t p0, uint32_t p1, uint32_t p_end)
{
    rbttext_layout_t *layout = c.layout;
    const RBTTEXT_LAYOUT_DESC &d = *c.desc;
    if (p0 == p1)
    {
        const bool rtl = d.direction == RBTTEXT_DIRECTION_RTL;
        return add_line(c, p0, p_end, (uint32_t)layout->glyphs.count, (uint32_t)layout->runs.count, 0.0, 0.0, rtl);
    }
    const uint32_t length = p1 - p0;

    // 1. Embedding levels. A paragraph that isn't right to left and holds
    // no right-to-left character, Arabic number or directional format
    // resolves to level 0 everywhere under the bidi algorithm (W7 makes
    // its European numbers L, N1 and N2 its neutrals): it is laid out
    // without SheenBidi's objects, which would allocate every build.
    bidi_objects bidi;
    bool simple = d.direction != RBTTEXT_DIRECTION_RTL;
    for (uint32_t i = p0; i < p1 && simple; i++)
    {
        const SBBidiType type = SBCodepointGetBidiType(layout->bidi_codepoints[i]);
        if (type == SBBidiTypeR || type == SBBidiTypeAL || type == SBBidiTypeAN || SBBidiTypeIsFormat(type))
            simple = false;
    }
    bool rtl = false;
    const SBLevel *levels = nullptr;
    if (simple)
    {
        if (layout->levels.resize(length) != EQ_SUCCESS)
            return rbttext_out_of_memory(c.error, "finding the text's directions");
        memset(layout->levels.data, 0, length);
        levels = layout->levels.data;
    }
    else
    {
        SBCodepointSequence sequence;
        sequence.stringEncoding = SBStringEncodingUTF32;
        sequence.stringBuffer = layout->bidi_codepoints.data + p0;
        sequence.stringLength = length;
        bidi.algorithm = SBAlgorithmCreate(&sequence);
        if (!bidi.algorithm)
            return rbttext_out_of_memory(c.error, "finding the text's directions");
        SBLevel base_level = SBLevelDefaultLTR;
        if (d.direction == RBTTEXT_DIRECTION_LTR)
            base_level = 0;
        if (d.direction == RBTTEXT_DIRECTION_RTL)
            base_level = 1;
        bidi.paragraph = SBAlgorithmCreateParagraph(bidi.algorithm, 0, length, base_level);
        if (!bidi.paragraph)
            return rbttext_out_of_memory(c.error, "finding the text's directions");
        rtl = (SBParagraphGetBaseLevel(bidi.paragraph) & 1) != 0;
        levels = SBParagraphGetLevelsPtr(bidi.paragraph);
    }

    // 2. Script runs, over the text as it is.
    SBCodepointSequence text;
    text.stringEncoding = SBStringEncodingUTF32;
    text.stringBuffer = layout->codepoints.data + p0;
    text.stringLength = length;
    if (!layout->script_locator)
        layout->script_locator = SBScriptLocatorCreate();
    if (!layout->script_locator)
        return rbttext_out_of_memory(c.error, "finding the text's scripts");
    SBScriptLocatorLoadCodepoints(layout->script_locator, &text);
    while (SBScriptLocatorMoveNext(layout->script_locator))
    {
        const SBScriptAgent *agent = SBScriptLocatorGetAgent(layout->script_locator);
        const uint32_t tag = SBScriptGetUnicodeTag(agent->script);
        for (SBUInteger i = agent->offset; i < agent->offset + agent->length && i < length; i++)
            layout->scripts[p0 + i] = tag;
    }

    // 4. Measured in pieces of one face, script and level.
    layout->shaped.clear();
    if (layout->advances.resize(length) != EQ_SUCCESS)
        return rbttext_out_of_memory(c.error, "laying out");
    if (layout->cluster_starts.resize(length) != EQ_SUCCESS)
        return rbttext_out_of_memory(c.error, "laying out");
    memset(layout->advances.data, 0, length * sizeof(double));
    memset(layout->cluster_starts.data, 0, length);
    for (uint32_t s0 = p0; s0 < p1;)
    {
        uint32_t s1 = s0 + 1;
        while (s1 < p1 && layout->faces[s1] == layout->faces[s0] && layout->scripts[s1] == layout->scripts[s0] &&
               levels[s1 - p0] == levels[s0 - p0])
            s1++;
        const size_t before = layout->shaped.count;
        if (RBT_RESULT result = shape_piece(c, p0, p1, s0, s1, layout->faces[s0], layout->scripts[s0],
                                          (levels[s0 - p0] & 1) != 0, 0))
            return result;
        layout->cluster_starts[s0 - p0] = 1;
        for (size_t i = before; i < layout->shaped.count; i++)
        {
            const rbttext_shaped &s = layout->shaped[i];
            layout->cluster_starts[s.cluster - p0] = 1;
            layout->advances[s.cluster - p0] += s.advance;
        }
        s0 = s1;
    }
    layout->clusters.clear();
    for (uint32_t i = p0; i < p1; i++)
    {
        const bool space = is_hanging_space(layout->codepoints[i]);
        if (layout->cluster_starts[i - p0] || layout->clusters.count == 0)
        {
            rbttext_cluster cluster = {i, i + 1, 0.0, space ? 1u : 0u};
            if (layout->clusters.push(cluster) != EQ_SUCCESS)
                return rbttext_out_of_memory(c.error, "laying out");
        }
        rbttext_cluster &cluster = layout->clusters[layout->clusters.count - 1];
        cluster.end = i + 1;
        cluster.advance += layout->advances[i - p0];
        if (!space)
            cluster.whitespace = 0;
    }

    // 5. Lines, at the break opportunities; a word wider than the line
    // alone breaks between clusters.
    if (layout->breaks.resize(length) != EQ_SUCCESS)
        return rbttext_out_of_memory(c.error, "breaking lines");
    set_linebreaks_utf32((const utf32_t *)(layout->codepoints.data + p0), length, d.language, layout->breaks.data);
    const double max_width = (double)d.max_width;
    const uint32_t cluster_count = (uint32_t)layout->clusters.count;
    layout->line_starts.clear();
    if (layout->line_starts.push(0) != EQ_SUCCESS)
        return rbttext_out_of_memory(c.error, "breaking lines");
    uint32_t line_start = 0;
    uint32_t last_break = 0;
    double width = 0.0;
    for (uint32_t i = 0; i < cluster_count;)
    {
        const rbttext_cluster &cluster = layout->clusters[i];
        if (i > line_start)
        {
            const char before = layout->breaks[cluster.start - p0 - 1];
            if (before == LINEBREAK_ALLOWBREAK || before == LINEBREAK_MUSTBREAK)
                last_break = i;
        }
        const double advance = cluster.advance * c.em_size;
        const double wider = i == line_start ? advance : width + c.letter_spacing + advance;
        if (max_width > 0.0 && !cluster.whitespace && i > line_start && wider > max_width)
        {
            const uint32_t end = last_break > line_start ? last_break : i;
            if (layout->line_starts.push(end) != EQ_SUCCESS)
                return rbttext_out_of_memory(c.error, "breaking lines");
            line_start = end;
            last_break = end;
            width = 0.0;
            i = end;
            continue;
        }
        width = wider;
        i++;
    }

    // 6 and 7. Each line placed.
    const uint32_t line_count = (uint32_t)layout->line_starts.count;
    for (uint32_t l = 0; l < line_count; l++)
    {
        const uint32_t first = layout->line_starts[l];
        const uint32_t last = l + 1 < line_count ? layout->line_starts[l + 1] : cluster_count;
        const uint32_t l0 = layout->clusters[first].start;
        const uint32_t l1 = layout->clusters[last - 1].end;
        uint32_t trailing = l1;
        for (uint32_t k = last; k > first && layout->clusters[k - 1].whitespace; k--)
            trailing = layout->clusters[k - 1].start;
        const uint32_t text_end = l + 1 < line_count ? l1 : p_end;
        if (RBT_RESULT result = place_line(c, bidi, p0, p1, l0, l1, text_end, trailing, rtl))
            return result;
    }
    return RBT_RESULT_OK;
}

// ---- Building -------------------------------------------------------------------------

static void empty_layout(rbttext_layout_t *layout)
{
    layout->chain = nullptr;
    layout->em_size = 0.0f;
    layout->missing_codepoint_count = 0;
    layout->replaced_sequence_count = 0;
    layout->width = 0.0f;
    layout->height = 0.0f;
    layout->glyphs.clear();
    layout->lines.clear();
    layout->runs.clear();
    layout->line_rtl.clear();
}

static RBT_RESULT build(rbttext_layout_t *layout, const RBTTEXT_LAYOUT_DESC &d, RBTTEXT_ERROR *error)
{
    rbttext_chain_t *chain = d.chain;
    build_context c;
    c.layout = layout;
    c.desc = &d;
    c.chain = chain;
    c.language = d.language ? hb_language_from_string(d.language, -1) : HB_LANGUAGE_INVALID;
    c.em_size = (double)d.em_size;
    c.letter_spacing = (double)d.letter_spacing;
    c.error = error;
    const RBTTEXT_FACE_METRICS &m = chain->faces[0]->metrics;
    const double ascender = (double)m.ascender * c.em_size;
    const double descender = (double)m.descender * c.em_size;
    const double natural = ((double)m.ascender - (double)m.descender + (double)m.line_gap) * c.em_size;
    c.line_advance = natural * (double)d.line_spacing;
    c.baseline_offset = (c.line_advance - (ascender - descender)) / 2.0 + ascender;

    // The text as codepoints, and what is known of each before any
    // paragraph is laid out.
    if (decode(layout, (const uint8_t *)d.text, d.text_size) != EQ_SUCCESS)
        return rbttext_out_of_memory(error, "decoding the text");
    const uint32_t n = (uint32_t)layout->codepoints.count;
    if (layout->shaping_codepoints.resize(n) != EQ_SUCCESS)
        return rbttext_out_of_memory(error, "laying out");
    if (layout->bidi_codepoints.resize(n) != EQ_SUCCESS)
        return rbttext_out_of_memory(error, "laying out");
    if (layout->faces.resize(n) != EQ_SUCCESS)
        return rbttext_out_of_memory(error, "laying out");
    if (layout->scripts.resize(n) != EQ_SUCCESS)
        return rbttext_out_of_memory(error, "laying out");
    for (uint32_t i = 0; i < n; i++)
    {
        const uint32_t codepoint = layout->codepoints[i];
        // A tab is laid out as a space until tab stops exist.
        layout->shaping_codepoints[i] = codepoint == 0x09 ? 0x20 : codepoint;
        // The bidirectional algorithm ends a paragraph at its paragraph
        // separators; the information separators U+001C-U+001E are the ones
        // the paragraphs here don't end at, so they reach it as U+001F, a
        // segment separator.
        layout->bidi_codepoints[i] = codepoint >= 0x1C && codepoint <= 0x1E ? 0x1F : codepoint;
        layout->scripts[i] = RBTTEXT_TAG('Z', 'y', 'y', 'y');
    }

    // 3. The face of each codepoint: the first in the chain with a glyph
    // for it; a mark or a default-ignorable codepoint stays with the face
    // before it where that face has it (or, ignorable, has nothing).
    for (uint32_t i = 0; i < n; i++)
    {
        const uint32_t codepoint = layout->shaping_codepoints[i];
        layout->faces[i] = 0;
        if (is_mandatory_break(codepoint))
            continue;
        const bool ignorable = is_default_ignorable(codepoint);
        uint32_t glyph = 0;
        if (i > 0 && !is_mandatory_break(layout->codepoints[i - 1]) && (ignorable || is_mark(codepoint)))
        {
            const uint32_t before = layout->faces[i - 1];
            if (ignorable || rbttext_face_lookup(chain->faces[before], codepoint, &glyph))
            {
                layout->faces[i] = before;
                continue;
            }
        }
        bool found = false;
        for (uint32_t f = 0; f < chain->faces.count && !found; f++)
        {
            if (rbttext_face_lookup(chain->faces[f], codepoint, &glyph))
            {
                layout->faces[i] = f;
                found = true;
            }
        }
        if (!found && !ignorable)
            layout->missing_codepoint_count++;
    }

    // The features, the same for every piece.
    layout->features.clear();
    const struct
    {
        uint32_t tag;
        uint32_t flag;
    } switched[4] = {{HB_TAG('k', 'e', 'r', 'n'), RBTTEXT_FEATURE_KERNING},
                     {HB_TAG('l', 'i', 'g', 'a'), RBTTEXT_FEATURE_LIGATURES},
                     {HB_TAG('c', 'l', 'i', 'g'), RBTTEXT_FEATURE_LIGATURES},
                     {HB_TAG('c', 'a', 'l', 't'), RBTTEXT_FEATURE_CONTEXTUAL_ALTERNATES}};
    for (const auto &feature : switched)
    {
        hb_feature_t f = {feature.tag, (d.features & feature.flag) ? 1u : 0u, HB_FEATURE_GLOBAL_START,
                          HB_FEATURE_GLOBAL_END};
        if (layout->features.push(f) != EQ_SUCCESS)
            return rbttext_out_of_memory(error, "laying out");
    }
    for (uint32_t i = 0; i < d.extra_feature_count; i++)
    {
        hb_feature_t f = {d.extra_features[i].tag, d.extra_features[i].value, HB_FEATURE_GLOBAL_START,
                          HB_FEATURE_GLOBAL_END};
        if (layout->features.push(f) != EQ_SUCCESS)
            return rbttext_out_of_memory(error, "laying out");
    }

    // Each paragraph, from the text's start or a mandatory break's end;
    // text ending in a break has a last, empty paragraph.
    uint32_t start = 0;
    for (uint32_t i = 0; i < n; i++)
    {
        const uint32_t codepoint = layout->codepoints[i];
        if (!is_mandatory_break(codepoint))
            continue;
        uint32_t end = i + 1;
        if (codepoint == 0x0D && end < n && layout->codepoints[end] == 0x0A)
            end++;
        if (RBT_RESULT result = lay_out_paragraph(c, start, i, end))
            return result;
        start = end;
        i = end - 1;
    }
    if (RBT_RESULT result = lay_out_paragraph(c, start, n, n))
        return result;

    // Every line aligned within the layout's width: max_width, or the
    // widest line's.
    double layout_width = 0.0;
    for (const RBTTEXT_LINE &line : layout->lines)
    {
        const double w = (double)line.box_right - (double)line.box_left;
        if (w > layout_width)
            layout_width = w;
    }
    const double align_width = d.max_width > 0.0f ? (double)d.max_width : layout_width;
    for (size_t l = 0; l < layout->lines.count; l++)
    {
        RBTTEXT_LINE &line = layout->lines[l];
        const bool rtl = layout->line_rtl[l] != 0;
        const double w = (double)line.box_right - (double)line.box_left;
        RBTTEXT_ALIGN align = d.align;
        if (align == RBTTEXT_ALIGN_START)
            align = rtl ? RBTTEXT_ALIGN_RIGHT : RBTTEXT_ALIGN_LEFT;
        if (align == RBTTEXT_ALIGN_END)
            align = rtl ? RBTTEXT_ALIGN_LEFT : RBTTEXT_ALIGN_RIGHT;
        double target = 0.0;
        if (align == RBTTEXT_ALIGN_CENTER)
            target = (align_width - w) / 2.0;
        if (align == RBTTEXT_ALIGN_RIGHT)
            target = align_width - w;
        const double shift = target - (double)line.box_left;
        for (uint32_t g = line.first_glyph; g < line.first_glyph + line.glyph_count; g++)
        {
            RBTTEXT_GLYPH &glyph = layout->glyphs[g];
            glyph.x = (float)((double)glyph.x + shift);
            glyph.box_left = (float)((double)glyph.box_left + shift);
            glyph.box_right = (float)((double)glyph.box_right + shift);
        }
        line.box_left = (float)target;
        line.box_right = (float)(target + w);
    }

    layout->chain = chain;
    layout->em_size = d.em_size;
    layout->width = (float)layout_width;
    layout->height = (float)(c.line_advance * (double)layout->lines.count);
    return RBT_RESULT_OK;
}

// ---- Objects ------------------------------------------------------------------------

rbttext_layout_t::~rbttext_layout_t()
{
    if (buffer)
        hb_buffer_destroy(buffer);
    if (script_locator)
        SBScriptLocatorRelease(script_locator);
}

// Whether [tag] is a feature RBTTEXT_FEATURE_FLAGS controls.
static bool is_flag_feature(uint32_t tag)
{
    if (tag == RBTTEXT_TAG('k', 'e', 'r', 'n'))
        return true;
    if (tag == RBTTEXT_TAG('l', 'i', 'g', 'a'))
        return true;
    if (tag == RBTTEXT_TAG('c', 'l', 'i', 'g'))
        return true;
    return tag == RBTTEXT_TAG('c', 'a', 'l', 't');
}

static const uint32_t ALL_FEATURE_FLAGS =
    RBTTEXT_FEATURE_KERNING | RBTTEXT_FEATURE_LIGATURES | RBTTEXT_FEATURE_CONTEXTUAL_ALTERNATES;

extern "C"
{
    RBT_RESULT rbttext_layout_create(rbttext_layout *out_layout)
    {
        if (!out_layout)
            return RBT_RESULT_INVALID_ARGUMENT;
        *out_layout = nullptr;
        rbttext_layout_t *layout = (rbttext_layout_t *)malloc(sizeof(rbttext_layout_t));
        if (!layout)
            return RBT_RESULT_OUT_OF_MEMORY;
        layout = new (layout) rbttext_layout_t();
        layout->buffer = hb_buffer_create();
        if (!hb_buffer_allocation_successful(layout->buffer))
        {
            layout->~rbttext_layout_t();
            free(layout);
            return RBT_RESULT_OUT_OF_MEMORY;
        }
        *out_layout = layout;
        return RBT_RESULT_OK;
    }

    RBT_RESULT rbttext_layout_build(rbttext_layout layout, const RBTTEXT_LAYOUT_DESC *desc, RBTTEXT_ERROR *out_error)
    {
        rbttext_clear_error(out_error);
        if (!layout)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "layout is null");
        if (RBT_STALE(layout, "rbttext_layout_build", "layout"))
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "layout is destroyed");
        empty_layout(layout);
        if (!desc)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc is null");
        if (RBT_RESULT result = rbt_struct_check(&desc->base, RBT_STRUCT_TYPE_TEXT_LAYOUT_DESC))
            return rbttext_fail(out_error, result, "desc %s", rbt_struct_check_message(result));
        const RBTTEXT_LAYOUT_DESC &d = *desc;
        if (!d.chain)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc's chain is null");
        if (rbttext_missing(d.text, d.text_size))
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc's text is null with a text_size of %u",
                               d.text_size);
        if (d.direction < RBTTEXT_DIRECTION_LTR)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc's direction %u is not an RBTTEXT_DIRECTION",
                               (unsigned)d.direction);
        if (d.direction > RBTTEXT_DIRECTION_AUTO)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc's direction %u is not an RBTTEXT_DIRECTION",
                               (unsigned)d.direction);
        if ((d.features & ~ALL_FEATURE_FLAGS) != 0)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc's features has bits 0x%X, which are no feature",
                               d.features & ~ALL_FEATURE_FLAGS);
        if (rbttext_missing(d.extra_features, d.extra_feature_count))
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT,
                               "desc's extra_features is null with an extra_feature_count of %u", d.extra_feature_count);
        for (uint32_t i = 0; i < d.extra_feature_count; i++)
        {
            if (is_flag_feature(d.extra_features[i].tag))
                return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT,
                                   "extra_features[%u] is a feature desc's features controls", i);
            for (uint32_t j = 0; j < i; j++)
            {
                if (d.extra_features[i].tag == d.extra_features[j].tag)
                    return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT,
                                       "extra_features[%u] has the tag of extra_features[%u]", i, j);
            }
        }
        if (!isfinite(d.em_size))
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc's em_size is not finite");
        if (d.em_size <= 0.0f)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc's em_size is not above 0");
        if (!isfinite(d.max_width))
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc's max_width is not finite");
        if (d.max_width < 0.0f)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc's max_width is negative");
        if (d.align < RBTTEXT_ALIGN_LEFT)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc's align %u is not an RBTTEXT_ALIGN",
                               (unsigned)d.align);
        if (d.align > RBTTEXT_ALIGN_END)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc's align %u is not an RBTTEXT_ALIGN",
                               (unsigned)d.align);
        if (!isfinite(d.line_spacing))
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc's line_spacing is not finite");
        if (d.line_spacing <= 0.0f)
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc's line_spacing is not above 0");
        if (!isfinite(d.letter_spacing))
            return rbttext_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc's letter_spacing is not finite");

        RBT_RESULT result = build(layout, d, out_error);
        if (result != RBT_RESULT_OK)
            empty_layout(layout);
        return result;
    }

    RBT_RESULT rbttext_layout_get_info(rbttext_layout layout, RBTTEXT_LAYOUT_INFO *out_info)
    {
        if (!layout)
            return RBT_RESULT_INVALID_ARGUMENT;
        if (RBT_STALE(layout, "rbttext_layout_get_info", "layout"))
            return RBT_RESULT_INVALID_ARGUMENT;
        RBT_ENTER(layout->overlap, "rbttext", "rbttext_layout_get_info", "layout");
        if (!out_info)
            return RBT_RESULT_INVALID_ARGUMENT;
        if (RBT_RESULT result = rbt_struct_out_check(&out_info->base, RBT_STRUCT_TYPE_TEXT_LAYOUT_INFO))
            return result;
        out_info->glyph_count = (uint32_t)layout->glyphs.count;
        out_info->line_count = (uint32_t)layout->lines.count;
        out_info->run_count = (uint32_t)layout->runs.count;
        out_info->missing_codepoint_count = layout->missing_codepoint_count;
        out_info->replaced_sequence_count = layout->replaced_sequence_count;
        out_info->width = layout->width;
        out_info->height = layout->height;
        return RBT_RESULT_OK;
    }

    RBT_RESULT rbttext_layout_get_glyphs(rbttext_layout layout, const RBTTEXT_GLYPH **out_glyphs, uint32_t *out_count)
    {
        if (!layout)
            return RBT_RESULT_INVALID_ARGUMENT;
        if (RBT_STALE(layout, "rbttext_layout_get_glyphs", "layout"))
            return RBT_RESULT_INVALID_ARGUMENT;
        RBT_ENTER(layout->overlap, "rbttext", "rbttext_layout_get_glyphs", "layout");
        if (!out_glyphs)
            return RBT_RESULT_INVALID_ARGUMENT;
        if (!out_count)
            return RBT_RESULT_INVALID_ARGUMENT;
        *out_glyphs = layout->glyphs.count ? layout->glyphs.data : nullptr;
        *out_count = (uint32_t)layout->glyphs.count;
        return RBT_RESULT_OK;
    }

    RBT_RESULT rbttext_layout_get_lines(rbttext_layout layout, const RBTTEXT_LINE **out_lines, uint32_t *out_count)
    {
        if (!layout)
            return RBT_RESULT_INVALID_ARGUMENT;
        if (RBT_STALE(layout, "rbttext_layout_get_lines", "layout"))
            return RBT_RESULT_INVALID_ARGUMENT;
        RBT_ENTER(layout->overlap, "rbttext", "rbttext_layout_get_lines", "layout");
        if (!out_lines)
            return RBT_RESULT_INVALID_ARGUMENT;
        if (!out_count)
            return RBT_RESULT_INVALID_ARGUMENT;
        *out_lines = layout->lines.count ? layout->lines.data : nullptr;
        *out_count = (uint32_t)layout->lines.count;
        return RBT_RESULT_OK;
    }

    RBT_RESULT rbttext_layout_get_runs(rbttext_layout layout, const RBTTEXT_RUN **out_runs, uint32_t *out_count)
    {
        if (!layout)
            return RBT_RESULT_INVALID_ARGUMENT;
        if (RBT_STALE(layout, "rbttext_layout_get_runs", "layout"))
            return RBT_RESULT_INVALID_ARGUMENT;
        RBT_ENTER(layout->overlap, "rbttext", "rbttext_layout_get_runs", "layout");
        if (!out_runs)
            return RBT_RESULT_INVALID_ARGUMENT;
        if (!out_count)
            return RBT_RESULT_INVALID_ARGUMENT;
        *out_runs = layout->runs.count ? layout->runs.data : nullptr;
        *out_count = (uint32_t)layout->runs.count;
        return RBT_RESULT_OK;
    }

    RBT_RESULT rbttext_layout_get_strokes(rbttext_layout layout, RBTTEXT_SEGMENT *segments, uint32_t capacity,
                                       uint32_t *out_count)
    {
        if (!layout)
            return RBT_RESULT_INVALID_ARGUMENT;
        if (RBT_STALE(layout, "rbttext_layout_get_strokes", "layout"))
            return RBT_RESULT_INVALID_ARGUMENT;
        if (rbttext_missing(segments, capacity))
            return RBT_RESULT_INVALID_ARGUMENT;
        if (!out_count)
            return RBT_RESULT_INVALID_ARGUMENT;
        *out_count = 0;
        if (layout->chain && layout->chain->kind != RBTTEXT_FACE_KIND_STROKE)
            return RBT_RESULT_INVALID_ARGUMENT;
        if (!layout->chain)
            return RBT_RESULT_OK; // empty: no glyph, no segment
        uint64_t total = 0;
        for (const RBTTEXT_GLYPH &g : layout->glyphs)
        {
            const rbttext_face_t *face = layout->chain->faces[g.face_index];
            if (g.glyph == 0)
                continue;
            const rbttext_stroke_glyph &sg = face->glyphs[g.glyph - 1];
            for (uint32_t p = 0; p < sg.polyline_count; p++)
                total += face->polylines[sg.first_polyline + p].point_count - 1;
        }
        if (total > UINT32_MAX)
            return RBT_RESULT_LIMIT;
        *out_count = (uint32_t)total;
        if (capacity == 0)
            return RBT_RESULT_OK;
        if (capacity < total)
            return RBT_RESULT_LIMIT;
        const double em = (double)layout->em_size;
        uint32_t written = 0;
        for (uint32_t i = 0; i < layout->glyphs.count; i++)
        {
            const RBTTEXT_GLYPH &g = layout->glyphs[i];
            const rbttext_face_t *face = layout->chain->faces[g.face_index];
            if (g.glyph == 0)
                continue;
            const rbttext_stroke_glyph &sg = face->glyphs[g.glyph - 1];
            const RbtVec2 *points = face->points.data + sg.first_point;
            for (uint32_t p = 0; p < sg.polyline_count; p++)
            {
                const RBTTEXT_STROKE_POLYLINE &line = face->polylines[sg.first_polyline + p];
                for (uint32_t q = 0; q + 1 < line.point_count; q++)
                {
                    const RbtVec2 &a = points[line.first_point + q];
                    const RbtVec2 &b = points[line.first_point + q + 1];
                    RBTTEXT_SEGMENT &s = segments[written++];
                    s.x0 = (float)((double)g.x + (double)a.x * em);
                    s.y0 = (float)((double)g.y + (double)a.y * em);
                    s.x1 = (float)((double)g.x + (double)b.x * em);
                    s.y1 = (float)((double)g.y + (double)b.y * em);
                    s.glyph_index = i;
                }
            }
        }
        return RBT_RESULT_OK;
    }

    void rbttext_layout_destroy(rbttext_layout layout)
    {
        if (!layout)
            return;
        if (RBT_STALE(layout, "rbttext_layout_destroy", "layout"))
            return;
        layout->~rbttext_layout_t();
        free(layout);
    }
}
