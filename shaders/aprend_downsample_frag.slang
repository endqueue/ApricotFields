// One mip level from the level above it, through a view of that one level.
//
// With a linear filter (params.box) each target texel is the average of the
// source texels its footprint covers, each weighted by how much of it the
// footprint covers: an exact box filter. On an even side the footprint is
// two whole texels and this is the average of each 2x2 block; on an odd side
// it is two and a bit, so a texel shared by two footprints is split between
// them, and every source texel weighs the same in the level below. (A
// filter of two taps a side, which bilinear sampling at the shared corner
// gives, weighs the last texel of an odd side less.) The texels are read
// with Load, so the sampler's filter is not what averages them.
//
// With a nearest filter, each target texel reads the source at its centre's
// place in [0, 1] through the sampler: one source texel.
//
// The place is taken from SV_Position, not from an interpolated coordinate,
// so it reads the same on every backend: the target's and the source's
// texel rows both start at the top.
//
// A separate texture and sampler, not a combined one: one sampler serves
// every level's set, and SpudGPU's Metal backend had no combined image
// sampler when this was written (it has since 2026-10-22).
//
// The source is a 2D array view of the one layer being filled, read at
// slice 0, rather than a 2D view: D3D12 has no 2D view of a layer past the
// first, only an array view of it, and a shader that declares a 2D texture
// and is given an array view is undefined there. Declared as an array, the
// view and the shader agree on every backend.

[[vk::binding(0, 0)]] Texture2DArray<float4> source_level : register(t0, space0);
[[vk::binding(1, 0)]] SamplerState source_sampler : register(s1, space0);

// aprend_downsample_params: x is 1 for the box filter, 0 for nearest.
struct DownsampleParams {
    uint4 mode;
};
[[vk::push_constant]] ConstantBuffer<DownsampleParams> params : register(b0, space4);

[shader("fragment")]
float4 aprend_downsample_frag(float4 frag_coord : SV_Position) : SV_Target0 {
    uint source_width, source_height, source_layers, source_levels;
    source_level.GetDimensions(0, source_width, source_height, source_layers, source_levels);
    int2 source_size = int2(int(source_width), int(source_height));
    int2 target_size = max(source_size >> 1, int2(1));
    if (params.mode.x == 0u)
        return source_level.SampleLevel(source_sampler, float3(frag_coord.xy / float2(target_size), 0.0), 0.0);

    // The footprint of this target texel, in source texels.
    float2 scale = float2(source_size) / float2(target_size);
    float2 low   = floor(frag_coord.xy) * scale;
    float2 high  = low + scale;
    int2 first   = int2(floor(low));
    int2 last    = min(int2(ceil(high)) - 1, source_size - 1);

    float4 sum   = float4(0.0);
    float weight = 0.0;
    for (int y = first.y; y <= last.y; ++y) {
        float wy = min(high.y, float(y + 1)) - max(low.y, float(y));
        for (int x = first.x; x <= last.x; ++x) {
            float w = (min(high.x, float(x + 1)) - max(low.x, float(x))) * wy;
            sum += source_level.Load(int4(x, y, 0, 0)) * w;
            weight += w;
        }
    }
    return sum / weight;
}
