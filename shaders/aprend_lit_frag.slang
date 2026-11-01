// Aprend's lit surface (aprendlit.h, APREND_LIT_SHADER_SURFACE_FRAGMENT): a
// metal/roughness surface with an optional clear coat and sheen, under the
// sun and its cascaded shadow, point and spot lights (spots with shadows of
// their own), a two-color hemisphere, an environment and ambient occlusion,
// shaded per pixel. The vertex shader is the caller's or Aprend's; it writes
// the four inputs below in any one space, which the scene's positions and
// directions are given in.
//
// Direct light: Cook-Torrance with GGX, Smith-Schlick (k = (r + 1)^2 / 8,
// for analytic lights) and Schlick's Fresnel, as Unreal's default lit
// shading and Hazel's. A clear coat is a second GGX lobe of F0 0.04 over the
// base, with Kelemen's visibility, taking its share of the light from it;
// sheen is the Charlie distribution with Neubelt's visibility (Estevez and
// Kulla; Filament's and glTF's KHR_materials_sheen). Point and spot lights
// fall off by the inverse square, windowed to 0 at their range (Karis, "Real
// Shading in Unreal Engine 4"); with the view's tiles they are read from the
// tile's list, not all of them. Ambient light: the hemisphere, plus the
// environment's irradiance and its prefiltered specular mips, the specular
// part weighted by the split sum's scale and bias from Aprend's BRDF lookup
// texture; occlusion (the ORM map's and the view's) darkens it alone.
//
// Light is scaled so a white, matte face square to a light reads the
// light's color: the diffuse part is albedo times radiance times the cosine,
// and the specular parts the BRDF times pi to match.

import aprend_lit_scene;
import aprend_lit_bindings;
import aprend_lit_material;

// The position first, then the four inputs, in the order a vertex shader
// writes them (aprend_lit_vert.slang), so D3D12 matches each by register.
struct LitFragmentInput {
    float4 frag_coord : SV_Position;
    [[vk::location(0)]] float3 position : TEXCOORD0;
    [[vk::location(1)]] float3 normal : TEXCOORD1;
    [[vk::location(2)]] float2 uv : TEXCOORD2;
    [[vk::location(3)]] float4 tangent : TEXCOORD3;
};

static const float PI = 3.14159265359;
// Below this a GGX highlight is too small to sample; Filament's floor.
static const float MIN_ROUGHNESS = 0.045;
// Below this the Charlie distribution's peak runs away.
static const float MIN_SHEEN_ROUGHNESS = 0.07;
// The share of light a sheen takes back from the layer under it, on
// average over its roughness: Charlie's directional albedo, about 0.18.
static const float SHEEN_ALBEDO = 0.18;

float3 safe_normalize(float3 v, float3 fallback) {
    float l = length(v);
    return l > 0.0 ? v / l : fallback;
}

float max3(float3 v) { return max(v.r, max(v.g, v.b)); }

// The ambient light from [direction]: the sky above, the ground below, mixed
// across the horizon.
float3 hemisphere(float3 direction) {
    float k = dot(direction, u_lit_scene.ambient_up.xyz) * 0.5 + 0.5;
    return lerp(u_lit_scene.ambient_ground.rgb, u_lit_scene.ambient_sky.rgb, k);
}

float3 to_environment(float3 direction) {
    return float3(dot(u_lit_scene.environment_rotation[0].xyz, direction), dot(u_lit_scene.environment_rotation[1].xyz, direction),
                  dot(u_lit_scene.environment_rotation[2].xyz, direction));
}

