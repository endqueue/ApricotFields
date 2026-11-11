module rbtrend_rt_path_trace;

// RBTREND_COMMAND_LIT_PATH_TRACE's megakernel body (T10, card T16): one
// compute dispatch a step, one thread a pixel, each tracing _samples paths
// and folding them into the running mean in the target. The entry points
// (rbtrend_rt_path_trace_<albedo>_<normal>_comp) differ only in the guides'
// declared formats, which storage images name.
//
// Practice as Falcor's and Cycles' path tracers, written here from the
// papers:
// - next-event estimation at every hit, with multiple importance sampling
//   (Veach's power heuristic) between the BSDF and each light it can meet:
//   the environment, every directional light over its disc, and emissive
//   triangles chosen uniformly
//   from the TLAS's emitter table; point and spot lights without MIS (a
//   BSDF ray can't hit a point);
// - Owen-scrambled Sobol points (Burley, "Practical Hash-based Owen
//   Scrambling", 2020: the index shuffled and both dimensions scrambled by
//   Laine and Karras's hash), a pair of dimensions a decision, with
//   subpixel jitter from the first;
// - Russian roulette from half the bounces; the light a bounce after the
//   first brings clamped to _clamp (0 for none);
// - rays leave a surface by Ray Tracing Gems' chapter 6 offsets
//   (rt_offset).
// The surface is the lit material model as the raster path has it: a
// Lambertian diffuse lobe and a GGX specular lobe, f0 = lerp(0.08 x
// specular, base color, metalness), every surface opaque and lit from both
// faces, each giving off its emission from both. The material maps,
// opacity, transmission, the coat and the sheen are not read (card A15's
// maps; the rest a later step). The environment is RBTREND_LIT_AMBIENT's
// hemisphere (its sky above the up axis, its ground below), sampled with
// the cosine about the surface's normal; an environment's cubes are not
// read yet (rbtrendcommands.h, LIT_PATH_TRACE: recorded in RBTREND_TODO.md
// item 24).

import rbtrend_lit_scene;
import rbtrend_rt_common;
import rbtrend_rt_trace;

// rbtrend_rt_trace_params.
public struct TraceParams {
    public uint2 size;
    public uint samples;
    public uint accumulated;
    public uint seed;
    public uint max_bounces;
    public uint cull_mask;
    public float clamp;
    public uint guides;           // bit 0 the albedo, bit 1 the normal
    public uint instance_count;   // of the TLAS's last build
    public uint material_count;   // its material table's length
    public uint padding;
};

// What a path's first hit leaves for the guides.
public struct PathResult {
    public float3 radiance;
    public float3 albedo;
    public float3 normal;
};

static const float MIN_ROUGHNESS = 0.045;
// A cosine at or below this is a ray along a surface: no light crosses it.
// It keeps a light sampled on the plane of the point it lights from giving
// a few bits of light, so an emitter seen face on reads its emission
// exactly.
static const float GRAZING = 1e-4;
// Decisions (pairs of dimensions) a bounce takes: the light chosen and the
// point on it; the BSDF's lobe and direction; the roulette.
static const uint DIMENSIONS_A_BOUNCE = 4u;

// ---- Owen-scrambled Sobol --------------------------------------------------

// Sobol's second dimension: its direction numbers are those of x + 1.
uint sobol_second(uint i) {
    uint v = 0x80000000u;
    uint x = 0u;
    for (; i != 0u; i >>= 1u) {
        if ((i & 1u) != 0u)
            x ^= v;
        v ^= v >> 1u;
    }
    return x;
}

// Laine and Karras's hash, an Owen scramble of the bits read high to low.
uint laine_karras(uint x, uint seed) {
    x += seed;
    x ^= x * 0x6c50b47cu;
    x ^= x * 0xb82f1e52u;
    x ^= x * 0xc7afe638u;
    x ^= x * 0x8d22f6e6u;
    return x;
}

uint nested_uniform_scramble(uint x, uint seed) {
    return reversebits(laine_karras(reversebits(x), seed));
}

// Point [index] of the Owen-scrambled Sobol sequence of [seed], its index
// shuffled by the same scramble (Burley's padding of independent pairs), in
// [0, 1)^2.
float2 sobol_pair(uint index, uint seed) {
    const uint shuffled = nested_uniform_scramble(index, rt_hash(seed));
    const uint x        = nested_uniform_scramble(reversebits(shuffled), rt_hash(seed ^ 0xa511e9b3u));
    const uint y        = nested_uniform_scramble(sobol_second(shuffled), rt_hash(seed ^ 0x63d83595u));
    return float2(float(x >> 8u), float(y >> 8u)) * (1.0 / 16777216.0);
}

