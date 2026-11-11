#ifndef RBTSYNC_INTERNAL_HPP
#define RBTSYNC_INTERNAL_HPP

// Internal header - never included outside rabbittus/src/sync/.
// The wire format, and the objects behind rbtsync_session.

#include "sync/rbtsyncnet.h"
#include "../rbtoverlap.hpp"
#include "../rbtstale.hpp"
#include <crnet.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Everything below needs Carrottus Net's TCP sockets. Where there are none
// (watchOS) the two calls that make a session return RBT_RESULT_UNSUPPORTED
// and no session ever exists.
#if CRNET_EXT_TCP

// ---- Wire format ------------------------------------------------------------

// A connection carries frames and nothing else:
//
//   [u32 length][u8 type][payload: length - 1 bytes]
//
// Every number is little-endian, written and read a byte at a time, so the
// layout depends on neither the compiler nor the machine.
//
// A frame is the unit a message-based transport would carry whole (one
// WebSocket binary message per frame, without the length); nothing here
// relies on two frames arriving together or on one arriving in pieces.
//
//   Type            Dir    Payload
//   HELLO           J->H   u32 magic, u16 wire version, u64 application id,
//                          u32 application version, u64 resume sequence,
//                          peer info to the end
//   WELCOME         H->J   u32 the joiner's peer id
//   REFUSE          H->J   u8 RBTSYNC_REASON
//   SNAPSHOT_BEGIN  H->J   u64 sequence, u64 total size
//   SNAPSHOT_DATA   H->J   bytes, at most RBTSYNC_SNAPSHOT_CHUNK_SIZE
//   SNAPSHOT_END    H->J   nothing
//   PEER_JOINED     H->J   u32 peer id, peer info to the end
//   PEER_LEFT       H->J   u32 peer id, u8 RBTSYNC_REASON
//   SUBMIT          J->H   u16 channel, message to the end
//   ORDERED         H->J   u16 channel, u64 sequence, u32 origin, message
//   LATEST          both   u16 channel, u64 key, u32 origin, message
//                          (origin is 0 from a joiner; the host fills it in)
//   PING            both   nothing
//
// Magic and wire version come first in HELLO so that everything after them
// may change with the version. [resume sequence] is always 0 today and
// ignored: it is where a joiner that was cut off would say which ordered
// message it last had, for a host that keeps enough of them to carry on
// from there instead of sending a snapshot (RBTSYNC_TODO.md).

constexpr uint32_t RBTSYNC_WIRE_MAGIC = 0x59535041u; // "APSY" as the bytes go out
constexpr uint16_t RBTSYNC_WIRE_VERSION = 1;

// A snapshot goes out in pieces of this size, so the largest frame a peer
// must accept doesn't depend on how big a snapshot is.
constexpr uint32_t RBTSYNC_SNAPSHOT_CHUNK_SIZE = 256u * 1024u;

// More than any frame carries besides the caller's own bytes.
constexpr uint32_t RBTSYNC_FRAME_OVERHEAD = 64;

enum RBTSYNC_FRAME : uint8_t
{
    RBTSYNC_FRAME_HELLO = 1,
    RBTSYNC_FRAME_WELCOME = 2,
    RBTSYNC_FRAME_REFUSE = 3,
    RBTSYNC_FRAME_SNAPSHOT_BEGIN = 4,
    RBTSYNC_FRAME_SNAPSHOT_DATA = 5,
    RBTSYNC_FRAME_SNAPSHOT_END = 6,
    RBTSYNC_FRAME_PEER_JOINED = 7,
    RBTSYNC_FRAME_PEER_LEFT = 8,
    RBTSYNC_FRAME_SUBMIT = 9,
    RBTSYNC_FRAME_ORDERED = 10,
    RBTSYNC_FRAME_LATEST = 11,
    RBTSYNC_FRAME_PING = 12,
};

// ---- Objects ----------------------------------------------------------------

using rbtsync_bytes = std::vector<uint8_t>;
// Bytes that several peers' queues send from; never changed once shared.
using rbtsync_shared_bytes = std::shared_ptr<const rbtsync_bytes>;
using rbtsync_clock = std::chrono::steady_clock;

