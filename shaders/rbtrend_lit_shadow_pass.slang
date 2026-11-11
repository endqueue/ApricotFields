module rbtrend_lit_shadow_pass;

// Set 0 of Rend's shadow pass shaders (rbtrendlit.h,
// rbtrend_lit_get_shadow_layout()): the one matrix a view of a shadow is
// rendered through. A scene keeps RBTREND_LIT_MAX_SHADOW_VIEWS of these,
// handed out to its shadows' views
// (rbtrend_lit_scene_get_shadow_pass_material()).

public struct RbtrendLitShadowPass {
    public float4x4 view_projection;
};
[[vk::binding(0, 0)]] public ConstantBuffer<RbtrendLitShadowPass> u_lit_shadow_pass : register(b0, space0);
