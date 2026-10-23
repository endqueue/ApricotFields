#version 450

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
// with texelFetch, so the sampler's filter is not what averages them.
//
// With a nearest filter, each target texel reads the source at its centre's
// place in [0, 1] through the sampler: one source texel.
//
// The place is taken from gl_FragCoord, not from an interpolated coordinate,
// so it reads the same on every backend: the target's and the source's
// texel rows both start at the top.
//
// A separate texture and sampler, not a sampler2D: one sampler serves every
// level's set, and SpudGPU's Metal backend had no combined image sampler
// when this was written (it has since 2026-10-22).
//
// The source is a 2D array view of the one layer being filled, read at
// slice 0, rather than a 2D view: D3D12 has no 2D view of a layer past the
// first, only an array view of it, and a shader that declares a 2D texture
// and is given an array view is undefined there. Declared as an array, the
// view and the shader agree on every backend.

layout(set = 0, binding = 0) uniform texture2DArray source_level;
layout(set = 0, binding = 1) uniform sampler source_sampler;

// aprend_downsample_params: x is 1 for the box filter, 0 for nearest.
layout(push_constant) uniform DownsampleParams {
    uvec4 mode;
} params;

layout(location = 0) out vec4 out_color;

void main() {
    ivec2 source_size = textureSize(sampler2DArray(source_level, source_sampler), 0).xy;
    ivec2 target_size = max(source_size >> 1, ivec2(1));
    if (params.mode.x == 0u) {
        out_color = textureLod(sampler2DArray(source_level, source_sampler), vec3(gl_FragCoord.xy / vec2(target_size), 0.0), 0.0);
        return;
    }

    // The footprint of this target texel, in source texels.
    vec2 scale  = vec2(source_size) / vec2(target_size);
    vec2 low    = floor(gl_FragCoord.xy) * scale;
    vec2 high   = low + scale;
    ivec2 first = ivec2(floor(low));
    ivec2 last  = min(ivec2(ceil(high)) - 1, source_size - 1);

    vec4 sum     = vec4(0.0);
    float weight = 0.0;
    for (int y = first.y; y <= last.y; ++y) {
        float wy = min(high.y, float(y + 1)) - max(low.y, float(y));
        for (int x = first.x; x <= last.x; ++x) {
            float w = (min(high.x, float(x + 1)) - max(low.x, float(x))) * wy;
            sum += texelFetch(sampler2DArray(source_level, source_sampler), ivec3(x, y, 0), 0) * w;
            weight += w;
        }
    }
    out_color = sum / weight;
}
