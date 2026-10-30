#include "aptext_internal.hpp"
#include <math.h>
#include <string.h>

// The CAD single-stroke formats (APTEXT_TODO.md item 4, card A20): LFF,
// LibreCAD's text format, and SHX, AutoCAD's compiled shapes. Each reader
// fills an aptext_stroke_font (em units, y up) that aptext.cpp turns into a
// stroke face as it does a Hershey font's.
//
// What they compute must be the same bits everywhere (as the rest of a
// stroke face is): arcs are cut by halving, with square roots only, which
// IEEE rounds exactly; the one place a sine is needed (an SHX fractional
// arc's ends) uses a series here, not the platform's library; numbers are
// parsed here, not by strtod, which follows the locale.

// Too much for any font: a file asking for more is refused, not followed.
static const uint64_t MAX_POINTS = 1u << 22;
static const uint32_t MAX_STEPS = 1u << 18; // SHX commands a glyph
static const int MAX_ARC_DEPTH = 16;         // halvings of one arc

// ---- Building polylines -----------------------------------------------------------

namespace
{
struct pen_t
{
    aptext_stroke_font *font;
    double to_em;         // file units to em
    double tolerance;     // in file units
    uint32_t line_start;  // the open polyline's first point, or UINT32_MAX
    APTEXT_ERROR *error;
    APRESULT failed = APRESULT_OK;
};
} // namespace

static bool pen_point(pen_t &pen, double x, double y)
{
    if (pen.failed)
        return false;
    if (pen.font->points.count >= MAX_POINTS)
    {
        pen.failed = aptext_fail(pen.error, APRESULT_APTEXT_NOT_A_FONT, "the font draws more than %u points",
                                 (unsigned)MAX_POINTS);
        return false;
    }
    ApriVec2 p;
    p.x = (float)(x * pen.to_em);
    p.y = (float)(y * pen.to_em);
    if (pen.font->points.push(p) != EQ_SUCCESS)
    {
        pen.failed = aptext_out_of_memory(pen.error, "reading the font");
        return false;
    }
    return true;
}

// Ends the open polyline; one of a single point becomes a zero-length line,
// so a dot still draws (as Hershey's).
static bool pen_end(pen_t &pen)
{
    if (pen.line_start == UINT32_MAX || pen.failed)
    {
        pen.line_start = UINT32_MAX;
        return !pen.failed;
    }
    uint32_t count = (uint32_t)pen.font->points.count - pen.line_start;
    if (count == 1)
    {
        ApriVec2 dot = pen.font->points[pen.line_start];
        if (pen.font->points.push(dot) != EQ_SUCCESS)
        {
            pen.failed = aptext_out_of_memory(pen.error, "reading the font");
            return false;
        }
        count = 2;
    }
    APTEXT_STROKE_POLYLINE line = {pen.line_start, count};
    pen.line_start = UINT32_MAX;
    if (pen.font->polylines.push(line) != EQ_SUCCESS)
    {
        pen.failed = aptext_out_of_memory(pen.error, "reading the font");
        return false;
    }
    return true;
}

// Starts a polyline at (x, y) unless one is open.
static bool pen_start(pen_t &pen, double x, double y)
{
    if (pen.line_start != UINT32_MAX)
        return true;
    pen.line_start = (uint32_t)pen.font->points.count;
    return pen_point(pen, x, y);
}

// The arc from (ax, ay) to (bx, by) of bulge k (the tangent of a quarter of
// its angle, positive anticlockwise), as points past the first: halved
// until its sagitta is within the tolerance. The arc's midpoint is the
// chord's, moved out by the sagitta k c / 2 to the chord's right; each half
// has bulge k / (1 + sqrt(1 + k^2)).
static bool pen_arc(pen_t &pen, double ax, double ay, double bx, double by, double k, int depth)
{
    const double dx = bx - ax, dy = by - ay;
    const double chord = sqrt(dx * dx + dy * dy);
    const double sagitta = k * chord / 2.0;
    if (depth >= MAX_ARC_DEPTH || chord == 0.0 || fabs(sagitta) <= pen.tolerance)
        return pen_point(pen, bx, by);
    const double mx = (ax + bx) / 2.0 + dy / chord * sagitta;
    const double my = (ay + by) / 2.0 - dx / chord * sagitta;
    const double half = k / (1.0 + sqrt(1.0 + k * k));
    return pen_arc(pen, ax, ay, mx, my, half, depth + 1) && pen_arc(pen, mx, my, bx, by, half, depth + 1);
}

