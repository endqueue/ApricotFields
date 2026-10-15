# ApMath TODO

ApMath is two headers: `include/math/apmathbvh.h` (ray casting against meshes,
double precision) and `include/math/apmathpoly.h` (plane polygons with holes on
an integer grid: booleans that carry each edge's tag through, a validity check,
an offset that moves each edge by its own distance, and triangulation).

## What has run where

- **apmathbvh**: built and tested (`tests/apmathbvh_tests.c`) on macOS.
- **apmathpoly**: written 2026-10-15; reported the same day to build and
  pass its tests (`tests/apmathpoly_tests.c`) - on one platform, which wasn't
  recorded (macOS is the one in use). Windows and Linux never built.
  Clipper2 1.5.4 and earcut.hpp v2.2.4 are fetched at configure time unless
  vendored under `../.deps/Clipper2` and `../.deps/earcut.hpp`.
  `apmath_poly_offset_edges` and its tests were added 2026-10-15 after that,
  and are written, not compiled.

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

### Not built

- [ ] **Round joins** for offsets. `apmath_poly_offset_edges` mitres and
      bevels only; a round corner is a tessellated arc, which is a caller's
      tolerance to choose.
- [ ] **A straight skeleton**, weighted per edge: every edge's face, and where
      faces meet, from the edges moving inward each at its own speed. General
      purpose (roof planes, mitred offsets, medial axes); the usual
      implementation (CGAL) is GPL, so it would be written here.
- [ ] **Point in polygon** for a set (winding number under a fill rule) as a
      call. The validity check has an exact one inside it.
