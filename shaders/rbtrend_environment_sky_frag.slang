// A face of an environment's radiance cube from Preetham, Shirley and
// Smits' analytic daylight ("A Practical Analytic Model for Daylight",
// 1999), the sky Hazel's PreethamSky.glsl is too: Perez's distribution of
// luminance and chromaticity over the sky, from the sun's height and the
// air's turbidity, in Yxy, scaled so the zenith reads the intensity, and
// converted to linear sRGB. Below the horizon, the horizon's light times
// the ground's albedo. The sun's disc is left out: the sun is a scene's own
// light, and a disc in the picture would only be blurred into the
// prefiltered mips. In the environment's own space, +Y up.

// RbtrendSkyParams in rbtrendlit_internal.hpp.
struct SkyParams {
    float2 target_size;
    uint face;
    float turbidity;
    float4 sun;    // xyz: towards the sun, unit. w: the zenith's radiance
    float4 ground; // rgb: the ground's albedo
};
[[vk::push_constant]] ConstantBuffer<SkyParams> params : register(b0, space4);

static const float PI = 3.14159265359;

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

// Perez's function for the coefficients [a] to [e] at view zenith angle
// [theta] and angle to the sun [gamma].
float perez(float theta, float gamma, float a, float b, float c, float d, float e) {
    return (1.0 + a * exp(b / max(cos(theta), 0.01))) * (1.0 + c * exp(d * gamma) + e * cos(gamma) * cos(gamma));
}

float3 sky(float3 v, float3 sun) {
    float T       = params.turbidity;
    float theta_s = acos(clamp(sun.y, -1.0, 1.0));
    float theta   = acos(clamp(v.y, 0.0, 1.0));
    float gamma   = acos(clamp(dot(v, sun), -1.0, 1.0));

    float chi = (4.0 / 9.0 - T / 120.0) * (PI - 2.0 * theta_s);
    float Yz  = (4.0453 * T - 4.9710) * tan(chi) - 0.2155 * T + 2.4192;
    float t2 = theta_s * theta_s, t3 = t2 * theta_s;
    float xz = T * T * (0.00166 * t3 - 0.00375 * t2 + 0.00209 * theta_s) + T * (-0.02903 * t3 + 0.06377 * t2 - 0.03202 * theta_s + 0.00394) +
               (0.11693 * t3 - 0.21196 * t2 + 0.06052 * theta_s + 0.25886);
    float yz = T * T * (0.00275 * t3 - 0.00610 * t2 + 0.00317 * theta_s) + T * (-0.04214 * t3 + 0.08970 * t2 - 0.04153 * theta_s + 0.00516) +
               (0.15346 * t3 - 0.26756 * t2 + 0.06670 * theta_s + 0.26688);

    float Y = Yz * perez(theta, gamma, 0.1787 * T - 1.4630, -0.3554 * T + 0.4275, -0.0227 * T + 5.3251, 0.1206 * T - 2.5771, -0.0670 * T + 0.3703) /
              perez(0.0, theta_s, 0.1787 * T - 1.4630, -0.3554 * T + 0.4275, -0.0227 * T + 5.3251, 0.1206 * T - 2.5771, -0.0670 * T + 0.3703);
    float x = xz * perez(theta, gamma, -0.0193 * T - 0.2592, -0.0665 * T + 0.0008, -0.0004 * T + 0.2125, -0.0641 * T - 0.8989, -0.0033 * T + 0.0452) /
              perez(0.0, theta_s, -0.0193 * T - 0.2592, -0.0665 * T + 0.0008, -0.0004 * T + 0.2125, -0.0641 * T - 0.8989, -0.0033 * T + 0.0452);
    float y = yz * perez(theta, gamma, -0.0167 * T - 0.2608, -0.0950 * T + 0.0092, -0.0079 * T + 0.2102, -0.0441 * T - 1.6537, -0.0109 * T + 0.0529) /
              perez(0.0, theta_s, -0.0167 * T - 0.2608, -0.0950 * T + 0.0092, -0.0079 * T + 0.2102, -0.0441 * T - 1.6537, -0.0109 * T + 0.0529);

    // Relative to the zenith, then Yxy to XYZ to linear sRGB.
    float rel  = Y / max(Yz, 1e-4);
    float3 xyz = float3(x / max(y, 1e-4) * rel, rel, (1.0 - x - y) / max(y, 1e-4) * rel);
    // Its columns, in the order GLSL's mat3 took them: transposed, since a
    // matrix here is built from its rows.
    const float3x3 to_srgb = transpose(float3x3(3.2404542, -0.9692660, 0.0556434, -1.5371385, 1.8760108, -0.2040259, -0.4985314, 0.0415560, 1.0572252));
    return max(mul(to_srgb, xyz), float3(0.0));
}

[shader("fragment")]
float4 rbtrend_environment_sky_frag(float4 frag_coord : SV_Position) : SV_Target0 {
    float3 v   = cube_direction(frag_coord.xy, params.face);
    float3 sun = normalize(params.sun.xyz);
    float3 c;
    if (v.y >= 0.0)
        c = sky(v, sun);
    else
        c = sky(normalize(float3(v.x, 0.0, v.z) + float3(0.0, 1e-3, 0.0)), sun) * params.ground.rgb;
    return float4(c * params.sun.w, 1.0);
}
