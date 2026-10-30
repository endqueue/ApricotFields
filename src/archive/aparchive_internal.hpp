#ifndef APARCHIVE_INTERNAL_HPP
#define APARCHIVE_INTERNAL_HPP

#include "archive/aparchive.h"
#include "../aprioverlap.hpp"
#include "../apristale.hpp"
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
    APRI_STALE_TAG
    std::vector<APARCHIVE_CODEC> codecs; // never STORED
    const APARCHIVE_CODEC *find(uint16_t method) const;
    // Debug builds' check of apricore.h's "Threads" rule (../aprioverlap.hpp).
    apri_overlap overlap{};
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
    uint16_t version_made_by{0};
    uint16_t internal_attributes{0};
    uint32_t external_attributes{0};
    std::vector<uint8_t> comment;
};

struct aparchive_writer_t
{
    const char *debug_name{nullptr};
    APRI_STALE_TAG
    APARCHIVE_SINK sink{};
    aparchive_codecs codecs{nullptr};
    uint64_t offset{0}; // bytes written to the sink
    std::vector<aparchive_written_entry_t> entries;
    std::unordered_set<std::string> names; // for the uniqueness check
    // A failure mid-record or a finish ends the writer: nothing more may be
    // written.
    bool failed{false};
    bool finished{false};
    bool allow_directory_entries{false};
    std::vector<uint8_t> comment; // the end record's
    // Updating: the archive is cut to [truncate_at] before the first byte
    // is written.
    bool (*truncate)(void *user, uint64_t size){nullptr};
    uint64_t truncate_at{0};
    bool truncate_pending{false};
    // The entry being streamed, when [streaming]: its codec (NULL for
    // STORED) and the codec's state, its running CRC-32 and sizes, and
    // where its compressed bytes go before the sink.
    bool streaming{false};
    const APARCHIVE_CODEC *stream_codec{nullptr};
    void *stream_state{nullptr};
    uint32_t stream_crc{0};
    uint64_t stream_in{0};
    uint64_t stream_out{0};
    std::vector<uint8_t> stream_buffer;
    // Debug builds' check of apricore.h's "Threads" rule (../aprioverlap.hpp).
    apri_overlap overlap{};
    ~aparchive_writer_t()
    {
        if (stream_codec && stream_state)
            stream_codec->stream_end(stream_codec->user, stream_state);
    }
};

struct aparchive_reader_t
{
    const char *debug_name{nullptr};
    APRI_STALE_TAG
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
    // Per entry: its central header gave a size in its ZIP64 record, so an
    // update writes it the same way.
    std::vector<bool> central_zip64;
    std::unordered_map<std::string_view, uint32_t> name_index;
    std::vector<uint8_t> comment; // the end record's
    // Bytes in front of the archive its own offsets don't count.
    uint64_t shift{0};
};

// One entry read in pieces.
struct aparchive_entry_reader_t
{
    const char *debug_name{nullptr};
    APRI_STALE_TAG
    aparchive_reader reader{nullptr};
    uint32_t index{0};
    const APARCHIVE_CODEC *codec{nullptr}; // NULL for STORED
    void *state{nullptr};
    uint64_t data_offset{0};
    uint64_t in_read{0};  // compressed bytes read from the source
    uint64_t out_total{0}; // uncompressed bytes handed out
    uint32_t crc{0};
    std::vector<uint8_t> in;
    size_t in_at{0};
    size_t in_end{0};
    bool stream_done{false}; // the codec reached its data's end
    bool finished{false};    // every byte handed out and checked
    bool failed{false};
    // Debug builds' check of apricore.h's "Threads" rule (../aprioverlap.hpp).
    apri_overlap overlap{};
};

#endif // APARCHIVE_INTERNAL_HPP
