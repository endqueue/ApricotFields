module aprend_environment;

// What Aprend's environment passes share (APREND_COMMAND_LIT_ENVIRONMENT):
// the direction a texel of a cube face looks along, their push constants,
// and Hammersley's low-discrepancy points.

// AprendEnvironmentParams in aprendbuiltin_internal.hpp.
public struct EnvironmentParams {
    public float2 target_size; // the face being rendered, in texels
    public uint face;          // 0 to 5: +X, -X, +Y, -Y, +Z, -Z
    public uint sample_count;
    public float roughness;    // the prefilter's
    public float source_size;  // a face of the source's first mip, in texels
    public float source_mips;  // the source's mip count
    // To a multiple of 8 bytes, as aprend_tone_map_frag.slang explains.
    public float padding;
};
[[vk::push_constant]] public ConstantBuffer<EnvironmentParams> params : register(b0, space4);

public static const float PI = 3.14159265359;

// The direction through texel [frag] (SV_Position.xy, the first row at the
// top) of face [face], as Vulkan, D3D and Metal pick a cube's face and its
// coordinates from a direction.
public float3 cube_direction(float2 frag, uint face) {
    float2 st = frag / params.target_size * 2.0 - 1.0;
    float s = st.x, t = st.y;
    float3 d;
    if (face == 0u)      d = float3(1.0, -t, -s);
    else if (face == 1u) d = float3(-1.0, -t, s);
    else if (face == 2u) d = float3(s, 1.0, t);
    else if (face == 3u) d = float3(s, -1.0, -t);
    else if (face == 4u) d = float3(s, -t, 1.0);
    else                 d = float3(-s, -t, -1.0);
    return normalize(d);
}

public float2 hammersley(uint i, uint count) {
    return float2(float(i) / float(count), float(reversebits(i)) * 2.3283064365386963e-10);
}

// A frame around [n]: tangent, bitangent, n, its columns (built as rows
// and transposed, since a matrix here is built from its rows).
public float3x3 frame(float3 n) {
    float3 up = abs(n.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 t  = normalize(cross(up, n));
    return transpose(float3x3(t, cross(n, t), n));
}