// ---- Numbers ----------------------------------------------------------------------

// A decimal number at [*at], [+-]digits[.digits][e[+-]digits], moving [*at]
// past it: the mantissa's first 19 digits times a power of ten, each step
// one IEEE operation, so the same bits everywhere. False if there is none.
static bool parse_number(const char *text, size_t size, size_t *at, double *out)
{
    static const double powers[23] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                                       1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    size_t i = *at;
    while (i < size && (text[i] == ' ' || text[i] == '\t'))
        i++;
    bool negative = false;
    if (i < size && (text[i] == '-' || text[i] == '+'))
        negative = text[i++] == '-';
    uint64_t mantissa = 0;
    int digits = 0, exponent = 0;
    bool any = false;
    while (i < size && text[i] >= '0' && text[i] <= '9')
    {
        if (digits < 19)
            mantissa = mantissa * 10 + (uint64_t)(text[i] - '0'), digits += mantissa != 0;
        else
            exponent++;
        any = true;
        i++;
    }
    if (i < size && text[i] == '.')
    {
        i++;
        while (i < size && text[i] >= '0' && text[i] <= '9')
        {
            if (digits < 19)
            {
                mantissa = mantissa * 10 + (uint64_t)(text[i] - '0');
                digits += mantissa != 0;
                exponent--;
            }
            any = true;
            i++;
        }
    }
    if (!any)
        return false;
    if (i < size && (text[i] == 'e' || text[i] == 'E'))
    {
        size_t e = i + 1;
        bool e_negative = false;
        if (e < size && (text[e] == '-' || text[e] == '+'))
            e_negative = text[e++] == '-';
        int value = 0;
        bool e_any = false;
        while (e < size && text[e] >= '0' && text[e] <= '9')
        {
            if (value < 10000)
                value = value * 10 + (text[e] - '0');
            e_any = true;
            e++;
        }
        if (e_any)
        {
            exponent += e_negative ? -value : value;
            i = e;
        }
    }
    double v = (double)mantissa;
    while (exponent > 22)
        v *= 1e22, exponent -= 22;
    while (exponent < -22)
        v /= 1e22, exponent += 22;
    v = exponent >= 0 ? v * powers[exponent] : v / powers[-exponent];
    *out = negative ? -v : v;
    *at = i;
    return true;
}

static bool parse_hex(const char *text, size_t size, size_t *at, uint32_t *out)
{
    size_t i = *at;
    uint32_t value = 0;
    bool any = false;
    while (i < size)
    {
        char c = text[i];
        uint32_t digit;
        if (c >= '0' && c <= '9')
            digit = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f')
            digit = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            digit = (uint32_t)(c - 'A' + 10);
        else
            break;
        if (value > 0x10FFFFu)
            return false;
        value = value * 16 + digit;
        any = true;
        i++;
    }
    if (!any || value > 0x10FFFFu)
        return false;
    *out = value;
    *at = i;
    return true;
}

// ---- LFF ------------------------------------------------------------------------

// A header line's value for [key] ("# Key: value"), if it is that key's.
static bool header_value(const char *line, size_t size, const char *key, double *out)
{
    size_t i = 1;
    while (i < size && (line[i] == ' ' || line[i] == '\t'))
        i++;
    const size_t key_size = strlen(key);
    if (size - i < key_size || memcmp(line + i, key, key_size) != 0)
        return false;
    i += key_size;
    while (i < size && (line[i] == ' ' || line[i] == '\t'))
        i++;
    if (i >= size || line[i] != ':')
        return false;
    i++;
    double v;
    if (!parse_number(line, size, &i, &v) || !isfinite(v))
        return false;
    *out = v;
    return true;
}

