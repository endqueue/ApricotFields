import rbtrend_lit_scene;
import rbtrend_rt_trace;
import rbtrend_rt_path_trace;

// RBTREND_COMMAND_LIT_TRACE_REFLECTIONS (rbtrendcommands.h): ray-traced
// reflections where the march's confidence is short of 1, a thread a texel
// of the half-resolution reflection target. Set 0 the scene's block, set 1
// the TLAS (rbtrend_rt_trace.slang), set 2 the targets and what a ray that
// misses, or a hit's ambient light, reads.
//
// Each texel takes the surface LIT_SSR takes (the texel of the 2x2 under it
// nearest the eye) and traces _ray_count rays from it along its reflection:
// over the GGX lobe's visible normals (Heitz, 2018) at the surface's
// roughness, in directions of Roberts' R2 sequence turned per texel by
// interleaved gradient noise, as LIT_SHADOW_MASK's are. A hit is the lit
// material's first bounce as the path trace has it (LitBsdf): its emission,
// every directional light and every point and spot light unshadowed, and
// the ambient light through the split sum as the raster surface has it; a
// miss is the hemisphere and the environment along the ray. The mean of the
// rays goes under what the march found, by its confidence, with a
// confidence of 1. Nothing is kept from one frame to the next, and nothing
// is filtered.

