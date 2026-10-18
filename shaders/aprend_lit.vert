#version 450
#extension GL_GOOGLE_include_directive : require

// Aprend's lit surface vertex shader (aprendlit.h,
// APREND_LIT_SHADER_SURFACE_VERTEX): a mesh of aprend_lit_vertex, placed by
// its instance's transform, seen through the view's view_projection. Hands
// the fragment shader the four outputs it reads.

#include "aprend_lit_scene.glslh"
#include "aprend_lit_vertex.glslh"

layout(location = 0) out vec3 out_position;
layout(location = 1) out vec3 out_normal;
layout(location = 2) out vec2 out_uv;
layout(location = 3) out vec4 out_tangent;

void main() {
    mat4 transform = instance_transform();
    vec4 world     = transform * vec4(in_position, 1.0);
    mat3 m         = mat3(transform);
    // The cofactor matrix: the inverse transpose times the determinant, so
    // normals stay square to their faces under any scale, with no inverse.
    // A mirror (a negative determinant) turns them, and the tangent's
    // handedness, back.
    mat3 cofactor = mat3(cross(m[1], m[2]), cross(m[2], m[0]), cross(m[0], m[1]));
    float mirror  = dot(m[0], cross(m[1], m[2])) < 0.0 ? -1.0 : 1.0;

    out_position = world.xyz;
    out_normal   = cofactor * in_normal * mirror;
    out_uv       = in_uv;
    out_tangent  = vec4(m * in_tangent.xyz, in_tangent.w * mirror);
    gl_Position  = u_lit_scene.view_projection * world;
}
