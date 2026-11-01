// A face of the environment's radiance cube from the caller's cube, read
// filtered at its own first mip.

import aprend_environment;

[[vk::binding(0, 0)]] TextureCube<float4> source : register(t0, space0);
[[vk::binding(1, 0)]] SamplerState source_sampler : register(s1, space0);

[shader("fragment")]
float4 aprend_environment_from_cube_frag(float4 frag_coord : SV_Position) : SV_Target0 {
    float3 d = cube_direction(frag_coord.xy, params.face);
    return float4(source.SampleLevel(source_sampler, d, 0.0).rgb, 1.0);
}