APRESULT aptext_read_lff(const APTEXT_LFF_FACE_DESC &d, aptext_stroke_font &font, APTEXT_ERROR *error)
{
    const char *text = (const char *)d.data;
    const size_t size = (size_t)d.data_size;
    // The format's own defaults, LibreCAD's, where a header leaves them out.
    double letter_spacing = 3.0, word_spacing = 6.75;
    pen_t pen{&font, 1.0 / (double)d.units_per_em, (double)d.arc_tolerance * (double)d.units_per_em, UINT32_MAX, error};
    bool in_glyph = false;
    bool have_space = false;
    APTEXT_STROKE_GLYPH glyph = {};
    double right = 0.0; // the open glyph's rightmost x, in file units
    bool any_point = false;
    uint32_t line_number = 0;
    eq::array<double> rights; // each glyph's rightmost x, for references

    auto close_glyph = [&]() -> APRESULT {
        if (!in_glyph)
            return APRESULT_OK;
        in_glyph = false;
        glyph.polyline_count = (uint32_t)font.polylines.count - glyph.first_polyline;
        double advance = glyph.codepoint == 0x20 ? word_spacing : (any_point ? right : 0.0) + letter_spacing;
        if (advance < 0.0)
            advance = 0.0;
        glyph.advance = (float)(advance / (double)d.units_per_em);
        if (!isfinite(glyph.advance))
            return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "line %u: a glyph's advance is out of range", line_number);
        if (font.glyphs.push(glyph) != EQ_SUCCESS || rights.push(any_point ? right : 0.0) != EQ_SUCCESS)
            return aptext_out_of_memory(error, "reading the LFF font");
        return APRESULT_OK;
    };

    size_t at = 0;
    while (at < size)
    {
        size_t end = at;
        while (end < size && text[end] != '\n')
            end++;
        size_t line_end = end;
        if (line_end > at && text[line_end - 1] == '\r')
            line_end--;
        const char *line = text + at;
        const size_t length = line_end - at;
        at = end < size ? end + 1 : end;
        line_number++;

        size_t first = 0;
        while (first < length && (line[first] == ' ' || line[first] == '\t'))
            first++;
        if (first == length)
        {
            if (APRESULT result = close_glyph())
                return result;
            continue;
        }
        if (line[first] == '#')
        {
            if (!in_glyph)
            {
                double v;
                if (header_value(line + first, length - first, "LetterSpacing", &v))
                    letter_spacing = v;
                else if (header_value(line + first, length - first, "WordSpacing", &v))
                    word_spacing = v;
            }
            continue;
        }
        if (line[first] == '[')
        {
            if (APRESULT result = close_glyph())
                return result;
            size_t i = first + 1;
            if (i < length && line[i] == '#')
                i++;
            uint32_t codepoint;
            if (!parse_hex(line, length, &i, &codepoint) || i >= length || line[i] != ']')
                return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "line %u: a glyph's codepoint isn't [hhhh]",
                                   line_number);
            if (!aptext_is_valid_codepoint(codepoint))
                return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "line %u: 0x%X is not a codepoint", line_number,
                                   codepoint);
            for (const APTEXT_STROKE_GLYPH &g : font.glyphs)
                if (g.codepoint == codepoint)
                    return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "line %u: U+%04X is in the font twice",
                                       line_number, codepoint);
            have_space = have_space || codepoint == 0x20;
            glyph = {};
            glyph.codepoint = codepoint;
            glyph.first_polyline = (uint32_t)font.polylines.count;
            right = 0.0;
            any_point = false;
            in_glyph = true;
            continue;
        }
        if (!in_glyph)
            return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "line %u: strokes outside a glyph", line_number);
        if (line[first] == 'C' || line[first] == 'c')
        {
            size_t i = first + 1;
            uint32_t codepoint;
            if (!parse_hex(line, length, &i, &codepoint))
                return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "line %u: a reference isn't Chhhh", line_number);
            uint32_t found = UINT32_MAX;
            for (uint32_t g = 0; g < font.glyphs.count; g++)
                if (font.glyphs[g].codepoint == codepoint)
                    found = g;
            if (found == UINT32_MAX)
                return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT,
                                   "line %u: U+%04X is referred to before it is read", line_number, codepoint);
            // Its polylines, copied: they index points of their own.
            const APTEXT_STROKE_GLYPH source = font.glyphs[found];
            for (uint32_t p = 0; p < source.polyline_count; p++)
            {
                const APTEXT_STROKE_POLYLINE from = font.polylines[source.first_polyline + p];
                APTEXT_STROKE_POLYLINE copy = {(uint32_t)font.points.count, from.point_count};
                if (font.points.count + from.point_count > MAX_POINTS)
                    return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "the font draws more than %u points",
                                       (unsigned)MAX_POINTS);
                for (uint32_t q = 0; q < from.point_count; q++)
                {
                    ApriVec2 point = font.points[from.first_point + q];
                    if (font.points.push(point) != EQ_SUCCESS)
                        return aptext_out_of_memory(error, "reading the LFF font");
                }
                if (font.polylines.push(copy) != EQ_SUCCESS)
                    return aptext_out_of_memory(error, "reading the LFF font");
            }
            if (source.polyline_count && (!any_point || rights[found] > right))
                right = rights[found];
            any_point = any_point || source.polyline_count;
            continue;
        }
        // A polyline: vertices "x,y" or "x,y,Ab", separated by ';'.
        size_t i = first;
        bool have_last = false;
        double last_x = 0.0, last_y = 0.0;
        while (i < length)
        {
            double x, y, bulge = 0.0;
            bool arc = false;
            if (!parse_number(line, length, &i, &x))
                return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "line %u: a vertex isn't x,y", line_number);
            while (i < length && line[i] == ' ')
                i++;
            if (i >= length || line[i] != ',')
                return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "line %u: a vertex isn't x,y", line_number);
            i++;
            if (!parse_number(line, length, &i, &y))
                return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "line %u: a vertex isn't x,y", line_number);
            while (i < length && line[i] == ' ')
                i++;
            if (i < length && line[i] == ',')
            {
                i++;
                while (i < length && line[i] == ' ')
                    i++;
                if (i >= length || (line[i] != 'A' && line[i] != 'a'))
                    return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "line %u: a vertex's third field isn't Ab",
                                       line_number);
                i++;
                if (!parse_number(line, length, &i, &bulge))
                    return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "line %u: an arc's bulge isn't a number",
                                       line_number);
                arc = true;
            }
            const double limit = 1e6;
            if (!(fabs(x) <= limit) || !(fabs(y) <= limit) || !(fabs(bulge) <= limit))
                return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "line %u: a value is out of range", line_number);
            if (!have_last)
            {
                if (!pen_start(pen, x, y))
                    return pen.failed;
            }
            else if (arc && bulge != 0.0)
            {
                if (!pen_arc(pen, last_x, last_y, x, y, bulge, 0))
                    return pen.failed;
            }
            else if (!pen_point(pen, x, y))
                return pen.failed;
            if (!any_point || x > right)
                right = x;
            any_point = true;
            have_last = true;
            last_x = x;
            last_y = y;
            while (i < length && line[i] == ' ')
                i++;
            if (i < length && line[i] == ';')
                i++;
            else if (i < length)
                return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "line %u: vertices aren't separated by ';'",
                                   line_number);
        }
        if (!pen_end(pen))
            return pen.failed;
    }
    if (APRESULT result = close_glyph())
        return result;
    if (font.glyphs.count == 0)
        return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "the text holds no LFF glyph");
    // LibreCAD's fonts leave the space out: it advances by the word spacing.
    if (!have_space)
    {
        APTEXT_STROKE_GLYPH space = {};
        space.codepoint = 0x20;
        space.first_polyline = (uint32_t)font.polylines.count;
        space.advance = (float)((word_spacing < 0.0 ? 0.0 : word_spacing) / (double)d.units_per_em);
        if (font.glyphs.push(space) != EQ_SUCCESS)
            return aptext_out_of_memory(error, "reading the LFF font");
    }
    return APRESULT_OK;
}

