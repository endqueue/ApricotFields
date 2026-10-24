#version 450

// A face of an environment's radiance cube from Hosek and Wilkie's analytic
// sky ("An Analytic Model for Full Spectral Sky-Dome Radiance", 2012), in
// its native RGB: per channel, the extended Perez function
//
//   F(theta, gamma) = (1 + A exp(B / (cos theta + 0.01)))
//                   * (C + D exp(E gamma) + F cos^2 gamma + G chi(H, gamma) + I sqrt(cos theta))
//   chi(g, a)       = (1 + cos^2 a) / (1 + g^2 - 2 g cos a)^(3/2)
//
// times the channel's radiance, at view zenith angle theta and angle to the
// sun gamma. The coefficients are in the order of the authors' code and
// dataset (ArHosekSkyModel_GetRadianceInternal), which keeps the paper's I
// at 7 and H at 8. The nine coefficients and the radiance of each channel are
// cooked on the CPU for the sun's height, the air's turbidity and the
// ground's albedo (aprendlit.cpp, from the authors' dataset), the radiance
// scaled so the zenith's luminance reads the intensity, as Preetham's sky
// (aprend_environment_sky.frag) does. Below the horizon, the horizon's light
// times the ground's albedo. The sun's disc is left out, as there. In the
// environment's own space, +Y up.

// AprendSkyParams in aprendlit_internal.hpp, shared with the Preetham pass:
// this one reads the size, the face, the sun's direction and the ground.
layout(push_constant) uniform SkyParams {
    vec2 target_size;
    uint face;
    float turbidity;
    vec4 sun;    // xyz: towards the sun, unit
    vec4 ground; // rgb: the ground's albedo
} params;

// aprend_hosek_sky_block in aprendlit_internal.hpp.
layout(set = 0, binding = 0) uniform AprendHosekSky {
    vec4 config[9]; // rgb: each channel's coefficients, in the dataset's order
    vec4 radiance;  // rgb: each channel's radiance, times the zenith's scale
} u_sky;

layout(location = 0) out vec4 out_color;

vec3 cube_direction(vec2 frag, uint face) {
    vec2 st = frag / params.target_size * 2.0 - 1.0;
    float s = st.x, t = st.y;
    vec3 d;
    if (face == 0u)      d = vec3(1.0, -t, -s);
    else if (face == 1u) d = vec3(-1.0, -t, s);
    else if (face == 2u) d = vec3(s, 1.0, t);
    else if (face == 3u) d = vec3(s, -1.0, -t);
    else if (face == 4u) d = vec3(s, -t, 1.0);
    else                 d = vec3(-s, -t, -1.0);
    return normalize(d);
}

vec3 sky(vec3 v, vec3 sun) {
    float cos_theta = clamp(v.y, 0.0, 1.0);
    float cos_gamma = clamp(dot(v, sun), -1.0, 1.0);
    float gamma     = acos(cos_gamma);
    // The paper's A to G, then I (the zenith term) and H (chi's g).
    vec3 A = u_sky.config[0].rgb, B = u_sky.config[1].rgb, C = u_sky.config[2].rgb;
    vec3 D = u_sky.config[3].rgb, E = u_sky.config[4].rgb, F = u_sky.config[5].rgb;
    vec3 G = u_sky.config[6].rgb, I = u_sky.config[7].rgb, H = u_sky.config[8].rgb;
    vec3 chi = (1.0 + cos_gamma * cos_gamma) / pow(1.0 + H * H - 2.0 * H * cos_gamma, vec3(1.5));
    vec3 f   = (1.0 + A * exp(B / (cos_theta + 0.01))) *
             (C + D * exp(E * gamma) + F * (cos_gamma * cos_gamma) + G * chi + I * sqrt(cos_theta));
    return max(f * u_sky.radiance.rgb, vec3(0.0));
}

void main() {
    vec3 v   = cube_direction(gl_FragCoord.xy, params.face);
    vec3 sun = normalize(params.sun.xyz);
    vec3 c;
    if (v.y >= 0.0)
        c = sky(v, sun);
    else
        c = sky(normalize(vec3(v.x, 0.0, v.z) + vec3(0.0, 1e-3, 0.0)), sun) * params.ground.rgb;
    out_color = vec4(c, 1.0);
}
