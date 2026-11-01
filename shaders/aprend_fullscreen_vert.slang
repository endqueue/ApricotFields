// One triangle that covers the whole target, from the vertex index alone:
// drawn with 3 vertices and no vertex buffer. Its corners are (-1, 1),
// (3, 1) and (-1, -3) in clip space (+Y up the target), so the target's
// square is inside it; the same texels and winding as (-1, -1), (3, -1)
// and (-1, 3) had when clip space's +Y was down the target.

[shader("vertex")]
float4 aprend_fullscreen_vert(uint vertex_index : SV_VertexID) : SV_Position {
    float2 corner = float2(float((vertex_index << 1) & 2u), float(vertex_index & 2u));
    float2 ndc    = corner * 2.0 - 1.0;
    return float4(ndc.x, -ndc.y, 0.0, 1.0);
}
