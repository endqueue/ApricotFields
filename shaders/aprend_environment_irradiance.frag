#version 450
#extension GL_GOOGLE_include_directive : require

// A face of the environment's irradiance cube: the radiance cube's light on
// a face looking along this texel's direction, cosine weighted and divided
// by pi, so a white matte face lit by it reads it as it is - a uniform
// environment of radiance c gives c. Cosine-weighted importance sampling,
// each sample read from a mip that covers its share of the hemisphere.

#include "aprend_environment.glslh"

layout(set = 0, binding = 0) uniform textureCube source;
layout(set = 0, binding = 1) uniform sampler source_sampler;

layout(location = 0) out vec4 out_color;

void main() {
    vec3 n           = cube_direction(gl_FragCoord.xy, params.face);
    mat3 basis       = frame(n);
    float texel_area = 4.0 * PI / (6.0 * params.source_size * params.source_size);
    vec3 sum         = vec3(0.0);
    for (uint i = 0u; i < params.sample_count; ++i) {
        vec2 xi     = hammersley(i, params.sample_count);
        float phi   = 2.0 * PI * xi.x;
        float cos_t = sqrt(1.0 - xi.y);
        float sin_t = sqrt(xi.y);
        vec3 l      = basis * vec3(sin_t * cos(phi), sin_t * sin(phi), cos_t);
        float pdf   = max(cos_t, 1e-4) / PI;
        float solid = 1.0 / (float(params.sample_count) * pdf);
        float lod   = clamp(0.5 * log2(solid / texel_area) + 1.0, 0.0, params.source_mips - 1.0);
        sum += textureLod(samplerCube(source, source_sampler), l, lod).rgb;
    }
    out_color = vec4(sum / float(params.sample_count), 1.0);
}
