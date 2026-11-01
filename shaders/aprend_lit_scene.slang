module aprend_lit_scene;

// The scene set of Aprend's lit shaders (aprendlit.h), set 0. Imported by
// every shader that reads it, so they declare it alike; aprendlit.cpp's
// aprend_lit_scene_block mirrors the block and checks every member's offset
// against the shader's reflection when the layout is made.

public static const int APREND_LIT_MAX_LIGHTS = 128;
public static const int APREND_LIT_MAX_CASCADES = 4;
public static const int APREND_LIT_MAX_SPOT_SHADOWS = 4;
public static const int APREND_LIT_MAX_POINT_SHADOWS = 4;
// Lights a tile of the view lists at most; one more is its count.
public static const int APREND_LIT_MAX_TILE_LIGHTS = 63;

public struct AprendLitLight {
    public float4 position_range;  // xyz: where it is. w: its range
    public float4 radiance_type;   // rgb: color times strength. w: 0 point, 1 spot
    public float4 direction_inner; // xyz: a spot's axis, unit. w: cos of its inner angle
    public float4 outer_shadow;    // x: cos of a spot's outer angle. y: its spot shadow, or -1. z: a point light's point shadow, or -1
};

// Bits of AprendLitScene.flags.
public static const uint APREND_LIT_SCENE_SUN_SHADOW     = 1u;  // sun_shadow_map is the sun's cascades
public static const uint APREND_LIT_SCENE_SUN_REVERSED   = 2u;  // rendered near at 1
public static const uint APREND_LIT_SCENE_ENVIRONMENT    = 4u;  // the environment cubes hold light
public static const uint APREND_LIT_SCENE_TILES          = 8u;  // tile_lights holds this view's lists
public static const uint APREND_LIT_SCENE_OCCLUSION      = 16u; // ambient_occlusion is this view's
public static const uint APREND_LIT_SCENE_SPOT_REVERSED  = 32u; // spot shadows rendered near at 1
public static const uint APREND_LIT_SCENE_BENT_NORMALS   = 64u; // ambient_occlusion's gba is a bent normal, read for diffuse ambient light
public static const uint APREND_LIT_SCENE_POINT_REVERSED = 128u; // point shadows rendered near at 1
public static const uint APREND_LIT_SCENE_SCENE_COLOR    = 256u; // scene_color holds the opaque picture behind
public static const uint APREND_LIT_SCENE_SUN_MASK       = 512u; // the sun's shadow is sun_mask, at sun_shadow_params.w's strength

// Every matrix is column-major in memory, as the CPU writes it.
public struct AprendLitScene {
    public float4x4 view_projection;
    public float4x4 inverse_view_projection;
    public float4x4 sun_shadow_view_projection[APREND_LIT_MAX_CASCADES];
    public float4x4 spot_shadow_view_projection[APREND_LIT_MAX_SPOT_SHADOWS];
    // Six a point shadow, its faces +X, -X, +Y, -Y, +Z, -Z: layer 6i + f of point_shadow_map.
    public float4x4 point_shadow_view_projection[APREND_LIT_MAX_POINT_SHADOWS * 6];
    public float4 eye_position;
    public float4 view_direction;          // xyz unit: the way the view looks
    public float4 sun_direction;           // xyz unit, towards the sun
    public float4 sun_radiance;            // rgb: color times strength. w: 1 - cos of its disc's angle, 0 for none
    public float4 ambient_up;              // xyz unit
    public float4 ambient_sky;
    public float4 ambient_ground;
    public float4 environment_rotation[3]; // rows, scene space into the environment's
    public float4 sun_shadow_splits;       // how far along view_direction each cascade reaches
    public float4 sun_shadow_params;       // x depth bias, y normal offset, z softness, w strength
    public float4 spot_shadow_params[APREND_LIT_MAX_SPOT_SHADOWS]; // the same, for each
    public float4 point_shadow_params[APREND_LIT_MAX_POINT_SHADOWS]; // and for each point shadow
    public float4 target_size;             // xy: the view's target in texels; zw: their reciprocals
    public float environment_intensity;
    public float environment_mips;         // the specular chain's mip count
    public float exposure;
    public uint output_encoding;           // APREND_COLOR_ENCODING
    public uint flags;
    public uint light_count;
    public uint sun_cascade_count;
    public uint tile_size;                 // in texels
    public uint tiles_x;
    public uint tiles_y;
    public float occlusion_strength;
    public float scene_color_mips;         // scene_color's mip count
    public AprendLitLight lights[APREND_LIT_MAX_LIGHTS];
};
[[vk::binding(0, 0)]] public ConstantBuffer<AprendLitScene> u_lit_scene : register(b0, space0);
