#include "aparchive_internal.hpp"
#include "../apristructbase.hpp"
#include <algorithm>
#include <libdeflate.h>
#include <new>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib-ng.h>
#include "../aprioverlap.hpp"

// ZIP writing and reading, entries whole or in pieces, with ZIP64 where
// sizes, offsets or the entry count need it. What isn't here yet is listed
// in APARCHIVE_TODO.md.

// ---- Errors -------------------------------------------------------------------

static void clear_error(APARCHIVE_ERROR *error)
{
    if (error)
        error->message[0] = '\0';
}

// Records [format] in [error] (if any) and returns [result].
static APRESULT fail(APARCHIVE_ERROR *error, APRESULT result, const char *format, ...)
{
    if (error)
    {
        va_list args;
        va_start(args, format);
        vsnprintf(error->message, sizeof(error->message), format, args);
        va_end(args);
    }
    return result;
}

// ---- Names --------------------------------------------------------------------

static bool is_valid_utf8(const char *text, size_t size)
{
    const uint8_t *s = (const uint8_t *)text;
    size_t i = 0;
    while (i < size)
    {
        uint8_t c = s[i];
        size_t length;
        uint32_t min;
        if (c < 0x80)
        {
            i++;
            continue;
        }
        else if ((c & 0xE0) == 0xC0)
            length = 2, min = 0x80;
        else if ((c & 0xF0) == 0xE0)
            length = 3, min = 0x800;
        else if ((c & 0xF8) == 0xF0)
            length = 4, min = 0x10000;
        else
            return false;
        if (i + length > size)
            return false;
        uint32_t code = c & (0x7F >> length);
        for (size_t k = 1; k < length; k++)
        {
            if ((s[i + k] & 0xC0) != 0x80)
                return false;
            code = (code << 6) | (s[i + k] & 0x3F);
        }
        // Overlong encodings, surrogates, and past U+10FFFF.
        if (code < min || (code >= 0xD800 && code <= 0xDFFF) || code > 0x10FFFF)
            return false;
        i += length;
    }
    return true;
}

static bool is_ascii(const char *text, size_t size)
{
    for (size_t i = 0; i < size; i++)
        if ((uint8_t)text[i] >= 0x80)
            return false;
    return true;
}

// ---- ZIP records --------------------------------------------------------------
//
// Everything is little-endian. Offsets in the comments below are from the
// start of each record.

static const uint32_t SIG_LOCAL = 0x04034b50;
static const uint32_t SIG_CENTRAL = 0x02014b50;
static const uint32_t SIG_END = 0x06054b50;
static const uint32_t SIG_END64 = 0x06064b50;
static const uint32_t SIG_LOCATOR64 = 0x07064b50;
static const uint32_t SIG_DESCRIPTOR = 0x08074b50;

static const size_t LOCAL_HEADER_SIZE = 30;
static const size_t CENTRAL_HEADER_SIZE = 46;
static const size_t END_SIZE = 22;
static const size_t END64_SIZE = 56;
static const size_t LOCATOR64_SIZE = 20;

static const uint16_t EXTRA_ZIP64 = 0x0001;
static const uint16_t EXTRA_PADDING = 0xD935;
// The most a central header's ZIP64 record takes: its 4-byte header and
// three 8-byte fields.
static const size_t ZIP64_EXTRA_MAX_SIZE = 28;

static const uint16_t FLAG_ENCRYPTED = 0x0001;
static const uint16_t FLAG_DATA_DESCRIPTOR = 0x0008;
static const uint16_t FLAG_PATCH_DATA = 0x0020;
static const uint16_t FLAG_STRONG_ENCRYPTION = 0x0040;
static const uint16_t FLAG_UTF8 = 0x0800;
static const uint16_t FLAG_MASKED_HEADERS = 0x2000;
static const uint16_t FLAGS_ENCRYPTION = FLAG_ENCRYPTED | FLAG_STRONG_ENCRYPTION | FLAG_MASKED_HEADERS;

// "Version made by": ZIP specification 4.5 (the one with ZIP64), host
// system 0 (MS-DOS). Host 0 because no file attributes are written: with a
// Unix host, extracting tools would take the zero attributes as "no
// permissions".
static const uint16_t VERSION_MADE_BY = 45;

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get64(const uint8_t *p)
{
    return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
}

static void put16(std::vector<uint8_t> &out, uint16_t value)
{
    out.push_back((uint8_t)value);
    out.push_back((uint8_t)(value >> 8));
}

static void put32(std::vector<uint8_t> &out, uint32_t value)
{
    put16(out, (uint16_t)value);
    put16(out, (uint16_t)(value >> 16));
}

static void put64(std::vector<uint8_t> &out, uint64_t value)
{
    put32(out, (uint32_t)value);
    put32(out, (uint32_t)(value >> 32));
}

static void put_bytes(std::vector<uint8_t> &out, const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    out.insert(out.end(), bytes, bytes + size);
}

// The version a reader needs for an entry: 4.5 for ZIP64, else what its
// method needs.
static uint16_t version_needed(uint16_t method, bool zip64)
{
    uint16_t version;
    switch (method)
    {
    case APARCHIVE_METHOD_STORED:
        version = 10;
        break;
    case APARCHIVE_METHOD_DEFLATE:
        version = 20;
        break;
    case 12: // bzip2
        version = 46;
        break;
    default: // LZMA, zstd, xz and the rest of the later methods
        version = 63;
        break;
    }
    return zip64 && version < 45 ? 45 : version;
}

// True if [extra] is whole records (id, size, data) end to end.
static bool extra_is_whole_records(const uint8_t *extra, size_t size)
{
    size_t at = 0;
    while (at < size)
    {
        if (size - at < 4)
            return false;
        size_t record_size = get16(extra + at + 2);
        if (size - at - 4 < record_size)
            return false;
        at += 4 + record_size;
    }
    return true;
}

// The data of the first record with [id] in an extra field. Stops at the
// first thing that isn't a whole record: other tools' extra fields aren't
// always well formed, and one that isn't only matters if it hides a record
// that was needed.
static bool find_extra(const uint8_t *extra, size_t size, uint16_t id, const uint8_t **out_data, size_t *out_size)
{
    size_t at = 0;
    while (size - at >= 4)
    {
        size_t record_size = get16(extra + at + 2);
        if (size - at - 4 < record_size)
            return false;
        if (get16(extra + at) == id)
        {
            *out_data = extra + at + 4;
            *out_size = record_size;
            return true;
        }
        at += 4 + record_size;
    }
    return false;
}

const APARCHIVE_CODEC *aparchive_codecs_t::find(uint16_t method) const
{
    for (const APARCHIVE_CODEC &codec : codecs)
        if (codec.method == method)
            return &codec;
    return nullptr;
}

// ---- DEFLATE (libdeflate) --------------------------------------------------
//
// libdeflate's compressors and decompressors aren't thread-safe, and codec
// callbacks may run on several threads at once, so each call allocates its
// own. Both are small; a per-thread cache can come later if it shows up.

static uint64_t deflate_compress_bound(void *, uint64_t in_size)
{
    return libdeflate_deflate_compress_bound(nullptr, (size_t)in_size);
}

static bool deflate_compress(void *, int32_t level, const void *in, uint64_t in_size, void *out, uint64_t out_capacity,
                             uint64_t *out_size)
{
    if (level < 0 || level > 12)
        return false;
    libdeflate_compressor *compressor = libdeflate_alloc_compressor(level);
    if (!compressor)
        return false;
    size_t size = libdeflate_deflate_compress(compressor, in, (size_t)in_size, out, (size_t)out_capacity);
    libdeflate_free_compressor(compressor);
    if (size == 0)
        return false;
    *out_size = size;
    return true;
}

static bool deflate_decompress(void *, const void *in, uint64_t in_size, void *out, uint64_t out_size)
{
    libdeflate_decompressor *decompressor = libdeflate_alloc_decompressor();
    if (!decompressor)
        return false;
    // A NULL actual_out_nbytes makes libdeflate require exactly out_size
    // bytes - so the data can't decompress past what was declared.
    libdeflate_result result =
        libdeflate_deflate_decompress(decompressor, in, (size_t)in_size, out, (size_t)out_size, nullptr);
    libdeflate_free_decompressor(decompressor);
    return result == LIBDEFLATE_SUCCESS;
}

// ---- DEFLATE in pieces (zlib-ng) -------------------------------------------
//
// For streamed entries only (card A24): raw DEFLATE (no zlib header), its
// window, memory level and strategy fixed here, so a level gives the same
// bytes every time with zlib-ng's version pinned.

struct deflate_stream_t
{
    zng_stream z;
    bool compress;
};

static void *deflate_stream_begin(void *, bool compress, int32_t level)
{
    if (compress && (level < 0 || level > 9))
        return nullptr;
    deflate_stream_t *stream = (deflate_stream_t *)calloc(1, sizeof(deflate_stream_t));
    if (!stream)
        return nullptr;
    stream->compress = compress;
    int result = compress ? zng_deflateInit2(&stream->z, level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY)
                          : zng_inflateInit2(&stream->z, -15);
    if (result != Z_OK)
    {
        free(stream);
        return nullptr;
    }
    return stream;
}

static bool deflate_stream_process(void *, void *state, const void *in, uint64_t in_size, uint64_t *in_used,
                                   void *out, uint64_t out_capacity, uint64_t *out_size, bool finish, bool *done)
{
    deflate_stream_t *stream = (deflate_stream_t *)state;
    // zlib counts in 32 bits: a call takes what fits, and is called again.
    uint32_t in_count = in_size > UINT32_MAX ? UINT32_MAX : (uint32_t)in_size;
    uint32_t out_count = out_capacity > UINT32_MAX ? UINT32_MAX : (uint32_t)out_capacity;
    stream->z.next_in = (const uint8_t *)in;
    stream->z.avail_in = in_count;
    stream->z.next_out = (uint8_t *)out;
    stream->z.avail_out = out_count;
    int result;
    if (stream->compress)
        result = zng_deflate(&stream->z, finish && in_count == in_size ? Z_FINISH : Z_NO_FLUSH);
    else
        result = zng_inflate(&stream->z, Z_NO_FLUSH);
    *in_used = in_count - stream->z.avail_in;
    *out_size = out_count - stream->z.avail_out;
    *done = result == Z_STREAM_END;
    return result == Z_OK || result == Z_STREAM_END || result == Z_BUF_ERROR;
}

static void deflate_stream_end(void *, void *state)
{
    deflate_stream_t *stream = (deflate_stream_t *)state;
    if (stream->compress)
        zng_deflateEnd(&stream->z);
    else
        zng_inflateEnd(&stream->z);
    free(stream);
}

// ---- Standard extra fields --------------------------------------------------

static const int64_t TICKS_PER_SECOND = 10000000;
// 1601-01-01 to 1970-01-01, in 100-nanosecond ticks.
static const uint64_t NTFS_TO_UNIX_TICKS = 116444736000000000ull;

// aparchive_extra_find(), on bytes: whether a record before [id]'s isn't
// whole tells MALFORMED from NOT_FOUND.
static APRESULT extra_find(const uint8_t *extra, size_t size, uint16_t id, const uint8_t **out_data,
                           size_t *out_size)
{
    size_t at = 0;
    while (at < size)
    {
        if (size - at < 4)
            return APRESULT_APARCHIVE_MALFORMED;
        size_t record_size = get16(extra + at + 2);
        if (size - at - 4 < record_size)
            return APRESULT_APARCHIVE_MALFORMED;
        if (get16(extra + at) == id)
        {
            *out_data = extra + at + 4;
            *out_size = record_size;
            return APRESULT_OK;
        }
        at += 4 + record_size;
    }
    return APRESULT_NOT_FOUND;
}

static int64_t floor_seconds(int64_t ticks)
{
    int64_t seconds = ticks / TICKS_PER_SECOND;
    return ticks % TICKS_PER_SECOND < 0 ? seconds - 1 : seconds;
}

static void put_u16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *out, uint32_t value)
{
    put_u16(out, (uint16_t)value);
    put_u16(out + 2, (uint16_t)(value >> 16));
}

static void put_u64(uint8_t *out, uint64_t value)
{
    put_u32(out, (uint32_t)value);
    put_u32(out + 4, (uint32_t)(value >> 32));
}

// ---- Writing ------------------------------------------------------------------

static bool is_directory_name(const char *name, size_t name_size)
{
    return name_size != 0 && name[name_size - 1] == '/';
}

// The checks for an extra field the caller hands over, local or central.
static APRESULT check_extra(const APARCHIVE_ENTRY_DESC &desc, const void *data, uint16_t size, const char *which,
                            APARCHIVE_ERROR *error)
{
    if (!data && size != 0)
        return fail(error, APRESULT_INVALID_ARGUMENT, "%s_size is %u but %s is null", which, size, which);
    if (size == 0)
        return APRESULT_OK;
    const uint8_t *extra = (const uint8_t *)data;
    const uint8_t *zip64;
    size_t zip64_size;
    if (!extra_is_whole_records(extra, size))
        return fail(error, APRESULT_INVALID_ARGUMENT, "'%s': %s isn't whole extra field records", desc.name, which);
    if (find_extra(extra, size, EXTRA_ZIP64, &zip64, &zip64_size))
        return fail(error, APRESULT_INVALID_ARGUMENT, "'%s': %s has a ZIP64 record, which the writer writes itself",
                    desc.name, which);
    // Room for the ZIP64 record a header may need.
    if (size > UINT16_MAX - ZIP64_EXTRA_MAX_SIZE)
        return fail(error, APRESULT_INVALID_ARGUMENT, "'%s': %s is %u bytes, over the %u that fit", desc.name, which,
                    size, (unsigned)(UINT16_MAX - ZIP64_EXTRA_MAX_SIZE));
    return APRESULT_OK;
}

