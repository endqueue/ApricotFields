module aprend_lit_vertex;

// What Aprend's lit vertex shaders take (aprendlit.h): aprend_lit_vertex at
// binding 0, locations 0 to 3, and aprend_lit_instance's transform at
// binding 1, locations 4 to 7, a column each.

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

// The four columns as a matrix: built from them as rows and transposed,
// since a matrix here is built from its rows.
public float4x4 instance_transform(AprendLitVertex input) {
    return transpose(float4x4(input.transform0, input.transform1, input.transform2, input.transform3));
}
