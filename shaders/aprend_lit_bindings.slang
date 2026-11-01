module aprend_lit_bindings;

// The textures and buffers of set 0 beside the scene block, for the shaders
// that read them: the surface and the sky. Every one is bound whether or not
// the scene uses it; a flag in the block says which hold anything.

// The view's tiles, row after row: each a count, then up to
// APREND_LIT_MAX_TILE_LIGHTS indices into u_lit_scene.lights. A uint each,
// raw (byte addressed), as SpudGPU's D3D12 backend views every storage
// buffer; read only here, through tile_lights(), and declared `readonly`
// (NonWritable in the SPIR-V), which Vulkan requires of a fragment shader's
// storage buffer on a device without fragmentStoresAndAtomics.
[[vk::binding(1, 0)]] public readonly RWByteAddressBuffer AprendLitTiles : register(u1, space0);
[[vk::binding(2, 0)]] public Texture2DArray<float4> sun_shadow_map : register(t2, space0);
[[vk::binding(3, 0)]] public Texture2DArray<float4> spot_shadow_map : register(t3, space0);
[[vk::binding(4, 0)]] public SamplerComparisonState shadow_sampler : register(s4, space0);
[[vk::binding(5, 0)]] public TextureCube<float4> environment_specular : register(t5, space0);
[[vk::binding(6, 0)]] public TextureCube<float4> environment_irradiance : register(t6, space0);
[[vk::binding(7, 0)]] public Texture2D<float4> brdf_lut : register(t7, space0);
[[vk::binding(8, 0)]] public SamplerState environment_sampler : register(s8, space0);
[[vk::binding(9, 0)]] public Texture2D<float4> ambient_occlusion : register(t9, space0);
// The same comparison the other way, for maps rendered with depth reversed
// (near at 1): a shadow sampler's comparison is fixed when it is made.
[[vk::binding(10, 0)]] public SamplerComparisonState shadow_sampler_reversed : register(s10, space0);
// Point lights' shadows: six layers a shadow, one a cube face.
[[vk::binding(11, 0)]] public Texture2DArray<float4> point_shadow_map : register(t11, space0);
// The opaque picture behind, mipped, for transmission
// (APREND_COMMAND_COPY_SCENE_COLOR): what the surfaces wrote, exposed.
[[vk::binding(12, 0)]] public Texture2D<float4> scene_color : register(t12, space0);

// Element [i] of the tiles' lists.
public uint tile_lights(uint i) {
    return AprendLitTiles.Load(i * 4u);
}
