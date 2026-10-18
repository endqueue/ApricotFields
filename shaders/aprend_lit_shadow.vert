#version 450
#extension GL_GOOGLE_include_directive : require

// Aprend's lit shadow vertex shader (aprendlit.h,
// APREND_LIT_SHADER_SHADOW_VERTEX): the same meshes as aprend_lit.vert,
// through the shadow pass's matrix, for the pass that renders a sun
// cascade's or a spot's depth. Hands the texture coordinate on, for a cut-out
// (APREND_LIT_SHADER_DEPTH_CUTOUT_FRAGMENT).

#include "aprend_lit_shadow_pass.glslh"
#include "aprend_lit_vertex.glslh"

layout(location = 2) out vec2 out_uv;

void main() {
    out_uv      = in_uv;
    gl_Position = u_lit_shadow_pass.view_projection * (instance_transform() * vec4(in_position, 1.0));
}
