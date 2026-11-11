// Rend's reflecting lit surface (rbtrendlit.h,
// RBTREND_LIT_SHADER_SURFACE_REFLECT_FRAGMENT): the picture's color at target
// 0, as rbtrend_lit_frag.slang writes it; the shading normal and the
// roughness at target 1 (R16G16B16A16_FLOAT); the weight the environment's
// specular light reaches the picture with at target 2 (R8G8B8A8_UNORM, its
// alpha 0 and unread). The reflection commands read the two
// (rbtrendcommands.h: LIT_SSR, LIT_TRACE_REFLECTIONS, LIT_REFLECT).
// rbtrend_lit_surface.slang has the shading.

import rbtrend_lit_scene;
import rbtrend_lit_bindings;
import rbtrend_lit_material;
import rbtrend_lit_surface;

struct ReflectingOutput {
    float4 color : SV_Target0;
    float4 normal_roughness : SV_Target1;
    float4 specular : SV_Target2;
};

[shader("fragment")]
ReflectingOutput rbtrend_lit_surface_reflect_frag(LitFragmentInput input) {
    LitShaded shaded = lit_shade(input);
    ReflectingOutput output;
    output.color            = shaded.color;
    output.normal_roughness = float4(shaded.normal, shaded.roughness);
    output.specular         = float4(shaded.specular, 0.0);
    return output;
}
