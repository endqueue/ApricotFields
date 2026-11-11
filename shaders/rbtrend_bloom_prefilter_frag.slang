// Bloom, first pass: the source into level 0 of the bloom chain, at half its
// size, through the same 13 taps as rbtrend_bloom_downsample_frag.slang, then
// weighted by a threshold with a soft knee. A threshold and knee of 0 keep
// every texel as it is, which is what a caller that draws only what should
// glow into the source wants. The weight is taken from the brightest of
// red, green and blue and applied to all four channels, so premultiplied
// alpha stays premultiplied and the glow carries its own coverage.
//
// With a mask (set 1), each tap is the source times the mask's red at the
// same place in [0, 1], before the taps are summed: a masked texel adds
// nothing, however bright, rather than being blurred into its neighbours
// and then weighted.
//
// Places are taken from SV_Position, as rbtrend_downsample_frag.slang
// explains.

[[vk::binding(0, 0)]] Texture2D<float4> source_level : register(t0, space0);
[[vk::binding(1, 0)]] SamplerState source_sampler : register(s1, space0);
[[vk::binding(0, 1)]] Texture2D<float4> mask_level : register(t0, space1);
[[vk::binding(1, 1)]] SamplerState mask_sampler : register(s1, space1);

struct BloomParams {
    float threshold;
    float knee;
    float radius;
    float intensity;
    float2 target_size;
    // 1 to weight the prefilter's taps by the mask at set 1, 0 to read none.
    float use_mask;
    // What the lens dirt at set 1 adds to the composite's glow; 0 for none.
    float lens_dirt_intensity;
};
[[vk::push_constant]] ConstantBuffer<BloomParams> params : register(b0, space4);

float4 tap(float2 uv) {
    float4 texel = source_level.SampleLevel(source_sampler, uv, 0.0);
    if (params.use_mask > 0.5)
        texel *= mask_level.SampleLevel(mask_sampler, uv, 0.0).r;
    return texel;
}

[shader("fragment")]
float4 rbtrend_bloom_prefilter_frag(float4 frag_coord : SV_Position) : SV_Target0 {
    uint source_width, source_height, source_levels;
    source_level.GetDimensions(0, source_width, source_height, source_levels);
    float2 source_size = float2(float(source_width), float(source_height));
    // Level 0's size, which is half the size the bloom was made for, not
    // necessarily half the source's.
    float2 uv          = frag_coord.xy / params.target_size;
    float2 t           = 1.0 / source_size;

    float4 a = tap(uv + t * float2(-2.0, -2.0));
    float4 b = tap(uv + t * float2( 0.0, -2.0));
    float4 c = tap(uv + t * float2( 2.0, -2.0));
    float4 d = tap(uv + t * float2(-2.0,  0.0));
    float4 e = tap(uv);
    float4 f = tap(uv + t * float2( 2.0,  0.0));
    float4 g = tap(uv + t * float2(-2.0,  2.0));
    float4 h = tap(uv + t * float2( 0.0,  2.0));
    float4 i = tap(uv + t * float2( 2.0,  2.0));
    float4 j = tap(uv + t * float2(-1.0, -1.0));
    float4 k = tap(uv + t * float2( 1.0, -1.0));
    float4 l = tap(uv + t * float2(-1.0,  1.0));
    float4 m = tap(uv + t * float2( 1.0,  1.0));
    float4 color = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;

    float brightness = max(color.r, max(color.g, color.b));
    float soft       = clamp(brightness - params.threshold + params.knee, 0.0, 2.0 * params.knee);
    soft             = soft * soft / (4.0 * params.knee + 1e-5);
    float weight     = max(soft, brightness - params.threshold) / max(brightness, 1e-5);
    return color * weight;
}
