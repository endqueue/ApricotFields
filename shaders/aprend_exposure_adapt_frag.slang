// APREND_COMMAND_AUTO_EXPOSURE, second pass: the exposure that brings the
// log-average luminance to the key (middle grey), its luminance clamped to
// the range given, eased towards from the last one by the time since it
// (exponentially, in stops, faster up than down as eyes adapt), into a 1x1
// float texture the tone map reads.

[[vk::binding(0, 0)]] Texture2D<float4> luminance : register(t0, space0);
[[vk::binding(1, 0)]] SamplerState luminance_sampler : register(s1, space0);
[[vk::binding(0, 1)]] Texture2D<float4> previous : register(t0, space1);
[[vk::binding(1, 1)]] SamplerState previous_sampler : register(s1, space1);

// AprendAdaptParams in aprendlit_internal.hpp.
struct AdaptParams {
    float key;
    float min_log;     // log2 of the least luminance exposed for
    float max_log;     // and of the most
    float up_rate;     // stops per second, brightening
    float down_rate;   // and darkening
    float delta_time;  // seconds since the last
    float last_mip;
    uint first;        // 1: no last exposure, take the target at once
};
[[vk::push_constant]] ConstantBuffer<AdaptParams> params : register(b0, space4);

[shader("fragment")]
float4 aprend_exposure_adapt_frag() : SV_Target0 {
    float log_average = luminance.SampleLevel(luminance_sampler, float2(0.5), params.last_mip).r;
    float target      = log2(params.key) - clamp(log_average, params.min_log, params.max_log);
    float ev          = target;
    if (params.first == 0u) {
        float last = log2(max(previous.Load(int3(0, 0, 0)).r, 1e-12));
        float rate = target > last ? params.up_rate : params.down_rate;
        ev         = last + (target - last) * (1.0 - exp(-params.delta_time * rate));
    }
    return float4(exp2(ev), 0.0, 0.0, 1.0);
}
