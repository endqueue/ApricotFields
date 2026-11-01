module aprend_lit_shadow_pass;

// Set 0 of Aprend's shadow pass shaders (aprendlit.h,
// aprend_lit_get_shadow_layout()): the one matrix a shadow map is rendered
// through. A scene keeps one of these for each sun cascade and each spot
// shadow (aprend_lit_scene_get_sun_shadow_material(),
// aprend_lit_scene_get_spot_shadow_material()).

public struct AprendLitShadowPass {
    public float4x4 view_projection;
};
[[vk::binding(0, 0)]] public ConstantBuffer<AprendLitShadowPass> u_lit_shadow_pass : register(b0, space0);