// What a latest channel coalesces on.
struct rbtsync_latest_key
{
    uint32_t channel{0};
    RBTSYNC_PEER origin{RBTSYNC_PEER_NONE};
    uint64_t key{0};
    bool operator==(const rbtsync_latest_key &other) const
    {
        return channel == other.channel && origin == other.origin && key == other.key;
    }
};

struct rbtsync_latest_key_hash
{
    size_t operator()(const rbtsync_latest_key &k) const
    {
        uint64_t h = k.key * 0x9E3779B97F4A7C15ull;
        h ^= (((uint64_t)k.channel << 32) | k.origin) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        return (size_t)(h ^ (h >> 32));
    }
};

// Something waiting to be sent to one peer: bytes [begin, end) of [bytes].
struct rbtsync_out_item
{
    rbtsync_shared_bytes bytes;
    size_t begin{0};
    size_t end{0};
    // Whether it counts towards the peer's backlog. A snapshot and the
    // frames of the opening exchange don't.
    bool counted{false};
    // A latest channel's value, which a newer one for [key] may replace
    // while this hasn't begun to go out.
    bool latest{false};
    rbtsync_latest_key key;
};

enum rbtsync_peer_state
{
    // Hosting, about a joiner.
    RBTSYNC_PEER_AWAITING_HELLO,    // connected, hasn't said what it is
    RBTSYNC_PEER_AWAITING_SNAPSHOT, // let in; the application owes it a snapshot
    // Joined, about the host.
    RBTSYNC_PEER_AWAITING_WELCOME,   // HELLO sent
    RBTSYNC_PEER_RECEIVING_SNAPSHOT, // let in; the snapshot is on its way
    // Either.
    RBTSYNC_PEER_LIVE,
};

// What a wait set hands back for a member: which kind it is.
struct rbtsync_wait_member
{
    bool is_listener{false};
};

struct rbtsync_listener : rbtsync_wait_member
{
    crnet_tcp_listener handle{nullptr}; // NULL once it has failed
    uint32_t failed_accepts{0};           // in a row
};

// One connection. Hosting: one per joiner. Joined: the one to the host.
// Touched by Rabbittus Sync's thread only.
struct rbtsync_peer : rbtsync_wait_member
{
    RBTSYNC_PEER id{RBTSYNC_PEER_NONE};
    crnet_tcp_socket socket{nullptr};
    rbtsync_peer_state state{RBTSYNC_PEER_AWAITING_HELLO};
    // Dropped, and waiting for the loop to take it out of the wait set and
    // destroy its socket. Nothing more is sent to it or read from it.
    bool dead{false};
    bool in_wait_set{false};
    // Whether the wait set is asking about room on its socket, which it
    // only does while [out] couldn't be emptied.
    bool waiting_for_room{false};

    rbtsync_bytes info; // hosting: what it described itself with

    rbtsync_bytes in; // received and not yet a whole frame

    std::list<rbtsync_out_item> out;
    size_t front_sent{0};   // bytes of out.front() already sent
    uint64_t out_bytes{0};  // counted bytes still in [out]
    std::unordered_map<rbtsync_latest_key, std::list<rbtsync_out_item>::iterator, rbtsync_latest_key_hash> latest;

    rbtsync_clock::time_point last_received;
    rbtsync_clock::time_point last_sent;
    rbtsync_clock::time_point handshake_deadline;

    // Joined: the snapshot as it arrives.
    bool snapshot_begun{false};
    uint64_t snapshot_sequence{0};
    uint64_t snapshot_total{0};
    rbtsync_bytes snapshot;
};

// An event as it waits to be polled.
struct rbtsync_queued_event
{
    RBTSYNC_EVENT_TYPE type{RBTSYNC_EVENT_NONE};
    RBTSYNC_PEER peer{RBTSYNC_PEER_NONE};
    uint32_t channel{0};
    uint64_t sequence{0};
    uint64_t key{0};
    RBTSYNC_REASON reason{RBTSYNC_REASON_NONE};
    rbtsync_bytes data;
    std::string message;
};

// What the caller's thread asks of Rabbittus Sync's.
struct rbtsync_command
{
    enum kind_t
    {
        SEND,
        SNAPSHOT,
        DISCONNECT,
    };
    kind_t kind{SEND};
    uint32_t channel{0};
    uint64_t key{0};
    RBTSYNC_PEER peer{RBTSYNC_PEER_NONE};
    uint64_t sequence{0}; // SNAPSHOT: the last ordered message the state includes
    rbtsync_bytes data;
};

