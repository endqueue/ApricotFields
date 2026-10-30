#version 450

// The batcher's glyphs (aprendbatch.h): a multi-channel signed distance
// field (MTSDF) from the frame's atlas, the median of RGB for the edge and
// the true distance in alpha for the outline, each scaled to screen pixels
// by how many the atlas's distance range covers here (msdfgen's
// screenPxRange), so the edge is a pixel wide at every size. The range is
// the frame's, never a constant here. Below a pixel of range (strong
// minification) the coverage fades rather than aliasing.

#include "aprend_batch.glslh"
#include "aprend_batch_inputs.glslh"

float median3(vec3 v) {
    return max(min(v.r, v.g), min(max(v.r, v.g), v.b));
}

void main() {
    vec4 field      = texture(sampler2DArray(batch_atlas, batch_sampler), vec3(in_local, float(in_flags.y)));
    vec2 unit_range = vec2(u_batch.params.y) / vec2(textureSize(sampler2DArray(batch_atlas, batch_sampler), 0).xy);
    vec2 screen     = vec2(1.0) / max(fwidth(in_local), vec2(1e-9));
    float range     = 0.5 * dot(unit_range, screen);
    float fade      = clamp(range, 0.0, 1.0);
    range           = max(range, 1.0);
    float fill      = clamp(range * (median3(field.rgb) - 0.5) + 0.5, 0.0, 1.0) * fade;
    vec4 c          = batch_premultiply(in_color, fill);
    float width     = clamp(in_extra, 0.0, 1.0);
    if (width > 0.0) {
        float edge    = 0.5 - 0.5 * width;
        float outline = clamp(range * (field.a - edge) + 0.5, 0.0, 1.0) * fade;
        c += batch_premultiply(in_d1, outline) * (1.0 - c.a);
    }
    out_color = c;
}
