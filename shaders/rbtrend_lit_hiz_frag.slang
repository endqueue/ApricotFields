// RBTREND_COMMAND_LIT_SSR, first pass: level 0 of the Hi-Z pyramid, a depth
// buffer's texels as their nearness, z where depth is near at 0 and 1 - z
// where it is reversed (near at 1), so the nearer of two is always the
// smaller and the levels below take the least. Nearness, as z, is linear
// across the screen along a ray, which the march relies on.

[[vk::binding(0, 0)]] Texture2D<float4> depth_map : register(t0, space0);
[[vk::binding(1, 0)]] SamplerState depth_sampler : register(s1, space0);

// rbtrend_hiz_params in rbtrendlit_internal.hpp.
struct HizParams {
    float2 target_size;
    uint reversed;
    uint padding;
};
[[vk::push_constant]] ConstantBuffer<HizParams> params : register(b0, space4);

[shader("fragment")]
float4 rbtrend_lit_hiz_frag(float4 frag_coord : SV_Position) : SV_Target0 {
    float z = depth_map.Load(int3(int2(frag_coord.xy), 0)).r;
    return float4(params.reversed != 0u ? 1.0 - z : z, 0.0, 0.0, 0.0);
}