// ---- The material, a Slang interface (T10) --------------------------------

float luminance(float3 c) {
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// A surface's BSDF at a hit, seen from [v] about the shading normal [n].
public interface IRtBsdf {
    // f(v, l) cos(n, l), and the pdf of [l] as sample() draws it.
    float3 eval(float3 l, out float pdf);
    // A direction from [u] and the lobe choice [w], its f cos / pdf in
    // [weight] and its pdf; false for none (the surface scatters nothing).
    bool sample(float2 u, float w, out float3 l, out float3 weight, out float pdf);
};

// The lit material's BSDF (rbtrendlit.h): Lambert and GGX, as the raster
// surface has them.
public struct LitBsdf : IRtBsdf {
    float3 n;
    float3 v;
    float3 diffuse;
    float3 f0;
    float alpha;
    float specular_share;

    [mutating]
    public void init(float3 normal, float3 view, float4 base_color, float4 surface) {
        n                = normal;
        v                = view;
        float metalness  = clamp(surface.y, 0.0, 1.0);
        float3 albedo    = max(base_color.rgb, float3(0.0, 0.0, 0.0));
        float roughness  = clamp(surface.x, MIN_ROUGHNESS, 1.0);
        alpha            = roughness * roughness;
        f0               = lerp(float3(0.08 * clamp(surface.z, 0.0, 1.0)), albedo, metalness);
        diffuse          = albedo * (1.0 - metalness);
        float d          = luminance(diffuse);
        float s          = luminance(f0) > 0.0 ? max(luminance(f0), 0.04) : 0.0;
        specular_share   = d + s > 0.0 ? s / (d + s) : 0.0;
    }

    float d_ggx(float nh) {
        float a2 = alpha * alpha;
        float t  = nh * nh * (a2 - 1.0) + 1.0;
        return a2 / (RT_PI * t * t);
    }

    float g1(float nx) {
        float a2 = alpha * alpha;
        return 2.0 * nx / (nx + sqrt(a2 + (1.0 - a2) * nx * nx));
    }

    float3 fresnel(float vh) {
        return f0 + (1.0 - f0) * pow(1.0 - vh, 5.0);
    }

    public float3 eval(float3 l, out float pdf) {
        float nl = dot(n, l);
        float nv = max(dot(n, v), 1e-4);
        pdf      = 0.0;
        if (nl <= 0.0)
            return float3(0.0, 0.0, 0.0);
        float3 value = diffuse * (nl / RT_PI);
        pdf          = (1.0 - specular_share) * nl / RT_PI;
        if (specular_share > 0.0) {
            float3 h = normalize(v + l);
            float nh = max(dot(n, h), 0.0);
            float vh = max(dot(v, h), 1e-4);
            float d  = d_ggx(nh);
            value += fresnel(vh) * d * g1(nv) * g1(nl) / (4.0 * nv);
            pdf += specular_share * d * g1(nv) / (4.0 * nv);
        }
        return value;
    }

    public bool sample(float2 u, float w, out float3 l, out float3 weight, out float pdf) {
        l      = float3(0.0, 0.0, 1.0);
        weight = float3(0.0, 0.0, 0.0);
        pdf    = 0.0;
        if (luminance(diffuse) <= 0.0 && specular_share <= 0.0)
            return false;
        float3 t = abs(n.z) < 0.999 ? normalize(cross(n, float3(0.0, 0.0, 1.0))) : float3(1.0, 0.0, 0.0);
        float3 b = cross(n, t);
        if (w >= specular_share) {
            // The diffuse lobe: the cosine about the normal.
            float r   = sqrt(u.x);
            float phi = 2.0 * RT_PI * u.y;
            l         = normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(0.0, 1.0 - u.x)));
        } else {
            // The specular lobe: GGX's visible normals (Heitz, 2018).
            float3 vl = float3(dot(v, t), dot(v, b), dot(v, n));
            float3 vh = normalize(float3(alpha * vl.x, alpha * vl.y, vl.z));
            float len = vh.x * vh.x + vh.y * vh.y;
            float3 t1 = len > 0.0 ? float3(-vh.y, vh.x, 0.0) / sqrt(len) : float3(1.0, 0.0, 0.0);
            float3 t2 = cross(vh, t1);
            float r   = sqrt(u.x);
            float phi = 2.0 * RT_PI * u.y;
            float p1  = r * cos(phi);
            float p2  = r * sin(phi);
            float s   = 0.5 * (1.0 + vh.z);
            p2        = (1.0 - s) * sqrt(max(0.0, 1.0 - p1 * p1)) + s * p2;
            float3 nh = p1 * t1 + p2 * t2 + sqrt(max(0.0, 1.0 - p1 * p1 - p2 * p2)) * vh;
            float3 hl = normalize(float3(alpha * nh.x, alpha * nh.y, max(0.0, nh.z)));
            float3 h  = t * hl.x + b * hl.y + n * hl.z;
            l         = reflect(-v, h);
        }
        float3 value = eval(l, pdf);
        if (pdf <= 0.0)
            return false;
        weight = value / pdf;
        return true;
    }
};