float ggx(float n_dot_h, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float d  = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

float smith_schlick(float n_dot_l, float n_dot_v, float roughness) {
    float r = roughness + 1.0;
    float k = r * r / 8.0;
    return (n_dot_l / (n_dot_l * (1.0 - k) + k)) * (n_dot_v / (n_dot_v * (1.0 - k) + k));
}

float3 fresnel(float3 f0, float cos_theta) {
    return f0 + (1.0 - f0) * pow(1.0 - cos_theta, 5.0);
}

float charlie(float n_dot_h, float roughness) {
    float a     = roughness * roughness;
    float inv_a = 1.0 / a;
    float sin2  = max(1.0 - n_dot_h * n_dot_h, 0.0);
    return (2.0 + inv_a) * pow(sin2, inv_a * 0.5) / (2.0 * PI);
}

float neubelt(float n_dot_l, float n_dot_v) {
    return 1.0 / (4.0 * (n_dot_l + n_dot_v - n_dot_l * n_dot_v));
}

// The split sum's scale and bias at [n_dot_v] and [roughness].
float2 split_sum(float n_dot_v, float roughness) {
    return brdf_lut.SampleLevel(environment_sampler, float2(n_dot_v, roughness), 0.0).rg;
}

// What a surface is made of, worked out once for every light.
struct Surface {
    float3 n;          // shading normal, normal map applied
    float3 geometric;  // the face's own, turned to the eye
    float3 v;
    float n_dot_v;
    float3 diffuse;
    float3 f0;
    float roughness;
    float clearcoat;
    float clearcoat_roughness;
    float3 sheen;
    float sheen_roughness;
};

// What the surface sends towards the eye of light arriving from [l] with
// [radiance].
float3 shade(Surface s, float3 l, float3 radiance) {
    float n_dot_l = dot(s.n, l);
    float3 h      = normalize(l + s.v);
    float v_dot_h = max(dot(s.v, h), 0.0);
    float3 color  = float3(0.0);
    if (n_dot_l > 0.0) {
        float n_dot_h     = max(dot(s.n, h), 0.0);
        float3 f          = fresnel(s.f0, v_dot_h);
        float3 specular   = f * ggx(n_dot_h, s.roughness) * smith_schlick(n_dot_l, s.n_dot_v, s.roughness) / (4.0 * n_dot_l * s.n_dot_v);
        float sheen_scale = 1.0 - max3(s.sheen) * SHEEN_ALBEDO;
        color             = ((1.0 - f) * s.diffuse + PI * specular) * sheen_scale;
        if (max3(s.sheen) > 0.0)
            color += PI * s.sheen * charlie(n_dot_h, s.sheen_roughness) * neubelt(n_dot_l, s.n_dot_v);
        color *= n_dot_l;
    }
    if (s.clearcoat > 0.0) {
        // On the face's own normal: a coat is smooth over any bumps below.
        float c_dot_l = dot(s.geometric, l);
        if (c_dot_l > 0.0) {
            float c_dot_h = max(dot(s.geometric, h), 0.0);
            float fc      = fresnel(float3(0.04), v_dot_h).x * s.clearcoat;
            float coat    = fc * ggx(c_dot_h, s.clearcoat_roughness) * 0.25 / max(v_dot_h * v_dot_h, 1e-4);
            color         = color * (1.0 - fc) + float3(PI * coat * c_dot_l);
        } else {
            color *= 1.0 - fresnel(float3(0.04), v_dot_h).x * s.clearcoat;
        }
    }
    return color * radiance;
}

// How much of a light gets past a shadow map: layer [layer] of [map]
// (read through the sampler that compares the right way), framed by
// [view_projection], with [params] (bias, normal offset, softness,
// strength). 1 outside the map. The map was rendered through
// [view_projection] with clip space's +Y up its first row: its rows run
// down from clip y = +1, so y is negated into the map's uv.
float shadow_visibility(Texture2DArray<float4> map, uint layer, float4x4 view_projection, float4 params, bool reversed, float3 position, float3 n) {
    float4 clip = mul(view_projection, float4(position + n * params.y, 1.0));
    if (clip.w <= 0.0)
        return 1.0;
    float3 ndc = clip.xyz / clip.w;
    float2 uv  = float2(ndc.x, -ndc.y) * 0.5 + 0.5;
    if (any(uv < float2(0.0)) || any(uv > float2(1.0)) || ndc.z < 0.0 || ndc.z > 1.0)
        return 1.0;
    float depth = reversed ? ndc.z + params.x : ndc.z - params.x;
    uint map_width, map_height, map_layers, map_levels;
    map.GetDimensions(0, map_width, map_height, map_layers, map_levels);
    float2 texel = 1.0 / float2(float(map_width), float(map_height));
    float lit    = 0.0;
    float taps   = params.z <= 0.0 ? 0.0 : 1.0;
    float count  = 0.0;
    // Each tap is the hardware's four-texel comparison, filtered; nine of
    // them [softness] texels apart soften the edge further.
    for (float y = -taps; y <= taps; y += 1.0) {
        for (float x = -taps; x <= taps; x += 1.0) {
            float3 at = float3(uv + float2(x, y) * texel * params.z, float(layer));
            lit += reversed ? map.SampleCmp(shadow_sampler_reversed, at, depth) : map.SampleCmp(shadow_sampler, at, depth);
            count += 1.0;
        }
    }
    return lerp(1.0, lit / count, params.w);
}

// The tenth of each cascade's stretch, at its far end, over which it is
// blended into the next (the last into no shadow).
static const float CASCADE_BLEND = 0.1;

// How much of the sun reaches [position]: the cascade its distance along
// the view falls in, blended into the next over the last tenth of the
// cascade's stretch so no seam shows between them, the last faded out to
// no shadow over its own.
float sun_visibility(float3 position, float3 n) {
    uint count = min(u_lit_scene.sun_cascade_count, uint(APREND_LIT_MAX_CASCADES));
    if ((u_lit_scene.flags & APREND_LIT_SCENE_SUN_SHADOW) == 0u || count == 0u)
        return 1.0;
    float along  = dot(position - u_lit_scene.eye_position.xyz, u_lit_scene.view_direction.xyz);
    uint cascade = count;
    for (uint i = 0u; i < count; ++i) {
        if (along <= u_lit_scene.sun_shadow_splits[i]) {
            cascade = i;
            break;
        }
    }
    if (cascade == count)
        return 1.0;
    bool reversed = (u_lit_scene.flags & APREND_LIT_SCENE_SUN_REVERSED) != 0u;
    float lit     = shadow_visibility(sun_shadow_map, cascade, u_lit_scene.sun_shadow_view_projection[cascade], u_lit_scene.sun_shadow_params, reversed,
                                      position, n);
    float begin   = cascade == 0u ? 0.0 : u_lit_scene.sun_shadow_splits[cascade - 1u];
    float end     = u_lit_scene.sun_shadow_splits[cascade];
    float blend   = smoothstep(end - CASCADE_BLEND * (end - begin), end, along);
    if (blend > 0.0) {
        float next = 1.0;
        if (cascade + 1u < count)
            next = shadow_visibility(sun_shadow_map, cascade + 1u, u_lit_scene.sun_shadow_view_projection[cascade + 1u], u_lit_scene.sun_shadow_params,
                                     reversed, position, n);
        lit = lerp(lit, next, blend);
    }
    return lit;
}

// [position]: the fragment's, in_position as the vertex shader wrote it.
float3 point_or_spot(Surface s, AprendLitLight light, float3 position) {
    float3 to_light = light.position_range.xyz - position;
    float distance2 = max(dot(to_light, to_light), 1e-8);
    float distance  = sqrt(distance2);
    float range     = light.position_range.w;
    if (distance >= range)
        return float3(0.0);
    float3 l     = to_light / distance;
    float ratio  = distance / range;
    float window = clamp(1.0 - ratio * ratio * ratio * ratio, 0.0, 1.0);
    float fall   = window * window / distance2;
    if (light.radiance_type.w > 0.5) {
        float cos_axis = dot(-l, light.direction_inner.xyz);
        float cos_in   = light.direction_inner.w;
        float cos_out  = light.outer_shadow.x;
        float cone     = clamp((cos_axis - cos_out) / max(cos_in - cos_out, 1e-4), 0.0, 1.0);
        fall *= cone * cone;
        int shadow = int(light.outer_shadow.y);
        if (fall > 0.0 && shadow >= 0 && shadow < APREND_LIT_MAX_SPOT_SHADOWS) {
            bool reversed = (u_lit_scene.flags & APREND_LIT_SCENE_SPOT_REVERSED) != 0u;
            fall *= shadow_visibility(spot_shadow_map, uint(shadow), u_lit_scene.spot_shadow_view_projection[shadow], u_lit_scene.spot_shadow_params[shadow],
                                      reversed, position, s.geometric);
        }
    } else {
        // A point light's shadow: the cube face the light looks along to
        // reach this fragment, by the largest of the offset's axes, then
        // that face's layer as a spot's.
        int shadow = int(light.outer_shadow.z);
        if (fall > 0.0 && shadow >= 0 && shadow < APREND_LIT_MAX_POINT_SHADOWS) {
            float3 d      = position - light.position_range.xyz;
            float3 a      = abs(d);
            uint face     = a.x >= a.y && a.x >= a.z ? (d.x >= 0.0 ? 0u : 1u) : (a.y >= a.z ? (d.y >= 0.0 ? 2u : 3u) : (d.z >= 0.0 ? 4u : 5u));
            uint layer    = uint(shadow) * 6u + face;
            bool reversed = (u_lit_scene.flags & APREND_LIT_SCENE_POINT_REVERSED) != 0u;
            fall *= shadow_visibility(point_shadow_map, layer, u_lit_scene.point_shadow_view_projection[layer], u_lit_scene.point_shadow_params[shadow],
                                      reversed, position, s.geometric);
        }
    }
    return fall > 0.0 ? shade(s, l, light.radiance_type.rgb * fall) : float3(0.0);
}

// What the opaque picture shows through the surface at [position]: the
// view ray bent into the surface by its index of refraction, followed
// through its thickness, and looked up where that point falls on the
// screen, blurrier (a lower mip) the rougher the surface; tinted by the
// distance travelled (Beer's law, glTF's KHR_materials_volume). Unexposed,
// as every other light here is until the end. [frag_coord]: the
// fragment's SV_Position.
float3 transmitted(Surface s, float3 position, float4 frag_coord) {
    float ior       = max(u_lit_material.transmission.y, 1.0);
    float thickness = max(u_lit_material.transmission.z, 0.0);
    float3 inside   = refract(-s.v, s.n, 1.0 / ior);
    float3 exit     = position + inside * thickness;
    float4 clip     = mul(u_lit_scene.view_projection, float4(exit, 1.0));
    float2 uv       = frag_coord.xy * u_lit_scene.target_size.zw;
    if (clip.w > 0.0) {
        // SpudGPU's clip space: +Y up the target, so y negated into uv,
        // whose rows run down as SV_Position's do.
        float2 ndc = clip.xy / clip.w;
        uv         = float2(ndc.x, -ndc.y) * 0.5 + 0.5;
    }
    // three.js's choice of mip: the roughness, sharpened as the index nears 1.
    float rough   = s.roughness * clamp(ior * 2.0 - 2.0, 0.0, 1.0);
    float lod     = rough * max(u_lit_scene.scene_color_mips - 1.0, 0.0);
    float3 behind = scene_color.SampleLevel(environment_sampler, uv, lod).rgb;
    if (u_lit_scene.output_encoding == 1u) // written encoded: back to linear
        behind = select(behind <= float3(0.04045), behind / 12.92, pow((behind + 0.055) / 1.055, float3(2.4)));
    behind /= max(u_lit_scene.exposure, 1e-20);
    float distance = u_lit_material.transmission.w;
    if (distance > 0.0)
        behind *= pow(max(u_lit_material.attenuation.rgb, float3(0.0)), float3(thickness / distance));
    return behind;
}

[shader("fragment")]
float4 aprend_lit_frag(LitFragmentInput input) : SV_Target0 {
    float3 in_position = input.position;
    float3 in_normal   = input.normal;
    float4 in_tangent  = input.tangent;

    float2 uv = material_uv(input.uv);
    uint maps = u_lit_material.maps;

    float4 base = u_lit_material.base_color;
    if ((maps & MAP_BASE_COLOR) != 0u)
        base *= base_color_map.Sample(material_sampler, map_uv(uv, MAP_INDEX_BASE_COLOR));
    float opacity = clamp(base.a, 0.0, 1.0);
    if (u_lit_material.alpha_cutoff > 0.0) {
        if (opacity < u_lit_material.alpha_cutoff)
            discard;
        opacity = 1.0;
    }

    float3 orm = float3(1.0);
    if ((maps & MAP_ORM) != 0u)
        orm = orm_map.Sample(material_sampler, map_uv(uv, MAP_INDEX_ORM)).rgb;
    float occlusion = lerp(1.0, orm.r, clamp(u_lit_material.occlusion_strength, 0.0, 1.0));
    // The view's bent normal, if the scene reads it: 0 for none.
    float3 bent_normal = float3(0.0);
    if ((u_lit_scene.flags & APREND_LIT_SCENE_OCCLUSION) != 0u) {
        float4 view_occlusion = ambient_occlusion.SampleLevel(environment_sampler, input.frag_coord.xy * u_lit_scene.target_size.zw, 0.0);
        occlusion *= lerp(1.0, view_occlusion.r, clamp(u_lit_scene.occlusion_strength, 0.0, 1.0));
        if ((u_lit_scene.flags & APREND_LIT_SCENE_BENT_NORMALS) != 0u)
            bent_normal = view_occlusion.gba * 2.0 - 1.0;
    }
    float3 emission = max(u_lit_material.emission.rgb, float3(0.0));
    if ((maps & MAP_EMISSION) != 0u)
        emission *= emission_map.Sample(material_sampler, map_uv(uv, MAP_INDEX_EMISSION)).rgb;

    Surface s;
    // The geometric normal, turned to the eye: a face seen from behind is
    // lit as its front, and one with no normal faces the eye.
    s.v = safe_normalize(u_lit_scene.eye_position.xyz - in_position, safe_normalize(in_normal, float3(0.0, 0.0, 1.0)));
    s.n = safe_normalize(in_normal, s.v);
    if (dot(s.n, s.v) < 0.0)
        s.n = -s.n;
    s.geometric = s.n;
    // The normal map, in the frame of the tangent, the bitangent its sign
    // makes (MikkTSpace's), and the normal.
    if ((maps & MAP_NORMAL) != 0u && dot(in_tangent.xyz, in_tangent.xyz) > 0.0) {
        float3 t = in_tangent.xyz - s.n * dot(s.n, in_tangent.xyz);
        if (dot(t, t) > 0.0) {
            t         = normalize(t);
            float3 b  = cross(s.n, t) * (in_tangent.w < 0.0 ? -1.0 : 1.0);
            float3 tn = normal_map.Sample(material_sampler, map_uv(uv, MAP_INDEX_NORMAL)).xyz * 2.0 - 1.0;
            tn.xy *= u_lit_material.normal_scale;
            s.n = safe_normalize(t * tn.x + b * tn.y + s.n * tn.z, s.n);
        }
    }

    float metalness       = clamp(u_lit_material.metalness * orm.b, 0.0, 1.0);
    float3 albedo         = max(base.rgb, float3(0.0));
    s.roughness           = clamp(u_lit_material.roughness * orm.g, MIN_ROUGHNESS, 1.0);
    s.f0                  = lerp(float3(0.08 * clamp(u_lit_material.specular, 0.0, 1.0)), albedo, metalness);
    s.diffuse             = albedo * (1.0 - metalness);
    // Transmission takes that share of the diffuse lobe: the light that
    // would scatter back passes through instead (glTF's transmission).
    float transmission = 0.0;
    if ((u_lit_scene.flags & APREND_LIT_SCENE_SCENE_COLOR) != 0u)
        transmission = clamp(u_lit_material.transmission.x, 0.0, 1.0) * (1.0 - metalness);
    s.diffuse *= 1.0 - transmission;
    s.n_dot_v             = max(dot(s.n, s.v), 1e-4);
    s.clearcoat           = clamp(u_lit_material.clearcoat, 0.0, 1.0);
    s.clearcoat_roughness = clamp(u_lit_material.clearcoat_roughness, MIN_ROUGHNESS, 1.0);
    s.sheen               = max(u_lit_material.sheen.rgb, float3(0.0));
    s.sheen_roughness     = clamp(u_lit_material.sheen.a, MIN_SHEEN_ROUGHNESS, 1.0);

    float3 color = float3(0.0);
    float3 sun   = u_lit_scene.sun_radiance.rgb;
    if (any(sun > float3(0.0)))
        color += shade(s, u_lit_scene.sun_direction.xyz, sun * sun_visibility(in_position, s.geometric));

    if ((u_lit_scene.flags & APREND_LIT_SCENE_TILES) != 0u) {
        uint2 tile = min(uint2(input.frag_coord.xy) / max(u_lit_scene.tile_size, 1u), uint2(u_lit_scene.tiles_x, u_lit_scene.tiles_y) - 1u);
        uint first = (tile.y * u_lit_scene.tiles_x + tile.x) * uint(APREND_LIT_MAX_TILE_LIGHTS + 1);
        uint count = min(tile_lights(first), uint(APREND_LIT_MAX_TILE_LIGHTS));
        for (uint i = 0u; i < count; ++i)
            color += point_or_spot(s, u_lit_scene.lights[min(tile_lights(first + 1u + i), uint(APREND_LIT_MAX_LIGHTS - 1))], in_position);
    } else {
        for (uint i = 0u; i < min(u_lit_scene.light_count, uint(APREND_LIT_MAX_LIGHTS)); ++i)
            color += point_or_spot(s, u_lit_scene.lights[i], in_position);
    }

    float3 r                = reflect(-s.v, s.n);
    // Diffuse ambient light along the bent normal, where the surface is
    // open, if there is one.
    float3 diffuse_normal   = safe_normalize(bent_normal, s.n);
    float3 ambient_diffuse  = hemisphere(diffuse_normal);
    float3 ambient_specular = hemisphere(r);
    float3 r_coat           = reflect(-s.v, s.geometric);
    float3 ambient_coat     = hemisphere(r_coat);
    if ((u_lit_scene.flags & APREND_LIT_SCENE_ENVIRONMENT) != 0u) {
        float intensity = u_lit_scene.environment_intensity;
        float top       = max(u_lit_scene.environment_mips - 1.0, 0.0);
        ambient_diffuse += intensity * environment_irradiance.SampleLevel(environment_sampler, to_environment(diffuse_normal), 0.0).rgb;
        ambient_specular += intensity * environment_specular.SampleLevel(environment_sampler, to_environment(r), s.roughness * top).rgb;
        ambient_coat += intensity * environment_specular.SampleLevel(environment_sampler, to_environment(r_coat), s.clearcoat_roughness * top).rgb;
    }
    float2 ab      = split_sum(s.n_dot_v, s.roughness);
    float3 ambient = s.diffuse * ambient_diffuse + (s.f0 * ab.x + ab.y) * ambient_specular;
    ambient        = ambient * (1.0 - max3(s.sheen) * SHEEN_ALBEDO) + s.sheen * SHEEN_ALBEDO * ambient_diffuse;
    if (s.clearcoat > 0.0) {
        float c_dot_v = max(dot(s.geometric, s.v), 1e-4);
        float2 abc    = split_sum(c_dot_v, s.clearcoat_roughness);
        float fc      = fresnel(float3(0.04), c_dot_v).x * s.clearcoat;
        ambient       = ambient * (1.0 - fc) + (0.04 * abc.x + abc.y) * s.clearcoat * ambient_coat;
    }
    color += occlusion * ambient;
    if (transmission > 0.0)
        color += transmission * albedo * (1.0 - (s.f0 * ab.x + ab.y)) * transmitted(s, in_position, input.frag_coord);
    color += emission;

    color *= u_lit_scene.exposure;
    if (u_lit_scene.output_encoding == 1u) {
        float3 c = clamp(color, 0.0, 1.0);
        color    = select(c <= float3(0.0031308), c * 12.92, 1.055 * pow(c, float3(1.0 / 2.4)) - 0.055);
    }
    return float4(color * opacity, opacity);
}
