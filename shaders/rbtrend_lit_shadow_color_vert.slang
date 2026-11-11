// Rend's colored lit shadow vertex shader (rbtrendlit.h,
// RBTREND_LIT_SHADER_SHADOW_VERTEX_COLOR): as rbtrend_lit_shadow_vert.slang,
// with the bindings of rbtrend_lit_color_vert.slang, handing the vertex's
// color on, so a cut-out's shadow has the holes its color's alpha makes.

import rbtrend_lit_shadow_pass;
import rbtrend_lit_vertex;

[shader("vertex")]
LitVertexOutput rbtrend_lit_shadow_color_vert(RbtrendLitColorVertex input) {
    return lit_shadow_vertex(instance_transform(input), u_lit_shadow_pass.view_projection, input.position, input.uv, input.color);
}