// ---- Lights --------------------------------------------------------------

// The power heuristic's weight of a strategy of pdf [a] against one of [b].
float power_heuristic(float a, float b) {
    float a2 = a * a;
    float b2 = b * b;
    return a2 + b2 > 0.0 ? a2 / (a2 + b2) : 0.0;
}

// The hemisphere's radiance along [d] (RBTREND_LIT_AMBIENT): its sky above
// the up axis, its ground below.
float3 environment(float3 d) {
    return dot(d, u_lit_scene.ambient_up.xyz) >= 0.0 ? u_lit_scene.ambient_sky.rgb : u_lit_scene.ambient_ground.rgb;
}

// A directional light's disc of [versine] (1 - cos of its radius): its
// solid angle, over which the radiance each direction of it carries is
// spread, so a surface it lights gets the light's radiance as the raster
// surface's (pi times its value over the disc's solid angle, as the sky's
// disc is drawn).
float disc_solid_angle(float versine) {
    return 2.0 * RT_PI * versine;
}

// ---- The scene's geometry at a hit ------------------------------------------

public struct Hit {
    public float3 position;
    public float3 geometric;   // the triangle's normal, towards the ray's side
    public float3 normal;      // the shading normal, on the same side
    public float area;         // the triangle's area in the scene's space
    public float4 base_color;
    public float3 emission;
    public float4 surface;     // roughness, metalness, specular
};

// Words of instance [instance]'s record tail: its BLAS slot, material base,
// mask.
uint4 record_tail(uint instance) {
    return rt_records.Load4(instance * RT_RECORD_BYTES + RT_RECORD_TAIL);
}

// The three vertex indices of triangle [primitive] of geometry [header].
uint3 triangle_indices(uint4 header, uint primitive) {
    if (header.w != 0u)
        return rt_arena.Load3((header.y + primitive * 3u) * 4u);
    return uint3(primitive * 3u, primitive * 3u + 1u, primitive * 3u + 2u);
}

float3 vertex_position(uint4 header, uint index) {
    return asfloat(rt_arena.Load3((header.x + index * RT_VERTEX_WORDS) * 4u));
}

float3 vertex_normal(uint4 header, uint index) {
    return asfloat(rt_arena.Load3((header.x + index * RT_VERTEX_WORDS + 3u) * 4u));
}

// [m]'s 3x3 cofactor applied to [n]: a normal into the scene's space
// whatever the instance's scale.
float3 to_world_normal(float3x4 m, float3 n) {
    float3 c0 = float3(m[0][0], m[1][0], m[2][0]);
    float3 c1 = float3(m[0][1], m[1][1], m[2][1]);
    float3 c2 = float3(m[0][2], m[1][2], m[2][2]);
    return n.x * cross(c1, c2) + n.y * cross(c2, c0) + n.z * cross(c0, c1);
}

float3 to_world(float3x4 m, float3 p) {
    return mul(m, float4(p, 1.0));
}

