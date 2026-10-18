#version 450

// APREND_COMMAND_AUTO_EXPOSURE, first pass: the base 2 log of the source's
// luminance (Rec. 709 weights), read filtered at this texel's place, into
// the exposure's luminance texture, whose mips are then averaged down to
// one texel: the log-average luminance (Reinhard et al.).

layout(set = 0, binding = 0) uniform texture2D source;
layout(set = 0, binding = 1) uniform sampler source_sampler;

layout(push_constant) uniform LuminanceParams {
    vec2 target_size;
} params;

layout(location = 0) out vec4 out_luminance;

void main() {
    vec3 c        = textureLod(sampler2D(source, source_sampler), gl_FragCoord.xy / params.target_size, 0.0).rgb;
    float l       = dot(max(c, vec3(0.0)), vec3(0.2126, 0.7152, 0.0722));
    out_luminance = vec4(log2(max(l, 1e-6)), 0.0, 0.0, 1.0);
}
