// RBTREND_COMMAND_LIT_AMBIENT_OCCLUSION, second pass: the raw estimate
// averaged over 4x4 texels, which is the period of the first pass's turns,
// onto the caller's target: the openness in red, and the bent normals (in
// green, blue and alpha as 0.5 + 0.5 b) summed, made unit again and written
// back the same way.

[[vk::binding(0, 0)]] Texture2D<float4> raw : register(t0, space0);
[[vk::binding(1, 0)]] SamplerState raw_sampler : register(s1, space0);

struct BlurParams {
    float2 target_size;
};
[[vk::push_constant]] ConstantBuffer<BlurParams> params : register(b0, space4);

[shader("fragment")]
float4 rbtrend_lit_occlusion_blur_frag(float4 frag_coord : SV_Position) : SV_Target0 {
    uint raw_width, raw_height, raw_levels;
    raw.GetDimensions(0, raw_width, raw_height, raw_levels);
    int2 size    = int2(int(raw_width), int(raw_height));
    float2 scale = float2(size) / params.target_size;
    int2 at      = int2(frag_coord.xy * scale);
    float sum    = 0.0;
    float3 bent  = float3(0.0);
    for (int y = -2; y < 2; ++y) {
        for (int x = -2; x < 2; ++x) {
            float4 texel = raw.Load(int3(clamp(at + int2(x, y), int2(0), size - 1), 0));
            sum += texel.r;
            bent += texel.gba * 2.0 - 1.0;
        }
    }
    float open = sum / 16.0;
    float3 b   = dot(bent, bent) > 0.0 ? normalize(bent) : float3(0.0);
    return float4(open, b * 0.5 + 0.5);
}
