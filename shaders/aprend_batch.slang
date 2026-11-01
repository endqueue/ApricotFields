module aprend_batch;

// The batcher's push constants, set 0 and kinds (aprendbatch.h), shared by
// its vertex shader and its four fragment shaders. aprendbatch.cpp's
// aprend_batch_push and aprend_batch_record mirror them.

public struct AprendBatchPush {
    public float4x4 view_projection; // world space only
    public float4 viewport;          // xy: the target's size in pixels; zw: their reciprocals
    public float4 params;            // x: 1 screen space, 2 world; y: the atlas's distance range
};
[[vk::push_constant]] public ConstantBuffer<AprendBatchPush> u_batch : register(b0, space4);

public static const uint BATCH_QUAD   = 0u;
public static const uint BATCH_CIRCLE = 1u;
public static const uint BATCH_LINE   = 2u;
public static const uint BATCH_GLYPH  = 3u;

// A line's flags (record r3.w), above its kind's two bits.
public static const uint LINE_HAS_PREV  = 4u;
public static const uint LINE_HAS_NEXT  = 8u;
public static const uint LINE_CAP_SHIFT = 4u;  // 2 bits: 1 butt, 2 square, 3 round
public static const uint LINE_JOIN_SHIFT = 6u; // 2 bits: 1 mitre, 2 bevel, 3 round
public static const uint QUAD_TEXTURED  = 4u;

// Where [p] lands on the screen: its clip position.
public float4 batch_clip(float3 p) {
    if (u_batch.params.x > 1.5)
        return mul(u_batch.view_projection, float4(p, 1.0));
    // Pixels, y down, to SpudGPU's clip space (+Y up the target): y negated.
    float2 ndc = p.xy * u_batch.viewport.zw * 2.0 - 1.0;
    return float4(ndc.x, -ndc.y, 0.0, 1.0);
}

// A clip position's pixel, y down: clip space's y negated.
public float2 batch_pixel(float4 clip) {
    float2 ndc = clip.xy / clip.w;
    return (float2(ndc.x, -ndc.y) * 0.5 + 0.5) * u_batch.viewport.xy;
}

// Premultiplied, as the pipelines blend (one, one minus source alpha).
public float4 batch_premultiply(float4 c, float coverage) {
    float a = clamp(c.a, 0.0, 1.0) * clamp(coverage, 0.0, 1.0);
    return float4(max(c.rgb, float3(0.0)) * a, a);
}
