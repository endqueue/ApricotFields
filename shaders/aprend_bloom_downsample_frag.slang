// Bloom: one level of the chain from the level above, at half its size,
// through 13 taps (Jimenez, "Next Generation Post Processing in Call of
// Duty: Advanced Warfare"): a box of the four texels round the target
// texel's centre, weighted 1/2, and four overlapping boxes round it, 1/8
// each. A plain 2x2 box lets a single bright texel flicker as it moves
// across texel boundaries; this does not.
//
// Places are taken from SV_Position, as aprend_downsample_frag.slang
// explains.

[[vk::binding(0, 0)]] Texture2D<float4> source_level : register(t0, space0);
[[vk::binding(1, 0)]] SamplerState source_sampler : register(s1, space0);

float4 tap(float2 uv) {
    return source_level.SampleLevel(source_sampler, uv, 0.0);
}

[shader("fragment")]
float4 aprend_bloom_downsample_frag(float4 frag_coord : SV_Position) : SV_Target0 {
    uint source_width, source_height, source_levels;
    source_level.GetDimensions(0, source_width, source_height, source_levels);
    float2 source_size = float2(float(source_width), float(source_height));
    float2 target_size = max(floor(source_size * 0.5), float2(1.0));
    float2 uv          = frag_coord.xy / target_size;
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
    return e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
}
