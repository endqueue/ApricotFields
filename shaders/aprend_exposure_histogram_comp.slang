// APREND_COMMAND_AUTO_EXPOSURE's histogram, for an exposure that leaves the
// darkest and brightest of a picture out of its measure: how many texels of
// the exposure's log luminance texture (aprend_exposure_luminance_frag.slang,
// mip 0) fall in each of 64 even bins from min_log to max_log, the ends
// taking what is past them. Each group of 16x16 texels counts into its own
// bins in shared memory, then adds them to the buffer's: one atomic a bin a
// group, not one a texel (Unreal's PostProcessHistogram works the same
// way). A dispatch with `clear` set, one group, empties the buffer first.

// AprendExposureHistogram's set 0, shared with aprend_exposure_adapt_histogram_frag.slang.
[[vk::binding(0, 0)]] Texture2D<float4> luminance : register(t0, space0);
[[vk::binding(1, 0)]] SamplerState luminance_sampler : register(s1, space0);
// 64 bins, a uint each. Raw (byte addressed), as SpudGPU's D3D12 backend
// views every storage buffer.
[[vk::binding(2, 0)]] RWByteAddressBuffer AprendExposureBins : register(u2, space0);

// AprendHistogramParams in aprendlit_internal.hpp.
struct HistogramParams {
    float min_log;
    float max_log;
    uint clear; // 1: set every bin to 0 and nothing else
    uint size;  // the luminance texture's side
};
[[vk::push_constant]] ConstantBuffer<HistogramParams> params : register(b0, space4);

groupshared uint group_bins[64];

[shader("compute")]
[numthreads(16, 16, 1)]
void aprend_exposure_histogram_comp(uint3 global_id : SV_DispatchThreadID, uint local_index : SV_GroupIndex) {
    uint index = local_index;
    if (params.clear != 0u) {
        if (index < 64u)
            AprendExposureBins.Store(index * 4u, 0u);
        return;
    }
    if (index < 64u)
        group_bins[index] = 0u;
    GroupMemoryBarrierWithGroupSync();
    uint2 texel = global_id.xy;
    if (texel.x < params.size && texel.y < params.size) {
        float l  = luminance.Load(int3(int2(texel), 0)).r;
        float t  = (l - params.min_log) / (params.max_log - params.min_log);
        uint bin = uint(clamp(int(floor(t * 64.0)), 0, 63));
        InterlockedAdd(group_bins[bin], 1u);
    }
    GroupMemoryBarrierWithGroupSync();
    if (index < 64u && group_bins[index] != 0u)
        AprendExposureBins.InterlockedAdd(index * 4u, group_bins[index]);
}