// ---- SHX ---------------------------------------------------------------------------

namespace
{
// A shape's definition: where its commands are in the file.
struct shape_t
{
    uint32_t number;
    uint32_t offset; // its commands, past its name
    uint32_t size;
};

struct shx_t
{
    const uint8_t *data;
    uint32_t size;
    bool unifont = false;
    eq::array<shape_t> shapes;
    eq::array<uint32_t> index; // shape number to shapes' index, or UINT32_MAX
};

struct turtle_t
{
    double x = 0.0, y = 0.0;
    double scale = 1.0;
    bool down = true;
    double stack_x[16];
    double stack_y[16];
    int depth = 0; // pushed
    uint32_t steps = 0;
};
} // namespace

// sin and cos of [r] radians, |r| up to a little past pi/4, by series:
// error far under a float's, the same on every platform.
static void sin_cos(double r, double *s, double *c)
{
    const double r2 = r * r;
    double sine = 0.0, cosine = 0.0, term_s = r, term_c = 1.0;
    for (int n = 0; n < 12; n++)
    {
        sine += term_s;
        cosine += term_c;
        term_s *= -r2 / (double)((2 * n + 2) * (2 * n + 3));
        term_c *= -r2 / (double)((2 * n + 1) * (2 * n + 2));
    }
    *s = sine;
    *c = cosine;
}

