module rbtrend_rt_trace;

// What Rend's traced passes share (RBTREND_COMMAND_LIT_SHADOW_MASK and
// RBTREND_COMMAND_LIT_PATH_TRACE, rbtrendraytracing.h): set 1, a TLAS and the
// tables a trace of it reads, declared alike by every traced shader so one
// set of a TLAS (rbtrend_tlas_t::trace_set) serves both; the camera ray;
// the low-discrepancy and blue-noise numbers; directions over a cone; and
// where a ray leaves a surface.
//
// Set 0 is the scene's block (rbtrend_lit_scene, imported by each shader).

import rbtrend_rt_common;

// The TLAS, as its last build left it.
[[vk::binding(0, 1)]] public RaytracingAccelerationStructure rt_scene : register(t0, space1);
// Its current records (RBTREND_RAY_INSTANCE, 64 bytes each) and material
// table (a lit-material slot an entry, 0 for NULL).
[[vk::binding(1, 1)]] public readonly RWByteAddressBuffer rt_records : register(u1, space1);
[[vk::binding(2, 1)]] public readonly RWByteAddressBuffer rt_materials : register(u2, space1);
// The instance's BLAS table, geometry arena and lit-material table.
[[vk::binding(3, 1)]] public readonly RWByteAddressBuffer rt_blases : register(u3, space1);
[[vk::binding(4, 1)]] public readonly RWByteAddressBuffer rt_arena : register(u4, space1);
[[vk::binding(5, 1)]] public readonly RWByteAddressBuffer rt_lit : register(u5, space1);
// The emitter table: per instance, the emissive triangles before it, then
// the total (rbtrend_rt_emitters_comp.slang).
[[vk::binding(6, 1)]] public readonly RWByteAddressBuffer rt_emitters : register(u6, space1);

public static const float RT_PI = 3.14159265358979;

// The point of clip space at pixel [p]'s place on a [size] target, row 0 at
// the top (clip space is +Y up on every backend, card S19), offset by
// [jitter] within the pixel (0.5 is its middle), at depth [z].
public float4 rt_clip(float2 p, float2 jitter, float2 size, float z) {
    float2 uv = (p + jitter) / size;
    return float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, z, 1.0);
}

// [clip] brought back into the scene's space through [inverse].
public float3 rt_unproject(float4x4 inverse, float4 clip) {
    float4 p = mul(inverse, clip);
    return p.xyz / p.w;
}

// A hash of [x] to 32 well-mixed bits (Hash Prospector's lowbias32).
public uint rt_hash(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// Jimenez's interleaved gradient noise at pixel [p]: a blue-noise-like
// value in [0, 1) that neighbouring pixels spread apart.
public float rt_interleaved_gradient(float2 p) {
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

// Point [i] of Roberts' R2 sequence: the plastic number's two-dimensional
// low-discrepancy points, in [0, 1)^2.
public float2 rt_r2(uint i) {
    static const float2 alpha = float2(0.7548776662466927, 0.5698402909980532);
    return frac(0.5 + alpha * float(i));
}

// A direction within [cos_max] of the unit [axis], uniform over that cone's
// solid angle, from [u] in [0, 1)^2.
public float3 rt_cone(float3 axis, float cos_max, float2 u) {
    float cos_theta = 1.0 - u.x * (1.0 - cos_max);
    float sin_theta = sqrt(max(0.0, 1.0 - cos_theta * cos_theta));
    float phi       = 2.0 * RT_PI * u.y;
    float3 t        = abs(axis.z) < 0.999 ? normalize(cross(axis, float3(0.0, 0.0, 1.0))) : float3(1.0, 0.0, 0.0);
    float3 b        = cross(axis, t);
    return normalize(t * (cos(phi) * sin_theta) + b * (sin(phi) * sin_theta) + axis * cos_theta);
}

// Where a ray leaves a surface at [p] with geometric normal [n] (on the side
// it leaves from) without hitting it again: Ray Tracing Gems, chapter 6
// (Wächter and Binder), the point moved along the normal in integer steps of
// its float bits, far enough at any scale.
public float3 rt_offset(float3 p, float3 n) {
    static const float origin      = 1.0 / 32.0;
    static const float float_scale = 1.0 / 65536.0;
    static const float int_scale   = 256.0;
    int3 of_i = int3(int_scale * n);
    float3 p_i = float3(asfloat(asint(p.x) + (p.x < 0.0 ? -of_i.x : of_i.x)),
                        asfloat(asint(p.y) + (p.y < 0.0 ? -of_i.y : of_i.y)),
                        asfloat(asint(p.z) + (p.z < 0.0 ? -of_i.z : of_i.z)));
    return float3(abs(p.x) < origin ? p.x + float_scale * n.x : p_i.x,
                  abs(p.y) < origin ? p.y + float_scale * n.y : p_i.y,
                  abs(p.z) < origin ? p.z + float_scale * n.z : p_i.z);
}

// Whether a ray from [origin] along [direction] reaches [distance] through
// the instances [cull_mask] lets in: the first hit ends the search, and no
// hit is the shader's to decide (every geometry is opaque, D8).
public bool rt_visible(float3 origin, float3 direction, float distance, uint cull_mask) {
    RayDesc ray;
    ray.Origin    = origin;
    ray.Direction = direction;
    ray.TMin      = 0.0;
    ray.TMax      = distance;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(rt_scene, RAY_FLAG_NONE, cull_mask, ray);
    while (query.Proceed()) {
    }
    return query.CommittedStatus() == COMMITTED_NOTHING;
}
