// Rend's lit surface (rbtrendlit.h, RBTREND_LIT_SHADER_SURFACE_FRAGMENT):
// the picture's color alone. rbtrend_lit_surface.slang has the shading.

import rbtrend_lit_scene;
import rbtrend_lit_bindings;
import rbtrend_lit_material;
import rbtrend_lit_surface;

[shader("fragment")]
float4 rbtrend_lit_frag(LitFragmentInput input) : SV_Target0 {
    return lit_shade(input).color;
}
