// Aprend's tiled light culling (aprendlit.h), each tile over the whole depth
// of the view: aprend_lit_cull.slang, without depth bounds. Recorded at the
// top of a list that binds a scene with a target size.

import aprend_lit_cull;

// One invocation a tile.
[shader("compute")]
[numthreads(64, 1, 1)]
void aprend_lit_cull_comp(uint3 global_id : SV_DispatchThreadID) {
    uint index = global_id.x;
    uint tx, ty;
    if (!lit_cull_tile(index, tx, ty))
        return;
    lit_cull_lights(index, tx, ty, false, 0.0, 0.0);
}
