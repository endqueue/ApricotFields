module aprend_lit_cull;

// Aprend's tiled light culling, the body both culling shaders share:
// aprend_lit_cull_comp.slang, over the whole depth of the view, and
// aprend_lit_cull_bounded_comp.slang, which narrows each tile's near and far
// planes to the depth its texels hold (APREND_COMMAND_LIT_CULL_LIGHTS), as
// Hazel's forward+ does with its pre-depth.
//
// For each tile of the view's target, the point and spot lights whose sphere
// of range reaches into the tile's frustum, written as a count and a list
// the surface shader reads in place of every light. A tile's frustum is the
// view_projection's four side planes cut to the tile's rectangle in clip
// space (Gribb and Hartmann's extraction), and its near and far planes: the
// view's, or with bounds the planes at the least and most depth of the
// tile's texels; a sphere is kept unless it lies wholly outside one. One
// invocation a tile, in groups of 64 ([numthreads(64, 1, 1)] on both entry
// points).

import aprend_lit_scene;

// A uint each, raw (byte addressed), as SpudGPU's D3D12 backend views every
// storage buffer; written only here.
[[vk::binding(1, 0)]] public RWByteAddressBuffer AprendLitTiles : register(u1, space0);

// Row [i] of [m]: a matrix here is indexed by row (GLSL's m[0][i] to
// m[3][i]).
float4 row(float4x4 m, int i) {
    return m[i];
}

// Whether [center] with [radius] reaches the inside of [plane] (inside where
// dot(plane, (p, 1)) >= 0).
bool reaches(float4 plane, float3 center, float radius) {
    float l = length(plane.xyz);
    return l == 0.0 || (dot(plane.xyz, center) + plane.w) / l >= -radius;
}

// Which tile invocation [index] culls, column [tx] and row [ty]: false for
// an invocation past the last tile, which writes nothing.
public bool lit_cull_tile(uint index, out uint tx, out uint ty) {
    uint tiles = u_lit_scene.tiles_x * u_lit_scene.tiles_y;
    tx         = 0u;
    ty         = 0u;
    if (index >= tiles)
        return false;
    tx = index % u_lit_scene.tiles_x;
    ty = index / u_lit_scene.tiles_x;
    return true;
}

// Culls tile [index] (column [tx], row [ty]) and writes its list. With
// [bounded], its near and far planes are at [near_z] and [far_z], the least
// and most depth of its texels as clip z / w; without, the view's.
public void lit_cull_lights(uint index, uint tx, uint ty, bool bounded, float near_z, float far_z) {
    // The tile's rectangle in normalized device coordinates, x right and y
    // down the target from its top left, as its texels are numbered.
    float2 size  = u_lit_scene.target_size.xy;
    float t      = float(u_lit_scene.tile_size);
    float2 lo    = float2(float(tx) * t, float(ty) * t) / size * 2.0 - 1.0;
    float2 hi    = min(float2(float(tx + 1u) * t, float(ty + 1u) * t) / size, float2(1.0)) * 2.0 - 1.0;
    float4x4 m   = u_lit_scene.view_projection;
    float4 r0 = row(m, 0), r1 = row(m, 1), r2 = row(m, 2), r3 = row(m, 3);
    float4 planes[6];
    planes[0] = r0 - lo.x * r3; // x / w >= lo.x
    planes[1] = hi.x * r3 - r0; // x / w <= hi.x
    // SpudGPU's clip space has +Y up the target, so row 0 is at y = +1:
    // the tile's y runs from -hi.y at its bottom to -lo.y at its top.
    planes[2] = r1 + hi.y * r3; // y / w >= -hi.y
    planes[3] = -lo.y * r3 - r1; // y / w <= -lo.y
    if (bounded) {
        planes[4] = r2 - near_z * r3; // z / w >= the least depth
        planes[5] = far_z * r3 - r2;  // z / w <= the most
    } else {
        planes[4] = r2;               // z >= 0
        planes[5] = r3 - r2;          // z <= w
    }

    uint first = index * uint(APREND_LIT_MAX_TILE_LIGHTS + 1);
    uint count = 0u;
    for (uint i = 0u; i < min(u_lit_scene.light_count, uint(APREND_LIT_MAX_LIGHTS)) && count < uint(APREND_LIT_MAX_TILE_LIGHTS); ++i) {
        float3 c  = u_lit_scene.lights[i].position_range.xyz;
        float r   = u_lit_scene.lights[i].position_range.w;
        bool in_  = true;
        for (int p = 0; p < 6 && in_; ++p)
            in_ = reaches(planes[p], c, r);
        if (in_) {
            AprendLitTiles.Store((first + 1u + count) * 4u, i);
            ++count;
        }
    }
    AprendLitTiles.Store(first * 4u, count);
}
