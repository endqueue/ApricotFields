// A separable Gaussian blur, one direction a pass: each texel the weighted
// sum of the source along `step` (one texel of the target, in uv, across or
// down), weights exp(-i^2 / 2 sigma^2) out to `radius` texels each side,
// normalized so they sum to 1. Taps go in pairs: one linear sample between
// texels i and i + 1, placed where its weights put it, reads both (Rakos,
// "Efficient Gaussian blur with linear sampling", 2010), so a radius of r
// costs r + 1 samples, not 2r + 1. Clamped to the edge by the sampler, so the
// edge texel stands in for what is past it.
//
// Places are taken from SV_Position, as aprend_downsample_frag.slang
// explains: the source is read at the same place in [0, 1], filtered.

[[vk::binding(0, 0)]] Texture2D<float4> source_level : register(t0, space0);
[[vk::binding(1, 0)]] SamplerState source_sampler : register(s1, space0);

struct BlurParams {
    // One texel of the target along the pass's direction, in uv.
    float2 step;
    float2 target_size;
    float sigma;
    // Whole texels each side: ceil(3 sigma).
    float radius;
};
[[vk::push_constant]] ConstantBuffer<BlurParams> params : register(b0, space4);

float4 tap(float2 uv) {
    return source_level.SampleLevel(source_sampler, uv, 0.0);
}

[shader("fragment")]
float4 aprend_blur_frag(float4 frag_coord : SV_Position) : SV_Target0 {
    float2 uv    = frag_coord.xy / params.target_size;
    float two_s2 = 2.0 * params.sigma * params.sigma;
    int radius   = int(params.radius);
    float4 sum   = tap(uv);
    float total  = 1.0;
    for (int i = 1; i <= radius; i += 2) {
        float a      = float(i);
        float w0     = exp(-a * a / two_s2);
        float w1     = i + 1 <= radius ? exp(-(a + 1.0) * (a + 1.0) / two_s2) : 0.0;
        float w      = w0 + w1;
        float offset = (a * w0 + (a + 1.0) * w1) / w;
        sum += (tap(uv + params.step * offset) + tap(uv - params.step * offset)) * w;
        total += 2.0 * w;
    }
    return sum / total;
}