// The lit material of entry [base + geometry] of the material table, of
// [material_count] entries (one past it reads as NULL).
void material_of(uint base, uint geometry, uint material_count, inout Hit hit) {
    hit.base_color = float4(0.0, 0.0, 0.0, 1.0);
    hit.emission   = float3(0.0, 0.0, 0.0);
    hit.surface    = float4(1.0, 0.0, 0.0, 0.0);
    const uint entry = base + geometry;
    if (entry >= material_count)
        return;
    const uint slot  = rt_materials.Load(entry * 4u);
    if (slot == 0u)
        return;
    const uint at  = (slot & RT_SLOT_INDEX_MASK) * RT_LIT_ENTRY_BYTES;
    hit.base_color = asfloat(rt_lit.Load4(at));
    hit.emission   = asfloat(rt_lit.Load3(at + 16u));
    hit.surface    = asfloat(rt_lit.Load4(at + 32u));
}

// The triangle of [instance], [geometry], [primitive] at barycentrics [b],
// seen along [direction].
public Hit hit_at(uint instance, uint geometry, uint primitive, float2 b, float3x4 to_scene, float3 direction, uint material_count) {
    Hit hit;
    const uint4 tail   = record_tail(instance);
    const uint4 entry  = rt_blases.Load4((tail.x & RT_SLOT_INDEX_MASK) * RT_BLAS_ENTRY_BYTES);
    const uint4 header = rt_arena.Load4((entry.z + geometry * RT_GEOMETRY_HEADER_WORDS) * 4u);
    const uint3 i      = triangle_indices(header, primitive);
    const float3 p0    = to_world(to_scene, vertex_position(header, i.x));
    const float3 p1    = to_world(to_scene, vertex_position(header, i.y));
    const float3 p2    = to_world(to_scene, vertex_position(header, i.z));
    const float w      = 1.0 - b.x - b.y;
    hit.position       = p0 * w + p1 * b.x + p2 * b.y;
    float3 g           = cross(p1 - p0, p2 - p0);
    hit.area           = 0.5 * length(g);
    g                  = length(g) > 0.0 ? normalize(g) : -direction;
    // The side the ray hit, from the geometric normal against the ray, not
    // the winding (D8).
    if (dot(g, direction) > 0.0)
        g = -g;
    hit.geometric = g;
    float3 n      = vertex_normal(header, i.x) * w + vertex_normal(header, i.y) * b.x + vertex_normal(header, i.z) * b.y;
    n             = to_world_normal(to_scene, n);
    n             = dot(n, n) > 0.0 ? normalize(n) : g;
    hit.normal    = dot(n, g) < 0.0 ? -n : n;
    material_of(tail.y, geometry, material_count, hit);
    return hit;
}

// Whether the closest hit of a ray exists; its parts if so.
bool trace_closest(float3 origin, float3 direction, uint cull_mask, uint material_count, out Hit hit) {
    RayDesc ray;
    ray.Origin    = origin;
    ray.Direction = direction;
    ray.TMin      = 0.0;
    ray.TMax      = 1e30;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(rt_scene, RAY_FLAG_NONE, cull_mask, ray);
    while (query.Proceed()) {
    }
    hit.position   = float3(0.0, 0.0, 0.0);
    hit.geometric  = float3(0.0, 0.0, 1.0);
    hit.normal     = float3(0.0, 0.0, 1.0);
    hit.area       = 0.0;
    hit.base_color = float4(0.0, 0.0, 0.0, 0.0);
    hit.emission   = float3(0.0, 0.0, 0.0);
    hit.surface    = float4(0.0, 0.0, 0.0, 0.0);
    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        return false;
    hit = hit_at(query.CommittedInstanceIndex(), query.CommittedGeometryIndex(), query.CommittedPrimitiveIndex(), query.CommittedTriangleBarycentrics(),
                 query.CommittedObjectToWorld3x4(), direction, material_count);
    return true;
}

