#include "aptext_internal.hpp"
#include "../apristructbase.hpp"
#include <SheenBidi/SheenBidi.h>
#include <linebreak.h>
#include <math.h>
#include <new>
#include <stdlib.h>
#include <string.h>

// ApText's layouts (APTEXT_TODO.md, item 2). A build runs, for each
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
static EQ_RESULT decode(aptext_layout_t *layout, const uint8_t *text, uint32_t size)
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
    aptext_layout_t *layout;
    const APTEXT_LAYOUT_DESC *desc;
    aptext_chain_t *chain;
    hb_language_t language;
    double em_size;
    double letter_spacing;
    double line_advance;
    double baseline_offset; // from a line's top to its baseline
    APTEXT_ERROR *error;
};

// Shapes codepoints [s0, s1) of the paragraph [p0, p1) with one face, into
// the layout's shaped glyphs, in visual order, each given [run].
static APRESULT shape_piece(build_context &c, uint32_t p0, uint32_t p1, uint32_t s0, uint32_t s1, uint32_t face_index,
                            uint32_t script, bool rtl, uint32_t run)
{
    aptext_layout_t *layout = c.layout;
    aptext_face_t *face = c.chain->faces[face_index];
    const size_t first = layout->shaped.count;
    if (face->kind == APTEXT_FACE_KIND_OUTLINE)
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
            return aptext_out_of_memory(c.error, "shaping");
        hb_shape(face->font, buffer, layout->features.data, (unsigned int)layout->features.count);
        if (!hb_buffer_allocation_successful(buffer))
            return aptext_out_of_memory(c.error, "shaping");
        unsigned int count = 0;
        const hb_glyph_info_t *infos = hb_buffer_get_glyph_infos(buffer, &count);
        const hb_glyph_position_t *positions = hb_buffer_get_glyph_positions(buffer, &count);
        const double units = (double)face->metrics.units_per_em;
        if (layout->shaped.reserve(first + count) != EQ_SUCCESS)
            return aptext_out_of_memory(c.error, "shaping");
        for (unsigned int i = 0; i < count; i++)
        {
            aptext_shaped s;
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
            return aptext_out_of_memory(c.error, "laying out");
        for (uint32_t i = s0; i < s1; i++)
        {
            uint32_t glyph = 0;
            bool found = aptext_face_lookup(face, layout->shaping_codepoints[i], &glyph);
            if (!found && is_default_ignorable(layout->codepoints[i]))
                continue;
            aptext_shaped s;
            s.glyph = glyph;
            s.cluster = i;
            s.cluster_end = s1;
            s.run = run;
            s.advance = glyph ? (double)face->glyphs[glyph - 1].advance : (double)APTEXT_STROKE_NOTDEF_ADVANCE;
            s.x_offset = 0.0;
            s.y_offset = 0.0;
            (void)layout->shaped.push(s);
        }
        if (rtl)
        {
            aptext_shaped *begin = layout->shaped.data + first;
            aptext_shaped *end = layout->shaped.data + layout->shaped.count;
            for (; begin + 1 < end; begin++, end--)
            {
                aptext_shaped swap = *begin;
                *begin = end[-1];
                end[-1] = swap;
            }
        }
    }
    // Where each glyph's cluster ends: the next cluster in logical order,
    // which is to the right in a left-to-right piece and to the left in a
    // right-to-left one; the piece's end for the last.
    aptext_shaped *glyphs = layout->shaped.data + first;
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
    return APRESULT_OK;
}

// ---- Paragraphs ---------------------------------------------------------------------

// Releases SheenBidi's objects when a paragraph is done.
struct bidi_objects
{
    SBAlgorithmRef algorithm = nullptr;
    SBParagraphRef paragraph = nullptr;
    SBScriptLocatorRef scripts = nullptr;
    SBLineRef line = nullptr;

    ~bidi_objects()
    {
        if (line)
            SBLineRelease(line);
        if (scripts)
            SBScriptLocatorRelease(scripts);
        if (paragraph)
            SBParagraphRelease(paragraph);
        if (algorithm)
            SBAlgorithmRelease(algorithm);
    }
};

