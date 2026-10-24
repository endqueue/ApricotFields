#version 450

// Bloom, first pass: the source into level 0 of the bloom chain, at half its
// size, through the same 13 taps as aprend_bloom_downsample.frag, then
// weighted by a threshold with a soft knee. A threshold and knee of 0 keep
// every texel as it is, which is what a caller that draws only what should
// glow into the source wants. The weight is taken from the brightest of
// red, green and blue and applied to all four channels, so premultiplied
// alpha stays premultiplied and the glow carries its own coverage.
//
// With a mask (set 1), each tap is the source times the mask's red at the
// same place in [0, 1], before the taps are summed: a masked texel adds
// nothing, however bright, rather than being blurred into its neighbours
// and then weighted.
//
// Places are taken from gl_FragCoord, as aprend_downsample.frag explains.

layout(set = 0, binding = 0) uniform texture2D source_level;
layout(set = 0, binding = 1) uniform sampler source_sampler;
layout(set = 1, binding = 0) uniform texture2D mask_level;
layout(set = 1, binding = 1) uniform sampler mask_sampler;

layout(push_constant) uniform BloomParams {
    float threshold;
    float knee;
    float radius;
    float intensity;
    vec2 target_size;
    // 1 to weight the prefilter's taps by the mask at set 1, 0 to read none.
    float use_mask;
    // What the lens dirt at set 1 adds to the composite's glow; 0 for none.
    float lens_dirt_intensity;
} params;

layout(location = 0) out vec4 out_color;

vec4 tap(vec2 uv) {
    vec4 texel = textureLod(sampler2D(source_level, source_sampler), uv, 0.0);
    if (params.use_mask > 0.5)
        texel *= textureLod(sampler2D(mask_level, mask_sampler), uv, 0.0).r;
    return texel;
}

void main() {
    vec2 source_size = vec2(textureSize(sampler2D(source_level, source_sampler), 0));
    // Level 0's size, which is half the size the bloom was made for, not
    // necessarily half the source's.
    vec2 uv          = gl_FragCoord.xy / params.target_size;
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
    vec4 color = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;

    float brightness = max(color.r, max(color.g, color.b));
    float soft       = clamp(brightness - params.threshold + params.knee, 0.0, 2.0 * params.knee);
    soft             = soft * soft / (4.0 * params.knee + 1e-5);
    float weight     = max(soft, brightness - params.threshold) / max(brightness, 1e-5);
    out_color        = color * weight;
}
