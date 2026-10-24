#version 450

// An outline drawn round a mask by a jump flood (Rong and Tan, "Jump
// Flooding in GPU with Applications to Voronoi Diagram and Distance
// Transform", 2006): each texel ends holding the place of the nearest texel
// inside the mask, from which its distance to the mask is one subtraction.
// Three modes, one pass each:
//
// - SEED: from the mask (red above 0.5 is inside), each inside texel's own
//   centre, every other texel "none" (far off the target).
// - JUMP: from the last pass's places, the nearest of this texel's own and
//   those of the eight texels `jump` texels off; a texel past the edge of
//   the target is not read. Jumps of halving length, then one more of 1,
//   reach every texel the outline covers.
// - COMPOSITE: from the last places, the outline's coverage of each texel
//   outside the mask: its width plus one, less the distance between its
//   centre and the nearest inside centre, held to [0, 1]. So a texel beside
//   a straight edge is covered by min(width, 1), and an outline of width w
//   covers w whole texels out from the edge. Written premultiplied, for a
//   blend over the target. A texel inside the mask is not touched.
//
// Places are texel centres in texels, as gl_FragCoord gives them; every read
// is a texelFetch, so the places' texture is never filtered.

layout(set = 0, binding = 0) uniform texture2D source_level;
layout(set = 0, binding = 1) uniform sampler source_sampler;

layout(push_constant) uniform OutlineParams {
    // Linear RGBA, alpha not premultiplied.
    vec4 color;
    vec2 target_size;
    float width;
    float jump;
    // 0 SEED, 1 JUMP, 2 COMPOSITE.
    uint mode;
    // The block is 48 bytes, as the pipeline's range is.
    uint unused0;
    uint unused1;
    uint unused2;
} params;

layout(location = 0) out vec4 out_color;

const float NONE = -1.0e9;

vec2 place_at(ivec2 texel) {
    return texelFetch(sampler2D(source_level, source_sampler), texel, 0).xy;
}

void main() {
    ivec2 texel = ivec2(gl_FragCoord.xy);
    vec2 here   = gl_FragCoord.xy;
    if (params.mode == 0u) {
        bool inside = texelFetch(sampler2D(source_level, source_sampler), texel, 0).r > 0.5;
        out_color   = vec4(inside ? here : vec2(NONE), 0.0, 0.0);
        return;
    }
    if (params.mode == 1u) {
        ivec2 size     = ivec2(params.target_size);
        int jump       = int(params.jump);
        vec2 best      = place_at(texel);
        float best_d2  = dot(best - here, best - here);
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                ivec2 at = texel + ivec2(x, y) * jump;
                if ((x == 0 && y == 0) || at.x < 0 || at.y < 0 || at.x >= size.x || at.y >= size.y)
                    continue;
                vec2 place = place_at(at);
                float d2   = dot(place - here, place - here);
                if (d2 < best_d2) {
                    best    = place;
                    best_d2 = d2;
                }
            }
        }
        out_color = vec4(best, 0.0, 0.0);
        return;
    }
    vec2 nearest = place_at(texel);
    float d      = length(nearest - here);
    // The nearest inside centre is this one's own: inside the mask.
    if (d < 0.5)
        discard;
    float coverage = clamp(params.width + 1.0 - d, 0.0, 1.0);
    if (coverage <= 0.0)
        discard;
    float alpha = params.color.a * coverage;
    out_color   = vec4(params.color.rgb * alpha, alpha);
}
