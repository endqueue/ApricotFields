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
 * Entries are written and read whole: one buffer in, one buffer out.
 *
 * Opening reads only the end of the archive and its central directory; each
 * entry is read only when asked for.
 *
 * What it writes is plain ZIP: any ZIP tool reads it. What it reads is
 * plain ZIP from any tool, including what its own writer never produces -
 * an archive comment, directory entries, entries written with a data
 * descriptor, extra field records it doesn't know.
 *
 * Not built yet (APARCHIVE_TODO.md): entries streamed in pieces, entries
 * added already compressed, writing directory entries, updating an archive
 * in place. Each is a new call or a new struct chained onto one. None
 * changes the bytes today's calls write, or which archives today's reader
 * opens.
 *
 * Out of scope: encryption (ZipCrypto, WinZip AES) and split or multi-disk
 * archives - reading one fails with APRESULT_UNSUPPORTED. Archives with
 * data before the first entry whose offsets don't account for it (some
 * self-extracting archives) aren't read.
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
 *     wrong state (a writer finished or failed).
 *   - APRESULT_IO: the source or sink callback returned false.
 *   - APRESULT_LIMIT: over an APARCHIVE_READER_DESC limit.
 *   - APRESULT_UNSUPPORTED: encryption, multiple disks, patch data, or
 *     compressing with a decode-only codec.
 *   - APRESULT_CORRUPT_DATA: an entry that fails its CRC-32 or doesn't
 *     decompress to its declared size.
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
 * APARCHIVE_SOURCE, APARCHIVE_SINK and APARCHIVE_ERROR are frozen and have
 * none.
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
 * A decode-only codec leaves `compress` and `compress_bound` NULL.
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
 *         STORED, a method already in the table, no `decompress`, or only
 *         one of `compress` and `compress_bound`; a struct-base code for a
 *         wrong `base`; or APRESULT_OUT_OF_MEMORY.
 */
APRESULT aparchive_codecs_add(aparchive_codecs codecs, const APARCHIVE_CODEC *codec);
/**
 * @brief Adds DEFLATE, built on libdeflate.
 *
 * Levels 0-12: 0 stores without compressing (still as DEFLATE), 1 is
 * fastest, 12 smallest. Its output for a given input and level is fixed by
 * the libdeflate version ApricotFields builds with - pinned, so archives
 * stay byte-identical.
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
	 * ending in '/' would be a directory entry, which the writer doesn't
	 * write yet: directories exist only as name prefixes. */
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
	/** Sets general-purpose flag bit 11: the name is UTF-8. */
	bool utf8_name_flag;
	/** Caller extra field records (id, size, data - already encoded),
	 * written into both the local and the central header. NULL/0 for
	 * none. Must be whole records, none with id 0x0001 (ZIP64, which
	 * ApArchive writes itself). With no extra, no alignment padding and
	 * sizes under 4 GiB, a local header has no extra field at all. */
	const void *extra;
	/** The size of #extra in bytes; 0 for none. */
	uint16_t extra_size;
	/** Write this entry's sizes in ZIP64 fields even though they'd fit
	 * without. Off, an entry gets ZIP64 fields only when its sizes or
	 * offset need them. */
	bool zip64;
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
 * the entry count or offsets need them), with no archive comment.
 *
 * Nothing can be added afterwards.
 *
 * @param[in]  writer    The writer.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK, or a code from aparchive.h's "Results":
 *         APRESULT_INVALID_ARGUMENT if @p writer is NULL, already finished
 *         or unusable after an earlier failure.
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
 * @param[in]  desc       Reader configuration. Only read during the call.
 * @param[out] out_reader The new reader; NULL on failure.
 * @param[out] out_error  Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_APARCHIVE_NOT_AN_ARCHIVE if there's no end
 *         record; APRESULT_APARCHIVE_MALFORMED as above; APRESULT_LIMIT
 *         over a limit in @p desc; or another code from aparchive.h's
 *         "Results" (APRESULT_INVALID_ARGUMENT for a NULL argument, a NULL
 *         `source.read` or a NULL `codecs`).
 *
 * @see aparchive_reader_destroy()
 */
APRESULT aparchive_reader_open(const APARCHIVE_READER_DESC *desc, aparchive_reader *out_reader,
                               APARCHIVE_ERROR *out_error);

/**
 * @brief Counts a reader's entries.
 *
 * Thread: may overlap the reader's other read calls (get_entry_count,
 * get_entry, find, get_data_offset, read_entry), from any thread; not its
 * destroy.
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
 * `name` and `extra` point into the reader and stay valid until it's
 * destroyed.
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
	/** Where the entry's local header starts in the archive. */
	uint64_t local_header_offset;
	/** The central header's extra field. */
	const void *extra;
	/** The size of #extra in bytes. */
	uint16_t extra_size;
	/** The name ends in '/'. */
	bool is_directory;
} APARCHIVE_ENTRY_INFO;

/**
 * @brief Reads an entry's central directory record.
 *
 * Fills @p out_info, whose base the caller sets, and leaves its base as it
 * was. Entries are in central directory order.
 *
 * Thread: may overlap the reader's other read calls (get_entry_count,
 * get_entry, find, get_data_offset, read_entry), from any thread; not its
 * destroy.
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
 * get_entry, find, get_data_offset, read_entry), from any thread; not its
 * destroy.
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
 * get_entry, find, get_data_offset, read_entry), from any thread; not its
 * destroy.
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
 * get_entry, find, get_data_offset, read_entry), from any thread; not its
 * destroy.
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
 * @brief Destroys a reader. Every name and extra field it handed out goes
 * with it.
 *
 * @param[in] reader The reader. NULL is accepted and does nothing.
 */
void aparchive_reader_destroy(aparchive_reader reader);

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
