// RBTREND_COMMAND_AUTO_EXPOSURE, first pass: the base 2 log of the source's
// luminance (Rec. 709 weights), read filtered at this texel's place, into
// the exposure's luminance texture, whose mips are then averaged down to
// one texel: the log-average luminance (Reinhard et al.).

[[vk::binding(0, 0)]] Texture2D<float4> source : register(t0, space0);
[[vk::binding(1, 0)]] SamplerState source_sampler : register(s1, space0);

struct LuminanceParams {
    float2 target_size;
};
[[vk::push_constant]] ConstantBuffer<LuminanceParams> params : register(b0, space4);

[shader("fragment")]
float4 rbtrend_exposure_luminance_frag(float4 frag_coord : SV_Position) : SV_Target0 {
    float3 c = source.SampleLevel(source_sampler, frag_coord.xy / params.target_size, 0.0).rgb;
    float l  = dot(max(c, float3(0.0)), float3(0.2126, 0.7152, 0.0722));
    return float4(log2(max(l, 1e-6)), 0.0, 0.0, 1.0);
}
