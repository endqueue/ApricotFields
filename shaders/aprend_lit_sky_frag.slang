// Aprend's sky (aprendlit.h, APREND_LIT_SHADER_SKY_FRAGMENT): the scene's
// environment as a background, for a pass over the whole target with
// aprend_get_fullscreen_vertex_shader(), drawn before the surfaces: each
// texel looks along its ray through the view's inverse view_projection, and
// reads the environment's picture there, times its intensity and the
// exposure, written out as the surface is. Black with no environment. Over
// it, with or without one, the disc of each directional light that asks for
// one (its radius, as 1 - cos, above 0): its radiance spread over the disc's
// solid angle, so it carries the light it lights surfaces with (pi times
// radiance, as the surface scales light), its edge softened over the change
// in angle across a texel.

import aprend_lit_scene;
import aprend_lit_bindings;

float3 unproject(float2 ndc, float depth) {
    float4 p = mul(u_lit_scene.inverse_view_projection, float4(ndc, depth, 1.0));
    return p.xyz / p.w;
}

[shader("fragment")]
float4 aprend_lit_sky_frag(float4 frag_coord : SV_Position) : SV_Target0 {
    // The texel, the first row at the top, in clip space, whose +Y is up
    // the target: y negated.
    float2 ndc       = frag_coord.xy * u_lit_scene.target_size.zw * 2.0 - 1.0;
    ndc.y            = -ndc.y;
    float3 direction = normalize(unproject(ndc, 1.0) - unproject(ndc, 0.0));
    // Near at 0 or at 1: either way the ray runs away from the eye.
    if (dot(direction, u_lit_scene.view_direction.xyz) < 0.0)
        direction = -direction;
    float3 color = float3(0.0);
    if ((u_lit_scene.flags & APREND_LIT_SCENE_ENVIRONMENT) != 0u) {
        float3 d = float3(dot(u_lit_scene.environment_rotation[0].xyz, direction), dot(u_lit_scene.environment_rotation[1].xyz, direction),
                          dot(u_lit_scene.environment_rotation[2].xyz, direction));
        color    = u_lit_scene.environment_intensity * environment_specular.SampleLevel(environment_sampler, d, 0.0).rgb;
    }
    for (uint i = 0u; i < min(u_lit_scene.directional_count, uint(APREND_LIT_MAX_DIRECTIONAL_LIGHTS)); ++i) {
        AprendLitDirectional d = u_lit_scene.directional[i];
        float versine          = d.direction_versine.w;
        // How far off the disc's centre the ray is, as 1 - cos, beside the
        // disc's: the two are compared where they are small without the
        // cancellation a cos near 1 has. Worked out for every light, so
        // fwidth sees every texel of a quad.
        float3 off   = direction - d.direction_versine.xyz;
        float away   = 0.5 * dot(off, off);
        float texel  = max(fwidth(away), 1e-9);
        if (d.radiance_disc.w > 0.5 && versine > 0.0) {
            float inside = clamp((versine - away) / texel + 0.5, 0.0, 1.0);
            float solid  = 2.0 * 3.14159265359 * versine;
            color += inside * 3.14159265359 / solid * d.radiance_disc.rgb;
        }
    }
    color *= u_lit_scene.exposure;
    if (u_lit_scene.output_encoding == 1u) {
        float3 c = clamp(color, 0.0, 1.0);
        color    = select(c <= float3(0.0031308), c * 12.92, 1.055 * pow(c, float3(1.0 / 2.4)) - 0.055);
    }
    return float4(color, 1.0);
}
