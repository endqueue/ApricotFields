// APREND_COMMAND_LIT_SSR (aprendcommands.h): screen-space reflections, a
// thread a texel of the half-resolution reflection target.
//
// Each texel takes the texel nearest the eye of the 2x2 of the depth buffer
// under it (fewer at the right and bottom edges of an odd size), so a texel
// on an edge is the foreground's and never a blend of two surfaces, and
// that texel's normal and roughness. The view ray is reflected about the
// normal and the reflected ray, cut to _max_distance and to what lies in
// front of the eye, is projected onto the screen, where it is a straight
// line in (x, y, nearness): nearness, the depth with near at 0, is linear
// across the screen. It is marched over the Hi-Z pyramid (Uludag, "Hi-Z
// Screen-Space Cone-Traced Reflections", GPU Pro 5; Hazel's SSR.glsl and
// AMD's FidelityFX SSSR the same): a cell of a level whose nearest depth the
// ray stays in front of is crossed whole and the march goes up a level; one
// it reaches is gone down into, until at level 0 the ray is at or behind a
// texel's depth. There it hits if it is within _thickness behind it, in the
// scene's units along the view, and otherwise steps on past that texel.
// Each cell visited is a step, _max_steps at most.
//
// A hit reads the mipped copy of the picture at its texel, at the mip a
// cone over the roughness's GGX lobe covers at the distance travelled on
// the screen (Uludag's cone, its angle from the lobe's Phong power), and
// undoes the picture's encoding and exposure. Its confidence fades to 0
// within _edge_fade of the screen's edges and as the ray turns back towards
// the eye. A miss, a texel with no surface (a normal of 0) and a surface
// rougher than _max_roughness write 0.

import aprend_lit_scene;

