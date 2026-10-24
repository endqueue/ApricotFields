#version 450

// APREND_COMMAND_AUTO_EXPOSURE's second pass for an exposure measured by
// histogram (aprend_exposure_histogram.comp): the average log luminance of
// the texels between the darkest `low` share and the brightest above the
// `high` share of the picture, a bin split where a share's edge falls in it,
// each bin standing for its middle; then, as aprend_exposure_adapt.frag, the
// exposure that brings that to the key, eased towards from the last one,
// into a 1x1 float texture the tone map reads. Unreal's eye adaptation
// measures between its low and high percent the same way.

layout(set = 0, binding = 0) uniform texture2D luminance;
layout(set = 0, binding = 1) uniform sampler luminance_sampler;
layout(set = 0, binding = 2, std430) readonly buffer AprendExposureBins {
    uint bins[64];
};
layout(set = 1, binding = 0) uniform texture2D previous;
layout(set = 1, binding = 1) uniform sampler previous_sampler;

// AprendHistogramAdaptParams in aprendlit_internal.hpp.
layout(push_constant) uniform AdaptParams {
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
} params;

layout(location = 0) out vec4 out_exposure;

void main() {
    float total = 0.0;
    for (int i = 0; i < 64; ++i)
        total += float(bins[i]);
    float from  = params.low * total;
    float to    = params.high * total;
    float width = (params.max_log - params.min_log) / 64.0;
    float below = 0.0;
    float sum   = 0.0;
    float count = 0.0;
    for (int i = 0; i < 64; ++i) {
        float n    = float(bins[i]);
        float kept = max(min(below + n, to) - max(below, from), 0.0);
        sum += kept * (params.min_log + (float(i) + 0.5) * width);
        count += kept;
        below += n;
    }
    float measured = count > 0.0 ? sum / count : params.min_log;
    float target   = log2(params.key) - clamp(measured, params.min_log, params.max_log);
    float ev       = target;
    if (params.first == 0u) {
        float last = log2(max(texelFetch(sampler2D(previous, previous_sampler), ivec2(0), 0).r, 1e-12));
        float rate = target > last ? params.up_rate : params.down_rate;
        ev         = last + (target - last) * (1.0 - exp(-params.delta_time * rate));
    }
    out_exposure = vec4(exp2(ev), 0.0, 0.0, 1.0);
}
