#version 450

// Bloom: one level of the chain from the level above, at half its size,
// through 13 taps (Jimenez, "Next Generation Post Processing in Call of
// Duty: Advanced Warfare"): a box of the four texels round the target
// texel's centre, weighted 1/2, and four overlapping boxes round it, 1/8
// each. A plain 2x2 box lets a single bright texel flicker as it moves
// across texel boundaries; this does not.
//
// Places are taken from gl_FragCoord, as aprend_downsample.frag explains.

layout(set = 0, binding = 0) uniform texture2D source_level;
layout(set = 0, binding = 1) uniform sampler source_sampler;

layout(location = 0) out vec4 out_color;

vec4 tap(vec2 uv) {
    return textureLod(sampler2D(source_level, source_sampler), uv, 0.0);
}

void main() {
    vec2 source_size = vec2(textureSize(sampler2D(source_level, source_sampler), 0));
    vec2 target_size = max(floor(source_size * 0.5), vec2(1.0));
    vec2 uv          = gl_FragCoord.xy / target_size;
    vec2 t           = 1.0 / source_size;

    vec4 a = tap(uv + t * vec2(-2.0, -2.0));
    vec4 b = tap(uv + t * vec2( 0.0, -2.0));
    vec4 c = tap(uv + t * vec2( 2.0, -2.0));
    vec4 d = tap(uv + t * vec2(-2.0,  0.0));
    vec4 e = tap(uv);
    vec4 f = tap(uv + t * vec2( 2.0,  0.0));
    vec4 g = tap(uv + t * vec2(-2.0,  2.0));
    vec4 h = tap(uv + t * vec2( 0.0,  2.0));
    vec4 i = tap(uv + t * vec2( 2.0,  2.0));
    vec4 j = tap(uv + t * vec2(-1.0, -1.0));
    vec4 k = tap(uv + t * vec2( 1.0, -1.0));
    vec4 l = tap(uv + t * vec2(-1.0,  1.0));
    vec4 m = tap(uv + t * vec2( 1.0,  1.0));
    out_color = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
}
