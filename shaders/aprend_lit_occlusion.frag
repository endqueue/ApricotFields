#version 450

// APREND_COMMAND_LIT_AMBIENT_OCCLUSION, first pass: ground-truth ambient
// occlusion (Jimenez, Wu, Pesce and Jarabo, "Practical Real-Time Strategies
// for Accurate Indirect Occlusion", 2016; Hazel's XeGTAO is the same
// algorithm). Around each texel of a depth buffer, four slices through the
// view vector, each turned by a 4x4 pattern; in each, the highest horizon
// either side found by four steps out to the radius (in the scene's units,
// faded over its outer part), clamped to the hemisphere of the normal
// projected into the slice, and the visible arc between them integrated
// with the cosine (the paper's closed form), weighted by the projected
// normal's length. The average over the slices is how open the texel is,
// cosine-weighted, as ambient light sees it: 1 for a texel nothing stands
// over. Raised to the intensity. The bent normal - the average direction
// that is open - comes from the same horizons (the paper's Algorithm 2).
//
// Positions come back through the view's inverse view_projection, normals
// from their derivatives, and the view vector is each texel's own ray, so a
// parallel projection works as a perspective one does. Written: red the
// openness, green, blue and alpha the bent normal in the scene's space as
// 0.5 + 0.5 b, for a target that has them.

// AprendOcclusionPass in aprendlit_internal.hpp.
layout(set = 0, binding = 0) uniform AprendOcclusionPass {
    mat4 view_projection;
    mat4 inverse_view_projection;
    vec4 target_size; // xy, and their reciprocals in zw
    vec4 eye_position;
    vec4 params;      // x radius in the scene's units, y intensity (a power), z the share of the radius the fade takes
} u_occlusion;
layout(set = 1, binding = 0) uniform texture2D depth_map;
layout(set = 1, binding = 1) uniform sampler depth_sampler;

layout(location = 0) out vec4 out_occlusion;

const float PI      = 3.14159265359;
const float HALF_PI = 1.57079632679;
const int SLICES    = 4;
const int STEPS     = 4;

vec3 unproject(vec2 frag, float d) {
    vec2 ndc = frag * u_occlusion.target_size.zw * 2.0 - 1.0;
    vec4 p   = u_occlusion.inverse_view_projection * vec4(ndc, d, 1.0);
    return p.xyz / p.w;
}

float depth_at(ivec2 at) {
    ivec2 size = textureSize(sampler2D(depth_map, depth_sampler), 0);
    return texelFetch(sampler2D(depth_map, depth_sampler), clamp(at, ivec2(0), size - 1), 0).r;
}

// The position of the texel at [frag], at its centre.
vec3 position_at(vec2 frag) {
    ivec2 size = textureSize(sampler2D(depth_map, depth_sampler), 0);
    ivec2 at   = clamp(ivec2(frag), ivec2(0), size - 1);
    return unproject(vec2(at) + 0.5, depth_at(at));
}

