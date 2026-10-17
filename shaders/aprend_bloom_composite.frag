#version 450

// Bloom, last pass: level 0 of the chain spread over the whole target
// through the same tent as aprend_bloom_upsample.frag, times the intensity,
// and added to what the target holds by the pipeline's additive blend:
// color and alpha alike, so a glow over a clear target has coverage of its
// own. The target may be any size; the chain is read at the same place in
// [0, 1].
//
// Places are taken from gl_FragCoord, as aprend_downsample.frag explains.

layout(set = 0, binding = 0) uniform texture2D source_level;
layout(set = 0, binding = 1) uniform sampler source_sampler;

layout(push_constant) uniform BloomParams {
    float threshold;
    float knee;
    float radius;
    float intensity;
    vec2 target_size;
} params;

layout(location = 0) out vec4 out_color;

vec4 tap(vec2 uv) {
    return textureLod(sampler2D(source_level, source_sampler), uv, 0.0);
}

void main() {
    vec2 source_size = vec2(textureSize(sampler2D(source_level, source_sampler), 0));
    vec2 uv          = gl_FragCoord.xy / params.target_size;
    vec2 t           = params.radius / source_size;

    vec4 sum = tap(uv) * 4.0;
    sum += (tap(uv + t * vec2( 0.0, -1.0)) + tap(uv + t * vec2(-1.0,  0.0)) +
            tap(uv + t * vec2( 1.0,  0.0)) + tap(uv + t * vec2( 0.0,  1.0))) * 2.0;
    sum += tap(uv + t * vec2(-1.0, -1.0)) + tap(uv + t * vec2( 1.0, -1.0)) +
           tap(uv + t * vec2(-1.0,  1.0)) + tap(uv + t * vec2( 1.0,  1.0));
    out_color = sum * (params.intensity / 16.0);
}
