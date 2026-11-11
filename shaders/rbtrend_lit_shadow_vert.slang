// Rend's lit shadow vertex shader (rbtrendlit.h,
// RBTREND_LIT_SHADER_SHADOW_VERTEX): the same meshes as rbtrend_lit_vert.slang,
// through the shadow pass's matrix, for the pass that renders a view of a
// shadow. Writes every location the lit fragment shaders read, the texture
// coordinate and the color (white) for a cut-out
// (RBTREND_LIT_SHADER_DEPTH_CUTOUT_FRAGMENT), the rest 0: D3D12 matches
// stages by register in order.

import rbtrend_lit_shadow_pass;
import rbtrend_lit_vertex;

[shader("vertex")]
LitVertexOutput rbtrend_lit_shadow_vert(RbtrendLitVertex input) {
    return lit_shadow_vertex(instance_transform(input), u_lit_shadow_pass.view_projection, input.position, input.uv, float4(1.0));
}
