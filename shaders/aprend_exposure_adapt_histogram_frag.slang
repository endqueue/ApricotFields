// APREND_COMMAND_AUTO_EXPOSURE's second pass for an exposure measured by
// histogram (aprend_exposure_histogram_comp.slang): the average log
// luminance of the texels between the darkest `low` share and the brightest
// above the `high` share of the picture, a bin split where a share's edge
// falls in it, each bin standing for its middle; then, as
// aprend_exposure_adapt_frag.slang, the exposure that brings that to the
// key, eased towards from the last one, into a 1x1 float texture the tone
// map reads. Unreal's eye adaptation measures between its low and high
// percent the same way.

// Set 0 is the histogram's (aprend_exposure_histogram_comp.slang), declared
// as its layout holds it: the luminance texture, which only that shader
// reads, and the bins.
[[vk::binding(0, 0)]] Texture2D<float4> luminance : register(t0, space0);
// 64 bins, a uint each. Raw (byte addressed), as SpudGPU's D3D12 backend
// views every storage buffer; read only here, and declared `readonly`
// (NonWritable), as Vulkan requires of a fragment shader's storage buffer
// on a device without fragmentStoresAndAtomics.
[[vk::binding(2, 0)]] readonly RWByteAddressBuffer AprendExposureBins : register(u2, space0);
[[vk::binding(0, 1)]] Texture2D<float4> previous : register(t0, space1);
[[vk::binding(1, 1)]] SamplerState previous_sampler : register(s1, space1);

// AprendHistogramAdaptParams in aprendlit_internal.hpp.
struct AdaptParams {
    float key;
    float min_log;
    float max_log;
    float up_rate;
    float down_rate;
    float delta_time;
    float low;   // the darkest share of the texels left out, 0 to 1
    float high;  // the share up to which they are measured, above low, to 1
    uint first;  // 1: no last exposure, take the target at once
    uint unused0;
    uint unused1;
    uint unused2;
};
[[vk::push_constant]] ConstantBuffer<AdaptParams> params : register(b0, space4);

uint bins(int i) {
    return AprendExposureBins.Load(uint(i) * 4u);
}

[shader("fragment")]
float4 aprend_exposure_adapt_histogram_frag() : SV_Target0 {
    float total = 0.0;
    for (int i = 0; i < 64; ++i)
        total += float(bins(i));
    float from  = params.low * total;
    float to    = params.high * total;
    float width = (params.max_log - params.min_log) / 64.0;
    float below = 0.0;
    float sum   = 0.0;
    float count = 0.0;
    for (int i = 0; i < 64; ++i) {
        float n    = float(bins(i));
        float kept = max(min(below + n, to) - max(below, from), 0.0);
        sum += kept * (params.min_log + (float(i) + 0.5) * width);
        count += kept;
        below += n;
    }
    float measured = count > 0.0 ? sum / count : params.min_log;
    float target   = log2(params.key) - clamp(measured, params.min_log, params.max_log);
    float ev       = target;
    if (params.first == 0u) {
        float last = log2(max(previous.Load(int3(0, 0, 0)).r, 1e-12));
        float rate = target > last ? params.up_rate : params.down_rate;
        ev         = last + (target - last) * (1.0 - exp(-params.delta_time * rate));
    }
    return float4(exp2(ev), 0.0, 0.0, 1.0);
}
