// Aprend's lit surface vertex shader (aprendlit.h,
// APREND_LIT_SHADER_SURFACE_VERTEX): a mesh of aprend_lit_vertex, placed by
// its instance's transform, seen through the view's view_projection. Hands
// the fragment shader the five outputs it reads, the color white.

import aprend_lit_scene;
// Set 0 whole, read or not: the lit scene's layout is every binding the
// surface and sky shaders read, and a module declares its set's whole
// layout (spudgpu_shader_module_desc, "Every binding of a set").
import aprend_lit_bindings;
import aprend_lit_vertex;

[shader("vertex")]
LitVertexOutput aprend_lit_vert(AprendLitVertex input) {
    return lit_surface_vertex(instance_transform(input), u_lit_scene.view_projection, input.position, input.normal, input.uv, input.tangent,
                              float4(1.0));
}
