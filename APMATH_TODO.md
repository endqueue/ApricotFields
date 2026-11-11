# ApMath TODO

ApMath is two headers: `include/math/apmathbvh.h` (ray casting against meshes,
double precision) and `include/math/apmathpoly.h` (plane polygons with holes on
an integer grid: booleans that carry each edge's tag through, a validity check,
an offset that moves each edge by its own distance, a weighted straight skeleton,
and triangulation).

## What has run where

- **2026-10-31, macOS (Apple M5 Pro):** volume tests in `apmathbvh.h`
  (card T21; a plane, a convex volume with clip planes, a box's three
  answers, a mesh BVH's overlaps and within): `apmathbvh_tests` (400
  random volumes against a brute force), `aprinoalloc_tests` (no
  allocation) and `aprioverlap_tests` (two threads, every build) pass in
  Debug, Release, `-asan` and `-tsan`, built inside a caller's presets.
  Card T22 holds one sentence of the contract: triangles with no area,
  which the BVH drops. (This said "Windows and Linux not built" until
  2026-11-05: see the Windows and Linux entries below.)
- **2026-11-05, Windows 11** (ThinkPad P1 Gen 5, MSVC 14.51.36231):
  `apricotfields-windows-d3d12`, Debug and Release, no warning:
  `apmathbvh_tests` (the volume tests among them), `apmathpoly_tests` and
  `apmathpoly_bits_tests` pass, the bits test's hashes macOS's (36 of 37
  ctests, `aprend_contract_tests`' `lit_fit_cascades` the one, card A49).
- **2026-11-04, Linux** (Fedora 44, GCC 16.2.1, and clang 22.1.8 under
  the sanitizers): ApricotFields' ctests on `apricotfields-linux-vulkan`,
  Debug and Release, `-asan` and `-tsan`, 36 of 37, the same one failing
  (`../ROADMAP.md`, section 2): ApMath's three, the bits test with
  macOS's hashes, pass.
- **2026-10-30, Windows 11** (ThinkPad P1 Gen 5, MSVC 14.51): the first
  Windows build of the polygons. `apmathpoly_tests`, `apmathpoly_bits_tests`
  and `apmathbvh_tests` pass on `windows-d3d12` and `windows-vulkan` in
  Debug, Release and RelWithDebInfo, and under both `-asan` presets; the
  bits test's hashes are macOS's in every configuration, so the polygons'
  results are the same bits on both platforms. The test needed
  `_USE_MATH_DEFINES` for MSVC's `M_PI`. ApMath's ray casts make no
  allocation there (`aprinoalloc_tests`). Linux built 2026-11-04 (above).
- **apmathbvh**: built and tested (`tests/apmathbvh_tests.c`) on macOS.
- **apmathpoly**: written 2026-10-15; reported the same day to build and
  pass its tests (`tests/apmathpoly_tests.c`) - on one platform, which wasn't
  recorded (macOS is the one in use). Windows built 2026-10-30 and
  Linux 2026-11-04 (above). Clipper2 1.5.4 and earcut.hpp v2.2.4 are fetched at configure time unless
  vendored under `../.deps/Clipper2` and `../.deps/earcut.hpp`.
  `apmath_poly_offset_edges` and its tests, added after, likewise build and
  pass (reported 2026-10-15). `apmath_poly_skeleton` and its tests were added
  after that.
- **2026-10-30, macOS (Apple M5 Pro), Debug and Release:** the polygon
  work of card A21 and the autonomous list (below): `apmathpoly_tests`
  with its new skeleton, location and round-join tests, and
  `apmathpoly_bits_tests`, whose hashes Debug and Release both match;
  plain and under ASan. A caller's model tests (its roofs use the offset
  and the skeleton) pass over it. Windows and Linux weren't built then;
  the bits test has passed on both since (2026-10-30 and 2026-11-05 on
  Windows, 2026-11-04 on Linux, above).
- **2026-10-17, macOS (Apple M5 Pro), Debug and Release:** all nine of
  ApricotFields' headless tests pass, `apmathpoly_tests` and the skeleton's
  among them, and a caller's model tests (its roofs use the offset) pass over
  it. Before that day's fix `apmathpoly_tests` failed one check,
  `offset_moves`: a square moved in past half its width came out as a 2x2
  square, not nothing (see "Offset: edges that go", below).

## apmathbvh: shapes and an instance BVH changed in place

**Drafted 2026-11-05, revised 2026-11-06 after two reviews, approved by the
user 2026-11-06:** `apmath_bvh_queries_draft.h`. Spheres, capsules and boxes
(`APMATH_SHAPE`) tested against a mesh or an instance BVH (overlaps) and
moved along a line to their first touch (sweeps), by one GJK algorithm,
a sweep walking the tree as a ray against boxes grown by the shape's;
an instance BVH's instances set in place, refitted and rebuilt with no
allocation, its surface-area cost read to choose between them; a box BVH
over solid boxes the caller gives, with the same queries and upkeep;
`APMATH_ERROR` moved to `math/apmatherror.h`; `apmathbvh.cpp` built
without floating-point contraction. No ABI break.

