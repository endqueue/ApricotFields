#ifndef RBTSYNC_NET_H
#define RBTSYNC_NET_H

#include "../rbtcore.h"
#include <crcore.h>
#include <stdbool.h>
#include <stdint.h>

/**
 * @file rbtsyncnet.h
 * @brief Rabbittus Sync: sessions, channels and snapshots between several running
 * copies of one application.
 *
 * General purpose: it knows nothing about what is being kept in step. Every
 * message, every snapshot and every peer's description is bytes the caller
 * wrote and the caller reads. What Rabbittus Sync owns is what lies between those
 * bytes and a socket, and is the same whatever they mean:
 *
 * - Sessions. One copy hosts, the others join it. Each has a peer id; who
 *   is in the session, and who came and went, arrives as events.
 * - Channels. Each has one of two ways of delivering:
 *   - ORDERED: nothing is lost, and every peer receives every message in
 *     the same order, which the host decides. For what has to agree
 *     everywhere - edits to a shared model.
 *   - LATEST: per key, only the newest value matters: a value still
 *     waiting to go out is replaced by the next one for its key. For what
 *     goes stale - a cursor, a selection, a drag under way, a camera.
 * - Snapshots. A peer that joins late gets the host application's state as
 *   one block, then exactly the ordered messages that came after it.
 * - The network itself: a thread Rabbittus Sync starts for each session does all
 *   of it, so no call here except rbtsync_session_wait() waits on anything
 *   (see "Threads").
 *
 * The shape is a star. Every joined peer talks to the host only, and the
 * host passes on what the others need. The host is the one copy that has to
 * stay up: when it goes, the session is over for everyone.
 *
 * How ordered channels are meant to be used. A peer does not change its
 * state when it sends an ordered message; it changes it when the message
 * comes back in rbtsync_session_poll(), which it does for the peer that sent
 * it as for everyone else (the host included). Every peer then applies the
 * same messages in the same order to the same starting snapshot, and that
 * is all that keeps them in agreement - two edits that conflict are settled
 * by which the host numbered first, and how the loser is treated is the
 * caller's rule. A caller that wants its own edit on screen before the
 * round trip shows it provisionally and settles up when the message
 * arrives.
 *
 * What Rabbittus Sync does not do, on purpose. It doesn't reconnect: a joined
 * session whose connection ends is finished, and the caller makes another,
 * which starts from a fresh snapshot. It doesn't store anything. It doesn't
 * decide who may join beyond the application id and version matching; an
 * unwelcome peer is turned away by the host application with
 * rbtsync_session_disconnect_peer().
 *
 * Transport, today: plain TCP (Carrottus Net's sockets), so nothing is encrypted
 * and nothing proves who a peer is. That is right for a local network the
 * users trust and for nothing else. RBTSYNC_TODO.md has what a second
 * transport would bring and where it fits.
 *
 * Nothing has to be set up first. Each session starts the platform's
 * networking for itself (a crnet_instance of its own, crnet.h) and
 * gives it back when it is destroyed, whatever else in the process uses
 * the network. That instance's memory comes from the allocator in the
 * session's desc.
 *
 * Not on every platform. Where Carrottus Net has no TCP (watchOS), the two calls
 * that make a session return RBT_RESULT_UNSUPPORTED.
 *
 * @par Results
 * Calls return RBT_RESULT (rbtcore.h): RBT_RESULT_OK, a general code, or an
 * RBT_RESULT_SYNC_* one. The general codes mean, here:
 * - RBT_RESULT_INVALID_ARGUMENT: also a desc with a field left at a value its
 *   comment refuses, and a call made on a session of the other role.
 * - RBT_RESULT_IO: the network, or the system under it, failed.
 * - RBT_RESULT_NOT_FOUND: no event, or no such peer.
 * - RBT_RESULT_LIMIT: over a limit the desc set.
 * - RBT_RESULT_UNSUPPORTED: this platform has no transport Rabbittus Sync uses, or,
 *   on Linux, its libcurl isn't thread-safe (Carrottus Net needs one that is).
 *
 * @par Error messages
 * The calls that can fail for a reason worth a sentence take an
 * RBTSYNC_ERROR pointer (NULL to skip it), as rbtarchive.h's do. What goes
 * wrong later, on the network, arrives as an event instead, with its own
 * message.
 *
 * @par Struct bases
 * Every struct here but RBTSYNC_ERROR starts with a `base` (rbtcore.h,
 * "Struct bases"): the caller sets `base.type` to the struct's own
 * RBT_STRUCT_TYPE_SYNC_* and `base.next` to NULL, on the output struct
 * too (RBTSYNC_EVENT). A wrong type is RBT_RESULT_STRUCT_WRONG_TYPE and a
 * chained struct RBT_RESULT_STRUCT_UNKNOWN_IN_CHAIN: nothing chains onto any
 * of them yet. RBTSYNC_ERROR is frozen and has none.
 *
 * @par Defaults
 * Nothing in a desc has a default. A limit or a time left at 0 is refused
 * unless its comment says what 0 means.
 *
 * @par Threads
 * rbtcore.h's rule ("Threads"): a session is used by one caller at a time.
 * The exceptions, marked on each: one rbtsync_session_wait() may overlap the
 * session's other calls, and rbtsync_session_wake() may come from any
 * thread at any time; neither may overlap rbtsync_session_destroy().
 *
 * Rabbittus Sync runs threads of its own: rbtsync_session_host() and
 * rbtsync_session_join() each start one that does the session's networking
 * until rbtsync_session_destroy() joins it, and a joining session's network
 * thread starts one more, detached, that only looks the host up and that
 * nothing waits for.
 * Rabbittus Sync never calls back: nothing of the caller's runs on those threads,
 * and what they produce reaches the caller as events it polls.
 */

