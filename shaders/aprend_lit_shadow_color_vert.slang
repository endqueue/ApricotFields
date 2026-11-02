// Aprend's colored lit shadow vertex shader (aprendlit.h,
// APREND_LIT_SHADER_SHADOW_VERTEX_COLOR): as aprend_lit_shadow_vert.slang,
// with the bindings of aprend_lit_color_vert.slang, handing the vertex's
// color on, so a cut-out's shadow has the holes its color's alpha makes.

import aprend_lit_shadow_pass;
import aprend_lit_vertex;

[shader("vertex")]
LitVertexOutput aprend_lit_shadow_color_vert(AprendLitColorVertex input) {
    return lit_shadow_vertex(instance_transform(input), u_lit_shadow_pass.view_projection, input.position, input.uv, input.color);
}
