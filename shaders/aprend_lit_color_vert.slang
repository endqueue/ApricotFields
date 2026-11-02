// Aprend's colored lit surface vertex shader (aprendlit.h,
// APREND_LIT_SHADER_SURFACE_VERTEX_COLOR): as aprend_lit_vert.slang, with a
// color per vertex at binding 1 handed on at location 4, and the instance's
// transform at binding 2.

import aprend_lit_scene;
// Set 0 whole, as aprend_lit_vert.slang declares it.
import aprend_lit_bindings;
import aprend_lit_vertex;

[shader("vertex")]
LitVertexOutput aprend_lit_color_vert(AprendLitColorVertex input) {
    return lit_surface_vertex(instance_transform(input), u_lit_scene.view_projection, input.position, input.normal, input.uv, input.tangent,
                              input.color);
}
