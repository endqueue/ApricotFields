#version 450

// APREND_COMMAND_LIT_AMBIENT_OCCLUSION, second pass: the raw estimate
// averaged over 4x4 texels, which is the period of the first pass's turns,
// onto the caller's target: the openness in red, and the bent normals (in
// green, blue and alpha as 0.5 + 0.5 b) summed, made unit again and written
// back the same way.

layout(set = 0, binding = 0) uniform texture2D raw;
layout(set = 0, binding = 1) uniform sampler raw_sampler;

layout(push_constant) uniform BlurParams {
    vec2 target_size;
} params;

layout(location = 0) out vec4 out_occlusion;

void main() {
    ivec2 size = textureSize(sampler2D(raw, raw_sampler), 0);
    vec2 scale = vec2(size) / params.target_size;
    ivec2 at   = ivec2(gl_FragCoord.xy * scale);
    float sum  = 0.0;
    vec3 bent  = vec3(0.0);
    for (int y = -2; y < 2; ++y) {
        for (int x = -2; x < 2; ++x) {
            vec4 texel = texelFetch(sampler2D(raw, raw_sampler), clamp(at + ivec2(x, y), ivec2(0), size - 1), 0);
            sum += texel.r;
            bent += texel.gba * 2.0 - 1.0;
        }
    }
    float open    = sum / 16.0;
    vec3 b        = dot(bent, bent) > 0.0 ? normalize(bent) : vec3(0.0);
    out_occlusion = vec4(open, b * 0.5 + 0.5);
}
