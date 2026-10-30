#version 450

// The batcher's line segments (aprendbatch.h). Everything is in pixels.
// A signed distance to the segment's shape (negative inside), from:
// - the body: the distance from the segment's line, less half the width;
// - each end with no neighbour: its cap (butt cuts at the end, square half
//   a width past it, round is the distance to the end point);
// - each corner: the stroke split along the corner's bisector, so a texel
//   belongs to one segment and is never blended twice; within this
//   segment's half, the body carried on is the mitre, cut by the chord
//   between the outer edges for a bevel (or a mitre past its limit), or the
//   distance to the corner point for a round join;
// - dashes: the distance along the line to the nearest dash, measured from
//   the line's start (the phase the record carries), each dash finished
//   with the line's cap.
// Coverage is a pixel's width across the zero.

#include "aprend_batch.glslh"
#include "aprend_batch_inputs.glslh"

float cross2(vec2 a, vec2 b) {
    return a.x * b.y - a.y * b.x;
}

// The corner at [corner] between this segment's direction [dir] (leaving
// it, pointing into the segment) and the neighbour's [other] (also
// pointing away from the corner): -1 where the texel is the neighbour's,
// else the extra constraint a bevel puts on it.
float corner_shape(vec2 p, vec2 corner, vec2 dir, vec2 other, float hw, uint join, float limit, out bool theirs) {
    // The bisector between the two directions away from the corner splits
    // the stroke; this segment owns the side its own direction is on.
    vec2 m = dir + other;
    m      = dot(m, m) > 1e-12 ? normalize(m) : vec2(-dir.y, dir.x);
    // Which side of the bisector this segment is: dir's.
    float own = cross2(m, dir);
    theirs    = cross2(m, p - corner) * own < 0.0;
    if (join != 2u && join != 1u)
        return -1e9;
    // The outer side: away from the bisector, which points into the angle
    // the two segments make.
    vec2 n_own     = vec2(-dir.y, dir.x);
    vec2 n_other   = vec2(-other.y, other.x);
    vec2 out_own   = dot(n_own, m) > 0.0 ? -n_own : n_own;
    vec2 out_other = dot(n_other, m) > 0.0 ? -n_other : n_other;
    vec2 mo        = out_own + out_other;
    if (dot(mo, mo) < 1e-12)
        return -1e9; // straight on: no corner
    mo          = normalize(mo);
    float chord = hw * dot(out_own, mo);
    if (join == 1u && chord > 0.0 && hw / chord <= limit)
        return -1e9; // the mitre, within its limit
    return dot(p - corner, mo) - chord;
}

void main() {
    vec2 p     = in_pixel;
    vec2 a     = in_a.xy;
    vec2 b     = in_a.zw;
    float hw   = in_c.x;
    uint flags = in_flags.x;
    uint cap   = (flags >> LINE_CAP_SHIFT) & 3u;
    uint join  = (flags >> LINE_JOIN_SHIFT) & 3u;
    vec2 d     = b - a;
    float len  = length(d);
    vec2 dir   = len > 0.0 ? d / len : vec2(1.0, 0.0);
    vec2 n     = vec2(-dir.y, dir.x);
    float t    = dot(p - a, dir);
    float s    = dot(p - a, n);

    float f = abs(s) - hw;
    // The start.
    if ((flags & LINE_HAS_PREV) != 0u) {
        vec2 back = in_b.xy - a;
        vec2 other = dot(back, back) > 0.0 ? normalize(back) : -dir;
        bool theirs;
        float bevel = corner_shape(p, a, dir, other, hw, join, in_c.y, theirs);
        if (theirs)
            discard;
        if (t < 0.0) {
            if (join == 3u)
                f = length(p - a) - hw;
            else
                f = max(f, bevel);
        }
    } else if (cap == 1u) {
        f = max(f, -t);
    } else if (cap == 2u) {
        f = max(f, -t - hw);
    } else if (t < 0.0) {
        f = length(p - a) - hw;
    }
    // The end.
    if ((flags & LINE_HAS_NEXT) != 0u) {
        vec2 ahead = in_b.zw - b;
        vec2 other = dot(ahead, ahead) > 0.0 ? normalize(ahead) : dir;
        bool theirs;
        float bevel = corner_shape(p, b, -dir, other, hw, join, in_c.y, theirs);
        if (theirs)
            discard;
        if (t > len) {
            if (join == 3u)
                f = length(p - b) - hw;
            else
                f = max(f, bevel);
        }
    } else if (cap == 1u) {
        f = max(f, t - len);
    } else if (cap == 2u) {
        f = max(f, t - len - hw);
    } else if (t > len) {
        f = length(p - b) - hw;
    }

    // Dashes: up to four dash-gap pairs, in pixels, from the line's start.
    float pattern = in_c.w;
    if (pattern > 0.0) {
        float pos = mod(in_c.z + clamp(t, 0.0, len), pattern);
        float lengths[8] = float[8](in_d0.x, in_d0.y, in_d0.z, in_d0.w, in_d1.x, in_d1.y, in_d1.z, in_d1.w);
        // The distance along the line to the nearest dash: negative inside one.
        float along = 1e9;
        float start = 0.0;
        for (int i = 0; i < 8; i += 2) {
            float dash = lengths[i];
            float gap  = lengths[i + 1];
            if (dash + gap <= 0.0)
                break;
            float end = start + dash;
            // The dash here, and its copies a pattern before and after.
            for (int k = -1; k <= 1; ++k) {
                float lo = start + float(k) * pattern;
                float hi = end + float(k) * pattern;
                along    = min(along, max(lo - pos, pos - hi));
            }
            start = end + gap;
        }
        float dash_f;
        if (cap == 3u)
            dash_f = along <= 0.0 ? -1e9 : length(vec2(along, s)) - hw;
        else if (cap == 2u)
            dash_f = along - hw;
        else
            dash_f = along;
        f = max(f, dash_f);
    }
    out_color = batch_premultiply(in_color, clamp(0.5 - f, 0.0, 1.0));
}