// The checks every new entry goes through before anything is written.
// [need_codec]: the writer compresses it (a raw entry's method needn't be
// in the table).
static APRESULT check_new_entry(aparchive_writer writer, const APARCHIVE_ENTRY_DESC &desc, bool need_codec,
                                APARCHIVE_ERROR *error)
{
    if (writer->failed || writer->finished)
        return fail(error, APRESULT_INVALID_ARGUMENT, "the writer is %s", writer->finished ? "finished" : "unusable after an earlier failure");
    if (writer->streaming)
        return fail(error, APRESULT_INVALID_ARGUMENT, "a streamed entry is open: end it first");
    if (!desc.name)
        return fail(error, APRESULT_INVALID_ARGUMENT, "the entry has no name");
    size_t name_size = strlen(desc.name);
    if (!aparchive_is_safe_name(desc.name, name_size))
        return fail(error, APRESULT_INVALID_ARGUMENT, "'%s' isn't a safe entry name", desc.name);
    if (!is_valid_utf8(desc.name, name_size))
        return fail(error, APRESULT_INVALID_ARGUMENT, "'%s' isn't valid UTF-8", desc.name);
    if (!desc.utf8_name_flag && !is_ascii(desc.name, name_size))
        return fail(error, APRESULT_INVALID_ARGUMENT, "'%s' has non-ASCII characters but utf8_name_flag isn't set",
                    desc.name);
    if (is_directory_name(desc.name, name_size))
    {
        if (!writer->allow_directory_entries)
            return fail(error, APRESULT_INVALID_ARGUMENT,
                        "'%s' is a directory entry, which the writer's desc doesn't allow", desc.name);
        if (desc.method != APARCHIVE_METHOD_STORED)
            return fail(error, APRESULT_INVALID_ARGUMENT, "'%s' is a directory entry, so STORED", desc.name);
    }
    if (writer->names.count(desc.name))
        return fail(error, APRESULT_INVALID_ARGUMENT, "'%s' is already in the archive", desc.name);
    if (desc.alignment > 1 && (desc.alignment & (desc.alignment - 1)) != 0)
        return fail(error, APRESULT_INVALID_ARGUMENT, "alignment %u isn't a power of two", desc.alignment);
    APRESULT result = check_extra(desc, desc.extra, desc.extra_size, "extra", error);
    if (result != APRESULT_OK)
        return result;
    if (desc.central_extra)
    {
        result = check_extra(desc, desc.central_extra, desc.central_extra_size, "central_extra", error);
        if (result != APRESULT_OK)
            return result;
    }
    if (!desc.comment && desc.comment_size != 0)
        return fail(error, APRESULT_INVALID_ARGUMENT, "comment_size is %u but comment is null", desc.comment_size);
    if (need_codec && !aparchive_codecs_has(writer->codecs, desc.method))
        return fail(error, APRESULT_APARCHIVE_UNKNOWN_METHOD, "method %u isn't in the writer's codec table",
                    desc.method);
    if (writer->entries.size() >= UINT32_MAX)
        return fail(error, APRESULT_LIMIT, "the archive already has %u entries", UINT32_MAX);
    return APRESULT_OK;
}

// Appends [size] bytes to the sink. A failure leaves a partial record
// behind, so it ends the writer. An update's archive is cut first.
static bool emit(aparchive_writer writer, const void *data, uint64_t size)
{
    if (size == 0)
        return true;
    if (writer->truncate_pending)
    {
        writer->truncate_pending = false;
        if (!writer->truncate(writer->sink.user, writer->truncate_at))
        {
            writer->failed = true;
            return false;
        }
    }
    if (!writer->sink.write(writer->sink.user, data, size))
    {
        writer->failed = true;
        return false;
    }
    writer->offset += size;
    return true;
}

// An entry's local header, ready to write: its ZIP64 record (sizes, or
// zeros for a streamed entry), the caller's records, then the padding that
// puts the data on its alignment. May throw std::bad_alloc.
static APRESULT local_header_bytes(aparchive_writer writer, const APARCHIVE_ENTRY_DESC &desc, uint16_t flags,
                                   uint32_t crc, uint64_t stored_size, uint64_t size, bool zip64, bool streamed,
                                   std::vector<uint8_t> &header, APARCHIVE_ERROR *error)
{
    size_t name_size = strlen(desc.name);
    std::vector<uint8_t> extra;
    if (zip64)
    {
        put16(extra, EXTRA_ZIP64);
        put16(extra, 16);
        put64(extra, streamed ? 0 : size);
        put64(extra, streamed ? 0 : stored_size);
    }
    if (desc.extra_size)
        put_bytes(extra, desc.extra, desc.extra_size);
    if (desc.alignment > 1)
    {
        uint64_t data_offset = writer->offset + LOCAL_HEADER_SIZE + name_size + extra.size();
        uint64_t padding = (desc.alignment - data_offset % desc.alignment) % desc.alignment;
        // A record is at least its 4-byte header.
        while (padding != 0 && padding < 4)
            padding += desc.alignment;
        if (padding > UINT16_MAX || extra.size() + padding > UINT16_MAX)
            return fail(error, APRESULT_INVALID_ARGUMENT,
                        "'%s': alignment %u needs more padding than an extra field holds", desc.name, desc.alignment);
        if (padding)
        {
            put16(extra, EXTRA_PADDING);
            put16(extra, (uint16_t)(padding - 4));
            extra.insert(extra.end(), (size_t)padding - 4, (uint8_t)0);
        }
    }
    if (extra.size() > UINT16_MAX)
        return fail(error, APRESULT_INVALID_ARGUMENT, "'%s': the extra field would be %llu bytes", desc.name,
                    (unsigned long long)extra.size());

    // A streamed entry's sizes and CRC-32 are in its data descriptor; with
    // ZIP64 its sizes say "see the ZIP64 record", as Info-ZIP writes them.
    uint32_t small_stored = zip64 ? UINT32_MAX : streamed ? 0 : (uint32_t)stored_size;
    uint32_t small_size = zip64 ? UINT32_MAX : streamed ? 0 : (uint32_t)size;
    header.clear();
    header.reserve(LOCAL_HEADER_SIZE + name_size + extra.size());
    put32(header, SIG_LOCAL);                                 //  0
    put16(header, version_needed(desc.method, zip64));        //  4
    put16(header, flags);                                     //  6
    put16(header, desc.method);                               //  8
    put16(header, desc.dos_time);                             // 10
    put16(header, desc.dos_date);                             // 12
    put32(header, streamed ? 0 : crc);                        // 14
    put32(header, small_stored);                              // 18
    put32(header, small_size);                                // 22
    put16(header, (uint16_t)name_size);                       // 26
    put16(header, (uint16_t)extra.size());                    // 28
    put_bytes(header, desc.name, name_size);
    put_bytes(header, extra.data(), extra.size());
    return APRESULT_OK;
}

// What the central directory needs about a new entry, recorded in the
// writer. May throw std::bad_alloc.
static void record_entry(aparchive_writer writer, const APARCHIVE_ENTRY_DESC &desc, uint16_t flags, uint32_t crc,
                         uint64_t stored_size, uint64_t size, uint64_t header_offset)
{
    aparchive_written_entry_t entry;
    entry.name.assign(desc.name);
    entry.method = desc.method;
    entry.flags = flags;
    entry.dos_date = desc.dos_date;
    entry.dos_time = desc.dos_time;
    entry.crc32 = crc;
    entry.compressed_size = stored_size;
    entry.uncompressed_size = size;
    entry.local_header_offset = header_offset;
    entry.zip64 = desc.zip64;
    entry.version_made_by = (uint16_t)(desc.host_system << 8 | VERSION_MADE_BY);
    entry.internal_attributes = desc.internal_attributes;
    entry.external_attributes = desc.external_attributes;
    if (desc.central_extra)
        put_bytes(entry.extra, desc.central_extra, desc.central_extra_size);
    else if (desc.extra_size)
        put_bytes(entry.extra, desc.extra, desc.extra_size);
    if (desc.comment_size)
        put_bytes(entry.comment, desc.comment, desc.comment_size);
    writer->entries.push_back(std::move(entry));
    writer->names.insert(desc.name);
}

// Writes an entry whose stored bytes, sizes and CRC-32 are known. The
// entry was checked. May throw std::bad_alloc.
static APRESULT write_whole(aparchive_writer writer, const APARCHIVE_ENTRY_DESC &desc, const void *stored,
                            uint64_t stored_size, uint64_t size, uint32_t crc, APARCHIVE_ERROR *error)
{
    bool zip64 = desc.zip64 || size >= UINT32_MAX || stored_size >= UINT32_MAX;
    uint16_t flags = desc.utf8_name_flag ? FLAG_UTF8 : 0;
    uint64_t header_offset = writer->offset;
    std::vector<uint8_t> header;
    APRESULT result = local_header_bytes(writer, desc, flags, crc, stored_size, size, zip64, false, header, error);
    if (result != APRESULT_OK)
        return result;
    // Everything that can run out of memory comes before the first byte is
    // written.
    record_entry(writer, desc, flags, crc, stored_size, size, header_offset);
    if (!emit(writer, header.data(), header.size()) || !emit(writer, stored, stored_size))
        return fail(error, APRESULT_IO, "the sink failed while '%s' was being written", desc.name);
    return APRESULT_OK;
}

// May throw std::bad_alloc.
static APRESULT add_entry(aparchive_writer writer, const APARCHIVE_ENTRY_DESC &desc, const void *data, uint64_t size,
                          APARCHIVE_ERROR *error)
{
    APRESULT result = check_new_entry(writer, desc, true, error);
    if (result != APRESULT_OK)
        return result;
    if (is_directory_name(desc.name, strlen(desc.name)) && size != 0)
        return fail(error, APRESULT_INVALID_ARGUMENT, "'%s' is a directory entry, so has no data", desc.name);

    // The bytes as they'll be stored.
    const void *stored = data;
    uint64_t stored_size = size;
    std::vector<uint8_t> compressed;
    if (desc.method != APARCHIVE_METHOD_STORED)
    {
        const APARCHIVE_CODEC *codec = writer->codecs->find(desc.method);
        if (!codec->compress)
            return fail(error, APRESULT_UNSUPPORTED, "method %u's codec only decompresses", desc.method);
        uint64_t bound = codec->compress_bound(codec->user, size);
        if (bound > (uint64_t)SIZE_MAX)
            return fail(error, APRESULT_OUT_OF_MEMORY, "'%s' is too big to compress in memory", desc.name);
        // Never empty, so the codec always has a buffer to write to.
        compressed.resize(bound ? (size_t)bound : 1);
        if (!codec->compress(codec->user, desc.level, data, size, compressed.data(), bound, &stored_size) ||
            stored_size > bound)
            return fail(error, APRESULT_INVALID_ARGUMENT, "method %u failed to compress '%s' at level %d", desc.method,
                        desc.name, desc.level);
        stored = compressed.data();
    }
    return write_whole(writer, desc, stored, stored_size, size, aparchive_crc32(0, data, size), error);
}

// May throw std::bad_alloc.
static APRESULT add_raw_entry(aparchive_writer writer, const APARCHIVE_ENTRY_DESC &desc, const void *stored,
                              uint64_t stored_size, uint64_t size, uint32_t crc, APARCHIVE_ERROR *error)
{
    APRESULT result = check_new_entry(writer, desc, false, error);
    if (result != APRESULT_OK)
        return result;
    if (is_directory_name(desc.name, strlen(desc.name)) && size != 0)
        return fail(error, APRESULT_INVALID_ARGUMENT, "'%s' is a directory entry, so has no data", desc.name);
    if (size != 0 && stored_size == 0)
        return fail(error, APRESULT_INVALID_ARGUMENT, "'%s' has %llu bytes of data but none stored", desc.name,
                    (unsigned long long)size);
    if (desc.method == APARCHIVE_METHOD_STORED && stored_size != size)
        return fail(error, APRESULT_INVALID_ARGUMENT, "'%s' is STORED but its sizes differ", desc.name);
    if (desc.method == APARCHIVE_METHOD_STORED && aparchive_crc32(0, stored, stored_size) != crc)
        return fail(error, APRESULT_INVALID_ARGUMENT, "'%s' is STORED and its data doesn't have the CRC-32 given",
                    desc.name);
    return write_whole(writer, desc, stored, stored_size, size, crc, error);
}

// ---- Streamed entries ---------------------------------------------------------

// How much compressed output a streamed entry gathers before the sink.
static const size_t STREAM_BUFFER_SIZE = 64 * 1024;

