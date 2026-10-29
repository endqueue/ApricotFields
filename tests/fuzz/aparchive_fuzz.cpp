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
//   the same bytes under the same name.

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
};

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
        APRESULT result = aparchive_writer_add_entry(writer, &desc, entry.data.data(), entry.data.size(), &error);
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
            continue;
        }
        APRIFUZZ_CHECK(data_offset <= size);
        APRIFUZZ_CHECK(info.compressed_size <= size - data_offset);

        std::vector<uint8_t> bytes((size_t)info.uncompressed_size);
        result = aparchive_reader_read_entry(reader, i, bytes.data(), bytes.size(), &error);
        APRIFUZZ_CHECK(message_is_terminated(error));
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
            entries.push_back({std::string(info.name, info.name_size), info.method, std::move(bytes)});
    }
    // A directory entry is the one kind the writer doesn't write yet.
    if (all_read)
        check_rewrite(codecs, entries);

    aparchive_reader_destroy(reader);
    aparchive_codecs_destroy(codecs);
    return 0;
}
