#version 450

// Bloom: a level of the chain read back up into the level above it, through
// a 3x3 tent of taps `radius` texels of the read level apart, and added to
// what that level holds by the pipeline's additive blend. Going up from the
// smallest level, each level ends as itself plus everything below it, so
// level 0 holds the sum of every level: a wide, soft glow built from narrow
// steps.
//
// Also aprend_bloom_composite.frag's filter, with the target its own size.
// Places are taken from gl_FragCoord, as aprend_downsample.frag explains.

layout(set = 0, binding = 0) uniform texture2D source_level;
layout(set = 0, binding = 1) uniform sampler source_sampler;

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
    return textureLod(sampler2D(source_level, source_sampler), uv, 0.0);
}

void main() {
    vec2 source_size = vec2(textureSize(sampler2D(source_level, source_sampler), 0));
    // Not twice the source's: a level of an odd side halves to less.
    vec2 uv          = gl_FragCoord.xy / params.target_size;
    vec2 t           = params.radius / source_size;

    vec4 sum = tap(uv) * 4.0;
    sum += (tap(uv + t * vec2( 0.0, -1.0)) + tap(uv + t * vec2(-1.0,  0.0)) +
            tap(uv + t * vec2( 1.0,  0.0)) + tap(uv + t * vec2( 0.0,  1.0))) * 2.0;
    sum += tap(uv + t * vec2(-1.0, -1.0)) + tap(uv + t * vec2( 1.0, -1.0)) +
           tap(uv + t * vec2(-1.0,  1.0)) + tap(uv + t * vec2( 1.0,  1.0));
    out_color = sum * (1.0 / 16.0);
}
