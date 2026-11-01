// Aprend's BRDF lookup texture: the split sum's scale and bias (Karis, "Real
// Shading in Unreal Engine 4") for each cosine of the view (u, across) and
// roughness (v, down), GGX importance sampled with Smith's visibility for
// image-based light (k = a^2 / 2), 512 samples a texel. Made once per
// instance, the first time a lit scene is drawn with.

struct LutParams {
    float2 target_size;
};
[[vk::push_constant]] ConstantBuffer<LutParams> params : register(b0, space4);

static const float PI = 3.14159265359;

float2 hammersley(uint i, uint count) {
    return float2(float(i) / float(count), float(reversebits(i)) * 2.3283064365386963e-10);
}

float smith_ibl(float n_dot_v, float n_dot_l, float roughness) {
    float a = roughness * roughness;
    float k = a / 2.0;
    return (n_dot_v / (n_dot_v * (1.0 - k) + k)) * (n_dot_l / (n_dot_l * (1.0 - k) + k));
}

[shader("fragment")]
float4 aprend_lit_brdf_lut_frag(float4 frag_coord : SV_Position) : SV_Target0 {
    float2 at       = frag_coord.xy / params.target_size;
    float n_dot_v   = max(at.x, 1e-4);
    float roughness = max(at.y, 0.045);
    float a         = roughness * roughness;
    float3 v        = float3(sqrt(1.0 - n_dot_v * n_dot_v), 0.0, n_dot_v);
    const uint count = 512u;
    float scale = 0.0, bias = 0.0;
    for (uint i = 0u; i < count; ++i) {
        float2 xi     = hammersley(i, count);
        float phi     = 2.0 * PI * xi.x;
        float cos_t   = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
        float sin_t   = sqrt(1.0 - cos_t * cos_t);
        float3 h      = float3(sin_t * cos(phi), sin_t * sin(phi), cos_t);
        float3 l      = 2.0 * dot(v, h) * h - v;
        float n_dot_l = max(l.z, 0.0);
        float n_dot_h = max(h.z, 0.0);
        float v_dot_h = max(dot(v, h), 0.0);
        if (n_dot_l > 0.0) {
            float g     = smith_ibl(n_dot_v, n_dot_l, roughness);
            float g_vis = g * v_dot_h / max(n_dot_h * n_dot_v, 1e-6);
            float fc    = pow(1.0 - v_dot_h, 5.0);
            scale += (1.0 - fc) * g_vis;
            bias += fc * g_vis;
        }
    }
    return float4(scale / float(count), bias / float(count), 0.0, 1.0);
}
