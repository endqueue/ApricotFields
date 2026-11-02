module aprend_lit_vertex;

// What Aprend's lit vertex shaders take (aprendlit.h): aprend_lit_vertex at
// binding 0, locations 0 to 3, and aprend_lit_instance's transform at
// binding 1, locations 4 to 7, a column each; or, for the colored shaders,
// the vertex at binding 0, its color at binding 1, location 4, and the
// transform at binding 2, locations 5 to 8. Locations follow the bindings
// in order, as a pipeline gives them.

public struct AprendLitVertex {
    [[vk::location(0)]] public float3 position : TEXCOORD0;
    [[vk::location(1)]] public float3 normal : TEXCOORD1;
    [[vk::location(2)]] public float2 uv : TEXCOORD2;
    [[vk::location(3)]] public float4 tangent : TEXCOORD3;
    [[vk::location(4)]] public float4 transform0 : TEXCOORD4;
    [[vk::location(5)]] public float4 transform1 : TEXCOORD5;
    [[vk::location(6)]] public float4 transform2 : TEXCOORD6;
    [[vk::location(7)]] public float4 transform3 : TEXCOORD7;
};

public struct AprendLitColorVertex {
    [[vk::location(0)]] public float3 position : TEXCOORD0;
    [[vk::location(1)]] public float3 normal : TEXCOORD1;
    [[vk::location(2)]] public float2 uv : TEXCOORD2;
    [[vk::location(3)]] public float4 tangent : TEXCOORD3;
    [[vk::location(4)]] public float4 color : TEXCOORD4;
    [[vk::location(5)]] public float4 transform0 : TEXCOORD5;
    [[vk::location(6)]] public float4 transform1 : TEXCOORD6;
    [[vk::location(7)]] public float4 transform2 : TEXCOORD7;
    [[vk::location(8)]] public float4 transform3 : TEXCOORD8;
};

// The four columns as a matrix: built from them as rows and transposed,
// since a matrix here is built from its rows.
public float4x4 instance_transform(AprendLitVertex input) {
    return transpose(float4x4(input.transform0, input.transform1, input.transform2, input.transform3));
}

public float4x4 instance_transform(AprendLitColorVertex input) {
    return transpose(float4x4(input.transform0, input.transform1, input.transform2, input.transform3));
}

// The lit vertex shaders' outputs, as the lit fragment shaders read them:
// the position first, then every location in order, so D3D12 matches each
// by register.
public struct LitVertexOutput {
    public float4 position : SV_Position;
    [[vk::location(0)]] public float3 world : TEXCOORD0;
    [[vk::location(1)]] public float3 normal : TEXCOORD1;
    [[vk::location(2)]] public float2 uv : TEXCOORD2;
    [[vk::location(3)]] public float4 tangent : TEXCOORD3;
    [[vk::location(4)]] public float4 color : TEXCOORD4;
};

// A surface's outputs for a vertex at [position], [normal], [uv] and
// [tangent] in its mesh's space, placed by [transform] and seen through
// [view_projection], with [color].
public LitVertexOutput lit_surface_vertex(float4x4 transform, float4x4 view_projection, float3 position, float3 normal, float2 uv, float4 tangent,
                                          float4 color) {
    LitVertexOutput output;
    float4 world = mul(transform, float4(position, 1.0));
    float3x3 m   = (float3x3)transform;
    // m's columns, as rows: GLSL's m[i] is column i, and a matrix here is
    // indexed by row.
    float3x3 columns = transpose(m);
    // The cofactor matrix: the inverse transpose times the determinant, so
    // normals stay square to their faces under any scale, with no inverse.
    // A mirror (a negative determinant) turns them, and the tangent's
    // handedness, back. Built from its columns, so transposed.
    float3x3 cofactor = transpose(float3x3(cross(columns[1], columns[2]), cross(columns[2], columns[0]), cross(columns[0], columns[1])));
    float mirror      = dot(columns[0], cross(columns[1], columns[2])) < 0.0 ? -1.0 : 1.0;

    output.world    = world.xyz;
    output.normal   = mul(cofactor, normal) * mirror;
    output.uv       = uv;
    output.tangent  = float4(mul(m, tangent.xyz), tangent.w * mirror);
    output.color    = color;
    output.position = mul(view_projection, world);
    return output;
}

// A shadow pass's outputs for the same: the position through
// [view_projection], the texture coordinate and the color, which the cut-out
// depth shader reads, and the rest 0.
public LitVertexOutput lit_shadow_vertex(float4x4 transform, float4x4 view_projection, float3 position, float2 uv, float4 color) {
    LitVertexOutput output;
    output.world    = float3(0.0);
    output.normal   = float3(0.0);
    output.uv       = uv;
    output.tangent  = float4(0.0);
    output.color    = color;
    output.position = mul(view_projection, mul(transform, float4(position, 1.0)));
    return output;
}
