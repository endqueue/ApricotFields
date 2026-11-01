import aprend_lit_scene;
import aprend_rt_trace;
import aprend_rt_path_trace;

// APREND_COMMAND_LIT_PATH_TRACE with an albedo guide of R16G16B16A16_FLOAT and a
// normal guide of R16G16B16A16_FLOAT (an absent guide is the instance's 1x1
// R32G32B32A32_FLOAT, unwritten). aprend_rt_path_trace.slang has the body.
// Set 0 the scene's block, set 1 the TLAS (aprend_rt_trace.slang), set 2
// the accumulation and the guides, each the running mean of its samples.

[[vk::binding(0, 2)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> path_target : register(u0, space2);
[[vk::binding(1, 2)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> path_albedo : register(u1, space2);
[[vk::binding(2, 2)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> path_normal : register(u2, space2);
[[vk::push_constant]] ConstantBuffer<TraceParams> params : register(b0, space4);

// The running mean at [old] of [held] samples, with [added] more summing
// to [sum]: what it was is not read when it held none.
float4 running_mean(float4 old, uint held, float3 sum, uint added) {
    float3 before = held > 0u ? old.rgb * float(held) : float3(0.0, 0.0, 0.0);
    return float4((before + sum) / (float(held) + float(added)), 1.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void aprend_rt_path_trace_f16_f16_comp(uint3 id : SV_DispatchThreadID) {
    TraceParams p = params;
    if (id.x >= p.size.x || id.y >= p.size.y)
        return;
    const uint seed = pixel_seed(id.xy, p);
    float3 radiance = float3(0.0, 0.0, 0.0);
    float3 albedo   = float3(0.0, 0.0, 0.0);
    float3 normal   = float3(0.0, 0.0, 0.0);
    for (uint s = 0u; s < p.samples; ++s) {
        PathResult r = trace_path(id.xy, p.accumulated + s, seed, p);
        radiance += r.radiance;
        albedo += r.albedo;
        normal += r.normal;
    }
    const bool held = p.accumulated > 0u;
    path_target[id.xy] = running_mean(held ? path_target[id.xy] : float4(0.0, 0.0, 0.0, 0.0), p.accumulated, radiance, p.samples);
    if ((p.guides & 1u) != 0u)
        path_albedo[id.xy] = running_mean(held ? path_albedo[id.xy] : float4(0.0, 0.0, 0.0, 0.0), p.accumulated, albedo, p.samples);
    if ((p.guides & 2u) != 0u)
        path_normal[id.xy] = running_mean(held ? path_normal[id.xy] : float4(0.0, 0.0, 0.0, 0.0), p.accumulated, normal, p.samples);
}
