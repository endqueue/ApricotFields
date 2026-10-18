#version 450
#extension GL_GOOGLE_include_directive : require

// A face of one mip of the environment's specular chain: the radiance cube
// convolved with GGX at this mip's roughness, by importance sampling with
// the reflection taken as the normal and the view (Karis, "Real Shading in
// Unreal Engine 4"), each sample read from the mip whose texels cover its
// share of the lobe (filtered importance sampling, Krivanek and Colbert),
// which keeps a few hundred samples free of fireflies. Roughness 0 is the
// picture itself.

#include "aprend_environment.glslh"

layout(set = 0, binding = 0) uniform textureCube source;
layout(set = 0, binding = 1) uniform sampler source_sampler;

layout(location = 0) out vec4 out_color;

void main() {
    vec3 n = cube_direction(gl_FragCoord.xy, params.face);
    if (params.roughness <= 0.0) {
        out_color = vec4(textureLod(samplerCube(source, source_sampler), n, 0.0).rgb, 1.0);
        return;
    }
    float a          = params.roughness * params.roughness;
    float a2         = a * a;
    mat3 basis       = frame(n);
    float texel_area = 4.0 * PI / (6.0 * params.source_size * params.source_size);
    vec3 sum         = vec3(0.0);
    float weight     = 0.0;
    for (uint i = 0u; i < params.sample_count; ++i) {
        vec2 xi        = hammersley(i, params.sample_count);
        float phi      = 2.0 * PI * xi.x;
        float cos_t    = sqrt((1.0 - xi.y) / (1.0 + (a2 - 1.0) * xi.y));
        float sin_t    = sqrt(1.0 - cos_t * cos_t);
        vec3 h         = basis * vec3(sin_t * cos(phi), sin_t * sin(phi), cos_t);
        vec3 l         = 2.0 * dot(n, h) * h - n;
        float n_dot_l  = dot(n, l);
        if (n_dot_l <= 0.0)
            continue;
        // pdf of l = D(h) * cos(h) / (4 * dot(v, h)), with v = n.
        float n_dot_h    = max(dot(n, h), 0.0);
        float d          = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
        float pdf        = a2 / (PI * d * d) / 4.0;
        float solid      = 1.0 / (float(params.sample_count) * pdf + 1e-6);
        float lod        = clamp(0.5 * log2(solid / texel_area) + 1.0, 0.0, params.source_mips - 1.0);
        sum += textureLod(samplerCube(source, source_sampler), l, lod).rgb * n_dot_l;
        weight += n_dot_l;
    }
    out_color = vec4(weight > 0.0 ? sum / weight : vec3(0.0), 1.0);
}
