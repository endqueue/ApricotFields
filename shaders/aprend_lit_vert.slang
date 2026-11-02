// Aprend's lit surface vertex shader (aprendlit.h,
// APREND_LIT_SHADER_SURFACE_VERTEX): a mesh of aprend_lit_vertex, placed by
// its instance's transform, seen through the view's view_projection. Hands
// the fragment shader the four outputs it reads.

import aprend_lit_scene;
// Set 0 whole, read or not: the lit scene's layout is every binding the
// surface and sky shaders read, and a module declares its set's whole
// layout (spudgpu_shader_module_desc, "Every binding of a set").
import aprend_lit_bindings;
import aprend_lit_vertex;

// The position first, then every location in order, as the lit fragment
// shaders read them, so D3D12 matches each by register.
struct LitVertexOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float3 world : TEXCOORD0;
    [[vk::location(1)]] float3 normal : TEXCOORD1;
    [[vk::location(2)]] float2 uv : TEXCOORD2;
    [[vk::location(3)]] float4 tangent : TEXCOORD3;
};

[shader("vertex")]
LitVertexOutput aprend_lit_vert(AprendLitVertex input) {
    LitVertexOutput output;
    float4x4 transform = instance_transform(input);
    float4 world       = mul(transform, float4(input.position, 1.0));
    float3x3 m         = (float3x3)transform;
    // m's columns, as rows: GLSL's m[i] is column i, and a matrix here is
    // indexed by row.
    float3x3 columns   = transpose(m);
    // The cofactor matrix: the inverse transpose times the determinant, so
    // normals stay square to their faces under any scale, with no inverse.
    // A mirror (a negative determinant) turns them, and the tangent's
    // handedness, back. Built from its columns, so transposed.
    float3x3 cofactor = transpose(float3x3(cross(columns[1], columns[2]), cross(columns[2], columns[0]), cross(columns[0], columns[1])));
    float mirror      = dot(columns[0], cross(columns[1], columns[2])) < 0.0 ? -1.0 : 1.0;

    output.world    = world.xyz;
    output.normal   = mul(cofactor, input.normal) * mirror;
    output.uv       = input.uv;
    output.tangent  = float4(mul(m, input.tangent.xyz), input.tangent.w * mirror);
    output.position = mul(u_lit_scene.view_projection, world);
    return output;
}