// May throw std::bad_alloc.
static APRESULT begin_entry(aparchive_writer writer, const APARCHIVE_ENTRY_DESC &desc, APARCHIVE_ERROR *error)
{
    APRESULT result = check_new_entry(writer, desc, true, error);
    if (result != APRESULT_OK)
        return result;
    if (is_directory_name(desc.name, strlen(desc.name)))
        return fail(error, APRESULT_INVALID_ARGUMENT, "'%s' is a directory entry, which has no data to stream",
                    desc.name);
    const APARCHIVE_CODEC *codec = nullptr;
    if (desc.method != APARCHIVE_METHOD_STORED)
    {
        codec = writer->codecs->find(desc.method);
        if (!codec->stream_begin)
            return fail(error, APRESULT_UNSUPPORTED, "method %u's codec works on whole entries only", desc.method);
    }
    uint16_t flags = (uint16_t)(FLAG_DATA_DESCRIPTOR | (desc.utf8_name_flag ? FLAG_UTF8 : 0));
    uint64_t header_offset = writer->offset;
    std::vector<uint8_t> header;
    result = local_header_bytes(writer, desc, flags, 0, 0, 0, desc.zip64, true, header, error);
    if (result != APRESULT_OK)
        return result;
    writer->stream_buffer.resize(STREAM_BUFFER_SIZE);
    record_entry(writer, desc, flags, 0, 0, 0, header_offset);
    // The codec last: nothing after it can fail but the sink.
    void *state = nullptr;
    if (codec)
    {
        state = codec->stream_begin(codec->user, true, desc.level);
        if (!state)
        {
            writer->names.erase(writer->entries.back().name);
            writer->entries.pop_back();
            return fail(error, APRESULT_INVALID_ARGUMENT, "method %u can't stream '%s' at level %d", desc.method,
                        desc.name, desc.level);
        }
    }
    writer->streaming = true;
    writer->stream_codec = codec;
    writer->stream_state = state;
    writer->stream_crc = 0;
    writer->stream_in = 0;
    writer->stream_out = 0;
    if (!emit(writer, header.data(), header.size()))
        return fail(error, APRESULT_IO, "the sink failed while '%s' was being written", desc.name);
    return APRESULT_OK;
}

// Ends the writer's stream state, finished or not.
static void close_stream(aparchive_writer writer)
{
    if (writer->stream_codec && writer->stream_state)
        writer->stream_codec->stream_end(writer->stream_codec->user, writer->stream_state);
    writer->stream_codec = nullptr;
    writer->stream_state = nullptr;
    writer->streaming = false;
}

// Emits [size] stored bytes of the open entry, within its size limit.
static APRESULT emit_stream(aparchive_writer writer, const void *data, uint64_t size, APARCHIVE_ERROR *error)
{
    const aparchive_written_entry_t &entry = writer->entries.back();
    if (!entry.zip64 && (size >= UINT32_MAX || writer->stream_out >= UINT32_MAX - size))
    {
        writer->failed = true;
        return fail(error, APRESULT_LIMIT, "'%s' reached 4 GiB stored, without zip64", entry.name.c_str());
    }
    if (!emit(writer, data, size))
        return fail(error, APRESULT_IO, "the sink failed while '%s' was being written", entry.name.c_str());
    writer->stream_out += size;
    return APRESULT_OK;
}

// Runs the open entry's codec over [data] (with [finish], to its end) and
// writes what comes out.
static APRESULT run_stream(aparchive_writer writer, const uint8_t *data, uint64_t size, bool finish,
                           APARCHIVE_ERROR *error)
{
    const APARCHIVE_CODEC *codec = writer->stream_codec;
    uint8_t *out = writer->stream_buffer.data();
    for (;;)
    {
        uint64_t used = 0, made = 0;
        bool done = false;
        if (!codec->stream_process(codec->user, writer->stream_state, data, size, &used, out, STREAM_BUFFER_SIZE,
                                   &made, finish, &done) ||
            used > size || made > STREAM_BUFFER_SIZE)
        {
            writer->failed = true;
            return fail(error, APRESULT_INVALID_ARGUMENT, "method %u failed to compress '%s'",
                        writer->entries.back().method, writer->entries.back().name.c_str());
        }
        data += used;
        size -= used;
        APRESULT result = emit_stream(writer, out, made, error);
        if (result != APRESULT_OK)
            return result;
        if (finish ? done : size == 0 && made < STREAM_BUFFER_SIZE)
            return APRESULT_OK; // what the codec still holds comes out later
        if (used == 0 && made == 0)
        {
            writer->failed = true;
            return fail(error, APRESULT_INVALID_ARGUMENT, "method %u stopped compressing '%s'",
                        writer->entries.back().method, writer->entries.back().name.c_str());
        }
    }
}

static APRESULT write_entry_data(aparchive_writer writer, const void *data, uint64_t size, APARCHIVE_ERROR *error)
{
    const aparchive_written_entry_t &entry = writer->entries.back();
    if (!entry.zip64 && (size >= UINT32_MAX || writer->stream_in >= UINT32_MAX - size))
    {
        writer->failed = true;
        return fail(error, APRESULT_LIMIT, "'%s' reached 4 GiB, without zip64", entry.name.c_str());
    }
    writer->stream_crc = aparchive_crc32(writer->stream_crc, data, size);
    writer->stream_in += size;
    if (!writer->stream_codec)
        return emit_stream(writer, data, size, error);
    if (size == 0)
        return APRESULT_OK;
    return run_stream(writer, (const uint8_t *)data, size, false, error);
}

// May throw std::bad_alloc.
static APRESULT end_entry(aparchive_writer writer, APARCHIVE_ERROR *error)
{
    if (writer->stream_codec)
    {
        APRESULT result = run_stream(writer, nullptr, 0, true, error);
        if (result != APRESULT_OK)
            return result;
    }
    aparchive_written_entry_t &entry = writer->entries.back();
    entry.crc32 = writer->stream_crc;
    entry.uncompressed_size = writer->stream_in;
    entry.compressed_size = writer->stream_out;
    close_stream(writer);
    std::vector<uint8_t> descriptor;
    put32(descriptor, SIG_DESCRIPTOR);
    put32(descriptor, entry.crc32);
    if (entry.zip64)
    {
        put64(descriptor, entry.compressed_size);
        put64(descriptor, entry.uncompressed_size);
    }
    else
    {
        put32(descriptor, (uint32_t)entry.compressed_size);
        put32(descriptor, (uint32_t)entry.uncompressed_size);
    }
    if (!emit(writer, descriptor.data(), descriptor.size()))
        return fail(error, APRESULT_IO, "the sink failed while '%s' was being written", entry.name.c_str());
    return APRESULT_OK;
}

// Whether [comment] holds the end record's signature.
static bool holds_end_signature(const uint8_t *comment, size_t size)
{
    for (size_t i = 0; i + 4 <= size; i++)
        if (get32(comment + i) == SIG_END)
            return true;
    return false;
}

// May throw std::bad_alloc.
static APRESULT finish(aparchive_writer writer, APARCHIVE_ERROR *error)
{
    uint64_t directory_offset = writer->offset;
    std::vector<uint8_t> record;
    for (const aparchive_written_entry_t &entry : writer->entries)
    {
        // A ZIP64 record holds only the fields that don't fit, in this
        // order.
        bool uncompressed64 = entry.zip64 || entry.uncompressed_size >= UINT32_MAX;
        bool compressed64 = entry.zip64 || entry.compressed_size >= UINT32_MAX;
        bool offset64 = entry.local_header_offset >= UINT32_MAX;
        uint16_t zip64_size = (uint16_t)(((int)uncompressed64 + (int)compressed64 + (int)offset64) * 8);
        size_t extra_size = (zip64_size ? 4 + zip64_size : 0) + entry.extra.size();

        record.clear();
        put32(record, SIG_CENTRAL);                                                        //  0
        put16(record, entry.version_made_by);                                              //  4
        put16(record, version_needed(entry.method, zip64_size != 0));                      //  6
        put16(record, entry.flags);                                                        //  8
        put16(record, entry.method);                                                       // 10
        put16(record, entry.dos_time);                                                     // 12
        put16(record, entry.dos_date);                                                     // 14
        put32(record, entry.crc32);                                                        // 16
        put32(record, compressed64 ? UINT32_MAX : (uint32_t)entry.compressed_size);        // 20
        put32(record, uncompressed64 ? UINT32_MAX : (uint32_t)entry.uncompressed_size);    // 24
        put16(record, (uint16_t)entry.name.size());                                        // 28
        put16(record, (uint16_t)extra_size);                                               // 30
        put16(record, (uint16_t)entry.comment.size());                                     // 32
        put16(record, 0);                                                                  // 34 disk
        put16(record, entry.internal_attributes);                                          // 36
        put32(record, entry.external_attributes);                                          // 38
        put32(record, offset64 ? UINT32_MAX : (uint32_t)entry.local_header_offset);        // 42
        put_bytes(record, entry.name.data(), entry.name.size());
        if (zip64_size)
        {
            put16(record, EXTRA_ZIP64);
            put16(record, zip64_size);
            if (uncompressed64)
                put64(record, entry.uncompressed_size);
            if (compressed64)
                put64(record, entry.compressed_size);
            if (offset64)
                put64(record, entry.local_header_offset);
        }
        put_bytes(record, entry.extra.data(), entry.extra.size());
        put_bytes(record, entry.comment.data(), entry.comment.size());
        if (!emit(writer, record.data(), record.size()))
            return fail(error, APRESULT_IO, "the sink failed while the central directory was being written");
    }
    uint64_t directory_size = writer->offset - directory_offset;
    uint64_t entry_count = writer->entries.size();

    record.clear();
    bool count64 = entry_count >= UINT16_MAX;
    bool size64 = directory_size >= UINT32_MAX;
    bool offset64 = directory_offset >= UINT32_MAX;
    if (count64 || size64 || offset64)
    {
        uint64_t end64_offset = writer->offset;
        put32(record, SIG_END64);            //  0
        put64(record, END64_SIZE - 12);      //  4 the record's size past this field
        put16(record, VERSION_MADE_BY);      // 12
        put16(record, 45);                   // 14 version needed
        put32(record, 0);                    // 16 this disk
        put32(record, 0);                    // 20 the directory's disk
        put64(record, entry_count);          // 24 entries on this disk
        put64(record, entry_count);          // 32 entries
        put64(record, directory_size);       // 40
        put64(record, directory_offset);     // 48
        put32(record, SIG_LOCATOR64);        //  0
        put32(record, 0);                    //  4 the ZIP64 end record's disk
        put64(record, end64_offset);         //  8
        put32(record, 1);                    // 16 disks
    }
    put32(record, SIG_END);                                                  //  0
    put16(record, 0);                                                        //  4 this disk
    put16(record, 0);                                                        //  6 the directory's disk
    put16(record, count64 ? UINT16_MAX : (uint16_t)entry_count);             //  8 entries on this disk
    put16(record, count64 ? UINT16_MAX : (uint16_t)entry_count);             // 10 entries
    put32(record, size64 ? UINT32_MAX : (uint32_t)directory_size);           // 12
    put32(record, offset64 ? UINT32_MAX : (uint32_t)directory_offset);       // 16
    put16(record, (uint16_t)writer->comment.size());                         // 20 comment size
    put_bytes(record, writer->comment.data(), writer->comment.size());
    if (!emit(writer, record.data(), record.size()))
        return fail(error, APRESULT_IO, "the sink failed while the end records were being written");
    writer->finished = true;
    return APRESULT_OK;
}

// An update writer's copy of a reader's entries. May throw std::bad_alloc.
static void copy_entries(aparchive_writer writer, aparchive_reader reader)
{
    writer->entries.reserve(reader->entries.size());
    for (size_t i = 0; i < reader->entries.size(); i++)
    {
        const APARCHIVE_ENTRY_INFO &info = reader->entries[i];
        aparchive_written_entry_t entry;
        entry.name.assign(info.name, info.name_size);
        entry.method = info.method;
        entry.flags = info.flags;
        entry.dos_date = info.dos_date;
        entry.dos_time = info.dos_time;
        entry.crc32 = info.crc32;
        entry.compressed_size = info.compressed_size;
        entry.uncompressed_size = info.uncompressed_size;
        entry.local_header_offset = info.local_header_offset;
        entry.zip64 = reader->central_zip64[i];
        entry.version_made_by = info.version_made_by;
        entry.internal_attributes = info.internal_attributes;
        entry.external_attributes = info.external_attributes;
        // The extra field less its ZIP64 records, which finish() writes
        // where they're needed. What follows a record that isn't whole is
        // kept as it is.
        const uint8_t *extra = (const uint8_t *)info.extra;
        size_t at = 0;
        while (info.extra_size - at >= 4)
        {
            size_t record_size = get16(extra + at + 2);
            if (info.extra_size - at - 4 < record_size)
                break;
            if (get16(extra + at) != EXTRA_ZIP64)
                put_bytes(entry.extra, extra + at, 4 + record_size);
            at += 4 + record_size;
        }
        if (at < info.extra_size)
            put_bytes(entry.extra, extra + at, info.extra_size - at);
        if (info.comment_size)
            put_bytes(entry.comment, info.comment, info.comment_size);
        writer->names.insert(entry.name);
        writer->entries.push_back(std::move(entry));
    }
    writer->comment = reader->comment;
    writer->offset = reader->directory_offset;
    writer->truncate_at = reader->directory_offset;
}

