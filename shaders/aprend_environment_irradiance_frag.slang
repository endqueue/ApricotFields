// A face of the environment's irradiance cube: the radiance cube's light on
// a face looking along this texel's direction, cosine weighted and divided
// by pi, so a white matte face lit by it reads it as it is - a uniform
// environment of radiance c gives c. Cosine-weighted importance sampling,
// each sample read from a mip that covers its share of the hemisphere.

import aprend_environment;

[[vk::binding(0, 0)]] TextureCube<float4> source : register(t0, space0);
[[vk::binding(1, 0)]] SamplerState source_sampler : register(s1, space0);

[shader("fragment")]
float4 aprend_environment_irradiance_frag(float4 frag_coord : SV_Position) : SV_Target0 {
    float3 n         = cube_direction(frag_coord.xy, params.face);
    float3x3 basis   = frame(n);
    float texel_area = 4.0 * PI / (6.0 * params.source_size * params.source_size);
    float3 sum       = float3(0.0);
    for (uint i = 0u; i < params.sample_count; ++i) {
        float2 xi   = hammersley(i, params.sample_count);
        float phi   = 2.0 * PI * xi.x;
        float cos_t = sqrt(1.0 - xi.y);
        float sin_t = sqrt(xi.y);
        float3 l    = mul(basis, float3(sin_t * cos(phi), sin_t * sin(phi), cos_t));
        float pdf   = max(cos_t, 1e-4) / PI;
        float solid = 1.0 / (float(params.sample_count) * pdf);
        float lod   = clamp(0.5 * log2(solid / texel_area) + 1.0, 0.0, params.source_mips - 1.0);
        sum += source.SampleLevel(source_sampler, l, lod).rgb;
    }
    return float4(sum / float(params.sample_count), 1.0);
}
