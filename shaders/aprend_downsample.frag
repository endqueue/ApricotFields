#version 450

// One mip level from the level above it: each texel of the target reads the
// source at the same place in [0, 1], through a view of that one level. With
// a linear sampler, a target texel's centre falls on the corner shared by the
// four source texels it covers, so an even-sized level is the average of each
// 2x2 block. Where a side of the source is odd, the last texel's weight is
// spread unevenly: a filter of two taps a side, not three.
//
// The place is taken from gl_FragCoord, not from an interpolated coordinate,
// so it reads the same on every backend: the target's and the source's
// texel rows both start at the top.
//
// A separate texture and sampler, not a sampler2D: SpudGPU's Metal backend
// has no combined image sampler.
//
// The source is a 2D array view of the one layer being filled, read at
// slice 0, rather than a 2D view: D3D12 has no 2D view of a layer past the
// first, only an array view of it, and a shader that declares a 2D texture
// and is given an array view is undefined there. Declared as an array, the
// view and the shader agree on every backend.

layout(set = 0, binding = 0) uniform texture2DArray source_level;
layout(set = 0, binding = 1) uniform sampler source_sampler;

layout(location = 0) out vec4 out_color;

void main() {
    ivec2 source_size = textureSize(sampler2DArray(source_level, source_sampler), 0).xy;
    vec2 target_size  = vec2(max(source_size >> 1, ivec2(1)));
    out_color         = textureLod(sampler2DArray(source_level, source_sampler), vec3(gl_FragCoord.xy / target_size, 0.0), 0.0);
}
