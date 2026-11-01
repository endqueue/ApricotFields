// An outline drawn round a mask by a jump flood (Rong and Tan, "Jump
// Flooding in GPU with Applications to Voronoi Diagram and Distance
// Transform", 2006): each texel ends holding the place of the nearest texel
// inside the mask, from which its distance to the mask is one subtraction.
// Three modes, one pass each:
//
// - SEED: from the mask (red above 0.5 is inside), each inside texel's own
//   centre, every other texel "none" (far off the target).
// - JUMP: from the last pass's places, the nearest of this texel's own and
//   those of the eight texels `jump` texels off; a texel past the edge of
//   the target is not read. Jumps of halving length, then one more of 1,
//   reach every texel the outline covers.
// - COMPOSITE: from the last places, the outline's coverage of each texel
//   outside the mask: its width plus one, less the distance between its
//   centre and the nearest inside centre, held to [0, 1]. So a texel beside
//   a straight edge is covered by min(width, 1), and an outline of width w
//   covers w whole texels out from the edge. Written premultiplied, for a
//   blend over the target. A texel inside the mask is not touched.
//
// Places are texel centres in texels, as SV_Position gives them; every read
// is a Load, so the places' texture is never filtered.

[[vk::binding(0, 0)]] Texture2D<float4> source_level : register(t0, space0);
[[vk::binding(1, 0)]] SamplerState source_sampler : register(s1, space0);

struct OutlineParams {
    // Linear RGBA, alpha not premultiplied.
    float4 color;
    float2 target_size;
    float width;
    float jump;
    // 0 SEED, 1 JUMP, 2 COMPOSITE.
    uint mode;
    // The block is 48 bytes, as the pipeline's range is.
    uint unused0;
    uint unused1;
    uint unused2;
};
[[vk::push_constant]] ConstantBuffer<OutlineParams> params : register(b0, space4);

static const float NONE = -1.0e9;

float2 place_at(int2 texel) {
    return source_level.Load(int3(texel, 0)).xy;
}

[shader("fragment")]
float4 aprend_outline_frag(float4 frag_coord : SV_Position) : SV_Target0 {
    int2 texel  = int2(frag_coord.xy);
    float2 here = frag_coord.xy;
    if (params.mode == 0u) {
        bool inside = source_level.Load(int3(texel, 0)).r > 0.5;
        return float4(inside ? here : float2(NONE), 0.0, 0.0);
    }
    if (params.mode == 1u) {
        int2 size     = int2(params.target_size);
        int jump      = int(params.jump);
        float2 best   = place_at(texel);
        float best_d2 = dot(best - here, best - here);
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                int2 at = texel + int2(x, y) * jump;
                if ((x == 0 && y == 0) || at.x < 0 || at.y < 0 || at.x >= size.x || at.y >= size.y)
                    continue;
                float2 place = place_at(at);
                float d2     = dot(place - here, place - here);
                if (d2 < best_d2) {
                    best    = place;
                    best_d2 = d2;
                }
            }
        }
        return float4(best, 0.0, 0.0);
    }
    float2 nearest = place_at(texel);
    float d        = length(nearest - here);
    // The nearest inside centre is this one's own: inside the mask.
    if (d < 0.5)
        discard;
    float coverage = clamp(params.width + 1.0 - d, 0.0, 1.0);
    if (coverage <= 0.0)
        discard;
    float alpha = params.color.a * coverage;
    return float4(params.color.rgb * alpha, alpha);
}