// The point at [degrees] on a unit circle: its octant's corner turned by
// the rest, so the series sees under 45 degrees.
static void unit_point(double degrees, double *x, double *y)
{
    const double h = sqrt(0.5);
    double octant = floor(degrees / 45.0);
    double rest = degrees - octant * 45.0;
    int k = (int)fmod(fmod(octant, 8.0) + 8.0, 8.0);
    const double base_c[8] = {1.0, h, 0.0, -h, -1.0, -h, 0.0, h};
    const double base_s[8] = {0.0, h, 1.0, h, 0.0, -h, -1.0, -h};
    double s, c;
    sin_cos(rest * (3.14159265358979323846 / 180.0), &s, &c);
    *x = base_c[k] * c - base_s[k] * s;
    *y = base_s[k] * c + base_c[k] * s;
}

// The 16 directions of a vector byte's low nibble.
static const double s_directions[16][2] = {{1, 0},  {1, 0.5},   {1, 1},   {0.5, 1},  {0, 1},  {-0.5, 1},
                                            {-1, 1}, {-1, 0.5},  {-1, 0},  {-1, -0.5}, {-1, -1}, {-0.5, -1},
                                            {0, -1}, {0.5, -1},  {1, -1},  {1, -0.5}};

static bool shx_move(pen_t &pen, turtle_t &t, double dx, double dy)
{
    const double x = t.x + dx, y = t.y + dy;
    if (t.down)
    {
        if (!pen_start(pen, t.x, t.y) || !pen_point(pen, x, y))
            return false;
    }
    t.x = x;
    t.y = y;
    return true;
}

// An arc of bulge [k] from the turtle to (dx, dy) from it.
static bool shx_arc(pen_t &pen, turtle_t &t, double dx, double dy, double k)
{
    const double x = t.x + dx, y = t.y + dy;
    if (t.down)
    {
        if (!pen_start(pen, t.x, t.y) || !pen_arc(pen, t.x, t.y, x, y, k, 0))
            return false;
    }
    t.x = x;
    t.y = y;
    return true;
}

// An arc round the circle of [radius] through the turtle from [start] to
// [end] degrees (the turtle is at start), in steps under a half turn, each
// an arc of bulge tan(step / 4).
static bool shx_circle_arc(pen_t &pen, turtle_t &t, double radius, double start, double end)
{
    double sx, sy;
    unit_point(start, &sx, &sy);
    const double cx = t.x - radius * sx, cy = t.y - radius * sy;
    const double sweep = end - start;
    const int steps = fabs(sweep) > 180.0 ? 4 : 1;
    for (int i = 1; i <= steps; i++)
    {
        const double a = start + sweep * (double)i / (double)steps;
        double ex, ey, qs, qc;
        unit_point(a, &ex, &ey);
        sin_cos(sweep / (double)steps / 4.0 * (3.14159265358979323846 / 180.0), &qs, &qc);
        if (!shx_arc(pen, t, cx + radius * ex - t.x, cy + radius * ey - t.y, qs / qc))
            return false;
        // Land exactly on the circle, not on the sum of the steps.
        t.x = cx + radius * ex;
        t.y = cy + radius * ey;
    }
    return true;
}

static int8_t s8(uint8_t b)
{
    return (int8_t)b;
}

static APRESULT shx_run(const shx_t &shx, uint32_t number, int nesting, turtle_t &t, pen_t &pen, APTEXT_ERROR *error);

// How many bytes the command at [at] takes, for code 14's skip; 0 if it
// runs past [end].
static uint32_t shx_command_size(const shx_t &shx, uint32_t at, uint32_t end)
{
    const uint8_t *d = shx.data;
    const uint8_t code = d[at];
    uint32_t size;
    switch (code)
    {
    case 3:
    case 4:
        size = 2;
        break;
    case 7:
        size = shx.unifont ? 3 : 2;
        break;
    case 8:
        size = 3;
        break;
    case 10:
        size = 3;
        break;
    case 11:
        size = 6;
        break;
    case 12:
        size = 4;
        break;
    case 9:
    case 13:
    {
        uint32_t i = at + 1;
        for (;;)
        {
            if (end - i < 2)
                return 0;
            if (d[i] == 0 && d[i + 1] == 0)
                return i + 2 - at;
            i += code == 9 ? 2 : 3;
            if (i > end)
                return 0;
        }
    }
    default:
        size = 1;
        break;
    }
    return end - at >= size ? size : 0;
}

