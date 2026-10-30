#version 450

// The batcher's vertex shader (aprendbatch.h): one record a shape, read
// per instance, expanded here into two triangles (six vertices). Quads,
// glyphs and circles are a parallelogram from a corner and two edges, in
// either space; a line segment is a rectangle round it in pixels, grown
// past its ends by what its caps and joins reach, its position in pixels
// handed on for the fragment shader to measure from.

#include "aprend_batch.glslh"

layout(location = 0) in vec4 in_r0;
layout(location = 1) in vec4 in_r1;
layout(location = 2) in vec4 in_r2;
layout(location = 3) in vec4 in_r3;
layout(location = 4) in vec4 in_r4;
layout(location = 5) in vec4 in_r5;
layout(location = 6) in vec4 in_r6;
layout(location = 7) in vec4 in_r7;

layout(location = 0) out vec2 out_local;              // quad, glyph: uv; circle: -1 to 1 across
layout(location = 1) noperspective out vec2 out_pixel; // line: the fragment's pixel
layout(location = 2) flat out vec4 out_color;
layout(location = 3) flat out vec4 out_a;             // line: start (xy), end (zw), in pixels
layout(location = 4) flat out vec4 out_b;             // line: previous point (xy), next (zw)
layout(location = 5) flat out vec4 out_c;             // line: half width, mitre limit, dash phase, pattern length
layout(location = 6) flat out vec4 out_d0;            // dashes 0-3; quad, glyph: uv rect; circle: inner, start, sweep
layout(location = 7) flat out vec4 out_d1;            // dashes 4-7; glyph: outline color
layout(location = 8) flat out uvec2 out_flags;        // x: flags; y: glyph: layer
layout(location = 9) flat out float out_extra;        // glyph: outline width

const vec2 CORNERS[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0), vec2(0.0, 1.0), vec2(1.0, 0.0), vec2(1.0, 1.0));

void main() {
    uint flags = uint(in_r3.w);
    uint kind  = flags & 3u;
    vec2 c     = CORNERS[gl_VertexIndex % 6];
    out_color  = in_r4;
    out_flags  = uvec2(flags, uint(in_r7.y));
    out_extra  = in_r7.x;
    out_d0     = in_r5;
    out_d1     = in_r6;
    out_a      = vec4(0.0);
    out_b      = vec4(0.0);
    out_c      = vec4(0.0);
    out_pixel  = vec2(0.0);

    if (kind != BATCH_LINE) {
        vec3 p = in_r0.xyz;
        if (kind == BATCH_CIRCLE) {
            // Centre and two radii: corners at -1 and +1 of each.
            vec2 s    = c * 2.0 - 1.0;
            p         = in_r0.xyz + s.x * in_r1.xyz + s.y * in_r2.xyz;
            out_local = s;
        } else {
            p         = in_r0.xyz + c.x * in_r1.xyz + c.y * in_r2.xyz;
            out_local = mix(in_r5.xy, in_r5.zw, c);
        }
        gl_Position = batch_clip(p);
        return;
    }

    // A segment: its ends on the screen, in pixels.
    vec4 clip_a = batch_clip(in_r0.xyz);
    vec4 clip_b = batch_clip(in_r1.xyz);
    vec2 a      = batch_pixel(clip_a);
    vec2 b      = batch_pixel(clip_b);
    vec2 prev   = (flags & LINE_HAS_PREV) != 0u ? batch_pixel(batch_clip(in_r2.xyz)) : a;
    vec2 next   = (flags & LINE_HAS_NEXT) != 0u ? batch_pixel(batch_clip(in_r3.xyz)) : b;
    float hw    = in_r1.w * 0.5;
    vec2 d      = b - a;
    float len   = length(d);
    vec2 dir    = len > 0.0 ? d / len : vec2(1.0, 0.0);
    vec2 n      = vec2(-dir.y, dir.x);
    // How far past each end the shape can reach: a mitre's limit at a
    // corner, half the width for a square or round cap or a round or
    // bevelled corner, nothing for a butt cap; and a pixel for the edge's
    // antialiasing all round.
    uint cap      = (flags >> LINE_CAP_SHIFT) & 3u;
    uint join     = (flags >> LINE_JOIN_SHIFT) & 3u;
    float corner  = join == 1u ? hw * max(in_r2.w, 1.0) : hw;
    float end_cap = cap == 1u ? 0.0 : hw;
    float reach_a = ((flags & LINE_HAS_PREV) != 0u ? corner : end_cap) + 1.0;
    float reach_b = ((flags & LINE_HAS_NEXT) != 0u ? corner : end_cap) + 1.0;
    float side    = max(hw, (flags & (LINE_HAS_PREV | LINE_HAS_NEXT)) != 0u ? corner : hw) + 1.0;
    float along   = c.x < 0.5 ? -reach_a : len + reach_b;
    vec2 offset   = dir * along + n * (c.y * 2.0 - 1.0) * side;
    vec2 pixel    = a + offset;
    // The end's own depth, so a world line is depth-tested where it is;
    // moved across the screen in pixels.
    vec4 base   = c.x < 0.5 ? clip_a : clip_b;
    vec2 from   = c.x < 0.5 ? a : b;
    vec2 ndc    = (pixel - from) * u_batch.viewport.zw * 2.0;
    gl_Position = base + vec4(ndc * base.w, 0.0, 0.0);
    out_pixel   = pixel;
    out_a       = vec4(a, b);
    out_b       = vec4(prev, next);
    out_c       = vec4(hw, in_r2.w, in_r0.w, in_r7.z);
}
