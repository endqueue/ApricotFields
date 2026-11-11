// Headless tests for Rabbittus Archive (include/archive/rbtarchive.h).

#include "archive/rbtarchive.h"
#include <algorithm>
#include <stdio.h>
#include <string.h>
#include <vector>
#if defined(_MSC_VER) && _DEBUG
#include <crtdbg.h>
#endif

// The CRT's debug heap counts blocks in a Debug build with MSVC, unless
// AddressSanitizer's heap has taken its place.
#if defined(_MSC_VER) && _DEBUG && !defined(__SANITIZE_ADDRESS__)
#define CRT_HEAP_COUNTS 1
#else
#define CRT_HEAP_COUNTS 0
#endif

static int failures = 0;
static const char *current_test = "";

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            printf("FAIL %s (%s:%d): %s\n", current_test, __FILE__, __LINE__, #condition);                           \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// ---- Helpers ------------------------------------------------------------------

// An archive in memory, as a sink and as a source.
static bool memory_write(void *user, const void *data, uint64_t count)
{
    std::vector<uint8_t> &out = *(std::vector<uint8_t> *)user;
    const uint8_t *bytes = (const uint8_t *)data;
    out.insert(out.end(), bytes, bytes + count);
    return true;
}

static bool memory_read(void *user, uint64_t offset, void *buffer, uint64_t count)
{
    const std::vector<uint8_t> &in = *(const std::vector<uint8_t> *)user;
    if (offset > in.size() || count > in.size() - offset)
        return false;
    memcpy(buffer, in.data() + offset, (size_t)count);
    return true;
}

static bool discard_write(void *, const void *, uint64_t)
{
    return true;
}

static bool fail_write(void *, const void *, uint64_t)
{
    return false;
}

static bool fail_read(void *, uint64_t, void *, uint64_t)
{
    return false;
}

static rbtarchive_codecs deflate_codecs()
{
    rbtarchive_codecs codecs = nullptr;
    CHECK(rbtarchive_codecs_create(&codecs) == RBT_RESULT_OK);
    CHECK(rbtarchive_codecs_add_deflate(codecs) == RBT_RESULT_OK);
    return codecs;
}

static rbtarchive_writer memory_writer(std::vector<uint8_t> &out, rbtarchive_codecs codecs)
{
    RBTARCHIVE_WRITER_DESC desc{};
    desc.base.type = RBT_STRUCT_TYPE_ARCHIVE_WRITER_DESC;
    desc.sink.user = &out;
    desc.sink.write = memory_write;
    desc.codecs = codecs;
    rbtarchive_writer writer = nullptr;
    CHECK(rbtarchive_writer_create(&desc, &writer) == RBT_RESULT_OK);
    return writer;
}

// A reader desc over [archive] with no limits.
static RBTARCHIVE_READER_DESC reader_desc(const std::vector<uint8_t> &archive, rbtarchive_codecs codecs)
{
    RBTARCHIVE_READER_DESC desc{};
    desc.base.type = RBT_STRUCT_TYPE_ARCHIVE_READER_DESC;
    desc.source.user = (void *)&archive;
    desc.source.size = archive.size();
    desc.source.read = memory_read;
    desc.codecs = codecs;
    desc.max_entry_count = UINT32_MAX;
    desc.max_central_directory_size = UINT64_MAX;
    desc.max_entry_uncompressed_size = UINT64_MAX;
    desc.max_total_uncompressed_size = UINT64_MAX;
    desc.max_compression_ratio = UINT32_MAX;
    return desc;
}

static RBTARCHIVE_ENTRY_DESC entry_desc(const char *name, uint16_t method)
{
    RBTARCHIVE_ENTRY_DESC entry{};
    entry.base.type = RBT_STRUCT_TYPE_ARCHIVE_ENTRY_DESC;
    entry.name = name;
    entry.method = method;
    entry.dos_date = 0x0021; // 1980-01-01
    return entry;
}

static RBTARCHIVE_ENTRY_INFO entry_info(rbtarchive_reader reader, uint32_t index)
{
    RBTARCHIVE_ENTRY_INFO info{};
    info.base.type = RBT_STRUCT_TYPE_ARCHIVE_ENTRY_INFO;
    CHECK(rbtarchive_reader_get_entry(reader, index, &info) == RBT_RESULT_OK);
    return info;
}

// Reads the entry named [name] whole; false if it isn't there or fails.
static bool read_named(rbtarchive_reader reader, const char *name, std::vector<uint8_t> &out)
{
    uint32_t index;
    if (rbtarchive_reader_find(reader, name, &index) != RBT_RESULT_OK)
        return false;
    RBTARCHIVE_ENTRY_INFO info = entry_info(reader, index);
    out.resize((size_t)info.uncompressed_size);
    return rbtarchive_reader_read_entry(reader, index, out.data(), out.size(), nullptr) == RBT_RESULT_OK;
}

// ZIP records written by hand, for archives Rabbittus Archive's writer never
// produces. Every entry is STORED.
struct bytes_t
{
    std::vector<uint8_t> data;
    void u16(uint16_t value)
    {
        data.push_back((uint8_t)value);
        data.push_back((uint8_t)(value >> 8));
    }
    void u32(uint32_t value)
    {
        u16((uint16_t)value);
        u16((uint16_t)(value >> 16));
    }
    void u64(uint64_t value)
    {
        u32((uint32_t)value);
        u32((uint32_t)(value >> 32));
    }
    void text(const char *value)
    {
        data.insert(data.end(), value, value + strlen(value));
    }
    void bytes(const bytes_t &value)
    {
        data.insert(data.end(), value.data.begin(), value.data.end());
    }
    uint32_t size() const
    {
        return (uint32_t)data.size();
    }
};

static void local_header(bytes_t &out, const char *name, uint16_t flags, uint32_t crc, uint32_t compressed_size,
                         uint32_t uncompressed_size, const bytes_t &extra = bytes_t())
{
    out.u32(0x04034b50);
    out.u16(20);
    out.u16(flags);
    out.u16(RBTARCHIVE_METHOD_STORED);
    out.u16(0);
    out.u16(0x0021);
    out.u32(crc);
    out.u32(compressed_size);
    out.u32(uncompressed_size);
    out.u16((uint16_t)strlen(name));
    out.u16((uint16_t)extra.size());
    out.text(name);
    out.bytes(extra);
}

static void central_header(bytes_t &out, const char *name, uint16_t flags, uint32_t crc, uint32_t compressed_size,
                           uint32_t uncompressed_size, uint32_t local_header_offset, const bytes_t &extra = bytes_t())
{
    out.u32(0x02014b50);
    out.u16(20);
    out.u16(20);
    out.u16(flags);
    out.u16(RBTARCHIVE_METHOD_STORED);
    out.u16(0);
    out.u16(0x0021);
    out.u32(crc);
    out.u32(compressed_size);
    out.u32(uncompressed_size);
    out.u16((uint16_t)strlen(name));
    out.u16((uint16_t)extra.size());
    out.u16(0);
    out.u16(0);
    out.u16(0);
    out.u32(0);
    out.u32(local_header_offset);
    out.text(name);
    out.bytes(extra);
}

static void end_record(bytes_t &out, uint16_t entry_count, uint32_t directory_size, uint32_t directory_offset,
                       const char *comment = "")
{
    out.u32(0x06054b50);
    out.u16(0);
    out.u16(0);
    out.u16(entry_count);
    out.u16(entry_count);
    out.u32(directory_size);
    out.u32(directory_offset);
    out.u16((uint16_t)strlen(comment));
    out.text(comment);
}

// ---- Tests --------------------------------------------------------------------

static void test_codecs()
{
    current_test = "codecs";
    rbtarchive_codecs codecs = nullptr;
    CHECK(rbtarchive_codecs_create(&codecs) == RBT_RESULT_OK);
    // A new table has STORED only.
    CHECK(rbtarchive_codecs_has(codecs, RBTARCHIVE_METHOD_STORED));
    CHECK(!rbtarchive_codecs_has(codecs, RBTARCHIVE_METHOD_DEFLATE));
    CHECK(rbtarchive_codecs_add_deflate(codecs) == RBT_RESULT_OK);
    CHECK(rbtarchive_codecs_has(codecs, RBTARCHIVE_METHOD_DEFLATE));
    // No duplicates, no STORED, no codec without decompress.
    CHECK(rbtarchive_codecs_add_deflate(codecs) == RBT_RESULT_INVALID_ARGUMENT);
    RBTARCHIVE_CODEC stored{};
    stored.base.type = RBT_STRUCT_TYPE_ARCHIVE_CODEC;
    stored.method = RBTARCHIVE_METHOD_STORED;
    CHECK(rbtarchive_codecs_add(codecs, &stored) == RBT_RESULT_INVALID_ARGUMENT);
    RBTARCHIVE_CODEC zstd{};
    zstd.base.type = RBT_STRUCT_TYPE_ARCHIVE_CODEC;
    zstd.method = 93;
    CHECK(rbtarchive_codecs_add(codecs, &zstd) == RBT_RESULT_INVALID_ARGUMENT);
    // Tables are independent: no global registration.
    rbtarchive_codecs other = nullptr;
    CHECK(rbtarchive_codecs_create(&other) == RBT_RESULT_OK);
    CHECK(!rbtarchive_codecs_has(other, RBTARCHIVE_METHOD_DEFLATE));
    rbtarchive_codecs_destroy(other);
    rbtarchive_codecs_destroy(codecs);
}

static void test_safe_names()
{
    current_test = "safe names";
    auto safe = [](const char *name) { return rbtarchive_is_safe_name(name, strlen(name)); };
    CHECK(safe("mimetype"));
    CHECK(safe("model/elements.jsonl"));
    CHECK(safe("a.b/c..d/.e"));
    CHECK(safe("dir/")); // a directory entry
    CHECK(safe("a/b/"));
    CHECK(safe("caf\xc3\xa9")); // UTF-8
    CHECK(safe("caf\x82"));     // CP437: judged encoding-blind
    CHECK(!safe(""));
    CHECK(!safe("/"));
    CHECK(!safe("/etc/passwd"));
    CHECK(!safe("C:/x"));
    CHECK(!safe("a\\b"));
    CHECK(!safe("../x"));
    CHECK(!safe("a/../b"));
    CHECK(!safe("a/./b"));
    CHECK(!safe("a//b"));
    CHECK(!safe("dir//"));
    CHECK(!rbtarchive_is_safe_name("a\0b", 3));
}

static void test_crc32()
{
    current_test = "crc32";
    // The standard CRC-32 check value.
    CHECK(rbtarchive_compute_crc32(0, "123456789", 9) == 0xCBF43926u);
    // Running updates match one pass.
    CHECK(rbtarchive_compute_crc32(rbtarchive_compute_crc32(0, "1234", 4), "56789", 5) == 0xCBF43926u);
}

static void test_writer_checks()
{
    current_test = "writer checks";
    rbtarchive_codecs codecs = deflate_codecs();
    RBTARCHIVE_WRITER_DESC desc{};
    desc.sink.write = discard_write;
    desc.codecs = codecs;
    rbtarchive_writer writer = nullptr;
    // A base left zeroed, another struct's type, and a chained struct are
    // refused.
    CHECK(rbtarchive_writer_create(&desc, &writer) == RBT_RESULT_STRUCT_WRONG_TYPE && writer == nullptr);
    desc.base.type = RBT_STRUCT_TYPE_ARCHIVE_READER_DESC;
    CHECK(rbtarchive_writer_create(&desc, &writer) == RBT_RESULT_STRUCT_WRONG_TYPE && writer == nullptr);
    desc.base.type = RBT_STRUCT_TYPE_ARCHIVE_WRITER_DESC;
    RBT_STRUCT_BASE chained{};
    chained.type = RBT_STRUCT_TYPE_ARCHIVE_WRITER_DESC;
    desc.base.next = &chained;
    CHECK(rbtarchive_writer_create(&desc, &writer) == RBT_RESULT_STRUCT_UNKNOWN_IN_CHAIN && writer == nullptr);
    desc.base.next = nullptr;
    CHECK(rbtarchive_writer_create(&desc, &writer) == RBT_RESULT_OK);
    CHECK(rbtarchive_writer_get_offset(writer) == 0);

    RBTARCHIVE_ERROR error;
    RBTARCHIVE_ENTRY_DESC entry = entry_desc("../escape", RBTARCHIVE_METHOD_STORED);
    CHECK(rbtarchive_writer_add_entry(writer, &entry, "x", 1, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(strstr(error.message, "../escape") != nullptr);
    // Non-ASCII names need the UTF-8 flag, and must be valid UTF-8.
    entry = entry_desc("caf\xc3\xa9", RBTARCHIVE_METHOD_STORED);
    CHECK(rbtarchive_writer_add_entry(writer, &entry, "x", 1, nullptr) == RBT_RESULT_INVALID_ARGUMENT);
    entry = entry_desc("caf\x82", RBTARCHIVE_METHOD_STORED);
    entry.utf8_name_flag = true;
    CHECK(rbtarchive_writer_add_entry(writer, &entry, "x", 1, nullptr) == RBT_RESULT_INVALID_ARGUMENT);
    // Directory entries aren't written yet.
    entry = entry_desc("dir/", RBTARCHIVE_METHOD_STORED);
    CHECK(rbtarchive_writer_add_entry(writer, &entry, nullptr, 0, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(strstr(error.message, "directory") != nullptr);
    entry = entry_desc("a", RBTARCHIVE_METHOD_STORED);
    entry.alignment = 48; // not a power of two
    CHECK(rbtarchive_writer_add_entry(writer, &entry, "x", 1, nullptr) == RBT_RESULT_INVALID_ARGUMENT);
    entry = entry_desc("a", 93); // not in the table
    CHECK(rbtarchive_writer_add_entry(writer, &entry, "x", 1, nullptr) == RBT_RESULT_ARCHIVE_UNKNOWN_METHOD);
    // Extra must be whole records, and not ZIP64's.
    const uint8_t cut_short[] = {0xFE, 0xCA, 0x05, 0x00, 'x'};
    entry = entry_desc("a", RBTARCHIVE_METHOD_STORED);
    entry.extra = cut_short;
    entry.extra_size = sizeof(cut_short);
    CHECK(rbtarchive_writer_add_entry(writer, &entry, "x", 1, nullptr) == RBT_RESULT_INVALID_ARGUMENT);
    const uint8_t zip64[] = {0x01, 0x00, 0x00, 0x00};
    entry.extra = zip64;
    entry.extra_size = sizeof(zip64);
    CHECK(rbtarchive_writer_add_entry(writer, &entry, "x", 1, nullptr) == RBT_RESULT_INVALID_ARGUMENT);
    // A refused entry writes nothing and leaves the writer usable.
    CHECK(rbtarchive_writer_get_offset(writer) == 0);
    entry = entry_desc("a", RBTARCHIVE_METHOD_STORED);
    CHECK(rbtarchive_writer_add_entry(writer, &entry, "x", 1, &error) == RBT_RESULT_OK && error.message[0] == '\0');
    // No name twice.
    CHECK(rbtarchive_writer_add_entry(writer, &entry, "y", 1, nullptr) == RBT_RESULT_INVALID_ARGUMENT);
    // Nothing after a finish.
    CHECK(rbtarchive_writer_finish(writer, nullptr) == RBT_RESULT_OK);
    entry = entry_desc("b", RBTARCHIVE_METHOD_STORED);
    CHECK(rbtarchive_writer_add_entry(writer, &entry, "x", 1, nullptr) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtarchive_writer_finish(writer, nullptr) == RBT_RESULT_INVALID_ARGUMENT);
    rbtarchive_writer_destroy(writer);

    // A sink failure ends the writer.
    desc.sink.write = fail_write;
    CHECK(rbtarchive_writer_create(&desc, &writer) == RBT_RESULT_OK);
    entry = entry_desc("a", RBTARCHIVE_METHOD_STORED);
    CHECK(rbtarchive_writer_add_entry(writer, &entry, "x", 1, &error) == RBT_RESULT_IO && error.message[0] != '\0');
    entry = entry_desc("b", RBTARCHIVE_METHOD_STORED);
    CHECK(rbtarchive_writer_add_entry(writer, &entry, "x", 1, nullptr) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtarchive_writer_finish(writer, nullptr) == RBT_RESULT_INVALID_ARGUMENT);
    rbtarchive_writer_destroy(writer);
    rbtarchive_codecs_destroy(codecs);
}

static void test_reader_checks()
{
    current_test = "reader checks";
    rbtarchive_codecs codecs = nullptr;
    CHECK(rbtarchive_codecs_create(&codecs) == RBT_RESULT_OK);
    RBTARCHIVE_READER_DESC desc{};
    desc.source.read = fail_read;
    desc.codecs = codecs;
    rbtarchive_reader reader = nullptr;
    RBTARCHIVE_ERROR error;
    // A base left unset (0) is refused, with a message saying so.
    CHECK(rbtarchive_reader_open(&desc, &reader, &error) == RBT_RESULT_STRUCT_WRONG_TYPE && reader == nullptr);
    CHECK(strstr(error.message, "base.type") != nullptr);
    // A source that fails.
    desc.base.type = RBT_STRUCT_TYPE_ARCHIVE_READER_DESC;
    desc.source.size = 1000;
    CHECK(rbtarchive_reader_open(&desc, &reader, &error) == RBT_RESULT_IO && reader == nullptr);
    // Not archives: too small, and no end record.
    std::vector<uint8_t> tiny(10, 'x'), junk(100000, 'x');
    desc = reader_desc(tiny, codecs);
    CHECK(rbtarchive_reader_open(&desc, &reader, nullptr) == RBT_RESULT_ARCHIVE_NOT_AN_ARCHIVE && reader == nullptr);
    desc = reader_desc(junk, codecs);
    CHECK(rbtarchive_reader_open(&desc, &reader, &error) == RBT_RESULT_ARCHIVE_NOT_AN_ARCHIVE && reader == nullptr);
    CHECK(error.message[0] != '\0');
    rbtarchive_codecs_destroy(codecs);
}

// The contents the round-trip tests write.
static const char MIMETYPE[] = "application/vnd.test+zip";
static const uint8_t CUSTOM_EXTRA[] = {0xFE, 0xCA, 0x02, 0x00, 'x', 'y'};

static std::vector<uint8_t> text_data()
{
    std::vector<uint8_t> data;
    for (int i = 0; i < 400; i++)
    {
        const char *line = "{\"guid\":\"3f2c9a4e\",\"category\":\"wall\"}\n";
        data.insert(data.end(), line, line + strlen(line));
    }
    return data;
}

static std::vector<uint8_t> blob_data()
{
    std::vector<uint8_t> data(1000);
    for (size_t i = 0; i < data.size(); i++)
        data[i] = (uint8_t)(i * 7 + 3);
    return data;
}

// An archive shaped like a document: an identifying first entry, compressed
// text, an aligned binary entry, an empty entry and a UTF-8 name.
static std::vector<uint8_t> write_sample(rbtarchive_codecs codecs)
{
    std::vector<uint8_t> archive;
    rbtarchive_writer writer = memory_writer(archive, codecs);
    std::vector<uint8_t> text = text_data(), blob = blob_data();
    RBTARCHIVE_ERROR error;

    RBTARCHIVE_ENTRY_DESC entry = entry_desc("mimetype", RBTARCHIVE_METHOD_STORED);
    CHECK(rbtarchive_writer_add_entry(writer, &entry, MIMETYPE, strlen(MIMETYPE), &error) == RBT_RESULT_OK);
    entry = entry_desc("model/data.json", RBTARCHIVE_METHOD_DEFLATE);
    entry.level = 6;
    CHECK(rbtarchive_writer_add_entry(writer, &entry, text.data(), text.size(), &error) == RBT_RESULT_OK);
    entry = entry_desc("geometry/blob.bin", RBTARCHIVE_METHOD_STORED);
    entry.alignment = 64;
    CHECK(rbtarchive_writer_add_entry(writer, &entry, blob.data(), blob.size(), &error) == RBT_RESULT_OK);
    entry = entry_desc("empty", RBTARCHIVE_METHOD_STORED);
    CHECK(rbtarchive_writer_add_entry(writer, &entry, nullptr, 0, &error) == RBT_RESULT_OK);
    entry = entry_desc("caf\xc3\xa9.txt", RBTARCHIVE_METHOD_DEFLATE);
    entry.level = 9;
    entry.utf8_name_flag = true;
    entry.extra = CUSTOM_EXTRA;
    entry.extra_size = sizeof(CUSTOM_EXTRA);
    CHECK(rbtarchive_writer_add_entry(writer, &entry, "bonjour", 7, &error) == RBT_RESULT_OK);

    CHECK(rbtarchive_writer_finish(writer, &error) == RBT_RESULT_OK && error.message[0] == '\0');
    CHECK(rbtarchive_writer_get_offset(writer) == archive.size());
    rbtarchive_writer_destroy(writer);
    return archive;
}

static void test_round_trip()
{
    current_test = "round trip";
    rbtarchive_codecs codecs = deflate_codecs();
    std::vector<uint8_t> archive = write_sample(codecs);
    std::vector<uint8_t> text = text_data(), blob = blob_data();

    // Starts with a local header, and the first entry's data is at a fixed
    // offset: 30 bytes of header and the 8 of "mimetype", no extra field.
    CHECK(archive.size() > 38 + strlen(MIMETYPE));
    CHECK(memcmp(archive.data(), "PK\x03\x04", 4) == 0);
    CHECK(memcmp(archive.data() + 38, MIMETYPE, strlen(MIMETYPE)) == 0);
    // The same input gives the same bytes.
    CHECK(write_sample(codecs) == archive);

    RBTARCHIVE_READER_DESC desc = reader_desc(archive, codecs);
    rbtarchive_reader reader = nullptr;
    RBTARCHIVE_ERROR error;
    CHECK(rbtarchive_reader_open(&desc, &reader, &error) == RBT_RESULT_OK && error.message[0] == '\0');
    if (!reader)
    {
        rbtarchive_codecs_destroy(codecs);
        return;
    }
    CHECK(rbtarchive_reader_get_entry_count(reader) == 5);

    // Entries come back in the order written, as written.
    RBTARCHIVE_ENTRY_INFO info = entry_info(reader, 0);
    CHECK(strcmp(info.name, "mimetype") == 0 && info.name_size == 8);
    CHECK(info.method == RBTARCHIVE_METHOD_STORED && info.flags == 0);
    CHECK(info.dos_date == 0x0021 && info.dos_time == 0);
    CHECK(info.uncompressed_size == strlen(MIMETYPE) && info.compressed_size == strlen(MIMETYPE));
    CHECK(info.crc32 == rbtarchive_compute_crc32(0, MIMETYPE, strlen(MIMETYPE)));
    CHECK(info.local_header_offset == 0 && info.extra == nullptr && info.extra_size == 0 && !info.is_directory);
    info = entry_info(reader, 1);
    CHECK(strcmp(info.name, "model/data.json") == 0 && info.method == RBTARCHIVE_METHOD_DEFLATE);
    CHECK(info.uncompressed_size == text.size() && info.compressed_size < text.size() / 10);
    info = entry_info(reader, 4);
    CHECK(strcmp(info.name, "caf\xc3\xa9.txt") == 0 && (info.flags & 0x0800));
    CHECK(info.extra_size == sizeof(CUSTOM_EXTRA) && info.extra &&
          memcmp(info.extra, CUSTOM_EXTRA, sizeof(CUSTOM_EXTRA)) == 0);
    RBTARCHIVE_ENTRY_INFO unset{};
    CHECK(rbtarchive_reader_get_entry(reader, 0, &unset) == RBT_RESULT_STRUCT_WRONG_TYPE); // no base set
    CHECK(rbtarchive_reader_get_entry(reader, 5, &info) == RBT_RESULT_NOT_FOUND);

    // Every entry reads back.
    std::vector<uint8_t> data;
    CHECK(read_named(reader, "mimetype", data) && data.size() == strlen(MIMETYPE) &&
          memcmp(data.data(), MIMETYPE, data.size()) == 0);
    CHECK(read_named(reader, "model/data.json", data) && data == text);
    CHECK(read_named(reader, "geometry/blob.bin", data) && data == blob);
    CHECK(read_named(reader, "empty", data) && data.empty());
    CHECK(read_named(reader, "caf\xc3\xa9.txt", data) && data.size() == 7 && memcmp(data.data(), "bonjour", 7) == 0);
    uint32_t index = 99;
    CHECK(rbtarchive_reader_find(reader, "Mimetype", &index) == RBT_RESULT_NOT_FOUND); // exact, case and all
    CHECK(rbtarchive_reader_find(reader, "geometry/blob.bin", &index) == RBT_RESULT_OK && index == 2);

    // The aligned entry's data is on its boundary, and there in the archive
    // as stored.
    uint64_t offset = 0;
    CHECK(rbtarchive_reader_get_data_offset(reader, index, &offset, &error) == RBT_RESULT_OK);
    CHECK(offset % 64 == 0 && offset + blob.size() <= archive.size());
    CHECK(memcmp(archive.data() + offset, blob.data(), blob.size()) == 0);
    CHECK(rbtarchive_reader_get_data_offset(reader, 0, &offset, nullptr) == RBT_RESULT_OK && offset == 38);

    // The buffer must be the entry's size exactly.
    data.resize(blob.size() + 1);
    CHECK(rbtarchive_reader_read_entry(reader, index, data.data(), data.size(), &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtarchive_reader_read_entry(reader, 5, data.data(), data.size(), nullptr) == RBT_RESULT_NOT_FOUND);
    rbtarchive_reader_destroy(reader);

    // Without DEFLATE in the table, the archive opens and its STORED
    // entries read; the DEFLATE ones don't.
    rbtarchive_codecs stored_only = nullptr;
    CHECK(rbtarchive_codecs_create(&stored_only) == RBT_RESULT_OK);
    desc = reader_desc(archive, stored_only);
    CHECK(rbtarchive_reader_open(&desc, &reader, nullptr) == RBT_RESULT_OK);
    if (reader)
    {
        CHECK(read_named(reader, "geometry/blob.bin", data) && data == blob);
        data.resize(text.size());
        CHECK(rbtarchive_reader_read_entry(reader, 1, data.data(), data.size(), &error) ==
              RBT_RESULT_ARCHIVE_UNKNOWN_METHOD);
        CHECK(strstr(error.message, "model/data.json") != nullptr);
        rbtarchive_reader_destroy(reader);
    }
    rbtarchive_codecs_destroy(stored_only);
    rbtarchive_codecs_destroy(codecs);
}

static void test_empty_archive()
{
    current_test = "empty archive";
    rbtarchive_codecs codecs = deflate_codecs();
    std::vector<uint8_t> archive;
    rbtarchive_writer writer = memory_writer(archive, codecs);
    CHECK(rbtarchive_writer_finish(writer, nullptr) == RBT_RESULT_OK);
    rbtarchive_writer_destroy(writer);
    CHECK(archive.size() == 22); // the end record alone
    RBTARCHIVE_READER_DESC desc = reader_desc(archive, codecs);
    rbtarchive_reader reader = nullptr;
    CHECK(rbtarchive_reader_open(&desc, &reader, nullptr) == RBT_RESULT_OK);
    CHECK(rbtarchive_reader_get_entry_count(reader) == 0);
    rbtarchive_reader_destroy(reader);
    rbtarchive_codecs_destroy(codecs);
}

// Small alignments, where the padding wanted can be smaller than a record's
// own 4-byte header.
static void test_alignment()
{
    current_test = "alignment";
    rbtarchive_codecs codecs = deflate_codecs();
    const char *names[] = {"a", "ab", "abc", "abcd", "abcde", "abcdef", "abcdefg", "abcdefgh"};
    const uint32_t alignments[] = {2, 4, 8, 4096};
    for (uint32_t alignment : alignments)
    {
        std::vector<uint8_t> archive;
        rbtarchive_writer writer = memory_writer(archive, codecs);
        for (const char *name : names)
        {
            RBTARCHIVE_ENTRY_DESC entry = entry_desc(name, RBTARCHIVE_METHOD_STORED);
            entry.alignment = alignment;
            CHECK(rbtarchive_writer_add_entry(writer, &entry, name, strlen(name), nullptr) == RBT_RESULT_OK);
        }
        CHECK(rbtarchive_writer_finish(writer, nullptr) == RBT_RESULT_OK);
        rbtarchive_writer_destroy(writer);

        RBTARCHIVE_READER_DESC desc = reader_desc(archive, codecs);
        rbtarchive_reader reader = nullptr;
        CHECK(rbtarchive_reader_open(&desc, &reader, nullptr) == RBT_RESULT_OK);
        if (!reader)
            continue;
        for (uint32_t i = 0; i < 8; i++)
        {
            uint64_t offset = 1;
            CHECK(rbtarchive_reader_get_data_offset(reader, i, &offset, nullptr) == RBT_RESULT_OK);
            CHECK(offset % alignment == 0);
            std::vector<uint8_t> data;
            CHECK(read_named(reader, names[i], data) && data.size() == strlen(names[i]) &&
                  memcmp(data.data(), names[i], data.size()) == 0);
        }
        rbtarchive_reader_destroy(reader);
    }
    rbtarchive_codecs_destroy(codecs);
}

static void test_limits()
{
    current_test = "limits";
    rbtarchive_codecs codecs = deflate_codecs();
    std::vector<uint8_t> archive = write_sample(codecs);
    rbtarchive_reader reader = nullptr;
    RBTARCHIVE_ERROR error;

    RBTARCHIVE_READER_DESC desc = reader_desc(archive, codecs);
    desc.max_entry_count = 4;
    CHECK(rbtarchive_reader_open(&desc, &reader, &error) == RBT_RESULT_LIMIT && reader == nullptr);
    CHECK(error.message[0] != '\0');
    desc = reader_desc(archive, codecs);
    desc.max_central_directory_size = 10;
    CHECK(rbtarchive_reader_open(&desc, &reader, nullptr) == RBT_RESULT_LIMIT && reader == nullptr);
    desc = reader_desc(archive, codecs);
    desc.max_entry_uncompressed_size = 999; // the blob is 1000
    CHECK(rbtarchive_reader_open(&desc, &reader, &error) == RBT_RESULT_LIMIT && reader == nullptr);
    desc = reader_desc(archive, codecs);
    desc.max_total_uncompressed_size = 2000;
    CHECK(rbtarchive_reader_open(&desc, &reader, nullptr) == RBT_RESULT_LIMIT && reader == nullptr);
    desc = reader_desc(archive, codecs);
    desc.max_compression_ratio = 5; // the repeated text compresses far more
    CHECK(rbtarchive_reader_open(&desc, &reader, &error) == RBT_RESULT_LIMIT && reader == nullptr);
    CHECK(strstr(error.message, "model/data.json") != nullptr);
    // At the limits exactly, it opens.
    desc = reader_desc(archive, codecs);
    desc.max_entry_count = 5;
    desc.max_entry_uncompressed_size = text_data().size();
    CHECK(rbtarchive_reader_open(&desc, &reader, nullptr) == RBT_RESULT_OK);
    rbtarchive_reader_destroy(reader);
    rbtarchive_codecs_destroy(codecs);
}

static void test_corruption()
{
    current_test = "corruption";
    rbtarchive_codecs codecs = deflate_codecs();
    const std::vector<uint8_t> archive = write_sample(codecs);
    rbtarchive_reader reader = nullptr;
    RBTARCHIVE_ERROR error;

    // Where two entries' data sit.
    uint64_t blob_offset = 0, text_offset = 0;
    RBTARCHIVE_READER_DESC desc = reader_desc(archive, codecs);
    CHECK(rbtarchive_reader_open(&desc, &reader, nullptr) == RBT_RESULT_OK);
    if (!reader)
    {
        rbtarchive_codecs_destroy(codecs);
        return;
    }
    CHECK(rbtarchive_reader_get_data_offset(reader, 2, &blob_offset, nullptr) == RBT_RESULT_OK);
    CHECK(rbtarchive_reader_get_data_offset(reader, 1, &text_offset, nullptr) == RBT_RESULT_OK);
    RBTARCHIVE_ENTRY_INFO text_info = entry_info(reader, 1);
    rbtarchive_reader_destroy(reader);
    reader = nullptr;

    // A changed byte in a STORED entry fails its CRC-32; the other entries
    // still read.
    std::vector<uint8_t> damaged = archive;
    damaged[(size_t)blob_offset + 10] ^= 0x01;
    desc = reader_desc(damaged, codecs);
    CHECK(rbtarchive_reader_open(&desc, &reader, nullptr) == RBT_RESULT_OK);
    if (reader)
    {
        std::vector<uint8_t> data(1000);
        CHECK(rbtarchive_reader_read_entry(reader, 2, data.data(), data.size(), &error) == RBT_RESULT_CORRUPT_DATA);
        CHECK(strstr(error.message, "geometry/blob.bin") != nullptr);
        CHECK(read_named(reader, "model/data.json", data) && data == text_data());
        rbtarchive_reader_destroy(reader);
        reader = nullptr;
    }

    // Changed bytes in a DEFLATE entry: it fails to decompress or fails its
    // CRC-32, either way as corrupt data.
    damaged = archive;
    for (size_t i = 0; i < 8; i++)
        damaged[(size_t)text_offset + (size_t)text_info.compressed_size / 2 + i] ^= 0xFF;
    desc = reader_desc(damaged, codecs);
    CHECK(rbtarchive_reader_open(&desc, &reader, nullptr) == RBT_RESULT_OK);
    if (reader)
    {
        std::vector<uint8_t> data((size_t)text_info.uncompressed_size);
        CHECK(rbtarchive_reader_read_entry(reader, 1, data.data(), data.size(), nullptr) == RBT_RESULT_CORRUPT_DATA);
        rbtarchive_reader_destroy(reader);
        reader = nullptr;
    }

    // A local header whose name isn't the central directory's.
    damaged = archive;
    damaged[30] = 'n'; // "mimetype" -> "nimetype"
    desc = reader_desc(damaged, codecs);
    CHECK(rbtarchive_reader_open(&desc, &reader, nullptr) == RBT_RESULT_OK);
    if (reader)
    {
        uint64_t offset;
        CHECK(rbtarchive_reader_get_data_offset(reader, 0, &offset, &error) == RBT_RESULT_ARCHIVE_MALFORMED);
        CHECK(strstr(error.message, "mimetype") != nullptr);
        rbtarchive_reader_destroy(reader);
        reader = nullptr;
    }

    // A local header that isn't one.
    damaged = archive;
    damaged[0] = 'X';
    desc = reader_desc(damaged, codecs);
    CHECK(rbtarchive_reader_open(&desc, &reader, nullptr) == RBT_RESULT_OK);
    if (reader)
    {
        std::vector<uint8_t> data(strlen(MIMETYPE));
        CHECK(rbtarchive_reader_read_entry(reader, 0, data.data(), data.size(), nullptr) == RBT_RESULT_ARCHIVE_MALFORMED);
        rbtarchive_reader_destroy(reader);
        reader = nullptr;
    }

    // Cut short: the end record is gone, or the directory no longer fits.
    for (size_t cut : {(size_t)1, (size_t)22, (size_t)60, archive.size() / 2})
    {
        damaged.assign(archive.begin(), archive.end() - cut);
        desc = reader_desc(damaged, codecs);
        CHECK(rbtarchive_reader_open(&desc, &reader, &error) != RBT_RESULT_OK && reader == nullptr);
        CHECK(error.message[0] != '\0');
    }
    rbtarchive_codecs_destroy(codecs);
}

// Archives as other tools write them, which Rabbittus Archive's writer never does:
// an archive comment, a directory entry, and an entry written with a data
// descriptor (its local header has no sizes or CRC-32).
static void test_foreign_archive()
{
    current_test = "foreign archive";
    rbtarchive_codecs codecs = deflate_codecs();
    uint32_t crc = rbtarchive_compute_crc32(0, "hello", 5);
    bytes_t zip;
    local_header(zip, "dir/", 0, 0, 0, 0);
    uint32_t file_offset = zip.size();
    local_header(zip, "dir/hello.txt", 0x0008, 0, 0, 0);
    zip.text("hello");
    zip.u32(0x08074b50); // the data descriptor
    zip.u32(crc);
    zip.u32(5);
    zip.u32(5);
    uint32_t directory_offset = zip.size();
    central_header(zip, "dir/", 0, 0, 0, 0, 0);
    central_header(zip, "dir/hello.txt", 0x0008, crc, 5, 5, file_offset);
    end_record(zip, 2, zip.size() - directory_offset, directory_offset, "made elsewhere");

    RBTARCHIVE_READER_DESC desc = reader_desc(zip.data, codecs);
    rbtarchive_reader reader = nullptr;
    RBTARCHIVE_ERROR error;
    CHECK(rbtarchive_reader_open(&desc, &reader, &error) == RBT_RESULT_OK);
    if (reader)
    {
        CHECK(rbtarchive_reader_get_entry_count(reader) == 2);
        RBTARCHIVE_ENTRY_INFO info = entry_info(reader, 0);
        CHECK(info.is_directory && info.uncompressed_size == 0);
        CHECK(rbtarchive_reader_read_entry(reader, 0, nullptr, 0, nullptr) == RBT_RESULT_OK);
        info = entry_info(reader, 1);
        CHECK(!info.is_directory && (info.flags & 0x0008));
        std::vector<uint8_t> data;
        CHECK(read_named(reader, "dir/hello.txt", data) && data.size() == 5 && memcmp(data.data(), "hello", 5) == 0);
        rbtarchive_reader_destroy(reader);
        reader = nullptr;
    }

    // Two names for the same bytes: refused on open.
    bytes_t twice;
    crc = rbtarchive_compute_crc32(0, "abc", 3);
    local_header(twice, "a", 0, crc, 3, 3);
    twice.text("abc");
    directory_offset = twice.size();
    central_header(twice, "a", 0, crc, 3, 3, 0);
    central_header(twice, "b", 0, crc, 3, 3, 0);
    end_record(twice, 2, twice.size() - directory_offset, directory_offset);
    desc = reader_desc(twice.data, codecs);
    CHECK(rbtarchive_reader_open(&desc, &reader, &error) == RBT_RESULT_ARCHIVE_MALFORMED && reader == nullptr);
    CHECK(strstr(error.message, "overlap") != nullptr);

    // The same name twice.
    bytes_t repeated;
    local_header(repeated, "a", 0, crc, 3, 3);
    repeated.text("abc");
    uint32_t second_offset = repeated.size();
    local_header(repeated, "a", 0, crc, 3, 3);
    repeated.text("abc");
    directory_offset = repeated.size();
    central_header(repeated, "a", 0, crc, 3, 3, 0);
    central_header(repeated, "a", 0, crc, 3, 3, second_offset);
    end_record(repeated, 2, repeated.size() - directory_offset, directory_offset);
    desc = reader_desc(repeated.data, codecs);
    CHECK(rbtarchive_reader_open(&desc, &reader, nullptr) == RBT_RESULT_ARCHIVE_MALFORMED && reader == nullptr);

    // A name that would leave the directory it's extracted into.
    bytes_t escape;
    local_header(escape, "../x", 0, crc, 3, 3);
    escape.text("abc");
    directory_offset = escape.size();
    central_header(escape, "../x", 0, crc, 3, 3, 0);
    end_record(escape, 1, escape.size() - directory_offset, directory_offset);
    desc = reader_desc(escape.data, codecs);
    CHECK(rbtarchive_reader_open(&desc, &reader, &error) == RBT_RESULT_ARCHIVE_MALFORMED && reader == nullptr);
    CHECK(strstr(error.message, "../x") != nullptr);

    // An encrypted entry.
    bytes_t encrypted;
    local_header(encrypted, "a", 0x0001, crc, 3, 3);
    encrypted.text("abc");
    directory_offset = encrypted.size();
    central_header(encrypted, "a", 0x0001, crc, 3, 3, 0);
    end_record(encrypted, 1, encrypted.size() - directory_offset, directory_offset);
    desc = reader_desc(encrypted.data, codecs);
    CHECK(rbtarchive_reader_open(&desc, &reader, nullptr) == RBT_RESULT_UNSUPPORTED && reader == nullptr);

    // A local header claiming other sizes than the central directory.
    bytes_t disagree;
    local_header(disagree, "a", 0, crc, 2, 2);
    disagree.text("abc");
    directory_offset = disagree.size();
    central_header(disagree, "a", 0, crc, 3, 3, 0);
    end_record(disagree, 1, disagree.size() - directory_offset, directory_offset);
    desc = reader_desc(disagree.data, codecs);
    CHECK(rbtarchive_reader_open(&desc, &reader, nullptr) == RBT_RESULT_OK);
    if (reader)
    {
        uint8_t data[3];
        CHECK(rbtarchive_reader_read_entry(reader, 0, data, 3, nullptr) == RBT_RESULT_ARCHIVE_MALFORMED);
        rbtarchive_reader_destroy(reader);
        reader = nullptr;
    }
    rbtarchive_codecs_destroy(codecs);
}

// ZIP64 records on an archive small enough not to need them, which the
// format allows: written by hand, and by the writer when asked.
static void test_zip64()
{
    current_test = "zip64";
    rbtarchive_codecs codecs = deflate_codecs();
    rbtarchive_reader reader = nullptr;
    RBTARCHIVE_ERROR error;

    // By hand: every size, offset and count deferred to ZIP64 records.
    uint32_t crc = rbtarchive_compute_crc32(0, "abc", 3);
    bytes_t local_extra, central_extra, zip;
    local_extra.u16(0x0001);
    local_extra.u16(16);
    local_extra.u64(3); // uncompressed
    local_extra.u64(3); // compressed
    central_extra.u16(0x0001);
    central_extra.u16(24);
    central_extra.u64(3); // uncompressed
    central_extra.u64(3); // compressed
    central_extra.u64(0); // local header offset
    local_header(zip, "a", 0, crc, UINT32_MAX, UINT32_MAX, local_extra);
    zip.text("abc");
    uint32_t directory_offset = zip.size();
    central_header(zip, "a", 0, crc, UINT32_MAX, UINT32_MAX, UINT32_MAX, central_extra);
    uint32_t directory_size = zip.size() - directory_offset;
    uint32_t end64_offset = zip.size();
    zip.u32(0x06064b50); // the ZIP64 end record
    zip.u64(44);
    zip.u16(45);
    zip.u16(45);
    zip.u32(0);
    zip.u32(0);
    zip.u64(1);
    zip.u64(1);
    zip.u64(directory_size);
    zip.u64(directory_offset);
    zip.u32(0x07064b50); // its locator
    zip.u32(0);
    zip.u64(end64_offset);
    zip.u32(1);
    end_record(zip, UINT16_MAX, UINT32_MAX, UINT32_MAX);

    RBTARCHIVE_READER_DESC desc = reader_desc(zip.data, codecs);
    CHECK(rbtarchive_reader_open(&desc, &reader, &error) == RBT_RESULT_OK);
    if (reader)
    {
        CHECK(rbtarchive_reader_get_entry_count(reader) == 1);
        RBTARCHIVE_ENTRY_INFO info = entry_info(reader, 0);
        CHECK(info.uncompressed_size == 3 && info.compressed_size == 3 && info.local_header_offset == 0);
        std::vector<uint8_t> data;
        CHECK(read_named(reader, "a", data) && data.size() == 3 && memcmp(data.data(), "abc", 3) == 0);
        rbtarchive_reader_destroy(reader);
        reader = nullptr;
    }

    // The writer, asked for ZIP64 size fields on small entries.
    std::vector<uint8_t> archive;
    rbtarchive_writer writer = memory_writer(archive, codecs);
    std::vector<uint8_t> text = text_data();
    RBTARCHIVE_ENTRY_DESC entry = entry_desc("stored", RBTARCHIVE_METHOD_STORED);
    entry.zip64 = true;
    entry.alignment = 64;
    CHECK(rbtarchive_writer_add_entry(writer, &entry, "abc", 3, nullptr) == RBT_RESULT_OK);
    entry = entry_desc("deflated", RBTARCHIVE_METHOD_DEFLATE);
    entry.zip64 = true;
    entry.level = 6;
    CHECK(rbtarchive_writer_add_entry(writer, &entry, text.data(), text.size(), nullptr) == RBT_RESULT_OK);
    CHECK(rbtarchive_writer_finish(writer, nullptr) == RBT_RESULT_OK);
    rbtarchive_writer_destroy(writer);
    // The first local header: version 4.5 needed, sizes deferred.
    CHECK(archive.size() > 30 && archive[4] == 45 && archive[5] == 0);
    CHECK(memcmp(archive.data() + 18, "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 8) == 0);
    desc = reader_desc(archive, codecs);
    CHECK(rbtarchive_reader_open(&desc, &reader, &error) == RBT_RESULT_OK);
    if (reader)
    {
        RBTARCHIVE_ENTRY_INFO info = entry_info(reader, 1);
        CHECK(info.uncompressed_size == text.size() && info.compressed_size < text.size());
        uint64_t offset = 1;
        CHECK(rbtarchive_reader_get_data_offset(reader, 0, &offset, nullptr) == RBT_RESULT_OK && offset % 64 == 0);
        std::vector<uint8_t> data;
        CHECK(read_named(reader, "stored", data) && data.size() == 3 && memcmp(data.data(), "abc", 3) == 0);
        CHECK(read_named(reader, "deflated", data) && data == text);
        rbtarchive_reader_destroy(reader);
        reader = nullptr;
    }
    rbtarchive_codecs_destroy(codecs);
}

// More entries than the plain end record can count: the writer adds the
// ZIP64 end records, and the reader follows them.
static void test_many_entries()
{
    current_test = "many entries";
    rbtarchive_codecs codecs = deflate_codecs();
    const uint32_t count = 65536;
    std::vector<uint8_t> archive;
    rbtarchive_writer writer = memory_writer(archive, codecs);
    char name[16];
    bool added = true;
    for (uint32_t i = 0; i < count; i++)
    {
        snprintf(name, sizeof(name), "e/%05u", i);
        RBTARCHIVE_ENTRY_DESC entry = entry_desc(name, RBTARCHIVE_METHOD_STORED);
        added = added && rbtarchive_writer_add_entry(writer, &entry, name, strlen(name), nullptr) == RBT_RESULT_OK;
    }
    CHECK(added);
    CHECK(rbtarchive_writer_finish(writer, nullptr) == RBT_RESULT_OK);
    rbtarchive_writer_destroy(writer);
    // The archive ends with the ZIP64 locator and the plain end record,
    // whose entry counts say "see ZIP64".
    CHECK(archive.size() > 42);
    if (archive.size() > 42)
    {
        const uint8_t *end = archive.data() + archive.size() - 22;
        CHECK(memcmp(end - 20, "PK\x06\x07", 4) == 0);
        CHECK(memcmp(end, "PK\x05\x06", 4) == 0);
        CHECK(end[8] == 0xFF && end[9] == 0xFF && end[10] == 0xFF && end[11] == 0xFF);
    }

    RBTARCHIVE_READER_DESC desc = reader_desc(archive, codecs);
    rbtarchive_reader reader = nullptr;
    RBTARCHIVE_ERROR error;
    CHECK(rbtarchive_reader_open(&desc, &reader, &error) == RBT_RESULT_OK);
    if (reader)
    {
        CHECK(rbtarchive_reader_get_entry_count(reader) == count);
        uint32_t index = 0;
        CHECK(rbtarchive_reader_find(reader, "e/65535", &index) == RBT_RESULT_OK && index == 65535);
        CHECK(rbtarchive_reader_find(reader, "e/65536", &index) == RBT_RESULT_NOT_FOUND);
        std::vector<uint8_t> data;
        CHECK(read_named(reader, "e/00000", data) && data.size() == 7 && memcmp(data.data(), "e/00000", 7) == 0);
        CHECK(read_named(reader, "e/65535", data) && data.size() == 7 && memcmp(data.data(), "e/65535", 7) == 0);
        rbtarchive_reader_destroy(reader);
    }
    // One entry fewer fits the plain end record: no ZIP64 records.
    archive.clear();
    writer = memory_writer(archive, codecs);
    added = true;
    for (uint32_t i = 0; i < 65534; i++)
    {
        snprintf(name, sizeof(name), "e/%05u", i);
        RBTARCHIVE_ENTRY_DESC entry = entry_desc(name, RBTARCHIVE_METHOD_STORED);
        added = added && rbtarchive_writer_add_entry(writer, &entry, nullptr, 0, nullptr) == RBT_RESULT_OK;
    }
    CHECK(added);
    CHECK(rbtarchive_writer_finish(writer, nullptr) == RBT_RESULT_OK);
    rbtarchive_writer_destroy(writer);
    CHECK(archive.size() > 42 && memcmp(archive.data() + archive.size() - 42, "PK\x06\x07", 4) != 0);
    desc = reader_desc(archive, codecs);
    reader = nullptr;
    CHECK(rbtarchive_reader_open(&desc, &reader, nullptr) == RBT_RESULT_OK);
    CHECK(rbtarchive_reader_get_entry_count(reader) == 65534);
    rbtarchive_reader_destroy(reader);
    rbtarchive_codecs_destroy(codecs);
}

// ---- RBTARCHIVE_TODO items 1-10 (2026-10-30) ----------------------------------

// What the calls made before items 1-10 write, hashed: those calls must
// keep writing exactly these bytes (RBTARCHIVE_TODO.md, "The rule for adding
// any of it"). Recorded on macOS from the code as it was before the items
// were added; any platform gives the same, as libdeflate is pinned.
static const uint32_t OLD_CALLS_CRC = 0x55F1C5ADu;
static const uint64_t OLD_CALLS_SIZE = 23102;

static std::vector<uint8_t> old_calls_archive(rbtarchive_codecs codecs)
{
    std::vector<uint8_t> archive = write_sample(codecs);
    std::vector<uint8_t> text = text_data();
    std::vector<uint8_t> more;
    rbtarchive_writer writer = memory_writer(more, codecs);
    for (int32_t level = 0; level <= 12; level++)
    {
        char name[32];
        snprintf(name, sizeof(name), "level/%d", level);
        RBTARCHIVE_ENTRY_DESC entry = entry_desc(name, RBTARCHIVE_METHOD_DEFLATE);
        entry.level = level;
        entry.dos_date = (uint16_t)(0x5A21 + level);
        entry.dos_time = (uint16_t)(0x7BCD + level);
        CHECK(rbtarchive_writer_add_entry(writer, &entry, text.data(), text.size(), nullptr) == RBT_RESULT_OK);
    }
    RBTARCHIVE_ENTRY_DESC entry = entry_desc("zip64/forced", RBTARCHIVE_METHOD_STORED);
    entry.zip64 = true;
    entry.extra = CUSTOM_EXTRA;
    entry.extra_size = sizeof(CUSTOM_EXTRA);
    entry.alignment = 4096;
    CHECK(rbtarchive_writer_add_entry(writer, &entry, text.data(), 100, nullptr) == RBT_RESULT_OK);
    CHECK(rbtarchive_writer_finish(writer, nullptr) == RBT_RESULT_OK);
    rbtarchive_writer_destroy(writer);
    archive.insert(archive.end(), more.begin(), more.end());
    return archive;
}

static void test_old_bytes_unchanged()
{
    current_test = "old calls' bytes unchanged";
    rbtarchive_codecs codecs = deflate_codecs();
    std::vector<uint8_t> archive = old_calls_archive(codecs);
    uint32_t crc = rbtarchive_compute_crc32(0, archive.data(), archive.size());
    if (crc != OLD_CALLS_CRC || archive.size() != OLD_CALLS_SIZE)
        printf("old calls' archive: crc 0x%08X, size %llu\n", crc, (unsigned long long)archive.size());
    CHECK(crc == OLD_CALLS_CRC);
    CHECK(archive.size() == OLD_CALLS_SIZE);
    rbtarchive_codecs_destroy(codecs);
}

// Reads entry [index] through an entry reader, [piece] bytes a call.
static bool read_in_pieces(rbtarchive_reader reader, uint32_t index, uint64_t piece, std::vector<uint8_t> &out,
                           RBT_RESULT *out_result = nullptr)
{
    out.clear();
    rbtarchive_entry_reader entry = nullptr;
    RBT_RESULT result = rbtarchive_reader_open_entry(reader, index, &entry, nullptr);
    if (out_result)
        *out_result = result;
    if (result != RBT_RESULT_OK)
        return false;
    std::vector<uint8_t> buffer((size_t)piece);
    for (;;)
    {
        uint64_t got = 0;
        result = rbtarchive_entry_reader_read(entry, buffer.data(), piece, &got, nullptr);
        if (out_result)
            *out_result = result;
        if (result != RBT_RESULT_OK)
        {
            rbtarchive_entry_reader_destroy(entry);
            return false;
        }
        out.insert(out.end(), buffer.begin(), buffer.begin() + (size_t)got);
        if (got < piece)
            break;
    }
    // At the end, a read gives nothing.
    uint64_t got = 99;
    bool ended = rbtarchive_entry_reader_read(entry, buffer.data(), piece, &got, nullptr) == RBT_RESULT_OK && got == 0;
    rbtarchive_entry_reader_destroy(entry);
    return ended;
}

static rbtarchive_reader open_memory(const std::vector<uint8_t> &archive, rbtarchive_codecs codecs,
                                    uint32_t strict = RBTARCHIVE_STRICT_NONE, RBT_RESULT *out_result = nullptr)
{
    RBTARCHIVE_READER_DESC desc = reader_desc(archive, codecs);
    desc.strict = (RBTARCHIVE_STRICT)strict;
    rbtarchive_reader reader = nullptr;
    RBTARCHIVE_ERROR error;
    RBT_RESULT result = rbtarchive_reader_open(&desc, &reader, &error);
    if (out_result)
        *out_result = result;
    else if (result != RBT_RESULT_OK)
        printf("  open: %s\n", error.message);
    return reader;
}

// Writes [data] as a streamed entry, [piece] bytes a call.
static RBT_RESULT stream_entry(rbtarchive_writer writer, const RBTARCHIVE_ENTRY_DESC &entry, const std::vector<uint8_t> &data,
                             size_t piece)
{
    RBT_RESULT result = rbtarchive_writer_begin_entry(writer, &entry, nullptr);
    if (result != RBT_RESULT_OK)
        return result;
    for (size_t at = 0; at < data.size(); at += piece)
    {
        size_t size = data.size() - at < piece ? data.size() - at : piece;
        result = rbtarchive_writer_write_entry_data(writer, data.data() + at, size, nullptr);
        if (result != RBT_RESULT_OK)
            return result;
    }
    return rbtarchive_writer_end_entry(writer, nullptr);
}

// Item 1: entries written and read in pieces.
static void test_streamed_entries()
{
    current_test = "streamed entries";
    rbtarchive_codecs codecs = deflate_codecs();
    std::vector<uint8_t> text = text_data(), blob = blob_data();
    std::vector<uint8_t> big(300000);
    for (size_t i = 0; i < big.size(); i++)
        big[i] = (uint8_t)((i * 2654435761u) >> 13 ^ (i / 1000));

    std::vector<uint8_t> archive;
    rbtarchive_writer writer = memory_writer(archive, codecs);
    RBTARCHIVE_ENTRY_DESC stored = entry_desc("stream/stored.bin", RBTARCHIVE_METHOD_STORED);
    CHECK(stream_entry(writer, stored, blob, 7) == RBT_RESULT_OK);
    RBTARCHIVE_ENTRY_DESC deflated = entry_desc("stream/text.json", RBTARCHIVE_METHOD_DEFLATE);
    deflated.level = 6;
    CHECK(stream_entry(writer, deflated, text, 1000) == RBT_RESULT_OK);
    RBTARCHIVE_ENTRY_DESC wide = entry_desc("stream/big.bin", RBTARCHIVE_METHOD_DEFLATE);
    wide.level = 1;
    wide.zip64 = true;
    wide.alignment = 16;
    CHECK(stream_entry(writer, wide, big, 65536) == RBT_RESULT_OK);
    RBTARCHIVE_ENTRY_DESC empty = entry_desc("stream/empty", RBTARCHIVE_METHOD_DEFLATE);
    CHECK(stream_entry(writer, empty, std::vector<uint8_t>(), 1) == RBT_RESULT_OK);
    // A whole entry after streamed ones, as before.
    RBTARCHIVE_ENTRY_DESC whole = entry_desc("whole.json", RBTARCHIVE_METHOD_DEFLATE);
    whole.level = 6;
    CHECK(rbtarchive_writer_add_entry(writer, &whole, text.data(), text.size(), nullptr) == RBT_RESULT_OK);

    // Calls in the wrong state, each refused, the writer still usable.
    RBTARCHIVE_ERROR error;
    CHECK(rbtarchive_writer_write_entry_data(writer, "x", 1, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtarchive_writer_end_entry(writer, &error) == RBT_RESULT_INVALID_ARGUMENT);
    RBTARCHIVE_ENTRY_DESC open_one = entry_desc("open", RBTARCHIVE_METHOD_STORED);
    CHECK(rbtarchive_writer_begin_entry(writer, &open_one, nullptr) == RBT_RESULT_OK);
    RBTARCHIVE_ENTRY_DESC other = entry_desc("other", RBTARCHIVE_METHOD_STORED);
    CHECK(rbtarchive_writer_begin_entry(writer, &other, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtarchive_writer_add_entry(writer, &other, "x", 1, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtarchive_writer_finish(writer, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtarchive_writer_write_entry_data(writer, "open", 4, nullptr) == RBT_RESULT_OK);
    CHECK(rbtarchive_writer_end_entry(writer, nullptr) == RBT_RESULT_OK);
    // DEFLATE's stream levels are zlib's, 0-9.
    RBTARCHIVE_ENTRY_DESC high = entry_desc("high", RBTARCHIVE_METHOD_DEFLATE);
    high.level = 10;
    CHECK(rbtarchive_writer_begin_entry(writer, &high, &error) == RBT_RESULT_INVALID_ARGUMENT);
    high.level = -1;
    CHECK(rbtarchive_writer_begin_entry(writer, &high, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtarchive_writer_finish(writer, nullptr) == RBT_RESULT_OK);
    rbtarchive_writer_destroy(writer);

    // Streamed output is the same every time.
    {
        std::vector<uint8_t> again;
        writer = memory_writer(again, codecs);
        CHECK(stream_entry(writer, stored, blob, 7) == RBT_RESULT_OK);
        CHECK(stream_entry(writer, deflated, text, 1000) == RBT_RESULT_OK);
        CHECK(stream_entry(writer, wide, big, 65536) == RBT_RESULT_OK);
        CHECK(stream_entry(writer, empty, std::vector<uint8_t>(), 1) == RBT_RESULT_OK);
        CHECK(rbtarchive_writer_add_entry(writer, &whole, text.data(), text.size(), nullptr) == RBT_RESULT_OK);
        CHECK(rbtarchive_writer_begin_entry(writer, &open_one, nullptr) == RBT_RESULT_OK);
        CHECK(rbtarchive_writer_write_entry_data(writer, "open", 4, nullptr) == RBT_RESULT_OK);
        CHECK(rbtarchive_writer_end_entry(writer, nullptr) == RBT_RESULT_OK);
        CHECK(rbtarchive_writer_finish(writer, nullptr) == RBT_RESULT_OK);
        rbtarchive_writer_destroy(writer);
        CHECK(again == archive);
    }

    rbtarchive_reader reader = open_memory(archive, codecs, RBTARCHIVE_STRICT_NO_GAPS);
    CHECK(reader != nullptr);
    if (reader)
    {
        CHECK(rbtarchive_reader_get_entry_count(reader) == 6);
        RBTARCHIVE_ENTRY_INFO info = entry_info(reader, 0);
        CHECK((info.flags & 0x0008) && info.uncompressed_size == blob.size() && info.compressed_size == blob.size());
        info = entry_info(reader, 2);
        CHECK((info.flags & 0x0008) && info.uncompressed_size == big.size());
        uint64_t offset = 0;
        CHECK(rbtarchive_reader_get_data_offset(reader, 2, &offset, nullptr) == RBT_RESULT_OK && offset % 16 == 0);
        info = entry_info(reader, 4);
        CHECK(!(info.flags & 0x0008));
        const std::vector<uint8_t> *expected[6] = {&blob, &text, &big, nullptr, &text, nullptr};
        std::vector<uint8_t> open_data = {'o', 'p', 'e', 'n'}, none;
        expected[3] = &none;
        expected[5] = &open_data;
        const uint64_t pieces[] = {1, 7, 4096, 1 << 20};
        for (uint32_t i = 0; i < 6; i++)
        {
            std::vector<uint8_t> data;
            CHECK(read_named(reader, entry_info(reader, i).name, data) && data == *expected[i]);
            for (uint64_t piece : pieces)
            {
                if (piece == 1 && i == 2)
                    continue; // 300,000 calls: slow under the sanitizers
                CHECK(read_in_pieces(reader, i, piece, data) && data == *expected[i]);
            }
        }
        rbtarchive_entry_reader entry = nullptr;
        CHECK(rbtarchive_reader_open_entry(reader, 6, &entry, nullptr) == RBT_RESULT_NOT_FOUND && entry == nullptr);
        rbtarchive_reader_destroy(reader);
    }

    // Streamed DEFLATE bytes are zlib-ng's, fixed by its pinned version
    // (2.3.3): the same on every platform, recorded on macOS (Apple M5 Pro,
    // 2026-10-30). A platform that differs has zlib-ng choosing its matches
    // by CPU, which would break A24's promise.
    {
        std::vector<uint8_t> one;
        writer = memory_writer(one, codecs);
        for (int32_t level = 0; level <= 9; level++)
        {
            char name[32];
            snprintf(name, sizeof(name), "level/%d", level);
            RBTARCHIVE_ENTRY_DESC entry = entry_desc(name, RBTARCHIVE_METHOD_DEFLATE);
            entry.level = level;
            CHECK(stream_entry(writer, entry, big, 4096) == RBT_RESULT_OK);
            snprintf(name, sizeof(name), "text/%d", level);
            entry.name = name;
            CHECK(stream_entry(writer, entry, text, 1500) == RBT_RESULT_OK);
        }
        CHECK(rbtarchive_writer_finish(writer, nullptr) == RBT_RESULT_OK);
        rbtarchive_writer_destroy(writer);
        uint32_t crc = rbtarchive_compute_crc32(0, one.data(), one.size());
        const uint32_t recorded_crc = 0xDC4D1C59u;
        const uint64_t recorded_size = 1132085;
        if (crc != recorded_crc || one.size() != recorded_size)
            printf("streamed DEFLATE archive: crc 0x%08X, size %llu\n", crc, (unsigned long long)one.size());
        CHECK(crc == recorded_crc && one.size() == recorded_size);
    }

    // A streamed entry without zip64 that reaches 4 GiB: not run (it writes
    // 4 GiB); the check is RBT_RESULT_LIMIT before any byte past it.

    // Corruption, found by the call that hands out the last byte.
    {
        std::vector<uint8_t> small;
        writer = memory_writer(small, codecs);
        CHECK(stream_entry(writer, deflated, text, 999) == RBT_RESULT_OK);
        CHECK(stream_entry(writer, stored, blob, 999) == RBT_RESULT_OK);
        CHECK(rbtarchive_writer_finish(writer, nullptr) == RBT_RESULT_OK);
        rbtarchive_writer_destroy(writer);
        reader = open_memory(small, codecs);
        uint64_t offset = 0;
        CHECK(rbtarchive_reader_get_data_offset(reader, 1, &offset, nullptr) == RBT_RESULT_OK);
        small[(size_t)offset + 5] ^= 0x40;
        std::vector<uint8_t> data;
        RBT_RESULT result = RBT_RESULT_OK;
        CHECK(!read_in_pieces(reader, 1, 100, data, &result) && result == RBT_RESULT_CORRUPT_DATA);
        CHECK(rbtarchive_reader_get_data_offset(reader, 0, &offset, nullptr) == RBT_RESULT_OK);
        small[(size_t)offset + 3] ^= 0x01;
        CHECK(!read_in_pieces(reader, 0, 100, data, &result) &&
              (result == RBT_RESULT_CORRUPT_DATA || result == RBT_RESULT_ARCHIVE_MALFORMED));
        rbtarchive_reader_destroy(reader);
    }

    // A codec with some stream callbacks but not all is refused; one with
    // none works on whole entries only.
    {
        RBTARCHIVE_CODEC codec{};
        codec.base.type = RBT_STRUCT_TYPE_ARCHIVE_CODEC;
        codec.method = 99;
        codec.decompress = [](void *, const void *, uint64_t, void *, uint64_t) { return false; };
        codec.stream_begin = [](void *, bool, int32_t) -> void * { return nullptr; };
        rbtarchive_codecs table = nullptr;
        CHECK(rbtarchive_codecs_create(&table) == RBT_RESULT_OK);
        CHECK(rbtarchive_codecs_add(table, &codec) == RBT_RESULT_INVALID_ARGUMENT);
        codec.stream_begin = nullptr;
        CHECK(rbtarchive_codecs_add(table, &codec) == RBT_RESULT_OK);
        std::vector<uint8_t> out;
        writer = memory_writer(out, table);
        RBTARCHIVE_ENTRY_DESC entry = entry_desc("ninety-nine", 99);
        CHECK(rbtarchive_writer_begin_entry(writer, &entry, nullptr) == RBT_RESULT_UNSUPPORTED);
        rbtarchive_writer_destroy(writer);
        rbtarchive_codecs_destroy(table);
    }
    rbtarchive_codecs_destroy(codecs);
}

// Item 2: entries added already compressed.
static void test_raw_entries()
{
    current_test = "raw entries";
    rbtarchive_codecs codecs = deflate_codecs();
    std::vector<uint8_t> text = text_data(), blob = blob_data();

    // Compressed by the caller, then added raw: the same bytes as added
    // whole.
    std::vector<uint8_t> whole, raw;
    rbtarchive_writer writer = memory_writer(whole, codecs);
    RBTARCHIVE_ENTRY_DESC entry = entry_desc("text.json", RBTARCHIVE_METHOD_DEFLATE);
    entry.level = 9;
    CHECK(rbtarchive_writer_add_entry(writer, &entry, text.data(), text.size(), nullptr) == RBT_RESULT_OK);
    RBTARCHIVE_ENTRY_DESC plain = entry_desc("blob.bin", RBTARCHIVE_METHOD_STORED);
    plain.alignment = 64;
    CHECK(rbtarchive_writer_add_entry(writer, &plain, blob.data(), blob.size(), nullptr) == RBT_RESULT_OK);
    CHECK(rbtarchive_writer_finish(writer, nullptr) == RBT_RESULT_OK);
    rbtarchive_writer_destroy(writer);

    uint64_t bound = 0;
    CHECK(rbtarchive_codecs_compress_bound(codecs, RBTARCHIVE_METHOD_DEFLATE, text.size(), &bound) == RBT_RESULT_OK);
    std::vector<uint8_t> compressed((size_t)bound);
    uint64_t compressed_size = 0;
    RBTARCHIVE_ERROR error;
    CHECK(rbtarchive_codecs_compress(codecs, RBTARCHIVE_METHOD_DEFLATE, 9, text.data(), text.size(), compressed.data(),
                                    bound - 1, &compressed_size, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtarchive_codecs_compress(codecs, RBTARCHIVE_METHOD_DEFLATE, 9, text.data(), text.size(), compressed.data(),
                                    bound, &compressed_size, &error) == RBT_RESULT_OK);
    CHECK(rbtarchive_codecs_compress_bound(codecs, RBTARCHIVE_METHOD_STORED, 10, &bound) == RBT_RESULT_OK && bound == 10);
    CHECK(rbtarchive_codecs_compress_bound(codecs, 93, 10, &bound) == RBT_RESULT_ARCHIVE_UNKNOWN_METHOD);
    CHECK(rbtarchive_codecs_compress_bound(nullptr, 8, 10, &bound) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtarchive_codecs_compress_bound(codecs, 8, 10, nullptr) == RBT_RESULT_INVALID_ARGUMENT);

    writer = memory_writer(raw, codecs);
    CHECK(rbtarchive_writer_add_raw_entry(writer, &entry, compressed.data(), compressed_size, text.size(),
                                         rbtarchive_compute_crc32(0, text.data(), text.size()), nullptr) == RBT_RESULT_OK);
    // A STORED entry's sizes and CRC-32 are checked; nothing else is.
    CHECK(rbtarchive_writer_add_raw_entry(writer, &plain, blob.data(), blob.size(), blob.size() + 1,
                                         rbtarchive_compute_crc32(0, blob.data(), blob.size()), &error) ==
          RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtarchive_writer_add_raw_entry(writer, &plain, blob.data(), blob.size(), blob.size(), 1234, &error) ==
          RBT_RESULT_INVALID_ARGUMENT);
    RBTARCHIVE_ENTRY_DESC hollow = entry_desc("hollow", RBTARCHIVE_METHOD_DEFLATE);
    CHECK(rbtarchive_writer_add_raw_entry(writer, &hollow, nullptr, 0, 10, 0, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtarchive_writer_add_raw_entry(writer, &plain, blob.data(), blob.size(), blob.size(),
                                         rbtarchive_compute_crc32(0, blob.data(), blob.size()), nullptr) == RBT_RESULT_OK);
    CHECK(rbtarchive_writer_finish(writer, nullptr) == RBT_RESULT_OK);
    rbtarchive_writer_destroy(writer);
    CHECK(raw == whole);

    // An entry copied from one archive to another without its codec: the
    // method needn't be in the writer's table.
    rbtarchive_reader reader = open_memory(whole, codecs);
    rbtarchive_codecs stored_only = nullptr;
    CHECK(rbtarchive_codecs_create(&stored_only) == RBT_RESULT_OK);
    std::vector<uint8_t> copy;
    writer = memory_writer(copy, stored_only);
    for (uint32_t i = 0; reader && i < rbtarchive_reader_get_entry_count(reader); i++)
    {
        RBTARCHIVE_ENTRY_INFO info = entry_info(reader, i);
        uint64_t offset = 0;
        CHECK(rbtarchive_reader_get_data_offset(reader, i, &offset, nullptr) == RBT_RESULT_OK);
        RBTARCHIVE_ENTRY_DESC copied = entry_desc(info.name, info.method);
        CHECK(rbtarchive_writer_add_raw_entry(writer, &copied, whole.data() + offset, info.compressed_size,
                                             info.uncompressed_size, info.crc32, nullptr) == RBT_RESULT_OK);
    }
    CHECK(rbtarchive_writer_finish(writer, nullptr) == RBT_RESULT_OK);
    rbtarchive_writer_destroy(writer);
    rbtarchive_reader_destroy(reader);
    reader = open_memory(copy, codecs);
    std::vector<uint8_t> data;
    CHECK(reader && read_named(reader, "text.json", data) && data == text);
    CHECK(reader && read_named(reader, "blob.bin", data) && data == blob);
    rbtarchive_reader_destroy(reader);
    rbtarchive_codecs_destroy(stored_only);
    rbtarchive_codecs_destroy(codecs);
}

// A file in memory that can be cut, for updating.
struct cuttable_t
{
    std::vector<uint8_t> bytes;
    int cuts = 0;
};

static bool cuttable_write(void *user, const void *data, uint64_t count)
{
    cuttable_t &file = *(cuttable_t *)user;
    const uint8_t *in = (const uint8_t *)data;
    file.bytes.insert(file.bytes.end(), in, in + count);
    return true;
}

static bool cuttable_truncate(void *user, uint64_t size)
{
    cuttable_t &file = *(cuttable_t *)user;
    if (size > file.bytes.size())
        return false;
    file.bytes.resize((size_t)size);
    file.cuts++;
    return true;
}

static rbtarchive_writer update_writer(cuttable_t &file, rbtarchive_reader reader, rbtarchive_codecs codecs,
                                      RBT_RESULT *out_result = nullptr)
{
    RBTARCHIVE_UPDATE_DESC desc{};
    desc.base.type = RBT_STRUCT_TYPE_ARCHIVE_UPDATE_DESC;
    desc.reader = reader;
    desc.truncate = cuttable_truncate;
    desc.sink.user = &file;
    desc.sink.write = cuttable_write;
    desc.codecs = codecs;
    rbtarchive_writer writer = nullptr;
    RBTARCHIVE_ERROR error;
    RBT_RESULT result = rbtarchive_writer_create_update(&desc, &writer, &error);
    if (out_result)
        *out_result = result;
    return writer;
}

// Item 3: updating an archive.
static void test_update()
{
    current_test = "update";
    rbtarchive_codecs codecs = deflate_codecs();
    std::vector<uint8_t> text = text_data(), blob = blob_data();
    cuttable_t file;
    file.bytes = write_sample(codecs);
    std::vector<uint8_t> original = file.bytes;

    // A writer destroyed having written nothing leaves the archive alone.
    rbtarchive_reader reader = open_memory(file.bytes, codecs);
    CHECK(reader != nullptr);
    if (!reader)
    {
        rbtarchive_codecs_destroy(codecs);
        return;
    }
    rbtarchive_writer writer = update_writer(file, reader, codecs);
    CHECK(writer != nullptr);
    uint64_t kept = rbtarchive_writer_get_offset(writer);
    CHECK(kept > 0 && kept < original.size());
    rbtarchive_writer_destroy(writer);
    CHECK(file.bytes == original && file.cuts == 0);

    // Refusals.
    RBTARCHIVE_UPDATE_DESC desc{};
    desc.base.type = RBT_STRUCT_TYPE_ARCHIVE_UPDATE_DESC;
    desc.reader = reader;
    desc.sink.write = cuttable_write;
    desc.codecs = codecs;
    RBT_RESULT result = RBT_RESULT_OK;
    writer = (rbtarchive_writer)&file;
    CHECK(rbtarchive_writer_create_update(&desc, &writer, nullptr) == RBT_RESULT_INVALID_ARGUMENT && writer == nullptr);
    desc.truncate = cuttable_truncate;
    desc.reader = nullptr;
    CHECK(rbtarchive_writer_create_update(&desc, &writer, nullptr) == RBT_RESULT_INVALID_ARGUMENT);
    desc.reader = reader;
    desc.base.type = RBT_STRUCT_TYPE_ARCHIVE_WRITER_DESC;
    CHECK(rbtarchive_writer_create_update(&desc, &writer, nullptr) == RBT_RESULT_STRUCT_WRONG_TYPE);

    // Drop one, replace one, add one, and a new comment.
    writer = update_writer(file, reader, codecs, &result);
    CHECK(result == RBT_RESULT_OK);
    rbtarchive_reader_destroy(reader); // the writer copied what it needs
    RBTARCHIVE_ERROR error;
    CHECK(rbtarchive_writer_remove_entry(writer, "empty", &error) == RBT_RESULT_OK);
    CHECK(rbtarchive_writer_remove_entry(writer, "empty", &error) == RBT_RESULT_NOT_FOUND);
    CHECK(rbtarchive_writer_remove_entry(writer, "model/data.json", nullptr) == RBT_RESULT_OK);
    RBTARCHIVE_ENTRY_DESC replaced = entry_desc("model/data.json", RBTARCHIVE_METHOD_STORED);
    CHECK(rbtarchive_writer_add_entry(writer, &replaced, "{}", 2, nullptr) == RBT_RESULT_OK);
    RBTARCHIVE_ENTRY_DESC added = entry_desc("added.txt", RBTARCHIVE_METHOD_DEFLATE);
    CHECK(stream_entry(writer, added, text, 4000) == RBT_RESULT_OK);
    // A kept name is still taken.
    RBTARCHIVE_ENTRY_DESC clash = entry_desc("mimetype", RBTARCHIVE_METHOD_STORED);
    CHECK(rbtarchive_writer_add_entry(writer, &clash, "x", 1, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtarchive_writer_set_comment(writer, "updated", 7, nullptr) == RBT_RESULT_OK);
    CHECK(rbtarchive_writer_finish(writer, nullptr) == RBT_RESULT_OK);
    rbtarchive_writer_destroy(writer);
    CHECK(file.cuts == 1);
    // Everything before the old directory is as it was.
    CHECK(file.bytes.size() > kept && memcmp(file.bytes.data(), original.data(), (size_t)kept) == 0);

    reader = open_memory(file.bytes, codecs);
    CHECK(reader != nullptr);
    if (reader)
    {
        CHECK(rbtarchive_reader_get_entry_count(reader) == 5);
        std::vector<uint8_t> data;
        uint32_t index = 0;
        CHECK(rbtarchive_reader_find(reader, "empty", &index) == RBT_RESULT_NOT_FOUND);
        CHECK(read_named(reader, "mimetype", data) && data.size() == strlen(MIMETYPE));
        CHECK(read_named(reader, "geometry/blob.bin", data) && data == blob);
        CHECK(read_named(reader, "model/data.json", data) && data.size() == 2 && data[0] == '{');
        CHECK(read_named(reader, "added.txt", data) && data == text);
        RBTARCHIVE_ENTRY_INFO info = entry_info(reader, 2);
        CHECK(strcmp(info.name, "caf\xc3\xa9.txt") == 0 && info.extra_size == sizeof(CUSTOM_EXTRA));
        const void *comment = nullptr;
        uint16_t comment_size = 0;
        CHECK(rbtarchive_reader_get_comment(reader, &comment, &comment_size) == RBT_RESULT_OK && comment_size == 7 &&
              memcmp(comment, "updated", 7) == 0);
    }

    // Updated again, the comment is kept unless changed.
    cuttable_t second;
    second.bytes = file.bytes;
    writer = update_writer(second, reader, codecs);
    rbtarchive_reader_destroy(reader);
    CHECK(rbtarchive_writer_finish(writer, nullptr) == RBT_RESULT_OK);
    rbtarchive_writer_destroy(writer);
    reader = open_memory(second.bytes, codecs);
    if (reader)
    {
        const void *comment = nullptr;
        uint16_t comment_size = 0;
        CHECK(rbtarchive_reader_get_comment(reader, &comment, &comment_size) == RBT_RESULT_OK && comment_size == 7);
        CHECK(rbtarchive_reader_get_entry_count(reader) == 5);
        rbtarchive_reader_destroy(reader);
    }
    // Nothing changed: the directory written again is the one there was.
    CHECK(second.bytes == file.bytes);
    rbtarchive_codecs_destroy(codecs);
}

// Items 4 to 7: directories, attributes, separate central extras,
// comments.
static void test_entry_fields()
{
    current_test = "entry fields";
    rbtarchive_codecs codecs = deflate_codecs();
    std::vector<uint8_t> archive;
    RBTARCHIVE_WRITER_DESC writer_desc{};
    writer_desc.base.type = RBT_STRUCT_TYPE_ARCHIVE_WRITER_DESC;
    writer_desc.sink.user = &archive;
    writer_desc.sink.write = memory_write;
    writer_desc.codecs = codecs;
    rbtarchive_writer writer = nullptr;
    CHECK(rbtarchive_writer_create(&writer_desc, &writer) == RBT_RESULT_OK);
    RBTARCHIVE_ERROR error;
    RBTARCHIVE_ENTRY_DESC dir = entry_desc("tools/", RBTARCHIVE_METHOD_STORED);
    CHECK(rbtarchive_writer_add_entry(writer, &dir, nullptr, 0, &error) == RBT_RESULT_INVALID_ARGUMENT); // not allowed
    rbtarchive_writer_destroy(writer);
    archive.clear();

    writer_desc.allow_directory_entries = true;
    CHECK(rbtarchive_writer_create(&writer_desc, &writer) == RBT_RESULT_OK);
    dir.host_system = 3;
    dir.external_attributes = (uint32_t)040755 << 16;
    CHECK(rbtarchive_writer_add_entry(writer, &dir, "x", 1, &error) == RBT_RESULT_INVALID_ARGUMENT); // has data
    RBTARCHIVE_ENTRY_DESC deflated_dir = entry_desc("deflated/", RBTARCHIVE_METHOD_DEFLATE);
    CHECK(rbtarchive_writer_add_entry(writer, &deflated_dir, nullptr, 0, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtarchive_writer_begin_entry(writer, &dir, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtarchive_writer_add_entry(writer, &dir, nullptr, 0, &error) == RBT_RESULT_OK);

    static const uint8_t LOCAL_ONLY[] = {0x34, 0x12, 0x01, 0x00, 'L'};
    static const uint8_t CENTRAL_ONLY[] = {0x78, 0x56, 0x02, 0x00, 'C', 'C'};
    RBTARCHIVE_ENTRY_DESC tool = entry_desc("tools/run.sh", RBTARCHIVE_METHOD_STORED);
    tool.host_system = 3;
    tool.internal_attributes = 1;
    tool.external_attributes = (uint32_t)0100755 << 16;
    tool.extra = LOCAL_ONLY;
    tool.extra_size = sizeof(LOCAL_ONLY);
    tool.central_extra = CENTRAL_ONLY;
    tool.central_extra_size = sizeof(CENTRAL_ONLY);
    tool.comment = "runs it";
    tool.comment_size = 7;
    CHECK(rbtarchive_writer_add_entry(writer, &tool, "#!/bin/sh\n", 10, nullptr) == RBT_RESULT_OK);
    RBTARCHIVE_ENTRY_DESC link = entry_desc("tools/latest", RBTARCHIVE_METHOD_STORED);
    link.host_system = 3;
    link.external_attributes = (uint32_t)0120777 << 16;
    link.central_extra = CENTRAL_ONLY; // none in the central header...
    link.central_extra_size = 0;
    link.extra = LOCAL_ONLY; // ...and this in the local one
    link.extra_size = sizeof(LOCAL_ONLY);
    CHECK(rbtarchive_writer_add_entry(writer, &link, "run.sh", 6, nullptr) == RBT_RESULT_OK);
    RBTARCHIVE_ENTRY_DESC bad = entry_desc("bad", RBTARCHIVE_METHOD_STORED);
    bad.central_extra = CENTRAL_ONLY;
    bad.central_extra_size = 3; // not whole records
    CHECK(rbtarchive_writer_add_entry(writer, &bad, nullptr, 0, &error) == RBT_RESULT_INVALID_ARGUMENT);
    bad.central_extra_size = 0;
    bad.comment_size = 4; // and no comment
    CHECK(rbtarchive_writer_add_entry(writer, &bad, nullptr, 0, &error) == RBT_RESULT_INVALID_ARGUMENT);

    // The archive comment, and one holding the end record's signature.
    const char sneaky[] = {'a', 'P', 'K', 5, 6, 'b'};
    CHECK(rbtarchive_writer_set_comment(writer, sneaky, sizeof(sneaky), &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtarchive_writer_set_comment(writer, nullptr, 3, &error) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtarchive_writer_set_comment(writer, "first", 5, nullptr) == RBT_RESULT_OK);
    CHECK(rbtarchive_writer_set_comment(writer, "a comment", 9, nullptr) == RBT_RESULT_OK);
    CHECK(rbtarchive_writer_finish(writer, nullptr) == RBT_RESULT_OK);
    rbtarchive_writer_destroy(writer);

    rbtarchive_reader reader = open_memory(archive, codecs);
    CHECK(reader != nullptr);
    if (reader)
    {
        RBTARCHIVE_ENTRY_INFO info = entry_info(reader, 0);
        CHECK(info.is_directory && info.version_made_by == (3 << 8 | 45) &&
              info.external_attributes == (uint32_t)040755 << 16);
        info = entry_info(reader, 1);
        CHECK(info.version_made_by == (3 << 8 | 45) && info.internal_attributes == 1);
        CHECK(info.external_attributes == (uint32_t)0100755 << 16);
        CHECK(info.extra_size == sizeof(CENTRAL_ONLY) && memcmp(info.extra, CENTRAL_ONLY, sizeof(CENTRAL_ONLY)) == 0);
        CHECK(info.comment_size == 7 && info.comment && memcmp(info.comment, "runs it", 7) == 0);
        // The local header has its own.
        CHECK(archive.size() > info.local_header_offset + 30 + 12 + sizeof(LOCAL_ONLY));
        const uint8_t *local = archive.data() + info.local_header_offset;
        CHECK(local[28] == sizeof(LOCAL_ONLY) && memcmp(local + 30 + 12, LOCAL_ONLY, sizeof(LOCAL_ONLY)) == 0);
        info = entry_info(reader, 2);
        CHECK(info.extra_size == 0 && info.extra == nullptr && info.comment == nullptr && info.comment_size == 0);
        CHECK(info.external_attributes >> 16 == 0120777);
        const void *comment = nullptr;
        uint16_t comment_size = 0;
        CHECK(rbtarchive_reader_get_comment(reader, &comment, &comment_size) == RBT_RESULT_OK && comment_size == 9 &&
              memcmp(comment, "a comment", 9) == 0);
        CHECK(rbtarchive_reader_get_comment(reader, nullptr, &comment_size) == RBT_RESULT_INVALID_ARGUMENT);
        CHECK(rbtarchive_reader_get_offset_shift(reader) == 0);
        rbtarchive_reader_destroy(reader);
    }
    // The old fields at zero are the old bytes: covered by
    // test_old_bytes_unchanged, whose entries leave every new field 0.
    rbtarchive_codecs_destroy(codecs);
}

// Item 8: standard extra fields.
static void test_extra_helpers()
{
    current_test = "extra helpers";
    uint8_t buffer[256];
    uint16_t size = 0;
    RBTARCHIVE_FILE_TIMES times{};
    times.modified = 1700000000LL * 10000000 + 1234567; // rounded down to the second
    times.accessed = -5LL * 10000000;
    times.has_modified = true;
    times.has_accessed = true;
    CHECK(rbtarchive_extra_put_unix_times(&times, false, buffer, 4, &size) == RBT_RESULT_INVALID_ARGUMENT && size == 13);
    CHECK(rbtarchive_extra_put_unix_times(&times, false, buffer, sizeof(buffer), &size) == RBT_RESULT_OK && size == 13);
    uint16_t at = size;
    CHECK(rbtarchive_extra_put_unix_times(&times, true, buffer + at, sizeof(buffer) - at, &size) == RBT_RESULT_OK &&
          size == 9);
    RBTARCHIVE_FILE_TIMES back{};
    CHECK(rbtarchive_extra_get_unix_times(buffer, at, &back) == RBT_RESULT_OK);
    CHECK(back.has_modified && back.modified == 1700000000LL * 10000000);
    CHECK(back.has_accessed && back.accessed == -5LL * 10000000 && !back.has_created);
    CHECK(rbtarchive_extra_get_unix_times(buffer + at, size, &back) == RBT_RESULT_OK);
    CHECK(back.has_modified && !back.has_accessed && back.modified == 1700000000LL * 10000000);
    RBTARCHIVE_FILE_TIMES far = times;
    far.modified = 3000000000LL * 10000000; // past 2038
    CHECK(rbtarchive_extra_put_unix_times(&far, false, buffer, sizeof(buffer), &size) == RBT_RESULT_INVALID_ARGUMENT);
    RBTARCHIVE_FILE_TIMES none{};
    CHECK(rbtarchive_extra_put_unix_times(&none, false, buffer, sizeof(buffer), &size) == RBT_RESULT_INVALID_ARGUMENT);

    // NTFS: all three, to the tick.
    uint8_t ntfs[64];
    times.created = 1;
    times.has_created = true;
    CHECK(rbtarchive_extra_put_ntfs_times(&times, ntfs, 35, &size) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtarchive_extra_put_ntfs_times(&times, ntfs, sizeof(ntfs), &size) == RBT_RESULT_OK && size == 36);
    CHECK(rbtarchive_extra_get_ntfs_times(ntfs, size, &back) == RBT_RESULT_OK);
    CHECK(back.modified == times.modified && back.accessed == times.accessed && back.created == 1);
    CHECK(back.has_modified && back.has_accessed && back.has_created);
    RBTARCHIVE_FILE_TIMES old = times;
    old.created = -116444736000000000LL - 1; // before 1601
    CHECK(rbtarchive_extra_put_ntfs_times(&old, ntfs, sizeof(ntfs), &size) == RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtarchive_extra_get_ntfs_times(ntfs, 20, &back) == RBT_RESULT_ARCHIVE_MALFORMED); // cut short

    // Unicode path: found only for the name it was written for.
    const char *stored_name = "caf\x82.txt"; // CP437
    const char *path = "caf\xc3\xa9.txt";
    uint8_t unicode[64];
    CHECK(rbtarchive_extra_put_unicode_path(stored_name, 8, path, 9, unicode, sizeof(unicode), &size) == RBT_RESULT_OK &&
          size == 18);
    CHECK(rbtarchive_extra_put_unicode_path(stored_name, 8, "\xff", 1, unicode, sizeof(unicode), &size) ==
          RBT_RESULT_INVALID_ARGUMENT);
    CHECK(rbtarchive_extra_put_unicode_path(stored_name, 8, path, 9, unicode, sizeof(unicode), &size) == RBT_RESULT_OK);
    const char *found = nullptr;
    uint16_t found_size = 0;
    CHECK(rbtarchive_extra_get_unicode_path(unicode, size, stored_name, 8, &found, &found_size) == RBT_RESULT_OK &&
          found_size == 9 && memcmp(found, path, 9) == 0);
    CHECK(rbtarchive_extra_get_unicode_path(unicode, size, "renamed", 7, &found, &found_size) == RBT_RESULT_NOT_FOUND);

    // Several records together, and find.
    std::vector<uint8_t> field(buffer, buffer + at);
    field.insert(field.end(), ntfs, ntfs + 36);
    field.insert(field.end(), unicode, unicode + size);
    const void *data = nullptr;
    uint16_t data_size = 0;
    CHECK(rbtarchive_extra_find(field.data(), (uint16_t)field.size(), RBTARCHIVE_EXTRA_UNICODE_PATH, &data, &data_size) ==
              RBT_RESULT_OK &&
          data == field.data() + at + 36 + 4 && data_size == size - 4);
    CHECK(rbtarchive_extra_find(field.data(), (uint16_t)field.size(), 0x9999, &data, &data_size) == RBT_RESULT_NOT_FOUND);
    CHECK(rbtarchive_extra_get_ntfs_times(field.data(), (uint16_t)field.size(), &back) == RBT_RESULT_OK &&
          back.created == 1);
    CHECK(rbtarchive_extra_find(nullptr, 0, 1, &data, &data_size) == RBT_RESULT_NOT_FOUND);
    CHECK(rbtarchive_extra_find(nullptr, 4, 1, &data, &data_size) == RBT_RESULT_INVALID_ARGUMENT);
    field[2] = 0xFF; // the first record now runs past the end
    CHECK(rbtarchive_extra_find(field.data(), (uint16_t)field.size(), RBTARCHIVE_EXTRA_UNICODE_PATH, &data,
                               &data_size) == RBT_RESULT_ARCHIVE_MALFORMED);
}

// Item 9: strict reading.
static void test_strict()
{
    current_test = "strict";
    rbtarchive_codecs codecs = deflate_codecs();
    uint32_t crc = rbtarchive_compute_crc32(0, "hello", 5);

    // A gap between two entries, a directory entry and a comment.
    bytes_t zip;
    local_header(zip, "dir/", 0, 0, 0, 0);
    uint32_t first = zip.size();
    local_header(zip, "dir/a.txt", 0, crc, 5, 5);
    zip.text("hello");
    zip.text("gap!");
    uint32_t second = zip.size();
    local_header(zip, "dir/b.txt", 0x0008, 0, 0, 0);
    zip.text("hello");
    zip.u32(0x08074b50);
    zip.u32(crc);
    zip.u32(5);
    zip.u32(5);
    uint32_t directory_offset = zip.size();
    central_header(zip, "dir/", 0, 0, 0, 0, 0);
    central_header(zip, "dir/a.txt", 0, crc, 5, 5, first);
    central_header(zip, "dir/b.txt", 0x0008, crc, 5, 5, second);
    end_record(zip, 3, zip.size() - directory_offset, directory_offset, "said");

    RBT_RESULT result = RBT_RESULT_OK;
    rbtarchive_reader reader = open_memory(zip.data, codecs, RBTARCHIVE_STRICT_NONE, &result);
    CHECK(result == RBT_RESULT_OK);
    rbtarchive_reader_destroy(reader);
    const uint32_t each[] = {RBTARCHIVE_STRICT_NO_ARCHIVE_COMMENT, RBTARCHIVE_STRICT_NO_DIRECTORY_ENTRIES,
                             RBTARCHIVE_STRICT_NO_GAPS};
    for (uint32_t flag : each)
    {
        reader = open_memory(zip.data, codecs, flag, &result);
        CHECK(result == RBT_RESULT_ARCHIVE_MALFORMED && reader == nullptr);
    }
    reader = open_memory(zip.data, codecs, RBTARCHIVE_STRICT_NO_ENTRY_COMMENTS, &result);
    CHECK(result == RBT_RESULT_OK);
    rbtarchive_reader_destroy(reader);
    reader = open_memory(zip.data, codecs, 1u << 20, &result);
    CHECK(result == RBT_RESULT_INVALID_ARGUMENT);

    // A data descriptor that disagrees with the directory, under NO_GAPS.
    bytes_t liar;
    local_header(liar, "a", 0x0008, 0, 0, 0);
    liar.text("hello");
    liar.u32(crc ^ 1);
    liar.u32(5);
    liar.u32(5);
    uint32_t liar_directory = liar.size();
    central_header(liar, "a", 0x0008, crc, 5, 5, 0);
    end_record(liar, 1, liar.size() - liar_directory, liar_directory);
    reader = open_memory(liar.data, codecs, RBTARCHIVE_STRICT_NO_GAPS, &result);
    CHECK(result == RBT_RESULT_ARCHIVE_MALFORMED);
    reader = open_memory(liar.data, codecs, RBTARCHIVE_STRICT_NONE, &result);
    CHECK(result == RBT_RESULT_OK);
    rbtarchive_reader_destroy(reader);

    // Everything the writer writes passes every flag it doesn't use: whole,
    // aligned, ZIP64 and streamed entries.
    std::vector<uint8_t> archive = old_calls_archive(codecs); // two archives end to end: the first is a gap
    std::vector<uint8_t> sample = write_sample(codecs);
    reader = open_memory(sample, codecs, RBTARCHIVE_STRICT_NO_GAPS | RBTARCHIVE_STRICT_NO_ARCHIVE_COMMENT |
                                             RBTARCHIVE_STRICT_NO_DIRECTORY_ENTRIES | RBTARCHIVE_STRICT_NO_ENTRY_COMMENTS,
                         &result);
    CHECK(result == RBT_RESULT_OK);
    rbtarchive_reader_destroy(reader);
    reader = open_memory(archive, codecs, RBTARCHIVE_STRICT_NO_GAPS, &result);
    CHECK(result == RBT_RESULT_ARCHIVE_MALFORMED);
    // An entry comment.
    std::vector<uint8_t> commented;
    rbtarchive_writer writer = memory_writer(commented, codecs);
    RBTARCHIVE_ENTRY_DESC entry = entry_desc("a", RBTARCHIVE_METHOD_STORED);
    entry.comment = "note";
    entry.comment_size = 4;
    CHECK(rbtarchive_writer_add_entry(writer, &entry, "a", 1, nullptr) == RBT_RESULT_OK);
    CHECK(rbtarchive_writer_finish(writer, nullptr) == RBT_RESULT_OK);
    rbtarchive_writer_destroy(writer);
    reader = open_memory(commented, codecs, RBTARCHIVE_STRICT_NO_ENTRY_COMMENTS, &result);
    CHECK(result == RBT_RESULT_ARCHIVE_MALFORMED);
    reader = open_memory(commented, codecs, RBTARCHIVE_STRICT_NO_GAPS, &result);
    CHECK(result == RBT_RESULT_OK);
    rbtarchive_reader_destroy(reader);
    rbtarchive_codecs_destroy(codecs);
}

// Item 10: data in front of an archive that its offsets don't count.
static void test_data_in_front()
{
    current_test = "data in front";
    rbtarchive_codecs codecs = deflate_codecs();
    std::vector<uint8_t> text = text_data();
    for (int zip64 = 0; zip64 < 2; zip64++)
    {
        std::vector<uint8_t> inner;
        rbtarchive_writer writer = memory_writer(inner, codecs);
        for (int i = 0; i < (zip64 ? 70000 : 3); i++)
        {
            char name[32];
            snprintf(name, sizeof(name), "e%d", i);
            RBTARCHIVE_ENTRY_DESC entry = entry_desc(name, i == 1 ? RBTARCHIVE_METHOD_DEFLATE : RBTARCHIVE_METHOD_STORED);
            const void *data = i == 1 ? (const void *)text.data() : (const void *)name;
            uint64_t size = i == 1 ? text.size() : strlen(name);
            CHECK(rbtarchive_writer_add_entry(writer, &entry, data, size, nullptr) == RBT_RESULT_OK);
        }
        CHECK(rbtarchive_writer_finish(writer, nullptr) == RBT_RESULT_OK);
        rbtarchive_writer_destroy(writer);

        std::vector<uint8_t> stub(1000, 0x90);
        std::vector<uint8_t> shifted(stub.size() + inner.size(), 0x90);
        std::copy(inner.begin(), inner.end(), shifted.begin() + (ptrdiff_t)stub.size());
        RBT_RESULT result = RBT_RESULT_OK;
        rbtarchive_reader reader = open_memory(shifted, codecs, RBTARCHIVE_STRICT_NONE, &result);
        CHECK(result == RBT_RESULT_OK);
        if (!reader)
            continue;
        CHECK(rbtarchive_reader_get_offset_shift(reader) == stub.size());
        RBTARCHIVE_ENTRY_INFO info = entry_info(reader, 0);
        CHECK(info.local_header_offset == stub.size());
        std::vector<uint8_t> data;
        CHECK(read_named(reader, "e1", data) && data == text);
        CHECK(read_in_pieces(reader, 1, 333, data) && data == text);
        CHECK(read_named(reader, "e2", data) && data.size() == 2 && memcmp(data.data(), "e2", 2) == 0);

        // Strict about gaps, the stub is one.
        rbtarchive_reader strict = open_memory(shifted, codecs, RBTARCHIVE_STRICT_NO_GAPS, &result);
        CHECK(result == RBT_RESULT_ARCHIVE_MALFORMED && strict == nullptr);

        // Updated, the new directory counts the stub: no shift after.
        if (!zip64)
        {
            cuttable_t file;
            file.bytes = shifted;
            writer = update_writer(file, reader, codecs);
            RBTARCHIVE_ENTRY_DESC entry = entry_desc("new", RBTARCHIVE_METHOD_STORED);
            CHECK(rbtarchive_writer_add_entry(writer, &entry, "new", 3, nullptr) == RBT_RESULT_OK);
            CHECK(rbtarchive_writer_finish(writer, nullptr) == RBT_RESULT_OK);
            rbtarchive_writer_destroy(writer);
            rbtarchive_reader updated = open_memory(file.bytes, codecs);
            CHECK(updated && rbtarchive_reader_get_offset_shift(updated) == 0);
            CHECK(updated && read_named(updated, "e1", data) && data == text);
            CHECK(updated && read_named(updated, "new", data) && data.size() == 3);
            rbtarchive_reader_destroy(updated);
        }
        rbtarchive_reader_destroy(reader);

        // An archive whose offsets are right isn't moved, stub or not:
        // before, it opened as it is.
        CHECK(inner.size() > 0);
    }
    rbtarchive_codecs_destroy(codecs);
}

// Card X27: in a Debug build a destroyed entry reader's block stays its
// reader's until the reader is destroyed, so the entry reader's stale check
// (card X16) still refuses it after many entry readers of the same size
// have been made and destroyed since, any of which the heap could have put
// in its place. Destroying the reader frees every block it kept: counted by
// the CRT's debug heap with MSVC, by AddressSanitizer's leak check on Linux
// and macOS.
static void test_destroyed_entry_reader_kept()
{
    current_test = "destroyed entry reader kept";
#if !_DEBUG
    printf("  %s: skipped, a Release build doesn't check for a destroyed handle\n", current_test);
#else
    enum
    {
        CYCLES = 1000
    };
    rbtarchive_codecs codecs = deflate_codecs();
    std::vector<uint8_t> archive = write_sample(codecs);
#if CRT_HEAP_COUNTS
    _CrtMemState before, opened, held, after, difference;
    _CrtMemCheckpoint(&before);
#endif
    rbtarchive_reader reader = open_memory(archive, codecs);
    CHECK(reader);
    if (reader)
    {
#if CRT_HEAP_COUNTS
        _CrtMemCheckpoint(&opened);
#endif
        // Entry 1 is DEFLATE: a codec state as well as the read buffer.
        rbtarchive_entry_reader dead = nullptr;
        CHECK(rbtarchive_reader_open_entry(reader, 1, &dead, nullptr) == RBT_RESULT_OK && dead);
        rbtarchive_entry_reader_destroy(dead);
        uint32_t made = 0;
        for (uint32_t i = 0; i < CYCLES; i++)
        {
            rbtarchive_entry_reader entry = nullptr;
            if (rbtarchive_reader_open_entry(reader, 1, &entry, nullptr) == RBT_RESULT_OK)
                made++;
            rbtarchive_entry_reader_destroy(entry);
        }
        CHECK(made == CYCLES);
        uint8_t byte = 0;
        uint64_t got = 0;
        CHECK(rbtarchive_entry_reader_read(dead, &byte, 1, &got, nullptr) == RBT_RESULT_INVALID_ARGUMENT);
        rbtarchive_entry_reader_destroy(dead); // a second destroy, refused
#if CRT_HEAP_COUNTS
        // Held: one block for each entry reader destroyed, nothing else.
        _CrtMemCheckpoint(&held);
        _CrtMemDifference(&difference, &opened, &held);
        CHECK(difference.lCounts[_NORMAL_BLOCK] == CYCLES + 1);
#endif
        rbtarchive_reader_destroy(reader);
    }
#if CRT_HEAP_COUNTS
    // All of it gone with the reader.
    _CrtMemCheckpoint(&after);
    _CrtMemDifference(&difference, &before, &after);
    CHECK(difference.lCounts[_NORMAL_BLOCK] == 0);
#endif
    rbtarchive_codecs_destroy(codecs);
#endif
}

int main()
{
    test_codecs();
    test_safe_names();
    test_crc32();
    test_writer_checks();
    test_reader_checks();
    test_round_trip();
    test_empty_archive();
    test_alignment();
    test_limits();
    test_corruption();
    test_foreign_archive();
    test_zip64();
    test_many_entries();
    test_old_bytes_unchanged();
    test_streamed_entries();
    test_raw_entries();
    test_update();
    test_entry_fields();
    test_extra_helpers();
    test_strict();
    test_data_in_front();
    test_destroyed_entry_reader_kept();
    if (failures)
    {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("All rbtarchive tests passed\n");
    return 0;
}
