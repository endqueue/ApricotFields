#version 450
#extension GL_GOOGLE_include_directive : require

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

#include "aprend_lit_scene.glslh"
#include "aprend_lit_bindings.glslh"
#include "aprend_lit_material.glslh"

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec4 in_tangent;

layout(location = 0) out vec4 out_color;

const float PI = 3.14159265359;
// Below this a GGX highlight is too small to sample; Filament's floor.
const float MIN_ROUGHNESS = 0.045;
// Below this the Charlie distribution's peak runs away.
const float MIN_SHEEN_ROUGHNESS = 0.07;
// The share of light a sheen takes back from the layer under it, on
// average over its roughness: Charlie's directional albedo, about 0.18.
const float SHEEN_ALBEDO = 0.18;

vec3 safe_normalize(vec3 v, vec3 fallback) {
    float l = length(v);
    return l > 0.0 ? v / l : fallback;
}

float max3(vec3 v) { return max(v.r, max(v.g, v.b)); }

// The ambient light from [direction]: the sky above, the ground below, mixed
// across the horizon.
vec3 hemisphere(vec3 direction) {
    float k = dot(direction, u_lit_scene.ambient_up.xyz) * 0.5 + 0.5;
    return mix(u_lit_scene.ambient_ground.rgb, u_lit_scene.ambient_sky.rgb, k);
}

vec3 to_environment(vec3 direction) {
    return vec3(dot(u_lit_scene.environment_rotation[0].xyz, direction), dot(u_lit_scene.environment_rotation[1].xyz, direction),
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

vec3 fresnel(vec3 f0, float cos_theta) {
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
vec2 split_sum(float n_dot_v, float roughness) {
    return textureLod(sampler2D(brdf_lut, environment_sampler), vec2(n_dot_v, roughness), 0.0).rg;
}

// What a surface is made of, worked out once for every light.
struct Surface {
    vec3 n;          // shading normal, normal map applied
    vec3 geometric;  // the face's own, turned to the eye
    vec3 v;
    float n_dot_v;
    vec3 diffuse;
    vec3 f0;
    float roughness;
    float clearcoat;
    float clearcoat_roughness;
    vec3 sheen;
    float sheen_roughness;
};

// What the surface sends towards the eye of light arriving from [l] with
// [radiance].
vec3 shade(Surface s, vec3 l, vec3 radiance) {
    float n_dot_l = dot(s.n, l);
    vec3 h        = normalize(l + s.v);
    float v_dot_h = max(dot(s.v, h), 0.0);
    vec3 color    = vec3(0.0);
    if (n_dot_l > 0.0) {
        float n_dot_h = max(dot(s.n, h), 0.0);
        vec3 f        = fresnel(s.f0, v_dot_h);
        vec3 specular = f * ggx(n_dot_h, s.roughness) * smith_schlick(n_dot_l, s.n_dot_v, s.roughness) / (4.0 * n_dot_l * s.n_dot_v);
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
            float fc      = fresnel(vec3(0.04), v_dot_h).x * s.clearcoat;
            float coat    = fc * ggx(c_dot_h, s.clearcoat_roughness) * 0.25 / max(v_dot_h * v_dot_h, 1e-4);
            color         = color * (1.0 - fc) + vec3(PI * coat * c_dot_l);
        } else {
            color *= 1.0 - fresnel(vec3(0.04), v_dot_h).x * s.clearcoat;
        }
    }
    return color * radiance;
}

// How much of a light gets past a shadow map: layer [layer] of [map]
// (read through the sampler that compares the right way), framed by
// [view_projection], with [params] (bias, normal offset, softness,
// strength). 1 outside the map.
float shadow_visibility(texture2DArray map, uint layer, mat4 view_projection, vec4 params, bool reversed, vec3 position, vec3 n) {
    vec4 clip = view_projection * vec4(position + n * params.y, 1.0);
    if (clip.w <= 0.0)
        return 1.0;
    vec3 ndc = clip.xyz / clip.w;
    vec2 uv  = ndc.xy * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) || ndc.z < 0.0 || ndc.z > 1.0)
        return 1.0;
    float depth  = reversed ? ndc.z + params.x : ndc.z - params.x;
    vec2 texel   = 1.0 / vec2(textureSize(sampler2DArrayShadow(map, shadow_sampler), 0).xy);
    float lit    = 0.0;
    float taps   = params.z <= 0.0 ? 0.0 : 1.0;
    float count  = 0.0;
    // Each tap is the hardware's four-texel comparison, filtered; nine of
    // them [softness] texels apart soften the edge further.
    for (float y = -taps; y <= taps; y += 1.0) {
        for (float x = -taps; x <= taps; x += 1.0) {
            vec4 at = vec4(uv + vec2(x, y) * texel * params.z, float(layer), depth);
            lit += reversed ? texture(sampler2DArrayShadow(map, shadow_sampler_reversed), at) : texture(sampler2DArrayShadow(map, shadow_sampler), at);
            count += 1.0;
        }
    }
    return mix(1.0, lit / count, params.w);
}

