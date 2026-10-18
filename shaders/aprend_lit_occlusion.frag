#version 450

// APREND_COMMAND_LIT_AMBIENT_OCCLUSION, first pass: how much of the sky each
// texel of a depth buffer sees, by the depth around it (a horizon-based
// estimate, after Bavoil and Sainz's HBAO and Jimenez et al.'s GTAO in
// spirit: eight directions, four steps each, out to a radius in the scene's
// units). Positions come back through the view's inverse view_projection,
// normals from their derivatives. White is open, black closed in.

// AprendOcclusionPass in aprendlit_internal.hpp.
layout(set = 0, binding = 0) uniform AprendOcclusionPass {
    mat4 view_projection;
    mat4 inverse_view_projection;
    vec4 target_size; // xy, and their reciprocals in zw
    vec4 eye_position;
    vec4 params;      // x radius in the scene's units, y intensity, z bias (a share of the radius)
} u_occlusion;
layout(set = 1, binding = 0) uniform texture2D depth_map;
layout(set = 1, binding = 1) uniform sampler depth_sampler;

layout(location = 0) out vec4 out_occlusion;

const float PI = 3.14159265359;

vec3 position_at(vec2 frag) {
    ivec2 size = textureSize(sampler2D(depth_map, depth_sampler), 0);
    ivec2 at   = clamp(ivec2(frag), ivec2(0), size - 1);
    float d    = texelFetch(sampler2D(depth_map, depth_sampler), at, 0).r;
    vec2 ndc   = (vec2(at) + 0.5) * u_occlusion.target_size.zw * 2.0 - 1.0;
    vec4 p     = u_occlusion.inverse_view_projection * vec4(ndc, d, 1.0);
    return p.xyz / p.w;
}

void main() {
    vec2 frag = gl_FragCoord.xy;
    vec3 p    = position_at(frag);
    vec3 n    = normalize(cross(position_at(frag + vec2(1.0, 0.0)) - p, position_at(frag + vec2(0.0, 1.0)) - p));
    // Turned to face the eye.
    if (dot(n, u_occlusion.eye_position.xyz - p) < 0.0)
        n = -n;
    // The texel's footprint of the radius, from the view_projection.
    vec4 c0      = u_occlusion.view_projection * vec4(p, 1.0);
    float radius = u_occlusion.params.x;
    vec3 side    = normalize(abs(n.x) < 0.9 ? cross(n, vec3(1.0, 0.0, 0.0)) : cross(n, vec3(0.0, 1.0, 0.0)));
    vec4 c1      = u_occlusion.view_projection * vec4(p + side * radius, 1.0);
    float pixels = length((c1.xy / c1.w - c0.xy / c0.w) * 0.5 * u_occlusion.target_size.xy);
    if (pixels < 1.0) {
        out_occlusion = vec4(1.0);
        return;
    }
    // A turn per texel of a 4x4 pattern, so neighbours sample different
    // directions and the blur after evens them out.
    float turn     = (float((int(frag.x) & 3) * 4 + (int(frag.y) & 3)) / 16.0) * (2.0 * PI / 8.0);
    float occluded = 0.0;
    for (int d = 0; d < 8; ++d) {
        float a  = turn + float(d) * (2.0 * PI / 8.0);
        vec2 dir = vec2(cos(a), sin(a));
        float best = 0.0;
        for (int s = 1; s <= 4; ++s) {
            vec3 q  = position_at(frag + dir * pixels * float(s) / 4.0);
            vec3 to = q - p;
            float l = length(to);
            if (l < 1e-6 || l > radius)
                continue;
            float rise = dot(to / l, n) - u_occlusion.params.z;
            best       = max(best, rise * (1.0 - l / radius));
        }
        occluded += best;
    }
    float open    = 1.0 - clamp(occluded / 8.0 * u_occlusion.params.y, 0.0, 1.0);
    out_occlusion = vec4(open, open, open, 1.0);
}