#if __cplusplus
extern "C" {
#endif

/* ---- Conventions ----------------------------------------------------- */

/** The size of RBTSYNC_ERROR's message buffer, in bytes, its NUL included. */
#define RBTSYNC_ERROR_MESSAGE_SIZE 256

/**
 * @brief Why a call failed, in a sentence, filled in by the calls that take
 * one.
 */
/* Frozen: filled per call. It never changes; a new need gets a new type. */
typedef struct RBTSYNC_ERROR {
	/** NUL-terminated, truncated to fit. */
	char message[RBTSYNC_ERROR_MESSAGE_SIZE];
} RBTSYNC_ERROR;

/* ---- Peers ----------------------------------------------------------- */

/**
 * @brief A peer's id within one session.
 *
 * The host gives them out, starting above its own, and never gives one out
 * twice in a session.
 */
typedef uint32_t RBTSYNC_PEER;

/** No peer: what rbtsync_session_get_local_peer() returns before a joined session has an id. */
#define RBTSYNC_PEER_NONE 0u
/** The host's own id, in every session. */
#define RBTSYNC_PEER_HOST 1u

/** The most bytes a peer may describe itself with (RBTSYNC_SESSION_DESC's `peer_info`). */
#define RBTSYNC_MAX_PEER_INFO_SIZE 4096u

/* ---- Channels -------------------------------------------------------- */

/** @brief How a channel delivers its messages. */
typedef enum RBTSYNC_CHANNEL_MODE RBT_ENUM_BASE {
	/**
	 * Reliable, and in one order for everyone. The message goes to the
	 * host, which numbers it and sends it to every peer, its sender
	 * included.
	 */
	RBTSYNC_CHANNEL_ORDERED = 1,
	/**
	 * Newest value per key. Goes to every other peer and not back to its
	 * sender. Never lost in transit, but a value may be skipped: where a
	 * newer one for the same key from the same peer catches up with it
	 * before it has been sent on, the older is dropped.
	 *
	 * A peer that joins sees a key at its next send and not before. A peer
	 * with something a newcomer should see sends it again when
	 * RBTSYNC_EVENT_PEER_JOINED arrives. Values are not withdrawn when their
	 * sender leaves; receivers drop them on RBTSYNC_EVENT_PEER_LEFT.
	 */
	RBTSYNC_CHANNEL_LATEST = 2,
} RBTSYNC_CHANNEL_MODE;

/** The most channels a session may have. */
#define RBTSYNC_MAX_CHANNELS 256u

/* ---- Making a session ------------------------------------------------ */

/**
 * @brief What the host and every joiner have to agree on, and what a
 * session of either role needs.
 */
typedef struct RBTSYNC_SESSION_DESC {
	/** This struct's type, RBT_STRUCT_TYPE_SYNC_SESSION_DESC, and its chain (rbtcore.h, "Struct bases"). */
	RBT_STRUCT_BASE base;

	/**
	 * Where the memory of the session's Carrottus Net instance, and of every
	 * Carrottus Net object the session makes, comes from (CR_ALLOCATOR,
	 * crcore.h). Copied, so the struct need not outlive the call; its
	 * functions and `user_data` must outlive the session. Not yet where
	 * Rabbittus Sync's own memory comes from, which is still the C++ heap's.
	 * NULL, or one missing a function, is refused.
	 */
	const CR_ALLOCATOR *allocator;

	/**
	 * Which application this is; with #application_version, which version
	 * of its messages it speaks. A joiner whose pair differs from the
	 * host's is turned away (RBTSYNC_REASON_REFUSED_APPLICATION): everything
	 * else in a session is bytes only the two of them can read, so a
	 * mismatch here is the one thing Rabbittus Sync can catch for them. The channel
	 * list and #max_message_size are part of what a version means; Rabbittus Sync
	 * doesn't compare them.
	 */
	uint64_t application_id;
	/** Which version of its messages the application speaks. @see #application_id */
	uint32_t application_version;

	/**
	 * The session's channels: `channels[i]` is how channel i delivers. At
	 * least one, at most RBTSYNC_MAX_CHANNELS, each an RBTSYNC_CHANNEL_MODE
	 * enumerator. Copied.
	 */
	const RBTSYNC_CHANNEL_MODE *channels;
	/** How many entries #channels has. */
	uint32_t channel_count;

	/**
	 * What this peer tells the others about itself - a name, a colour; the
	 * caller's format. Every other peer gets it with
	 * RBTSYNC_EVENT_PEER_JOINED. May be NULL with a #peer_info_size of 0.
	 * Copied.
	 */
	const void *peer_info;
	/** The size of #peer_info in bytes. At most RBTSYNC_MAX_PEER_INFO_SIZE. */
	uint32_t peer_info_size;

	/**
	 * The largest message this session sends or accepts, in bytes. A peer
	 * that sends a larger one is taken to be broken and dropped
	 * (RBTSYNC_REASON_PROTOCOL). 0 is refused.
	 */
	uint32_t max_message_size;

	/**
	 * How many bytes may wait to be sent to one peer. Hosting: a peer that
	 * falls further behind than this is dropped (RBTSYNC_REASON_BACKLOG) -
	 * it isn't reading, and an ordered channel can't skip anything to let
	 * it catch up. Joined: rbtsync_session_send() returns RBT_RESULT_LIMIT
	 * while this much is already waiting for the host. A snapshot on its
	 * way to a joiner doesn't count. 0 is refused.
	 */
	uint64_t max_backlog_size;

	/**
	 * How long a connection may go without this side sending anything
	 * before it sends a few bytes to show it is alive, in milliseconds.
	 * Both sides of a connection use their own; with #peer_timeout_ms it
	 * is part of what an application version means, since a time-out
	 * shorter than the other end's interval drops healthy peers. 0 turns
	 * this half off.
	 */
	uint32_t heartbeat_interval_ms;
	/**
	 * How long a connection may go without receiving anything before the
	 * other end is taken to be gone (RBTSYNC_REASON_TIMED_OUT), in
	 * milliseconds. Both sides of a connection use their own; see
	 * #heartbeat_interval_ms. 0 turns this half off: with no time-out, a
	 * peer that vanishes without closing is only noticed when the
	 * operating system gives up on it, which can be many minutes.
	 */
	uint32_t peer_timeout_ms;

	/**
	 * How long the opening exchange may take once a connection is up, in
	 * milliseconds: for the host, a newcomer saying what it is; for a
	 * joiner, the host's answer. Something that connects and says nothing
	 * is dropped after this. 0 is refused.
	 */
	uint32_t handshake_timeout_ms;
} RBTSYNC_SESSION_DESC;

/** @brief What hosting a session needs beyond RBTSYNC_SESSION_DESC. */
typedef struct RBTSYNC_HOST_DESC {
	/** This struct's type, RBT_STRUCT_TYPE_SYNC_HOST_DESC, and its chain (rbtcore.h, "Struct bases"). */
	RBT_STRUCT_BASE base;

	/**
	 * The local addresses to listen on, numeric, as Carrottus Net takes them:
	 * "0.0.0.0" for every IPv4 interface, "::" for every IPv6 one,
	 * "127.0.0.1" for this machine only, or an interface's own. One
	 * listener each; listening on both families is two entries. None may
	 * be NULL.
	 */
	const char *const *listen_addresses;
	/** How many entries #listen_addresses has: at least one, at most RBTSYNC_MAX_LISTEN_ADDRESSES. */
	uint32_t listen_address_count;

	/**
	 * The port, on every address. 0 has the system pick one per address;
	 * rbtsync_session_get_port() says which.
	 */
	uint16_t port;

	/**
	 * How many connections may wait to be accepted before the system turns
	 * new ones away. 0 is refused.
	 */
	uint32_t listen_backlog;

	/**
	 * The most peers that may be joined or joining at once, not counting
	 * the host. One more is turned away (RBTSYNC_REASON_REFUSED_FULL). 0 is
	 * refused.
	 */
	uint32_t max_peers;
} RBTSYNC_HOST_DESC;

/** The most addresses a host may listen on (RBTSYNC_HOST_DESC's `listen_address_count`). */
#define RBTSYNC_MAX_LISTEN_ADDRESSES 8u

/** @brief Which address family a joiner connects over. */
typedef enum RBTSYNC_ADDRESS_FAMILY RBT_ENUM_BASE {
	/** Whichever the name has. */
	RBTSYNC_ADDRESS_FAMILY_ANY = 0,
	/** IPv4 addresses only. */
	RBTSYNC_ADDRESS_FAMILY_IPV4 = 1,
	/** IPv6 addresses only. */
	RBTSYNC_ADDRESS_FAMILY_IPV6 = 2,
} RBTSYNC_ADDRESS_FAMILY;

/** @brief What joining a session needs beyond RBTSYNC_SESSION_DESC. */
typedef struct RBTSYNC_JOIN_DESC {
	/** This struct's type, RBT_STRUCT_TYPE_SYNC_JOIN_DESC, and its chain (rbtcore.h, "Struct bases"). */
	RBT_STRUCT_BASE base;

	/** The host: a name or a numeric address, UTF-8. Neither NULL nor empty. Copied. */
	const char *host;
	/** The port the host listens on. */
	uint16_t port;

	/**
	 * Limits a name's addresses to one family. They are tried in the order
	 * the system's resolver gives them, until one connects.
	 */
	RBTSYNC_ADDRESS_FAMILY family;

	/** How long looking the name up may take, in milliseconds. 0 is refused. */
	uint32_t resolve_timeout_ms;
	/** How long connecting to each address may take, in milliseconds. 0 is refused. */
	uint32_t connect_timeout_ms;

	/**
	 * The largest snapshot this peer will take from the host, in bytes. A
	 * larger one ends the session (RBTSYNC_REASON_SNAPSHOT_TOO_BIG) before
	 * any of it is kept. 0 is refused.
	 */
	uint64_t max_snapshot_size;
} RBTSYNC_JOIN_DESC;

/** @brief Opaque handle to a session, hosted or joined. */
typedef struct rbtsync_session_t *rbtsync_session;

/**
 * @brief Starts a session and listens for peers.
 *
 * Listening has begun when this returns, and a failure to is this call's:
 * RBT_RESULT_SYNC_ADDRESS_IN_USE when another listener holds one of the
 * addresses, RBT_RESULT_IO otherwise, with the system's reason in
 * @p out_error.
 *
 * @param[in]  desc        What the session needs. Only read during the call.
 * @param[in]  host_desc   What hosting needs. Only read during the call.
 * @param[out] out_session The new session, or NULL on failure.
 * @param[out] out_error   Why the call failed, in a sentence. May be NULL.
 *
 * @return RBT_RESULT_OK; RBT_RESULT_INVALID_ARGUMENT for a refused argument,
 *         RBT_RESULT_STRUCT_* for a wrong base; RBT_RESULT_SYNC_ADDRESS_IN_USE
 *         or RBT_RESULT_IO as above; RBT_RESULT_OUT_OF_MEMORY; or
 *         RBT_RESULT_UNSUPPORTED where this platform has no transport.
 *
 * Thread: starts the session's network thread (see "Threads", at the top
 * of this header), which runs until rbtsync_session_destroy().
 *
 * @see rbtsync_session_destroy()
 */
RBT_RESULT rbtsync_session_host(const RBTSYNC_SESSION_DESC *desc, const RBTSYNC_HOST_DESC *host_desc,
                             rbtsync_session *out_session, RBTSYNC_ERROR *out_error);

/**
 * @brief Starts joining a host and returns at once; nothing has been
 * connected to yet.
 *
 * The rest arrives as events: RBTSYNC_EVENT_CONNECTED, then
 * RBTSYNC_EVENT_SNAPSHOT, then the session's traffic - or, if any of it
 * fails, RBTSYNC_EVENT_DISCONNECTED and nothing more.
 *
 * @param[in]  desc        What the session needs. Only read during the call.
 * @param[in]  join_desc   What joining needs. Only read during the call.
 * @param[out] out_session The new session, or NULL on failure.
 * @param[out] out_error   Why the call failed, in a sentence. May be NULL.
 *
 * @return RBT_RESULT_OK; RBT_RESULT_INVALID_ARGUMENT for a refused argument,
 *         RBT_RESULT_STRUCT_* for a wrong base; RBT_RESULT_IO when the session
 *         can't be started; RBT_RESULT_OUT_OF_MEMORY; or RBT_RESULT_UNSUPPORTED
 *         where this platform has no transport.
 *
 * Thread: starts the session's network thread (see "Threads", at the top
 * of this header), which runs until rbtsync_session_destroy(), and which
 * starts one more, detached, to look the host up.
 *
 * @see rbtsync_session_destroy()
 */
RBT_RESULT rbtsync_session_join(const RBTSYNC_SESSION_DESC *desc, const RBTSYNC_JOIN_DESC *join_desc,
                             rbtsync_session *out_session, RBTSYNC_ERROR *out_error);

/**
 * @brief Ends the session for this peer and frees it.
 *
 * What was still waiting to be sent may not be. Doesn't wait on the
 * network; it can wait a moment for Rabbittus Sync's thread to come back.
 *
 * Thread: aborts and joins the session's network thread. A host lookup's
 * thread is not waited for: it ends on its own once the lookup returns.
 * Overlaps no other call on @p session, rbtsync_session_wait() and
 * rbtsync_session_wake() included.
 *
 * @param[in] session Session to destroy. NULL does nothing.
 */
void rbtsync_session_destroy(rbtsync_session session);

/**
 * @brief Hosting: the port being listened on for
 * `listen_addresses[index]` - the desc's, or the one the system picked
 * for 0.
 *
 * @param[in]  session  A hosted session.
 * @param[in]  index    Index into the host desc's `listen_addresses`.
 * @param[out] out_port The port.
 *
 * @return RBT_RESULT_OK, or RBT_RESULT_INVALID_ARGUMENT for a NULL argument, a
 *         joined session or an @p index past the last address.
 */
RBT_RESULT rbtsync_session_get_port(rbtsync_session session, uint32_t index, uint16_t *out_port);

/**
 * @brief This peer's id.
 *
 * @param[in] session The session.
 *
 * @return RBTSYNC_PEER_HOST when hosting; when joined, the id the host gave,
 *         or RBTSYNC_PEER_NONE until RBTSYNC_EVENT_CONNECTED has been polled.
 *         RBTSYNC_PEER_NONE for a NULL @p session.
 */
RBTSYNC_PEER rbtsync_session_get_local_peer(rbtsync_session session);

/* ---- Events ---------------------------------------------------------- */

/** @brief What an RBTSYNC_EVENT reports. */
typedef enum RBTSYNC_EVENT_TYPE RBT_ENUM_BASE {
	/** Not an event: the zero value. */
	RBTSYNC_EVENT_NONE = 0,

	/**
	 * Joined only: the host has let this peer in. `peer` is its id. The
	 * snapshot is still to come and nothing may be sent yet.
	 */
	RBTSYNC_EVENT_CONNECTED = 1,

	/**
	 * Joined only, once: the host application's state. `data` and `size`
	 * are what it passed to rbtsync_session_send_snapshot(), and `sequence`
	 * the number of the last ordered message that state includes. Every
	 * ordered message from here on has a higher one. Sending is allowed
	 * once this has been polled.
	 */
	RBTSYNC_EVENT_SNAPSHOT = 2,

	/**
	 * Hosting only: `peer` has connected and needs a snapshot; `data` is
	 * its peer info. Answer with rbtsync_session_send_snapshot(), or turn it
	 * away with rbtsync_session_disconnect_peer(). Until one or the other
	 * the peer is not in the session: it is sent nothing and nobody is told
	 * of it.
	 */
	RBTSYNC_EVENT_SNAPSHOT_REQUEST = 3,

	/**
	 * `peer` is in the session; `data` is its peer info. Hosting: its
	 * snapshot has been queued. Joined: also sent once for each peer that
	 * was already there, the host among them, right after the snapshot.
	 */
	RBTSYNC_EVENT_PEER_JOINED = 4,

	/**
	 * `peer` is gone, for `reason`, with `message` where there is more to
	 * say. Hosting: also for a peer that asked for a snapshot and left
	 * before getting one. Joined: `reason` is the host's, as it saw that
	 * peer go.
	 */
	RBTSYNC_EVENT_PEER_LEFT = 5,

	/**
	 * A message on `channel` from `peer`. Ordered: `sequence` is its place
	 * in the session's one order, counted from 1 across all ordered
	 * channels, and `key` is 0. Latest: `key` is the sender's and
	 * `sequence` is 0.
	 */
	RBTSYNC_EVENT_MESSAGE = 6,

	/**
	 * Joined: the session is over for this peer - it couldn't get in, or
	 * the connection to the host ended. Nothing follows, and only
	 * rbtsync_session_destroy() is left. Hosting: a listener failed and no
	 * longer takes newcomers at its address; the session carries on with
	 * the peers it has. `reason` and `message` say why.
	 *
	 * Either role, with RBTSYNC_REASON_INTERNAL: Rabbittus Sync's thread could not
	 * go on, and the session is over.
	 */
	RBTSYNC_EVENT_DISCONNECTED = 7,
} RBTSYNC_EVENT_TYPE;

/** @brief Why a peer left or a session ended. */
typedef enum RBTSYNC_REASON RBT_ENUM_BASE {
	/** No reason: the value on an event that doesn't carry one. */
	RBTSYNC_REASON_NONE = 0,
	/** The other end closed the connection. */
	RBTSYNC_REASON_CLOSED = 1,
	/**
	 * The network or the system failed; the event's `message` has the
	 * platform's own code for a log.
	 */
	RBTSYNC_REASON_NETWORK = 2,
	/**
	 * Nothing was heard for the desc's `peer_timeout_ms`, the opening
	 * exchange outran `handshake_timeout_ms`, or the lookup or every
	 * connection attempt outran its limit.
	 */
	RBTSYNC_REASON_TIMED_OUT = 3,
	/**
	 * The other end sent something that isn't Rabbittus Sync's, or a message over
	 * `max_message_size`, or one the channel list doesn't allow.
	 */
	RBTSYNC_REASON_PROTOCOL = 4,
	/**
	 * The host has a different `application_id` or
	 * `application_version`, or an Rabbittus Sync that speaks another version of
	 * its own wire format.
	 */
	RBTSYNC_REASON_REFUSED_APPLICATION = 5,
	/** The host has `max_peers` already. */
	RBTSYNC_REASON_REFUSED_FULL = 6,
	/** The peer fell `max_backlog_size` behind. */
	RBTSYNC_REASON_BACKLOG = 7,
	/**
	 * The host application dropped the peer
	 * (rbtsync_session_disconnect_peer()). The peer it was used on sees
	 * RBTSYNC_REASON_CLOSED itself.
	 */
	RBTSYNC_REASON_DISCONNECTED_BY_HOST = 8,
	/** The host's name has no address. */
	RBTSYNC_REASON_RESOLVE_FAILED = 9,
	/** No address of the host's took the connection. */
	RBTSYNC_REASON_CONNECT_FAILED = 10,
	/** The snapshot is over the join desc's `max_snapshot_size`. */
	RBTSYNC_REASON_SNAPSHOT_TOO_BIG = 11,
	/** Rabbittus Sync ran out of memory, or couldn't start a thread. */
	RBTSYNC_REASON_INTERNAL = 12,
} RBTSYNC_REASON;

/**
 * @brief One event, filled in by rbtsync_session_poll().
 *
 * Which members an event uses is said under its `type`'s enumerator; the
 * others are 0.
 */
typedef struct RBTSYNC_EVENT {
	/** This struct's type, RBT_STRUCT_TYPE_SYNC_EVENT, set by the caller (rbtcore.h, "Struct bases"). */
	RBT_STRUCT_BASE_OUT base;
	/** What happened. */
	RBTSYNC_EVENT_TYPE type;
	/** The peer the event is about. */
	RBTSYNC_PEER peer;
	/** A message's channel. */
	uint32_t channel;
	/** An ordered message's place in the session's order, or a snapshot's last ordered message. */
	uint64_t sequence;
	/** A latest-channel message's key. */
	uint64_t key;
	/** Why a peer left or the session ended. */
	RBTSYNC_REASON reason;
	/**
	 * The event's bytes, or NULL with a #size of 0. Rabbittus Sync's, and good
	 * until the next rbtsync_session_poll() on this session or its destroy.
	 */
	const void *data;
	/** The size of #data in bytes. */
	uint64_t size;
	/**
	 * Never NULL; empty where there is nothing to add. Same lifetime as
	 * #data. For a log: not for parsing, and not in any fixed language.
	 */
	const char *message;
} RBTSYNC_EVENT;

/**
 * @brief Takes the next event, in the order things happened.
 *
 * Never waits. Events are kept until they are polled, however long that
 * is, so a session that is not polled grows.
 *
 * @param[in]     session   The session.
 * @param[in,out] out_event The event. Its `base` is the caller's to set.
 *
 * @return RBT_RESULT_OK; RBT_RESULT_NOT_FOUND when there is none, which is not
 *         a failure; RBT_RESULT_INVALID_ARGUMENT for a NULL argument, or
 *         RBT_RESULT_STRUCT_* for a wrong base.
 */
RBT_RESULT rbtsync_session_poll(rbtsync_session session, RBTSYNC_EVENT *out_event);

/**
 * @brief Waits for there to be an event to poll, for a caller with no
 * frame loop to poll from.
 *
 * Thread: one wait at a time may overlap the session's other calls, on
 * another thread, except rbtsync_session_destroy().
 *
 * @param[in] session    The session.
 * @param[in] timeout_ms How long to wait at most, in milliseconds.
 *                       RBTSYNC_WAIT_FOREVER sets no limit.
 *
 * @return RBT_RESULT_OK when there is one, RBT_RESULT_NOT_FOUND when the time
 *         ran out or rbtsync_session_wake() ended the wait first.
 */
RBT_RESULT rbtsync_session_wait(rbtsync_session session, uint32_t timeout_ms);

/** No time limit, for rbtsync_session_wait()'s `timeout_ms`. */
#define RBTSYNC_WAIT_FOREVER 0xFFFFFFFFu

/**
 * @brief Ends an rbtsync_session_wait() in progress, or the next one when
 * none is.
 *
 * Thread: may come from any thread, overlapping any call on @p session
 * except rbtsync_session_destroy().
 *
 * @param[in] session The session. NULL does nothing.
 */
void rbtsync_session_wake(rbtsync_session session);

/* ---- Sending --------------------------------------------------------- */

/**
 * @brief Sends @p size bytes on @p channel.
 *
 * The data is copied; the call never waits on the network. A size of 0 is
 * a message like any other.
 *
 * Ordered: the message comes back through rbtsync_session_poll(), for this
 * peer too, and this peer's own come back in the order it sent them.
 *
 * @param[in] session The session.
 * @param[in] channel The channel's index in the desc's `channels`.
 * @param[in] key     The value's key on a latest channel; must be 0 on an
 *                    ordered one.
 * @param[in] data    The message. May be NULL when @p size is 0.
 * @param[in] size    The message's size in bytes.
 *
 * @return RBT_RESULT_OK. RBT_RESULT_SYNC_NOT_CONNECTED on a joined session
 *         before RBTSYNC_EVENT_SNAPSHOT has been polled or after
 *         RBTSYNC_EVENT_DISCONNECTED has. RBT_RESULT_LIMIT for a message over
 *         `max_message_size`, or, joined, while `max_backlog_size` is
 *         waiting for the host: nothing was queued, and the caller tries
 *         again later or gives the session up. RBT_RESULT_INVALID_ARGUMENT
 *         for a NULL @p session, no such @p channel, a NULL @p data with a
 *         size, or a @p key on an ordered channel. RBT_RESULT_OUT_OF_MEMORY.
 */
RBT_RESULT rbtsync_session_send(rbtsync_session session, uint32_t channel, uint64_t key, const void *data,
                             uint32_t size);

/**
 * @brief Hosting: answers an RBTSYNC_EVENT_SNAPSHOT_REQUEST.
 *
 * @p data is the application's state as it stands with every event polled
 * so far applied and nothing else - so it is made, and this called,
 * without polling in between. Rabbittus Sync notes which ordered message that
 * state ends at and sends @p peer the ones after it. The data is copied.
 *
 * The peer is in the session from here: it gets RBTSYNC_EVENT_PEER_JOINED
 * for everyone present, everyone present gets one for it, and this session
 * polls one too.
 *
 * @param[in] session A hosted session.
 * @param[in] peer    The peer whose request this answers.
 * @param[in] data    The state. May be NULL with a @p size of 0, for an
 *                    application whose state is all in its messages.
 * @param[in] size    The state's size in bytes.
 *
 * @return RBT_RESULT_OK. RBT_RESULT_NOT_FOUND when @p peer has no request
 *         waiting - also when it has left, which RBTSYNC_EVENT_PEER_LEFT
 *         will say. RBT_RESULT_INVALID_ARGUMENT for a NULL @p session, a
 *         joined session, or a NULL @p data with a size.
 *         RBT_RESULT_OUT_OF_MEMORY.
 */
RBT_RESULT rbtsync_session_send_snapshot(rbtsync_session session, RBTSYNC_PEER peer, const void *data, uint64_t size);

/**
 * @brief Hosting: drops @p peer, whether it is in the session or still
 * waiting for a snapshot.
 *
 * RBTSYNC_EVENT_PEER_LEFT follows either way. A peer that has already gone
 * is not an error.
 *
 * @param[in] session A hosted session.
 * @param[in] peer    The peer to drop.
 *
 * @return RBT_RESULT_OK; RBT_RESULT_INVALID_ARGUMENT for RBTSYNC_PEER_NONE and
 *         RBTSYNC_PEER_HOST, a NULL @p session or a joined one;
 *         RBT_RESULT_OUT_OF_MEMORY.
 */
RBT_RESULT rbtsync_session_disconnect_peer(rbtsync_session session, RBTSYNC_PEER peer);

#if __cplusplus
}
#endif

#endif // RBTSYNC_NET_H