// An ordered message the host has numbered, kept until the host application
// has polled it: a snapshot made before then doesn't include it, and its
// joiner has to be sent it.
struct rbtsync_log_entry
{
    uint64_t sequence{0};
    rbtsync_shared_bytes frame;
};

struct rbtsync_session_t
{
    const char *debug_name{nullptr};
    RBT_STALE_TAG

    // ---- Set at creation and never changed ----
    bool hosting{false};
    uint64_t application_id{0};
    uint32_t application_version{0};
    std::vector<RBTSYNC_CHANNEL_MODE> channels;
    rbtsync_bytes peer_info;
    uint32_t max_message_size{0};
    uint64_t max_backlog_size{0};
    uint32_t heartbeat_interval_ms{0};
    uint32_t peer_timeout_ms{0};
    uint32_t handshake_timeout_ms{0};
    uint32_t frame_limit{0}; // the longest frame accepted
    // Hosting.
    uint32_t max_peers{0};
    std::vector<uint16_t> ports; // per listen address
    // Joined.
    std::string join_host;
    uint16_t join_port{0};
    CRNET_ADDRESS_FAMILY join_family{CRNET_ADDRESS_FAMILY_ANY};
    uint32_t resolve_timeout_ms{0};
    uint32_t connect_timeout_ms{0};
    uint64_t max_snapshot_size{0};

    // ---- The caller's thread only ----
    rbtsync_queued_event current; // what the last poll handed out
    RBTSYNC_PEER local_peer{RBTSYNC_PEER_NONE};
    bool can_send{false};
    // Hosting: peers whose RBTSYNC_EVENT_SNAPSHOT_REQUEST has been polled
    // and not yet answered.
    std::unordered_set<RBTSYNC_PEER> awaiting_snapshot;
    // Hosting: the last ordered message polled. Also in
    // [delivered_sequence] for Rabbittus Sync's thread.
    uint64_t polled_sequence{0};

    // ---- Shared ----
    std::mutex inbox_mutex;
    std::condition_variable inbox_changed;
    std::deque<rbtsync_queued_event> inbox; // Rabbittus Sync's thread -> the caller's
    bool woken{false};                     // rbtsync_session_wake

    std::mutex outbox_mutex;
    std::vector<rbtsync_command> outbox; // the caller's thread -> Rabbittus Sync's
    uint64_t outbox_bytes{0};

    std::atomic<uint64_t> delivered_sequence{0};
    // Joined: counted bytes waiting for the host, as of the thread's last
    // look. With [outbox_bytes], what rbtsync_session_send holds against
    // [max_backlog_size].
    std::atomic<uint64_t> queued_bytes{0};
    std::atomic<bool> stop{false};

    // The two objects a joining thread blocks in outside the wait set, for
    // rbtsync_session_destroy to abort. Set and cleared under the mutex, so
    // an abort never meets a destroy.
    std::mutex abort_mutex;
    crnet_resolver resolver{nullptr};
    crnet_tcp_socket connecting{nullptr};

    // Made before the thread starts and destroyed after it has ended. The
    // instance is the session's own: every Carrottus Net object the session makes
    // is made with it, and it goes last.
    crnet_instance instance{nullptr};
    crnet_wait_set wait_set{nullptr};
    std::thread thread;

    // ---- Rabbittus Sync's thread only (and creation, before it starts) ----
    std::vector<std::unique_ptr<rbtsync_listener>> listeners;
    std::vector<std::unique_ptr<rbtsync_peer>> peers;
    rbtsync_bytes receive_buffer;
    rbtsync_shared_bytes ping_frame;
    // Hosting.
    RBTSYNC_PEER next_peer{RBTSYNC_PEER_HOST + 1};
    uint64_t sequence{0}; // the last ordered message numbered
    std::deque<rbtsync_log_entry> log;
    // Debug builds' check of rbtcore.h's "Threads" rule (../rbtoverlap.hpp).
    rbt_overlap overlap{};
};

#endif // CRNET_EXT_TCP

#endif // RBTSYNC_INTERNAL_HPP
