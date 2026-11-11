// Rend's cut-out depth fragment shader (rbtrendlit.h,
// RBTREND_LIT_SHADER_DEPTH_CUTOUT_FRAGMENT): depth only, as
// rbtrend_lit_depth_frag.slang, but a fragment of a cut-out material whose
// opacity is below its cutoff is discarded, so the holes in leaves or a
// perforated screen let light through. Reads the lit material at set 1, the
// texture coordinate at location 2 and the color at location 4, whose alpha
// multiplies the opacity as the surface's does.

import rbtrend_lit_material;

// The lit vertex shaders' outputs up to location 4, in their order: D3D12
// matches stages by register in order. The texture coordinate and the
// color are read.
struct LitDepthCutoutInput {
    float4 position : SV_Position;
    [[vk::location(0)]] float3 world : TEXCOORD0;
    [[vk::location(1)]] float3 normal : TEXCOORD1;
    [[vk::location(2)]] float2 uv : TEXCOORD2;
    [[vk::location(3)]] float4 tangent : TEXCOORD3;
    [[vk::location(4)]] float4 color : TEXCOORD4;
};

[shader("fragment")]
void rbtrend_lit_depth_cutout_frag(LitDepthCutoutInput input) {
    if (u_lit_material.alpha_cutoff <= 0.0)
        return;
    float opacity = u_lit_material.base_color.a * input.color.a;
    if ((u_lit_material.maps & MAP_BASE_COLOR) != 0u)
        opacity *= base_color_map.Sample(material_sampler, map_uv(material_uv(input.uv), MAP_INDEX_BASE_COLOR)).a;
    if (opacity < u_lit_material.alpha_cutoff)
        discard;
}
