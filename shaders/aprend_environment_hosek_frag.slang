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
// (aprend_environment_sky_frag.slang) does. Below the horizon, the horizon's
// light times the ground's albedo. The sun's disc is left out, as there. In
// the environment's own space, +Y up.

// AprendSkyParams in aprendlit_internal.hpp, shared with the Preetham pass:
// this one reads the size, the face, the sun's direction and the ground.
struct SkyParams {
    float2 target_size;
    uint face;
    float turbidity;
    float4 sun;    // xyz: towards the sun, unit
    float4 ground; // rgb: the ground's albedo
};
[[vk::push_constant]] ConstantBuffer<SkyParams> params : register(b0, space4);

// aprend_hosek_sky_block in aprendlit_internal.hpp.
struct AprendHosekSky {
    float4 config[9]; // rgb: each channel's coefficients, in the dataset's order
    float4 radiance;  // rgb: each channel's radiance, times the zenith's scale
};
[[vk::binding(0, 0)]] ConstantBuffer<AprendHosekSky> u_sky : register(b0, space0);

float3 cube_direction(float2 frag, uint face) {
    float2 st = frag / params.target_size * 2.0 - 1.0;
    float s = st.x, t = st.y;
    float3 d;
    if (face == 0u)      d = float3(1.0, -t, -s);
    else if (face == 1u) d = float3(-1.0, -t, s);
    else if (face == 2u) d = float3(s, 1.0, t);
    else if (face == 3u) d = float3(s, -1.0, -t);
    else if (face == 4u) d = float3(s, -t, 1.0);
    else                 d = float3(-s, -t, -1.0);
    return normalize(d);
}

float3 sky(float3 v, float3 sun) {
    float cos_theta = clamp(v.y, 0.0, 1.0);
    float cos_gamma = clamp(dot(v, sun), -1.0, 1.0);
    float gamma     = acos(cos_gamma);
    // The paper's A to G, then I (the zenith term) and H (chi's g).
    float3 A = u_sky.config[0].rgb, B = u_sky.config[1].rgb, C = u_sky.config[2].rgb;
    float3 D = u_sky.config[3].rgb, E = u_sky.config[4].rgb, F = u_sky.config[5].rgb;
    float3 G = u_sky.config[6].rgb, I = u_sky.config[7].rgb, H = u_sky.config[8].rgb;
    float3 chi = (1.0 + cos_gamma * cos_gamma) / pow(1.0 + H * H - 2.0 * H * cos_gamma, float3(1.5));
    float3 f   = (1.0 + A * exp(B / (cos_theta + 0.01))) *
                 (C + D * exp(E * gamma) + F * (cos_gamma * cos_gamma) + G * chi + I * sqrt(cos_theta));
    return max(f * u_sky.radiance.rgb, float3(0.0));
}

[shader("fragment")]
float4 aprend_environment_hosek_frag(float4 frag_coord : SV_Position) : SV_Target0 {
    float3 v   = cube_direction(frag_coord.xy, params.face);
    float3 sun = normalize(params.sun.xyz);
    float3 c;
    if (v.y >= 0.0)
        c = sky(v, sun);
    else
        c = sky(normalize(float3(v.x, 0.0, v.z) + float3(0.0, 1e-3, 0.0)), sun) * params.ground.rgb;
    return float4(c, 1.0);
}
