module aprend_rt_shadow_mask;

// APREND_COMMAND_LIT_SHADOW_MASK's body (aprendcommands.h): a directional
// light's shadow traced over a depth pre-pass. The two entry points
// (aprend_rt_shadow_mask_r8_comp, _r16f_comp) differ only in the target's
// format, which a storage image declares.
//
// Each texel is brought back into the scene's space through the inverse of
// the scene's view_projection, moved _normal_offset along the normal the
// depth's own slopes give (no normal target is read: card T9's
// approximation until A6 and A30's target exists), and _ray_count rays are
// traced towards the light over its disc (its directional record's
// direction_versine.w, 1 - the cosine of its angular radius), in directions of Roberts' R2 sequence
// turned per texel by interleaved gradient noise, so neighbouring texels
// sample the disc apart and a filter averages them. The fraction that
// reaches the light is filtered over _filter_radius texels each way, its
// weights Gaussian in distance and falling with the difference in normal
// and in depth, so an edge between surfaces is kept. Nothing is kept from
// one frame to the next.

import aprend_lit_scene;
import aprend_rt_trace;

// aprend_rt_mask_params.
public struct MaskParams {
    public uint2 size;
    public uint ray_count;
    public uint filter_radius;
    public uint cull_mask;
    public float normal_offset;
    public float max_distance;
    public uint light;            // its place among the scene's directional lights
};

// The entry points' group: 8 x 8 texels.
public static const uint MASK_GROUP = 8u;
// The largest filter radius (APREND_RT_MAX_FILTER_RADIUS), and the tile of
// texels a group traces for its filter: the group and that apron.
static const uint MAX_RADIUS = 8u;
static const uint MAX_TILE = MASK_GROUP + 2u * MAX_RADIUS;

static groupshared float s_visibility[MAX_TILE * MAX_TILE];
// Each tile texel's normal (xyz) and its depth along the view (w).
static groupshared float4 s_surface[MAX_TILE * MAX_TILE];

// The scene-space point of depth texel [p].
float3 mask_position(Texture2D<float4> depth, int2 p, float2 size) {
    float z = depth.Load(int3(p, 0)).r;
    return rt_unproject(u_lit_scene.inverse_view_projection, rt_clip(float2(p), float2(0.5, 0.5), size, z));
}

// The step from [c] to the nearer of its two neighbours [a] and [b] along
// one axis (a texel on the target's edge has one).
float3 nearer_step(float3 c, float3 a, float3 b, bool has_a, bool has_b) {
    float3 to_a = c - a;
    float3 to_b = b - c;
    if (!has_a)
        return to_b;
    if (!has_b)
        return to_a;
    return dot(to_a, to_a) < dot(to_b, to_b) ? to_a : to_b;
}

// The surface's normal at texel [p], at [c], from the depth's own slopes:
// on each axis the step to the nearer neighbour, so a depth edge doesn't
// tilt it. Turned to face the eye.
float3 mask_normal(Texture2D<float4> depth, int2 p, int2 size, float3 c) {
    float2 fsize = float2(size);
    bool l       = p.x > 0;
    bool r       = p.x + 1 < size.x;
    bool u       = p.y > 0;
    bool d       = p.y + 1 < size.y;
    float3 pl    = mask_position(depth, int2(max(p.x - 1, 0), p.y), fsize);
    float3 pr    = mask_position(depth, int2(min(p.x + 1, size.x - 1), p.y), fsize);
    float3 pu    = mask_position(depth, int2(p.x, max(p.y - 1, 0)), fsize);
    float3 pd    = mask_position(depth, int2(p.x, min(p.y + 1, size.y - 1)), fsize);
    float3 dx    = nearer_step(c, pl, pr, l, r);
    float3 dy    = nearer_step(c, pu, pd, u, d);
    float3 n     = cross(dx, dy);
    float3 to_eye = u_lit_scene.eye_position.xyz - c;
    if (dot(n, n) <= 0.0)
        n = to_eye;
    n = normalize(n);
    return dot(n, to_eye) < 0.0 ? -n : n;
}

// How much of the light texel [p] sees, and its normal and view depth
// through [surface].
float mask_visibility(Texture2D<float4> depth, int2 p, MaskParams params, out float4 surface) {
    int2 size      = int2(params.size);
    float3 c       = mask_position(depth, p, float2(size));
    float3 n       = mask_normal(depth, p, size, c);
    float3 origin  = c + n * params.normal_offset;
    AprendLitDirectional d = u_lit_scene.directional[min(params.light, uint(APREND_LIT_MAX_DIRECTIONAL_LIGHTS - 1))];
    float3 light   = normalize(d.direction_versine.xyz);
    float cos_max  = 1.0 - d.direction_versine.w;
    float2 turn    = float2(rt_interleaved_gradient(float2(p)), rt_interleaved_gradient(float2(p) + float2(5.588238, 5.588238)));
    uint reached   = 0u;
    for (uint i = 0u; i < params.ray_count; ++i) {
        float3 direction = cos_max >= 1.0 ? light : rt_cone(light, cos_max, frac(rt_r2(i) + turn));
        if (rt_visible(origin, direction, params.max_distance, params.cull_mask))
            ++reached;
    }
    surface = float4(n, dot(c - u_lit_scene.eye_position.xyz, u_lit_scene.view_direction.xyz));
    return float(reached) / float(params.ray_count);
}

// The mask at the texel of thread [thread] of group [group]: traced, then
// filtered over the group's tile. Every thread of a group calls it, inside
// the target or not (a barrier is in it); the caller writes the texels
// inside.
public float shadow_mask_texel(Texture2D<float4> depth, uint2 group, uint2 thread, MaskParams params) {
    int2 size    = int2(params.size);
    uint radius  = min(params.filter_radius, MAX_RADIUS);
    int2 texel   = int2(group * MASK_GROUP + thread);
    if (radius == 0u) {
        float4 surface;
        return mask_visibility(depth, clamp(texel, int2(0, 0), size - 1), params, surface);
    }
    // The group's tile, the apron's texels clamped to the target.
    uint tile    = MASK_GROUP + 2u * radius;
    int2 corner  = int2(group * MASK_GROUP) - int(radius);
    uint index   = thread.y * MASK_GROUP + thread.x;
    for (uint i = index; i < tile * tile; i += MASK_GROUP * MASK_GROUP) {
        int2 p = clamp(corner + int2(int(i % tile), int(i / tile)), int2(0, 0), size - 1);
        float4 surface;
        s_visibility[i] = mask_visibility(depth, p, params, surface);
        s_surface[i]    = surface;
    }
    GroupMemoryBarrierWithGroupSync();
    uint2 local   = thread + radius;
    float4 me     = s_surface[local.y * tile + local.x];
    float sigma   = 0.5 * float(radius) + 0.5;
    float scale   = 0.05 * max(abs(me.w), 1e-3);
    float sum     = 0.0;
    float weight  = 0.0;
    for (int dy = -int(radius); dy <= int(radius); ++dy) {
        for (int dx = -int(radius); dx <= int(radius); ++dx) {
            uint k   = uint(int(local.y) + dy) * tile + uint(int(local.x) + dx);
            float4 o = s_surface[k];
            float w  = exp(-float(dx * dx + dy * dy) / (2.0 * sigma * sigma));
            w *= pow(saturate(dot(o.xyz, me.xyz)), 8.0);
            w *= exp(-abs(o.w - me.w) / scale);
            sum += w * s_visibility[k];
            weight += w;
        }
    }
    return weight > 0.0 ? sum / weight : s_visibility[local.y * tile + local.x];
}
