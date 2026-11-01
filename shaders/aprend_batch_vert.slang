// The batcher's vertex shader (aprendbatch.h): one record a shape, read
// per instance, expanded here into two triangles (six vertices). Quads,
// glyphs and circles are a parallelogram from a corner and two edges, in
// either space; a line segment is a rectangle round it in pixels, grown
// past its ends by what its caps and joins reach, its position in pixels
// handed on for the fragment shader to measure from.

import aprend_batch;

struct BatchRecord {
    [[vk::location(0)]] float4 r0 : TEXCOORD0;
    [[vk::location(1)]] float4 r1 : TEXCOORD1;
    [[vk::location(2)]] float4 r2 : TEXCOORD2;
    [[vk::location(3)]] float4 r3 : TEXCOORD3;
    [[vk::location(4)]] float4 r4 : TEXCOORD4;
    [[vk::location(5)]] float4 r5 : TEXCOORD5;
    [[vk::location(6)]] float4 r6 : TEXCOORD6;
    [[vk::location(7)]] float4 r7 : TEXCOORD7;
};

// The position first, then every location in order, as
// aprend_batch_inputs.slang's BatchFragmentInput reads them.
struct BatchVertexOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 local : TEXCOORD0;                       // quad, glyph: uv; circle: -1 to 1 across
    [[vk::location(1)]] noperspective float2 pixel : TEXCOORD1;         // line: the fragment's pixel
    [[vk::location(2)]] nointerpolation float4 color : TEXCOORD2;
    [[vk::location(3)]] nointerpolation float4 a : TEXCOORD3;           // line: start (xy), end (zw), in pixels
    [[vk::location(4)]] nointerpolation float4 b : TEXCOORD4;           // line: previous point (xy), next (zw)
    [[vk::location(5)]] nointerpolation float4 c : TEXCOORD5;           // line: half width, mitre limit, dash phase, pattern length
    [[vk::location(6)]] nointerpolation float4 d0 : TEXCOORD6;          // dashes 0-3; quad, glyph: uv rect; circle: inner, start, sweep
    [[vk::location(7)]] nointerpolation float4 d1 : TEXCOORD7;          // dashes 4-7; glyph: outline color
    [[vk::location(8)]] nointerpolation uint2 flags : TEXCOORD8;        // x: flags; y: glyph: layer
    [[vk::location(9)]] nointerpolation float extra : TEXCOORD9;        // glyph: outline width
};

static const float2 CORNERS[6] = { float2(0.0, 0.0), float2(1.0, 0.0), float2(0.0, 1.0), float2(0.0, 1.0), float2(1.0, 0.0), float2(1.0, 1.0) };

[shader("vertex")]
BatchVertexOutput aprend_batch_vert(BatchRecord record, uint vertex_index : SV_VertexID) {
    BatchVertexOutput output;
    uint flags   = uint(record.r3.w);
    uint kind    = flags & 3u;
    float2 c     = CORNERS[int(vertex_index) % 6];
    output.color = record.r4;
    output.flags = uint2(flags, uint(record.r7.y));
    output.extra = record.r7.x;
    output.d0    = record.r5;
    output.d1    = record.r6;
    output.a     = float4(0.0);
    output.b     = float4(0.0);
    output.c     = float4(0.0);
    output.pixel = float2(0.0);
    output.local = float2(0.0);

    if (kind != BATCH_LINE) {
        float3 p = record.r0.xyz;
        if (kind == BATCH_CIRCLE) {
            // Centre and two radii: corners at -1 and +1 of each.
            float2 s     = c * 2.0 - 1.0;
            p            = record.r0.xyz + s.x * record.r1.xyz + s.y * record.r2.xyz;
            output.local = s;
        } else {
            p            = record.r0.xyz + c.x * record.r1.xyz + c.y * record.r2.xyz;
            output.local = lerp(record.r5.xy, record.r5.zw, c);
        }
        output.position = batch_clip(p);
        return output;
    }

    // A segment: its ends on the screen, in pixels.
    float4 clip_a = batch_clip(record.r0.xyz);
    float4 clip_b = batch_clip(record.r1.xyz);
    float2 a      = batch_pixel(clip_a);
    float2 b      = batch_pixel(clip_b);
    float2 prev   = (flags & LINE_HAS_PREV) != 0u ? batch_pixel(batch_clip(record.r2.xyz)) : a;
    float2 next   = (flags & LINE_HAS_NEXT) != 0u ? batch_pixel(batch_clip(record.r3.xyz)) : b;
    float hw      = record.r1.w * 0.5;
    float2 d      = b - a;
    float len     = length(d);
    float2 dir    = len > 0.0 ? d / len : float2(1.0, 0.0);
    float2 n      = float2(-dir.y, dir.x);
    // How far past each end the shape can reach: a mitre's limit at a
    // corner, half the width for a square or round cap or a round or
    // bevelled corner, nothing for a butt cap; and a pixel for the edge's
    // antialiasing all round.
    uint cap      = (flags >> LINE_CAP_SHIFT) & 3u;
    uint join     = (flags >> LINE_JOIN_SHIFT) & 3u;
    float corner  = join == 1u ? hw * max(record.r2.w, 1.0) : hw;
    float end_cap = cap == 1u ? 0.0 : hw;
    float reach_a = ((flags & LINE_HAS_PREV) != 0u ? corner : end_cap) + 1.0;
    float reach_b = ((flags & LINE_HAS_NEXT) != 0u ? corner : end_cap) + 1.0;
    float side    = max(hw, (flags & (LINE_HAS_PREV | LINE_HAS_NEXT)) != 0u ? corner : hw) + 1.0;
    float along   = c.x < 0.5 ? -reach_a : len + reach_b;
    float2 offset = dir * along + n * (c.y * 2.0 - 1.0) * side;
    float2 pixel  = a + offset;
    // The end's own depth, so a world line is depth-tested where it is;
    // moved across the screen in pixels, y down, so clip space's y the
    // other way.
    float4 base     = c.x < 0.5 ? clip_a : clip_b;
    float2 from     = c.x < 0.5 ? a : b;
    float2 ndc      = (pixel - from) * u_batch.viewport.zw * 2.0;
    output.position = base + float4(float2(ndc.x, -ndc.y) * base.w, 0.0, 0.0);
    output.pixel    = pixel;
    output.a        = float4(a, b);
    output.b        = float4(prev, next);
    output.c        = float4(hw, record.r2.w, record.r0.w, record.r7.z);
    return output;
}
