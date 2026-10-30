#version 450

// The batcher's quads (aprendbatch.h): the color, times the frame's texture
// where the quad is textured.

#include "aprend_batch.glslh"
#include "aprend_batch_inputs.glslh"

void main() {
    vec4 c = in_color;
    if ((in_flags.x & QUAD_TEXTURED) != 0u)
        c *= texture(sampler2D(batch_texture, batch_sampler), in_local);
    out_color = batch_premultiply(c, 1.0);
}
