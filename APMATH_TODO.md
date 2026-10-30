# ApMath TODO

ApMath is two headers: `include/math/apmathbvh.h` (ray casting against meshes,
double precision) and `include/math/apmathpoly.h` (plane polygons with holes on
an integer grid: booleans that carry each edge's tag through, a validity check,
an offset that moves each edge by its own distance, a weighted straight skeleton,
and triangulation).

## What has run where

- **2026-10-30, Windows 11** (ThinkPad P1 Gen 5, MSVC 14.51): the first
  Windows build of the polygons. `apmathpoly_tests`, `apmathpoly_bits_tests`
  and `apmathbvh_tests` pass on `windows-d3d12` and `windows-vulkan` in
  Debug, Release and RelWithDebInfo, and under both `-asan` presets; the
  bits test's hashes are macOS's in every configuration, so the polygons'
  results are the same bits on both platforms. The test needed
  `_USE_MATH_DEFINES` for MSVC's `M_PI`. ApMath's ray casts make no
  allocation there (`aprinoalloc_tests`). Linux never built.
- **apmathbvh**: built and tested (`tests/apmathbvh_tests.c`) on macOS.
- **apmathpoly**: written 2026-10-15; reported the same day to build and
  pass its tests (`tests/apmathpoly_tests.c`) - on one platform, which wasn't
  recorded (macOS is the one in use). Windows and Linux never built.
  Clipper2 1.5.4 and earcut.hpp v2.2.4 are fetched at configure time unless
  vendored under `../.deps/Clipper2` and `../.deps/earcut.hpp`.
  `apmath_poly_offset_edges` and its tests, added after, likewise build and
  pass (reported 2026-10-15). `apmath_poly_skeleton` and its tests were added
  after that.
- **2026-10-30, macOS (Apple M5 Pro), Debug and Release:** the polygon
  work of card A21 and the autonomous list (below): `apmathpoly_tests`
  with its new skeleton, location and round-join tests, and
  `apmathpoly_bits_tests`, whose hashes Debug and Release both match;
  plain and under ASan. A caller's model tests (its roofs use the offset
  and the skeleton) pass over it. **Windows and Linux never built**: the
  bits test is the one to run there first.
- **2026-10-17, macOS (Apple M5 Pro), Debug and Release:** all nine of
  ApricotFields' headless tests pass, `apmathpoly_tests` and the skeleton's
  among them, and a caller's model tests (its roofs use the offset) pass over
  it. Before that day's fix `apmathpoly_tests` failed one check,
  `offset_moves`: a square moved in past half its width came out as a 2x2
  square, not nothing (see "Offset: edges that go", below).

## apmathpoly

### Before it's relied on

- [ ] **Build and run the tests** on Windows (MSVC: the 128-bit products are
      written out for it, and `/fp:contract` must stay off) and Linux.
- [x] **Same bits everywhere** (2026-10-30): `tests/apmathpoly_bits_tests.c`
      runs a fixed set of booleans (every operation and fill), point
      locations, triangulations, offsets (mitred and round, out and in, a
      hole) and skeletons (a hole, mixed weights, a merge on one line), and
      compares an FNV-1a hash of every output with the 107 values recorded
      on macOS (`tests/data/apmathpoly/bits.inc`), which Debug and Release
      both give. `--print` writes them out. Left: the same values on
      Windows and Linux.
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

- [x] Several reflex corners reaching one edge at once, or one reaching a
      vertex exactly (a "vertex event"): handled as a split at the edge's end,
      then a collapse of the empty edge that leaves. Tested 2026-10-30
      (`test_skeleton_degenerate`): a comb whose two notches reach the
      bottom at once, a chevron whose notch reaches the opposite tip, a
      plus whose four reflex corners meet in the middle, an octagon closing
      at one point; every face closes and they cover the polygon.
- [x] Two edges that come to follow each other on one line mid-way with
      different weights: the faster leads, which is a guess, not a rule.
      **Decided 2026-10-28 with the user (card A21): the rule.** The
      larger weight (the faster edge) wins the shared line; on equal
      weights, the edge earlier in the input. Written in the header and
      tested with a footprint that reaches it, so every platform builds
      the same skeleton. **Built 2026-10-30** (`merge_on_line`): the
      loser's face is closed along the line from the event to its other
      end, whose vertex goes on beside the winner. Tested
      (`test_skeleton_on_one_line`) with a step whose low edge at weight 2
      catches the high one at 1, and a bump whose halves meet at equal
      weight; before the rule both footprints failed outright ("never
      closes").
- [x] A face walk through a node with more than two traces meeting in one
      face: the first unused trace is taken. Since 2026-10-30 the walk takes
      the trace that turns most to the right (the face is on its left),
      ordered by a pseudo-angle from a division, not `atan2`, so it is the
      same everywhere; the plus and the octagon reach it.

### Not built

- [x] **Round joins** for offsets (2026-10-30): `APMATH_POLY_OFFSET_DESC`'s
      `join` (`APMATH_POLY_JOIN_MITER`, 0, as before, or `_ROUND`) and
      `arc_tolerance`. A corner the move opens is an arc about the original
      corner, the radius going from one edge's distance to the other's,
      halved in angle as often as the tolerance needs with arithmetic and
      square roots only (the same on every platform); a corner the move
      closes is mitred either way, and which edges go is still worked out
      from mitred corners. Tested (`test_offset_round`): every point of a
      square grown by 1000 within a unit of 1000 from it, its area that of
      the rounded square less the chords, finer tolerances giving more
      points, an inward move identical to the mitred one, a hole, uneven
      distances, and the refusals.
- [x] **Skeletons of polygons with holes** (2026-10-30): the set is polygons
      with holes; each ring is a loop of the wavefront, and a reflex corner
      may reach an edge of any loop, which splits its own loop or joins two.
      Tested (`test_skeleton_holes`): a centred hole (every face a
      trapezoid), one off to a side, and two holes, the faces covering the
      polygon round them.
- [x] **Point in polygon** (2026-10-30): `apmath_poly_locate_point(set,
      fill, point)` gives inside, outside or on an edge, from an exact
      winding number over any rings. Tested against a slow angle sum in
      doubles at 8,000 random points of random crossing rings, under every
      fill rule, and on edges, corners and level with a vertex.