static APRESULT shx_run(const shx_t &shx, uint32_t number, int nesting, turtle_t &t, pen_t &pen, APTEXT_ERROR *error)
{
    if (nesting > 8)
        return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "subshapes nest past 8 deep");
    if (number >= shx.index.count || shx.index[number] == UINT32_MAX)
        return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "a shape draws shape %u, which the file hasn't", number);
    const shape_t &shape = shx.shapes[shx.index[number]];
    const uint8_t *d = shx.data;
    uint32_t at = shape.offset;
    const uint32_t end = shape.offset + shape.size;
    auto cut = [&]() { return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "shape %u is cut short", number); };
    while (at < end)
    {
        if (++t.steps > MAX_STEPS)
            return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "shape %u takes more than %u steps", number,
                               (unsigned)MAX_STEPS);
        const uint8_t code = d[at];
        if (code >= 16)
        {
            const double length = (double)(code >> 4) * t.scale;
            if (!shx_move(pen, t, s_directions[code & 15][0] * length, s_directions[code & 15][1] * length))
                return pen.failed;
            at++;
            continue;
        }
        switch (code)
        {
        case 0:
            return APRESULT_OK;
        case 1:
            t.down = true;
            at++;
            break;
        case 2:
            t.down = false;
            if (!pen_end(pen))
                return pen.failed;
            at++;
            break;
        case 3:
        case 4:
            if (end - at < 2)
                return cut();
            if (d[at + 1] == 0)
                return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "shape %u scales by 0", number);
            t.scale = code == 3 ? t.scale / (double)d[at + 1] : t.scale * (double)d[at + 1];
            if (!(t.scale < 1e6) || !(t.scale > 1e-6))
                return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "shape %u scales out of range", number);
            at += 2;
            break;
        case 5:
            if (t.depth >= 16)
                return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "shape %u pushes past 16", number);
            t.stack_x[t.depth] = t.x;
            t.stack_y[t.depth] = t.y;
            t.depth++;
            at++;
            break;
        case 6:
            if (t.depth == 0)
                return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "shape %u pops an empty stack", number);
            t.depth--;
            // The pen jumps: a new polyline starts where it lands.
            if (!pen_end(pen))
                return pen.failed;
            t.x = t.stack_x[t.depth];
            t.y = t.stack_y[t.depth];
            at++;
            break;
        case 7:
        {
            const uint32_t size = shx.unifont ? 3 : 2;
            if (end - at < size)
                return cut();
            const uint32_t sub = shx.unifont ? (uint32_t)d[at + 1] << 8 | d[at + 2] : d[at + 1];
            if (APRESULT result = shx_run(shx, sub, nesting + 1, t, pen, error))
                return result;
            at += size;
            break;
        }
        case 8:
            if (end - at < 3)
                return cut();
            if (!shx_move(pen, t, s8(d[at + 1]) * t.scale, s8(d[at + 2]) * t.scale))
                return pen.failed;
            at += 3;
            break;
        case 9:
            at++;
            for (;;)
            {
                if (end - at < 2)
                    return cut();
                const uint8_t dx = d[at], dy = d[at + 1];
                at += 2;
                if (dx == 0 && dy == 0)
                    break;
                if (!shx_move(pen, t, s8(dx) * t.scale, s8(dy) * t.scale))
                    return pen.failed;
            }
            break;
        case 10:
        {
            // Octant arc: a radius, then direction (bit 7, clockwise),
            // starting octant (bits 4-6) and count (bits 0-2, 0 for 8).
            if (end - at < 3)
                return cut();
            const double radius = (double)d[at + 1] * t.scale;
            const uint8_t o = d[at + 2];
            const int start = (o >> 4) & 7;
            const int count = (o & 7) ? (o & 7) : 8;
            const bool clockwise = (o & 0x80) != 0;
            const double from = start * 45.0;
            if (!shx_circle_arc(pen, t, radius, from, from + (clockwise ? -45.0 : 45.0) * count))
                return pen.failed;
            at += 3;
            break;
        }
        case 11:
        {
            // Fractional arc: start offset, end offset (each 45/256 of a
            // degree past an octant boundary), the radius's high and low
            // bytes, then the octant byte as code 10's. It runs from the
            // start octant plus its offset to the last octant it spans plus
            // the end offset.
            if (end - at < 6)
                return cut();
            const double start_offset = d[at + 1] * (45.0 / 256.0);
            const double end_offset = d[at + 2] * (45.0 / 256.0);
            const double radius = (double)((uint32_t)d[at + 3] << 8 | d[at + 4]) * t.scale;
            const uint8_t o = d[at + 5];
            const int start = (o >> 4) & 7;
            const int count = (o & 7) ? (o & 7) : 8;
            const bool clockwise = (o & 0x80) != 0;
            const double from = start * 45.0 + start_offset;
            const int last = clockwise ? start - (count - 1) : start + (count - 1);
            const double to = last * 45.0 + end_offset;
            if (!shx_circle_arc(pen, t, radius, from, to))
                return pen.failed;
            at += 6;
            break;
        }
        case 12:
            if (end - at < 4)
                return cut();
            if (!shx_arc(pen, t, s8(d[at + 1]) * t.scale, s8(d[at + 2]) * t.scale, s8(d[at + 3]) / 127.0))
                return pen.failed;
            at += 4;
            break;
        case 13:
            at++;
            for (;;)
            {
                if (end - at < 2)
                    return cut();
                if (d[at] == 0 && d[at + 1] == 0)
                {
                    at += 2;
                    break;
                }
                if (end - at < 3)
                    return cut();
                if (!shx_arc(pen, t, s8(d[at]) * t.scale, s8(d[at + 1]) * t.scale, s8(d[at + 2]) / 127.0))
                    return pen.failed;
                at += 3;
            }
            break;
        case 14:
        {
            // The next command is for vertical text only: skipped.
            at++;
            if (at >= end)
                return cut();
            const uint32_t size = shx_command_size(shx, at, end);
            if (size == 0)
                return cut();
            at += size;
            break;
        }
        default:
            return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "shape %u has command %u, which isn't SHX", number,
                               code);
        }
    }
    return cut();
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// Adds a shape whose definition is [size] bytes at [at]: a name ending in
// NUL, then its commands.
static APRESULT add_shape(shx_t &shx, uint32_t number, uint32_t at, uint32_t size, APTEXT_ERROR *error)
{
    if (at > shx.size || size > shx.size - at)
        return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "shape %u runs past the file", number);
    const void *nul = memchr(shx.data + at, 0, size);
    if (!nul)
        return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "shape %u's name doesn't end", number);
    const uint32_t name = (uint32_t)((const uint8_t *)nul - (shx.data + at)) + 1;
    if (number < shx.index.count && shx.index[number] != UINT32_MAX)
        return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "shape %u is in the file twice", number);
    shape_t shape = {number, at + name, size - name};
    if (shx.shapes.push(shape) != EQ_SUCCESS)
        return aptext_out_of_memory(error, "reading the SHX font");
    shx.index[number] = (uint32_t)shx.shapes.count - 1;
    return APRESULT_OK;
}

