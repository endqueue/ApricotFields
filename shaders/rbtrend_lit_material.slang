module rbtrend_lit_material;

// The material set of Rend's lit shaders (rbtrendlit.h), set 1, for the
// shaders that read it: the surface, and the cut-out depth pass.
// rbtrendlit.cpp's rbtrend_lit_material_block mirrors the block.

// Bits of RbtrendLitMaterial.maps: which maps hold a texture of the caller's.
// A map left out is not read, and its binding holds a placeholder.
public static const uint MAP_BASE_COLOR = 1u;
public static const uint MAP_NORMAL     = 2u;
public static const uint MAP_ORM        = 4u;
public static const uint MAP_EMISSION   = 8u;

public struct RbtrendLitMaterial {
    public float4 base_color;      // rgb linear, a: opacity
    public float4 emission;        // rgb linear
    public float4 sheen;           // rgb: sheen color, linear. a: sheen roughness
    public float roughness;
    public float metalness;
    public float specular;         // a dielectric's reflectance: 0.5 is F0 0.04
    public float normal_scale;
    public float occlusion_strength;
    public float alpha_cutoff;     // 0: blended by opacity; above: a cut-out
    public float clearcoat;        // 0 to 1: how much of a clear coat there is
    public float clearcoat_roughness;
    public uint maps;
    public float4 uv_transform[2]; // rows of a 2x3 matrix, before every map
    // Each map's own, after uv_transform: rows 2m and 2m + 1 for map m
    // (base color, normal, ORM, emission, as the MAP_ bits number them).
    public float4 map_uv_transform[8];
    // glTF's KHR_materials_transmission and _volume: x how much light
    // passes through, 0 to 1; y the index of refraction; z the thickness,
    // in the scene's units; w the distance light travels before it is
    // tinted to the attenuation color, 0 for never.
    public float4 transmission;
    public float4 attenuation;     // rgb: the color light is tinted to over w's distance
};
[[vk::binding(0, 1)]] public ConstantBuffer<RbtrendLitMaterial> u_lit_material : register(b0, space1);
[[vk::binding(1, 1)]] public Texture2D<float4> base_color_map : register(t1, space1);
[[vk::binding(2, 1)]] public Texture2D<float4> normal_map : register(t2, space1);
[[vk::binding(3, 1)]] public Texture2D<float4> orm_map : register(t3, space1);
[[vk::binding(4, 1)]] public Texture2D<float4> emission_map : register(t4, space1);
[[vk::binding(5, 1)]] public SamplerState material_sampler : register(s5, space1);

// Map indices of map_uv_transform, in MAP_ bit order.
public static const uint MAP_INDEX_BASE_COLOR = 0u;
public static const uint MAP_INDEX_NORMAL     = 1u;
public static const uint MAP_INDEX_ORM        = 2u;
public static const uint MAP_INDEX_EMISSION   = 3u;

public float2 material_uv(float2 uv) {
    return float2(dot(u_lit_material.uv_transform[0].xyz, float3(uv, 1.0)), dot(u_lit_material.uv_transform[1].xyz, float3(uv, 1.0)));
}

// Where map [map] is read for [uv], after material_uv().
public float2 map_uv(float2 uv, uint map) {
    return float2(dot(u_lit_material.map_uv_transform[2u * map].xyz, float3(uv, 1.0)),
                  dot(u_lit_material.map_uv_transform[2u * map + 1u].xyz, float3(uv, 1.0)));
}