// ---- Reading ------------------------------------------------------------------

// Reads [count] bytes at [offset]. [what] names what was being read, for
// the message.
static APRESULT read_source(const APARCHIVE_SOURCE &source, uint64_t offset, void *buffer, uint64_t count,
                            APARCHIVE_ERROR *error, const char *what)
{
    if (offset > source.size || count > source.size - offset)
        return fail(error, APRESULT_APARCHIVE_MALFORMED, "%s runs past the end of the archive", what);
    if (count != 0 && !source.read(source.user, offset, buffer, count))
        return fail(error, APRESULT_IO, "the source failed reading %s", what);
    return APRESULT_OK;
}

// Whether the 4 bytes at [offset] are [signature]; false where they can't
// be read.
static bool signature_at(const APARCHIVE_SOURCE &source, uint64_t offset, uint32_t signature)
{
    uint8_t bytes[4];
    if (offset > source.size || source.size - offset < 4)
        return false;
    return source.read(source.user, offset, bytes, 4) && get32(bytes) == signature;
}

static APRESULT locate_data(aparchive_reader reader, uint32_t index, uint64_t *out_offset, bool *out_local_zip64,
                            APARCHIVE_ERROR *error);

// With APARCHIVE_STRICT_NO_GAPS: every byte from the start to the end
// records belongs to an entry or a record. May throw std::bad_alloc.
static APRESULT check_no_gaps(aparchive_reader reader, uint64_t directory_size, uint64_t directory_limit,
                              uint64_t records_gap, APARCHIVE_ERROR *error)
{
    std::vector<uint32_t> order(reader->entries.size());
    for (uint32_t i = 0; i < (uint32_t)order.size(); i++)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        return reader->entries[a].local_header_offset < reader->entries[b].local_header_offset;
    });
    uint64_t at = 0;
    for (uint32_t index : order)
    {
        const APARCHIVE_ENTRY_INFO &info = reader->entries[index];
        if (info.local_header_offset != at)
            return fail(error, APRESULT_APARCHIVE_MALFORMED, "%llu bytes no entry accounts for before '%s'",
                        (unsigned long long)(info.local_header_offset - at), info.name);
        uint64_t data_offset;
        bool local_zip64;
        APRESULT result = locate_data(reader, index, &data_offset, &local_zip64, error);
        if (result != APRESULT_OK)
            return result;
        at = data_offset + info.compressed_size;
        if (info.flags & FLAG_DATA_DESCRIPTOR)
        {
            // An optional signature, the CRC-32, then both sizes: 8 bytes
            // each when the local header has a ZIP64 record.
            uint8_t descriptor[24];
            size_t size_bytes = local_zip64 ? 8 : 4;
            uint64_t room = reader->directory_offset - at;
            size_t wanted = 4 + 4 + 2 * size_bytes;
            size_t got = room < wanted ? (size_t)room : wanted;
            result = read_source(reader->source, at, descriptor, got, error, info.name);
            if (result != APRESULT_OK)
                return result;
            size_t skip = got >= 4 && get32(descriptor) == SIG_DESCRIPTOR ? 4 : 0;
            if (got < skip + 4 + 2 * size_bytes)
                return fail(error, APRESULT_APARCHIVE_MALFORMED, "'%s': its data descriptor runs into the directory",
                            info.name);
            const uint8_t *fields = descriptor + skip;
            uint64_t compressed = local_zip64 ? get64(fields + 4) : get32(fields + 4);
            uint64_t uncompressed = local_zip64 ? get64(fields + 12) : get32(fields + 8);
            if (get32(fields) != info.crc32 || compressed != info.compressed_size ||
                uncompressed != info.uncompressed_size)
                return fail(error, APRESULT_APARCHIVE_MALFORMED,
                            "'%s': its data descriptor disagrees with the central directory", info.name);
            at += skip + 4 + 2 * size_bytes;
        }
    }
    if (at != reader->directory_offset)
        return fail(error, APRESULT_APARCHIVE_MALFORMED, "%llu bytes no entry accounts for before the directory",
                    (unsigned long long)(reader->directory_offset - at));
    if (reader->directory_offset + directory_size != directory_limit || records_gap != 0)
        return fail(error, APRESULT_APARCHIVE_MALFORMED, "bytes no record accounts for after the directory");
    return APRESULT_OK;
}

// Reads the end records and the central directory into [reader]. May throw
// std::bad_alloc.
static APRESULT open_reader(const APARCHIVE_READER_DESC &desc, aparchive_reader reader, APARCHIVE_ERROR *error)
{
    const APARCHIVE_SOURCE &source = desc.source;
    if (source.size < END_SIZE)
        return fail(error, APRESULT_APARCHIVE_NOT_AN_ARCHIVE, "%llu bytes is too small to be a ZIP archive",
                    (unsigned long long)source.size);

    // The end record is the last thing in the archive, followed only by its
    // comment (up to 65,535 bytes): search backwards for a signature whose
    // comment size reaches exactly the end.
    size_t tail_size = source.size < END_SIZE + UINT16_MAX ? (size_t)source.size : END_SIZE + UINT16_MAX;
    std::vector<uint8_t> tail(tail_size);
    APRESULT result = read_source(source, source.size - tail_size, tail.data(), tail_size, error, "the end record");
    if (result != APRESULT_OK)
        return result;
    size_t end_at = tail_size;
    for (size_t i = tail_size - END_SIZE + 1; i-- > 0;)
    {
        if (get32(tail.data() + i) == SIG_END && get16(tail.data() + i + 20) == tail_size - i - END_SIZE)
        {
            end_at = i;
            break;
        }
    }
    if (end_at == tail_size)
        return fail(error, APRESULT_APARCHIVE_NOT_AN_ARCHIVE, "no end of central directory record: not a ZIP archive");
    const uint8_t *end = tail.data() + end_at;
    uint64_t end_offset = source.size - tail_size + end_at;
    reader->comment.assign(end + END_SIZE, end + END_SIZE + get16(end + 20));
    if ((desc.strict & APARCHIVE_STRICT_NO_ARCHIVE_COMMENT) && !reader->comment.empty())
        return fail(error, APRESULT_APARCHIVE_MALFORMED, "the archive has a comment, which strict refuses");

    uint32_t disk = get16(end + 4);
    uint32_t directory_disk = get16(end + 6);
    uint64_t disk_entry_count = get16(end + 8);
    uint64_t entry_count = get16(end + 10);
    uint64_t directory_size = get32(end + 12);
    uint64_t directory_offset = get32(end + 16);
    // Where the directory and everything before it must end.
    uint64_t directory_limit = end_offset;
    // Bytes between the ZIP64 end record and its locator, for strict.
    uint64_t records_gap = 0;

    // A field at its largest value means "see the ZIP64 end record", which
    // a locator just before the end record points to. With no locator the
    // values are literal.
    bool any_marker = disk == UINT16_MAX || directory_disk == UINT16_MAX || disk_entry_count == UINT16_MAX ||
                      entry_count == UINT16_MAX || directory_size == UINT32_MAX || directory_offset == UINT32_MAX;
    if (any_marker && end_offset >= LOCATOR64_SIZE)
    {
        uint8_t locator[LOCATOR64_SIZE];
        uint64_t locator_offset = end_offset - LOCATOR64_SIZE;
        result = read_source(source, locator_offset, locator, sizeof(locator), error, "the ZIP64 locator");
        if (result != APRESULT_OK)
            return result;
        if (get32(locator) == SIG_LOCATOR64)
        {
            if (get32(locator + 4) != 0 || get32(locator + 16) > 1)
                return fail(error, APRESULT_UNSUPPORTED, "the archive is split over several disks");
            uint64_t end64_offset = get64(locator + 8);
            if (end64_offset > locator_offset || locator_offset - end64_offset < END64_SIZE)
                return fail(error, APRESULT_APARCHIVE_MALFORMED, "the ZIP64 end record isn't where its locator says");
            // Not there, but just before the locator: data in front of the
            // archive that its offsets don't count (see below).
            if (!signature_at(source, end64_offset, SIG_END64) &&
                signature_at(source, locator_offset - END64_SIZE, SIG_END64))
                end64_offset = locator_offset - END64_SIZE;
            uint8_t end64[END64_SIZE];
            result = read_source(source, end64_offset, end64, sizeof(end64), error, "the ZIP64 end record");
            if (result != APRESULT_OK)
                return result;
            // Its size field counts what follows it (12 bytes in); the
            // record may be longer than the fields read here.
            uint64_t end64_size = get64(end64 + 4);
            if (get32(end64) != SIG_END64 || end64_size < END64_SIZE - 12 ||
                end64_size > locator_offset - end64_offset - 12)
                return fail(error, APRESULT_APARCHIVE_MALFORMED, "the ZIP64 end record is malformed");
            records_gap = locator_offset - end64_offset - 12 - end64_size;
            disk = get32(end64 + 16);
            directory_disk = get32(end64 + 20);
            disk_entry_count = get64(end64 + 24);
            entry_count = get64(end64 + 32);
            directory_size = get64(end64 + 40);
            directory_offset = get64(end64 + 48);
            directory_limit = end64_offset;
        }
    }
    if (disk != 0 || directory_disk != 0 || disk_entry_count != entry_count)
        return fail(error, APRESULT_UNSUPPORTED, "the archive is split over several disks");

    // A directory that isn't where the end record says, but ends just
    // before it, has bytes in front of the archive that its offsets don't
    // count. Only an archive that would otherwise fail is moved: one that
    // opened before opens as it did.
    bool fits = directory_offset <= directory_limit && directory_size <= directory_limit - directory_offset;
    bool at_stated = entry_count == 0 || (fits && signature_at(source, directory_offset, SIG_CENTRAL));
    if (!at_stated && directory_size <= directory_limit)
    {
        uint64_t candidate = directory_limit - directory_size;
        if (candidate > directory_offset && signature_at(source, candidate, SIG_CENTRAL))
        {
            reader->shift = candidate - directory_offset;
            directory_offset = candidate;
            fits = true;
        }
    }
    if (!fits)
        return fail(error, APRESULT_APARCHIVE_MALFORMED, "the central directory doesn't fit in the archive");

    // The limits on the directory, before any of it is read.
    if (entry_count > desc.max_entry_count)
        return fail(error, APRESULT_LIMIT, "%llu entries, over the limit of %u", (unsigned long long)entry_count,
                    desc.max_entry_count);
    if (directory_size > desc.max_central_directory_size)
        return fail(error, APRESULT_LIMIT, "a central directory of %llu bytes, over the limit of %llu",
                    (unsigned long long)directory_size, (unsigned long long)desc.max_central_directory_size);
    if (entry_count > directory_size / CENTRAL_HEADER_SIZE)
        return fail(error, APRESULT_APARCHIVE_MALFORMED, "%llu entries can't fit in a central directory of %llu bytes",
                    (unsigned long long)entry_count, (unsigned long long)directory_size);
    if (directory_size > (uint64_t)SIZE_MAX)
        return fail(error, APRESULT_OUT_OF_MEMORY, "the central directory is too big to hold in memory");

    reader->source = source;
    reader->codecs = desc.codecs;
    reader->directory_offset = directory_offset;
    reader->central_directory.resize((size_t)directory_size);
    result = read_source(source, directory_offset, reader->central_directory.data(), directory_size, error,
                         "the central directory");
    if (result != APRESULT_OK)
        return result;

    // Reserved once, never to grow: entries, and the name index's keys,
    // point into it. Each name is copied once it is known to fit in what is
    // left of the directory, before the entries after it are checked, so the
    // bound is the directory's size, plus a NUL each - not the directory
    // less every entry's fixed header, which holds only for an archive whose
    // every entry fits.
    reader->names.reserve((size_t)(directory_size + entry_count));
    reader->entries.reserve((size_t)entry_count);
    reader->central_zip64.reserve((size_t)entry_count);
    reader->name_index.reserve((size_t)entry_count);

    const uint8_t *directory = reader->central_directory.data();
    size_t at = 0;
    uint64_t total_uncompressed = 0;
    for (uint64_t k = 0; k < entry_count; k++)
    {
        size_t left = (size_t)directory_size - at;
        const uint8_t *header = directory + at;
        if (left < CENTRAL_HEADER_SIZE || get32(header) != SIG_CENTRAL)
            return fail(error, APRESULT_APARCHIVE_MALFORMED, "central directory entry %llu is malformed",
                        (unsigned long long)k);
        uint16_t flags = get16(header + 8);
        uint16_t method = get16(header + 10);
        uint64_t compressed_size = get32(header + 20);
        uint64_t uncompressed_size = get32(header + 24);
        size_t name_size = get16(header + 28);
        size_t extra_size = get16(header + 30);
        size_t comment_size = get16(header + 32);
        uint32_t entry_disk = get16(header + 34);
        uint64_t header_offset = get32(header + 42);
        if (left - CENTRAL_HEADER_SIZE < name_size + extra_size + comment_size)
            return fail(error, APRESULT_APARCHIVE_MALFORMED, "central directory entry %llu runs past the directory",
                        (unsigned long long)k);
        const char *stored_name = (const char *)header + CENTRAL_HEADER_SIZE;
        const uint8_t *extra = header + CENTRAL_HEADER_SIZE + name_size;
        const uint8_t *comment = extra + extra_size;
        at += CENTRAL_HEADER_SIZE + name_size + extra_size + comment_size;
        bool sizes_zip64 = uncompressed_size == UINT32_MAX || compressed_size == UINT32_MAX;

        // The name, copied out so it can be NUL-terminated.
        size_t name_at = reader->names.size();
        reader->names.insert(reader->names.end(), stored_name, stored_name + name_size);
        reader->names.push_back('\0');
        const char *name = reader->names.data() + name_at;
        if (!aparchive_is_safe_name(stored_name, name_size))
            return fail(error, APRESULT_APARCHIVE_MALFORMED, "entry %llu's name '%s' isn't safe", (unsigned long long)k,
                        name);
        bool is_directory = stored_name[name_size - 1] == '/';
        if ((desc.strict & APARCHIVE_STRICT_NO_DIRECTORY_ENTRIES) && is_directory)
            return fail(error, APRESULT_APARCHIVE_MALFORMED, "'%s' is a directory entry, which strict refuses", name);
        if ((desc.strict & APARCHIVE_STRICT_NO_ENTRY_COMMENTS) && comment_size != 0)
            return fail(error, APRESULT_APARCHIVE_MALFORMED, "'%s' has a comment, which strict refuses", name);

        if (flags & FLAGS_ENCRYPTION)
            return fail(error, APRESULT_UNSUPPORTED, "'%s' is encrypted", name);
        if (flags & FLAG_PATCH_DATA)
            return fail(error, APRESULT_UNSUPPORTED, "'%s' is compressed patch data", name);

        // ZIP64: a field at its largest value has its real value in the
        // ZIP64 record, which holds only those fields, in this order.
        if (uncompressed_size == UINT32_MAX || compressed_size == UINT32_MAX || header_offset == UINT32_MAX ||
            entry_disk == UINT16_MAX)
        {
            const uint8_t *zip64;
            size_t zip64_size;
            if (find_extra(extra, extra_size, EXTRA_ZIP64, &zip64, &zip64_size))
            {
                size_t zip64_at = 0;
                auto take = [&](uint64_t &value) {
                    if (zip64_size - zip64_at < 8)
                        return false;
                    value = get64(zip64 + zip64_at);
                    zip64_at += 8;
                    return true;
                };
                if ((uncompressed_size == UINT32_MAX && !take(uncompressed_size)) ||
                    (compressed_size == UINT32_MAX && !take(compressed_size)) ||
                    (header_offset == UINT32_MAX && !take(header_offset)))
                    return fail(error, APRESULT_APARCHIVE_MALFORMED, "'%s' has a ZIP64 record that's too short", name);
                if (entry_disk == UINT16_MAX)
                {
                    if (zip64_size - zip64_at < 4)
                        return fail(error, APRESULT_APARCHIVE_MALFORMED, "'%s' has a ZIP64 record that's too short",
                                    name);
                    entry_disk = get32(zip64 + zip64_at);
                }
            }
        }
        if (entry_disk != 0)
            return fail(error, APRESULT_UNSUPPORTED, "'%s' is on another disk of a split archive", name);
        if (header_offset > UINT64_MAX - reader->shift)
            return fail(error, APRESULT_APARCHIVE_MALFORMED, "'%s' doesn't fit before the central directory", name);
        header_offset += reader->shift;

        // Sizes that can't be true, then the limits.
        if (method == APARCHIVE_METHOD_STORED && compressed_size != uncompressed_size)
            return fail(error, APRESULT_APARCHIVE_MALFORMED, "'%s' is STORED but its sizes differ", name);
        if (uncompressed_size != 0 && compressed_size == 0)
            return fail(error, APRESULT_APARCHIVE_MALFORMED, "'%s' has data but no stored bytes", name);
        if (header_offset > directory_offset || directory_offset - header_offset < LOCAL_HEADER_SIZE + name_size ||
            directory_offset - header_offset - LOCAL_HEADER_SIZE - name_size < compressed_size)
            return fail(error, APRESULT_APARCHIVE_MALFORMED, "'%s' doesn't fit before the central directory", name);
        if (uncompressed_size > desc.max_entry_uncompressed_size)
            return fail(error, APRESULT_LIMIT, "'%s' is %llu bytes, over the limit of %llu", name,
                        (unsigned long long)uncompressed_size, (unsigned long long)desc.max_entry_uncompressed_size);
        if (uncompressed_size > UINT64_MAX - total_uncompressed ||
            total_uncompressed + uncompressed_size > desc.max_total_uncompressed_size)
            return fail(error, APRESULT_LIMIT, "the entries together are over the limit of %llu bytes",
                        (unsigned long long)desc.max_total_uncompressed_size);
        total_uncompressed += uncompressed_size;
        if (compressed_size != 0)
        {
            uint64_t ratio = uncompressed_size / compressed_size + (uncompressed_size % compressed_size != 0);
            if (ratio > desc.max_compression_ratio)
                return fail(error, APRESULT_LIMIT, "'%s' compresses %llu:1, over the limit of %u:1", name,
                            (unsigned long long)ratio, desc.max_compression_ratio);
        }

        APARCHIVE_ENTRY_INFO info{}; // its base is the caller's, set as it's handed out
        info.name = name;
        info.name_size = (uint16_t)name_size;
        info.method = method;
        info.flags = flags;
        info.dos_time = get16(header + 12);
        info.dos_date = get16(header + 14);
        info.crc32 = get32(header + 16);
        info.compressed_size = compressed_size;
        info.uncompressed_size = uncompressed_size;
        info.local_header_offset = header_offset;
        info.extra = extra_size ? extra : nullptr;
        info.extra_size = (uint16_t)extra_size;
        info.is_directory = is_directory;
        info.version_made_by = get16(header + 4);
        info.internal_attributes = get16(header + 36);
        info.external_attributes = get32(header + 38);
        info.comment = comment_size ? comment : nullptr;
        info.comment_size = (uint16_t)comment_size;
        if (!reader->name_index.emplace(std::string_view(name, name_size), (uint32_t)k).second)
            return fail(error, APRESULT_APARCHIVE_MALFORMED, "'%s' is in the archive twice", name);
        reader->entries.push_back(info);
        reader->central_zip64.push_back(sizes_zip64);
    }

    // No two entries may share bytes: one small entry listed many times
    // over is how an archive claims far more data than it holds. Each
    // entry takes at least its local header, name and stored bytes.
    std::vector<uint32_t> order(reader->entries.size());
    for (uint32_t i = 0; i < (uint32_t)order.size(); i++)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        return reader->entries[a].local_header_offset < reader->entries[b].local_header_offset;
    });
    for (size_t i = 1; i < order.size(); i++)
    {
        const APARCHIVE_ENTRY_INFO &before = reader->entries[order[i - 1]];
        const APARCHIVE_ENTRY_INFO &after = reader->entries[order[i]];
        uint64_t before_end = before.local_header_offset + LOCAL_HEADER_SIZE + before.name_size + before.compressed_size;
        if (before_end > after.local_header_offset)
            return fail(error, APRESULT_APARCHIVE_MALFORMED, "'%s' and '%s' overlap", before.name, after.name);
    }
    if (desc.strict & APARCHIVE_STRICT_NO_GAPS)
        return check_no_gaps(reader, directory_size, directory_limit, records_gap, error);
    return APRESULT_OK;
}

