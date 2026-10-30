#ifndef APARCHIVE_H
#define APARCHIVE_H

#include "../apricore.h"
#include <stdbool.h>
#include <stdint.h>

#if __cplusplus
extern "C" {
#endif

/**
 * @file aparchive.h
 * @brief ApArchive - ZIP archives (with ZIP64), read and written through
 * caller callbacks.
 *
 * General purpose: it knows nothing about what an archive holds. Every
 * layout decision - entry order, compression method and level, data
 * alignment, timestamps, extra fields - is the caller's, and the writer adds
 * nothing it wasn't asked for, so the same input always gives the same
 * bytes.
 *
 * It does no I/O of its own. A reader reads through an APARCHIVE_SOURCE
 * (random access); a writer writes through an APARCHIVE_SINK (append only,
 * never seeks). It reports where each entry's data sits in the archive, so a
 * caller can map or range-read an entry's bytes itself.
 *
 * Entries are written and read whole (one buffer in, one buffer out) or
 * streamed in pieces, for an entry bigger than memory or of a size not
 * known up front.
 *
 * Opening reads only the end of the archive and its central directory; each
 * entry is read only when asked for.
 *
 * What it writes is plain ZIP: any ZIP tool reads it. What it reads is
 * plain ZIP from any tool, including what its own writer never produces -
 * an archive comment, directory entries, entries written with a data
 * descriptor, extra field records it doesn't know.
 *
 * Each feature added since the first (streamed and raw entries, updating,
 * directory entries, attributes, comments, separate central extra fields,
 * strict reading) is a new call or a field at the end of a struct whose
 * zero is the old behaviour: none changes the bytes the older calls write,
 * and the reader opens every archive it opened before, and more (data in
 * front of an archive that its offsets don't count).
 *
 * Out of scope: encryption (ZipCrypto, WinZip AES) and split or multi-disk
 * archives - reading one fails with APRESULT_UNSUPPORTED.
 *
 * @par Threads
 * apricore.h's rule ("Threads"): a writer, and a codec table while it is
 * being changed, is used by one caller at a time. What is particular here:
 * a reader never changes after aparchive_reader_open(), so its read calls
 * may overlap each other, from any number of threads, provided the
 * source's read callback can be called that way; the calls that may are
 * marked. Readers and writers only read their codec table, so one table
 * may be used by several of them at once, as long as nothing adds to it or
 * destroys it meanwhile; and a codec's callbacks may then be called from
 * several threads at once.
 *
 * @par Results
 * Calls return APRESULT (apricore.h): APRESULT_OK, a general code, or an
 * APRESULT_APARCHIVE_* one. The general codes mean, here:
 *   - APRESULT_INVALID_ARGUMENT: also an unsafe entry name, or a call in the
 *     wrong state (a writer finished or failed, or with a streamed entry
 *     open, or none).
 *   - APRESULT_IO: the source or sink callback returned false.
 *   - APRESULT_LIMIT: over an APARCHIVE_READER_DESC limit.
 *   - APRESULT_UNSUPPORTED: encryption, multiple disks, patch data, or
 *     compressing with a decode-only codec.
 *   - APRESULT_CORRUPT_DATA: an entry that fails its CRC-32 or doesn't
 *     decompress to its declared size.
 *   - APRESULT_APARCHIVE_MALFORMED: also an archive refused by a reader's
 *     `strict` flags.
 *
 * @par Error messages
 * Every call that can fail for a reason worth a sentence takes an
 * APARCHIVE_ERROR pointer (NULL to skip it): on failure it holds a
 * description naming the entry and what was wrong; on success its message
 * is empty. Being per call, not on the reader, it lets a reader's calls
 * overlap (see "Threads").
 *
 * @par Struct bases
 * Every struct that may grow starts with a `base` (apricore.h, "Struct
 * bases"): the caller sets `base.type` to the struct's own
 * APRI_STRUCT_TYPE_APARCHIVE_* and `base.next` to NULL, on
 * APARCHIVE_ENTRY_INFO too, which ApArchive fills in. A wrong type is
 * APRESULT_STRUCT_WRONG_TYPE and a chained struct
 * APRESULT_STRUCT_UNKNOWN_IN_CHAIN: nothing chains onto any of them yet.
 * APARCHIVE_SOURCE, APARCHIVE_SINK, APARCHIVE_ERROR and
 * APARCHIVE_FILE_TIMES are frozen and have none.
 */

/* ---- Conventions ----------------------------------------------------- */

/** @brief The size of APARCHIVE_ERROR's `message`, its NUL included. */
#define APARCHIVE_ERROR_MESSAGE_SIZE 256

/**
 * @brief Why a call failed, filled per call (aparchive.h, "Error
 * messages").
 */
/* Frozen: filled per call. It never changes; a new need gets a new type. */
typedef struct APARCHIVE_ERROR {
	char message[APARCHIVE_ERROR_MESSAGE_SIZE]; /**< NUL-terminated, truncated to fit; empty on success. */
} APARCHIVE_ERROR;

/* ---- I/O ------------------------------------------------------------- */

/**
 * @brief Random-access input for a reader.
 *
 * Offsets come in any order.
 *
 * Thread: `read` is called from inside the reader's calls, on the thread of
 * each; when the caller overlaps a reader's calls (see "Threads", at the top
 * of this header), it is called from several threads at once.
 */
/* Frozen: held by value in APARCHIVE_READER_DESC. It never changes; a new need gets a new type. */
typedef struct APARCHIVE_SOURCE {
	/** Passed to #read as its `user`. */
	void *user;
	/** The whole archive's size in bytes. */
	uint64_t size;
	/** Copies exactly `count` bytes at `offset` into `buffer`, and returns
	 * false on any failure, including a short read. Never NULL. */
	bool (*read)(void *user, uint64_t offset, void *buffer, uint64_t count);
} APARCHIVE_SOURCE;

/**
 * @brief Append-only output for a writer.
 */
/* Frozen: held by value in APARCHIVE_WRITER_DESC. It never changes; a new need gets a new type. */
typedef struct APARCHIVE_SINK {
	/** Passed to #write as its `user`. */
	void *user;
	/** Appends `count` bytes of `data` and returns false on failure, which
	 * fails the call that was writing. Never NULL. */
	bool (*write)(void *user, const void *data, uint64_t count);
} APARCHIVE_SINK;

/* ---- Codecs ---------------------------------------------------------- */

/*
 * A codec table says which compression methods a reader can decode and a
 * writer can encode. Each reader or writer is given one; there's no global
 * table, so what one product registers never changes what another product's
 * archives accept. A table must not change while a reader or writer uses
 * it: add every codec first.
 *
 * STORED (no compression) is part of ZIP itself and always available. A new
 * table has nothing else - add DEFLATE with aparchive_codecs_add_deflate(),
 * and any other method with aparchive_codecs_add().
 */

/** @brief ZIP method 0: the data as is, with no compression. Always available. */
#define APARCHIVE_METHOD_STORED 0
/** @brief ZIP method 8: DEFLATE, added with aparchive_codecs_add_deflate(). */
#define APARCHIVE_METHOD_DEFLATE 8

/**
 * @brief A compression method, by its ZIP method id.
 *
 * Methods other than STORED and DEFLATE (zstd is 93, LZMA 14, ...) aren't
 * read by most ZIP tools: an archive using them only opens in software that
 * registers them too.
 *
 * A decode-only codec leaves `compress` and `compress_bound` NULL. A codec
 * with the three stream callbacks also works on entries in pieces
 * (aparchive_writer_begin_entry(), aparchive_reader_open_entry()); one
 * without them works on whole entries only.
 *
 * Thread: the callbacks are called from inside the reader's and writer's
 * calls, on the thread of each; with readers or writers used at once, or a
 * reader's calls overlapping, from several threads at once.
 *
 * @see aparchive_codecs_add()
 */
typedef struct APARCHIVE_CODEC {
	/** This struct's type, APRI_STRUCT_TYPE_APARCHIVE_CODEC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** The ZIP method id. Never APARCHIVE_METHOD_STORED. */
	uint16_t method;
	/** Passed to each callback as its `user`. */
	void *user;
	/** The most bytes compressing `in_size` bytes can produce. NULL for a
	 * decode-only codec. */
	uint64_t (*compress_bound)(void *user, uint64_t in_size);
	/** Compresses `in` into `out` (at least #compress_bound bytes) at
	 * `level`, whose meaning is the codec's; sets `out_size`; false on
	 * failure. NULL for a decode-only codec. */
	bool (*compress)(void *user, int32_t level, const void *in, uint64_t in_size, void *out, uint64_t out_capacity,
	                 uint64_t *out_size);
	/** Decompresses `in` into exactly `out_size` bytes; false if the data is
	 * malformed or decompresses to any other size. Never NULL. */
	bool (*decompress)(void *user, const void *in, uint64_t in_size, void *out, uint64_t out_size);
	/** Starts one entry's stream: compressing at `level` if `compress`
	 * (`level` as #compress takes it, or its own range, which the codec
	 * documents), decompressing if not (`level` 0). Returns the stream's
	 * state, handed to #stream_process and #stream_end, or NULL on failure
	 * (a level it doesn't take, no memory). NULL, with the other two, for a
	 * codec that works on whole entries only; never NULL alone. */
	void *(*stream_begin)(void *user, bool compress, int32_t level);
	/** Takes up to `in_size` bytes of `in`, setting `in_used` to how many it
	 * took, and writes up to `out_capacity` bytes to `out`, setting
	 * `out_size`. `finish` says no input follows `in`: a compressing stream
	 * then writes everything it holds, and sets `done` once it has; a
	 * decompressing one sets `done` once it reaches its data's end, finish
	 * or not. Called again while it takes input or gives output; one call
	 * that does neither with `finish` set and `done` unset is a failure.
	 * False on failure: malformed data, when decompressing. */
	bool (*stream_process)(void *user, void *state, const void *in, uint64_t in_size, uint64_t *in_used, void *out,
	                       uint64_t out_capacity, uint64_t *out_size, bool finish, bool *done);
	/** Frees a stream's state, whether it finished or not. */
	void (*stream_end)(void *user, void *state);
} APARCHIVE_CODEC;

/**
 * @brief Opaque handle to a codec table.
 *
 * Thread: readers and writers only read it, so any number of them may use
 * one table at once, from any threads, while no call adds to it or destroys
 * it; aparchive_codecs_has() is such a read too.
 */
typedef struct aparchive_codecs_t *aparchive_codecs;

/**
 * @brief Creates an empty codec table: STORED only.
 *
 * @param[out] out_codecs The new table.
 *
 * @return APRESULT_OK, APRESULT_INVALID_ARGUMENT if @p out_codecs is NULL,
 *         or APRESULT_OUT_OF_MEMORY.
 *
 * @see aparchive_codecs_destroy()
 */
APRESULT aparchive_codecs_create(aparchive_codecs *out_codecs);
/**
 * @brief Copies a codec into a table.
 *
 * @param[in] codecs The table.
 * @param[in] codec  The codec. Only read during the call.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT for a NULL argument, method
 *         STORED, a method already in the table, no `decompress`, only one
 *         of `compress` and `compress_bound`, or only some of the three
 *         stream callbacks; a struct-base code for a wrong `base`; or
 *         APRESULT_OUT_OF_MEMORY.
 */
APRESULT aparchive_codecs_add(aparchive_codecs codecs, const APARCHIVE_CODEC *codec);
/**
 * @brief Adds DEFLATE, built on libdeflate.
 *
 * Whole entries are compressed by libdeflate, at levels 0-12: 0 stores
 * without compressing (still as DEFLATE), 1 is fastest, 12 smallest. Its
 * output for a given input and level is fixed by the libdeflate version
 * ApricotFields builds with - pinned, so archives stay byte-identical.
 *
 * Streamed entries are compressed by zlib-ng (native mode), at levels 0-9,
 * zlib's: 0 stores, 1 is fastest, 9 smallest; a streamed entry at level 10
 * or above is refused. Its bytes differ from libdeflate's for the same data
 * and are fixed by the zlib-ng version, also pinned. Both decompress any
 * DEFLATE data, so a reader neither knows nor cares which wrote an entry.
 *
 * @param[in] codecs The table.
 *
 * @return As aparchive_codecs_add(): APRESULT_INVALID_ARGUMENT if
 *         @p codecs is NULL or already has DEFLATE.
 */
APRESULT aparchive_codecs_add_deflate(aparchive_codecs codecs);
/**
 * @brief Whether a table can handle a method.
 *
 * @param[in] codecs The table.
 * @param[in] method A ZIP method id.
 *
 * @return True if @p method is STORED or in the table; false if not, or if
 *         @p codecs is NULL.
 */
bool aparchive_codecs_has(aparchive_codecs codecs, uint16_t method);

/**
 * @brief The most bytes aparchive_codecs_compress() can produce from
 * @p in_size bytes.
 *
 * Thread: a read of the table, as a writer's (see aparchive_codecs).
 *
 * @param[in]  codecs    The table.
 * @param[in]  method    STORED, or a method in the table that compresses.
 * @param[in]  in_size   The size of the data to compress, in bytes.
 * @param[out] out_bound The bound.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT for a NULL argument;
 *         APRESULT_APARCHIVE_UNKNOWN_METHOD if @p method isn't in the
 *         table; or APRESULT_UNSUPPORTED if its codec only decompresses.
 */
APRESULT aparchive_codecs_compress_bound(aparchive_codecs codecs, uint16_t method, uint64_t in_size,
                                         uint64_t *out_bound);

/**
 * @brief Compresses data as aparchive_writer_add_entry() would store it,
 * byte for byte, for aparchive_writer_add_raw_entry(): so entries can be
 * compressed on the caller's threads and written in order.
 *
 * STORED copies @p in.
 *
 * Thread: a read of the table, as a writer's (see aparchive_codecs).
 *
 * @param[in]  codecs       The table.
 * @param[in]  method       STORED, or a method in the table that compresses.
 * @param[in]  level        The codec's level; ignored for STORED.
 * @param[in]  in           The data. May be NULL when @p in_size is 0.
 * @param[in]  in_size      The size of @p in in bytes.
 * @param[out] out          Receives the compressed bytes. May be NULL when
 *                          @p out_capacity is 0.
 * @param[in]  out_capacity The size of @p out: at least
 *                          aparchive_codecs_compress_bound()'s.
 * @param[out] out_size     The size of the compressed bytes.
 * @param[out] out_error    Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT for a NULL argument, an
 *         @p out_capacity under the bound, or a level the codec refuses;
 *         APRESULT_APARCHIVE_UNKNOWN_METHOD if @p method isn't in the
 *         table; or APRESULT_UNSUPPORTED if its codec only decompresses.
 */
APRESULT aparchive_codecs_compress(aparchive_codecs codecs, uint16_t method, int32_t level, const void *in,
                                   uint64_t in_size, void *out, uint64_t out_capacity, uint64_t *out_size,
                                   APARCHIVE_ERROR *out_error);

/**
 * @brief Destroys a codec table.
 *
 * @warning Destroy every reader and writer using the table first.
 *
 * @param[in] codecs The table. NULL is accepted and does nothing.
 */
void aparchive_codecs_destroy(aparchive_codecs codecs);

/* ---- Writing --------------------------------------------------------- */

/** @brief Configuration descriptor for a writer. */
typedef struct APARCHIVE_WRITER_DESC {
	/** This struct's type, APRI_STRUCT_TYPE_APARCHIVE_WRITER_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** Where the archive is written. Its `write` is never NULL. */
	APARCHIVE_SINK sink;
	/** The methods the writer can encode. Never NULL. Must outlive the
	 * writer. */
	aparchive_codecs codecs;
	/** Lets an entry's name end in '/': a directory entry, STORED and
	 * empty. False refuses them, as before: directories then exist only as
	 * name prefixes. */
	bool allow_directory_entries;
} APARCHIVE_WRITER_DESC;

/** @brief Opaque handle to an archive writer. */
typedef struct aparchive_writer_t *aparchive_writer;

/**
 * @brief Creates a writer. Nothing is written until the first entry.
 *
 * @param[in]  desc       Writer configuration. Only read during the call.
 * @param[out] out_writer The new writer; NULL on failure.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT for a NULL argument, a
 *         NULL `sink.write` or a NULL `codecs`; a struct-base code for a
 *         wrong `base`; or APRESULT_OUT_OF_MEMORY.
 *
 * @see aparchive_writer_destroy()
 */
APRESULT aparchive_writer_create(const APARCHIVE_WRITER_DESC *desc, aparchive_writer *out_writer);

/**
 * @brief One entry to write.
 *
 * Every header field ApArchive writes comes from here or is a fixed value -
 * nothing is taken from the clock, the file system or the platform.
 */
typedef struct APARCHIVE_ENTRY_DESC {
	/** This struct's type, APRI_STRUCT_TYPE_APARCHIVE_ENTRY_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** Valid UTF-8, '/'-separated, passing aparchive_is_safe_name(), and not
	 * a name already in the archive. A name with any non-ASCII byte needs
	 * #utf8_name_flag: without it, readers take the name as CP437. A name
	 * ending in '/' is a directory entry: refused unless the writer's desc
	 * sets `allow_directory_entries`, and then STORED with no data. */
	const char *name;
	/** STORED, or a method in the writer's codec table. */
	uint16_t method;
	/** The codec's level; ignored for STORED. */
	int32_t level;
	/** Where the entry's data starts in the archive: a power of two, 0 or 1
	 * for anywhere. Reached by padding the local header with one extra
	 * field record of zeros (id 0xD935, the id Android's zipalign uses),
	 * so alignment adds at least 4 bytes whenever padding is needed. */
	uint32_t alignment;
	/** MS-DOS date, written as given (date 0x0021 / time 0 is
	 * 1980-01-01 00:00:00, the earliest representable). */
	uint16_t dos_date;
	/** MS-DOS time, written as given. @see #dos_date */
	uint16_t dos_time;
	/** Sets general-purpose flag bit 11: the name, and #comment, are UTF-8. */
	bool utf8_name_flag;
	/** Caller extra field records (id, size, data - already encoded),
	 * written into the local header, and into the central header unless
	 * #central_extra is set. NULL/0 for none. Must be whole records, none with id 0x0001 (ZIP64, which
	 * ApArchive writes itself). With no extra, no alignment padding and
	 * sizes under 4 GiB, a local header has no extra field at all. */
	const void *extra;
	/** The size of #extra in bytes; 0 for none. */
	uint16_t extra_size;
	/** Write this entry's sizes in ZIP64 fields even though they'd fit
	 * without. Off, an entry gets ZIP64 fields only when its sizes or
	 * offset need them - and a streamed entry, whose sizes aren't known
	 * when its header is written, may then not reach 4 GiB. */
	bool zip64;
	/** The host system in "version made by" (its upper byte), which says
	 * how to read #external_attributes: 0 MS-DOS (as before), 3 Unix, 19
	 * OS X; any value is written as given. */
	uint8_t host_system;
	/** The internal file attributes, written as given (bit 0: text). 0
	 * for none. */
	uint16_t internal_attributes;
	/** The external file attributes, written as given, in #host_system's
	 * form. For Unix, the mode in the upper 16 bits: 0100755 << 16 for an
	 * executable, 0120777 << 16 for a symbolic link, whose data is then the
	 * link's target. For MS-DOS, its attribute bits in the lowest byte. 0
	 * for none: with host 0 that is the old behaviour; with a Unix host,
	 * tools that extract take it as "no permissions". */
	uint32_t external_attributes;
	/** Extra field records for the central header alone, under the rules
	 * #extra follows. NULL: the central header has #extra, as before.
	 * Otherwise #central_extra_size bytes, 0 for none. */
	const void *central_extra;
	/** The size of #central_extra in bytes. */
	uint16_t central_extra_size;
	/** The entry's comment, written into its central header as given:
	 * UTF-8 if #utf8_name_flag, else CP437. NULL/0 for none. */
	const void *comment;
	/** The size of #comment in bytes. */
	uint16_t comment_size;
} APARCHIVE_ENTRY_DESC;


/**
 * @brief Compresses an entry's data (whole) with the entry's method and
 * appends the entry.
 *
 * Sizes and CRC-32 are known before the header is written, so no data
 * descriptor is used.
 *
 * @param[in]  writer    The writer.
 * @param[in]  desc      The entry. Only read during the call.
 * @param[in]  data      The entry's uncompressed bytes. May be NULL when
 *                       @p size is 0.
 * @param[in]  size      The size of @p data in bytes.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK, or a code from aparchive.h's "Results". An entry
 *         refused for its arguments writes nothing and leaves the writer
 *         usable; a sink failure or running out of memory doesn't (see
 *         aparchive_writer_destroy()).
 */
APRESULT aparchive_writer_add_entry(aparchive_writer writer, const APARCHIVE_ENTRY_DESC *desc, const void *data,
                                    uint64_t size, APARCHIVE_ERROR *out_error);

/**
 * @brief Appends an entry whose data is already stored as its method
 * stores it: copied from another archive without recompressing it, or
 * compressed on the caller's threads (aparchive_codecs_compress()).
 *
 * Nothing is compressed or checked against the data: the sizes and CRC-32
 * are the caller's, written as given, except that a STORED entry's CRC-32
 * is checked. `level` is ignored, and the method needn't be in the
 * writer's codec table, so an entry in a method this writer can't encode
 * can still be copied.
 *
 * @param[in]  writer            The writer.
 * @param[in]  desc              The entry. Only read during the call.
 * @param[in]  stored            The entry's bytes as stored. May be NULL
 *                               when @p stored_size is 0.
 * @param[in]  stored_size       The size of @p stored in bytes.
 * @param[in]  uncompressed_size The size of the data uncompressed, in bytes.
 * @param[in]  crc32             The CRC-32 of the uncompressed data.
 * @param[out] out_error         Why it failed, when it did. May be NULL.
 *
 * @return As aparchive_writer_add_entry(); APRESULT_INVALID_ARGUMENT also
 *         for a STORED entry whose sizes differ or whose CRC-32 is wrong,
 *         or data with no stored bytes.
 */
APRESULT aparchive_writer_add_raw_entry(aparchive_writer writer, const APARCHIVE_ENTRY_DESC *desc, const void *stored,
                                        uint64_t stored_size, uint64_t uncompressed_size, uint32_t crc32,
                                        APARCHIVE_ERROR *out_error);

/**
 * @brief Starts an entry whose data comes in pieces: an entry bigger than
 * memory, or of a size not known up front.
 *
 * Its local header is written now, with general-purpose flag bit 3 and no
 * sizes or CRC-32: a data descriptor after the data holds them, written by
 * aparchive_writer_end_entry(). The data goes in with
 * aparchive_writer_write_entry_data(). Until the entry ends, no other entry
 * can be added and the writer can't finish.
 *
 * Its method is STORED or a codec with stream callbacks; @p desc's level is
 * the codec's stream level (DEFLATE's are 0-9, see
 * aparchive_codecs_add_deflate()). An entry that may reach 4 GiB sets
 * `zip64`.
 *
 * @param[in]  writer    The writer.
 * @param[in]  desc      The entry, as aparchive_writer_add_entry() takes it.
 *                       Only read during the call.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return As aparchive_writer_add_entry(); APRESULT_UNSUPPORTED also for a
 *         method whose codec has no stream callbacks; and
 *         APRESULT_INVALID_ARGUMENT for a directory entry, which has no
 *         data to stream, or a level the codec refuses.
 */
APRESULT aparchive_writer_begin_entry(aparchive_writer writer, const APARCHIVE_ENTRY_DESC *desc,
                                      APARCHIVE_ERROR *out_error);

/**
 * @brief Adds the next piece of the open entry's data.
 *
 * Compressed bytes are written to the sink as the codec gives them, so
 * memory doesn't grow with the entry.
 *
 * @param[in]  writer    The writer.
 * @param[in]  data      The piece. May be NULL when @p size is 0.
 * @param[in]  size      The size of @p data in bytes.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT with no entry open;
 *         APRESULT_LIMIT if the entry reaches 4 GiB without `zip64`; or
 *         another code from aparchive.h's "Results". Anything but a refused
 *         argument leaves the writer unusable: part of the entry is
 *         written.
 */
APRESULT aparchive_writer_write_entry_data(aparchive_writer writer, const void *data, uint64_t size,
                                           APARCHIVE_ERROR *out_error);

/**
 * @brief Ends the open entry: flushes its codec and writes its data
 * descriptor.
 *
 * @param[in]  writer    The writer.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT with no entry open;
 *         APRESULT_LIMIT as aparchive_writer_write_entry_data(); or another
 *         code from aparchive.h's "Results", leaving the writer unusable.
 */
APRESULT aparchive_writer_end_entry(aparchive_writer writer, APARCHIVE_ERROR *out_error);

/**
 * @brief Sets the archive's comment, which aparchive_writer_finish()
 * writes into the end record. A writer starts with none, or, updating, with
 * the archive's own.
 *
 * @param[in]  writer       The writer.
 * @param[in]  comment      The comment's bytes, copied. May be NULL when
 *                          @p comment_size is 0, for none.
 * @param[in]  comment_size The size of @p comment in bytes.
 * @param[out] out_error    Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; or APRESULT_INVALID_ARGUMENT for a NULL argument, a
 *         writer finished or unusable, or a comment holding the end
 *         record's signature (the bytes 'P', 'K', 5, 6), which would let a
 *         reader take part of the comment for the end record.
 */
APRESULT aparchive_writer_set_comment(aparchive_writer writer, const void *comment, uint16_t comment_size,
                                      APARCHIVE_ERROR *out_error);

/**
 * @brief Drops an entry already in the archive from the central directory
 * the writer will write. Its bytes stay where they are, unreferenced; the
 * name may be used again. Replacing an entry is dropping it and adding the
 * new one.
 *
 * @param[in]  writer    The writer.
 * @param[in]  name      The entry's name, matched byte for byte.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_NOT_FOUND if no entry has @p name; or
 *         APRESULT_INVALID_ARGUMENT for a NULL argument, or a writer
 *         finished, unusable or with a streamed entry open.
 */
APRESULT aparchive_writer_remove_entry(aparchive_writer writer, const char *name, APARCHIVE_ERROR *out_error);

/**
 * @brief Bytes written to the sink so far - where the next entry's local
 * header will start.
 *
 * @param[in] writer The writer.
 *
 * @return The offset, or 0 if @p writer is NULL.
 */
uint64_t aparchive_writer_get_offset(aparchive_writer writer);

/**
 * @brief Writes the central directory and end records (ZIP64 ones only when
 * the entry count or offsets need them), with the comment
 * aparchive_writer_set_comment() set, or none.
 *
 * Nothing can be added afterwards.
 *
 * @param[in]  writer    The writer.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK, or a code from aparchive.h's "Results":
 *         APRESULT_INVALID_ARGUMENT if @p writer is NULL, already finished,
 *         unusable after an earlier failure, or with a streamed entry open.
 */
APRESULT aparchive_writer_finish(aparchive_writer writer, APARCHIVE_ERROR *out_error);

/**
 * @brief Destroys a writer.
 *
 * A sink failure, or running out of memory, while an entry or the central
 * directory was being written leaves the writer unusable - what follows
 * would sit after a partial record - and every later call fails with
 * APRESULT_INVALID_ARGUMENT. An entry refused for its arguments writes
 * nothing and leaves the writer usable. Without a finish, what was written
 * isn't a valid archive.
 *
 * @param[in] writer The writer. NULL is accepted and does nothing.
 */
void aparchive_writer_destroy(aparchive_writer writer);

/* ---- Reading --------------------------------------------------------- */

/**
 * @brief What a strict reader refuses (APARCHIVE_READER_DESC's `strict`), as
 * bits: what ZIP allows and some callers' own formats don't. Each is
 * APRESULT_APARCHIVE_MALFORMED on open.
 */
typedef enum APARCHIVE_STRICT APRI_ENUM_BASE {
	/** Nothing beyond the checks every reader makes. */
	APARCHIVE_STRICT_NONE = 0,
	/** An archive comment. */
	APARCHIVE_STRICT_NO_ARCHIVE_COMMENT = 1u << 0,
	/** An entry comment. */
	APARCHIVE_STRICT_NO_ENTRY_COMMENTS = 1u << 1,
	/** A directory entry (a name ending in '/'). */
	APARCHIVE_STRICT_NO_DIRECTORY_ENTRIES = 1u << 2,
	/** Bytes no entry or record accounts for: before the first entry,
	 * between entries, between the last entry and the central directory,
	 * or between the directory and the end records. Opening then reads
	 * every entry's local header (and data descriptor), once, to know where
	 * it ends; a data descriptor must agree with the central directory. */
	APARCHIVE_STRICT_NO_GAPS = 1u << 3,
} APARCHIVE_STRICT;

/**
 * @brief Configuration descriptor for a reader.
 *
 * The limits are checked on open against what the central directory
 * declares - so an archive claiming more fails before the caller allocates
 * anything. A declared size is exact: reading an entry fails unless it
 * decompresses to precisely that many bytes. Each limit fails with
 * APRESULT_LIMIT; UINT32_MAX / UINT64_MAX for none.
 */
typedef struct APARCHIVE_READER_DESC {
	/** This struct's type, APRI_STRUCT_TYPE_APARCHIVE_READER_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** Where the archive is read from. Its `read` is never NULL. */
	APARCHIVE_SOURCE source;
	/** The methods the reader can decode. Never NULL. Must outlive the
	 * reader. */
	aparchive_codecs codecs;
	/** The most entries the directory may declare. */
	uint32_t max_entry_count;
	/** The largest the central directory may be, in bytes. */
	uint64_t max_central_directory_size;
	/** The largest uncompressed size any one entry may declare. */
	uint64_t max_entry_uncompressed_size;
	/** The largest uncompressed size every entry together may declare. */
	uint64_t max_total_uncompressed_size;
	/** The largest an entry's uncompressed size over its compressed size,
	 * rounded up, may be (a zip bomb compresses far beyond what real data
	 * does; DEFLATE peaks near 1032:1). An empty entry never exceeds it. */
	uint32_t max_compression_ratio;
	/** APARCHIVE_STRICT bits: what to refuse beyond the checks every reader
	 * makes. APARCHIVE_STRICT_NONE reads as before. */
	APARCHIVE_STRICT strict;
} APARCHIVE_READER_DESC;

/** @brief Opaque handle to an archive reader. */
typedef struct aparchive_reader_t *aparchive_reader;

/**
 * @brief Opens an archive: reads the end records and the central
 * directory - nothing else.
 *
 * Some checks are always made, because they keep any caller safe: an
 * archive with an unsafe name (aparchive_is_safe_name()), a duplicate name,
 * overlapping entries, or sizes and offsets that don't fit the file fails
 * with APRESULT_APARCHIVE_MALFORMED.
 *
 * An archive whose central directory isn't where the end record says, but
 * ends just before it (or before the ZIP64 end record), has bytes in front
 * of it that its offsets don't count - a self-extracting archive's
 * program, often. It is read with every offset moved by that much, and
 * every offset this reader reports is where the bytes are in the source.
 * aparchive_reader_get_offset_shift() says by how much.
 *
 * @param[in]  desc       Reader configuration. Only read during the call.
 * @param[out] out_reader The new reader; NULL on failure.
 * @param[out] out_error  Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_APARCHIVE_NOT_AN_ARCHIVE if there's no end
 *         record; APRESULT_APARCHIVE_MALFORMED as above, or for what
 *         `strict` refuses; APRESULT_LIMIT over a limit in @p desc; or
 *         another code from aparchive.h's "Results"
 *         (APRESULT_INVALID_ARGUMENT for a NULL argument, a NULL
 *         `source.read`, a NULL `codecs` or an unknown `strict` bit).
 *
 * @see aparchive_reader_destroy()
 */
APRESULT aparchive_reader_open(const APARCHIVE_READER_DESC *desc, aparchive_reader *out_reader,
                               APARCHIVE_ERROR *out_error);

/**
 * @brief Counts a reader's entries.
 *
 * Thread: may overlap the reader's other read calls (get_entry_count,
 * get_entry, find, get_data_offset, read_entry, get_comment,
 * get_offset_shift, open_entry), from any thread; not its destroy.
 *
 * @param[in] reader The reader.
 *
 * @return The number of entries in the central directory, or 0 if
 *         @p reader is NULL.
 */
uint32_t aparchive_reader_get_entry_count(aparchive_reader reader);

/**
 * @brief What the central directory says about an entry.
 *
 * `name`, `extra` and `comment` point into the reader and stay valid until
 * it's destroyed.
 */
typedef struct APARCHIVE_ENTRY_INFO {
	/** This struct's type, APRI_STRUCT_TYPE_APARCHIVE_ENTRY_INFO, and its chain (apricore.h, "Struct bases"). The caller sets it. */
	APRI_STRUCT_BASE_OUT base;
	/** NUL-terminated, as stored: UTF-8 if #flags bit 11, else CP437. */
	const char *name;
	/** The length of #name in bytes, its NUL not included. */
	uint16_t name_size;
	/** The ZIP method id the entry is stored with. */
	uint16_t method;
	/** General-purpose bit flags; bit 3: a data descriptor follows the data. */
	uint16_t flags;
	/** MS-DOS date, as stored. */
	uint16_t dos_date;
	/** MS-DOS time, as stored. */
	uint16_t dos_time;
	/** The CRC-32 of the uncompressed data, as declared. */
	uint32_t crc32;
	/** The size of the data as stored, in bytes. */
	uint64_t compressed_size;
	/** The size of the data uncompressed, in bytes. */
	uint64_t uncompressed_size;
	/** Where the entry's local header starts in the source (the archive's
	 * own offset, moved by aparchive_reader_get_offset_shift()). */
	uint64_t local_header_offset;
	/** The central header's extra field. */
	const void *extra;
	/** The size of #extra in bytes. */
	uint16_t extra_size;
	/** The name ends in '/'. */
	bool is_directory;
	/** "Version made by", as stored: the host system in the upper byte (0
	 * MS-DOS, 3 Unix, 19 OS X...), the ZIP version in the lower. */
	uint16_t version_made_by;
	/** The internal file attributes, as stored (bit 0: text). */
	uint16_t internal_attributes;
	/** The external file attributes, as stored, in the form of the host in
	 * #version_made_by: for Unix, the mode in the upper 16 bits. */
	uint32_t external_attributes;
	/** The entry's comment, as stored (UTF-8 if #flags bit 11, else
	 * CP437); NULL when it has none. Not NUL-terminated. */
	const void *comment;
	/** The size of #comment in bytes. */
	uint16_t comment_size;
} APARCHIVE_ENTRY_INFO;

/**
 * @brief Reads an entry's central directory record.
 *
 * Fills @p out_info, whose base the caller sets, and leaves its base as it
 * was. Entries are in central directory order.
 *
 * Thread: may overlap the reader's other read calls (get_entry_count,
 * get_entry, find, get_data_offset, read_entry, get_comment,
 * get_offset_shift, open_entry), from any thread; not its destroy.
 *
 * @param[in]     reader   The reader.
 * @param[in]     index    The entry, from 0.
 * @param[in,out] out_info The entry's details.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT for a NULL argument; a
 *         struct-base code for a wrong `base`; or APRESULT_NOT_FOUND if
 *         @p index is past the last entry.
 */
APRESULT aparchive_reader_get_entry(aparchive_reader reader, uint32_t index, APARCHIVE_ENTRY_INFO *out_info);

/**
 * @brief Finds an entry by name.
 *
 * Thread: may overlap the reader's other read calls (get_entry_count,
 * get_entry, find, get_data_offset, read_entry, get_comment,
 * get_offset_shift, open_entry), from any thread; not its destroy.
 *
 * @param[in]  reader    The reader.
 * @param[in]  name      The name, NUL-terminated: matched exactly, byte for
 *                       byte - no case folding or Unicode normalization.
 * @param[out] out_index The entry's index.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT for a NULL argument; or
 *         APRESULT_NOT_FOUND if there's no entry named @p name.
 */
APRESULT aparchive_reader_find(aparchive_reader reader, const char *name, uint32_t *out_index);

/**
 * @brief Where an entry's data starts in the archive: past its local
 * header, which this reads and checks against the central directory.
 *
 * The entry's bytes as stored - uncompressed for STORED, compressed
 * otherwise - are [offset, offset + compressed_size) of the source: for the
 * caller to map or range-read, or to decompress itself (on the GPU, say).
 * Nothing read this way is checked against the CRC-32.
 *
 * Thread: may overlap the reader's other read calls (get_entry_count,
 * get_entry, find, get_data_offset, read_entry, get_comment,
 * get_offset_shift, open_entry), from any thread; not its destroy.
 *
 * @param[in]  reader     The reader.
 * @param[in]  index      The entry, from 0.
 * @param[out] out_offset The offset of the entry's data in the source.
 * @param[out] out_error  Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_APARCHIVE_MALFORMED if the local header and
 *         the central directory disagree; APRESULT_NOT_FOUND if @p index is
 *         past the last entry; or another code from aparchive.h's
 *         "Results".
 */
APRESULT aparchive_reader_get_data_offset(aparchive_reader reader, uint32_t index, uint64_t *out_offset,
                                          APARCHIVE_ERROR *out_error);

/**
 * @brief Reads an entry whole, decompressing it, and checks its CRC-32.
 *
 * Thread: may overlap the reader's other read calls (get_entry_count,
 * get_entry, find, get_data_offset, read_entry, get_comment,
 * get_offset_shift, open_entry), from any thread; not its destroy.
 *
 * @param[in]  reader      The reader.
 * @param[in]  index       The entry, from 0.
 * @param[out] buffer      Receives the uncompressed bytes. May be NULL
 *                         when @p buffer_size is 0.
 * @param[in]  buffer_size The entry's `uncompressed_size` exactly, so
 *                         nothing can decompress past what the central
 *                         directory declared.
 * @param[out] out_error   Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_APARCHIVE_UNKNOWN_METHOD if its method isn't
 *         in the codec table; APRESULT_NOT_FOUND if @p index is past the
 *         last entry; APRESULT_INVALID_ARGUMENT if @p buffer_size isn't the
 *         entry's size; APRESULT_CORRUPT_DATA if it fails its CRC-32 or
 *         decompresses to another size; or another code from aparchive.h's
 *         "Results".
 */
APRESULT aparchive_reader_read_entry(aparchive_reader reader, uint32_t index, void *buffer, uint64_t buffer_size,
                                     APARCHIVE_ERROR *out_error);

/**
 * @brief The archive's comment, as stored (the end record's).
 *
 * Thread: may overlap the reader's other read calls, from any thread; not
 * its destroy.
 *
 * @param[in]  reader      The reader.
 * @param[out] out_comment The comment's bytes, valid until the reader is
 *                         destroyed; NULL when there is none. Not
 *                         NUL-terminated.
 * @param[out] out_size    The size of the comment in bytes.
 *
 * @return APRESULT_OK, or APRESULT_INVALID_ARGUMENT for a NULL argument.
 */
APRESULT aparchive_reader_get_comment(aparchive_reader reader, const void **out_comment, uint16_t *out_size);

/**
 * @brief How many bytes sit in front of the archive that its own offsets
 * don't count (see aparchive_reader_open()).
 *
 * Thread: may overlap the reader's other read calls, from any thread; not
 * its destroy.
 *
 * @param[in] reader The reader.
 *
 * @return The shift: 0 for an archive whose offsets are right, and if
 *         @p reader is NULL.
 */
uint64_t aparchive_reader_get_offset_shift(aparchive_reader reader);

/**
 * @brief Opaque handle to one entry being read in pieces. Used by one
 * caller at a time; any number may be open on one reader, from any
 * threads.
 */
typedef struct aparchive_entry_reader_t *aparchive_entry_reader;

/**
 * @brief Starts reading an entry in pieces, for an entry bigger than the
 * caller wants to hold.
 *
 * Its local header is read and checked now, as
 * aparchive_reader_get_data_offset() does.
 *
 * Thread: may overlap the reader's other read calls, from any thread; not
 * its destroy. Every entry reader is destroyed before the reader.
 *
 * @param[in]  reader           The reader.
 * @param[in]  index            The entry, from 0.
 * @param[out] out_entry_reader The new entry reader; NULL on failure.
 * @param[out] out_error        Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_APARCHIVE_UNKNOWN_METHOD if its method
 *         isn't in the codec table; APRESULT_UNSUPPORTED if its codec has
 *         no stream callbacks; APRESULT_NOT_FOUND if @p index is past the
 *         last entry; or another code from
 *         aparchive_reader_get_data_offset().
 *
 * @see aparchive_entry_reader_destroy()
 */
APRESULT aparchive_reader_open_entry(aparchive_reader reader, uint32_t index, aparchive_entry_reader *out_entry_reader,
                                     APARCHIVE_ERROR *out_error);

/**
 * @brief Reads the next piece of an entry, uncompressed.
 *
 * Fills @p buffer unless the entry ends first: fewer bytes than
 * @p capacity means the end, and a read at the end gives 0 bytes. The
 * whole entry's CRC-32 and size are checked by the call that hands out its
 * last byte (or, for an empty entry, the first call): if they fail, that
 * call fails, and what it put in @p buffer is not the entry's. Bytes handed
 * out before are unchecked until then.
 *
 * DEFLATE in pieces is decoded by zlib-ng, and read whole by libdeflate.
 * They agree on every valid stream, and on all but one kind of invalid
 * one: data using a code its own Huffman table left unassigned, which
 * libdeflate decodes (to bytes the CRC-32 then judges) and zlib-ng refuses.
 * Such an entry may read whole and fail here with APRESULT_CORRUPT_DATA.
 *
 * @param[in]  entry_reader The entry reader.
 * @param[out] buffer       Receives the bytes. May be NULL when
 *                          @p capacity is 0.
 * @param[in]  capacity     The size of @p buffer in bytes.
 * @param[out] out_size     How many bytes were read.
 * @param[out] out_error    Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_CORRUPT_DATA if the entry fails its CRC-32
 *         or doesn't decompress to its declared size; or another code from
 *         aparchive.h's "Results". After a failure every later call fails.
 */
APRESULT aparchive_entry_reader_read(aparchive_entry_reader entry_reader, void *buffer, uint64_t capacity,
                                     uint64_t *out_size, APARCHIVE_ERROR *out_error);

/**
 * @brief Destroys an entry reader, read to its end or not.
 *
 * @param[in] entry_reader The entry reader. NULL is accepted and does
 *                         nothing.
 */
void aparchive_entry_reader_destroy(aparchive_entry_reader entry_reader);

/**
 * @brief Destroys a reader. Every name and extra field it handed out goes
 * with it.
 *
 * @param[in] reader The reader. NULL is accepted and does nothing.
 */
void aparchive_reader_destroy(aparchive_reader reader);

/* ---- Updating -------------------------------------------------------- */

/**
 * @brief Configuration descriptor for a writer that updates an archive.
 *
 * Updating keeps every byte of the archive before its central directory,
 * appends what is added after them, and writes a new central directory and
 * end records: the entries kept are neither read nor moved, and an entry
 * dropped (aparchive_writer_remove_entry()) leaves its bytes behind,
 * unreferenced, until the caller writes a fresh archive.
 */
typedef struct APARCHIVE_UPDATE_DESC {
	/** This struct's type, APRI_STRUCT_TYPE_APARCHIVE_UPDATE_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** The archive as it is now, opened. Only read during the call: what
	 * the writer needs of it is copied. */
	aparchive_reader reader;
	/** Cuts the archive to `size` bytes, its central directory's start, so
	 * that #sink appends from there; called with the sink's `user`, once,
	 * just before the writer's first byte is written, so a writer destroyed
	 * having written nothing leaves the archive as it was. From then until
	 * aparchive_writer_finish() the archive is not valid. False on failure,
	 * which ends the writer as a failed write does. Never NULL. */
	bool (*truncate)(void *user, uint64_t size);
	/** Appends to the archive, after the cut. Its `write` is never NULL. */
	APARCHIVE_SINK sink;
	/** The methods the writer can encode. Never NULL. Must outlive the
	 * writer. */
	aparchive_codecs codecs;
	/** As APARCHIVE_WRITER_DESC's: lets new entries be directories. The
	 * archive's own directory entries are kept either way. */
	bool allow_directory_entries;
} APARCHIVE_UPDATE_DESC;

/**
 * @brief Creates a writer over an existing archive: its entries already in
 * place, its comment set, and every call of a writer available.
 *
 * aparchive_writer_get_offset() starts at the archive's central directory.
 * Each kept entry's central record is written again as the reader read it -
 * its attributes, comment and extra field (less any ZIP64 record, which the
 * writer writes itself where needed) - with the offset of where it sits in
 * the source.
 *
 * @param[in]  desc       Update configuration. Only read during the call.
 * @param[out] out_writer The new writer; NULL on failure.
 * @param[out] out_error  Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT for a NULL argument, a
 *         NULL `truncate`, `sink.write` or `codecs`; a struct-base code
 *         for a wrong `base`; or APRESULT_OUT_OF_MEMORY.
 *
 * @see aparchive_writer_destroy()
 */
APRESULT aparchive_writer_create_update(const APARCHIVE_UPDATE_DESC *desc, aparchive_writer *out_writer,
                                        APARCHIVE_ERROR *out_error);

/* ---- Extra fields ---------------------------------------------------- */

/*
 * Help with the standard extra field records other tools write and read.
 * They work on bytes only - an entry's `extra`, or a buffer the caller
 * builds its own from - and the writer writes none unless the caller puts
 * it in an entry's extra.
 *
 * Thread: each may be called from any thread at any time; they hold no
 * state.
 */

/** @brief Extra field id 0x5455: the extended timestamp (Info-ZIP), Unix seconds. */
#define APARCHIVE_EXTRA_UNIX_TIMES 0x5455
/** @brief Extra field id 0x000A: NTFS times, in 100-nanosecond ticks since 1601. */
#define APARCHIVE_EXTRA_NTFS_TIMES 0x000A
/** @brief Extra field id 0x7075: the Info-ZIP Unicode path, a UTF-8 name. */
#define APARCHIVE_EXTRA_UNICODE_PATH 0x7075

/**
 * @brief A file's times, in 100-nanosecond ticks since 1970-01-01
 * 00:00:00 UTC (negative before), each with whether it is known.
 */
/* Frozen: a value, read and written whole. It never changes; a new need gets a new type. */
typedef struct APARCHIVE_FILE_TIMES {
	int64_t modified;  /**< Last modified. */
	int64_t accessed;  /**< Last accessed. */
	int64_t created;   /**< Created. */
	bool has_modified; /**< #modified is known. */
	bool has_accessed; /**< #accessed is known. */
	bool has_created;  /**< #created is known. */
} APARCHIVE_FILE_TIMES;

/**
 * @brief Finds the first record with an id in an extra field.
 *
 * @param[in]  extra      The extra field: whole records end to end. May be
 *                        NULL when @p extra_size is 0.
 * @param[in]  extra_size The size of @p extra in bytes.
 * @param[in]  id         The record's id.
 * @param[out] out_data   The record's data, past its id and size, inside
 *                        @p extra.
 * @param[out] out_size   The size of its data in bytes.
 *
 * @return APRESULT_OK; APRESULT_NOT_FOUND if no record has @p id;
 *         APRESULT_APARCHIVE_MALFORMED if a record before it runs past the
 *         field's end; or APRESULT_INVALID_ARGUMENT for a NULL argument.
 */
APRESULT aparchive_extra_find(const void *extra, uint16_t extra_size, uint16_t id, const void **out_data,
                              uint16_t *out_size);

/**
 * @brief Reads an extended timestamp record (0x5455). A central header's
 * holds the modified time only, whatever its flags say.
 *
 * @param[in]  extra      The extra field, as aparchive_extra_find() takes it.
 * @param[in]  extra_size The size of @p extra in bytes.
 * @param[out] out_times  The times it holds, the others not known.
 *
 * @return APRESULT_OK; APRESULT_NOT_FOUND if there's no such record;
 *         APRESULT_APARCHIVE_MALFORMED if it is empty, or as
 *         aparchive_extra_find(); or APRESULT_INVALID_ARGUMENT for a NULL
 *         argument.
 */
APRESULT aparchive_extra_get_unix_times(const void *extra, uint16_t extra_size, APARCHIVE_FILE_TIMES *out_times);

/**
 * @brief Writes an extended timestamp record (0x5455): its id, size and
 * data, the known times in whole seconds (rounded down).
 *
 * @param[in]  times        The times; at least one known.
 * @param[in]  central      True for a central header's form, the modified
 *                          time only (the flags still say which the local
 *                          one holds).
 * @param[out] out          Receives the record.
 * @param[in]  out_capacity The size of @p out in bytes.
 * @param[out] out_size     The record's size; set even when @p out is too
 *                          small.
 *
 * @return APRESULT_OK; or APRESULT_INVALID_ARGUMENT for a NULL argument, no
 *         time known, a time outside a signed 32-bit count of seconds
 *         (1901 to 2038), or an @p out_capacity under the record's size.
 */
APRESULT aparchive_extra_put_unix_times(const APARCHIVE_FILE_TIMES *times, bool central, void *out,
                                        uint16_t out_capacity, uint16_t *out_size);

/**
 * @brief Reads an NTFS times record (0x000A): its attribute 1, which holds
 * all three times.
 *
 * @param[in]  extra      The extra field, as aparchive_extra_find() takes it.
 * @param[in]  extra_size The size of @p extra in bytes.
 * @param[out] out_times  The three times.
 *
 * @return APRESULT_OK; APRESULT_NOT_FOUND if there's no such record, or it
 *         has no attribute 1; APRESULT_APARCHIVE_MALFORMED if it's cut
 *         short, a time is past what 64-bit ticks since 1970 hold, or as
 *         aparchive_extra_find(); or APRESULT_INVALID_ARGUMENT for a NULL
 *         argument.
 */
APRESULT aparchive_extra_get_ntfs_times(const void *extra, uint16_t extra_size, APARCHIVE_FILE_TIMES *out_times);

/**
 * @brief Writes an NTFS times record (0x000A), 36 bytes: its id, size and
 * data.
 *
 * @param[in]  times        The times: all three known, none before 1601.
 * @param[out] out          Receives the record.
 * @param[in]  out_capacity The size of @p out in bytes.
 * @param[out] out_size     The record's size, 36.
 *
 * @return APRESULT_OK; or APRESULT_INVALID_ARGUMENT for a NULL argument, a
 *         time not known or before 1601, or an @p out_capacity under 36.
 */
APRESULT aparchive_extra_put_ntfs_times(const APARCHIVE_FILE_TIMES *times, void *out, uint16_t out_capacity,
                                        uint16_t *out_size);

/**
 * @brief Reads a Unicode path record (0x7075): the UTF-8 name a tool wrote
 * beside a CP437 one. A record whose CRC-32 doesn't match @p name was
 * written for another name (the entry renamed by a tool that didn't know
 * it) and is ignored, as the format says.
 *
 * @param[in]  extra      The extra field, as aparchive_extra_find() takes it.
 * @param[in]  extra_size The size of @p extra in bytes.
 * @param[in]  name       The entry's name as stored.
 * @param[in]  name_size  The size of @p name in bytes.
 * @param[out] out_path   The UTF-8 name, inside @p extra; not
 *                        NUL-terminated, not checked to be UTF-8 or safe.
 * @param[out] out_size   The size of the UTF-8 name in bytes.
 *
 * @return APRESULT_OK; APRESULT_NOT_FOUND if there's no such record, or
 *         its CRC-32 is another name's; APRESULT_APARCHIVE_MALFORMED if
 *         it's cut short, its version isn't 1, or as
 *         aparchive_extra_find(); or APRESULT_INVALID_ARGUMENT for a NULL
 *         argument.
 */
APRESULT aparchive_extra_get_unicode_path(const void *extra, uint16_t extra_size, const char *name,
                                          uint16_t name_size, const char **out_path, uint16_t *out_size);

/**
 * @brief Writes a Unicode path record (0x7075): its id, size and data.
 *
 * @param[in]  name         The entry's name as it will be stored, whose
 *                          CRC-32 the record holds.
 * @param[in]  name_size    The size of @p name in bytes.
 * @param[in]  path         The UTF-8 name.
 * @param[in]  path_size    The size of @p path in bytes.
 * @param[out] out          Receives the record.
 * @param[in]  out_capacity The size of @p out in bytes.
 * @param[out] out_size     The record's size, 9 + @p path_size; set even
 *                          when @p out is too small.
 *
 * @return APRESULT_OK; or APRESULT_INVALID_ARGUMENT for a NULL argument, a
 *         @p path that isn't UTF-8 or makes a record past 65,535 bytes, or
 *         an @p out_capacity under the record's size.
 */
APRESULT aparchive_extra_put_unicode_path(const char *name, uint16_t name_size, const char *path, uint16_t path_size,
                                          void *out, uint16_t out_capacity, uint16_t *out_size);

/* ---- Utilities ------------------------------------------------------- */

/**
 * @brief Whether a name is a safe entry name.
 *
 * Safe is: not empty, no NUL, no leading '/', no '\\', no drive letter, and
 * no empty, "." or ".." segments - except that it may end in one '/', which
 * makes it a directory entry's name. Encoding-blind, so it judges names
 * from any archive, UTF-8 or CP437; the writer also requires UTF-8, and
 * names that are unique and not a directory's.
 *
 * @param[in] name      The name. Need not be NUL-terminated.
 * @param[in] name_size The length of @p name in bytes.
 *
 * @return True if @p name is safe; false if not, if @p name is NULL, or if
 *         @p name_size is over 65535 (a ZIP name's limit).
 */
bool aparchive_is_safe_name(const char *name, uint64_t name_size);

/**
 * @brief Updates a running CRC-32 (ZIP's polynomial).
 *
 * @param[in] crc  The CRC so far; start from 0.
 * @param[in] data The bytes to add.
 * @param[in] size The size of @p data in bytes.
 *
 * @return The updated CRC-32.
 */
uint32_t aparchive_crc32(uint32_t crc, const void *data, uint64_t size);

#if __cplusplus
}
#endif

#endif // APARCHIVE_H
