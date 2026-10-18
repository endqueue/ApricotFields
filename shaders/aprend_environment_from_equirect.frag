#version 450
#extension GL_GOOGLE_include_directive : require

// A face of the environment's radiance cube from the caller's
// equirectangular picture: longitude across, its middle column facing +Z,
// the north pole (+Y) at the top row.

#include "aprend_environment.glslh"

layout(set = 0, binding = 0) uniform texture2D source;
layout(set = 0, binding = 1) uniform sampler source_sampler;

layout(location = 0) out vec4 out_color;

void main() {
    vec3 d    = cube_direction(gl_FragCoord.xy, params.face);
    vec2 uv   = vec2(atan(d.x, d.z) / (2.0 * PI) + 0.5, acos(clamp(d.y, -1.0, 1.0)) / PI);
    out_color = vec4(textureLod(sampler2D(source, source_sampler), uv, 0.0).rgb, 1.0);
}