// Where entry [index]'s stored bytes start, after checking its local header
// against the central directory; and whether that header has a ZIP64
// record, which makes its data descriptor's sizes 8 bytes. May throw
// std::bad_alloc.
static APRESULT locate_data(aparchive_reader reader, uint32_t index, uint64_t *out_offset, bool *out_local_zip64,
                            APARCHIVE_ERROR *error)
{
    const APARCHIVE_ENTRY_INFO &info = reader->entries[index];
    uint8_t header[LOCAL_HEADER_SIZE];
    APRESULT result = read_source(reader->source, info.local_header_offset, header, sizeof(header), error, info.name);
    if (result != APRESULT_OK)
        return result;
    if (get32(header) != SIG_LOCAL)
        return fail(error, APRESULT_APARCHIVE_MALFORMED, "'%s' has no local header where the central directory says",
                    info.name);
    uint16_t flags = get16(header + 6);
    uint16_t method = get16(header + 8);
    uint32_t crc = get32(header + 14);
    uint64_t compressed_size = get32(header + 18);
    uint64_t uncompressed_size = get32(header + 22);
    size_t name_size = get16(header + 26);
    size_t extra_size = get16(header + 28);
    if ((flags & FLAGS_ENCRYPTION) || method != info.method || name_size != info.name_size)
        return fail(error, APRESULT_APARCHIVE_MALFORMED, "'%s': the local header disagrees with the central directory",
                    info.name);

    // The name and the extra field (the name is never empty).
    std::vector<uint8_t> rest(name_size + extra_size);
    result = read_source(reader->source, info.local_header_offset + LOCAL_HEADER_SIZE, rest.data(), rest.size(), error,
                         info.name);
    if (result != APRESULT_OK)
        return result;
    if (memcmp(rest.data(), info.name, name_size) != 0)
        return fail(error, APRESULT_APARCHIVE_MALFORMED, "'%s': the local header has another name", info.name);
    const uint8_t *zip64;
    size_t zip64_size;
    bool has_zip64 = find_extra(rest.data() + name_size, extra_size, EXTRA_ZIP64, &zip64, &zip64_size);

    // With a data descriptor the local header was written before the sizes
    // and CRC-32 were known, and has nothing to compare.
    if (!(flags & FLAG_DATA_DESCRIPTOR))
    {
        bool uncompressed64 = uncompressed_size == UINT32_MAX;
        bool compressed64 = compressed_size == UINT32_MAX;
        if ((uncompressed64 || compressed64) && has_zip64)
        {
            // A local ZIP64 record should hold both sizes, uncompressed
            // first; some writers store only the one that didn't fit.
            if (zip64_size >= 16)
            {
                if (uncompressed64)
                    uncompressed_size = get64(zip64);
                if (compressed64)
                    compressed_size = get64(zip64 + 8);
            }
            else if (zip64_size >= 8 && uncompressed64 != compressed64)
                (uncompressed64 ? uncompressed_size : compressed_size) = get64(zip64);
            else
                return fail(error, APRESULT_APARCHIVE_MALFORMED, "'%s' has a ZIP64 record that's too short", info.name);
        }
        if (crc != info.crc32 || compressed_size != info.compressed_size ||
            uncompressed_size != info.uncompressed_size)
            return fail(error, APRESULT_APARCHIVE_MALFORMED,
                        "'%s': the local header's sizes or CRC-32 disagree with the central directory", info.name);
    }

    uint64_t data_offset = info.local_header_offset + LOCAL_HEADER_SIZE + name_size + extra_size;
    if (data_offset > reader->directory_offset || info.compressed_size > reader->directory_offset - data_offset)
        return fail(error, APRESULT_APARCHIVE_MALFORMED, "'%s' runs into the central directory", info.name);
    *out_offset = data_offset;
    if (out_local_zip64)
        *out_local_zip64 = has_zip64;
    return APRESULT_OK;
}

// May throw std::bad_alloc.
static APRESULT read_entry(aparchive_reader reader, uint32_t index, void *buffer, APARCHIVE_ERROR *error)
{
    const APARCHIVE_ENTRY_INFO &info = reader->entries[index];
    uint64_t data_offset;
    APRESULT result = locate_data(reader, index, &data_offset, nullptr, error);
    if (result != APRESULT_OK)
        return result;
    if (info.method == APARCHIVE_METHOD_STORED)
    {
        result = read_source(reader->source, data_offset, buffer, info.uncompressed_size, error, info.name);
        if (result != APRESULT_OK)
            return result;
    }
    else
    {
        const APARCHIVE_CODEC *codec = reader->codecs->find(info.method);
        if (!codec)
            return fail(error, APRESULT_APARCHIVE_UNKNOWN_METHOD, "'%s' uses method %u, which isn't in the codec table",
                        info.name, info.method);
        // No bigger than the archive itself: the directory was checked to
        // fit on open.
        if (info.compressed_size > (uint64_t)SIZE_MAX)
            return fail(error, APRESULT_OUT_OF_MEMORY, "'%s' is too big to decompress in memory", info.name);
        std::vector<uint8_t> compressed((size_t)info.compressed_size);
        result = read_source(reader->source, data_offset, compressed.data(), compressed.size(), error, info.name);
        if (result != APRESULT_OK)
            return result;
        if (!codec->decompress(codec->user, compressed.data(), compressed.size(), buffer, info.uncompressed_size))
            return fail(error, APRESULT_CORRUPT_DATA, "'%s' doesn't decompress to its declared %llu bytes", info.name,
                        (unsigned long long)info.uncompressed_size);
    }
    if (aparchive_crc32(0, buffer, info.uncompressed_size) != info.crc32)
        return fail(error, APRESULT_CORRUPT_DATA, "'%s' fails its CRC-32", info.name);
    return APRESULT_OK;
}

// ---- Entries read in pieces -----------------------------------------------------

// How much stored data an entry reader reads from the source at once.
static const size_t ENTRY_READ_SIZE = 64 * 1024;