// The tenth of each cascade's stretch, at its far end, over which it is
// blended into the next (the last into no shadow).
const float CASCADE_BLEND = 0.1;

// How much of the sun reaches [position]: the cascade its distance along
// the view falls in, blended into the next over the last tenth of the
// cascade's stretch so no seam shows between them, the last faded out to
// no shadow over its own.
float sun_visibility(vec3 position, vec3 n) {
    uint count = min(u_lit_scene.sun_cascade_count, uint(APREND_LIT_MAX_CASCADES));
    if ((u_lit_scene.flags & APREND_LIT_SCENE_SUN_SHADOW) == 0u || count == 0u)
        return 1.0;
    float along = dot(position - u_lit_scene.eye_position.xyz, u_lit_scene.view_direction.xyz);
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
        lit = mix(lit, next, blend);
    }
    return lit;
}

vec3 point_or_spot(Surface s, AprendLitLight light) {
    vec3 to_light   = light.position_range.xyz - in_position;
    float distance2 = max(dot(to_light, to_light), 1e-8);
    float distance  = sqrt(distance2);
    float range     = light.position_range.w;
    if (distance >= range)
        return vec3(0.0);
    vec3 l       = to_light / distance;
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
                                      reversed, in_position, s.geometric);
        }
    } else {
        // A point light's shadow: the cube face the light looks along to
        // reach this fragment, by the largest of the offset's axes, then
        // that face's layer as a spot's.
        int shadow = int(light.outer_shadow.z);
        if (fall > 0.0 && shadow >= 0 && shadow < APREND_LIT_MAX_POINT_SHADOWS) {
            vec3 d    = in_position - light.position_range.xyz;
            vec3 a    = abs(d);
            uint face = a.x >= a.y && a.x >= a.z ? (d.x >= 0.0 ? 0u : 1u) : (a.y >= a.z ? (d.y >= 0.0 ? 2u : 3u) : (d.z >= 0.0 ? 4u : 5u));
            uint layer    = uint(shadow) * 6u + face;
            bool reversed = (u_lit_scene.flags & APREND_LIT_SCENE_POINT_REVERSED) != 0u;
            fall *= shadow_visibility(point_shadow_map, layer, u_lit_scene.point_shadow_view_projection[layer], u_lit_scene.point_shadow_params[shadow],
                                      reversed, in_position, s.geometric);
        }
    }
    return fall > 0.0 ? shade(s, l, light.radiance_type.rgb * fall) : vec3(0.0);
}

// What the opaque picture shows through the surface at [position]: the
// view ray bent into the surface by its index of refraction, followed
// through its thickness, and looked up where that point falls on the
// screen, blurrier (a lower mip) the rougher the surface; tinted by the
// distance travelled (Beer's law, glTF's KHR_materials_volume). Unexposed,
// as every other light here is until the end.
vec3 transmitted(Surface s, vec3 position) {
    float ior       = max(u_lit_material.transmission.y, 1.0);
    float thickness = max(u_lit_material.transmission.z, 0.0);
    vec3 inside     = refract(-s.v, s.n, 1.0 / ior);
    vec3 exit       = position + inside * thickness;
    vec4 clip       = u_lit_scene.view_projection * vec4(exit, 1.0);
    vec2 uv         = gl_FragCoord.xy * u_lit_scene.target_size.zw;
    if (clip.w > 0.0)
        uv = clip.xy / clip.w * 0.5 + 0.5; // SpudGPU's clip space: +Y down the target, as gl_FragCoord
    // three.js's choice of mip: the roughness, sharpened as the index nears 1.
    float rough = s.roughness * clamp(ior * 2.0 - 2.0, 0.0, 1.0);
    float lod   = rough * max(u_lit_scene.scene_color_mips - 1.0, 0.0);
    vec3 behind = textureLod(sampler2D(scene_color, environment_sampler), uv, lod).rgb;
    if (u_lit_scene.output_encoding == 1u) // written encoded: back to linear
        behind = mix(pow((behind + 0.055) / 1.055, vec3(2.4)), behind / 12.92, lessThanEqual(behind, vec3(0.04045)));
    behind /= max(u_lit_scene.exposure, 1e-20);
    float distance = u_lit_material.transmission.w;
    if (distance > 0.0)
        behind *= pow(max(u_lit_material.attenuation.rgb, vec3(0.0)), vec3(thickness / distance));
    return behind;
}

