// Bloom, last pass: level 0 of the chain spread over the whole target
// through the same tent as aprend_bloom_upsample_frag.slang, times the
// intensity, and added to what the target holds by the pipeline's additive
// blend: color and alpha alike, so a glow over a clear target has coverage
// of its own. The target may be any size; the chain is read at the same
// place in [0, 1].
//
// With lens dirt (set 1), laid over the whole target, the glow's color is
// times 1 + dirt * lens_dirt_intensity: brighter where the dirt is, and
// nothing where there is no glow. Alpha is the glow's alone.
//
// Places are taken from SV_Position, as aprend_downsample_frag.slang
// explains.

[[vk::binding(0, 0)]] Texture2D<float4> source_level : register(t0, space0);
[[vk::binding(1, 0)]] SamplerState source_sampler : register(s1, space0);
[[vk::binding(0, 1)]] Texture2D<float4> dirt_level : register(t0, space1);
[[vk::binding(1, 1)]] SamplerState dirt_sampler : register(s1, space1);

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
    return source_level.SampleLevel(source_sampler, uv, 0.0);
}

[shader("fragment")]
float4 aprend_bloom_composite_frag(float4 frag_coord : SV_Position) : SV_Target0 {
    uint source_width, source_height, source_levels;
    source_level.GetDimensions(0, source_width, source_height, source_levels);
    float2 source_size = float2(float(source_width), float(source_height));
    float2 uv          = frag_coord.xy / params.target_size;
    float2 t           = params.radius / source_size;

    float4 sum = tap(uv) * 4.0;
    sum += (tap(uv + t * float2( 0.0, -1.0)) + tap(uv + t * float2(-1.0,  0.0)) +
            tap(uv + t * float2( 1.0,  0.0)) + tap(uv + t * float2( 0.0,  1.0))) * 2.0;
    sum += tap(uv + t * float2(-1.0, -1.0)) + tap(uv + t * float2( 1.0, -1.0)) +
           tap(uv + t * float2(-1.0,  1.0)) + tap(uv + t * float2( 1.0,  1.0));
    float4 out_color = sum * (params.intensity / 16.0);
    if (params.lens_dirt_intensity > 0.0) {
        float3 dirt = dirt_level.SampleLevel(dirt_sampler, uv, 0.0).rgb;
        out_color.rgb *= 1.0 + dirt * params.lens_dirt_intensity;
    }
    return out_color;
}
