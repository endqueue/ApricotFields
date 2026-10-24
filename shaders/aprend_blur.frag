#version 450

// A separable Gaussian blur, one direction a pass: each texel the weighted
// sum of the source along `step` (one texel of the target, in uv, across or
// down), weights exp(-i^2 / 2 sigma^2) out to `radius` texels each side,
// normalized so they sum to 1. Taps go in pairs: one linear sample between
// texels i and i + 1, placed where its weights put it, reads both (Rakos,
// "Efficient Gaussian blur with linear sampling", 2010), so a radius of r
// costs r + 1 samples, not 2r + 1. Clamped to the edge by the sampler, so the
// edge texel stands in for what is past it.
//
// Places are taken from gl_FragCoord, as aprend_downsample.frag explains: the
// source is read at the same place in [0, 1], filtered.

layout(set = 0, binding = 0) uniform texture2D source_level;
layout(set = 0, binding = 1) uniform sampler source_sampler;

layout(push_constant) uniform BlurParams {
    // One texel of the target along the pass's direction, in uv.
    vec2 step;
    vec2 target_size;
    float sigma;
    // Whole texels each side: ceil(3 sigma).
    float radius;
} params;

layout(location = 0) out vec4 out_color;

vec4 tap(vec2 uv) {
    return textureLod(sampler2D(source_level, source_sampler), uv, 0.0);
}

void main() {
    vec2 uv        = gl_FragCoord.xy / params.target_size;
    float two_s2   = 2.0 * params.sigma * params.sigma;
    int radius     = int(params.radius);
    vec4 sum       = tap(uv);
    float total    = 1.0;
    for (int i = 1; i <= radius; i += 2) {
        float a      = float(i);
        float w0     = exp(-a * a / two_s2);
        float w1     = i + 1 <= radius ? exp(-(a + 1.0) * (a + 1.0) / two_s2) : 0.0;
        float w      = w0 + w1;
        float offset = (a * w0 + (a + 1.0) * w1) / w;
        sum += (tap(uv + params.step * offset) + tap(uv - params.step * offset)) * w;
        total += 2.0 * w;
    }
    out_color = sum / total;
}
