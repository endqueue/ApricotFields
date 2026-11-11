// Rend's colored lit surface vertex shader (rbtrendlit.h,
// RBTREND_LIT_SHADER_SURFACE_VERTEX_COLOR): as rbtrend_lit_vert.slang, with a
// color per vertex at binding 1 handed on at location 4, and the instance's
// transform at binding 2.

import rbtrend_lit_scene;
// Set 0 whole, as rbtrend_lit_vert.slang declares it.
import rbtrend_lit_bindings;
import rbtrend_lit_vertex;

[shader("vertex")]
LitVertexOutput rbtrend_lit_color_vert(RbtrendLitColorVertex input) {
    return lit_surface_vertex(instance_transform(input), u_lit_scene.view_projection, input.position, input.normal, input.uv, input.tangent,
                              input.color);
}