APRESULT aptext_read_shx(const APTEXT_SHX_FACE_DESC &d, aptext_stroke_font &font, float *out_descender,
                         APTEXT_ERROR *error)
{
    if (d.data_size > 0xFFFFFFFFull)
        return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "the file is over 4 GiB, more than SHX addresses");
    shx_t shx;
    shx.data = (const uint8_t *)d.data;
    shx.size = (uint32_t)d.data_size;
    if (shx.index.resize(65536) != EQ_SUCCESS)
        return aptext_out_of_memory(error, "reading the SHX font");
    for (uint32_t &i : shx.index)
        i = UINT32_MAX;
    static const char shapes_10[] = "AutoCAD-86 shapes 1.0\r\n\x1a";
    static const char shapes_11[] = "AutoCAD-86 shapes 1.1\r\n\x1a";
    static const char unifont[] = "AutoCAD-86 unifont 1.0\r\n\x1a";
    static const char bigfont[] = "AutoCAD-86 bigfont";
    const uint8_t *p = shx.data;
    uint32_t above = 0, below = 0;
    if (shx.size >= sizeof(bigfont) - 1 && memcmp(p, bigfont, sizeof(bigfont) - 1) == 0)
        return aptext_fail(error, APRESULT_UNSUPPORTED, "the file is an SHX big font, which isn't read");
    if (shx.size >= sizeof(unifont) - 1 && memcmp(p, unifont, sizeof(unifont) - 1) == 0)
    {
        shx.unifont = true;
        uint32_t at = sizeof(unifont) - 1;
        if (shx.size - at < 6)
            return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "the unifont's header is cut short");
        const uint32_t count = le32(p + at);
        const uint32_t info_size = le16(p + at + 4);
        at += 6;
        if (info_size > shx.size - at)
            return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "the unifont's information runs past the file");
        const void *nul = memchr(p + at, 0, info_size);
        const uint32_t name = nul ? (uint32_t)((const uint8_t *)nul - (p + at)) + 1 : info_size;
        if (info_size - name < 2)
            return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "the unifont's information is cut short");
        above = p[at + name];
        below = p[at + name + 1];
        at += info_size;
        // The count is the shapes', the information's included.
        for (uint32_t i = 1; i < count && at < shx.size; i++)
        {
            if (shx.size - at < 4)
                return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "a shape's header is cut short");
            const uint32_t number = le16(p + at), size = le16(p + at + 2);
            if (APRESULT result = add_shape(shx, number, at + 4, size, error))
                return result;
            at += 4 + size;
        }
    }
    else if (shx.size >= sizeof(shapes_10) - 1 &&
             (memcmp(p, shapes_10, sizeof(shapes_10) - 1) == 0 || memcmp(p, shapes_11, sizeof(shapes_11) - 1) == 0))
    {
        uint32_t at = sizeof(shapes_10) - 1;
        if (shx.size - at < 6)
            return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "the shapes' header is cut short");
        const uint32_t count = le16(p + at + 4);
        at += 6;
        if ((uint64_t)count * 4 > shx.size - at)
            return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "the shapes' index runs past the file");
        uint32_t def = at + count * 4;
        for (uint32_t i = 0; i < count; i++)
        {
            const uint32_t number = le16(p + at + 4 * i), size = le16(p + at + 4 * i + 2);
            if (APRESULT result = add_shape(shx, number, def, size, error))
                return result;
            def += size;
        }
        if (shx.index[0] == UINT32_MAX)
            return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "the file has no shape 0, its font information");
        const shape_t &info = shx.shapes[shx.index[0]];
        if (info.size < 2)
            return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "the font information is cut short");
        above = p[info.offset];
        below = p[info.offset + 1];
    }
    else
        return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "the bytes are not an SHX font");
    if (above == 0)
        return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "the font's above height is 0");
    *out_descender = -(float)below / (float)above;

    pen_t pen{&font, 1.0 / (double)above, (double)d.arc_tolerance * (double)above, UINT32_MAX, error};
    for (const shape_t &shape : shx.shapes)
    {
        // Controls (and the line feed shape) are subshapes only.
        if (shape.number < 32)
            continue;
        uint32_t codepoint;
        if (shx.unifont)
            codepoint = shape.number;
        else if (shape.number < 128)
            codepoint = shape.number;
        else if (shape.number - 128 < d.high_codepoint_count)
            codepoint = d.high_codepoints[shape.number - 128];
        else
            continue;
        if (!aptext_is_valid_codepoint(codepoint))
            continue;
        bool twice = false;
        for (const APTEXT_STROKE_GLYPH &g : font.glyphs)
            twice = twice || g.codepoint == codepoint;
        if (twice)
            return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "two shapes draw U+%04X", codepoint);
        APTEXT_STROKE_GLYPH glyph = {};
        glyph.codepoint = codepoint;
        glyph.first_polyline = (uint32_t)font.polylines.count;
        turtle_t t;
        if (APRESULT result = shx_run(shx, shape.number, 0, t, pen, error))
            return result;
        if (!pen_end(pen))
            return pen.failed;
        glyph.polyline_count = (uint32_t)font.polylines.count - glyph.first_polyline;
        // Where the pen ends is where the next glyph starts.
        glyph.advance = t.x > 0.0 ? (float)(t.x / (double)above) : 0.0f;
        if (font.glyphs.push(glyph) != EQ_SUCCESS)
            return aptext_out_of_memory(error, "reading the SHX font");
    }
    if (font.glyphs.count == 0)
        return aptext_fail(error, APRESULT_APTEXT_NOT_A_FONT, "no shape of the file draws a codepoint");
    return APRESULT_OK;
}
