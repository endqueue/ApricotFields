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
- **2026-10-17, macOS (Apple M5 Pro), Debug and Release:** all nine of
  ApricotFields' headless tests pass, `apmathpoly_tests` and the skeleton's
  among them, and trellislib's model tests (roofs use the offset) pass over
  it. Before that day's fix `apmathpoly_tests` failed one check,
  `offset_moves`: a square moved in past half its width came out as a 2x2
  square, not nothing (see "Offset: edges that go", below).

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

### Offset: edges that go

Fixed 2026-10-17. Each edge was moved and its corners re-met, and the union
was left to remove what a large inward move turned over. A ring moved in
past its width comes out turned through half a turn (a convex ring's moved
corners each land past the opposite one), which winds the same way as the
ring, so the union kept it. `move_edges` now runs the move from 0 to 1 and
takes out each edge whose ends meet, earliest first, so its neighbours meet
directly; `test_offset_collapses` covers a square, a thin bar and a triangle
shrunk and shrunk away, and a notch grown shut. Left:

- [ ] **Splits are still the union's.** A ring pinched in two by an inward
      move (a narrow waist) crosses itself, and the union takes the crossed
      part out, which is right for a waist and has no test. A pinch where a
      reflex corner meets an edge, then an edge of one half goes later, is
      worked out on the unsplit ring: no test either. The skeleton handles
      splits properly; an offset from it (the wavefront at a time) would be
      the exact version, once it takes holes.
- [ ] **Bevels and the mitre geometry.** Which edges go is worked out from
      the mitred corners, even where a corner is then drawn bevelled: an
      edge beside a sharp corner can go a little early or late against the
      bevel drawn. Untested.
- [ ] **Cost**: each edge taken out looks at every edge left, so about n^2
      for n edges where many go; none at all where none does.

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