// The emissive triangle [chosen] of [count] instances' emitter table:
// instance, geometry and primitive, through its prefix sums.
bool emitter_at(uint chosen, uint count, uint material_count, out uint instance, out uint geometry, out uint primitive) {
    instance  = 0u;
    geometry  = 0u;
    primitive = 0u;
    // The last instance whose prefix is at or below [chosen].
    uint low  = 0u;
    uint high = count;
    while (high - low > 1u) {
        uint middle = (low + high) / 2u;
        if (rt_emitters.Load(middle * 4u) <= chosen)
            low = middle;
        else
            high = middle;
    }
    instance          = low;
    uint local        = chosen - rt_emitters.Load(low * 4u);
    const uint4 tail  = record_tail(instance);
    const uint4 entry = rt_blases.Load4((tail.x & RT_SLOT_INDEX_MASK) * RT_BLAS_ENTRY_BYTES);
    for (uint g = 0u; g < entry.w; ++g) {
        const uint m = tail.y + g;
        if (m >= material_count)
            continue;
        const uint slot = rt_materials.Load(m * 4u);
        if (slot == 0u)
            continue;
        const float3 e = asfloat(rt_lit.Load3((slot & RT_SLOT_INDEX_MASK) * RT_LIT_ENTRY_BYTES + 16u));
        if (!any(e > float3(0.0, 0.0, 0.0)))
            continue;
        const uint4 header = rt_arena.Load4((entry.z + g * RT_GEOMETRY_HEADER_WORDS) * 4u);
        if (local < header.z) {
            geometry  = g;
            primitive = local;
            return true;
        }
        local -= header.z;
    }
    return false;
}

// The object-to-scene transform of instance [instance], from its record.
float3x4 instance_transform(uint instance) {
    const uint at = instance * RT_RECORD_BYTES;
    return float3x4(asfloat(rt_records.Load4(at)), asfloat(rt_records.Load4(at + 16u)), asfloat(rt_records.Load4(at + 32u)));
}

// The pdf, over solid angle, of the emitter sampling choosing the point
// [hit] seen from [from] along [direction], of [total] triangles.
float emitter_pdf(Hit hit, float3 from, float3 direction, uint total) {
    float cos_light = abs(dot(hit.geometric, direction));
    float3 d        = hit.position - from;
    if (total == 0u || hit.area <= 0.0 || cos_light <= GRAZING)
        return 0.0;
    return dot(d, d) / (float(total) * hit.area * cos_light);
}

