// Aprend's tiled light culling with depth bounds
// (APREND_COMMAND_LIT_CULL_LIGHTS): aprend_lit_cull.slang, each tile's near
// and far planes at the least and most depth its texels hold in the view's
// depth buffer, so a light whose range lies wholly in front of or behind
// what a tile shows is left out of its list.

import aprend_lit_scene;
import aprend_lit_cull;

// The view's depth: a depth view's environment set (a texture and a sampler,
// aprendlit_internal.hpp).
[[vk::binding(0, 1)]] Texture2D<float4> tile_depth : register(t0, space1);
[[vk::binding(1, 1)]] SamplerState tile_depth_sampler : register(s1, space1);

// One invocation a tile.
[shader("compute")]
[numthreads(64, 1, 1)]
void aprend_lit_cull_bounded_comp(uint3 global_id : SV_DispatchThreadID) {
    uint index = global_id.x;
    uint tx, ty;
    if (!lit_cull_tile(index, tx, ty))
        return;
    // The least and most depth of the tile's texels: a range of clip z / w,
    // whichever way round the depth runs.
    uint depth_width, depth_height, depth_levels;
    tile_depth.GetDimensions(0, depth_width, depth_height, depth_levels);
    int2 depth_size = int2(int(depth_width), int(depth_height));
    int2 from       = int2(int(tx * u_lit_scene.tile_size), int(ty * u_lit_scene.tile_size));
    int2 to         = min(from + int2(int(u_lit_scene.tile_size)), depth_size);
    float near_z    = 1.0;
    float far_z     = 0.0;
    for (int y = from.y; y < to.y; ++y) {
        for (int x = from.x; x < to.x; ++x) {
            float d = tile_depth.Load(int3(x, y, 0)).r;
            near_z  = min(near_z, d);
            far_z   = max(far_z, d);
        }
    }
    lit_cull_lights(index, tx, ty, true, near_z, far_z);
}
