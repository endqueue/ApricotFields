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
// The scene's shadow atlas (aprend_lit_scene_set_shadow_atlas()): every
// view of every map shadow, each in its rectangle.
[[vk::binding(2, 0)]] public Texture2D<float4> shadow_atlas : register(t2, space0);
[[vk::binding(3, 0)]] public SamplerComparisonState shadow_sampler : register(s3, space0);
// The same comparison the other way, for an atlas rendered with depth
// reversed (near at 1): a shadow sampler's comparison is fixed when it is
// made.
[[vk::binding(4, 0)]] public SamplerComparisonState shadow_sampler_reversed : register(s4, space0);
[[vk::binding(5, 0)]] public TextureCube<float4> environment_specular : register(t5, space0);
[[vk::binding(6, 0)]] public TextureCube<float4> environment_irradiance : register(t6, space0);
[[vk::binding(7, 0)]] public Texture2D<float4> brdf_lut : register(t7, space0);
[[vk::binding(8, 0)]] public SamplerState environment_sampler : register(s8, space0);
[[vk::binding(9, 0)]] public Texture2D<float4> ambient_occlusion : register(t9, space0);
// The opaque picture behind, mipped, for transmission
// (APREND_COMMAND_COPY_SCENE_COLOR): what the surfaces wrote, exposed.
[[vk::binding(10, 0)]] public Texture2D<float4> scene_color : register(t10, space0);
// Each directional light's traced shadow, by its place among the scene's
// directional lights, when its shadow is a mask
// (aprend_lit_scene_set_light_shadow(), APREND_COMMAND_LIT_SHADOW_MASK): red
// 1 lit, 0 in shadow, at the view's target size. A binding each, not an
// array of them, so no device needs arrays of textures.
[[vk::binding(11, 0)]] public Texture2D<float4> directional_mask_0 : register(t11, space0);
[[vk::binding(12, 0)]] public Texture2D<float4> directional_mask_1 : register(t12, space0);
[[vk::binding(13, 0)]] public Texture2D<float4> directional_mask_2 : register(t13, space0);
[[vk::binding(14, 0)]] public Texture2D<float4> directional_mask_3 : register(t14, space0);

// Element [i] of the tiles' lists.
public uint tile_lights(uint i) {
    return AprendLitTiles.Load(i * 4u);
}

// Directional light [slot]'s mask at [texel].
public float directional_mask(uint slot, int2 texel) {
    switch (slot) {
    case 0u:
        return directional_mask_0.Load(int3(texel, 0)).r;
    case 1u:
        return directional_mask_1.Load(int3(texel, 0)).r;
    case 2u:
        return directional_mask_2.Load(int3(texel, 0)).r;
    default:
        return directional_mask_3.Load(int3(texel, 0)).r;
    }
}
