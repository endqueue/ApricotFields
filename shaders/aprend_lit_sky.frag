#version 450
#extension GL_GOOGLE_include_directive : require

// Aprend's sky (aprendlit.h, APREND_LIT_SHADER_SKY_FRAGMENT): the scene's
// environment as a background, for a pass over the whole target with
// aprend_get_fullscreen_vertex_shader(), drawn before the surfaces: each
// texel looks along its ray through the view's inverse view_projection, and
// reads the environment's picture there, times its intensity and the
// exposure, written out as the surface is. Black with no environment. The
// sun's disc over it, with or without one: the sun's radiance spread over
// the disc's solid angle, so it carries the light the sun lights surfaces
// with (pi times radiance, as the surface scales light), its edge
// softened over the change in angle across a texel.

#include "aprend_lit_scene.glslh"
#include "aprend_lit_bindings.glslh"

layout(location = 0) out vec4 out_color;

vec3 unproject(vec2 ndc, float depth) {
    vec4 p = u_lit_scene.inverse_view_projection * vec4(ndc, depth, 1.0);
    return p.xyz / p.w;
}

void main() {
    vec2 ndc       = gl_FragCoord.xy * u_lit_scene.target_size.zw * 2.0 - 1.0;
    vec3 direction = normalize(unproject(ndc, 1.0) - unproject(ndc, 0.0));
    // Near at 0 or at 1: either way the ray runs away from the eye.
    if (dot(direction, u_lit_scene.view_direction.xyz) < 0.0)
        direction = -direction;
    vec3 color = vec3(0.0);
    if ((u_lit_scene.flags & APREND_LIT_SCENE_ENVIRONMENT) != 0u) {
        vec3 d = vec3(dot(u_lit_scene.environment_rotation[0].xyz, direction), dot(u_lit_scene.environment_rotation[1].xyz, direction),
                      dot(u_lit_scene.environment_rotation[2].xyz, direction));
        color  = u_lit_scene.environment_intensity * textureLod(samplerCube(environment_specular, environment_sampler), d, 0.0).rgb;
    }
    float versine = u_lit_scene.sun_radiance.w;
    if (versine > 0.0) {
        // How far off the sun's centre the ray is, as 1 - cos, beside the
        // disc's: the two are compared where they are small without the
        // cancellation a cos near 1 has.
        vec3 off      = direction - u_lit_scene.sun_direction.xyz;
        float away    = 0.5 * dot(off, off);
        float texel   = max(fwidth(away), 1e-9);
        float inside  = clamp((versine - away) / texel + 0.5, 0.0, 1.0);
        float solid   = 2.0 * 3.14159265359 * versine;
        color += inside * 3.14159265359 / solid * u_lit_scene.sun_radiance.rgb;
    }
    color *= u_lit_scene.exposure;
    if (u_lit_scene.output_encoding == 1u) {
        vec3 c = clamp(color, 0.0, 1.0);
        color  = mix(1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, c * 12.92, lessThanEqual(c, vec3(0.0031308)));
    }
    out_color = vec4(color, 1.0);
}
