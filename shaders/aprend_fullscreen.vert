#version 450

// One triangle that covers the whole target, from the vertex index alone:
// drawn with 3 vertices and no vertex buffer. Its corners are (-1, -1),
// (3, -1) and (-1, 3) in clip space, so the target's square is inside it
// whichever way up clip space is and whatever the winding.

void main() {
    vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
