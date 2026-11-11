// Bloom: a level of the chain read back up into the level above it, through
// a 3x3 tent of taps `radius` texels of the read level apart, and added to
// what that level holds by the pipeline's additive blend. Going up from the
// smallest level, each level ends as itself plus everything below it, so
// level 0 holds the sum of every level: a wide, soft glow built from narrow
// steps.
//
// Also rbtrend_bloom_composite_frag.slang's filter, with the target its own
// size. Places are taken from SV_Position, as rbtrend_downsample_frag.slang
// explains.

[[vk::binding(0, 0)]] Texture2D<float4> source_level : register(t0, space0);
[[vk::binding(1, 0)]] SamplerState source_sampler : register(s1, space0);

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
float4 rbtrend_bloom_upsample_frag(float4 frag_coord : SV_Position) : SV_Target0 {
    uint source_width, source_height, source_levels;
    source_level.GetDimensions(0, source_width, source_height, source_levels);
    float2 source_size = float2(float(source_width), float(source_height));
    // Not twice the source's: a level of an odd side halves to less.
    float2 uv          = frag_coord.xy / params.target_size;
    float2 t           = params.radius / source_size;

    float4 sum = tap(uv) * 4.0;
    sum += (tap(uv + t * float2( 0.0, -1.0)) + tap(uv + t * float2(-1.0,  0.0)) +
            tap(uv + t * float2( 1.0,  0.0)) + tap(uv + t * float2( 0.0,  1.0))) * 2.0;
    sum += tap(uv + t * float2(-1.0, -1.0)) + tap(uv + t * float2( 1.0, -1.0)) +
           tap(uv + t * float2(-1.0,  1.0)) + tap(uv + t * float2( 1.0,  1.0));
    return sum * (1.0 / 16.0);
}
