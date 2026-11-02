module aprend_lit_shadow_pass;

// Set 0 of Aprend's shadow pass shaders (aprendlit.h,
// aprend_lit_get_shadow_layout()): the one matrix a view of a shadow is
// rendered through. A scene keeps APREND_LIT_MAX_SHADOW_VIEWS of these,
// handed out to its shadows' views
// (aprend_lit_scene_get_shadow_pass_material()).

public struct AprendLitShadowPass {
    public float4x4 view_projection;
};
[[vk::binding(0, 0)]] public ConstantBuffer<AprendLitShadowPass> u_lit_shadow_pass : register(b0, space0);