// The light at a hit from its next-event estimation: every light, each
// sampled once, weighted against the BSDF where a BSDF ray could also find
// it. [u0] to [u1] the hit's decisions.
float3 next_events<B : IRtBsdf>(B bsdf, Hit hit, float3 origin, TraceParams params, float2 u0, float2 u1) {
    float3 light = float3(0.0, 0.0, 0.0);
    // The environment's hemisphere, cosine about the normal.
    {
        float3 n   = hit.normal;
        float3 t   = abs(n.z) < 0.999 ? normalize(cross(n, float3(0.0, 0.0, 1.0))) : float3(1.0, 0.0, 0.0);
        float3 b   = cross(n, t);
        float r    = sqrt(u0.x);
        float phi  = 2.0 * RT_PI * u0.y;
        float3 l   = normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(0.0, 1.0 - u0.x)));
        float pdf  = max(dot(n, l), 0.0) / RT_PI;
        float3 env = environment(l);
        if (pdf > 0.0 && any(env > float3(0.0, 0.0, 0.0)) && dot(l, hit.geometric) > GRAZING && rt_visible(origin, l, 1e30, params.cull_mask)) {
            float bsdf_pdf;
            float3 f = bsdf.eval(l, bsdf_pdf);
            light += f * env / pdf * power_heuristic(pdf, bsdf_pdf);
        }
    }
    // Each directional light over its disc (a delta light with no disc: no
    // MIS).
    for (uint d = 0u; d < min(u_lit_scene.directional_count, uint(RBTREND_LIT_MAX_DIRECTIONAL_LIGHTS)); ++d) {
        float3 sun = u_lit_scene.directional[d].radiance_disc.rgb;
        if (!any(sun > float3(0.0, 0.0, 0.0)))
            continue;
        float versine = u_lit_scene.directional[d].direction_versine.w;
        float3 axis   = normalize(u_lit_scene.directional[d].direction_versine.xyz);
        float cos_max = 1.0 - versine;
        bool disc     = cos_max < 1.0;
        float3 l      = disc ? rt_cone(axis, cos_max, u1) : axis;
        if (dot(l, hit.geometric) > GRAZING && rt_visible(origin, l, 1e30, params.cull_mask)) {
            float bsdf_pdf;
            float3 f = bsdf.eval(l, bsdf_pdf);
            if (disc) {
                float pdf = 1.0 / disc_solid_angle(versine);
                light += f * (RT_PI * sun / disc_solid_angle(versine)) / pdf * power_heuristic(pdf, bsdf_pdf);
            } else {
                light += f * RT_PI * sun;
            }
        }
    }
    // Point and spot lights, without MIS, as the raster surface falls them
    // off.
    for (uint i = 0u; i < u_lit_scene.light_count; ++i) {
        RbtrendLitLight point = u_lit_scene.lights[i];
        float3 to_light      = point.position_range.xyz - hit.position;
        float distance2      = max(dot(to_light, to_light), 1e-8);
        float distance       = sqrt(distance2);
        float range          = point.position_range.w;
        if (distance >= range)
            continue;
        float3 l     = to_light / distance;
        float ratio  = distance / range;
        float window = clamp(1.0 - ratio * ratio * ratio * ratio, 0.0, 1.0);
        float fall   = window * window / distance2;
        if (point.radiance_type.w > 0.5) {
            float cos_axis = dot(-l, point.direction_inner.xyz);
            float cone     = clamp((cos_axis - point.outer_shadow.x) / max(point.direction_inner.w - point.outer_shadow.x, 1e-4), 0.0, 1.0);
            fall *= cone * cone;
        }
        if (fall <= 0.0 || dot(l, hit.geometric) <= GRAZING || !rt_visible(origin, l, distance, params.cull_mask))
            continue;
        float bsdf_pdf;
        light += bsdf.eval(l, bsdf_pdf) * RT_PI * point.radiance_type.rgb * fall;
    }
    // One emissive triangle, chosen uniformly from the emitter table, a
    // point uniform over its area.
    const uint total = params.instance_count > 0u ? rt_emitters.Load(params.instance_count * 4u) : 0u;
    if (total > 0u) {
        uint chosen = min(uint(u1.x * float(total)), total - 1u);
        uint instance, geometry, primitive;
        // A second point of the pair for the place on the triangle.
        float2 v = frac(u1 * float2(float(total), 1.0) + float2(0.0, u0.x));
        if (emitter_at(chosen, params.instance_count, params.material_count, instance, geometry, primitive)) {
            float su    = sqrt(v.x);
            float2 b    = float2(su * (1.0 - v.y), su * v.y);
            Hit emitter = hit_at(instance, geometry, primitive, b, instance_transform(instance), float3(0.0, 0.0, -1.0), params.material_count);
            float3 d    = emitter.position - hit.position;
            float dist  = length(d);
            if (dist > 0.0) {
                float3 l        = d / dist;
                float cos_light = abs(dot(emitter.geometric, l));
                float cos_here  = dot(l, hit.geometric);
                if (cos_light > GRAZING && cos_here > GRAZING && emitter.area > 0.0 && rt_visible(origin, l, dist * (1.0 - 1e-4), params.cull_mask)) {
                    float pdf = dist * dist / (float(total) * emitter.area * cos_light);
                    float bsdf_pdf;
                    float3 f = bsdf.eval(l, bsdf_pdf);
                    light += f * emitter.emission / pdf * power_heuristic(pdf, bsdf_pdf);
                }
            }
        }
    }
    return light;
}

