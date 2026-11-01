// A face of one mip of the environment's specular chain: the radiance cube
// convolved with GGX at this mip's roughness, by importance sampling with
// the reflection taken as the normal and the view (Karis, "Real Shading in
// Unreal Engine 4"), each sample read from the mip whose texels cover its
// share of the lobe (filtered importance sampling, Krivanek and Colbert),
// which keeps a few hundred samples free of fireflies. Roughness 0 is the
// picture itself.

import aprend_environment;

[[vk::binding(0, 0)]] TextureCube<float4> source : register(t0, space0);
[[vk::binding(1, 0)]] SamplerState source_sampler : register(s1, space0);

[shader("fragment")]
float4 aprend_environment_prefilter_frag(float4 frag_coord : SV_Position) : SV_Target0 {
    float3 n = cube_direction(frag_coord.xy, params.face);
    if (params.roughness <= 0.0)
        return float4(source.SampleLevel(source_sampler, n, 0.0).rgb, 1.0);
    float a          = params.roughness * params.roughness;
    float a2         = a * a;
    float3x3 basis   = frame(n);
    float texel_area = 4.0 * PI / (6.0 * params.source_size * params.source_size);
    float3 sum       = float3(0.0);
    float weight     = 0.0;
    for (uint i = 0u; i < params.sample_count; ++i) {
        float2 xi      = hammersley(i, params.sample_count);
        float phi      = 2.0 * PI * xi.x;
        float cos_t    = sqrt((1.0 - xi.y) / (1.0 + (a2 - 1.0) * xi.y));
        float sin_t    = sqrt(1.0 - cos_t * cos_t);
        float3 h       = mul(basis, float3(sin_t * cos(phi), sin_t * sin(phi), cos_t));
        float3 l       = 2.0 * dot(n, h) * h - n;
        float n_dot_l  = dot(n, l);
        if (n_dot_l <= 0.0)
            continue;
        // pdf of l = D(h) * cos(h) / (4 * dot(v, h)), with v = n.
        float n_dot_h    = max(dot(n, h), 0.0);
        float d          = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
        float pdf        = a2 / (PI * d * d) / 4.0;
        float solid      = 1.0 / (float(params.sample_count) * pdf + 1e-6);
        float lod        = clamp(0.5 * log2(solid / texel_area) + 1.0, 0.0, params.source_mips - 1.0);
        sum += source.SampleLevel(source_sampler, l, lod).rgb * n_dot_l;
        weight += n_dot_l;
    }
    return float4(weight > 0.0 ? sum / weight : float3(0.0), 1.0);
}