static APRESULT entry_fail(aparchive_entry_reader entry, APARCHIVE_ERROR *error, APRESULT result, const char *what)
{
    entry->failed = true;
    return fail(error, result, "'%s' %s", entry->reader->entries[entry->index].name, what);
}

// Runs the codec until [out] has [want] bytes or the data ends; [want] 0
// with [check_end] runs it to its data's end, expecting nothing more.
static APRESULT entry_decompress(aparchive_entry_reader entry, uint8_t *out, uint64_t want, uint64_t *out_made,
                                 APARCHIVE_ERROR *error)
{
    const APARCHIVE_ENTRY_INFO &info = entry->reader->entries[entry->index];
    const APARCHIVE_CODEC *codec = entry->codec;
    uint64_t made = 0;
    // Past the end, one byte of room: anything that comes out is too much.
    uint8_t extra_byte;
    bool to_end = want == 0;
    while (to_end ? !entry->stream_done : made < want)
    {
        if (entry->in_at == entry->in_end && entry->in_read < info.compressed_size)
        {
            uint64_t left = info.compressed_size - entry->in_read;
            size_t count = left < ENTRY_READ_SIZE ? (size_t)left : ENTRY_READ_SIZE;
            APRESULT result = read_source(entry->reader->source, entry->data_offset + entry->in_read, entry->in.data(),
                                          count, error, info.name);
            if (result != APRESULT_OK)
            {
                entry->failed = true;
                return result;
            }
            entry->in_read += count;
            entry->in_at = 0;
            entry->in_end = count;
        }
        if (entry->stream_done)
            return entry_fail(entry, error, APRESULT_CORRUPT_DATA, "decompresses to fewer bytes than declared");
        bool finish = entry->in_read == info.compressed_size;
        uint64_t used = 0, got = 0;
        uint8_t *to = to_end ? &extra_byte : out + made;
        uint64_t room = to_end ? 1 : want - made;
        bool done = false;
        if (!codec->stream_process(codec->user, entry->state, entry->in.data() + entry->in_at,
                                   entry->in_end - entry->in_at, &used, to, room, &got, finish, &done) ||
            used > entry->in_end - entry->in_at || got > room)
            return entry_fail(entry, error, APRESULT_CORRUPT_DATA, "doesn't decompress");
        entry->in_at += (size_t)used;
        entry->stream_done = done;
        if (to_end && got != 0)
            return entry_fail(entry, error, APRESULT_CORRUPT_DATA, "decompresses past its declared size");
        made += got;
        if (used == 0 && got == 0 && !done && (finish || entry->in_at < entry->in_end))
            return entry_fail(entry, error, APRESULT_CORRUPT_DATA, "ends before its data does");
    }
    *out_made = made;
    return APRESULT_OK;
}

static APRESULT entry_read(aparchive_entry_reader entry, void *buffer, uint64_t capacity, uint64_t *out_size,
                           APARCHIVE_ERROR *error)
{
    const APARCHIVE_ENTRY_INFO &info = entry->reader->entries[entry->index];
    *out_size = 0;
    if (entry->finished)
        return APRESULT_OK;
    uint64_t left = info.uncompressed_size - entry->out_total;
    uint64_t want = capacity < left ? capacity : left;
    uint64_t made = 0;
    if (!entry->codec)
    {
        APRESULT result = read_source(entry->reader->source, entry->data_offset + entry->out_total, buffer, want, error,
                                      info.name);
        if (result != APRESULT_OK)
        {
            entry->failed = true;
            return result;
        }
        made = want;
    }
    else if (want != 0)
    {
        APRESULT result = entry_decompress(entry, (uint8_t *)buffer, want, &made, error);
        if (result != APRESULT_OK)
            return result;
    }
    entry->crc = aparchive_crc32(entry->crc, buffer, made);
    entry->out_total += made;
    if (entry->out_total == info.uncompressed_size)
    {
        if (entry->codec)
        {
            uint64_t none = 0;
            APRESULT result = entry_decompress(entry, nullptr, 0, &none, error);
            if (result != APRESULT_OK)
                return result;
        }
        if (entry->crc != info.crc32)
            return entry_fail(entry, error, APRESULT_CORRUPT_DATA, "fails its CRC-32");
        entry->finished = true;
    }
    *out_size = made;
    return APRESULT_OK;
}

