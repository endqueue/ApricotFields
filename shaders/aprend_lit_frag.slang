// Aprend's lit surface (aprendlit.h, APREND_LIT_SHADER_SURFACE_FRAGMENT):
// the picture's color alone. aprend_lit_surface.slang has the shading.

import aprend_lit_scene;
import aprend_lit_bindings;
import aprend_lit_material;
import aprend_lit_surface;

[shader("fragment")]
float4 aprend_lit_frag(LitFragmentInput input) : SV_Target0 {
    return lit_shade(input).color;
}