void main() {
    vec2 frag    = gl_FragCoord.xy;
    float depth  = depth_at(ivec2(frag));
    vec3 p       = unproject(frag, depth);
    // The texel's own ray, towards the eye: the far end of its depth range
    // minus the near, turned to the eye's side of the surface.
    vec3 v       = normalize(unproject(frag, 0.0) - unproject(frag, 1.0));
    if (dot(v, u_occlusion.eye_position.xyz - p) < 0.0)
        v = -v;
    vec3 n       = normalize(cross(position_at(frag + vec2(1.0, 0.0)) - p, position_at(frag + vec2(0.0, 1.0)) - p));
    if (dot(n, v) < 0.0)
        n = -n;
    // The texel's footprint of the radius, from the view_projection.
    float radius = u_occlusion.params.x;
    vec4 c0      = u_occlusion.view_projection * vec4(p, 1.0);
    vec3 side    = normalize(abs(n.x) < 0.9 ? cross(n, vec3(1.0, 0.0, 0.0)) : cross(n, vec3(0.0, 1.0, 0.0)));
    vec4 c1      = u_occlusion.view_projection * vec4(p + side * radius, 1.0);
    float pixels = length((c1.xy / c1.w - c0.xy / c0.w) * 0.5 * u_occlusion.target_size.xy);
    if (pixels < 1.0) {
        out_occlusion = vec4(1.0, n * 0.5 + 0.5);
        return;
    }
    // The fade: full weight out to (1 - fade) of the radius, none at it.
    float fade_from = radius * (1.0 - u_occlusion.params.z);
    float fade_span = max(radius * u_occlusion.params.z, 1e-6);
    // A turn per texel of a 4x4 pattern, so neighbours take different
    // slices and the blur after evens them out.
    float turn       = (float((int(frag.x) & 3) * 4 + (int(frag.y) & 3)) + 0.5) / 16.0;
    float visibility = 0.0;
    vec3 bent        = vec3(0.0);
    for (int slice = 0; slice < SLICES; ++slice) {
        float phi = (float(slice) + turn) * (PI / float(SLICES));
        vec2 dir  = vec2(cos(phi), sin(phi));
        // The slice's direction in the scene: along the screen's dir at the
        // texel's depth, square to the view vector.
        vec3 t    = unproject(frag + dir, depth) - p;
        t         = t - v * dot(t, v);
        if (dot(t, t) < 1e-20)
            continue;
        t                 = normalize(t);
        vec3 axis         = normalize(cross(t, v));
        vec3 projected    = n - axis * dot(n, axis);
        float length_n    = length(projected);
        if (length_n < 1e-6)
            continue;
        float cos_n       = clamp(dot(projected, v) / length_n, -1.0, 1.0);
        float angle_n     = sign(dot(t, projected)) * acos(cos_n);
        // Each side starts at the tangent plane; the higher a sample, the
        // larger its cosine with the view vector.
        float horizon_pos = cos(angle_n + HALF_PI);
        float horizon_neg = cos(angle_n - HALF_PI);
        float low_pos     = horizon_pos;
        float low_neg     = horizon_neg;
        for (int s = 0; s < STEPS; ++s) {
            float at = pixels * (float(s) + 1.0) / float(STEPS);
            for (int way = 0; way < 2; ++way) {
                vec2 offset = dir * (way == 0 ? at : -at);
                vec3 to     = position_at(frag + offset) - p;
                float l     = length(to);
                if (l < 1e-6)
                    continue;
                float weight = clamp((radius - l) / fade_span, 0.0, 1.0);
                if (l <= fade_from)
                    weight = 1.0;
                if (way == 0)
                    horizon_pos = max(horizon_pos, mix(low_pos, dot(to / l, v), weight));
                else
                    horizon_neg = max(horizon_neg, mix(low_neg, dot(to / l, v), weight));
            }
        }
        // The horizons as angles from the view vector, inside the normal's
        // hemisphere.
        float h0 = -acos(clamp(horizon_neg, -1.0, 1.0));
        float h1 = acos(clamp(horizon_pos, -1.0, 1.0));
        h0       = angle_n + clamp(h0 - angle_n, -HALF_PI, HALF_PI);
        h1       = angle_n + clamp(h1 - angle_n, -HALF_PI, HALF_PI);
        float sin_n = sin(angle_n);
        float arc0  = (cos_n + 2.0 * h0 * sin_n - cos(2.0 * h0 - angle_n)) * 0.25;
        float arc1  = (cos_n + 2.0 * h1 * sin_n - cos(2.0 * h1 - angle_n)) * 0.25;
        visibility += length_n * (arc0 + arc1);
        float t0 = (6.0 * sin(h0 - angle_n) - sin(3.0 * h0 - angle_n) + 6.0 * sin(h1 - angle_n) - sin(3.0 * h1 - angle_n) + 16.0 * sin_n -
                    3.0 * (sin(h0 + angle_n) + sin(h1 + angle_n))) / 12.0;
        float t1 = (-cos(3.0 * h0 - angle_n) - cos(3.0 * h1 - angle_n) + 8.0 * cos_n - 3.0 * (cos(h0 + angle_n) + cos(h1 + angle_n))) / 12.0;
        bent += (t * t0 + v * t1) * length_n;
    }
    visibility     = clamp(visibility / float(SLICES), 0.0, 1.0);
    float open     = pow(visibility, u_occlusion.params.y);
    vec3 b         = dot(bent, bent) > 0.0 ? normalize(bent) : n;
    out_occlusion  = vec4(open, b * 0.5 + 0.5);
}