[[vk::binding(0, 1)]] Texture2D<float4> ssr_depth : register(t0, space1);
[[vk::binding(1, 1)]] Texture2D<float4> ssr_normal_roughness : register(t1, space1);
[[vk::binding(2, 1)]] Texture2D<float4> ssr_color : register(t2, space1);
[[vk::binding(3, 1)]] Texture2D<float4> ssr_hiz : register(t3, space1);
[[vk::binding(4, 1)]] SamplerState ssr_sampler : register(s4, space1);
[[vk::binding(5, 1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> ssr_reflection : register(u5, space1);

// aprend_ssr_params in aprendlit_internal.hpp.
struct SsrParams {
    uint2 size;          // the reflection target's
    uint2 full;          // the depth buffer's, the view's target size
    uint max_steps;
    float max_distance;
    float thickness;
    float edge_fade;
    float max_roughness;
    uint hiz_levels;
    uint reversed;       // the depth buffer is near at 1
    float color_mips;
};
[[vk::push_constant]] ConstantBuffer<SsrParams> params : register(b0, space4);

// The share of a pixel a crossing is pushed past a cell's edge by, so the
// next cell is the one beyond it.
static const float CROSS = 1.0 / 512.0;

float nearness(float z) {
    return params.reversed != 0u ? 1.0 - z : z;
}

float depth_of(float near) {
    return params.reversed != 0u ? 1.0 - near : near;
}

// The scene-space point at [p] on the full target (in texels, the first row
// at the top) at depth [z]: clip space's +Y is up the target, so y is
// negated.
float3 unproject(float2 p, float z) {
    float2 ndc = p / float2(params.full) * 2.0 - 1.0;
    float4 h   = mul(u_lit_scene.inverse_view_projection, float4(ndc.x, -ndc.y, z, 1.0));
    return h.xyz / h.w;
}

// [clip] on the full target: texels, and nearness.
float3 to_screen(float4 clip) {
    float3 ndc = clip.xyz / clip.w;
    return float3((ndc.x * 0.5 + 0.5) * float(params.full.x), (0.5 - ndc.y * 0.5) * float(params.full.y), nearness(ndc.z));
}

// How far [p] is along the view.
float along_view(float3 p) {
    return dot(p - u_lit_scene.eye_position.xyz, u_lit_scene.view_direction.xyz);
}

// The half-angle's tangent of a cone over a GGX lobe of [roughness]: the
// lobe's Phong power 2 / a^2 - 2 (a the roughness squared), and the angle
// within which 0.244 of a Phong lobe of that power falls (Uludag).
float cone_tangent(float roughness) {
    float a     = roughness * roughness;
    float power = max(2.0 / max(a * a, 1e-12) - 2.0, 0.0);
    float c     = exp(log(0.244) / (power + 1.0));
    return sqrt(max(1.0 - c * c, 0.0)) / max(c, 1e-6);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void aprend_lit_ssr_comp(uint3 id : SV_DispatchThreadID) {
    SsrParams p = params;
    if (id.x >= p.size.x || id.y >= p.size.y)
        return;
    const float4 none = float4(0.0, 0.0, 0.0, 0.0);

    // The texel of the 2x2 nearest the eye; the first in raster order of
    // two as near.
    int2 best       = int2(id.xy) * 2;
    float best_near = 2.0;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            int2 q = int2(id.xy) * 2 + int2(x, y);
            if (q.x >= int(p.full.x) || q.y >= int(p.full.y))
                continue;
            float n = nearness(ssr_depth.Load(int3(q, 0)).r);
            if (n < best_near) {
                best_near = n;
                best      = q;
            }
        }
    }
    float4 nr = ssr_normal_roughness.Load(int3(best, 0));
    if (dot(nr.xyz, nr.xyz) < 0.25 || !(nr.w <= p.max_roughness)) {
        ssr_reflection[id.xy] = none;
        return;
    }
    float3 n        = normalize(nr.xyz);
    float roughness = nr.w;
    float3 origin   = unproject(float2(best) + 0.5, depth_of(best_near));
    float3 view     = normalize(origin - u_lit_scene.eye_position.xyz);
    float3 r        = reflect(view, n);
    // 1 for a ray across the view or away from the eye, fading to 0 as it
    // turns back towards it.
    float facing = saturate(1.0 + dot(r, view));

    // The ray on the screen, cut where it would pass behind the eye.
    float4 c0 = mul(u_lit_scene.view_projection, float4(origin, 1.0));
    float4 c1 = mul(u_lit_scene.view_projection, float4(origin + r * p.max_distance, 1.0));
    float w_min = c0.w * 1e-3;
    if (c1.w < w_min)
        c1 = lerp(c0, c1, (c0.w - w_min) / (c0.w - c1.w));
    float3 s0      = to_screen(c0);
    float3 s1      = to_screen(c1);
    float3 d       = s1 - s0;
    float pixels   = length(d.xy);
    if (!(pixels >= 1.0) || facing <= 0.0) {
        ssr_reflection[id.xy] = none;
        return;
    }
    // A texel's own depth isn't a hit: the march starts a texel along.
    float t           = 1.0 / pixels;
    int level         = 0;
    int top           = int(p.hiz_levels) - 1;
    bool hit          = false;
    float3 at         = s0;
    float2 inverse    = float2(abs(d.x) > 1e-6 ? 1.0 / d.x : 1e30, abs(d.y) > 1e-6 ? 1.0 / d.y : 1e30);
    float2 full       = float2(p.full);
    for (uint step = 0u; step < p.max_steps && t <= 1.0; ++step) {
        at = s0 + d * t;
        if (any(at.xy < float2(0.0, 0.0)) || any(at.xy >= full))
            break;
        uint level_width, level_height, level_count;
        ssr_hiz.GetDimensions(uint(level), level_width, level_height, level_count);
        int2 cell        = min(int2(at.xy) >> level, int2(int(level_width), int(level_height)) - 1);
        float cell_near  = ssr_hiz.Load(int3(cell, level)).r;
        // Where the ray leaves the cell across the screen; the last cell
        // of an odd level reaches to the target's edge.
        float size       = float(1 << level);
        float2 low       = float2(cell) * size;
        float2 high      = low + size;
        if (cell.x == int(level_width) - 1)
            high.x = full.x;
        if (cell.y == int(level_height) - 1)
            high.y = full.y;
        float2 edge      = float2(d.x > 0.0 ? high.x : low.x, d.y > 0.0 ? high.y : low.y);
        float2 crossings = (edge - s0.xy) * inverse;
        float leave      = min(abs(d.x) > 1e-6 ? crossings.x : 1e30, abs(d.y) > 1e-6 ? crossings.y : 1e30);
        if (at.z < cell_near) {
            // In front of everything in the cell: where it would reach the
            // cell's nearest depth.
            float reach = d.z > 0.0 ? t + (cell_near - at.z) / d.z : 1e30;
            if (reach >= leave) {
                t     = leave + CROSS / pixels;
                level = min(level + 1, top);
                continue;
            }
            t = max(reach, t);
            if (level > 0) {
                --level;
                continue;
            }
            at = s0 + d * t;
        } else if (level > 0) {
            --level;
            continue;
        }
        // At level 0, at or behind the texel's depth: a hit within the
        // thickness behind it, and otherwise on past it.
        int2 texel    = min(int2(at.xy), int2(p.full) - 1);
        float surface = along_view(unproject(float2(texel) + 0.5, ssr_depth.Load(int3(texel, 0)).r));
        float ray     = along_view(unproject(at.xy, depth_of(at.z)));
        if (ray - surface <= p.thickness) {
            hit = true;
            break;
        }
        t = leave + CROSS / pixels;
    }
    if (!hit) {
        ssr_reflection[id.xy] = none;
        return;
    }

    // The hit's texel, through a cone at the distance travelled.
    float2 texel_at = floor(at.xy) + 0.5;
    float2 uv       = texel_at / full;
    uint color_width, color_height, color_levels;
    ssr_color.GetDimensions(0, color_width, color_height, color_levels);
    float travelled = length(at.xy - s0.xy) * float(color_width) / full.x;
    float diameter  = 2.0 * cone_tangent(roughness) * travelled;
    float lod       = clamp(log2(max(diameter, 1.0)), 0.0, max(p.color_mips - 1.0, 0.0));
    float3 color    = ssr_color.SampleLevel(ssr_sampler, uv, lod).rgb;
    if (u_lit_scene.output_encoding == 1u) // written encoded: back to linear
        color = select(color <= float3(0.04045), color / 12.92, pow((color + 0.055) / 1.055, float3(2.4)));
    color /= max(u_lit_scene.exposure, 1e-20);
    float2 from_edge = min(uv, 1.0 - uv);
    float fade       = p.edge_fade > 0.0 ? saturate(min(from_edge.x, from_edge.y) / p.edge_fade) : 1.0;
    ssr_reflection[id.xy] = float4(color, fade * facing);
}
