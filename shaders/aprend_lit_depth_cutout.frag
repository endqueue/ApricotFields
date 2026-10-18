#version 450
#extension GL_GOOGLE_include_directive : require

// Aprend's cut-out depth fragment shader (aprendlit.h,
// APREND_LIT_SHADER_DEPTH_CUTOUT_FRAGMENT): depth only, as
// aprend_lit_depth.frag, but a fragment of a cut-out material whose opacity
// is below its cutoff is discarded, so the holes in leaves or a perforated
// screen let light through. Reads the lit material at set 1 and the texture
// coordinate at location 2.

#include "aprend_lit_material.glslh"

layout(location = 2) in vec2 in_uv;

void main() {
    if (u_lit_material.alpha_cutoff <= 0.0)
        return;
    float opacity = u_lit_material.base_color.a;
    if ((u_lit_material.maps & MAP_BASE_COLOR) != 0u)
        opacity *= texture(sampler2D(base_color_map, material_sampler), material_uv(in_uv)).a;
    if (opacity < u_lit_material.alpha_cutoff)
        discard;
}
