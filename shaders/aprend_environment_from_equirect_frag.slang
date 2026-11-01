// A face of the environment's radiance cube from the caller's
// equirectangular picture: longitude across, its middle column facing +Z,
// the north pole (+Y) at the top row.

import aprend_environment;

[[vk::binding(0, 0)]] Texture2D<float4> source : register(t0, space0);
[[vk::binding(1, 0)]] SamplerState source_sampler : register(s1, space0);

[shader("fragment")]
float4 aprend_environment_from_equirect_frag(float4 frag_coord : SV_Position) : SV_Target0 {
    float3 d  = cube_direction(frag_coord.xy, params.face);
    float2 uv = float2(atan2(d.x, d.z) / (2.0 * PI) + 0.5, acos(clamp(d.y, -1.0, 1.0)) / PI);
    return float4(source.SampleLevel(source_sampler, uv, 0.0).rgb, 1.0);
}