[[vk::binding(0, 2)]] Texture2D<float4> trace_depth : register(t0, space2);
[[vk::binding(1, 2)]] Texture2D<float4> trace_normal_roughness : register(t1, space2);
[[vk::binding(2, 2)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> trace_reflection : register(u2, space2);
[[vk::binding(3, 2)]] TextureCube<float4> trace_specular : register(t3, space2);
[[vk::binding(4, 2)]] TextureCube<float4> trace_irradiance : register(t4, space2);
[[vk::binding(5, 2)]] Texture2D<float4> trace_brdf_lut : register(t5, space2);
[[vk::binding(6, 2)]] SamplerState trace_sampler : register(s6, space2);

// rbtrend_rt_reflection_params in rbtrendraytracing_internal.hpp.
struct ReflectionParams {
    uint2 size;            // the reflection target's
    uint2 full;            // the depth buffer's, the view's target size
    uint ray_count;
    uint cull_mask;
    float max_distance;
    float max_roughness;
    uint material_count;   // the TLAS's material table's length
    uint reversed;         // the depth buffer is near at 1
    uint2 padding;
};
[[vk::push_constant]] ConstantBuffer<ReflectionParams> params : register(b0, space4);

static const float MIN_ROUGHNESS = 0.045;
// How far a ray starts off the surface it leaves, as a share of the
// surface's distance from the eye: a point brought back from a depth
// buffer is only as exact as the depth, which is coarser the farther it is.
static const float LEAVE = 1e-3;

float nearness(float z) {
    return params.reversed != 0u ? 1.0 - z : z;
}

float depth_of(float near) {
    return params.reversed != 0u ? 1.0 - near : near;
}

float3 unproject(float2 p, float z) {
    float2 ndc = p / float2(params.full) * 2.0 - 1.0;
    float4 h   = mul(u_lit_scene.inverse_view_projection, float4(ndc.x, -ndc.y, z, 1.0));
    return h.xyz / h.w;
}

// The scene's ambient light along [d]: the hemisphere, and the
// environment's level [level] of its specular chain.
float3 ambient_along(float3 d, float level) {
    float3 light = hemisphere(d);
    if ((u_lit_scene.flags & RBTREND_LIT_SCENE_ENVIRONMENT) != 0u)
        light += u_lit_scene.environment_intensity * trace_specular.SampleLevel(trace_sampler, to_environment(d), level).rgb;
    return light;
}

// A direction about [n] over the visible normals of a GGX lobe of [alpha]
// seen from [v], from [u] in [0, 1)^2, reflected into the light it reads.
float3 lobe_direction(float3 n, float3 v, float alpha, float2 u) {
    float3 t  = abs(n.z) < 0.999 ? normalize(cross(n, float3(0.0, 0.0, 1.0))) : float3(1.0, 0.0, 0.0);
    float3 b  = cross(n, t);
    float3 vl = float3(dot(v, t), dot(v, b), dot(v, n));
    float3 vh = normalize(float3(alpha * vl.x, alpha * vl.y, vl.z));
    float len = vh.x * vh.x + vh.y * vh.y;
    float3 t1 = len > 0.0 ? float3(-vh.y, vh.x, 0.0) / sqrt(len) : float3(1.0, 0.0, 0.0);
    float3 t2 = cross(vh, t1);
    float r   = sqrt(u.x);
    float phi = 2.0 * RT_PI * u.y;
    float p1  = r * cos(phi);
    float p2  = r * sin(phi);
    float s   = 0.5 * (1.0 + vh.z);
    p2        = (1.0 - s) * sqrt(max(0.0, 1.0 - p1 * p1)) + s * p2;
    float3 nh = p1 * t1 + p2 * t2 + sqrt(max(0.0, 1.0 - p1 * p1 - p2 * p2)) * vh;
    float3 hl = normalize(float3(alpha * nh.x, alpha * nh.y, max(0.0, nh.z)));
    float3 h  = t * hl.x + b * hl.y + n * hl.z;
    return reflect(-v, h);
}

// What a hit sends back along [direction]: the first bounce of the path
// trace's lit material, lit by every light unshadowed and by the ambient
// light through the split sum.
float3 shade_hit(Hit hit, float3 direction) {
    float3 v = -direction;
    LitBsdf bsdf;
    bsdf.init(hit.normal, v, hit.base_color, hit.surface);
    float3 radiance = hit.emission;
    float pdf       = 0.0;
    for (uint i = 0u; i < min(u_lit_scene.directional_count, uint(RBTREND_LIT_MAX_DIRECTIONAL_LIGHTS)); ++i) {
        RbtrendLitDirectional d = u_lit_scene.directional[i];
        radiance += RT_PI * bsdf.eval(d.direction_versine.xyz, pdf) * d.radiance_disc.rgb;
    }
    for (uint i = 0u; i < min(u_lit_scene.light_count, uint(RBTREND_LIT_MAX_LIGHTS)); ++i) {
        RbtrendLitLight light = u_lit_scene.lights[i];
        float3 to_light = light.position_range.xyz - hit.position;
        float distance2 = max(dot(to_light, to_light), 1e-8);
        float distance  = sqrt(distance2);
        float range     = light.position_range.w;
        if (distance >= range)
            continue;
        float3 l     = to_light / distance;
        float ratio  = distance / range;
        float window = clamp(1.0 - ratio * ratio * ratio * ratio, 0.0, 1.0);
        float fall   = window * window / distance2;
        if (light.radiance_type.w > 0.5) {
            float cos_axis = dot(-l, light.direction_inner.xyz);
            float cone     = clamp((cos_axis - light.outer_shadow.x) / max(light.direction_inner.w - light.outer_shadow.x, 1e-4), 0.0, 1.0);
            fall *= cone * cone;
        }
        if (fall > 0.0)
            radiance += RT_PI * bsdf.eval(l, pdf) * light.radiance_type.rgb * fall;
    }
    // The ambient light, as the raster surface has it without occlusion.
    float metalness = clamp(hit.surface.y, 0.0, 1.0);
    float3 albedo   = max(hit.base_color.rgb, float3(0.0, 0.0, 0.0));
    float roughness = clamp(hit.surface.x, MIN_ROUGHNESS, 1.0);
    float3 f0       = lerp(float3(0.08 * clamp(hit.surface.z, 0.0, 1.0)), albedo, metalness);
    float3 diffuse  = albedo * (1.0 - metalness);
    float n_dot_v   = max(dot(hit.normal, v), 1e-4);
    float2 ab       = trace_brdf_lut.SampleLevel(trace_sampler, float2(n_dot_v, roughness), 0.0).rg;
    float3 ambient_diffuse = hemisphere(hit.normal);
    if ((u_lit_scene.flags & RBTREND_LIT_SCENE_ENVIRONMENT) != 0u)
        ambient_diffuse += u_lit_scene.environment_intensity * trace_irradiance.SampleLevel(trace_sampler, to_environment(hit.normal), 0.0).rgb;
    float top = max(u_lit_scene.environment_mips - 1.0, 0.0);
    radiance += diffuse * ambient_diffuse + (f0 * ab.x + ab.y) * ambient_along(reflect(direction, hit.normal), roughness * top);
    return radiance;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void rbtrend_rt_reflections_comp(uint3 id : SV_DispatchThreadID) {
    ReflectionParams p = params;
    if (id.x >= p.size.x || id.y >= p.size.y)
        return;
    float4 marched = trace_reflection[id.xy];
    if (!(marched.a < 1.0))
        return;
    int2 best       = int2(id.xy) * 2;
    float best_near = 2.0;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            int2 q = int2(id.xy) * 2 + int2(x, y);
            if (q.x >= int(p.full.x) || q.y >= int(p.full.y))
                continue;
            float n = nearness(trace_depth.Load(int3(q, 0)).r);
            if (n < best_near) {
                best_near = n;
                best      = q;
            }
        }
    }
    float4 nr = trace_normal_roughness.Load(int3(best, 0));
    if (dot(nr.xyz, nr.xyz) < 0.25 || !(nr.w <= p.max_roughness))
        return;
    float3 n        = normalize(nr.xyz);
    float alpha     = nr.w * nr.w;
    float3 surface  = unproject(float2(best) + 0.5, depth_of(best_near));
    float3 to_eye   = u_lit_scene.eye_position.xyz - surface;
    float3 v        = normalize(to_eye);
    float3 origin   = surface + n * (LEAVE * length(to_eye));
    float2 turn     = float2(rt_interleaved_gradient(float2(id.xy)), rt_interleaved_gradient(float2(id.xy) + float2(5.588238, 5.588238)));
    float3 traced   = float3(0.0, 0.0, 0.0);
    for (uint i = 0u; i < p.ray_count; ++i) {
        float3 direction = lobe_direction(n, v, alpha, frac(rt_r2(i) + turn));
        if (dot(direction, n) <= 0.0)
            direction = reflect(-v, n);
        RayDesc ray;
        ray.Origin    = origin;
        ray.Direction = direction;
        ray.TMin      = 0.0;
        ray.TMax      = p.max_distance;
        RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
        query.TraceRayInline(rt_scene, RAY_FLAG_NONE, p.cull_mask, ray);
        while (query.Proceed()) {
        }
        if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
            Hit hit = hit_at(query.CommittedInstanceIndex(), query.CommittedGeometryIndex(), query.CommittedPrimitiveIndex(),
                             query.CommittedTriangleBarycentrics(), query.CommittedObjectToWorld3x4(), direction, p.material_count);
            traced += shade_hit(hit, direction);
        } else
            traced += ambient_along(direction, 0.0);
    }
    traced /= float(p.ray_count);
    trace_reflection[id.xy] = float4(lerp(traced, marched.rgb, marched.a), 1.0);
}
