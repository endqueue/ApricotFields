// The batcher's circles, rings and arcs (rbtrendbatch.h): coverage from the
// distance to each edge, in pixels by the screen-space derivative, so the
// edge is antialiased at any size and under any projection.

import rbtrend_batch;
import rbtrend_batch_inputs;

static const float TAU = 6.28318530718;

[shader("fragment")]
float4 rbtrend_batch_circle_frag(BatchFragmentInput input) : SV_Target0 {
    float2 p = input.local;
    float r  = length(p);
    float px = max(fwidth(r), 1e-6); // one pixel, in units of the radius
    float coverage = clamp((1.0 - r) / px + 0.5, 0.0, 1.0);
    float inner    = input.d0.x;
    if (inner > 0.0)
        coverage = min(coverage, clamp((r - inner) / px + 0.5, 0.0, 1.0));
    float sweep = input.d0.z;
    if (sweep != 0.0 && abs(sweep) < TAU) {
        // Measured from the start, the way the arc turns.
        float2 q = sweep < 0.0 ? float2(p.x, -p.y) : p;
        float s  = sweep < 0.0 ? -input.d0.y : input.d0.y;
        float w  = abs(sweep);
        // GLSL's mod: floored, not fmod's truncation.
        float turned = atan2(q.y, q.x) - s;
        float a      = turned - TAU * floor(turned / TAU);
        // How far inside the arc, as a distance along the circle at this
        // radius: negative outside it.
        float inside = a <= w ? min(a, w - a) : -min(a - w, TAU - a);
        coverage = min(coverage, clamp(inside * r / px + 0.5, 0.0, 1.0));
    }
    return batch_premultiply(input.color, coverage);
}