void main() {
    vec2 uv   = material_uv(in_uv);
    uint maps = u_lit_material.maps;

    vec4 base = u_lit_material.base_color;
    if ((maps & MAP_BASE_COLOR) != 0u)
        base *= texture(sampler2D(base_color_map, material_sampler), map_uv(uv, MAP_INDEX_BASE_COLOR));
    float opacity = clamp(base.a, 0.0, 1.0);
    if (u_lit_material.alpha_cutoff > 0.0) {
        if (opacity < u_lit_material.alpha_cutoff)
            discard;
        opacity = 1.0;
    }

    vec3 orm = vec3(1.0);
    if ((maps & MAP_ORM) != 0u)
        orm = texture(sampler2D(orm_map, material_sampler), map_uv(uv, MAP_INDEX_ORM)).rgb;
    float occlusion = mix(1.0, orm.r, clamp(u_lit_material.occlusion_strength, 0.0, 1.0));
    // The view's bent normal, if the scene reads it: 0 for none.
    vec3 bent_normal = vec3(0.0);
    if ((u_lit_scene.flags & APREND_LIT_SCENE_OCCLUSION) != 0u) {
        vec4 view_occlusion = textureLod(sampler2D(ambient_occlusion, environment_sampler), gl_FragCoord.xy * u_lit_scene.target_size.zw, 0.0);
        occlusion *= mix(1.0, view_occlusion.r, clamp(u_lit_scene.occlusion_strength, 0.0, 1.0));
        if ((u_lit_scene.flags & APREND_LIT_SCENE_BENT_NORMALS) != 0u)
            bent_normal = view_occlusion.gba * 2.0 - 1.0;
    }
    vec3 emission = max(u_lit_material.emission.rgb, vec3(0.0));
    if ((maps & MAP_EMISSION) != 0u)
        emission *= texture(sampler2D(emission_map, material_sampler), map_uv(uv, MAP_INDEX_EMISSION)).rgb;

    Surface s;
    // The geometric normal, turned to the eye: a face seen from behind is
    // lit as its front, and one with no normal faces the eye.
    s.v = safe_normalize(u_lit_scene.eye_position.xyz - in_position, safe_normalize(in_normal, vec3(0.0, 0.0, 1.0)));
    s.n = safe_normalize(in_normal, s.v);
    if (dot(s.n, s.v) < 0.0)
        s.n = -s.n;
    s.geometric = s.n;
    // The normal map, in the frame of the tangent, the bitangent its sign
    // makes (MikkTSpace's), and the normal.
    if ((maps & MAP_NORMAL) != 0u && dot(in_tangent.xyz, in_tangent.xyz) > 0.0) {
        vec3 t = in_tangent.xyz - s.n * dot(s.n, in_tangent.xyz);
        if (dot(t, t) > 0.0) {
            t       = normalize(t);
            vec3 b  = cross(s.n, t) * (in_tangent.w < 0.0 ? -1.0 : 1.0);
            vec3 tn = texture(sampler2D(normal_map, material_sampler), map_uv(uv, MAP_INDEX_NORMAL)).xyz * 2.0 - 1.0;
            tn.xy *= u_lit_material.normal_scale;
            s.n = safe_normalize(t * tn.x + b * tn.y + s.n * tn.z, s.n);
        }
    }

    float metalness       = clamp(u_lit_material.metalness * orm.b, 0.0, 1.0);
    vec3 albedo           = max(base.rgb, vec3(0.0));
    s.roughness           = clamp(u_lit_material.roughness * orm.g, MIN_ROUGHNESS, 1.0);
    s.f0                  = mix(vec3(0.08 * clamp(u_lit_material.specular, 0.0, 1.0)), albedo, metalness);
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
    s.sheen               = max(u_lit_material.sheen.rgb, vec3(0.0));
    s.sheen_roughness     = clamp(u_lit_material.sheen.a, MIN_SHEEN_ROUGHNESS, 1.0);

    vec3 color = vec3(0.0);
    vec3 sun   = u_lit_scene.sun_radiance.rgb;
    if (any(greaterThan(sun, vec3(0.0))))
        color += shade(s, u_lit_scene.sun_direction.xyz, sun * sun_visibility(in_position, s.geometric));

    if ((u_lit_scene.flags & APREND_LIT_SCENE_TILES) != 0u) {
        uvec2 tile  = min(uvec2(gl_FragCoord.xy) / max(u_lit_scene.tile_size, 1u), uvec2(u_lit_scene.tiles_x, u_lit_scene.tiles_y) - 1u);
        uint first  = (tile.y * u_lit_scene.tiles_x + tile.x) * uint(APREND_LIT_MAX_TILE_LIGHTS + 1);
        uint count  = min(tile_lights[first], uint(APREND_LIT_MAX_TILE_LIGHTS));
        for (uint i = 0u; i < count; ++i)
            color += point_or_spot(s, u_lit_scene.lights[min(tile_lights[first + 1u + i], uint(APREND_LIT_MAX_LIGHTS - 1))]);
    } else {
        for (uint i = 0u; i < min(u_lit_scene.light_count, uint(APREND_LIT_MAX_LIGHTS)); ++i)
            color += point_or_spot(s, u_lit_scene.lights[i]);
    }

    vec3 r                = reflect(-s.v, s.n);
    // Diffuse ambient light along the bent normal, where the surface is
    // open, if there is one.
    vec3 diffuse_normal   = safe_normalize(bent_normal, s.n);
    vec3 ambient_diffuse  = hemisphere(diffuse_normal);
    vec3 ambient_specular = hemisphere(r);
    vec3 r_coat           = reflect(-s.v, s.geometric);
    vec3 ambient_coat     = hemisphere(r_coat);
    if ((u_lit_scene.flags & APREND_LIT_SCENE_ENVIRONMENT) != 0u) {
        float intensity = u_lit_scene.environment_intensity;
        float top       = max(u_lit_scene.environment_mips - 1.0, 0.0);
        ambient_diffuse += intensity * textureLod(samplerCube(environment_irradiance, environment_sampler), to_environment(diffuse_normal), 0.0).rgb;
        ambient_specular += intensity * textureLod(samplerCube(environment_specular, environment_sampler), to_environment(r), s.roughness * top).rgb;
        ambient_coat += intensity * textureLod(samplerCube(environment_specular, environment_sampler), to_environment(r_coat), s.clearcoat_roughness * top).rgb;
    }
    vec2 ab         = split_sum(s.n_dot_v, s.roughness);
    vec3 ambient    = s.diffuse * ambient_diffuse + (s.f0 * ab.x + ab.y) * ambient_specular;
    ambient         = ambient * (1.0 - max3(s.sheen) * SHEEN_ALBEDO) + s.sheen * SHEEN_ALBEDO * ambient_diffuse;
    if (s.clearcoat > 0.0) {
        float c_dot_v = max(dot(s.geometric, s.v), 1e-4);
        vec2 abc      = split_sum(c_dot_v, s.clearcoat_roughness);
        float fc      = fresnel(vec3(0.04), c_dot_v).x * s.clearcoat;
        ambient       = ambient * (1.0 - fc) + (0.04 * abc.x + abc.y) * s.clearcoat * ambient_coat;
    }
    color += occlusion * ambient;
    if (transmission > 0.0)
        color += transmission * albedo * (1.0 - (s.f0 * ab.x + ab.y)) * transmitted(s, in_position);
    color += emission;

    color *= u_lit_scene.exposure;
    if (u_lit_scene.output_encoding == 1u) {
        vec3 c = clamp(color, 0.0, 1.0);
        color  = mix(1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, c * 12.92, lessThanEqual(c, vec3(0.0031308)));
    }
    out_color = vec4(color * opacity, opacity);
}