**Built 2026-11-06 on Windows 11** (ThinkPad P1 Gen 5, MSVC 14.51;
`apricotfields-windows-d3d12` and `-vulkan`, Debug and Release; macOS and
Linux not built, so the contraction flag and the bits test's hashes on
Clang and GCC are still to see). Every ctest passes on both presets in
both configurations but `aprend_contract_tests` (card A49's
`lit_fit_cascades`, as before), and under both `-asan` presets (from a
Visual Studio developer prompt) the same, no sanitizer report. New or grown: `apmathbvh_tests` (exact
cases; 3,000 overlaps and 3,000 sweeps against brute force - exact
point-, segment- and separating-axis distances, a march of them bisected
to 1e-9 - with no near touch to skip; 1,000 sliver triangles grazed by
capsules, GJK's capped cases; sweeps of INFINITY through 100,000
instances against each instance's own; tree independence - created, set
and refitted, rebuilt, created fresh: the same bits; the in-place calls'
refusals in parameter order, each changing nothing; the box BVH against
brute force; the cast's early stop), `apmathbvh_bits_tests` (new: casts,
overlaps, sweeps, a refit, a rebuild's cost and the box BVH hashed,
recorded here, Debug and Release alike, `tests/data/apmathbvh/bits.inc`;
the same on macOS, Apple M5 Pro, 2026-11-09, once the test itself was
built with no floating-point contraction: clang had fused its inputs'
multiply-adds on arm64, so the inputs, not the BVH, differed),
`aprinoalloc_tests` (an instance BVH of 10,000 moving, set, refit or
rebuilt, 8 shapes overlapped and swept a frame; a box BVH of 100,000, every
call: none of ours), `aprioverlap_tests` (queries, overlaps and sweeps
from two threads; sets on disjoint instances from two threads, then a
refit; in Debug, the six overlaps the rule forbids reported and the two
it allows not), and the misuse harness (140 calls with a destroyed handle,
ten times in Debug, no failure). The Debug check of shared roles is new
(`apri_shared_overlap`, `src/aprioverlap.hpp`). How the build differs from
the draft, none in a contract: `../claude_made_decisions.md`, 2026-11-06.

Numbers, Release, the ThinkPad (`tests/apmathbvh_bench.c`, a 224-triangle
ball placed 100, 10,000 and 100,000 times at the density of 10,000 in a
100 m cube). Casts before and after (`--casts`, best of three runs each):
nearest 1.42-1.50 M / 367-376 k / 225-234 k rays a second before, 1.43-1.50
M / 365-379 k / 217-227 k after - unchanged; the early stop of a cast that
asks only whether is worth little here (the scene's first hit is near the
nearest); create 4.8 and 60 ms before at 10,000 and 100,000, 6.0 and 70-78
ms after (each instance is placed twice: checked before anything is
allocated, then filled). Shapes and sweeps (100 / 10,000 / 100,000):
overlaps_shape 5.6 M / 3.1 M / 1.9 M a second; overlap_all 5.4 M / 2.1 M
/ 1.4 M; a sweep of 10 m 259 k / 119 k / 115 k; of INFINITY 220 k / 70 k /
54 k, beside rays along the same lines 1.31 M / 249 k / 159 k (the
sweep's walk is a ray's: within 5 times). Upkeep a step, set and refit
against set and rebuild against a fresh create:

| instances | moving | set + refit | set + rebuild | create | cost: built, refitted 60 steps, rebuilt |
|---|---|---|---|---|---|
| 100 | all | 15 us | 40 us | 44 us | 8.6, 11.3, 7.0 |
| 100 | 1% | 0.4 us | 21 us | 41 us | 7.0, 7.0, 7.0 |
| 10,000 | all | 2.07 ms | 6.62 ms | 7.74 ms | 55.4, 104.3, 52.2 |
| 10,000 | 1% | 52 us | 5.51 ms | 8.76 ms | 52.2, 49.9, 48.7 |
| 100,000 | all | 22.3 ms | 91.3 ms | 101 ms | 127.6, 257.5, 122.8 |
| 100,000 | 1% | 0.75 ms | 73.1 ms | 101 ms | 122.8, 120.2, 117.3 |

Refitting all of them for 60 steps of random drift doubles the cost a ray
pays; with 1% moving it doesn't move. The ratio to rebuild at is the
caller's. A set computes each instance twice (checked, then placed, so a
refused set changes nothing): about half of an all-moving step's set.

- [ ] Build and run on macOS and Linux: the bits test against these
  hashes, `-tsan` on `aprioverlap_tests`.

## apmathpoly

### Before it's relied on

- [x] **Build and run the tests** on Windows (MSVC: the 128-bit products are
      written out for it, and `/fp:contract` must stay off) and Linux:
      Windows 2026-10-30 and 2026-11-05 (MSVC 14.51), Linux 2026-11-04
      (GCC 16.2.1, clang 22.1.8), every ApMath test passing.
- [x] **Same bits everywhere** (2026-10-30): `tests/apmathpoly_bits_tests.c`
      runs a fixed set of booleans (every operation and fill), point
      locations, triangulations, offsets (mitred and round, out and in, a
      hole) and skeletons (a hole, mixed weights, a merge on one line), and
      compares an FNV-1a hash of every output with the 107 values recorded
      on macOS (`tests/data/apmathpoly/bits.inc`), which Debug and Release
      both give. `--print` writes them out. The same values on Windows
      (MSVC, 2026-10-30 and 2026-11-05) and Linux (GCC 16.2.1, 2026-11-04).
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
