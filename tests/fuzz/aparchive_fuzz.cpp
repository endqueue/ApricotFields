// aparchive_fuzz - ApArchive's reader over an archive held in memory: the
// end records, the ZIP64 locator and end record, the central directory,
// each entry's local header and its data, STORED or DEFLATE (libdeflate).
//
// The input is the archive, all of it. Beyond not crashing, it checks:
// - a failed open leaves no reader and a terminated message;
// - every entry an open reader lists is within the desc's limits, has a
//   safe, terminated name that aparchive_reader_find() finds again, and
//   stored bytes that lie inside the archive;
// - an entry read whole is exactly its declared size, and a STORED one is
//   the archive's own bytes at its data offset;
// - every entry that reads, written again by the writer and read back, is
//   the same bytes under the same name: whole, streamed in pieces, and
//   copied raw from the archive's own stored bytes;
// - an entry read in pieces (zlib-ng) reads exactly when it reads whole
//   (libdeflate), to the same bytes;
// - a strict open refuses only what a lenient one opens; the comment, the
//   offset shift and the extra field helpers stay inside the archive;
// - updating the archive (one entry dropped, one added) gives one that
//   opens, with every kept entry as it was.

#include "aprifuzz.hpp"

#include "archive/aparchive.h"

#include <string.h>
#include <string>
#include <vector>

