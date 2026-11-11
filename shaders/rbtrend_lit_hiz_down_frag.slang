// RBTREND_COMMAND_LIT_SSR, the passes after the first: each level of the
// Hi-Z pyramid the least nearness of the texels of the level above it
// covers. A level is half the one above rounded down, so where the level
// above is odd its last texel's column (or row) has no texel of its own
// below and is taken into the last one: no texel of the level above is
// left out, and a level's value is never farther than any it covers.

[[vk::binding(0, 0)]] Texture2D<float4> above : register(t0, space0);
[[vk::binding(1, 0)]] SamplerState above_sampler : register(s1, space0);

struct DownParams {
    float2 target_size;
};
[[vk::push_constant]] ConstantBuffer<DownParams> params : register(b0, space4);

[shader("fragment")]
float4 rbtrend_lit_hiz_down_frag(float4 frag_coord : SV_Position) : SV_Target0 {
    uint width, height, levels;
    above.GetDimensions(0, width, height, levels);
    int2 at   = int2(frag_coord.xy);
    int2 last = int2(params.target_size) - 1;
    // Two texels each way, and a third where this is the last of an odd
    // level above.
    int2 count = int2(2, 2);
    if (at.x == last.x && (width & 1u) != 0u)
        count.x = 3;
    if (at.y == last.y && (height & 1u) != 0u)
        count.y = 3;
    float nearest = 1.0;
    for (int y = 0; y < count.y; ++y)
        for (int x = 0; x < count.x; ++x)
            nearest = min(nearest, above.Load(int3(at * 2 + int2(x, y), 0)).r);
    return float4(nearest, 0.0, 0.0, 0.0);
}
