// A source rectangle fitted into a target rectangle
// (APREND_COMMAND_DRAW_TEXTURE, and PRESENT_TEXTURE's STRETCH and
// ASPECT_FIT, whose rectangles are both whole): each texel of the target
// rectangle maps into the part of it the source rectangle fills, and reads
// it there through the sampler (linear or nearest, the command's filter),
// or is the border color outside it. The pass's scissor keeps the draw
// inside the target rectangle.
//
// The part filled, in texels of the target rectangle:
// - mode 0: the source rectangle's own size, from its top-left corner;
// - mode 1: the whole target rectangle;
// - mode 2: the source rectangle scaled to the largest size that fits with
//   its aspect kept, centred.
//
// The place is taken from SV_Position, so it reads the same on every
// backend: the target's and the source's texel rows both start at the top.
// What is written is in the target's format: a float source onto an 8-bit
// target is clamped there, and an sRGB target encodes, as any draw's
// output is.
//
// A separate texture and sampler, as the other built-in passes: one view
// is read through either filter, each a set the view keeps.

[[vk::binding(0, 0)]] Texture2D<float4> source_texture : register(t0, space0);
[[vk::binding(1, 0)]] SamplerState source_sampler : register(s1, space0);

// aprend_present_params.
struct PresentParams {
    float4 border;
    float2 target_offset;
    float2 target_size;
    float2 source_offset;
    float2 source_size;
    uint mode;
    uint unused0;
    uint unused1;
    uint unused2;
};
[[vk::push_constant]] ConstantBuffer<PresentParams> params : register(b0, space4);

[shader("fragment")]
float4 aprend_present_frag(float4 frag_coord : SV_Position) : SV_Target0 {
    uint texture_width, texture_height, texture_levels;
    source_texture.GetDimensions(0, texture_width, texture_height, texture_levels);
    float2 texture_size = float2(float(texture_width), float(texture_height));
    float2 source       = params.source_size;
    float2 target       = params.target_size;
    float2 size   = target;
    float2 offset = float2(0.0);
    if (params.mode == 0u) {
        size = source;
    } else if (params.mode == 2u) {
        float scale = min(target.x / source.x, target.y / source.y);
        size        = source * scale;
        offset      = (target - size) * 0.5;
    }
    float2 place = (frag_coord.xy - params.target_offset - offset) / size;
    if (any(place < float2(0.0)) || any(place >= float2(1.0)))
        return params.border;
    float2 uv = (params.source_offset + place * source) / texture_size;
    return source_texture.SampleLevel(source_sampler, uv, 0.0);
}