// Adds a line to the layout, its glyphs and box not yet aligned: the box
// spans the glyphs left of [content_left] and right of [content_right] out.
static APRESULT add_line(build_context &c, uint32_t text_start, uint32_t text_end, uint32_t first_glyph,
                         uint32_t first_run, double content_left, double content_right, bool rtl)
{
    aptext_layout_t *layout = c.layout;
    const uint32_t index = (uint32_t)layout->lines.count;
    APTEXT_LINE line;
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
        return aptext_out_of_memory(c.error, "laying out");
    if (layout->line_rtl.push(rtl ? 1 : 0) != EQ_SUCCESS)
        return aptext_out_of_memory(c.error, "laying out");
    return APRESULT_OK;
}

// Places a paragraph's line [l0, l1), whose text runs on to [text_end]
// (its break, on the paragraph's last line), and whose trailing spaces
// start at [trailing].
static APRESULT place_line(build_context &c, bidi_objects &bidi, uint32_t p0, uint32_t p1, uint32_t l0, uint32_t l1,
                           uint32_t text_end, uint32_t trailing, bool rtl)
{
    aptext_layout_t *layout = c.layout;
    const uint32_t first_glyph = (uint32_t)layout->glyphs.count;
    const uint32_t first_run = (uint32_t)layout->runs.count;
    const uint32_t line_index = (uint32_t)layout->lines.count;
    const double baseline = (double)line_index * c.line_advance + c.baseline_offset;

    if (bidi.line)
    {
        SBLineRelease(bidi.line);
        bidi.line = nullptr;
    }
    bidi.line = SBParagraphCreateLine(bidi.paragraph, l0 - p0, l1 - l0);
    if (!bidi.line)
        return aptext_out_of_memory(c.error, "reordering a line");
    const SBUInteger run_count = SBLineGetRunCount(bidi.line);
    const SBRun *bidi_runs = SBLineGetRunsPtr(bidi.line);

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
            if (APRESULT result = shape_piece(c, p0, p1, s0, s1, layout->faces[s0], layout->scripts[s0], odd, run_index))
                return result;
            const size_t shaped = layout->shaped.count - before;
            if (shaped == 0)
                continue; // nothing but codepoints that give no glyph
            APTEXT_RUN run;
            run.first_glyph = first_glyph + (uint32_t)before;
            run.glyph_count = (uint32_t)shaped;
            run.text_offset = layout->byte_offsets[s0];
            run.text_size = layout->byte_offsets[s1] - run.text_offset;
            run.face_index = layout->faces[s0];
            run.script = layout->scripts[s0];
            run.bidi_level = level;
            run.direction = odd ? APTEXT_DIRECTION_RTL : APTEXT_DIRECTION_LTR;
            run.line = line_index;
            if (layout->runs.push(run) != EQ_SUCCESS)
                return aptext_out_of_memory(c.error, "laying out");
        }
    }

    // Placed along the line from 0: letter spacing after each cluster but
    // the line's last, the box over everything but the trailing spaces.
    const size_t count = layout->shaped.count;
    if (layout->glyphs.reserve(first_glyph + count) != EQ_SUCCESS)
        return aptext_out_of_memory(c.error, "laying out");
    double pen = 0.0;
    double content_left = INFINITY;
    double content_right = -INFINITY;
    for (size_t i = 0; i < count; i++)
    {
        const aptext_shaped &s = layout->shaped[i];
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
        APTEXT_GLYPH g;
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
static APRESULT lay_out_paragraph(build_context &c, uint32_t p0, uint32_t p1, uint32_t p_end)
{
    aptext_layout_t *layout = c.layout;
    const APTEXT_LAYOUT_DESC &d = *c.desc;
    if (p0 == p1)
    {
        const bool rtl = d.direction == APTEXT_DIRECTION_RTL;
        return add_line(c, p0, p_end, (uint32_t)layout->glyphs.count, (uint32_t)layout->runs.count, 0.0, 0.0, rtl);
    }
    const uint32_t length = p1 - p0;

    // 1. Embedding levels.
    bidi_objects bidi;
    SBCodepointSequence sequence;
    sequence.stringEncoding = SBStringEncodingUTF32;
    sequence.stringBuffer = layout->bidi_codepoints.data + p0;
    sequence.stringLength = length;
    bidi.algorithm = SBAlgorithmCreate(&sequence);
    if (!bidi.algorithm)
        return aptext_out_of_memory(c.error, "finding the text's directions");
    SBLevel base_level = SBLevelDefaultLTR;
    if (d.direction == APTEXT_DIRECTION_LTR)
        base_level = 0;
    if (d.direction == APTEXT_DIRECTION_RTL)
        base_level = 1;
    bidi.paragraph = SBAlgorithmCreateParagraph(bidi.algorithm, 0, length, base_level);
    if (!bidi.paragraph)
        return aptext_out_of_memory(c.error, "finding the text's directions");
    const bool rtl = (SBParagraphGetBaseLevel(bidi.paragraph) & 1) != 0;
    const SBLevel *levels = SBParagraphGetLevelsPtr(bidi.paragraph);

    // 2. Script runs, over the text as it is.
    SBCodepointSequence text;
    text.stringEncoding = SBStringEncodingUTF32;
    text.stringBuffer = layout->codepoints.data + p0;
    text.stringLength = length;
    bidi.scripts = SBScriptLocatorCreate();
    if (!bidi.scripts)
        return aptext_out_of_memory(c.error, "finding the text's scripts");
    SBScriptLocatorLoadCodepoints(bidi.scripts, &text);
    while (SBScriptLocatorMoveNext(bidi.scripts))
    {
        const SBScriptAgent *agent = SBScriptLocatorGetAgent(bidi.scripts);
        const uint32_t tag = SBScriptGetUnicodeTag(agent->script);
        for (SBUInteger i = agent->offset; i < agent->offset + agent->length && i < length; i++)
            layout->scripts[p0 + i] = tag;
    }

    // 4. Measured in pieces of one face, script and level.
    layout->shaped.clear();
    if (layout->advances.resize(length) != EQ_SUCCESS)
        return aptext_out_of_memory(c.error, "laying out");
    if (layout->cluster_starts.resize(length) != EQ_SUCCESS)
        return aptext_out_of_memory(c.error, "laying out");
    memset(layout->advances.data, 0, length * sizeof(double));
    memset(layout->cluster_starts.data, 0, length);
    for (uint32_t s0 = p0; s0 < p1;)
    {
        uint32_t s1 = s0 + 1;
        while (s1 < p1 && layout->faces[s1] == layout->faces[s0] && layout->scripts[s1] == layout->scripts[s0] &&
               levels[s1 - p0] == levels[s0 - p0])
            s1++;
        const size_t before = layout->shaped.count;
        if (APRESULT result = shape_piece(c, p0, p1, s0, s1, layout->faces[s0], layout->scripts[s0],
                                          (levels[s0 - p0] & 1) != 0, 0))
            return result;
        layout->cluster_starts[s0 - p0] = 1;
        for (size_t i = before; i < layout->shaped.count; i++)
        {
            const aptext_shaped &s = layout->shaped[i];
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
            aptext_cluster cluster = {i, i + 1, 0.0, space ? 1u : 0u};
            if (layout->clusters.push(cluster) != EQ_SUCCESS)
                return aptext_out_of_memory(c.error, "laying out");
        }
        aptext_cluster &cluster = layout->clusters[layout->clusters.count - 1];
        cluster.end = i + 1;
        cluster.advance += layout->advances[i - p0];
        if (!space)
            cluster.whitespace = 0;
    }

    // 5. Lines, at the break opportunities; a word wider than the line
    // alone breaks between clusters.
    if (layout->breaks.resize(length) != EQ_SUCCESS)
        return aptext_out_of_memory(c.error, "breaking lines");
    set_linebreaks_utf32((const utf32_t *)(layout->codepoints.data + p0), length, d.language, layout->breaks.data);
    const double max_width = (double)d.max_width;
    const uint32_t cluster_count = (uint32_t)layout->clusters.count;
    layout->line_starts.clear();
    if (layout->line_starts.push(0) != EQ_SUCCESS)
        return aptext_out_of_memory(c.error, "breaking lines");
    uint32_t line_start = 0;
    uint32_t last_break = 0;
    double width = 0.0;
    for (uint32_t i = 0; i < cluster_count;)
    {
        const aptext_cluster &cluster = layout->clusters[i];
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
                return aptext_out_of_memory(c.error, "breaking lines");
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
        if (APRESULT result = place_line(c, bidi, p0, p1, l0, l1, text_end, trailing, rtl))
            return result;
    }
    return APRESULT_OK;
}

// ---- Building -------------------------------------------------------------------------

static void empty_layout(aptext_layout_t *layout)
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

static APRESULT build(aptext_layout_t *layout, const APTEXT_LAYOUT_DESC &d, APTEXT_ERROR *error)
{
    aptext_chain_t *chain = d.chain;
    build_context c;
    c.layout = layout;
    c.desc = &d;
    c.chain = chain;
    c.language = d.language ? hb_language_from_string(d.language, -1) : HB_LANGUAGE_INVALID;
    c.em_size = (double)d.em_size;
    c.letter_spacing = (double)d.letter_spacing;
    c.error = error;
    const APTEXT_FACE_METRICS &m = chain->faces[0]->metrics;
    const double ascender = (double)m.ascender * c.em_size;
    const double descender = (double)m.descender * c.em_size;
    const double natural = ((double)m.ascender - (double)m.descender + (double)m.line_gap) * c.em_size;
    c.line_advance = natural * (double)d.line_spacing;
    c.baseline_offset = (c.line_advance - (ascender - descender)) / 2.0 + ascender;

    // The text as codepoints, and what is known of each before any
    // paragraph is laid out.
    if (decode(layout, (const uint8_t *)d.text, d.text_size) != EQ_SUCCESS)
        return aptext_out_of_memory(error, "decoding the text");
    const uint32_t n = (uint32_t)layout->codepoints.count;
    if (layout->shaping_codepoints.resize(n) != EQ_SUCCESS)
        return aptext_out_of_memory(error, "laying out");
    if (layout->bidi_codepoints.resize(n) != EQ_SUCCESS)
        return aptext_out_of_memory(error, "laying out");
    if (layout->faces.resize(n) != EQ_SUCCESS)
        return aptext_out_of_memory(error, "laying out");
    if (layout->scripts.resize(n) != EQ_SUCCESS)
        return aptext_out_of_memory(error, "laying out");
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
        layout->scripts[i] = APTEXT_TAG('Z', 'y', 'y', 'y');
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
            if (ignorable || aptext_face_lookup(chain->faces[before], codepoint, &glyph))
            {
                layout->faces[i] = before;
                continue;
            }
        }
        bool found = false;
        for (uint32_t f = 0; f < chain->faces.count && !found; f++)
        {
            if (aptext_face_lookup(chain->faces[f], codepoint, &glyph))
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
    } switched[4] = {{HB_TAG('k', 'e', 'r', 'n'), APTEXT_FEATURE_KERNING},
                     {HB_TAG('l', 'i', 'g', 'a'), APTEXT_FEATURE_LIGATURES},
                     {HB_TAG('c', 'l', 'i', 'g'), APTEXT_FEATURE_LIGATURES},
                     {HB_TAG('c', 'a', 'l', 't'), APTEXT_FEATURE_CONTEXTUAL_ALTERNATES}};
    for (const auto &feature : switched)
    {
        hb_feature_t f = {feature.tag, (d.features & feature.flag) ? 1u : 0u, HB_FEATURE_GLOBAL_START,
                          HB_FEATURE_GLOBAL_END};
        if (layout->features.push(f) != EQ_SUCCESS)
            return aptext_out_of_memory(error, "laying out");
    }
    for (uint32_t i = 0; i < d.extra_feature_count; i++)
    {
        hb_feature_t f = {d.extra_features[i].tag, d.extra_features[i].value, HB_FEATURE_GLOBAL_START,
                          HB_FEATURE_GLOBAL_END};
        if (layout->features.push(f) != EQ_SUCCESS)
            return aptext_out_of_memory(error, "laying out");
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
        if (APRESULT result = lay_out_paragraph(c, start, i, end))
            return result;
        start = end;
        i = end - 1;
    }
    if (APRESULT result = lay_out_paragraph(c, start, n, n))
        return result;

    // Every line aligned within the layout's width: max_width, or the
    // widest line's.
    double layout_width = 0.0;
    for (const APTEXT_LINE &line : layout->lines)
    {
        const double w = (double)line.box_right - (double)line.box_left;
        if (w > layout_width)
            layout_width = w;
    }
    const double align_width = d.max_width > 0.0f ? (double)d.max_width : layout_width;
    for (size_t l = 0; l < layout->lines.count; l++)
    {
        APTEXT_LINE &line = layout->lines[l];
        const bool rtl = layout->line_rtl[l] != 0;
        const double w = (double)line.box_right - (double)line.box_left;
        APTEXT_ALIGN align = d.align;
        if (align == APTEXT_ALIGN_START)
            align = rtl ? APTEXT_ALIGN_RIGHT : APTEXT_ALIGN_LEFT;
        if (align == APTEXT_ALIGN_END)
            align = rtl ? APTEXT_ALIGN_LEFT : APTEXT_ALIGN_RIGHT;
        double target = 0.0;
        if (align == APTEXT_ALIGN_CENTER)
            target = (align_width - w) / 2.0;
        if (align == APTEXT_ALIGN_RIGHT)
            target = align_width - w;
        const double shift = target - (double)line.box_left;
        for (uint32_t g = line.first_glyph; g < line.first_glyph + line.glyph_count; g++)
        {
            APTEXT_GLYPH &glyph = layout->glyphs[g];
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
    return APRESULT_OK;
}

// ---- Objects ------------------------------------------------------------------------

aptext_layout_t::~aptext_layout_t()
{
    if (buffer)
        hb_buffer_destroy(buffer);
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
    APRESULT aptext_layout_create(aptext_layout *out_layout)
    {
        if (!out_layout)
            return APRESULT_INVALID_ARGUMENT;
        *out_layout = nullptr;
        aptext_layout_t *layout = (aptext_layout_t *)malloc(sizeof(aptext_layout_t));
        if (!layout)
            return APRESULT_OUT_OF_MEMORY;
        layout = new (layout) aptext_layout_t();
        layout->buffer = hb_buffer_create();
        if (!hb_buffer_allocation_successful(layout->buffer))
        {
            layout->~aptext_layout_t();
            free(layout);
            return APRESULT_OUT_OF_MEMORY;
        }
        *out_layout = layout;
        return APRESULT_OK;
    }

    APRESULT aptext_layout_build(aptext_layout layout, const APTEXT_LAYOUT_DESC *desc, APTEXT_ERROR *out_error)
    {
        aptext_clear_error(out_error);
        if (!layout)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "layout is null");
        empty_layout(layout);
        if (!desc)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is null");
        if (APRESULT result = apri_struct_check(&desc->base, APRI_STRUCT_TYPE_APTEXT_LAYOUT_DESC))
            return aptext_fail(out_error, result, "desc %s", apri_struct_check_message(result));
        const APTEXT_LAYOUT_DESC &d = *desc;
        if (!d.chain)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's chain is null");
        if (aptext_missing(d.text, d.text_size))
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's text is null with a text_size of %u",
                               d.text_size);
        if (d.direction < APTEXT_DIRECTION_LTR)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's direction %u is not an APTEXT_DIRECTION",
                               (unsigned)d.direction);
        if (d.direction > APTEXT_DIRECTION_AUTO)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's direction %u is not an APTEXT_DIRECTION",
                               (unsigned)d.direction);
        if ((d.features & ~ALL_FEATURE_FLAGS) != 0)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's features has bits 0x%X, which are no feature",
                               d.features & ~ALL_FEATURE_FLAGS);
        if (aptext_missing(d.extra_features, d.extra_feature_count))
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT,
                               "desc's extra_features is null with an extra_feature_count of %u", d.extra_feature_count);
        for (uint32_t i = 0; i < d.extra_feature_count; i++)
        {
            if (is_flag_feature(d.extra_features[i].tag))
                return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT,
                                   "extra_features[%u] is a feature desc's features controls", i);
            for (uint32_t j = 0; j < i; j++)
            {
                if (d.extra_features[i].tag == d.extra_features[j].tag)
                    return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT,
                                       "extra_features[%u] has the tag of extra_features[%u]", i, j);
            }
        }
        if (!isfinite(d.em_size))
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's em_size is not finite");
        if (d.em_size <= 0.0f)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's em_size is not above 0");
        if (!isfinite(d.max_width))
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's max_width is not finite");
        if (d.max_width < 0.0f)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's max_width is negative");
        if (d.align < APTEXT_ALIGN_LEFT)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's align %u is not an APTEXT_ALIGN",
                               (unsigned)d.align);
        if (d.align > APTEXT_ALIGN_END)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's align %u is not an APTEXT_ALIGN",
                               (unsigned)d.align);
        if (!isfinite(d.line_spacing))
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's line_spacing is not finite");
        if (d.line_spacing <= 0.0f)
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's line_spacing is not above 0");
        if (!isfinite(d.letter_spacing))
            return aptext_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's letter_spacing is not finite");

        APRESULT result = build(layout, d, out_error);
        if (result != APRESULT_OK)
            empty_layout(layout);
        return result;
    }

    APRESULT aptext_layout_get_info(aptext_layout layout, APTEXT_LAYOUT_INFO *out_info)
    {
        if (!layout)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_info)
            return APRESULT_INVALID_ARGUMENT;
        if (APRESULT result = apri_struct_out_check(&out_info->base, APRI_STRUCT_TYPE_APTEXT_LAYOUT_INFO))
            return result;
        out_info->glyph_count = (uint32_t)layout->glyphs.count;
        out_info->line_count = (uint32_t)layout->lines.count;
        out_info->run_count = (uint32_t)layout->runs.count;
        out_info->missing_codepoint_count = layout->missing_codepoint_count;
        out_info->replaced_sequence_count = layout->replaced_sequence_count;
        out_info->width = layout->width;
        out_info->height = layout->height;
        return APRESULT_OK;
    }

    APRESULT aptext_layout_get_glyphs(aptext_layout layout, const APTEXT_GLYPH **out_glyphs, uint32_t *out_count)
    {
        if (!layout)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_glyphs)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_count)
            return APRESULT_INVALID_ARGUMENT;
        *out_glyphs = layout->glyphs.count ? layout->glyphs.data : nullptr;
        *out_count = (uint32_t)layout->glyphs.count;
        return APRESULT_OK;
    }

    APRESULT aptext_layout_get_lines(aptext_layout layout, const APTEXT_LINE **out_lines, uint32_t *out_count)
    {
        if (!layout)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_lines)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_count)
            return APRESULT_INVALID_ARGUMENT;
        *out_lines = layout->lines.count ? layout->lines.data : nullptr;
        *out_count = (uint32_t)layout->lines.count;
        return APRESULT_OK;
    }

    APRESULT aptext_layout_get_runs(aptext_layout layout, const APTEXT_RUN **out_runs, uint32_t *out_count)
    {
        if (!layout)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_runs)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_count)
            return APRESULT_INVALID_ARGUMENT;
        *out_runs = layout->runs.count ? layout->runs.data : nullptr;
        *out_count = (uint32_t)layout->runs.count;
        return APRESULT_OK;
    }

    APRESULT aptext_layout_get_strokes(aptext_layout layout, APTEXT_SEGMENT *segments, uint32_t capacity,
                                       uint32_t *out_count)
    {
        if (!layout)
            return APRESULT_INVALID_ARGUMENT;
        if (aptext_missing(segments, capacity))
            return APRESULT_INVALID_ARGUMENT;
        if (!out_count)
            return APRESULT_INVALID_ARGUMENT;
        *out_count = 0;
        if (layout->chain && layout->chain->kind != APTEXT_FACE_KIND_STROKE)
            return APRESULT_INVALID_ARGUMENT;
        if (!layout->chain)
            return APRESULT_OK; // empty: no glyph, no segment
        uint64_t total = 0;
        for (const APTEXT_GLYPH &g : layout->glyphs)
        {
            const aptext_face_t *face = layout->chain->faces[g.face_index];
            if (g.glyph == 0)
                continue;
            const aptext_stroke_glyph &sg = face->glyphs[g.glyph - 1];
            for (uint32_t p = 0; p < sg.polyline_count; p++)
                total += face->polylines[sg.first_polyline + p].point_count - 1;
        }
        if (total > UINT32_MAX)
            return APRESULT_LIMIT;
        *out_count = (uint32_t)total;
        if (capacity == 0)
            return APRESULT_OK;
        if (capacity < total)
            return APRESULT_LIMIT;
        const double em = (double)layout->em_size;
        uint32_t written = 0;
        for (uint32_t i = 0; i < layout->glyphs.count; i++)
        {
            const APTEXT_GLYPH &g = layout->glyphs[i];
            const aptext_face_t *face = layout->chain->faces[g.face_index];
            if (g.glyph == 0)
                continue;
            const aptext_stroke_glyph &sg = face->glyphs[g.glyph - 1];
            const ApriVec2 *points = face->points.data + sg.first_point;
            for (uint32_t p = 0; p < sg.polyline_count; p++)
            {
                const APTEXT_STROKE_POLYLINE &line = face->polylines[sg.first_polyline + p];
                for (uint32_t q = 0; q + 1 < line.point_count; q++)
                {
                    const ApriVec2 &a = points[line.first_point + q];
                    const ApriVec2 &b = points[line.first_point + q + 1];
                    APTEXT_SEGMENT &s = segments[written++];
                    s.x0 = (float)((double)g.x + (double)a.x * em);
                    s.y0 = (float)((double)g.y + (double)a.y * em);
                    s.x1 = (float)((double)g.x + (double)b.x * em);
                    s.y1 = (float)((double)g.y + (double)b.y * em);
                    s.glyph_index = i;
                }
            }
        }
        return APRESULT_OK;
    }

    void aptext_layout_destroy(aptext_layout layout)
    {
        if (!layout)
            return;
        layout->~aptext_layout_t();
        free(layout);
    }
}
