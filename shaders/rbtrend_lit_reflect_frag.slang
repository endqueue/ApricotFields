// RBTREND_COMMAND_LIT_REFLECT (rbtrendcommands.h): a reflection put on the
// picture, drawn over it with additive blending. Set 0 is the scene's own
// set, as the surface reads it, so the environment specular worked out
// again here is the surface's, through the same cube and sampler; set 1 the
// reflecting surfaces' targets and the reflection.
//
// At each texel, the four texels of the half-resolution reflection round it
// are each weighted by how near they are (bilinearly), by how near the
// point each stands for (the texel nearest the eye of its 2x2, as LIT_SSR
// took it) lies to this texel's surface, measured from the plane through
// this texel square to its normal so a slanted surface's own neighbours
// count in full, and by how alike their normals are, so a reflection
// doesn't bleed across an edge (as GTAO's blur keeps one).
// What is added is specular x a x (reflection - environment specular) x
// exposure: the surface added specular x environment specular, so at a of
// 1 that is taken back and the reflection put in its place, to within the
// precision of the targets it is worked out again from. Where a is 0 or
// there is no surface, 0 is added, which leaves the picture as it was, bit
// for bit.

import rbtrend_lit_scene;
import rbtrend_lit_bindings;

[[vk::binding(0, 1)]] Texture2D<float4> reflect_depth : register(t0, space1);
[[vk::binding(1, 1)]] Texture2D<float4> reflect_normal_roughness : register(t1, space1);
[[vk::binding(2, 1)]] Texture2D<float4> reflect_specular : register(t2, space1);
[[vk::binding(3, 1)]] Texture2D<float4> reflect_reflection : register(t3, space1);

// rbtrend_reflect_params in rbtrendlit_internal.hpp.
struct ReflectParams {
    float2 target_size;
    uint reversed;       // the depth buffer is near at 1
    uint padding;
};
[[vk::push_constant]] ConstantBuffer<ReflectParams> params : register(b0, space4);

// How far a neighbour's point may lie off this texel's plane, as a share of
// this texel's distance along the view, before its weight falls to nothing;
// and how sharply a neighbour's weight falls as its normal turns from this
// texel's.
static const float DEPTH_SHARE  = 0.05;
static const float NORMAL_POWER = 8.0;

float nearness(float z) {
    return params.reversed != 0u ? 1.0 - z : z;
}

float3 unproject(float2 p, float z) {
    float2 ndc = p / params.target_size * 2.0 - 1.0;
    float4 h   = mul(u_lit_scene.inverse_view_projection, float4(ndc.x, -ndc.y, z, 1.0));
    return h.xyz / h.w;
}

float along_view(float3 p) {
    return dot(p - u_lit_scene.eye_position.xyz, u_lit_scene.view_direction.xyz);
}

[shader("fragment")]
float4 rbtrend_lit_reflect_frag(float4 frag_coord : SV_Position) : SV_Target0 {
    const float4 nothing = float4(0.0, 0.0, 0.0, 0.0);
    int2 at   = int2(frag_coord.xy);
    float4 nr = reflect_normal_roughness.Load(int3(at, 0));
    if (dot(nr.xyz, nr.xyz) < 0.25)
        return nothing;
    float3 n        = normalize(nr.xyz);
    float z         = reflect_depth.Load(int3(at, 0)).r;
    float3 position = unproject(float2(at) + 0.5, z);
    float here      = along_view(position);

    uint half_width, half_height, half_levels;
    reflect_reflection.GetDimensions(0, half_width, half_height, half_levels);
    int2 full     = int2(params.target_size);
    int2 last     = int2(int(half_width), int(half_height)) - 1;
    float2 h      = (float2(at) + 0.5) * 0.5 - 0.5;
    int2 base     = int2(floor(h));
    float2 f      = h - float2(base);
    float4 sum    = float4(0.0, 0.0, 0.0, 0.0);
    float weights = 0.0;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            int2 q          = clamp(base + int2(x, y), int2(0, 0), last);
            float bilinear  = (x == 0 ? 1.0 - f.x : f.x) * (y == 0 ? 1.0 - f.y : f.y);
            // The texel q stands for: the nearest the eye of its 2x2.
            int2 best       = q * 2;
            float best_near = 2.0;
            for (int j = 0; j < 2; ++j) {
                for (int i = 0; i < 2; ++i) {
                    int2 c = q * 2 + int2(i, j);
                    if (c.x >= full.x || c.y >= full.y)
                        continue;
                    float near = nearness(reflect_depth.Load(int3(c, 0)).r);
                    if (near < best_near) {
                        best_near = near;
                        best      = c;
                    }
                }
            }
            float3 there = unproject(float2(best) + 0.5, reflect_depth.Load(int3(best, 0)).r);
            float3 other = reflect_normal_roughness.Load(int3(best, 0)).xyz;
            float near   = saturate(1.0 - abs(dot(there - position, n)) / max(DEPTH_SHARE * abs(here), 1e-6));
            float alike  = dot(other, other) > 0.25 ? pow(saturate(dot(normalize(other), n)), NORMAL_POWER) : 0.0;
            float w      = bilinear * near * alike;
            sum += w * reflect_reflection.Load(int3(q, 0));
            weights += w;
        }
    }
    if (!(weights > 0.0))
        return nothing;
    float4 reflection = sum / weights;
    if (!(reflection.a > 0.0))
        return nothing;

    // The environment specular the surface added, worked out again.
    float3 v        = normalize(u_lit_scene.eye_position.xyz - position);
    float3 r        = reflect(-v, n);
    float3 specular = hemisphere(r);
    if ((u_lit_scene.flags & RBTREND_LIT_SCENE_ENVIRONMENT) != 0u) {
        float top = max(u_lit_scene.environment_mips - 1.0, 0.0);
        specular += u_lit_scene.environment_intensity * environment_specular.SampleLevel(environment_sampler, to_environment(r), nr.w * top).rgb;
    }
    float3 weight = reflect_specular.Load(int3(at, 0)).rgb;
    float3 added  = weight * reflection.a * (reflection.rgb - specular) * u_lit_scene.exposure;
    return float4(added, 0.0);
}
