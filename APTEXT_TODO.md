# ApText TODO

ApText is the module that turns text into positioned glyphs and keeps their
distance-field atlas: font faces from the caller's bytes, shaping, line
layout, and MTSDF glyph generation. It is CPU only and unrelated to Aprend,
the way ApMath is; Aprend draws what it hands back (`APREND_TODO.md`, item
23, "Text: the design").

**Implemented 2026-10-29** (the implementation pass; see "The
implementation" below), over the contract of the same day: every call is
built, and `tests/aptext_tests.c`'s 41 tests pass, none pending.
`tests/aptext_bits_tests.c` checks one layout's and one saved atlas's bytes
against values recorded on macOS, and `tests/fuzz/aptext_fuzz.cpp` fuzzes
the readers of outside bytes. Builds and runs on macOS (Debug and Release,
AddressSanitizer with UndefinedBehaviorSanitizer, ThreadSanitizer,
2026-10-29); Windows and Linux never built.

Header `include/text/aptext.h`, prefix `aptext_` / `APTEXT_`, newer
generation (see `CLAUDE.md`, "Writing a new module"): `APRESULT` returns,
`APTEXT_ERROR` per call, descs with a struct base (`APRI_STRUCT_TYPE_APTEXT_*`,
6000+, in a block of its own), and its own numbered block of
`APRESULT_APTEXT_*` codes in `apricore.h` (6001+).

## What it deliberately does not do

- **No file I/O.** A face is created from bytes the caller supplies, as
  ApArchive takes its I/O through callbacks. No font search path, no system
  font lookup, no disk cache of atlases: where fonts and cached atlases live
  is the caller's.
- **No GPU.** It produces atlas pixels and glyph quads; uploading and
  drawing them is Aprend's and the caller's.
- **No default font and no global state.** Every face, fallback chain and
  atlas is an object the caller creates and passes in.
- **No platform text APIs** (CoreText, DirectWrite). They shape and lay out
  differently from each other, which would be a behavioral difference
  between platforms; one implementation everywhere instead.
- **Doesn't know what the text is for.** A string, a style and a box in; glyphs,
  positions and boxes out.

## Dependencies (decided 2026-10-15)

- [x] **HarfBuzz** (MIT): shaping, and glyph outlines through
      `hb_font_draw_glyph`, so FreeType isn't needed at all.
- [x] **msdfgen** core (MIT): MTSDF generation from an outline built from
      HarfBuzz's draw callbacks. Not msdfgen's `ext` (that is FreeType and
      file loading) and not msdf-atlas-gen (its packer assumes a fixed
      glyph set; ours is on demand).
- [x] **Bidirectional text and line breaking:** SheenBidi plus libunibreak
      (small), or ICU (complete, large). Undecided; neither is needed for
      the first version, but the layout API must have room for both.
      **Decided 2026-10-28 with the user (card A19):** SheenBidi (Apache
      2.0) and libunibreak (zlib), vendored, not ICU (about 30 MB with its
      data) and not the operating system's text engines: text must wrap
      the same on every platform, or a sheet's notes differ between
      collaborators and between prints. Left to right first, with bidi
      wired in from the start and tested when a right-to-left locale is
      planned. Thai, Lao and Khmer need a dictionary for word breaks,
      which libunibreak covers only in part: a known gap.
- [x] Vendored under `../.deps/` like the repo's other third-party code
      (`CLAUDE.md`, "Build"), pinned, linked `PRIVATE` (no HarfBuzz or
      msdfgen type in `include/`), and built under this repo's flags on all
      three platforms. HarfBuzz's shaping must give the same output
      everywhere for layout to be deterministic (item 2), so pin it and
      check it the way ApMath's polygons are checked.
      **Done 2026-10-29:** HarfBuzz 14.6.0 (its amalgamation, no optional
      dependency), msdfgen 1.13 (core), SheenBidi 2.9.0, libunibreak 8.0,
      each a static library of its own built from its sources
      (`aptext_harfbuzz`, `aptext_msdfgen`, `aptext_sheenbidi`,
      `aptext_unibreak`), linked PRIVATE, pinned by tag in
      `CMakeLists.txt` and fetched there when `../.deps/` has no copy (none
      is vendored by hand yet), with floating-point contraction off. SheenBidi
      2.9, not 3.0: 3.0 adds a process-wide allocator a caller can replace,
      writable global state. `tests/aptext_bits_tests.c` is the check.

## The contract (2026-10-29)

Written before the implementation, as `../ROADMAP.md` section 1 asks: the
header, its docs and the black-box tests first. **The implementation pass
fills in `src/text/aptext.cpp` behind the same argument checks and doesn't
edit `tests/aptext_tests.c`.**

- **The API.** Faces (`aptext_face`: `aptext_face_create` from font bytes,
  `aptext_face_create_stroke` from the caller's polylines,
  `aptext_face_create_hershey`; metrics, cmap lookup, a stroke glyph's
  polylines). Fallback chains (`aptext_chain`). Layouts (`aptext_layout`:
  made empty, `aptext_layout_build` fills it; glyphs, lines and runs as
  arrays, stroke layouts as segments). `aptext_generate_glyph` (card A18's
  first step). Atlases (`aptext_atlas`: `aptext_atlas_insert`, card A18's
  second step; `aptext_atlas_add_layout`, find, pages, dirty rectangles,
  save and load).
- **The stub.** Every entry point makes its argument and struct-base checks,
  line by line, then returns `APRESULT_NOT_IMPLEMENTED` with a message
  saying so. Two checks the header promises are left to the implementation,
  each marked where it belongs: a stroke face's duplicate codepoints, and a
  chain whose faces are of more than one kind (it needs the faces).
- **The tests** (41 on 2026-10-29: 9 pass, the argument checks; 32 pending).
  A behavioural test that meets `APRESULT_NOT_IMPLEMENTED` reports itself
  pending and stops; everything else it asserts. The implementation turns
  them on call by call, and a call left unbuilt stays pending rather than
  failing. The fonts are built byte by byte in the test (`build_font()`: two
  TrueType fonts at 1024 units per em and a collection of both, with a
  kerning pair, a `liga` ligature, Han and Hebrew glyphs), so every expected
  position is an exact binary fraction compared with `==`, which is also the
  "same bits on every platform" check. `aptext_tests --write-fonts <dir>`
  writes them out. On 2026-10-29 they were read with fontTools 4.60.2
  (checksums verified) and shaped with HarfBuzz through uharfbuzz 0.51.7:
  metrics, character maps, the ligature, the removed zero-width joiner,
  `.notdef`, the scripts and the right-to-left order are what the tests
  expect. HarfBuzz splits a legacy kern table's value between the pair's
  advance and offset, so the kerning test checks only where V lands and the
  line's width.
- **The calls the contract made**, where this file or its cards left it
  open (each also in the header's comments):
  1. A stroke face is an `aptext_face` of kind `APTEXT_FACE_KIND_STROKE`,
     so one layout code serves both kinds (card A20); a chain's faces are of
     one kind. The caller's polylines are copied (they are small and built
     by the caller), unlike font bytes (card A17).
  2. Hershey: the common ".jhf" files. The format has no scale or baseline,
     so the desc takes `units_per_em`, `baseline` and the metrics, and a
     codepoint per glyph or a first codepoint.
  3. Metrics are in em units as floats, not font units, so outline and
     stroke faces answer alike.
  4. Two coordinate spaces: font space (em, y up) for what describes a face
     and for stroke input; layout space (x right, y down, origin top-left,
     em units times `em_size`) for everything ApText produces, atlas plane
     bounds included (em, y down from the glyph's origin).
  5. A layout is a reusable object (created empty, rebuilt in place), so
     text that changes every frame doesn't allocate every frame. It keeps
     its chain, which must outlive it or its next build.
  6. Every line has the same advance, from the chain's first face:
     (ascender - descender + line gap) times `em_size` times
     `line_spacing`, with the extra shared equally above and below (the
     formula is in `APTEXT_LAYOUT_DESC`'s comment). Not the tallest face on
     each line (CSS's way): lines that move when a fallback glyph appears
     are worse for drawings and for comparing layouts.
  7. A word wider than `max_width` wraps between grapheme clusters, never
     overflowing; trailing spaces hang (glyphs on the line, outside its box
     and alignment); letter spacing goes after every cluster but a line's
     last; mandatory breaks give no glyph, default-ignorable codepoints are
     removed, U+0009 is a space until tab stops exist.
  8. Glyphs and runs are in visual order; each glyph carries its cluster's
     byte range and an advance box over its line's box; each run its script
     (ISO 15924 tag), bidi level and direction.
  9. Features: kerning, ligatures (`liga` + `clig`) and contextual
     alternates are bits the caller sets (no defaults, so the caller says
     "kerning and ligatures on"); any other feature is an `APTEXT_FEATURE`;
     a script's required features are always on.
  10. Scripts are detected (Unicode's Script property); a caller-set script
      is later, as a chained struct. Language is an optional BCP 47 tag.
  11. An ill-formed UTF-8 sequence is one U+FFFD per maximal subpart.
  12. The atlas keys glyphs by face and glyph id, any number of outline
      faces in one atlas; pages are the layers of one 2D array texture;
      skyline packing; `max_pages` is required (UINT32_MAX for none).
  13. `aptext_generate_glyph` takes the scale in its desc and measures
      when given no memory (no separate measure call); `aptext_atlas_insert`
      takes the same desc, refuses other settings than the atlas's, and is
      idempotent (two jobs may generate the same glyph).
  14. The image format is fixed in the header: RGBA8, alpha
      round(255 * clamp(0.5 + d / range)) with d positive inside, a margin
      of at least range / 2 pixels, plane bounds at the pixel corners.
  15. Dirty rectangles: at most one per page, the union of what changed,
      taken up to a capacity with the rest left for the next call.
  16. `aptext_atlas_add_layout`: the serial "on demand" path, beside
      generate-then-insert for callers with a scheduler.
  17. Save and load are to and from memory; the bytes carry the settings, a
      fingerprint per face (from the font's head table: revision, checksum
      adjustment, glyph count), the generator's version and a checksum.
      Load takes the settings the caller wants and fails with
      `APRESULT_APTEXT_ATLAS_MISMATCH` on any difference; damage is
      `APRESULT_CORRUPT_DATA`, an unknown format version
      `APRESULT_UNSUPPORTED`. A loaded atlas's pages are dirty in full.
  18. Two `APRESULT_APTEXT_*` codes: `NOT_A_FONT` (a caller loading a user's
      font tells the user) and `ATLAS_MISMATCH` (a cache to rebuild,
      expected after a font update, unlike damage).
  19. A collection index past the last font is `APRESULT_NOT_FOUND`, a
      plain font's included: HarfBuzz ignores the index for a plain font,
      so the implementation checks `hb_face_count`.
  20. A capacity too small for everything is `APRESULT_LIMIT` with nothing
      written (`aptext_layout_get_strokes`, `aptext_atlas_save`); dirty
      rectangles are the exception, taken in part.
- **Bidi is tested live, not left pending.** `test_layout_scripts_and_bidi`
  checks a Hebrew run's reversal, a right-to-left paragraph and
  `APTEXT_DIRECTION_AUTO`. Card A19 wires SheenBidi in from the start; an
  implementation that lays out left to right only would fail this test, not
  pend it.

## The implementation (2026-10-29)

`src/text/aptext.cpp` (faces and chains), `aptext_layout.cpp` (layouts) and
`aptext_atlas.cpp` (glyph generation and atlases), behind the contract's
argument checks, unchanged. Containers are eqstd's (`eq::array`,
`eq::vector`), which ApricotFields links from here on.

- [x] Every call, until `aptext_tests` has nothing pending: 41 of 41 pass.
- [x] The two checks the stub left: duplicate codepoints in a stroke face,
      faces of more than one kind in a chain.
- [x] A fuzz target, `aptext_fuzz`: font bytes (laid out, generated into an
      atlas, saved and loaded back), Hershey text (laid out, its strokes
      taken) and saved atlases, chosen by the input's first byte. Fifteen
      minutes on macOS, 2026-10-29: one bug, fixed (below).
- [ ] Run on Windows and Linux: `aptext_bits_tests` there is the
      comparison of a layout's and an atlas's bytes with macOS's (item 5).
      Floating-point contraction is off for MSVC by default; msdfgen's cubic
      solver calls the C library's `acos`, `cos` and `pow`, which can differ
      in the last bit between platforms, and would show here first.

How a build lays text out is described at the top of
`src/text/aptext_layout.cpp`. The calls made where the contract left room
(each also in `../claude_made_decisions.md`, 2026-10-29):

1. **The test's one wrong value.** `test_layout_wrapping` expected the
   hanging space after "aa aa" at x 2.0; it is at 2.25 (two "a"s of 0.5,
   a space of 0.25, two more), as the same test's line box (right edge
   2.25) says. That one value was corrected; nothing else in
   `tests/aptext_tests.c` changed.
2. **One pixel more than half the range around each glyph image.** The
   contract asks for at least range / 2. Off a convex corner an MTSDF's
   colour channels hold the distance to each edge's line, not to the
   corner, so a texel half the range from both lines reads as near the
   outline; with one pixel more the edge texels read as outside, and
   bilinear sampling at an image's edge in an atlas doesn't blend the glyph
   into its neighbour (`test_distance_field` checks the corner texel).
3. **Lines are measured, then shaped again.** A paragraph is shaped once in
   pieces of one face, script and level to measure its clusters; once its
   lines are chosen, each line's runs (in visual order, from SheenBidi's
   line, which applies L1 and L2) are split where the face or script
   changes and shaped again, so shaping at a line's ends sees only the
   line's text. Twice the shaping, never a stale glyph at a break.
4. **Which face a codepoint is drawn from:** the first in the chain with a
   glyph for it; a mark, or a default-ignorable codepoint, stays with the
   face before it where that face has it (an ignorable one always, since it
   gives no glyph), so a base and its marks shape together.
5. **Clusters** are HarfBuzz's at its default level (monotone graphemes):
   a line breaks between them when a word is wider than the line, and
   letter spacing goes after each of them but the line's visually last.
6. **The spaces that hang** at a line's end are Unicode's breaking space
   separators and the tab (not the no-break spaces); the line's box leaves
   out the trailing ones, wherever the paragraph's direction puts them.
7. **U+001C-U+001E** are paragraph separators to the bidirectional
   algorithm but not mandatory breaks here; they reach SheenBidi as U+001F,
   a segment separator, so a paragraph's levels always cover all of it.
8. **A glyph id past the face's last** - a damaged font's substitution,
   which HarfBuzz passes on - is laid out as `.notdef`. Found by the fuzzer
   (an atlas saved from such a layout wouldn't load); the input is kept in
   `tests/fuzz/regressions/aptext_fuzz`.
9. **No glyph image over 16384 pixels a side**: a `pixels_per_em` that
   would make one is refused (`APRESULT_INVALID_ARGUMENT`), and an atlas
   refuses a glyph larger than its page before generating it, not after.
10. **Generation:** the outline from HarfBuzz's draw callbacks, in em; a
    contour set wound the wrong way (CFF against TrueType) is reversed by the
    sign of the distance from a point outside, as msdf-atlas-gen does; edge
    colouring with a fixed seed; msdfgen's MTSDF with overlap support and
    its default error correction; each channel round(255 * clamp(v)), rows
    from the top. The generator's version (`APTEXT_GENERATOR_VERSION`, 1)
    goes up whenever any of this changes what is written.
11. **The atlas** packs bottom-left on a skyline per page (lowest, then
    leftmost); glyphs are found through an open-addressing table by face and
    glyph. Its saved form is little-endian, floats as their bits, with the
    pages' skylines, so a loaded atlas packs on where the saved one would
    have; the CRC-32 is libdeflate's.
12. **What isn't ApricotFields' own** but runs inside it: HarfBuzz keeps
    lazily made, immutable shared data (its Unicode functions, its language
    list), and libunibreak a table it only reads, each in its own static
    library, outside `check.sh mutable-globals`' reading of
    `libApricotFields.a`. SheenBidi allocates per paragraph with `malloc`.

## Items (checked: built 2026-10-29)

### 1. Faces and fallback

- [x] A face from font bytes (TTF/OTF; a collection index for TTC). The
      caller keeps the bytes alive for the face's lifetime, or ApText
      copies them: decide, and say so in the header.
      **Decided 2026-10-28 with the user (card A17):** borrowed by
      default - the header says the bytes must outlive the face, as
      HarfBuzz's and FreeType's memory faces borrow - so a memory-mapped or
      embedded font (CJK fonts reach 10-20 MB) isn't held twice; a copy
      flag on the face's desc has ApText copy them instead, for bytes from
      a network or a temporary buffer.
- [x] Metrics: units per em, ascender, descender, line gap, underline.
- [x] A fallback chain: an ordered list of faces, the first with a glyph
      for a codepoint wins. Shaping runs per face run, since HarfBuzz shapes
      one face at a time. A codepoint no face has becomes the first face's
      `.notdef`, never a skipped glyph.
- [ ] Variable fonts (weight, width axes through HarfBuzz): later.

### 2. Shaping and layout

- [x] UTF-8 in (validated; an invalid sequence is U+FFFD, not a failure).
- [x] Shaping with HarfBuzz: script, language and direction per run,
      features (kerning and ligatures on; the caller can switch them).
- [x] Line layout: max width, wrapping at break opportunities, explicit
      newlines, alignment, line spacing, letter spacing. Output in em
      units or at a caller-given size, as positioned glyph ids with the
      source byte range each came from (for selection and hit testing),
      plus a box per glyph and per line.
- [x] Bidirectional reordering and Unicode line breaking (see
      Dependencies). The first version may handle left-to-right text only
      and say so, as long as the output already carries runs and
      directions.
- [x] Layout is deterministic: the same input gives the same output on
      every platform, so a layout can be cached or compared by its caller.

### 3. The atlas

- [x] MTSDF per glyph at a caller-chosen size in pixels per em and
      distance range (pxRange, default none: both are required fields).
      A range of 4 to 8 pixels leaves room for outlines and halos; Hazel's 2
      doesn't.
- [x] On demand: a glyph is generated the first time a layout uses it, and
      packed with a skyline or shelf packer into pages of a caller-given
      size. A full page starts a new layer; nothing is evicted in the first
      version.
- [x] Dirty rectangles per page since the last call, so the caller uploads
      only what changed, in RGBA8.
- [x] Per glyph: page, atlas rectangle, plane bounds in em units. The
      atlas's distance range is reported with it, for the shader.
- [x] Generation is the expensive part (Hazel measured about 96% of its
      load time there). Threads are the caller's: either a call that
      generates one glyph so the caller can spread them over its own
      threads, or a pre-warm call over a list of glyph ids. No thread of
      ApText's own.
      **Decided 2026-10-28 with the user (card A18):** two steps.
      `aptext_generate_glyph()` renders one glyph's MTSDF into memory the
      caller gives; it only reads the face, which never changes, so it may
      run on many threads at once (a `Thread:` line, the root
      `CLAUDE.md`'s "Threading" exception). `aptext_atlas_insert()` packs a
      generated glyph into the atlas, one caller at a time as every
      object. Pre-warming is the caller's: generation as jobs on its own
      scheduler, then inserts in turn. As Skia's and Chrome's glyph
      rasterising on worker threads.
- [x] Saving and loading an atlas as bytes, so the caller can cache it
      wherever it keeps files.

### 4. Single-stroke fonts

- [ ] Plotter-style single-stroke fonts (1 and 2 built 2026-10-29; LFF
      and SHX not) are open polylines, not filled
      outlines, and have no distance field. Their glyphs come out as
      polylines in em units, laid out by the same layout code, for the
      caller to draw as lines. Which formats to read is undecided.
      **Decided 2026-10-28 with the user (card A20):**
      1. An API taking the caller's polylines per codepoint (with an
         advance), laid out by the same layout code as outline faces,
         out as line segments; where the strokes came from is the
         caller's.
      2. A Hershey reader built in: the Hershey fonts (1967) are public
         domain and may ship with any caller.
      3. LFF (LibreCAD's format) next: the reader is ours; LibreCAD's
         fonts are GPL and can't ship with a closed caller, so it reads
         fonts the user has.
      4. SHX (AutoCAD's compiled shape fonts) only when asked: the format
         is reverse-engineered and its well-known fonts are Autodesk's,
         so it reads the user's own files.
      Drawn as lines, e.g. by Aprend's batcher with pen widths (item 23).

### 5. Tests

- [x] Shaping and layout without a GPU: known strings against known
      glyph ids and advances, fallback, `.notdef`, wrapping, a ligature.
- [x] Atlas: a glyph's distance field sampled at known points, packing
      without overlap, dirty rectangles.
- [ ] The same layout bits on all three platforms: `aptext_bits_tests`,
      passing on macOS; Windows and Linux not run.
