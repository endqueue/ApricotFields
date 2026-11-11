// The batcher's glyphs (rbtrendbatch.h): a multi-channel signed distance
// field (MTSDF) from the frame's atlas, the median of RGB for the edge and
// the true distance in alpha for the outline, each scaled to screen pixels
// by how many the atlas's distance range covers here (msdfgen's
// screenPxRange), so the edge is a pixel wide at every size. The range is
// the frame's, never a constant here. Below a pixel of range (strong
// minification) the coverage fades rather than aliasing.

import rbtrend_batch;
import rbtrend_batch_inputs;

float median3(float3 v) {
    return max(min(v.r, v.g), min(max(v.r, v.g), v.b));
}

[shader("fragment")]
float4 rbtrend_batch_glyph_frag(BatchFragmentInput input) : SV_Target0 {
    uint atlas_width, atlas_height, atlas_layers, atlas_levels;
    batch_atlas.GetDimensions(0, atlas_width, atlas_height, atlas_layers, atlas_levels);
    float4 field      = batch_atlas.Sample(batch_sampler, float3(input.local, float(input.flags.y)));
    float2 unit_range = float2(u_batch.params.y) / float2(float(atlas_width), float(atlas_height));
    float2 screen     = float2(1.0) / max(fwidth(input.local), float2(1e-9));
    float range       = 0.5 * dot(unit_range, screen);
    float fade        = clamp(range, 0.0, 1.0);
    range             = max(range, 1.0);
    float fill        = clamp(range * (median3(field.rgb) - 0.5) + 0.5, 0.0, 1.0) * fade;
    float4 c          = batch_premultiply(input.color, fill);
    float width       = clamp(input.extra, 0.0, 1.0);
    if (width > 0.0) {
        float edge    = 0.5 - 0.5 * width;
        float outline = clamp(range * (field.a - edge) + 0.5, 0.0, 1.0) * fade;
        c += batch_premultiply(input.d1, outline) * (1.0 - c.a);
    }
    return c;
}
