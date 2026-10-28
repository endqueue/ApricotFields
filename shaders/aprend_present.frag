#version 450

// A source rectangle fitted into a target rectangle
// (APREND_COMMAND_DRAW_TEXTURE, and PRESENT_TEXTURE's STRETCH and
// ASPECT_FIT, whose rectangles are both whole): each texel of the target
// rectangle maps into the part of it the source rectangle fills, and reads
// it there through the sampler (linear or nearest, the command's filter),
// or is the border color outside it. The pass's scissor keeps the draw
// inside the target rectangle.
//
// The part filled, in texels of the target rectangle:
// - mode 0: the source rectangle's own size, from its top-left corner;
// - mode 1: the whole target rectangle;
// - mode 2: the source rectangle scaled to the largest size that fits with
//   its aspect kept, centred.
//
// The place is taken from gl_FragCoord, so it reads the same on every
// backend: the target's and the source's texel rows both start at the top.
// What is written is in the target's format: a float source onto an 8-bit
// target is clamped there, and an sRGB target encodes, as any draw's
// output is.
//
// A separate texture and sampler, as the other built-in passes: one view
// is read through either filter, each a set the view keeps.

layout(set = 0, binding = 0) uniform texture2D source_texture;
layout(set = 0, binding = 1) uniform sampler source_sampler;

// aprend_present_params.
layout(push_constant) uniform PresentParams {
    vec4 border;
    vec2 target_offset;
    vec2 target_size;
    vec2 source_offset;
    vec2 source_size;
    uint mode;
    uint unused0;
    uint unused1;
    uint unused2;
} params;

layout(location = 0) out vec4 out_color;

void main() {
    vec2 texture_size = vec2(textureSize(sampler2D(source_texture, source_sampler), 0));
    vec2 source       = params.source_size;
    vec2 target       = params.target_size;
    vec2 size   = target;
    vec2 offset = vec2(0.0);
    if (params.mode == 0u) {
        size = source;
    } else if (params.mode == 2u) {
        float scale = min(target.x / source.x, target.y / source.y);
        size        = source * scale;
        offset      = (target - size) * 0.5;
    }
    vec2 place = (gl_FragCoord.xy - params.target_offset - offset) / size;
    if (any(lessThan(place, vec2(0.0))) || any(greaterThanEqual(place, vec2(1.0)))) {
        out_color = params.border;
        return;
    }
    vec2 uv   = (params.source_offset + place * source) / texture_size;
    out_color = textureLod(sampler2D(source_texture, source_sampler), uv, 0.0);
}
