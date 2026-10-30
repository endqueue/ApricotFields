# ApArchive TODO

`include/archive/aparchive.h` reads and writes ZIP archives: STORED and DEFLATE
(or any registered codec), entries whole or streamed in pieces, raw entries,
updating, ZIP64, aligned data, caller extra fields, attributes, comments,
directory entries, reader limits and strict reading.

Items 1-10 below were what a general-purpose ZIP library had that this one
didn't, for the products that will need it (a game engine's asset packs, CGI
scene and cache files, mechanical CAD); all ten were built on 2026-10-30.

## The rule for adding any of it

Nothing here may change existing files or existing callers:

- **Files:** today's calls keep writing exactly the bytes they write now, and
  today's reader keeps opening every archive it opens now. The writer's output is
  plain ZIP, so a later feature is a different set of standard ZIP records, used
  only by the entries that ask for it.
- **Callers:** a feature is a new function, or a new struct chained onto an
  existing one (`apricore.h`, "Struct bases"). A caller that doesn't chain it on
  gets what it gets now, so leaving it out must mean "as before".

Each item says how it fits that rule.

## Built (2026-10-30, macOS): items 1-10

All ten, under the rule above: new calls, and fields at the end of
`APARCHIVE_CODEC`, `APARCHIVE_WRITER_DESC`, `APARCHIVE_ENTRY_DESC`,
`APARCHIVE_ENTRY_INFO` and `APARCHIVE_READER_DESC` whose zero is the old
behaviour (pre-1.0 ABI breaks, each listed against `abi.txt`; source is
unchanged for a caller that zeroes its structs). `test_old_bytes_unchanged`
holds the rule: an archive made with every call and field that existed
before, its CRC-32 and size recorded from the code as it was before these
items were written, and the same after. The reader opens every archive it
opened before: the shift (item 10) is taken only when the directory isn't
where the end record says.

1. **Streamed entries.** `aparchive_writer_begin_entry` /
   `write_entry_data` / `end_entry` (general-purpose bit 3, a data
   descriptor with its signature, 8-byte sizes under `zip64`; an entry
   reaching 4 GiB without `zip64` is `APRESULT_LIMIT` before a byte past
   it); `aparchive_reader_open_entry` and `aparchive_entry_reader_read` /
   `_destroy`, the CRC-32 and size checked by the read that hands out the
   last byte. `APARCHIVE_CODEC` gains `stream_begin`, `stream_process` and
   `stream_end` (all or none). DEFLATE streams through zlib-ng 2.3.3,
   native mode (card A24), raw DEFLATE, window 15, memory level 8, default
   strategy, levels 0-9 (10 and up refused for a streamed entry); whole
   entries stay libdeflate's. A streamed archive's bytes are recorded in
   the tests (`test_streamed_entries`) so a platform whose zlib-ng picks
   other matches fails. Known difference, written into the header:
   libdeflate decodes DEFLATE data that uses a code its Huffman table left
   unassigned; zlib-ng refuses it, so such an entry reads whole and fails
   in pieces with `APRESULT_CORRUPT_DATA` (found by `aparchive_fuzz`, kept
   in its regressions).
2. **Raw entries.** `aparchive_writer_add_raw_entry` (any method, the
   codec table not needed; STORED's sizes and CRC-32 checked, data with no
   stored bytes refused) and `aparchive_codecs_compress_bound` /
   `aparchive_codecs_compress`, which give exactly the bytes
   `add_entry` stores (tested: an archive written raw equals one written
   whole).
3. **Updating.** `APARCHIVE_UPDATE_DESC` (struct type 1005: the reader, a
   `truncate` callback, the sink, codecs) and
   `aparchive_writer_create_update`; `aparchive_writer_remove_entry`
   drops an entry, so replacing is remove and add. Kept entries are not
   read or moved; their central records are written again as read (less
   ZIP64 records, rewritten where needed), so updating with no change
   gives the same bytes. The cut happens just before the first byte is
   written: a writer destroyed unused leaves the archive alone. An archive
   with data in front is updated with true offsets.
4. **Directory entries:** `APARCHIVE_WRITER_DESC::allow_directory_entries`
   (STORED and empty; never streamed). Symbolic links are item 5's Unix
   mode (0120777) with the target as data.
5. **Attributes:** `host_system`, `internal_attributes`,
   `external_attributes` on the entry desc; `version_made_by`,
   `internal_attributes`, `external_attributes` on the info.
6. **Central extra:** `central_extra` / `central_extra_size` (NULL: the
   local extra, as before; non-NULL with size 0: none).