// One path of pixel [pixel], sample [index], of the pixel's [seed].
public PathResult trace_path(uint2 pixel, uint index, uint seed, TraceParams params) {
    PathResult result;
    result.radiance = float3(0.0, 0.0, 0.0);
    result.albedo   = float3(0.0, 0.0, 0.0);
    result.normal   = float3(0.0, 0.0, 0.0);
    // The camera ray: from the view's near plane through a jittered point
    // of the pixel, along its depth, the eye's side whichever end that is.
    float2 jitter = sobol_pair(index, seed);
    float3 a      = rt_unproject(u_lit_scene.inverse_view_projection, rt_clip(float2(pixel), jitter, float2(params.size), 0.0));
    float3 b      = rt_unproject(u_lit_scene.inverse_view_projection, rt_clip(float2(pixel), jitter, float2(params.size), 1.0));
    float3 eye    = u_lit_scene.eye_position.xyz;
    bool a_near   = dot(a - eye, a - eye) <= dot(b - eye, b - eye);
    float3 origin = a_near ? a : b;
    float3 dir    = normalize(a_near ? b - a : a - b);

    float3 throughput   = float3(1.0, 1.0, 1.0);
    float last_bsdf_pdf = 0.0;  // 0: the camera's ray, or a hit after none
    float3 last_normal  = float3(0.0, 0.0, 0.0);
    float3 last_point   = origin;
    uint total          = params.instance_count > 0u ? rt_emitters.Load(params.instance_count * 4u) : 0u;
    for (uint bounce = 0u;; ++bounce) {
        Hit hit;
        float3 add = float3(0.0, 0.0, 0.0);
        if (!trace_closest(origin, dir, params.cull_mask, params.material_count, hit)) {
            // The environment, and each directional light's disc the ray meets,
            // each weighted against its own sampling.
            float3 env = environment(dir);
            if (bounce > 0u) {
                float env_pdf = max(dot(last_normal, dir), 0.0) / RT_PI;
                env *= power_heuristic(last_bsdf_pdf, env_pdf);
            }
            add = env;
            for (uint d = 0u; d < min(u_lit_scene.directional_count, uint(RBTREND_LIT_MAX_DIRECTIONAL_LIGHTS)); ++d) {
                float3 sun    = u_lit_scene.directional[d].radiance_disc.rgb;
                float versine = u_lit_scene.directional[d].direction_versine.w;
                float cos_max = 1.0 - versine;
                if (cos_max < 1.0 && any(sun > float3(0.0, 0.0, 0.0)) && dot(dir, normalize(u_lit_scene.directional[d].direction_versine.xyz)) >= cos_max) {
                    float3 disc = RT_PI * sun / disc_solid_angle(versine);
                    if (bounce > 0u)
                        disc *= power_heuristic(last_bsdf_pdf, 1.0 / disc_solid_angle(versine));
                    add += disc;
                }
            }
            add *= throughput;
            if (bounce > 1u && params.clamp > 0.0)
                add = min(add, float3(params.clamp, params.clamp, params.clamp));
            result.radiance += add;
            break;
        }
        float3 v = -dir;
        if (bounce == 0u) {
            result.albedo = hit.base_color.rgb;
            result.normal = hit.normal;
        }
        // What it gives off, from either face, weighted against the
        // emitter sampling that could have chosen it; a bounce after the
        // first's clamped.
        if (any(hit.emission > float3(0.0, 0.0, 0.0))) {
            float3 e = hit.emission;
            if (bounce > 0u)
                e *= power_heuristic(last_bsdf_pdf, emitter_pdf(hit, last_point, dir, total));
            e *= throughput;
            if (bounce > 1u && params.clamp > 0.0)
                e = min(e, float3(params.clamp, params.clamp, params.clamp));
            result.radiance += e;
        }
        LitBsdf bsdf;
        bsdf.init(hit.normal, v, hit.base_color, hit.surface);
        const uint dimension = 1u + bounce * DIMENSIONS_A_BOUNCE;
        float2 u_light       = sobol_pair(index, rt_hash(seed + dimension));
        float2 u_place       = sobol_pair(index, rt_hash(seed + dimension + 1u));
        float2 u_bsdf        = sobol_pair(index, rt_hash(seed + dimension + 2u));
        float2 u_choice      = sobol_pair(index, rt_hash(seed + dimension + 3u));
        float3 leave         = rt_offset(hit.position, hit.geometric);
        // The light it receives directly: a bounce after the first's
        // clamped from the second hit on.
        add = next_events(bsdf, hit, leave, params, u_light, u_place) * throughput;
        if (bounce > 0u && params.clamp > 0.0)
            add = min(add, float3(params.clamp, params.clamp, params.clamp));
        result.radiance += add;
        if (bounce + 1u > params.max_bounces)
            break;
        // The next direction, from the BSDF.
        float3 l;
        float3 weight;
        float pdf;
        if (!bsdf.sample(u_bsdf, u_choice.x, l, weight, pdf))
            break;
        if (dot(l, hit.geometric) <= GRAZING)
            break;
        throughput *= weight;
        // Russian roulette from half the bounces.
        if (bounce + 1u >= (params.max_bounces + 1u) / 2u && bounce > 0u) {
            float keep = clamp(max(throughput.x, max(throughput.y, throughput.z)), 0.05, 1.0);
            if (u_choice.y >= keep)
                break;
            throughput /= keep;
        }
        last_bsdf_pdf = pdf;
        last_normal   = hit.normal;
        last_point    = hit.position;
        origin        = leave;
        dir           = l;
    }
    return result;
}

// The seed of pixel [pixel] for a step of [params].
public uint pixel_seed(uint2 pixel, TraceParams params) {
    return rt_hash(pixel.x + params.size.x * pixel.y) ^ rt_hash(params.seed * 0x9e3779b9u + 0x7f4a7c15u);
}
