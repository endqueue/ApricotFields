#ifndef APARCHIVE_INTERNAL_HPP
#define APARCHIVE_INTERNAL_HPP

#include "archive/aparchive.h"
#include <stddef.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Every opaque handle's first member is its debug name in Debug builds
// (apricore.h).

// ---- Objects ----------------------------------------------------------------

struct aparchive_codecs_t
{
    const char *debug_name{nullptr};
    std::vector<APARCHIVE_CODEC> codecs; // never STORED
    const APARCHIVE_CODEC *find(uint16_t method) const;
};

// What the central directory needs about each entry written so far.
struct aparchive_written_entry_t
{
    std::string name;
    uint16_t method{0};
    uint16_t flags{0};
    uint16_t dos_date{0};
    uint16_t dos_time{0};
    uint32_t crc32{0};
    uint64_t compressed_size{0};
    uint64_t uncompressed_size{0};
    uint64_t local_header_offset{0};
    bool zip64{false};          // APARCHIVE_ENTRY_DESC::zip64: ZIP64 size fields whatever the sizes
    std::vector<uint8_t> extra; // the caller's records, written to the central header
};

struct aparchive_writer_t
{
    const char *debug_name{nullptr};
    APARCHIVE_SINK sink{};
    aparchive_codecs codecs{nullptr};
    uint64_t offset{0}; // bytes written to the sink
    std::vector<aparchive_written_entry_t> entries;
    std::unordered_set<std::string> names; // for the uniqueness check
    // A failure mid-record or a finish ends the writer: nothing more may be
    // written.
    bool failed{false};
    bool finished{false};
};

struct aparchive_reader_t
{
    const char *debug_name{nullptr};
    APARCHIVE_SOURCE source{};
    aparchive_codecs codecs{nullptr};
    // Where the central directory starts: every entry's header and data end
    // at or before it.
    uint64_t directory_offset{0};
    // The central directory as read, kept whole: entries' extras point into
    // it.
    std::vector<uint8_t> central_directory;
    // Every name, NUL-terminated, one after another: entries' names point
    // into it. Sized once on open and never grown.
    std::vector<char> names;
    std::vector<APARCHIVE_ENTRY_INFO> entries;
    std::unordered_map<std::string_view, uint32_t> name_index;
};

#endif // APARCHIVE_INTERNAL_HPP
