#version 450

// APREND_COMMAND_AUTO_EXPOSURE, second pass: the exposure that brings the
// log-average luminance to the key (middle grey), its luminance clamped to
// the range given, eased towards from the last one by the time since it
// (exponentially, in stops, faster up than down as eyes adapt), into a 1x1
// float texture the tone map reads.

layout(set = 0, binding = 0) uniform texture2D luminance;
layout(set = 0, binding = 1) uniform sampler luminance_sampler;
layout(set = 1, binding = 0) uniform texture2D previous;
layout(set = 1, binding = 1) uniform sampler previous_sampler;

// AprendAdaptParams in aprendlit_internal.hpp.
layout(push_constant) uniform AdaptParams {
    float key;
    float min_log;     // log2 of the least luminance exposed for
    float max_log;     // and of the most
    float up_rate;     // stops per second, brightening
    float down_rate;   // and darkening
    float delta_time;  // seconds since the last
    float last_mip;
    uint first;        // 1: no last exposure, take the target at once
} params;

layout(location = 0) out vec4 out_exposure;

void main() {
    float log_average = textureLod(sampler2D(luminance, luminance_sampler), vec2(0.5), params.last_mip).r;
    float target      = log2(params.key) - clamp(log_average, params.min_log, params.max_log);
    float ev          = target;
    if (params.first == 0u) {
        float last = log2(max(texelFetch(sampler2D(previous, previous_sampler), ivec2(0), 0).r, 1e-12));
        float rate = target > last ? params.up_rate : params.down_rate;
        ev         = last + (target - last) * (1.0 - exp(-params.delta_time * rate));
    }
    out_exposure = vec4(exp2(ev), 0.0, 0.0, 1.0);
}