7. **Comments:** `comment` / `comment_size` on the desc and the info;
   `aparchive_writer_set_comment` (refusing the end record's signature
   inside it) and `aparchive_reader_get_comment`.
8. **Extra-field helpers:** `aparchive_extra_find`; the extended timestamp
   (0x5455), NTFS times (0x000A) and Unicode path (0x7075) read and
   written, times as `APARCHIVE_FILE_TIMES` (100 ns ticks since 1970,
   frozen).
9. **Strict reading:** `APARCHIVE_READER_DESC::strict`, `APARCHIVE_STRICT`
   bits: no archive comment, no entry comments, no directory entries, no
   gaps (reading every local header and data descriptor on open, the
   descriptor checked against the directory). A refusal is
   `APRESULT_APARCHIVE_MALFORMED`; an unknown bit is
   `APRESULT_INVALID_ARGUMENT`.
10. **Data in front:** the shift found from where the directory (and the
    ZIP64 end record) actually sits; every offset reported is the
    source's; `aparchive_reader_get_offset_shift`.

`aparchive_fuzz` covers each new reader path: pieces against whole reads,
the four strict flags, the comment, the shift, the extra helpers on every
entry, an update (one entry dropped, one added) reopened with every kept
entry as it was, a raw copy of every entry, and the rewrite streaming
every other entry. Also converted: the writer is allocated by hand
(`CLAUDE.md`, "Code conventions"), as the entry reader is.

**What has run where:** macOS (Apple M5 Pro, Debug), 2026-10-30: the
tests plain, under ASan and UBSan, and under TSan; `aparchive_fuzz` (see
"Testing still owed"). **Windows and Linux have built none of it:** there,
zlib-ng 2.3.3 through its own CMake under MSVC and GCC, `aparchive_tests`
(whose recorded streamed-DEFLATE bytes are the cross-platform check), the
fuzz target's replay, Windows' `-asan` and Linux's `-asan` and `-tsan`.

## Smaller additions

- **Custom memory allocator.** Game engines expect to supply their own; today it's
  always the global allocator. Paused 2026-10-28 for every library (card
  X15); `eqstd`'s containers take an allocator when one is decided.
- **Progress and cancellation callbacks** for long compressions and reads.
- **Verifying an entry's CRC without returning its data,** for callers that
  memory-map stored entries and still want them checked.
- **Archive-level info:** whether ZIP64 is used, how many bytes come before the
  first entry.
- **Case-insensitive collision detection** when extracting to disk on Windows or
  macOS (`A.txt` and `a.txt`), as an option. Unicode normalization (NFC vs NFD)
  only needs a documented note.
- **A decompressor cache.** The DEFLATE codec allocates a libdeflate compressor or
  decompressor per call so it's safe from any thread; a per-thread cache would
  save that for archives with many small entries.

## Testing still owed

- **Fuzz the reader** - done 2026-10-28 on macOS (`tests/fuzz/aparchive_fuzz.cpp`,
  the `apricotfields-macos-metal-fuzz` preset; `CLAUDE.md`, "Tests"): open,
  every entry's info, name lookup, data offset and read, and the writer
  writing back what was read. About 14 minutes and 80 million inputs after
  the fix, with ASan and UBSan. It found one bug, fixed: the names buffer was
  reserved for an archive whose every entry fits, so a directory whose entry
  count promised more than it held grew the buffer under the name index's
  keys (a use-after-free, `tests/fuzz/regressions/aparchive_fuzz/`).
  Extended 2026-10-30 to items 1-10's reader paths (see "Built"): in its
  first minutes it found whole and streamed DEFLATE disagreeing on a
  stream with an unassigned Huffman code (kept as
  `deflate_unassigned_code_whole_only.zip`, documented, not a bug in
  either); then 15 minutes, 2,986,408 inputs, Debug, ASan and UBSan,
  found nothing. Still owed: running it on Linux and Windows, and longer
  runs on the CI machine.
- **Other tools:** CI opens what the tests write with Python's `zipfile` and
  Info-ZIP `unzip -t`, and ApArchive opens archives those tools wrote.
- **Real ZIP64 sizes:** the tests reach the ZIP64 records through an entry count
  over 65,535 and through `APARCHIVE_ENTRY_DESC::zip64`, never through an entry or
  an archive over 4 GiB.

## Out of scope, and the header says so

- **Encryption** (ZipCrypto or WinZip AES).
- **Split and multi-disk archives.**