namespace
{
// Small enough that a fuzzed archive can't ask for much memory, large
// enough for the corpus.
constexpr uint32_t MAX_ENTRY_COUNT = 4096;
constexpr uint64_t MAX_CENTRAL_DIRECTORY_SIZE = 1u << 20;
constexpr uint64_t MAX_ENTRY_SIZE = 1u << 24;
constexpr uint64_t MAX_TOTAL_SIZE = 1u << 26;
constexpr uint32_t MAX_COMPRESSION_RATIO = 1100;

struct memory_t
{
    const uint8_t *data;
    size_t size;
};

bool memory_read(void *user, uint64_t offset, void *buffer, uint64_t count)
{
    const memory_t &memory = *(const memory_t *)user;
    APRIFUZZ_CHECK(offset <= memory.size);
    APRIFUZZ_CHECK(count <= memory.size - offset);
    memcpy(buffer, memory.data + offset, (size_t)count);
    return true;
}

bool vector_write(void *user, const void *data, uint64_t count)
{
    std::vector<uint8_t> &out = *(std::vector<uint8_t> *)user;
    const uint8_t *bytes = (const uint8_t *)data;
    out.insert(out.end(), bytes, bytes + count);
    return true;
}

bool vector_read(void *user, uint64_t offset, void *buffer, uint64_t count)
{
    const std::vector<uint8_t> &in = *(const std::vector<uint8_t> *)user;
    APRIFUZZ_CHECK(offset <= in.size());
    APRIFUZZ_CHECK(count <= in.size() - offset);
    memcpy(buffer, in.data() + offset, (size_t)count);
    return true;
}

bool message_is_terminated(const APARCHIVE_ERROR &error)
{
    return memchr(error.message, '\0', sizeof(error.message)) != nullptr;
}

APARCHIVE_READER_DESC reader_desc(aparchive_codecs codecs)
{
    APARCHIVE_READER_DESC desc{};
    desc.base.type = APRI_STRUCT_TYPE_APARCHIVE_READER_DESC;
    desc.codecs = codecs;
    desc.max_entry_count = MAX_ENTRY_COUNT;
    desc.max_central_directory_size = MAX_CENTRAL_DIRECTORY_SIZE;
    desc.max_entry_uncompressed_size = MAX_ENTRY_SIZE;
    desc.max_total_uncompressed_size = MAX_TOTAL_SIZE;
    desc.max_compression_ratio = MAX_COMPRESSION_RATIO;
    return desc;
}

bool is_ascii(const char *text, size_t size)
{
    for (size_t i = 0; i < size; i++)
    {
        if ((uint8_t)text[i] >= 0x80)
            return false;
    }
    return true;
}

struct read_entry_t
{
    std::string name;
    uint16_t method;
    std::vector<uint8_t> data;
    // Where its stored bytes are in the input, and their size and CRC-32.
    uint64_t data_offset;
    uint64_t compressed_size;
    uint32_t crc32;
};

bool inside(const void *pointer, uint64_t size, const uint8_t *start, size_t total)
{
    const uint8_t *p = (const uint8_t *)pointer;
    return p >= start && size <= total && p - start <= (ptrdiff_t)(total - size);
}

// Reads entry [index] in pieces of [piece] bytes.
APRESULT read_in_pieces(aparchive_reader reader, uint32_t index, uint64_t piece, std::vector<uint8_t> &out)
{
    out.clear();
    aparchive_entry_reader entry = (aparchive_entry_reader)&out; // must be overwritten
    APARCHIVE_ERROR error;
    APRESULT result = aparchive_reader_open_entry(reader, index, &entry, &error);
    APRIFUZZ_CHECK(message_is_terminated(error));
    if (result != APRESULT_OK)
    {
        APRIFUZZ_CHECK(entry == nullptr);
        return result;
    }
    std::vector<uint8_t> buffer((size_t)piece);
    for (;;)
    {
        uint64_t got = UINT64_MAX;
        result = aparchive_entry_reader_read(entry, buffer.data(), piece, &got, &error);
        APRIFUZZ_CHECK(message_is_terminated(error));
        if (result != APRESULT_OK)
            break;
        APRIFUZZ_CHECK(got <= piece);
        out.insert(out.end(), buffer.begin(), buffer.begin() + (size_t)got);
        APRIFUZZ_CHECK(out.size() <= MAX_ENTRY_SIZE);
        if (got < piece)
        {
            uint64_t more = UINT64_MAX;
            APRIFUZZ_CHECK(aparchive_entry_reader_read(entry, buffer.data(), piece, &more, nullptr) == APRESULT_OK);
            APRIFUZZ_CHECK(more == 0);
            break;
        }
    }
    aparchive_entry_reader_destroy(entry);
    return result;
}

// The extra field helpers over an entry's extra field: whatever they find
// is inside it.
void check_extra_helpers(const APARCHIVE_ENTRY_INFO &info)
{
    const uint16_t ids[] = {APARCHIVE_EXTRA_UNIX_TIMES, APARCHIVE_EXTRA_NTFS_TIMES, APARCHIVE_EXTRA_UNICODE_PATH,
                            0x0001};
    for (uint16_t id : ids)
    {
        const void *data = nullptr;
        uint16_t data_size = 0;
        APRESULT result = aparchive_extra_find(info.extra, info.extra_size, id, &data, &data_size);
        APRIFUZZ_CHECK(result == APRESULT_OK || result == APRESULT_NOT_FOUND || result == APRESULT_APARCHIVE_MALFORMED);
        if (result == APRESULT_OK)
            APRIFUZZ_CHECK(inside(data, data_size, (const uint8_t *)info.extra, info.extra_size));
    }
    APARCHIVE_FILE_TIMES times{};
    if (aparchive_extra_get_unix_times(info.extra, info.extra_size, &times) == APRESULT_OK)
    {
        // What was read writes back.
        uint8_t record[32];
        uint16_t record_size = 0;
        if (times.has_modified || times.has_accessed || times.has_created)
            APRIFUZZ_CHECK(aparchive_extra_put_unix_times(&times, false, record, sizeof(record), &record_size) ==
                           APRESULT_OK);
    }
    if (aparchive_extra_get_ntfs_times(info.extra, info.extra_size, &times) == APRESULT_OK)
    {
        uint8_t record[36];
        uint16_t record_size = 0;
        APRESULT result = aparchive_extra_put_ntfs_times(&times, record, sizeof(record), &record_size);
        APRIFUZZ_CHECK(result == APRESULT_OK || result == APRESULT_INVALID_ARGUMENT); // before 1601
    }
    const char *path = nullptr;
    uint16_t path_size = 0;
    if (aparchive_extra_get_unicode_path(info.extra, info.extra_size, info.name, info.name_size, &path, &path_size) ==
        APRESULT_OK)
        APRIFUZZ_CHECK(inside(path, path_size, (const uint8_t *)info.extra, info.extra_size));
}

// An archive held in memory that can be cut, for updating.
bool vector_truncate(void *user, uint64_t size)
{
    std::vector<uint8_t> &file = *(std::vector<uint8_t> *)user;
    APRIFUZZ_CHECK(size <= file.size());
    file.resize((size_t)size);
    return true;
}

// Writes [entries] with the writer and reads them back.
void check_rewrite(aparchive_codecs codecs, const std::vector<read_entry_t> &entries)
{
    std::vector<uint8_t> archive;
    APARCHIVE_WRITER_DESC writer_desc{};
    writer_desc.base.type = APRI_STRUCT_TYPE_APARCHIVE_WRITER_DESC;
    writer_desc.sink.user = &archive;
    writer_desc.sink.write = vector_write;
    writer_desc.codecs = codecs;
    aparchive_writer writer = nullptr;
    APRIFUZZ_CHECK(aparchive_writer_create(&writer_desc, &writer) == APRESULT_OK);

    std::vector<const read_entry_t *> written;
    for (const read_entry_t &entry : entries)
    {
        APARCHIVE_ENTRY_DESC desc{};
        desc.base.type = APRI_STRUCT_TYPE_APARCHIVE_ENTRY_DESC;
        desc.name = entry.name.c_str();
        desc.method = entry.method;
        desc.level = 1;
        desc.utf8_name_flag = !is_ascii(entry.name.data(), entry.name.size());
        APARCHIVE_ERROR error;
        APRESULT result;
        // Every other entry streamed, in pieces of a size its place picks.
        if (written.size() % 2 == 1)
        {
            result = aparchive_writer_begin_entry(writer, &desc, &error);
            APRIFUZZ_CHECK(message_is_terminated(error));
            if (result == APRESULT_OK)
            {
                size_t piece = 1 + written.size() * 977 % 5000;
                for (size_t at = 0; at < entry.data.size(); at += piece)
                {
                    size_t size = entry.data.size() - at < piece ? entry.data.size() - at : piece;
                    APRIFUZZ_CHECK(aparchive_writer_write_entry_data(writer, entry.data.data() + at, size, nullptr) ==
                                   APRESULT_OK);
                }
                APRIFUZZ_CHECK(aparchive_writer_end_entry(writer, nullptr) == APRESULT_OK);
            }
        }
        else
            result = aparchive_writer_add_entry(writer, &desc, entry.data.data(), entry.data.size(), &error);
        APRIFUZZ_CHECK(message_is_terminated(error));
        // A name the reader takes but the writer doesn't (not UTF-8, or one
        // the writer leaves out) is refused before anything is written.
        if (result == APRESULT_INVALID_ARGUMENT)
            continue;
        APRIFUZZ_CHECK(result == APRESULT_OK);
        written.push_back(&entry);
    }
    APRIFUZZ_CHECK(aparchive_writer_finish(writer, nullptr) == APRESULT_OK);
    aparchive_writer_destroy(writer);

    APARCHIVE_READER_DESC desc = reader_desc(codecs);
    desc.source.user = &archive;
    desc.source.size = archive.size();
    desc.source.read = vector_read;
    // What came out of a reader with these limits goes back in under them,
    // except that DEFLATE may compress better than the original did.
    desc.max_compression_ratio = UINT32_MAX;
    aparchive_reader reader = nullptr;
    APARCHIVE_ERROR error;
    APRESULT result = aparchive_reader_open(&desc, &reader, &error);
    if (result != APRESULT_OK)
        fprintf(stderr, "rewritten archive: %s\n", error.message);
    APRIFUZZ_CHECK(result == APRESULT_OK);
    APRIFUZZ_CHECK(aparchive_reader_get_entry_count(reader) == written.size());
    for (uint32_t i = 0; i < (uint32_t)written.size(); i++)
    {
        APARCHIVE_ENTRY_INFO info{};
        info.base.type = APRI_STRUCT_TYPE_APARCHIVE_ENTRY_INFO;
        APRIFUZZ_CHECK(aparchive_reader_get_entry(reader, i, &info) == APRESULT_OK);
        APRIFUZZ_CHECK(info.name_size == written[i]->name.size());
        APRIFUZZ_CHECK(memcmp(info.name, written[i]->name.data(), info.name_size) == 0);
        APRIFUZZ_CHECK(info.uncompressed_size == written[i]->data.size());
        std::vector<uint8_t> data((size_t)info.uncompressed_size);
        APRIFUZZ_CHECK(aparchive_reader_read_entry(reader, i, data.data(), data.size(), nullptr) == APRESULT_OK);
        APRIFUZZ_CHECK(data == written[i]->data);
    }
    aparchive_reader_destroy(reader);
}

// Copies [entries] raw from the input's stored bytes and reads them back.
void check_raw_copy(aparchive_codecs codecs, const uint8_t *input, const std::vector<read_entry_t> &entries)
{
    std::vector<uint8_t> archive;
    APARCHIVE_WRITER_DESC writer_desc{};
    writer_desc.base.type = APRI_STRUCT_TYPE_APARCHIVE_WRITER_DESC;
    writer_desc.sink.user = &archive;
    writer_desc.sink.write = vector_write;
    writer_desc.codecs = codecs;
    aparchive_writer writer = nullptr;
    APRIFUZZ_CHECK(aparchive_writer_create(&writer_desc, &writer) == APRESULT_OK);
    std::vector<const read_entry_t *> written;
    for (const read_entry_t &entry : entries)
    {
        APARCHIVE_ENTRY_DESC desc{};
        desc.base.type = APRI_STRUCT_TYPE_APARCHIVE_ENTRY_DESC;
        desc.name = entry.name.c_str();
        desc.method = entry.method;
        desc.utf8_name_flag = !is_ascii(entry.name.data(), entry.name.size());
        APRESULT result = aparchive_writer_add_raw_entry(writer, &desc, input + entry.data_offset,
                                                         entry.compressed_size, entry.data.size(), entry.crc32, nullptr);
        if (result == APRESULT_INVALID_ARGUMENT)
            continue;
        APRIFUZZ_CHECK(result == APRESULT_OK);
        written.push_back(&entry);
    }
    APRIFUZZ_CHECK(aparchive_writer_finish(writer, nullptr) == APRESULT_OK);
    aparchive_writer_destroy(writer);

    APARCHIVE_READER_DESC desc = reader_desc(codecs);
    desc.source.user = &archive;
    desc.source.size = archive.size();
    desc.source.read = vector_read;
    desc.strict = APARCHIVE_STRICT_NO_GAPS; // the writer leaves none
    aparchive_reader reader = nullptr;
    APRIFUZZ_CHECK(aparchive_reader_open(&desc, &reader, nullptr) == APRESULT_OK);
    APRIFUZZ_CHECK(aparchive_reader_get_entry_count(reader) == written.size());
    for (uint32_t i = 0; i < (uint32_t)written.size(); i++)
    {
        std::vector<uint8_t> data(written[i]->data.size());
        APRIFUZZ_CHECK(aparchive_reader_read_entry(reader, i, data.data(), data.size(), nullptr) == APRESULT_OK);
        APRIFUZZ_CHECK(data == written[i]->data);
    }
    aparchive_reader_destroy(reader);
}

// Updates the archive: its first entry dropped, one added.
void check_update(aparchive_codecs codecs, aparchive_reader reader, const uint8_t *input, size_t size)
{
    uint32_t count = aparchive_reader_get_entry_count(reader);
    std::vector<APARCHIVE_ENTRY_INFO> before(count);
    std::vector<std::string> names(count);
    for (uint32_t i = 0; i < count; i++)
    {
        before[i].base.type = APRI_STRUCT_TYPE_APARCHIVE_ENTRY_INFO;
        APRIFUZZ_CHECK(aparchive_reader_get_entry(reader, i, &before[i]) == APRESULT_OK);
        names[i].assign(before[i].name, before[i].name_size);
    }
    std::vector<uint8_t> file(input, input + size);
    APARCHIVE_UPDATE_DESC desc{};
    desc.base.type = APRI_STRUCT_TYPE_APARCHIVE_UPDATE_DESC;
    desc.reader = reader;
    desc.truncate = vector_truncate;
    desc.sink.user = &file;
    desc.sink.write = vector_write;
    desc.codecs = codecs;
    aparchive_writer writer = nullptr;
    APRIFUZZ_CHECK(aparchive_writer_create_update(&desc, &writer, nullptr) == APRESULT_OK);
    if (count > 0)
        APRIFUZZ_CHECK(aparchive_writer_remove_entry(writer, names[0].c_str(), nullptr) == APRESULT_OK);
    APARCHIVE_ENTRY_DESC entry{};
    entry.base.type = APRI_STRUCT_TYPE_APARCHIVE_ENTRY_DESC;
    entry.name = "fuzz/added-by-update";
    entry.method = APARCHIVE_METHOD_DEFLATE;
    entry.level = 1;
    APRESULT result = aparchive_writer_add_entry(writer, &entry, "added", 5, nullptr);
    APRIFUZZ_CHECK(result == APRESULT_OK || result == APRESULT_INVALID_ARGUMENT); // the name may be there
    bool added = result == APRESULT_OK;
    APRIFUZZ_CHECK(aparchive_writer_finish(writer, nullptr) == APRESULT_OK);
    aparchive_writer_destroy(writer);

    APARCHIVE_READER_DESC reader_desc_ = reader_desc(codecs);
    reader_desc_.source.user = &file;
    reader_desc_.source.size = file.size();
    reader_desc_.source.read = vector_read;
    aparchive_reader updated = nullptr;
    APARCHIVE_ERROR error;
    result = aparchive_reader_open(&reader_desc_, &updated, &error);
    if (result != APRESULT_OK)
        fprintf(stderr, "updated archive: %s\n", error.message);
    APRIFUZZ_CHECK(result == APRESULT_OK);
    APRIFUZZ_CHECK(aparchive_reader_get_offset_shift(updated) == 0);
    uint32_t kept = count > 0 ? count - 1 : 0;
    APRIFUZZ_CHECK(aparchive_reader_get_entry_count(updated) == kept + (added ? 1 : 0));
    for (uint32_t i = 0; i < kept; i++)
    {
        APARCHIVE_ENTRY_INFO info{};
        info.base.type = APRI_STRUCT_TYPE_APARCHIVE_ENTRY_INFO;
        APRIFUZZ_CHECK(aparchive_reader_get_entry(updated, i, &info) == APRESULT_OK);
        const APARCHIVE_ENTRY_INFO &old = before[i + 1];
        APRIFUZZ_CHECK(info.name_size == old.name_size && memcmp(info.name, names[i + 1].data(), info.name_size) == 0);
        APRIFUZZ_CHECK(info.crc32 == old.crc32 && info.method == old.method && info.flags == old.flags);
        APRIFUZZ_CHECK(info.compressed_size == old.compressed_size && info.uncompressed_size == old.uncompressed_size);
        APRIFUZZ_CHECK(info.local_header_offset == old.local_header_offset);
        APRIFUZZ_CHECK(info.external_attributes == old.external_attributes);
        APRIFUZZ_CHECK(info.comment_size == old.comment_size);
    }
    if (added)
    {
        uint32_t index = UINT32_MAX;
        APRIFUZZ_CHECK(aparchive_reader_find(updated, "fuzz/added-by-update", &index) == APRESULT_OK);
        char data[5];
        APRIFUZZ_CHECK(aparchive_reader_read_entry(updated, index, data, 5, nullptr) == APRESULT_OK);
        APRIFUZZ_CHECK(memcmp(data, "added", 5) == 0);
    }
    aparchive_reader_destroy(updated);
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    aparchive_codecs codecs = nullptr;
    APRIFUZZ_CHECK(aparchive_codecs_create(&codecs) == APRESULT_OK);
    APRIFUZZ_CHECK(aparchive_codecs_add_deflate(codecs) == APRESULT_OK);

    memory_t memory{data, size};
    APARCHIVE_READER_DESC desc = reader_desc(codecs);
    desc.source.user = &memory;
    desc.source.size = size;
    desc.source.read = memory_read;

    aparchive_reader reader = (aparchive_reader)&memory; // must be overwritten
    APARCHIVE_ERROR error;
    APRESULT result = aparchive_reader_open(&desc, &reader, &error);
    APRIFUZZ_CHECK(message_is_terminated(error));
    if (result != APRESULT_OK)
    {
        APRIFUZZ_CHECK(reader == nullptr);
        aparchive_codecs_destroy(codecs);
        return 0;
    }
    APRIFUZZ_CHECK(reader != nullptr);

    // Strict opens refuse only what a lenient one opens.
    const uint32_t strict_flags[] = {APARCHIVE_STRICT_NO_ARCHIVE_COMMENT, APARCHIVE_STRICT_NO_ENTRY_COMMENTS,
                                     APARCHIVE_STRICT_NO_DIRECTORY_ENTRIES, APARCHIVE_STRICT_NO_GAPS};
    for (uint32_t flag : strict_flags)
    {
        APARCHIVE_READER_DESC strict_desc = desc;
        strict_desc.strict = (APARCHIVE_STRICT)flag;
        aparchive_reader strict = nullptr;
        result = aparchive_reader_open(&strict_desc, &strict, &error);
        APRIFUZZ_CHECK(message_is_terminated(error));
        APRIFUZZ_CHECK(result == APRESULT_OK || result == APRESULT_APARCHIVE_MALFORMED || result == APRESULT_IO);
        APRIFUZZ_CHECK((result == APRESULT_OK) == (strict != nullptr));
        if (strict)
            APRIFUZZ_CHECK(aparchive_reader_get_entry_count(strict) == aparchive_reader_get_entry_count(reader));
        aparchive_reader_destroy(strict);
    }
    const void *comment = nullptr;
    uint16_t comment_size = 0;
    APRIFUZZ_CHECK(aparchive_reader_get_comment(reader, &comment, &comment_size) == APRESULT_OK);
    APRIFUZZ_CHECK(comment_size == 0 ? comment == nullptr : comment != nullptr);
    APRIFUZZ_CHECK(aparchive_reader_get_offset_shift(reader) < size);

    uint32_t count = aparchive_reader_get_entry_count(reader);
    APRIFUZZ_CHECK(count <= MAX_ENTRY_COUNT);
    uint64_t total = 0;
    bool all_read = true;
    std::vector<read_entry_t> entries;
    for (uint32_t i = 0; i < count; i++)
    {
        APARCHIVE_ENTRY_INFO info{};
        info.base.type = APRI_STRUCT_TYPE_APARCHIVE_ENTRY_INFO;
        APRIFUZZ_CHECK(aparchive_reader_get_entry(reader, i, &info) == APRESULT_OK);
        APRIFUZZ_CHECK(info.name != nullptr);
        APRIFUZZ_CHECK(info.name[info.name_size] == '\0');
        APRIFUZZ_CHECK(aparchive_is_safe_name(info.name, info.name_size));
        APRIFUZZ_CHECK(info.uncompressed_size <= MAX_ENTRY_SIZE);
        total += info.uncompressed_size;
        APRIFUZZ_CHECK(total <= MAX_TOTAL_SIZE);
        APRIFUZZ_CHECK(info.local_header_offset < size);
        APRIFUZZ_CHECK(info.extra_size == 0 || info.extra != nullptr);
        APRIFUZZ_CHECK(info.comment_size == 0 ? info.comment == nullptr : info.comment != nullptr);
        check_extra_helpers(info);

        // A name with a NUL in it isn't safe, so find() sees all of it.
        uint32_t found = UINT32_MAX;
        APRIFUZZ_CHECK(aparchive_reader_find(reader, info.name, &found) == APRESULT_OK);
        APRIFUZZ_CHECK(found == i);

        uint64_t data_offset = 0;
        result = aparchive_reader_get_data_offset(reader, i, &data_offset, &error);
        APRIFUZZ_CHECK(message_is_terminated(error));
        if (result != APRESULT_OK)
        {
            all_read = false;
            std::vector<uint8_t> pieces;
            APRIFUZZ_CHECK(read_in_pieces(reader, i, 4096, pieces) != APRESULT_OK);
            continue;
        }
        APRIFUZZ_CHECK(data_offset <= size);
        APRIFUZZ_CHECK(info.compressed_size <= size - data_offset);

        std::vector<uint8_t> bytes((size_t)info.uncompressed_size);
        result = aparchive_reader_read_entry(reader, i, bytes.data(), bytes.size(), &error);
        APRIFUZZ_CHECK(message_is_terminated(error));

        // In pieces, the same answer: the same bytes, or a failure - with
        // one exception the header names: DEFLATE data using a code its
        // own Huffman table leaves unassigned, which libdeflate (whole)
        // decodes and zlib-ng (pieces) refuses as corrupt
        // (regressions/aparchive_fuzz/deflate_unassigned_code_whole_only.zip).
        std::vector<uint8_t> pieces;
        APRESULT pieces_result = read_in_pieces(reader, i, 1 + (uint64_t)i * 7919 % 4096, pieces);
        if (pieces_result == APRESULT_OK)
            APRIFUZZ_CHECK(result == APRESULT_OK && pieces == bytes);
        else if (result == APRESULT_OK)
            APRIFUZZ_CHECK(pieces_result == APRESULT_CORRUPT_DATA && info.method == APARCHIVE_METHOD_DEFLATE);
        if (result != APRESULT_OK)
        {
            APRIFUZZ_CHECK(result == APRESULT_APARCHIVE_MALFORMED || result == APRESULT_APARCHIVE_UNKNOWN_METHOD ||
                           result == APRESULT_CORRUPT_DATA);
            all_read = false;
            continue;
        }
        if (info.method == APARCHIVE_METHOD_STORED)
            APRIFUZZ_CHECK(bytes.empty() || memcmp(bytes.data(), data + data_offset, bytes.size()) == 0);
        if (!info.is_directory)
            entries.push_back({std::string(info.name, info.name_size), info.method, std::move(bytes), data_offset,
                               info.compressed_size, info.crc32});
    }
    // Directory entries left out: the writer writes them only when its desc
    // allows, and they have nothing to compare.
    if (all_read)
    {
        check_rewrite(codecs, entries);
        check_raw_copy(codecs, data, entries);
        check_update(codecs, reader, data, size);
    }

    aparchive_reader_destroy(reader);
    aparchive_codecs_destroy(codecs);
    return 0;
}
