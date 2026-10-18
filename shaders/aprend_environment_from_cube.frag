#version 450
#extension GL_GOOGLE_include_directive : require

// A face of the environment's radiance cube from the caller's cube, read
// filtered at its own first mip.

#include "aprend_environment.glslh"

layout(set = 0, binding = 0) uniform textureCube source;
layout(set = 0, binding = 1) uniform sampler source_sampler;

layout(location = 0) out vec4 out_color;

void main() {
    vec3 d    = cube_direction(gl_FragCoord.xy, params.face);
    out_color = vec4(textureLod(samplerCube(source, source_sampler), d, 0.0).rgb, 1.0);
}