extern "C"
{
    // ---- Codecs -------------------------------------------------------------

    APRESULT aparchive_codecs_create(aparchive_codecs *out_codecs)
    {
        if (!out_codecs)
            return APRESULT_INVALID_ARGUMENT;
        *out_codecs = new (std::nothrow) aparchive_codecs_t();
        return *out_codecs ? APRESULT_OK : APRESULT_OUT_OF_MEMORY;
    }

    APRESULT aparchive_codecs_add(aparchive_codecs codecs, const APARCHIVE_CODEC *codec)
    {
        if (!codecs)
            return APRESULT_INVALID_ARGUMENT;
        APRI_ENTER(codecs->overlap, "aparchive", "aparchive_codecs_add", "codec table");
        if (!codec)
            return APRESULT_INVALID_ARGUMENT;
        if (APRESULT result = apri_struct_check(&codec->base, APRI_STRUCT_TYPE_APARCHIVE_CODEC))
            return result;
        const APARCHIVE_CODEC &c = *codec;
        if (c.method == APARCHIVE_METHOD_STORED || !c.decompress)
            return APRESULT_INVALID_ARGUMENT;
        if ((c.compress == nullptr) != (c.compress_bound == nullptr))
            return APRESULT_INVALID_ARGUMENT;
        if ((c.stream_begin == nullptr) != (c.stream_process == nullptr))
            return APRESULT_INVALID_ARGUMENT;
        if ((c.stream_begin == nullptr) != (c.stream_end == nullptr))
            return APRESULT_INVALID_ARGUMENT;
        if (codecs->find(c.method))
            return APRESULT_INVALID_ARGUMENT;
        try
        {
            codecs->codecs.push_back(c);
        }
        catch (const std::bad_alloc &)
        {
            return APRESULT_OUT_OF_MEMORY;
        }
        return APRESULT_OK;
    }

    APRESULT aparchive_codecs_add_deflate(aparchive_codecs codecs)
    {
        APARCHIVE_CODEC codec{};
        codec.base = apri_struct_base(APRI_STRUCT_TYPE_APARCHIVE_CODEC);
        codec.method = APARCHIVE_METHOD_DEFLATE;
        codec.compress_bound = deflate_compress_bound;
        codec.compress = deflate_compress;
        codec.decompress = deflate_decompress;
        codec.stream_begin = deflate_stream_begin;
        codec.stream_process = deflate_stream_process;
        codec.stream_end = deflate_stream_end;
        return aparchive_codecs_add(codecs, &codec);
    }

    bool aparchive_codecs_has(aparchive_codecs codecs, uint16_t method)
    {
        if (!codecs)
            return false;
        APRI_ENTER(codecs->overlap, "aparchive", "aparchive_codecs_has", "codec table");
        return method == APARCHIVE_METHOD_STORED || codecs->find(method) != nullptr;
    }

    void aparchive_codecs_destroy(aparchive_codecs codecs)
    {
        delete codecs;
    }

    APRESULT aparchive_codecs_compress_bound(aparchive_codecs codecs, uint16_t method, uint64_t in_size,
                                             uint64_t *out_bound)
    {
        if (!codecs)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_bound)
            return APRESULT_INVALID_ARGUMENT;
        if (method == APARCHIVE_METHOD_STORED)
        {
            *out_bound = in_size;
            return APRESULT_OK;
        }
        const APARCHIVE_CODEC *codec = codecs->find(method);
        if (!codec)
            return APRESULT_APARCHIVE_UNKNOWN_METHOD;
        if (!codec->compress_bound)
            return APRESULT_UNSUPPORTED;
        *out_bound = codec->compress_bound(codec->user, in_size);
        return APRESULT_OK;
    }

    APRESULT aparchive_codecs_compress(aparchive_codecs codecs, uint16_t method, int32_t level, const void *in,
                                       uint64_t in_size, void *out, uint64_t out_capacity, uint64_t *out_size,
                                       APARCHIVE_ERROR *out_error)
    {
        clear_error(out_error);
        if (!codecs)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "codecs is null");
        if (!in && in_size != 0)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "in is null but in_size is %llu",
                        (unsigned long long)in_size);
        if (!out && out_capacity != 0)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "out is null but out_capacity is %llu",
                        (unsigned long long)out_capacity);
        if (!out_size)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "out_size is null");
        uint64_t bound = 0;
        APRESULT result = aparchive_codecs_compress_bound(codecs, method, in_size, &bound);
        if (result != APRESULT_OK)
            return fail(out_error, result, "method %u can't compress", method);
        if (out_capacity < bound)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "out_capacity %llu is under the bound, %llu",
                        (unsigned long long)out_capacity, (unsigned long long)bound);
        if (method == APARCHIVE_METHOD_STORED)
        {
            if (in_size)
                memcpy(out, in, (size_t)in_size);
            *out_size = in_size;
            return APRESULT_OK;
        }
        const APARCHIVE_CODEC *codec = codecs->find(method);
        uint64_t size = 0;
        // A buffer of 0 bytes is still somewhere to write, as add_entry's is.
        uint8_t nothing;
        if (!codec->compress(codec->user, level, in, in_size, out ? out : &nothing, bound, &size) || size > bound)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "method %u failed to compress at level %d", method,
                        level);
        *out_size = size;
        return APRESULT_OK;
    }

    // ---- Writing ------------------------------------------------------------

    APRESULT aparchive_writer_create(const APARCHIVE_WRITER_DESC *desc, aparchive_writer *out_writer)
    {
        if (!out_writer)
            return APRESULT_INVALID_ARGUMENT;
        *out_writer = nullptr;
        if (!desc)
            return APRESULT_INVALID_ARGUMENT;
        if (APRESULT result = apri_struct_check(&desc->base, APRI_STRUCT_TYPE_APARCHIVE_WRITER_DESC))
            return result;
        const APARCHIVE_WRITER_DESC &d = *desc;
        if (!d.sink.write)
            return APRESULT_INVALID_ARGUMENT;
        if (!d.codecs)
            return APRESULT_INVALID_ARGUMENT;
        aparchive_writer_t *writer = (aparchive_writer_t *)malloc(sizeof(aparchive_writer_t));
        if (!writer)
            return APRESULT_OUT_OF_MEMORY;
        writer = new (writer) aparchive_writer_t();
        writer->sink = d.sink;
        writer->codecs = d.codecs;
        writer->allow_directory_entries = d.allow_directory_entries;
        *out_writer = writer;
        return APRESULT_OK;
    }

    APRESULT aparchive_writer_add_entry(aparchive_writer writer, const APARCHIVE_ENTRY_DESC *desc, const void *data,
                                        uint64_t size, APARCHIVE_ERROR *out_error)
    {
        clear_error(out_error);
        if (!writer)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "writer is null");
        APRI_ENTER(writer->overlap, "aparchive", "aparchive_writer_add_entry", "writer");
        if (!desc)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is null");
        if (APRESULT result = apri_struct_check(&desc->base, APRI_STRUCT_TYPE_APARCHIVE_ENTRY_DESC))
            return fail(out_error, result, "desc %s", apri_struct_check_message(result));
        const APARCHIVE_ENTRY_DESC &d = *desc;
        if (!data && size != 0)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "data is null but size is %llu", (unsigned long long)size);
        try
        {
            return add_entry(writer, d, data, size, out_error);
        }
        catch (const std::bad_alloc &)
        {
            // The entry may be half recorded.
            writer->failed = true;
            return fail(out_error, APRESULT_OUT_OF_MEMORY, "out of memory adding the entry");
        }
    }

    APRESULT aparchive_writer_add_raw_entry(aparchive_writer writer, const APARCHIVE_ENTRY_DESC *desc,
                                            const void *stored, uint64_t stored_size, uint64_t uncompressed_size,
                                            uint32_t crc32, APARCHIVE_ERROR *out_error)
    {
        clear_error(out_error);
        if (!writer)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "writer is null");
        APRI_ENTER(writer->overlap, "aparchive", "aparchive_writer_add_raw_entry", "writer");
        if (!desc)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is null");
        if (APRESULT result = apri_struct_check(&desc->base, APRI_STRUCT_TYPE_APARCHIVE_ENTRY_DESC))
            return fail(out_error, result, "desc %s", apri_struct_check_message(result));
        if (!stored && stored_size != 0)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "stored is null but stored_size is %llu",
                        (unsigned long long)stored_size);
        try
        {
            return add_raw_entry(writer, *desc, stored, stored_size, uncompressed_size, crc32, out_error);
        }
        catch (const std::bad_alloc &)
        {
            writer->failed = true;
            return fail(out_error, APRESULT_OUT_OF_MEMORY, "out of memory adding the entry");
        }
    }

    APRESULT aparchive_writer_begin_entry(aparchive_writer writer, const APARCHIVE_ENTRY_DESC *desc,
                                          APARCHIVE_ERROR *out_error)
    {
        clear_error(out_error);
        if (!writer)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "writer is null");
        APRI_ENTER(writer->overlap, "aparchive", "aparchive_writer_begin_entry", "writer");
        if (!desc)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is null");
        if (APRESULT result = apri_struct_check(&desc->base, APRI_STRUCT_TYPE_APARCHIVE_ENTRY_DESC))
            return fail(out_error, result, "desc %s", apri_struct_check_message(result));
        try
        {
            return begin_entry(writer, *desc, out_error);
        }
        catch (const std::bad_alloc &)
        {
            writer->failed = true;
            return fail(out_error, APRESULT_OUT_OF_MEMORY, "out of memory starting the entry");
        }
    }

    APRESULT aparchive_writer_write_entry_data(aparchive_writer writer, const void *data, uint64_t size,
                                               APARCHIVE_ERROR *out_error)
    {
        clear_error(out_error);
        if (!writer)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "writer is null");
        APRI_ENTER(writer->overlap, "aparchive", "aparchive_writer_write_entry_data", "writer");
        if (!data && size != 0)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "data is null but size is %llu", (unsigned long long)size);
        if (writer->failed)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "the writer is unusable after an earlier failure");
        if (!writer->streaming)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "no streamed entry is open");
        return write_entry_data(writer, data, size, out_error);
    }

    APRESULT aparchive_writer_end_entry(aparchive_writer writer, APARCHIVE_ERROR *out_error)
    {
        clear_error(out_error);
        if (!writer)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "writer is null");
        APRI_ENTER(writer->overlap, "aparchive", "aparchive_writer_end_entry", "writer");
        if (writer->failed)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "the writer is unusable after an earlier failure");
        if (!writer->streaming)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "no streamed entry is open");
        try
        {
            return end_entry(writer, out_error);
        }
        catch (const std::bad_alloc &)
        {
            writer->failed = true;
            return fail(out_error, APRESULT_OUT_OF_MEMORY, "out of memory ending the entry");
        }
    }

    APRESULT aparchive_writer_set_comment(aparchive_writer writer, const void *comment, uint16_t comment_size,
                                          APARCHIVE_ERROR *out_error)
    {
        clear_error(out_error);
        if (!writer)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "writer is null");
        APRI_ENTER(writer->overlap, "aparchive", "aparchive_writer_set_comment", "writer");
        if (!comment && comment_size != 0)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "comment is null but comment_size is %u", comment_size);
        if (writer->failed || writer->finished)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "the writer is %s",
                        writer->finished ? "finished" : "unusable after an earlier failure");
        if (holds_end_signature((const uint8_t *)comment, comment_size))
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "the comment holds the end record's signature");
        try
        {
            writer->comment.assign((const uint8_t *)comment, (const uint8_t *)comment + comment_size);
        }
        catch (const std::bad_alloc &)
        {
            return fail(out_error, APRESULT_OUT_OF_MEMORY, "out of memory copying the comment");
        }
        return APRESULT_OK;
    }

    APRESULT aparchive_writer_remove_entry(aparchive_writer writer, const char *name, APARCHIVE_ERROR *out_error)
    {
        clear_error(out_error);
        if (!writer)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "writer is null");
        APRI_ENTER(writer->overlap, "aparchive", "aparchive_writer_remove_entry", "writer");
        if (!name)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "name is null");
        if (writer->failed || writer->finished)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "the writer is %s",
                        writer->finished ? "finished" : "unusable after an earlier failure");
        if (writer->streaming)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "a streamed entry is open: end it first");
        for (size_t i = 0; i < writer->entries.size(); i++)
        {
            if (writer->entries[i].name != name)
                continue;
            // Erasing moves no entry's bytes: they're the sink's.
            writer->names.erase(writer->entries[i].name);
            writer->entries.erase(writer->entries.begin() + (ptrdiff_t)i);
            return APRESULT_OK;
        }
        return fail(out_error, APRESULT_NOT_FOUND, "no entry is named '%s'", name);
    }

    APRESULT aparchive_writer_create_update(const APARCHIVE_UPDATE_DESC *desc, aparchive_writer *out_writer,
                                            APARCHIVE_ERROR *out_error)
    {
        clear_error(out_error);
        if (!out_writer)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "out_writer is null");
        *out_writer = nullptr;
        if (!desc)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is null");
        if (APRESULT result = apri_struct_check(&desc->base, APRI_STRUCT_TYPE_APARCHIVE_UPDATE_DESC))
            return fail(out_error, result, "desc %s", apri_struct_check_message(result));
        const APARCHIVE_UPDATE_DESC &d = *desc;
        if (!d.reader)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's reader is null");
        if (!d.truncate)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's truncate is null");
        if (!d.sink.write)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's sink write callback is null");
        if (!d.codecs)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's codecs is null");
        aparchive_writer_t *writer = (aparchive_writer_t *)malloc(sizeof(aparchive_writer_t));
        if (!writer)
            return fail(out_error, APRESULT_OUT_OF_MEMORY, "out of memory creating the writer");
        writer = new (writer) aparchive_writer_t();
        writer->sink = d.sink;
        writer->codecs = d.codecs;
        writer->allow_directory_entries = d.allow_directory_entries;
        writer->truncate = d.truncate;
        writer->truncate_pending = true;
        try
        {
            copy_entries(writer, d.reader);
        }
        catch (const std::bad_alloc &)
        {
            writer->~aparchive_writer_t();
            free(writer);
            return fail(out_error, APRESULT_OUT_OF_MEMORY, "out of memory copying the archive's entries");
        }
        *out_writer = writer;
        return APRESULT_OK;
    }

    uint64_t aparchive_writer_get_offset(aparchive_writer writer)
    {
        return writer ? writer->offset : 0;
    }

    APRESULT aparchive_writer_finish(aparchive_writer writer, APARCHIVE_ERROR *out_error)
    {
        clear_error(out_error);
        if (!writer)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "writer is null");
        APRI_ENTER(writer->overlap, "aparchive", "aparchive_writer_finish", "writer");
        if (writer->failed || writer->finished)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "the writer is %s",
                        writer->finished ? "finished" : "unusable after an earlier failure");
        if (writer->streaming)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "a streamed entry is open: end it first");
        try
        {
            return finish(writer, out_error);
        }
        catch (const std::bad_alloc &)
        {
            writer->failed = true;
            return fail(out_error, APRESULT_OUT_OF_MEMORY, "out of memory writing the central directory");
        }
    }

    void aparchive_writer_destroy(aparchive_writer writer)
    {
        if (!writer)
            return;
        writer->~aparchive_writer_t();
        free(writer);
    }

    // ---- Reading ------------------------------------------------------------

    APRESULT aparchive_reader_open(const APARCHIVE_READER_DESC *desc, aparchive_reader *out_reader,
                                   APARCHIVE_ERROR *out_error)
    {
        clear_error(out_error);
        if (!out_reader)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "out_reader is null");
        *out_reader = nullptr;
        if (!desc)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is null");
        if (APRESULT result = apri_struct_check(&desc->base, APRI_STRUCT_TYPE_APARCHIVE_READER_DESC))
            return fail(out_error, result, "desc %s", apri_struct_check_message(result));
        const APARCHIVE_READER_DESC &d = *desc;
        if (!d.source.read || !d.codecs)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's source read callback or codecs is null");
        const uint32_t strict_known = APARCHIVE_STRICT_NO_ARCHIVE_COMMENT | APARCHIVE_STRICT_NO_ENTRY_COMMENTS |
                                      APARCHIVE_STRICT_NO_DIRECTORY_ENTRIES | APARCHIVE_STRICT_NO_GAPS;
        if ((uint32_t)d.strict & ~strict_known)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's strict has unknown bits 0x%X",
                        (unsigned)((uint32_t)d.strict & ~strict_known));
        aparchive_reader_t *reader = new (std::nothrow) aparchive_reader_t();
        if (!reader)
            return fail(out_error, APRESULT_OUT_OF_MEMORY, "out of memory opening the archive");
        APRESULT result;
        try
        {
            result = open_reader(d, reader, out_error);
        }
        catch (const std::bad_alloc &)
        {
            result = fail(out_error, APRESULT_OUT_OF_MEMORY, "out of memory reading the central directory");
        }
        if (result != APRESULT_OK)
        {
            delete reader;
            return result;
        }
        *out_reader = reader;
        return APRESULT_OK;
    }

    uint32_t aparchive_reader_get_entry_count(aparchive_reader reader)
    {
        return reader ? (uint32_t)reader->entries.size() : 0;
    }

    APRESULT aparchive_reader_get_entry(aparchive_reader reader, uint32_t index, APARCHIVE_ENTRY_INFO *out_info)
    {
        if (!reader)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_info)
            return APRESULT_INVALID_ARGUMENT;
        if (APRESULT result = apri_struct_out_check(&out_info->base, APRI_STRUCT_TYPE_APARCHIVE_ENTRY_INFO))
            return result;
        if (index >= reader->entries.size())
            return APRESULT_NOT_FOUND;
        APRI_STRUCT_BASE_OUT base = out_info->base;
        *out_info = reader->entries[index];
        out_info->base = base;
        return APRESULT_OK;
    }

    APRESULT aparchive_reader_find(aparchive_reader reader, const char *name, uint32_t *out_index)
    {
        if (!reader || !name || !out_index)
            return APRESULT_INVALID_ARGUMENT;
        auto it = reader->name_index.find(std::string_view(name));
        if (it == reader->name_index.end())
            return APRESULT_NOT_FOUND;
        *out_index = it->second;
        return APRESULT_OK;
    }

    APRESULT aparchive_reader_get_data_offset(aparchive_reader reader, uint32_t index, uint64_t *out_offset,
                                              APARCHIVE_ERROR *out_error)
    {
        clear_error(out_error);
        if (!reader || !out_offset)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "reader or out_offset is null");
        if (index >= reader->entries.size())
            return fail(out_error, APRESULT_NOT_FOUND, "no entry %u (the archive has %u)", index,
                        (uint32_t)reader->entries.size());
        try
        {
            return locate_data(reader, index, out_offset, nullptr, out_error);
        }
        catch (const std::bad_alloc &)
        {
            return fail(out_error, APRESULT_OUT_OF_MEMORY, "out of memory reading the entry's local header");
        }
    }

    APRESULT aparchive_reader_read_entry(aparchive_reader reader, uint32_t index, void *buffer, uint64_t buffer_size,
                                         APARCHIVE_ERROR *out_error)
    {
        clear_error(out_error);
        if (!reader || (!buffer && buffer_size != 0))
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "reader is null, or buffer is null with a size");
        if (index >= reader->entries.size())
            return fail(out_error, APRESULT_NOT_FOUND, "no entry %u (the archive has %u)", index,
                        (uint32_t)reader->entries.size());
        const APARCHIVE_ENTRY_INFO &info = reader->entries[index];
        if (buffer_size != info.uncompressed_size)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "'%s' is %llu bytes; buffer_size is %llu", info.name,
                        (unsigned long long)info.uncompressed_size, (unsigned long long)buffer_size);
        try
        {
            return read_entry(reader, index, buffer, out_error);
        }
        catch (const std::bad_alloc &)
        {
            return fail(out_error, APRESULT_OUT_OF_MEMORY, "out of memory reading '%s'", info.name);
        }
    }

    void aparchive_reader_destroy(aparchive_reader reader)
    {
        delete reader;
    }

    APRESULT aparchive_reader_get_comment(aparchive_reader reader, const void **out_comment, uint16_t *out_size)
    {
        if (!reader)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_comment)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_size)
            return APRESULT_INVALID_ARGUMENT;
        *out_comment = reader->comment.empty() ? nullptr : reader->comment.data();
        *out_size = (uint16_t)reader->comment.size();
        return APRESULT_OK;
    }

    uint64_t aparchive_reader_get_offset_shift(aparchive_reader reader)
    {
        return reader ? reader->shift : 0;
    }

    APRESULT aparchive_reader_open_entry(aparchive_reader reader, uint32_t index,
                                         aparchive_entry_reader *out_entry_reader, APARCHIVE_ERROR *out_error)
    {
        clear_error(out_error);
        if (!out_entry_reader)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "out_entry_reader is null");
        *out_entry_reader = nullptr;
        if (!reader)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "reader is null");
        if (index >= reader->entries.size())
            return fail(out_error, APRESULT_NOT_FOUND, "no entry %u (the archive has %u)", index,
                        (uint32_t)reader->entries.size());
        const APARCHIVE_ENTRY_INFO &info = reader->entries[index];
        const APARCHIVE_CODEC *codec = nullptr;
        if (info.method != APARCHIVE_METHOD_STORED)
        {
            codec = reader->codecs->find(info.method);
            if (!codec)
                return fail(out_error, APRESULT_APARCHIVE_UNKNOWN_METHOD,
                            "'%s' uses method %u, which isn't in the codec table", info.name, info.method);
            if (!codec->stream_begin)
                return fail(out_error, APRESULT_UNSUPPORTED, "method %u's codec reads whole entries only",
                            info.method);
        }
        aparchive_entry_reader_t *entry = (aparchive_entry_reader_t *)malloc(sizeof(aparchive_entry_reader_t));
        if (!entry)
            return fail(out_error, APRESULT_OUT_OF_MEMORY, "out of memory opening '%s'", info.name);
        entry = new (entry) aparchive_entry_reader_t();
        entry->reader = reader;
        entry->index = index;
        APRESULT result;
        try
        {
            result = locate_data(reader, index, &entry->data_offset, nullptr, out_error);
            if (result == APRESULT_OK && codec)
                entry->in.resize(ENTRY_READ_SIZE);
        }
        catch (const std::bad_alloc &)
        {
            result = fail(out_error, APRESULT_OUT_OF_MEMORY, "out of memory opening '%s'", info.name);
        }
        if (result == APRESULT_OK && codec)
        {
            entry->state = codec->stream_begin(codec->user, false, 0);
            entry->codec = codec;
            if (!entry->state)
                result = fail(out_error, APRESULT_OUT_OF_MEMORY, "method %u couldn't start reading '%s'",
                              info.method, info.name);
        }
        if (result != APRESULT_OK)
        {
            aparchive_entry_reader_destroy(entry);
            return result;
        }
        *out_entry_reader = entry;
        return APRESULT_OK;
    }

    APRESULT aparchive_entry_reader_read(aparchive_entry_reader entry_reader, void *buffer, uint64_t capacity,
                                         uint64_t *out_size, APARCHIVE_ERROR *out_error)
    {
        clear_error(out_error);
        if (!entry_reader)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "entry_reader is null");
        APRI_ENTER(entry_reader->overlap, "aparchive", "aparchive_entry_reader_read", "entry reader");
        if (!buffer && capacity != 0)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "buffer is null but capacity is %llu",
                        (unsigned long long)capacity);
        if (!out_size)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "out_size is null");
        *out_size = 0;
        if (entry_reader->failed)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "an earlier read of the entry failed");
        return entry_read(entry_reader, buffer, capacity, out_size, out_error);
    }

    void aparchive_entry_reader_destroy(aparchive_entry_reader entry_reader)
    {
        if (!entry_reader)
            return;
        if (entry_reader->codec && entry_reader->state)
            entry_reader->codec->stream_end(entry_reader->codec->user, entry_reader->state);
        entry_reader->~aparchive_entry_reader_t();
        free(entry_reader);
    }

    // ---- Extra fields ---------------------------------------------------------

    APRESULT aparchive_extra_find(const void *extra, uint16_t extra_size, uint16_t id, const void **out_data,
                                  uint16_t *out_size)
    {
        if (!extra && extra_size != 0)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_data)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_size)
            return APRESULT_INVALID_ARGUMENT;
        const uint8_t *data = nullptr;
        size_t size = 0;
        APRESULT result = extra_find((const uint8_t *)extra, extra_size, id, &data, &size);
        if (result != APRESULT_OK)
            return result;
        *out_data = data;
        *out_size = (uint16_t)size;
        return APRESULT_OK;
    }

    APRESULT aparchive_extra_get_unix_times(const void *extra, uint16_t extra_size, APARCHIVE_FILE_TIMES *out_times)
    {
        if (!extra && extra_size != 0)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_times)
            return APRESULT_INVALID_ARGUMENT;
        const uint8_t *data = nullptr;
        size_t size = 0;
        APRESULT result = extra_find((const uint8_t *)extra, extra_size, APARCHIVE_EXTRA_UNIX_TIMES, &data, &size);
        if (result != APRESULT_OK)
            return result;
        if (size < 1)
            return APRESULT_APARCHIVE_MALFORMED;
        // The flags say which times the local record holds, in this order;
        // a central one stops after the first. Times are signed seconds.
        APARCHIVE_FILE_TIMES times{};
        uint8_t flags = data[0];
        size_t at = 1;
        int64_t *values[3] = {&times.modified, &times.accessed, &times.created};
        bool *known[3] = {&times.has_modified, &times.has_accessed, &times.has_created};
        for (int i = 0; i < 3; i++)
        {
            if (!(flags & (1 << i)) || size - at < 4)
                continue;
            *values[i] = (int64_t)(int32_t)get32(data + at) * TICKS_PER_SECOND;
            *known[i] = true;
            at += 4;
        }
        *out_times = times;
        return APRESULT_OK;
    }

    APRESULT aparchive_extra_put_unix_times(const APARCHIVE_FILE_TIMES *times, bool central, void *out,
                                            uint16_t out_capacity, uint16_t *out_size)
    {
        if (!times)
            return APRESULT_INVALID_ARGUMENT;
        if (!out)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_size)
            return APRESULT_INVALID_ARGUMENT;
        const int64_t values[3] = {times->modified, times->accessed, times->created};
        const bool known[3] = {times->has_modified, times->has_accessed, times->has_created};
        uint8_t flags = 0;
        int32_t seconds[3] = {0, 0, 0};
        for (int i = 0; i < 3; i++)
        {
            if (!known[i])
                continue;
            int64_t value = floor_seconds(values[i]);
            if (value < INT32_MIN || value > INT32_MAX)
                return APRESULT_INVALID_ARGUMENT;
            seconds[i] = (int32_t)value;
            flags = (uint8_t)(flags | 1 << i);
        }
        if (flags == 0)
            return APRESULT_INVALID_ARGUMENT;
        // A central record holds the modified time alone.
        uint16_t data_size = 1;
        for (int i = 0; i < 3; i++)
            if ((flags & (1 << i)) && (!central || i == 0))
                data_size += 4;
        *out_size = (uint16_t)(4 + data_size);
        if (out_capacity < *out_size)
            return APRESULT_INVALID_ARGUMENT;
        uint8_t *bytes = (uint8_t *)out;
        put_u16(bytes, APARCHIVE_EXTRA_UNIX_TIMES);
        put_u16(bytes + 2, data_size);
        bytes[4] = flags;
        size_t at = 5;
        for (int i = 0; i < 3; i++)
        {
            if (!(flags & (1 << i)) || (central && i != 0))
                continue;
            put_u32(bytes + at, (uint32_t)seconds[i]);
            at += 4;
        }
        return APRESULT_OK;
    }

    APRESULT aparchive_extra_get_ntfs_times(const void *extra, uint16_t extra_size, APARCHIVE_FILE_TIMES *out_times)
    {
        if (!extra && extra_size != 0)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_times)
            return APRESULT_INVALID_ARGUMENT;
        const uint8_t *data = nullptr;
        size_t size = 0;
        APRESULT result = extra_find((const uint8_t *)extra, extra_size, APARCHIVE_EXTRA_NTFS_TIMES, &data, &size);
        if (result != APRESULT_OK)
            return result;
        // 4 reserved bytes, then attributes: a tag, a size, its data.
        if (size < 4)
            return APRESULT_APARCHIVE_MALFORMED;
        const uint8_t *attributes = nullptr;
        size_t attributes_size = 0;
        result = extra_find(data + 4, size - 4, 0x0001, &attributes, &attributes_size);
        if (result != APRESULT_OK)
            return result;
        if (attributes_size < 24)
            return APRESULT_APARCHIVE_MALFORMED;
        APARCHIVE_FILE_TIMES times{};
        int64_t *values[3] = {&times.modified, &times.accessed, &times.created};
        for (int i = 0; i < 3; i++)
        {
            uint64_t ticks = get64(attributes + 8 * i);
            // What 64-bit ticks since 1970 hold, either way.
            if (ticks >= NTFS_TO_UNIX_TICKS ? ticks - NTFS_TO_UNIX_TICKS > (uint64_t)INT64_MAX : false)
                return APRESULT_APARCHIVE_MALFORMED;
            *values[i] = ticks >= NTFS_TO_UNIX_TICKS ? (int64_t)(ticks - NTFS_TO_UNIX_TICKS)
                                                    : -(int64_t)(NTFS_TO_UNIX_TICKS - ticks);
        }
        times.has_modified = true;
        times.has_accessed = true;
        times.has_created = true;
        *out_times = times;
        return APRESULT_OK;
    }

    APRESULT aparchive_extra_put_ntfs_times(const APARCHIVE_FILE_TIMES *times, void *out, uint16_t out_capacity,
                                            uint16_t *out_size)
    {
        if (!times)
            return APRESULT_INVALID_ARGUMENT;
        if (!out)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_size)
            return APRESULT_INVALID_ARGUMENT;
        if (!times->has_modified)
            return APRESULT_INVALID_ARGUMENT;
        if (!times->has_accessed)
            return APRESULT_INVALID_ARGUMENT;
        if (!times->has_created)
            return APRESULT_INVALID_ARGUMENT;
        const int64_t values[3] = {times->modified, times->accessed, times->created};
        uint64_t ticks[3];
        for (int i = 0; i < 3; i++)
        {
            if (values[i] < -(int64_t)NTFS_TO_UNIX_TICKS)
                return APRESULT_INVALID_ARGUMENT;
            ticks[i] = NTFS_TO_UNIX_TICKS + (uint64_t)values[i]; // wraps back into range for negatives
        }
        *out_size = 36;
        if (out_capacity < 36)
            return APRESULT_INVALID_ARGUMENT;
        uint8_t *bytes = (uint8_t *)out;
        put_u16(bytes, APARCHIVE_EXTRA_NTFS_TIMES);
        put_u16(bytes + 2, 32);
        put_u32(bytes + 4, 0); // reserved
        put_u16(bytes + 8, 0x0001);
        put_u16(bytes + 10, 24);
        for (int i = 0; i < 3; i++)
            put_u64(bytes + 12 + 8 * i, ticks[i]);
        return APRESULT_OK;
    }

    APRESULT aparchive_extra_get_unicode_path(const void *extra, uint16_t extra_size, const char *name,
                                              uint16_t name_size, const char **out_path, uint16_t *out_size)
    {
        if (!extra && extra_size != 0)
            return APRESULT_INVALID_ARGUMENT;
        if (!name && name_size != 0)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_path)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_size)
            return APRESULT_INVALID_ARGUMENT;
        const uint8_t *data = nullptr;
        size_t size = 0;
        APRESULT result = extra_find((const uint8_t *)extra, extra_size, APARCHIVE_EXTRA_UNICODE_PATH, &data, &size);
        if (result != APRESULT_OK)
            return result;
        // A version, the CRC-32 of the name it was written beside, the name.
        if (size < 5 || data[0] != 1)
            return APRESULT_APARCHIVE_MALFORMED;
        if (get32(data + 1) != aparchive_crc32(0, name, name_size))
            return APRESULT_NOT_FOUND;
        *out_path = (const char *)data + 5;
        *out_size = (uint16_t)(size - 5);
        return APRESULT_OK;
    }

    APRESULT aparchive_extra_put_unicode_path(const char *name, uint16_t name_size, const char *path,
                                              uint16_t path_size, void *out, uint16_t out_capacity,
                                              uint16_t *out_size)
    {
        if (!name && name_size != 0)
            return APRESULT_INVALID_ARGUMENT;
        if (!path && path_size != 0)
            return APRESULT_INVALID_ARGUMENT;
        if (!out)
            return APRESULT_INVALID_ARGUMENT;
        if (!out_size)
            return APRESULT_INVALID_ARGUMENT;
        if (!is_valid_utf8(path, path_size))
            return APRESULT_INVALID_ARGUMENT;
        if (path_size > UINT16_MAX - 9)
            return APRESULT_INVALID_ARGUMENT;
        *out_size = (uint16_t)(9 + path_size);
        if (out_capacity < *out_size)
            return APRESULT_INVALID_ARGUMENT;
        uint8_t *bytes = (uint8_t *)out;
        put_u16(bytes, APARCHIVE_EXTRA_UNICODE_PATH);
        put_u16(bytes + 2, (uint16_t)(5 + path_size));
        bytes[4] = 1;
        put_u32(bytes + 5, aparchive_crc32(0, name, name_size));
        if (path_size)
            memcpy(bytes + 9, path, path_size);
        return APRESULT_OK;
    }

    // ---- Utilities ----------------------------------------------------------

    bool aparchive_is_safe_name(const char *name, uint64_t name_size)
    {
        if (!name || name_size == 0 || name_size > UINT16_MAX)
            return false;
        // A drive letter ("C:") or a leading '/' makes the name absolute.
        if (name[0] == '/' || (name_size >= 2 && name[1] == ':'))
            return false;
        // One trailing '/' marks a directory entry; judge the path before it.
        uint64_t path_size = name[name_size - 1] == '/' ? name_size - 1 : name_size;
        if (path_size == 0)
            return false;
        uint64_t segment_start = 0;
        for (uint64_t i = 0; i <= path_size; i++)
        {
            char c = i < path_size ? name[i] : '/';
            if (i < path_size && (c == '\0' || c == '\\'))
                return false;
            if (c != '/')
                continue;
            uint64_t length = i - segment_start;
            const char *segment = name + segment_start;
            // Empty segments ("a//b"), ".", and "..".
            if (length == 0 || (length == 1 && segment[0] == '.') ||
                (length == 2 && segment[0] == '.' && segment[1] == '.'))
                return false;
            segment_start = i + 1;
        }
        return true;
    }

    uint32_t aparchive_crc32(uint32_t crc, const void *data, uint64_t size)
    {
        const uint8_t *bytes = (const uint8_t *)data;
        // libdeflate_crc32 takes a size_t; feed huge inputs in pieces.
        while (size > 0)
        {
            size_t chunk = size > (uint64_t)SIZE_MAX ? SIZE_MAX : (size_t)size;
            crc = libdeflate_crc32(crc, bytes, chunk);
            bytes += chunk;
            size -= chunk;
        }
        return crc;
    }
}
