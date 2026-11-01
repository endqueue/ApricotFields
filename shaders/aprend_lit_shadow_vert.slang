// Aprend's lit shadow vertex shader (aprendlit.h,
// APREND_LIT_SHADER_SHADOW_VERTEX): the same meshes as aprend_lit_vert.slang,
// through the shadow pass's matrix, for the pass that renders a sun
// cascade's or a spot's depth. Hands the texture coordinate on, for a cut-out
// (APREND_LIT_SHADER_DEPTH_CUTOUT_FRAGMENT).

import aprend_lit_shadow_pass;
import aprend_lit_vertex;

// aprend_lit_vert.slang's outputs up to the texture coordinate, the one
// read here: D3D12 matches stages by register in order, so locations 0 and
// 1 are declared (and written 0) to put location 2 where the cut-out
// fragment shader reads it after either vertex shader.
struct LitShadowVertexOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float3 world : TEXCOORD0;
    [[vk::location(1)]] float3 normal : TEXCOORD1;
    [[vk::location(2)]] float2 uv : TEXCOORD2;
};

[shader("vertex")]
LitShadowVertexOutput aprend_lit_shadow_vert(AprendLitVertex input) {
    LitShadowVertexOutput output;
    output.world    = float3(0.0);
    output.normal   = float3(0.0);
    output.uv       = input.uv;
    output.position = mul(u_lit_shadow_pass.view_projection, mul(instance_transform(input), float4(input.position, 1.0)));
    return output;
}
