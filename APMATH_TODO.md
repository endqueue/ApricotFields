# ApMath TODO

ApMath is two headers: `include/math/apmathbvh.h` (ray casting against meshes,
double precision) and `include/math/apmathpoly.h` (plane polygons with holes on
an integer grid: booleans that carry each edge's tag through, a validity check,
an offset that moves each edge by its own distance, a weighted straight skeleton,
and triangulation).

## What has run where

- **apmathbvh**: built and tested (`tests/apmathbvh_tests.c`) on macOS.
- **apmathpoly**: written 2026-10-15; reported the same day to build and
  pass its tests (`tests/apmathpoly_tests.c`) - on one platform, which wasn't
  recorded (macOS is the one in use). Windows and Linux never built.
  Clipper2 1.5.4 and earcut.hpp v2.2.4 are fetched at configure time unless
  vendored under `../.deps/Clipper2` and `../.deps/earcut.hpp`.
  `apmath_poly_offset_edges` and its tests, added after, likewise build and
  pass (reported 2026-10-15). `apmath_poly_skeleton` and its tests were added
  after that, and are written, not compiled.

## apmathpoly

### Before it's relied on

- [ ] **Build and run the tests** on Windows (MSVC: the 128-bit products are
      written out for it, and `/fp:contract` must stay off) and Linux.
- [ ] **Same bits everywhere.** A test that runs a fixed set of booleans and
      triangulations and compares a hash of every output array against a value
      recorded once - then the same value on all three platforms. That is the
      claim the header makes, and nothing checks it yet.
- [ ] **Clipper2 under this repo's warnings.** Its sources are built with this
      repo's flags, not its own `-Werror`; they build with C++26 on the one
      platform reported, so check GCC and MSVC.

### Cost

- [ ] **Tagging a boolean's result** compares every result edge with every input
      edge: fine for outlines of tens or hundreds of edges, not for thousands.
      A uniform grid or a sort by x over the input edges would make it close to
      linear. Measure first.
- [ ] **`apmath_poly_validate`** tests every pair of edges, likewise. A sweep
      line would make it n log n.
- [ ] **`apmath_poly_skeleton`** looks for the next event from scratch after
      every event: about n^3 for n edges. A priority queue of events,
      invalidated as vertices end, is the standard n^2 log n; worth it only if
      a caller brings polygons of hundreds of edges.

### Skeleton: degenerate cases

The simulation is in doubles with a tolerance of 1e-9 of the polygon's size,
and these are the cases it meets least gracefully - none breaks it, but each is
where a result could be less than exact. A test for each, from real footprints,
before relying on them:

- [ ] Several reflex corners reaching one edge at once, or one reaching a
      vertex exactly (a "vertex event"): handled as a split at the edge's end,
      then a collapse of the empty edge that leaves.
- [ ] Two edges that come to follow each other on one line mid-way with
      different weights: the faster leads, which is a guess, not a rule.
- [ ] A face walk through a node with more than two traces meeting in one
      face: the first unused trace is taken.

### Not built

- [ ] **Round joins** for offsets. `apmath_poly_offset_edges` mitres and
      bevels only; a round corner is a tessellated arc, which is a caller's
      tolerance to choose.
- [ ] **Skeletons of polygons with holes.** `apmath_poly_skeleton` takes one
      ring (`APRESULT_UNSUPPORTED` otherwise); the wavefront of a hole is the
      same simulation with more loops, and splits between loops.
- [ ] **Point in polygon** for a set (winding number under a fill rule) as a
      call. The validity check has an exact one inside it.
