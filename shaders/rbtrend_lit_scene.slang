module rbtrend_lit_scene;

// The scene set of Rend's lit shaders (rbtrendlit.h), set 0. Imported by
// every shader that reads it, so they declare it alike; rbtrendlit.cpp's
// rbtrend_lit_scene_block mirrors the block and checks every member's offset
// against the shader's reflection when the layout is made.

public static const int RBTREND_LIT_MAX_LIGHTS = 128;
public static const int RBTREND_LIT_MAX_DIRECTIONAL_LIGHTS = 4;
public static const int RBTREND_LIT_MAX_CASCADES = 4;
public static const int RBTREND_LIT_MAX_SHADOW_VIEWS = 64;
// Lights a tile of the view lists at most; one more is its count.
public static const int RBTREND_LIT_MAX_TILE_LIGHTS = 63;

// A point or spot light, culled per tile.
public struct RbtrendLitLight {
    public float4 position_range;  // xyz: where it is. w: its range
    public float4 radiance_type;   // rgb: color times strength. w: 0 point, 1 spot
    public float4 direction_inner; // xyz: a spot's axis, unit. w: cos of its inner angle
    public float4 outer_shadow;    // x: cos of a spot's outer angle. y: its shadow in shadows[], or -1
};

// Kinds of a directional light's shadow (RbtrendLitDirectional.shadow_views.x).
public static const uint RBTREND_LIT_DIRECTIONAL_NO_SHADOW = 0u;
public static const uint RBTREND_LIT_DIRECTIONAL_MASK      = 1u; // its slot's mask texture
public static const uint RBTREND_LIT_DIRECTIONAL_CASCADES  = 2u; // views in the atlas

// A directional light, in the order the scene's list has them.
public struct RbtrendLitDirectional {
    public float4 direction_versine; // xyz unit, towards the light. w: 1 - cos of its radius, 0 for none
    public float4 radiance_disc;     // rgb: color times strength. w: 1 if the sky draws its disc
    public float4 splits;            // how far along view_direction each cascade reaches
    public float4 shadow_params;     // x depth bias, y normal offset, z softness, w strength
    public uint4 shadow_views;       // x kind, y first view, z view count
};

// A point or spot light's map shadow.
public struct RbtrendLitShadow {
    public float4 params;            // x depth bias, y normal offset, z softness, w strength
    public uint4 views;              // x first view, y view count
};

// One view of a shadow: a cascade, a spot's map or a point light's face.
public struct RbtrendLitShadowView {
    public float4x4 view_projection; // the scene's space to its clip space
    public float4 rect;              // its rectangle of the atlas, in texels: x, y, width, height
};

// Bits of RbtrendLitScene.flags.
public static const uint RBTREND_LIT_SCENE_SHADOW_ATLAS     = 1u;  // shadow_atlas holds the scene's map shadows
public static const uint RBTREND_LIT_SCENE_SHADOW_REVERSED  = 2u;  // the atlas was rendered near at 1
public static const uint RBTREND_LIT_SCENE_ENVIRONMENT      = 4u;  // the environment cubes hold light
public static const uint RBTREND_LIT_SCENE_TILES            = 8u;  // tile_lights holds this view's lists
public static const uint RBTREND_LIT_SCENE_OCCLUSION        = 16u; // ambient_occlusion is this view's
public static const uint RBTREND_LIT_SCENE_BENT_NORMALS     = 64u; // ambient_occlusion's gba is a bent normal, read for diffuse ambient light
public static const uint RBTREND_LIT_SCENE_SCENE_COLOR      = 256u; // scene_color holds the opaque picture behind

// Every matrix is column-major in memory, as the CPU writes it.
public struct RbtrendLitScene {
    public float4x4 view_projection;
    public float4x4 inverse_view_projection;
    public RbtrendLitShadowView shadow_views[RBTREND_LIT_MAX_SHADOW_VIEWS];
    public RbtrendLitDirectional directional[RBTREND_LIT_MAX_DIRECTIONAL_LIGHTS];
    public RbtrendLitShadow shadows[RBTREND_LIT_MAX_SHADOW_VIEWS];
    public float4 eye_position;
    public float4 view_direction;          // xyz unit: the way the view looks
    public float4 ambient_up;              // xyz unit
    public float4 ambient_sky;
    public float4 ambient_ground;
    public float4 environment_rotation[3]; // rows, scene space into the environment's
    public float4 target_size;             // xy: the view's target in texels; zw: their reciprocals
    public float4 shadow_atlas_size;       // xy: the atlas in texels; zw: their reciprocals
    public float environment_intensity;
    public float environment_mips;         // the specular chain's mip count
    public float exposure;
    public uint output_encoding;           // RBTREND_COLOR_ENCODING
    public uint flags;
    public uint light_count;               // point and spot lights
    public uint directional_count;
    public uint tile_size;                 // in texels
    public uint tiles_x;
    public uint tiles_y;
    public float occlusion_strength;
    public float scene_color_mips;         // scene_color's mip count
    public RbtrendLitLight lights[RBTREND_LIT_MAX_LIGHTS];
};
[[vk::binding(0, 0)]] public ConstantBuffer<RbtrendLitScene> u_lit_scene : register(b0, space0);

// The ambient light from [direction]: the sky above, the ground below, mixed
// across the horizon.
public float3 hemisphere(float3 direction) {
    float k = dot(direction, u_lit_scene.ambient_up.xyz) * 0.5 + 0.5;
    return lerp(u_lit_scene.ambient_ground.rgb, u_lit_scene.ambient_sky.rgb, k);
}

// [direction] in the scene's space turned into the environment's.
public float3 to_environment(float3 direction) {
    return float3(dot(u_lit_scene.environment_rotation[0].xyz, direction), dot(u_lit_scene.environment_rotation[1].xyz, direction),
                  dot(u_lit_scene.environment_rotation[2].xyz, direction));
}
