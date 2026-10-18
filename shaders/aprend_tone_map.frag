#version 450

// APREND_COMMAND_TONE_MAP: the source at this texel's place in [0, 1], times
// the exposure, through the curve, then encoded. Places are taken from
// gl_FragCoord, as aprend_downsample.frag explains. Alpha is kept.

layout(set = 0, binding = 0) uniform texture2D source;
layout(set = 0, binding = 1) uniform sampler source_sampler;
// An aprend_exposure's adapted exposure, 1x1, read where `automatic` is 1.
layout(set = 1, binding = 0) uniform texture2D auto_exposure;
layout(set = 1, binding = 1) uniform sampler auto_exposure_sampler;

// aprend_tone_map_params in aprendbuiltin_internal.hpp.
layout(push_constant) uniform ToneMapParams {
    vec2 target_size;
    float exposure;
    uint curve;    // APREND_TONE_CURVE
    uint encoding; // APREND_COLOR_ENCODING
    // 1: times the exposure an aprend_exposure adapted. Also brings the
    // block to a multiple of 8 bytes, as Metal aligns one with a vec2.
    uint automatic;
} params;

layout(location = 0) out vec4 out_color;

// Stephen Hill's fit of ACES's reference rendering and sRGB output
// transforms (BakingLab, ACES.hlsl): into ACES's working space, the fitted
// curve, and back to linear sRGB.
vec3 aces(vec3 color) {
    const mat3 to_aces   = mat3(0.59719, 0.07600, 0.02840, 0.35458, 0.90834, 0.13383, 0.04823, 0.01566, 0.83777);
    const mat3 from_aces = mat3(1.60475, -0.10208, -0.00327, -0.53108, 1.10813, -0.07276, -0.07367, -0.00605, 1.07602);
    vec3 v               = to_aces * color;
    v                    = (v * (v + 0.0245786) - 0.000090537) / (v * (0.983729 * v + 0.4329510) + 0.238081);
    return from_aces * v;
}

// AgX, Troy Sobotka's (Blender's default since 4.0), in Benjamin Wrensch's
// minimal fit: into AgX's working space, a log2 encoding over 16.5 stops, a
// sixth-order fit of its contrast curve, and back out, to display-linear
// sRGB.
vec3 agx(vec3 color) {
    const mat3 inset  = mat3(0.842479062253094, 0.0423282422610123, 0.0423756549057051, 0.0784335999999992, 0.878468636469772, 0.0784336,
                             0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const mat3 outset = mat3(1.19687900512017, -0.0528968517574562, -0.0529716355144438, -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
                             -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float min_ev = -12.47393;
    const float max_ev = 4.026069;
    vec3 v  = inset * color;
    v       = clamp(log2(max(v, vec3(1e-10))), min_ev, max_ev);
    v       = (v - min_ev) / (max_ev - min_ev);
    vec3 v2 = v * v;
    vec3 v4 = v2 * v2;
    v       = 15.5 * v4 * v2 - 40.14 * v4 * v + 31.96 * v4 - 6.868 * v2 * v + 0.4298 * v2 + 0.1191 * v - 0.00232;
    v       = outset * v;
    return pow(max(v, vec3(0.0)), vec3(2.2));
}

// Khronos PBR Neutral, as its reference implementation has it.
vec3 pbr_neutral(vec3 color) {
    const float start_compression = 0.8 - 0.04;
    const float desaturation      = 0.15;
    float x      = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;
    float peak = max(color.r, max(color.g, color.b));
    if (peak < start_compression)
        return color;
    const float d  = 1.0 - start_compression;
    float new_peak = 1.0 - d * d / (peak + d - start_compression);
    color *= new_peak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - new_peak) + 1.0);
    return mix(color, vec3(new_peak), g);
}

void main() {
    vec4 texel = textureLod(sampler2D(source, source_sampler), gl_FragCoord.xy / params.target_size, 0.0);
    float exposure = params.exposure;
    if (params.automatic == 1u)
        exposure *= texelFetch(sampler2D(auto_exposure, auto_exposure_sampler), ivec2(0), 0).r;
    vec3 color = max(texel.rgb * exposure, vec3(0.0));
    if (params.curve == 1u)
        color = aces(color);
    else if (params.curve == 2u)
        color = pbr_neutral(color);
    else if (params.curve == 3u)
        color = agx(color);
    color = clamp(color, 0.0, 1.0);
    if (params.encoding == 1u)
        color = mix(1.055 * pow(color, vec3(1.0 / 2.4)) - 0.055, color * 12.92, lessThanEqual(color, vec3(0.0031308)));
    out_color = vec4(color, texel.a);
}
