import aprend_lit_scene;
import aprend_rt_trace;
import aprend_rt_shadow_mask;

// APREND_COMMAND_LIT_SHADOW_MASK into an SPUDGPU_FORMAT_R8_UNORM target
// (aprend_rt_shadow_mask.slang has the body). Set 0 the scene's block, set 1
// the TLAS (aprend_rt_trace.slang), set 2 the depth and the target.

[[vk::binding(0, 2)]] Texture2D<float4> mask_depth : register(t0, space2);
[[vk::binding(1, 2)]] [[vk::image_format("r8")]] RWTexture2D<float> mask_target : register(u1, space2);
[[vk::push_constant]] ConstantBuffer<MaskParams> params : register(b0, space4);

[shader("compute")]
[numthreads(8, 8, 1)]
void aprend_rt_shadow_mask_r8_comp(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID, uint3 id : SV_DispatchThreadID) {
    MaskParams p = params;
    float value = shadow_mask_texel(mask_depth, group.xy, thread.xy, p);
    if (id.x < p.size.x && id.y < p.size.y)
        mask_target[id.xy] = value;
}
