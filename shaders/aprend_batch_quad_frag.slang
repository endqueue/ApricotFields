// The batcher's quads (aprendbatch.h): the color, times the frame's texture
// where the quad is textured.

import aprend_batch;
import aprend_batch_inputs;

[shader("fragment")]
float4 aprend_batch_quad_frag(BatchFragmentInput input) : SV_Target0 {
    float4 c = input.color;
    if ((input.flags.x & QUAD_TEXTURED) != 0u)
        c *= batch_texture.Sample(batch_sampler, input.local);
    return batch_premultiply(c, 1.0);
}
