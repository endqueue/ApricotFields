module aprend_batch_inputs;

// What aprend_batch_vert.slang hands each of the batcher's fragment shaders,
// and their set 0: the frame's texture, its atlas and one sampler. Each
// fragment shader returns its color at SV_Target0.

// Every output of the vertex shader, in its order, the position first, so
// D3D12 matches each by register.
public struct BatchFragmentInput {
    public float4 position : SV_Position;
    [[vk::location(0)]] public float2 local : TEXCOORD0;
    [[vk::location(1)]] public noperspective float2 pixel : TEXCOORD1;
    [[vk::location(2)]] public nointerpolation float4 color : TEXCOORD2;
    [[vk::location(3)]] public nointerpolation float4 a : TEXCOORD3;
    [[vk::location(4)]] public nointerpolation float4 b : TEXCOORD4;
    [[vk::location(5)]] public nointerpolation float4 c : TEXCOORD5;
    [[vk::location(6)]] public nointerpolation float4 d0 : TEXCOORD6;
    [[vk::location(7)]] public nointerpolation float4 d1 : TEXCOORD7;
    [[vk::location(8)]] public nointerpolation uint2 flags : TEXCOORD8;
    [[vk::location(9)]] public nointerpolation float extra : TEXCOORD9;
};

[[vk::binding(0, 0)]] public Texture2D<float4> batch_texture : register(t0, space0);
[[vk::binding(1, 0)]] public Texture2DArray<float4> batch_atlas : register(t1, space0);
[[vk::binding(2, 0)]] public SamplerState batch_sampler : register(s2, space0);
