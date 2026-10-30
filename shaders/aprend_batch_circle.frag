#version 450

// The batcher's circles, rings and arcs (aprendbatch.h): coverage from the
// distance to each edge, in pixels by the screen-space derivative, so the
// edge is antialiased at any size and under any projection.

#include "aprend_batch.glslh"
#include "aprend_batch_inputs.glslh"

const float TAU = 6.28318530718;

void main() {
    vec2 p   = in_local;
    float r  = length(p);
    float px = max(fwidth(r), 1e-6); // one pixel, in units of the radius
    float coverage = clamp((1.0 - r) / px + 0.5, 0.0, 1.0);
    float inner    = in_d0.x;
    if (inner > 0.0)
        coverage = min(coverage, clamp((r - inner) / px + 0.5, 0.0, 1.0));
    float sweep = in_d0.z;
    if (sweep != 0.0 && abs(sweep) < TAU) {
        // Measured from the start, the way the arc turns.
        vec2 q  = sweep < 0.0 ? vec2(p.x, -p.y) : p;
        float s = sweep < 0.0 ? -in_d0.y : in_d0.y;
        float w = abs(sweep);
        float a = mod(atan(q.y, q.x) - s, TAU);
        // How far inside the arc, as a distance along the circle at this
        // radius: negative outside it.
        float inside = a <= w ? min(a, w - a) : -min(a - w, TAU - a);
        coverage = min(coverage, clamp(inside * r / px + 0.5, 0.0, 1.0));
    }
    out_color = batch_premultiply(in_color, coverage);
}
