# ApText TODO

ApText doesn't exist yet. It is the planned module that turns text into
positioned glyphs and keeps their distance-field atlas: font faces from the
caller's bytes, shaping, line layout, and MTSDF glyph generation. It is CPU
only and unrelated to Aprend, the way ApMath is; Aprend draws what it hands
back (`APREND_TODO.md`, item 23, "Text: the design").

Planned header `include/text/aptext.h`, prefix `aptext_` / `APTEXT_`, newer
generation (see `CLAUDE.md`, "Writing a new module"): `APRESULT` returns,
`APTEXT_ERROR` per call, descs with a struct base (`APRI_STRUCT_TYPE_APTEXT_*`
in a block of its own), its own numbered block of
`APRESULT_APTEXT_*` codes in `apricore.h` if it needs any.

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

- [ ] **HarfBuzz** (MIT): shaping, and glyph outlines through
      `hb_font_draw_glyph`, so FreeType isn't needed at all.
- [ ] **msdfgen** core (MIT): MTSDF generation from an outline built from
      HarfBuzz's draw callbacks. Not msdfgen's `ext` (that is FreeType and
      file loading) and not msdf-atlas-gen (its packer assumes a fixed
      glyph set; ours is on demand).
- [ ] **Bidirectional text and line breaking:** SheenBidi plus libunibreak
      (small), or ICU (complete, large). Undecided; neither is needed for
      the first version, but the layout API must have room for both.
- [ ] Vendored under `../.deps/` like the repo's other third-party code
      (`CLAUDE.md`, "Build"), pinned, linked `PRIVATE` (no HarfBuzz or
      msdfgen type in `include/`), and built under this repo's flags on all
      three platforms. HarfBuzz's shaping must give the same output
      everywhere for layout to be deterministic (item 2), so pin it and
      check it the way ApMath's polygons are checked.

## Not built

### 1. Faces and fallback

- [ ] A face from font bytes (TTF/OTF; a collection index for TTC). The
      caller keeps the bytes alive for the face's lifetime, or ApText
      copies them: decide, and say so in the header.
- [ ] Metrics: units per em, ascender, descender, line gap, underline.
- [ ] A fallback chain: an ordered list of faces, the first with a glyph
      for a codepoint wins. Shaping runs per face run, since HarfBuzz shapes
      one face at a time. A codepoint no face has becomes the first face's
      `.notdef`, never a skipped glyph.
- [ ] Variable fonts (weight, width axes through HarfBuzz): later.

### 2. Shaping and layout

- [ ] UTF-8 in (validated; an invalid sequence is U+FFFD, not a failure).
- [ ] Shaping with HarfBuzz: script, language and direction per run,
      features (kerning and ligatures on; the caller can switch them).
- [ ] Line layout: max width, wrapping at break opportunities, explicit
      newlines, alignment, line spacing, letter spacing. Output in em
      units or at a caller-given size, as positioned glyph ids with the
      source byte range each came from (for selection and hit testing),
      plus a box per glyph and per line.
- [ ] Bidirectional reordering and Unicode line breaking (see
      Dependencies). The first version may handle left-to-right text only
      and say so, as long as the output already carries runs and
      directions.
- [ ] Layout is deterministic: the same input gives the same output on
      every platform, so a layout can be cached or compared by its caller.

### 3. The atlas

- [ ] MTSDF per glyph at a caller-chosen size in pixels per em and
      distance range (pxRange, default none: both are required fields).
      A range of 4 to 8 pixels leaves room for outlines and halos; Hazel's 2
      doesn't.
- [ ] On demand: a glyph is generated the first time a layout uses it, and
      packed with a skyline or shelf packer into pages of a caller-given
      size. A full page starts a new layer; nothing is evicted in the first
      version.
- [ ] Dirty rectangles per page since the last call, so the caller uploads
      only what changed, in RGBA8.
- [ ] Per glyph: page, atlas rectangle, plane bounds in em units. The
      atlas's distance range is reported with it, for the shader.
- [ ] Generation is the expensive part (Hazel measured about 96% of its
      load time there). Threads are the caller's: either a call that
      generates one glyph so the caller can spread them over its own
      threads, or a pre-warm call over a list of glyph ids. No thread of
      ApText's own.
- [ ] Saving and loading an atlas as bytes, so the caller can cache it
      wherever it keeps files.

### 4. Single-stroke fonts

- [ ] Plotter-style single-stroke fonts are open polylines, not filled
      outlines, and have no distance field. Their glyphs come out as
      polylines in em units, laid out by the same layout code, for the
      caller to draw as lines. Which formats to read is undecided.

### 5. Tests

- [ ] Shaping and layout without a GPU: known strings against known
      glyph ids and advances, fallback, `.notdef`, wrapping, a ligature.
- [ ] Atlas: a glyph's distance field sampled at known points, packing
      without overlap, dirty rectangles.
- [ ] The same layout bits on all three platforms.
