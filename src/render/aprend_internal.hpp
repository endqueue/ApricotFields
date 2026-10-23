#ifndef APREND_INTERNAL_HPP
#define APREND_INTERNAL_HPP

/* Internal header — never included outside ApricotFields/src/render/.
 * Defines the concrete structs behind Aprend's opaque handles, and provides
 * zero-cost GLM conversion helpers. */

#include "aprimath.h"
#include "render/aprendbuffers.h"
#include "render/aprendcommands.h"
#include "render/aprendcontext.h"
#include "render/aprenddrawlist.h"
#include "render/aprendframes.h"
#include "render/aprendmaterial.h"
#include "render/aprendpass.h"
#include "render/aprendpipeline.h"
#include "render/aprendswapchain.h"
#include "render/aprendtransient.h"
#include "render/aprendtimer.h"

#include "aprendbuiltin_internal.hpp"
#include "aprendpools_internal.hpp"
#include "aprendrendertargets_internal.hpp"
#include "aprendreflect_internal.hpp"
#include "aprendtransient_internal.hpp"

#include <spudgpu.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "../apristructbase.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/* A caller's based struct, checked as every Aprend call that takes one checks
 * it (apricore.h, "Struct bases"): true if its base is [type] with nothing
 * chained on; otherwise false, after printing why for [who], the call, and
 * [name], the parameter. */
static inline bool aprend_base_ok(const char *who, const char *name, const APRI_STRUCT_BASE *base, APRI_STRUCT_TYPE type) {
	const APRESULT result = apri_struct_check(base, type);
	if (result == APRESULT_OK)
		return true;
	printf("apricot: %s: %s %s\n", who, name, apri_struct_check_message(result));
	return false;
}

/* ============================================================
   GLM conversion helpers
   Reinterpret-cast is valid because Apri### structs are laid out
   identically to their GLM counterparts (same member order, same types).
   ============================================================ */

inline glm::vec2 &to_glm(ApriVec2 &v) { return reinterpret_cast<glm::vec2 &>(v); }
inline glm::vec3 &to_glm(ApriVec3 &v) { return reinterpret_cast<glm::vec3 &>(v); }
inline glm::vec4 &to_glm(ApriVec4 &v) { return reinterpret_cast<glm::vec4 &>(v); }
inline glm::dvec3 &to_glm(ApriDVec3 &v) { return reinterpret_cast<glm::dvec3 &>(v); }
inline glm::dvec4 &to_glm(ApriDVec4 &v) { return reinterpret_cast<glm::dvec4 &>(v); }
inline glm::quat &to_glm(ApriQuat &v) { return reinterpret_cast<glm::quat &>(v); }
inline glm::mat4 &to_glm(ApriMat4 &v) { return reinterpret_cast<glm::mat4 &>(v); }
inline glm::dmat4 &to_glm(ApriDMat4 &v) { return reinterpret_cast<glm::dmat4 &>(v); }

inline const glm::vec2 &to_glm(const ApriVec2 &v) { return reinterpret_cast<const glm::vec2 &>(v); }
inline const glm::vec3 &to_glm(const ApriVec3 &v) { return reinterpret_cast<const glm::vec3 &>(v); }
inline const glm::vec4 &to_glm(const ApriVec4 &v) { return reinterpret_cast<const glm::vec4 &>(v); }
inline const glm::dvec3 &to_glm(const ApriDVec3 &v) { return reinterpret_cast<const glm::dvec3 &>(v); }
inline const glm::quat &to_glm(const ApriQuat &v) { return reinterpret_cast<const glm::quat &>(v); }
inline const glm::mat4 &to_glm(const ApriMat4 &v) { return reinterpret_cast<const glm::mat4 &>(v); }

inline ApriVec3 from_glm(const glm::vec3 &v) { return {v.x, v.y, v.z}; }
inline ApriDVec3 from_glm(const glm::dvec3 &v) { return {v.x, v.y, v.z}; }
inline ApriQuat from_glm(const glm::quat &v) { return {v.x, v.y, v.z, v.w}; }
inline ApriMat4 from_glm(const glm::mat4 &m) {
	ApriMat4 r;
	std::memcpy(r.m, &m[0][0], sizeof(r.m));
	return r;
}

/* ============================================================
 * Macro helpers for managing
 * malloc & new(var)constructor() & deletion/free
 * ============================================================ */
#define APREND_MALLOC__T(var, T) T *var = (T *)malloc(sizeof(T))
#define APREND_CONSTRUCT__T(var, T) var = new (var) T()
/* One statement, so it is whole under an `if` with no braces. */
#define APREND_DESTRUCT__T(var, T) \
	do {                           \
		(var)->~T();               \
		free(var);                 \
	} while (0)

/* Binding sets per descriptor pool of a binding layout. Sets are small and a
 * layout with more than a handful is rare, so this is sized to make a second
 * pool unusual without reserving much for a layout that has one set. */
#define APREND_BINDING_SETS_PER_POOL 32

/* How many SPUDGPU_DESCRIPTOR_TYPE values there are; they are 0 to this - 1. */
#define APREND_DESCRIPTOR_TYPE_COUNT (SPUDGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE + 1)

struct aprend_binding_layout_core;

/* The size of one buffer of the material arena. A block larger than this
 * gets a buffer of its own. 64 KiB is also the most one uniform buffer
 * descriptor can see on D3D12. */
#define APREND_MATERIAL_ARENA_BUFFER_SIZE 65536u

/* One buffer of the material arena: device-local uniform memory, written
 * only by copies recorded at submit (aprend_material_stage_uploads), and the
 * ranges of it handed out, in units of the instance's
 * material_block_alignment. */
struct aprend_material_arena_buffer {
	aprend_uniform_buffer buffer{nullptr};
	aprend_range_list ranges{};
	/* Made for one block larger than an arena buffer, and destroyed when that
	 * block is given back. */
	bool dedicated{false};
};

/* An instance's material arena: where every material's owned uniform blocks
 * live, each at a fixed range for the material's life. Buffers are added a
 * buffer at a time. One left with no range is destroyed if another is empty
 * already and kept as the one spare if not; a dedicated one goes with its
 * block; the rest go with the instance. Used only on the thread using the instance: a range is taken at
 * aprend_material_create and given back by the material's release function,
 * once the GPU has finished with it. */
struct aprend_material_arena {
	std::vector<aprend_material_arena_buffer *> buffers{};
};

typedef struct aprend_instance_t {
	char *debug_name{nullptr};
	aprend_instance_t() = default;
	~aprend_instance_t();

	aprend_instance_desc desc{};
	/* What every recording of one frame index is made into: an allocator,
	 * and the SpudGPU lists recorded from it. aprend_instance_next_frame
	 * resets the allocator once it has waited for the frame that last used
	 * the index, which is when its memory comes back, and every list is free
	 * again. Before that, a list handed out stays handed out: one recording
	 * may still be running, and Vulkan can't reopen a pending command
	 * buffer. So a frame takes a list for each recording, and the vector
	 * holds as many as the busiest turn of the index needed. */
	struct command_pool {
		spudgpu_command_allocator allocator{nullptr};
		std::vector<spudgpu_command_list> lists{};
		/* lists[0, used) are handed out in this turn of the index. */
		size_t used{0};
	};
	/* Two pools for each index. Only one list of an allocator may be open
	 * at a time (spudgpu_begin_command_list), and the upload batch stays
	 * open while other recordings are made, so it is made from an allocator
	 * of its own. Batches follow one another, so one at a time is all that
	 * allocator ever has open. */
	struct command_frame {
		command_pool recordings{};
		command_pool batches{};
	};
	command_frame frame_commands[APREND_MAX_FRAMES_IN_FLIGHT]{};
	/* Counts aprend_instance_next_frame calls. A recording is made in one
	 * frame and submitted in the same one, which this tells apart from the
	 * next turn of the same index. */
	uint64_t frame_number{0};
	/* The upload batch: a list from the current frame's batches pool, open for
	 * recording, that texture updates, readbacks and framebuffer clears are
	 * recorded into instead of each being a submission (item 6). It goes
	 * ahead of the lists of the next submission, in that submission, and
	 * aprend_instance_next_frame and aprend_instance_wait_idle submit it if
	 * nothing else has. NULL with nothing recorded. upload_batch_open is
	 * false once it has been closed for a submission that then failed: it
	 * waits, closed, to go with the next. */
	spudgpu_command_list upload_batch{nullptr};
	bool upload_batch_open{false};
	/* The queue the batch's first call named; every later one names the
	 * same, and so does the submission that takes it. */
	spudgpu_command_queue upload_batch_queue{nullptr};
	/* Whether there is a batch waiting, for aprend_instance_retire on any
	 * thread: what is destroyed while one waits may be used by it, so it is
	 * released after the submission that will carry the batch. Written
	 * under release_mutex, with submit_serial, on the thread using the
	 * instance. */
	bool upload_batch_waiting{false};
	/* The SPUDGPU_FEATURE_* bits desc.device was created with
	 * (spudgpu_get_device_features), read once at creation. Every feature
	 * check reads this, never the adapter's supported_features. */
	SPUDGPU_FEATURES features{0};
	/* SPUDGPU_ADAPTER_PROPERTIES::unified_memory of desc.device's adapter,
	 * read once at creation: a property, not a feature. Decides where
	 * vertex, index and storage buffers are kept (aprend_buffer_store). */
	bool unified_memory{false};
	/* SPUDGPU_FEATURE_DEVICE_MAPPABLE_MEMORY in features: whether a buffer a
	 * shader may write can be written directly too
	 * (SPUDGPU_MEMORY_KIND_DEVICE_MAPPABLE). */
	bool device_mappable_memory{false};
	/* SPUDGPU_ADAPTER_PROPERTIES::uniform_buffer_offset_alignment and
	 * ::storage_buffer_offset_alignment of desc.device's adapter, read with
	 * it: what a
	 * binding set entry's `_offset` into a uniform or storage buffer is a
	 * multiple of. Powers of two. */
	uint32_t uniform_buffer_offset_alignment{0};
	uint32_t storage_buffer_offset_alignment{0};
	/* Where a material's uniform block may start in the arena, and what each
	 * block's range is a multiple of: uniform_buffer_offset_alignment, or 16
	 * where that is less, since std140 rounds a block to 16. It divides
	 * APREND_MATERIAL_ARENA_BUFFER_SIZE, which every power of two up to 256
	 * does. */
	uint32_t material_block_alignment{0};
	/* SPUDGPU_ADAPTER_PROPERTIES::max_draw_indirect_count of desc.device's
	 * adapter on a device with SPUDGPU_FEATURE_MULTI_DRAW_INDIRECT, else 1,
	 * read once at creation: the most draws one indirect SpudGPU call takes.
	 * An indirect draw command with more is recorded as several calls. */
	uint32_t max_draw_indirect_count{1};
	/* Which copy of every buffer set is current: 0 to
	 * desc.frames_in_flight - 1. */
	uint32_t frame_index{0};
	/* Transient memory (render/aprendtransient.h): a run of chunks for each
	 * frame index, empty until the first aprend_transient_allocate. */
	aprend_transient_arena transient{};
	/* The owned uniform blocks of every material (render/aprendmaterial.h). */
	aprend_material_arena material_arena{};
	/* Paces frames. Every aprend_command_list_submit signals it to the next
	 * submit_serial, and frame_last_serial[i] is the serial of the last
	 * submission made while frame index i was current: what
	 * aprend_instance_next_frame waits for before index i is current again.
	 * 0 for an index nothing has been submitted in, which the fence, starting
	 * at 0, has already reached. */
	spudgpu_fence frame_fence{nullptr};
	/* Atomic because a destroy call reads it from whatever thread it is
	 * made on (aprend_instance_retire); only a submission writes it, under
	 * release_mutex, with upload_batch_waiting. */
	std::atomic<uint64_t> submit_serial{0};
	uint64_t frame_last_serial[APREND_MAX_FRAMES_IN_FLIGHT]{};
	/* The one queue everything of this instance is submitted on: the queue
	 * of its first submission, NULL until then. The serials are only an
	 * order of completion on a single queue, so aprend_instance_submit
	 * refuses any other. */
	spudgpu_command_queue submit_queue{nullptr};
	/* The release queue: what has been destroyed by its owner and may still
	 * be in use by work the GPU hasn't finished. An entry is released once
	 * the fence has reached its serial, which is submit_serial as it was
	 * when the object was retired: everything submitted up to then may use
	 * the object, and nothing submitted after may. Serials never go down
	 * along the queue, so it is released strictly from the front, in the
	 * order things were retired. Entries before release_head are done. */
	struct release_entry {
		void (*release)(void *object);
		void *object;
		uint64_t serial;
	};
	std::vector<release_entry> release_queue{};
	size_t release_head{0};
	/* Every binding layout core alive (aprend_binding_layout_core): what a
	 * new binding layout looks for one of the same bindings in. Written only
	 * by the thread using the instance, by creating a layout and by releasing
	 * a core's last holder. */
	std::vector<aprend_binding_layout_core *> binding_layout_cores{};
	/* What Aprend draws with itself (aprendbuiltin_internal.hpp), made as
	 * first needed. */
	aprend_builtins builtins{};
	/* Render targets a built-in effect's command takes at compile
	 * (aprendrendertargets_internal.hpp). */
	aprend_render_target_pool render_targets{};
	/* Every command list compiled and not yet submitted, reset or compiled
	 * again: what a destroy call, from any thread, and a resize look through
	 * for lists that use what they take away (aprend_instance_forget). A
	 * list here is not changed by its own thread until it is taken off, so
	 * what it uses can be read under release_mutex. Under release_mutex. */
	std::vector<aprend_command_list> compiled_lists{};
	/* The thread inside one of the calls APREND_ENTER guards, and how deep
	 * (a guarded call may make another): Debug builds' check of the rule in
	 * aprendcontext.h ("Threads"). In every build, so the struct is the same
	 * in each; only Debug builds read or write them. */
	std::atomic<std::thread::id> entered_thread{};
	uint32_t entered_depth{0};
	/* Guards release_queue, release_head and upload_batch_waiting, and makes
	 * a new submit_serial and the batch it carried change together for
	 * aprend_instance_retire; and compiled_lists, with each list's
	 * listed and stale. It is what
	 * lets a destroy call come from any thread: retiring only pushes an
	 * entry under it. It is never held while a release function runs. */
	std::mutex release_mutex{};
} aprend_instance_t;

/* Debug builds' check of the rule in aprendcontext.h ("Threads"): one
 * thread at a time inside an instance, which may be a different thread from
 * call to call. Each call that meets in state the instance holds (the ones
 * that header lists) opens with APREND_ENTER; a second thread entering while
 * one is inside is a broken rule, which would otherwise be a silent race,
 * and is printed with the call it was in - as Vulkan's validation layer
 * reports an object used by two threads at once. The same thread entering
 * again (a guarded call making another) is not. Nothing is refused: the
 * call goes on, as it would in Release, where the guard is nothing. */
class aprend_instance_entry {
  public:
	aprend_instance_entry(aprend_instance instance, const char *who) {
#if _DEBUG
		const std::thread::id me = std::this_thread::get_id();
		std::thread::id expected{};
		if (instance->entered_thread.compare_exchange_strong(expected, me)) {
			instance_                = instance;
			instance->entered_depth = 1;
		} else if (expected == me) {
			instance_ = instance;
			++instance->entered_depth;
		} else {
			printf("aprend: %s: called while another thread is inside the instance; an instance is used by one thread at a time (aprendcontext.h, "
			       "\"Threads\")\n",
			       who);
		}
#else
		(void)instance;
		(void)who;
#endif
	}
	~aprend_instance_entry() {
#if _DEBUG
		if (instance_ && --instance_->entered_depth == 0)
			instance_->entered_thread.store(std::thread::id{});
#endif
	}
	aprend_instance_entry(const aprend_instance_entry &)            = delete;
	aprend_instance_entry &operator=(const aprend_instance_entry &) = delete;

  private:
#if _DEBUG
	/* NULL if this entry found another thread inside. */
	aprend_instance instance_{nullptr};
#endif
};
#define APREND_ENTER(instance, who) aprend_instance_entry aprend_entry_guard_(instance, who)

/* Queues [object], a handle of [instance] or something one owns, to be
 * released with [release] once every submission made so far has finished,
 * and with an upload batch waiting, once the submission that carries it has
 * too: the batch may have recorded work on the object. Every destroy call of
 * a GPU-backed handle goes through this, so the caller of one doesn't have
 * to know the GPU is finished with it.
 *
 * Safe from any thread, at the same time as anything else on the instance:
 * it takes the serial and pushes the entry under release_mutex and does
 * nothing more. It never releases anything itself, since a release function
 * touches what the one-thread rule guards (SpudGPU objects, a binding
 * layout's pools). Releasing is aprend_instance_collect's, on the thread
 * that is using the instance.
 *
 * Never releases early. With no memory to queue the entry, the object is
 * left unreleased and that is printed: there is no thread here it would be
 * safe to release it on, and a leak under memory exhaustion is the one
 * outcome that can't make things worse.
 *
 * What it can't see is a command list that was compiled against the object
 * and not yet submitted: the serial is taken at retirement, so such a list,
 * submitted afterwards, would run after the object may already be gone. So
 * every destroy call of something a compile records also calls
 * aprend_instance_forget, and such a submission is refused. */
void aprend_instance_retire(
    aprend_instance instance,
    void *object,
    void (*release)(void *object));

/* Marks stale every compiled, unsubmitted command list that uses [used]: a
 * texture's aprend_texture_layouts, a buffer's aprend_buffer_store, a
 * material, a GPU timer, a swap chain, or a handle in the list's
 * used_handles (a pipeline, binding set, view, sampler or uniform buffer).
 * aprend_command_list_submit refuses a stale list, so it never reads [used]
 * again and the GPU never runs commands that name it; compiling it again
 * clears the mark. Called by the destroy calls of all of those, from any
 * thread; by a texture's resize, which replaces what its layouts describe;
 * and by aprend_binding_set_update, whose old descriptor sets the list
 * would bind. Takes release_mutex. Needs no memory. */
void aprend_instance_forget(
    aprend_instance instance,
    const void *used);
/* Releases every queued entry whose serial the fence has reached, each
 * outside release_mutex. Only for the thread using the instance under the
 * one-thread rule: called after each submission and each wait, never from a
 * destroy call. Cheap when there is nothing to do. */
void aprend_instance_collect(aprend_instance instance);

/* The release function of a handle struct T: its destructor, then free.
 * For aprend_instance_retire. */
template <typename T> void aprend_release_handle(void *object) {
	T *handle = (T *)object;
	handle->~T();
	free(handle);
}
/* Release functions for SpudGPU objects retired on their own, apart from the
 * handle that owned them. */
inline void aprend_release_spudgpu_buffer(void *object) { spudgpu_destroy_buffer((spudgpu_buffer)object); }
inline void aprend_release_spudgpu_image(void *object) { spudgpu_destroy_image((spudgpu_image)object); }
inline void aprend_release_spudgpu_image_view(void *object) { spudgpu_destroy_image_view((spudgpu_image_view)object); }
inline void aprend_release_spudgpu_command_list(void *object) { spudgpu_destroy_command_list((spudgpu_command_list)object); }
inline void aprend_release_spudgpu_command_allocator(void *object) { spudgpu_destroy_command_allocator((spudgpu_command_allocator)object); }

/* The most lists one aprend_instance_submit takes from its caller: a command
 * list's upload list and its own. The upload batch is added to them. */
#define APREND_SUBMIT_MAX_LISTS 2

/* The one way anything of [instance] reaches a queue. Refuses, with
 * SPUDRESULT_GPU_INVALID_COMMAND_QUEUE and nothing submitted, a [queue] that
 * is not the one the instance's first submission went on, or not the one the
 * waiting upload batch was recorded for. Submits the upload batch, if one is
 * waiting, and then [lists] (at most APREND_SUBMIT_MAX_LISTS, and none only
 * when a batch waits) on [queue], synchronized with [swap_chain] if
 * it isn't NULL, and signals the instance's frame fence to the next serial,
 * which it records against the current frame index and hands back through
 * [out_serial] (NULL to skip). A failed submission takes no serial, and
 * leaves the batch waiting, closed, for the next. */
SPUDRESULT aprend_instance_submit(
    aprend_instance instance,
    spudgpu_command_queue queue,
    spudgpu_command_list *lists,
    uint32_t list_count,
    spudgpu_swap_chain swap_chain,
    uint64_t *out_serial);
/* Blocks until the submission that took [serial], and every one before it,
 * has finished. Serial 0 returns at once. */
SPUDRESULT aprend_instance_wait_serial(
    aprend_instance instance,
    uint64_t serial);

/* A SpudGPU list of the current frame's recordings pool, for one recording
 * to be submitted in this frame. Not handed out again until
 * aprend_instance_next_frame has come round to this index and waited, so it
 * is never reopened while its recording may run. Made if the frame has none
 * left. Opened and closed again before the next is taken from the pool:
 * the upload batch, which stays open, has a pool of its own. */
SPUDRESULT aprend_instance_take_command_list(
    aprend_instance instance,
    spudgpu_command_list *out_list);

/* The upload batch, open for recording, for work to go on [queue]: the one
 * waiting, or a new one from the current frame's batches pool. Refuses, with
 * SPUDRESULT_GPU_INVALID_COMMAND_QUEUE, a queue that is not the instance's
 * or not the batch's. A batch left closed by a failed submission is
 * submitted first, and its failure is this call's. What is recorded runs
 * after everything submitted before it and ahead of everything submitted
 * after. */
SPUDRESULT aprend_instance_open_upload_batch(
    aprend_instance instance,
    spudgpu_command_queue queue,
    spudgpu_command_list *out_list);

/* Submits the upload batch on its own, if one is waiting, and hands back
 * through [out_serial] the serial to wait for to know it has run: the
 * batch's, or with none waiting the last one taken. */
SPUDRESULT aprend_instance_flush_upload_batch(
    aprend_instance instance,
    uint64_t *out_serial);

/* Where the bytes of a vertex, index or storage buffer are kept, chosen from
 * the device's memory model when the buffer is created.
 *
 * On a device whose memory is the system's, [buffer] is mapped and a write
 * goes straight into it: there is one memory, so nothing is gained by a
 * second copy. A vertex or index buffer is SPUDGPU_MEMORY_KIND_UPLOAD. A
 * storage buffer, which a shader may write, is
 * SPUDGPU_MEMORY_KIND_DEVICE_MAPPABLE where the device has that kind, and is
 * staged like the case below where it doesn't.
 *
 * On a device with memory of its own, [buffer] is SPUDGPU_MEMORY_KIND_DEVICE:
 * in that memory, where the GPU reads it without crossing the bus, and never
 * mapped ([staged]). A write is staged in an UPLOAD buffer and noted in
 * [uploads]; the next aprend_command_list_submit of a list that uses the
 * buffer copies each across ahead of the list's own commands, and empties
 * the list once the submission has gone in.
 *
 * What a write is staged in follows APREND_BUFFER_WRITES:
 * - MANY: [staging], an UPLOAD buffer of the same size that holds
 *   everything ever written, for life. An upload names a range of it, and a
 *   write merges into the ranges it overlaps or touches, so the list stays
 *   sorted and disjoint; bytes between two ranges are never copied.
 * - ONCE: no [staging]. Each write makes an UPLOAD buffer of its own size,
 *   which its upload owns: after the submission that copies it, it is
 *   retired, and released once that submission has run. Writes are not
 *   merged, and are copied in the order they were made. A pool's blocks are
 *   stores of this kind, written by the pool once a range.
 *
 * Every write is flushed (spudgpu_flush_buffer), whichever buffer it lands
 * in.
 *
 * Uniform buffers don't use this: they are small and rewritten every frame,
 * and are UPLOAD memory, mapped for life, on every device. */
struct aprend_buffer_upload {
	/* Where the bytes are: [staging] for a MANY store, a buffer the upload
	 * owns for a ONCE store. */
	spudgpu_buffer source{nullptr};
	uint64_t source_offset{0};
	/* Where they go in the store's [buffer]. */
	uint64_t offset{0};
	uint64_t size{0};
};

struct aprend_buffer_store {
	aprend_instance instance{nullptr};
	/* What the GPU reads, and what views and descriptors are made of. */
	spudgpu_buffer buffer{nullptr};
	/* [buffer] is in the device's memory and written through uploads. */
	bool staged{false};
	/* APREND_BUFFER_WRITES_ONCE: the buffer's update call refuses. */
	bool written_once{false};
	/* A MANY store's staging buffer; NULL for a ONCE store and for one
	 * written directly. */
	spudgpu_buffer staging{nullptr};
	uint64_t size{0};
	/* The state [buffer] is used in, which a copy into it leaves it in. */
	SPUDGPU_RESOURCE_STATE use_state{SPUDGPU_RESOURCE_STATE_COMMON};
	/* Writes staged and not yet copied. A MANY store's are sorted by
	 * offset, and no two overlap or touch; a ONCE store's are in the order
	 * they were written. */
	std::vector<aprend_buffer_upload> uploads{};
	/* The buffer is used in more than one state: a storage buffer that is
	 * also an indirect argument buffer. A command list tracks the state of
	 * such a store and moves it (aprend_command_list_t::buffer_states); a
	 * store that is only ever used in [use_state] needs no tracking. */
	bool multi_state{false};
};

/* Creates [store]'s buffers, of [size] bytes, for [usage], and written as
 * [writes] says (ONCE or MANY, checked by the caller). On failure the store
 * holds nothing. */
SPUDRESULT aprend_buffer_store_create(
    aprend_buffer_store *store,
    aprend_instance instance,
    uint64_t size,
    SPUDGPU_BUFFER_USAGE usage,
    SPUDGPU_RESOURCE_STATE use_state,
    APREND_BUFFER_WRITES writes);
/* Destroys whatever [store] holds, the buffers its uploads own included.
 * Views of its buffer go first. */
void aprend_buffer_store_destroy(aprend_buffer_store *store);
/* Copies [size] bytes from [data] to [offset]. The range is the caller's to
 * have checked against the store's size. SPUDRESULT_OUT_OF_MEMORY, with
 * nothing written, if there is no memory to note the write. Doesn't look at
 * [written_once]: that is the update calls' to refuse. */
SPUDRESULT aprend_buffer_store_write(
    aprend_buffer_store *store,
    uint64_t offset,
    uint64_t size,
    const void *data);
/* After a submission that copies [store]'s uploads has gone in: every
 * buffer an upload owns is retired, and the list emptied. */
void aprend_buffer_store_uploads_submitted(aprend_buffer_store *store);

typedef struct aprend_gpu_timer_t {
	char *debug_name{nullptr};
	aprend_gpu_timer_t() = default;
	~aprend_gpu_timer_t();
	aprend_instance instance{nullptr};
	uint32_t slot_count{0};
	/* One SpudGPU timestamp set for each frame index, of two entries a slot:
	 * slot i's start at 2i and its end at 2i + 1. */
	spudgpu_timestamp_set sets[APREND_MAX_FRAMES_IN_FLIGHT]{};
	/* For each frame index and slot (index * slot_count + slot), the frame
	 * number of the last submission whose pass timed it; UINT64_MAX for
	 * none. aprend_gpu_timer_get_ms reads a slot only when this is the frame
	 * `frames_in_flight` before the current one. */
	std::vector<uint64_t> written_frame{};
} aprend_gpu_timer_t;

/* One mip of one layer of one image: what a command list keeps a layout
 * for (aprend_command_list_t::layout_uses), and its index's key. */
struct aprend_subresource_key {
	spudgpu_image image;
	uint32_t mip;
	uint32_t layer;
	bool operator==(const aprend_subresource_key &other) const { return image == other.image && mip == other.mip && layer == other.layer; }
};
struct aprend_subresource_key_hash {
	size_t operator()(const aprend_subresource_key &key) const {
		const size_t h = std::hash<const void *>()(key.image);
		return h ^ (((size_t)key.mip << 16 | (size_t)key.layer) * 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2));
	}
};

typedef struct aprend_command_list_t {
	char *debug_name{nullptr};
	aprend_command_list_t() = default;
	~aprend_command_list_t();
	aprend_instance instance{nullptr};
	/* The SpudGPU list the last compile recorded into: one of the instance's
	 * frame commands, taken in the frame the list was compiled in
	 * (compiled_frame_number), and the instance's, not the list's. NULL
	 * before the first compile and after a submit, since a submitted
	 * recording may still be running and the next compile takes another. A
	 * compile in the same frame that finds one not submitted records into it
	 * again. The copies of the staged buffers the list uses are recorded at
	 * submit into another list taken there, and submitted ahead of it. */
	spudgpu_command_list cmd_list{nullptr};
	/* Every staged buffer store the last compile found in use, once each. */
	std::vector<aprend_buffer_store *> staged_stores{};
	/* Every buffer store the last compile found in use, staged or not, once
	 * each: what aprend_instance_forget looks for. */
	std::vector<aprend_buffer_store *> used_stores{};
	/* Every handle the last compile's recording names that a submission
	 * reads only through the recorded commands: pipelines, binding sets and
	 * what they hold (views, samplers, uniform buffers), and the views of
	 * pass targets and of the built-in effects' commands. What
	 * aprend_instance_forget looks for besides the above, so a list that
	 * names a destroyed one, or a binding set updated since, is refused at
	 * submit. A hash set: a list may name thousands, and a destroy call
	 * looks in every compiled list. */
	std::unordered_set<const void *> used_handles{};
	/* The state of each multi-state buffer store as the compile walks the
	 * list, from the first point the list uses it. Every such store starts
	 * the list in its use_state: aprend_command_list_submit puts it there
	 * ahead of the list's own commands, whether or not it also copies into
	 * it, so the barriers compiled here always name the state the buffer is
	 * really in. Kept after the compile for submit to know which stores
	 * those are. */
	struct buffer_state {
		aprend_buffer_store *store;
		SPUDGPU_RESOURCE_STATE state;
	};
	std::vector<buffer_state> buffer_states{};
	/* The instance's frame index and frame number at the last compile. A
	 * list is submittable only in the frame it was compiled in: its
	 * recording is made into that frame's allocator, which the next turn of
	 * the index resets, and a list that binds a per-frame set or a transient
	 * span reads that frame's copies and memory as well. */
	uint32_t compiled_frame{0};
	uint64_t compiled_frame_number{0};
	/* The timer slots the last compile's passes time: submit marks each as
	 * written in the compiled frame. */
	struct timer_use {
		aprend_gpu_timer timer;
		uint32_t slot;
	};
	std::vector<timer_use> timer_uses{};
	/* What the last compile took from the instance's render target pool,
	 * given back if that compile is never submitted (compiled again, reset,
	 * failed, destroyed) and kept by the submission if it is. */
	std::vector<aprend_pool_take> pool_takes{};
	/* A copy of each BEGIN_LABEL's text, so recorded commands never point at
	 * caller memory. A deque, as a vector's growth would move a short
	 * string's characters along with it. */
	std::deque<std::string> label_storage{};
	/* Every material a SET_MATERIAL of the last compile names, once each:
	 * submit writes their uniform values into the frame's copy first. */
	std::vector<aprend_material> materials{};
	std::vector<APREND_COMMAND> commands{};
	/* Owned copies of each BEGIN_RENDERING's color target array, so recorded
	 * commands never point at caller memory. Each inner vector's buffer stays
	 * put when the outer vector grows (moves don't reallocate it). */
	std::vector<std::vector<aprend_color_target>> color_target_storage{};
	/* The same for the arrays of SET_VERTEX_BUFFERS, SET_VIEWPORTS and
	 * SET_SCISSOR_RECTS. */
	std::vector<std::vector<aprend_vertex_buffer>> vertex_buffer_storage{};
	std::vector<std::vector<SPUDGPU_VIEWPORT>> viewport_storage{};
	std::vector<std::vector<SPUDGPU_SCISSOR_RECT>> scissor_rect_storage{};
	/* And the bytes of PUSH_CONSTANTS. */
	std::vector<std::vector<uint8_t>> push_constant_storage{};
	/* Set by aprend_send_command on a malformed command; fails compile until reset. */
	bool recording_error{false};

	/* The layout transitions of the last compile, one entry for each mip of
	 * each layer of each texture the list uses. Compiling never touches a
	 * texture's tracked layouts (what the GPU will see once everything
	 * submitted so far has run) - only a successful
	 * aprend_command_list_submit commits final_layout to them. initial_layout
	 * is what this list's first barrier on that subresource assumes, checked
	 * against the tracked layout at submit. */
	struct layout_use {
		spudgpu_image image;
		uint32_t mip;
		uint32_t layer;
		/* The texture's tracked layout for (mip, layer), 2D or 3D. */
		SPUDGPU_IMAGE_LAYOUT *tracked;
		/* What [tracked] is in: the texture's, for aprend_instance_forget
		 * to find. */
		const void *tracking;
		SPUDGPU_IMAGE_LAYOUT initial_layout;
		SPUDGPU_IMAGE_LAYOUT final_layout;
		/* Since the last barrier the list recorded on the subresource: a
		 * shader may have used it through a storage slot, and may have
		 * written it. Both start true, since a submission before this one
		 * may have; a barrier clears both. A storage use needs a barrier
		 * first if [storage_written], or if it writes and
		 * [storage_accessed]. */
		bool storage_accessed;
		bool storage_written;
	};
	std::vector<layout_use> layout_uses{};
	/* Where each subresource's entry is in layout_uses: a list may touch
	 * every mip of every layer of many textures, and each barrier looks its
	 * subresources up. Cleared with layout_uses. */
	std::unordered_map<aprend_subresource_key, size_t, aprend_subresource_key_hash> layout_index{};
	/* The same for each storage buffer store the list uses through a set,
	 * from the first point it does. */
	struct storage_hazard {
		aprend_buffer_store *store;
		bool accessed;
		bool written;
	};
	std::vector<storage_hazard> storage_hazards{};
	/* The last compile succeeded, so the spudgpu list is closed and submittable. */
	bool compiled{false};
	/* In the instance's compiled_lists: from a successful compile until the
	 * list is submitted, reset, compiled again or destroyed. Under the
	 * instance's release_mutex. */
	bool listed{false};
	/* Something the listed compile uses was destroyed or resized since
	 * (aprend_instance_forget): submit refuses it. Under release_mutex. */
	bool stale{false};
	/* Set by a compiled APREND_COMMAND_PRESENT_TEXTURE: the swap chain and the
	 * back buffer index the list copies into. Submit uses the swap-chain-
	 * synchronized path for such a list. */
	aprend_swap_chain present_swap_chain{nullptr};
	uint32_t present_image_index{0};
	/* present_swap_chain's generation at the compile. */
	uint64_t present_generation{0};
} aprend_command_list_t;

#define APREND_SWAP_CHAIN_NO_IMAGE 0xFFFFFFFFu

typedef struct aprend_swap_chain_t {
	char *debug_name{nullptr};
	aprend_swap_chain_t() = default;
	~aprend_swap_chain_t();
	aprend_instance instance{nullptr};
	aprend_swap_chain_desc desc{};
	spudgpu_swap_chain swap_chain{nullptr};
	/* APREND_SWAP_CHAIN_NO_IMAGE until aprend_swap_chain_acquire. */
	uint32_t acquired_image{APREND_SWAP_CHAIN_NO_IMAGE};
	/* A list presenting into acquired_image has been submitted. */
	bool submitted{false};
	/* Counts the SpudGPU swap chains made for this handle: creation and each
	 * resize that replaced the back buffers. A list that presents records
	 * the count it was compiled against, and submit refuses it once a resize
	 * has moved it on, since a new swap chain's back buffer may have the
	 * index of the old one's. */
	uint64_t generation{0};
} aprend_swap_chain_t;

typedef struct aprend_uniform_buffer_t {
	char *debug_name{nullptr};
	aprend_uniform_buffer_t() = default;
	~aprend_uniform_buffer_t();
	/* For aprend_instance_retire when the handle is destroyed. */
	aprend_instance instance{nullptr};
	/* The whole block, bound by binding sets with an offset and range. */
	spudgpu_buffer buffer{nullptr};
	/* The block's size in bytes, a multiple of 16. */
	uint32_t size{0};
	/* The buffer's own copy of its layout: the uniforms and their names in
	 * one allocation, NULL with no uniforms. Empty for one of the device's
	 * memory. */
	aprend_uniform_layout layout{};
	/* Where the CPU writes the block: UPLOAD memory mapped for the buffer's
	 * life (aprend_uniform_buffer_create). NULL for one in the device's own
	 * memory (aprend_uniform_buffer_create_device, the material arena's),
	 * which only the GPU's copies write and every update call refuses. */
	void *mapped{nullptr};
} aprend_uniform_buffer_t;

typedef struct aprend_vertex_buffer_t {
	char *debug_name{nullptr};
	aprend_vertex_buffer_t() = default;
	~aprend_vertex_buffer_t();
	/* For aprend_instance_retire when the handle is destroyed. */
	aprend_instance instance{nullptr};
	aprend_buffer_store store{};
	/* The buffer's size in bytes. */
	uint64_t size{0};
	/* A SpudGPU view of the whole buffer for each stride a pipeline has read
	 * it at, made by the first compile that binds it at that stride
	 * (aprend_vertex_buffer_view): a SpudGPU vertex buffer view carries its
	 * stride (D3D12's does), and the stride is the pipeline's. Usually one.
	 * Destroyed with the buffer. */
	struct stride_view {
		uint32_t stride;
		spudgpu_buffer_view view;
	};
	std::vector<stride_view> views{};
} aprend_vertex_buffer_t;

/* The view of [buffer] at [stride], made the first time it is asked for.
 * On the thread using the instance. SPUDRESULT_OUT_OF_MEMORY, or SpudGPU's
 * failure, with nothing made. */
SPUDRESULT aprend_vertex_buffer_view(
    aprend_vertex_buffer buffer,
    uint32_t stride,
    spudgpu_buffer_view *out_view);

typedef struct aprend_index_buffer_t {
	char *debug_name{nullptr};
	aprend_index_buffer_t() = default;
	~aprend_index_buffer_t();
	/* For aprend_instance_retire when the handle is destroyed. */
	aprend_instance instance{nullptr};
	aprend_buffer_store store{};
	spudgpu_buffer_view buffer_view{nullptr};
	spudgpu_buffer_view_desc buffer_view_desc{{SPUD_STRUCT_TYPE_GPU_BUFFER_VIEW_DESC, nullptr}};
	uint32_t index_count{0};
	APREND_INDEX_STRIDE index_stride{APREND_INDEX_STRIDE_NONE};
} aprend_index_buffer_t;

typedef struct aprend_storage_buffer_t {
	char *debug_name{nullptr};
	aprend_storage_buffer_t() = default;
	~aprend_storage_buffer_t();
	/* For aprend_instance_retire when the handle is destroyed. */
	aprend_instance instance{nullptr};
	aprend_buffer_store store{};
	spudgpu_buffer_view buffer_view{nullptr};
	spudgpu_buffer_view_desc buffer_view_desc{{SPUD_STRUCT_TYPE_GPU_BUFFER_VIEW_DESC, nullptr}};
	uint64_t size{0};
	APREND_STORAGE_BUFFER_USAGE usage{APREND_STORAGE_BUFFER_USAGE_NONE};
} aprend_storage_buffer_t;

/* One uniform buffer for each frame in flight, all of one layout.
 * buffers[instance->frame_index] is the current one. */
typedef struct aprend_uniform_buffer_set_t {
	char *debug_name{nullptr};
	aprend_uniform_buffer_set_t() = default;
	~aprend_uniform_buffer_set_t();
	aprend_instance instance{nullptr};
	aprend_uniform_buffer buffers[APREND_MAX_FRAMES_IN_FLIGHT]{};
	uint32_t buffer_count{0};
} aprend_uniform_buffer_set_t;

/* The same for storage buffers, all of one size. */
typedef struct aprend_storage_buffer_set_t {
	char *debug_name{nullptr};
	aprend_storage_buffer_set_t() = default;
	~aprend_storage_buffer_set_t();
	aprend_instance instance{nullptr};
	aprend_storage_buffer buffers[APREND_MAX_FRAMES_IN_FLIGHT]{};
	uint32_t buffer_count{0};
} aprend_storage_buffer_set_t;

typedef struct aprend_shader_t {
	char *debug_name{nullptr};
	aprend_shader_t() = default;
	~aprend_shader_t();
	aprend_instance instance{nullptr};
	spudgpu_shader_module shader_module{nullptr};
	/* The one stage the shader was created for, so a pipeline can refuse a
	 * shader put in the wrong place. */
	SPUDGPU_SHADER_STAGE stage{SPUDGPU_SHADER_STAGE_NONE};
	/* What the SPIR-V declares, read at creation. It points into itself, so
	 * it lives here and is never copied. */
	aprend_reflection_store reflection{};
} aprend_shader_t;

/* A sampled or storage image binding a pipeline's shaders use, and what
 * they read it as: a command list checks the view a set gives it against
 * this when it binds the set for the pipeline. */
struct aprend_image_expectation {
	uint32_t set;
	uint32_t binding;
	APREND_TEXTURE_VIEW_SHAPE shape;
	APREND_SHADER_SCALAR_TYPE scalar_type;
	/* A storage image's declared format (`layout(rgba8)`), which its view
	 * has to be in; UNKNOWN for one declared without, and for anything else. */
	SPUDGPU_FORMAT format;
};

/* A storage image or storage buffer binding a pipeline's shaders write:
 * declared without `readonly` by a shader whose entry point uses it. A
 * command list orders the next use of what a set holds there after this
 * pipeline's draws or dispatch; one no shader writes is only read, and two
 * reads in a row need no barrier. */
struct aprend_storage_write {
	uint32_t set;
	uint32_t binding;
};

/* Whether [writes] holds [binding] of the set at [slot]. */
inline bool aprend_storage_writes_hold(const std::vector<aprend_storage_write> &writes, uint32_t slot, uint32_t binding) {
	for (const aprend_storage_write &write : writes)
		if (write.set == slot && write.binding == binding)
			return true;
	return false;
}

/* False, printed, if a texture view [set] holds is not what [expected]
 * declares at its binding, for a pipeline that has [set] at [slot]: another
 * shape; a stencil read as anything but an unsigned integer, or a depth read
 * as one; a color format whose kind of number (float, normalized or not,
 * signed or unsigned integer) is not the one the shader reads; or, for a
 * storage image declared with a format, a view in another. [who] names the
 * command. */
bool aprend_binding_set_matches_images(
    const aprend_binding_set_t *set,
    uint32_t slot,
    const std::vector<aprend_image_expectation> &expected,
    const char *who);

typedef struct aprend_graphics_pipeline_t {
	char *debug_name{nullptr};
	aprend_graphics_pipeline_t() = default;
	~aprend_graphics_pipeline_t();
	aprend_graphics_pipeline_desc desc{};
	aprend_instance instance{nullptr};
	spudgpu_shader_pipeline pipeline{nullptr};
	/* One allocation holding the vertex entry point's name and then the
	 * fragment's, each with its terminator. desc's two entry point pointers
	 * point into it, not at the caller's strings. */
	char *entry_points{nullptr};
	/* Stride of each counted vertex binding: what a draw binds the vertex
	 * buffer in that slot at, and checks its vertices against. */
	uint32_t vertex_strides[APREND_MAX_VERTEX_BINDINGS]{};
	/* Where each counted binding's furthest element ends in one vertex: the
	 * last vertex a draw reads ends that far past its start. */
	uint32_t vertex_extents[APREND_MAX_VERTEX_BINDINGS]{};
	/* Where the furthest push constant range ends; 0 with no ranges. */
	uint32_t push_constant_end{0};
	/* One allocation holding every vertex binding's elements, then their
	 * names: desc's layouts point into it. NULL with no vertex input. */
	char *vertex_elements{nullptr};
	/* Created with a mesh shader in place of a vertex shader: it is run
	 * with DISPATCH_MESH and reads no vertex buffers. */
	bool is_mesh{false};
	/* The images its shaders use, from their reflection. */
	std::vector<aprend_image_expectation> images{};
	/* The storage bindings its shaders write, from their reflection. */
	std::vector<aprend_storage_write> storage_writes{};
} aprend_graphics_pipeline_t;

typedef struct aprend_compute_pipeline_t {
	char *debug_name{nullptr};
	aprend_compute_pipeline_t() = default;
	~aprend_compute_pipeline_t();
	aprend_compute_pipeline_desc desc{};
	aprend_instance instance{nullptr};
	spudgpu_compute_pipeline pipeline{nullptr};
	/* The pipeline's copy of the entry point's name; desc's pointer points
	 * at it, not at the caller's string. */
	char *entry_point{nullptr};
	/* Where the furthest push constant range ends; 0 with no ranges. */
	uint32_t push_constant_end{0};
	/* The images its shader uses, from its reflection. */
	std::vector<aprend_image_expectation> images{};
	/* The storage bindings its shader writes, from its reflection. */
	std::vector<aprend_storage_write> storage_writes{};
} aprend_compute_pipeline_t;

/* The part of a binding layout SpudGPU sees: its bindings without names,
 * the SpudGPU layout made of them, and the pools its sets come from. Every
 * aprend_binding_layout of an instance with the same bindings (number, type,
 * count, stages; in any order) shares one core, so a set of one is a set of
 * all of them and a pipeline declaring one takes a set of any: they are one
 * SpudGPU object, on every backend.
 *
 * Held by each layout handle and each binding set made through one; freed,
 * and taken off the instance's list, when the last holder is released. Every
 * holder is released on the thread using the instance (a handle's or set's
 * release function, or a failed create), so [holders] needs no lock. Plain
 * data: calloc'd, freed with free. */
struct aprend_binding_layout_core {
	aprend_instance instance;
	/* Sorted by _binding; _name is NULL in every one. */
	aprend_binding_desc bindings[APREND_MAX_BINDINGS_PER_LAYOUT];
	uint32_t binding_count;
	spudgpu_descriptor_set_layout layout;
	/* The pools this core's sets come from, each sized for
	 * APREND_BINDING_SETS_PER_POOL sets. A pool is added when the last one is
	 * full; none is released before the core. */
	spudgpu_descriptor_pool *pools;
	uint32_t pool_count;
	/* Sets allocated so far from the last pool. */
	uint32_t sets_in_last_pool;
	/* Sets whose aprend_binding_set was destroyed, handed out again before
	 * anything new is allocated: SpudGPU releases sets by the pool, not one
	 * at a time. Always has room for every set of every pool. */
	spudgpu_descriptor_set *free_sets;
	uint32_t free_set_count;
	/* Layout handles and binding sets holding the core. */
	uint32_t holders;
};

/* Drops one holder of [core], and frees it when that was the last. NULL is
 * accepted. */
void aprend_binding_layout_core_release(aprend_binding_layout_core *core);

typedef struct aprend_binding_layout_t {
	char *debug_name{nullptr};
	aprend_binding_layout_t() = default;
	~aprend_binding_layout_t();
	aprend_instance instance{nullptr};
	/* The bindings as the caller or the shaders gave them, in that order,
	 * each _name pointing at names[i]. The core has the same ones sorted. */
	aprend_binding_desc bindings[APREND_MAX_BINDINGS_PER_LAYOUT]{};
	uint32_t binding_count{0};
	/* Every binding's _count added up: the entries one set takes. */
	uint32_t descriptor_count{0};
	/* Shared with every layout of the same bindings; held by this handle. */
	aprend_binding_layout_core *core{nullptr};

	/* Each binding's name, parallel to bindings: the caller's _name, or the
	 * shader's for a layout made from shaders; empty for none.
	 * bindings[i]._name points at names[i], or is NULL. */
	std::string names[APREND_MAX_BINDINGS_PER_LAYOUT]{};
	/* A layout made from shaders: each buffer binding's block, parallel to
	 * bindings. Its members are in [members], and the ones with a type, as
	 * aprend_uniform, in [uniforms]; the names of both are in [strings]. */
	struct block_info {
		bool known;
		const char *block_name;
		uint32_t size;
		uint32_t first_member;
		uint32_t member_count;
		uint32_t first_uniform;
		uint32_t uniform_count;
	};
	block_info blocks[APREND_MAX_BINDINGS_PER_LAYOUT]{};
	std::vector<aprend_shader_member> members{};
	std::vector<aprend_uniform> uniforms{};
	std::deque<std::string> strings{};
} aprend_binding_layout_t;

typedef struct aprend_binding_set_t {
	char *debug_name{nullptr};
	aprend_binding_set_t() = default;
	~aprend_binding_set_t();
	/* For aprend_instance_retire when the handle is destroyed. */
	aprend_instance instance{nullptr};
	/* The handle the set was created through: its names are what a
	 * material reads. */
	aprend_binding_layout layout{nullptr};
	/* The layout's core, held by the set: what the set is a set of, for
	 * every layout that shares it. */
	aprend_binding_layout_core *core{nullptr};
	/* From the core's pools; each goes back on its free list. One set if
	 * every entry is a single buffer, texture or sampler. If any entry is a
	 * buffer set there is one for each frame in flight, sets[f] pointing at
	 * copy f of every buffer set and at the same single resources. */
	spudgpu_descriptor_set sets[APREND_MAX_FRAMES_IN_FLIGHT]{};
	uint32_t frame_set_count{0};
	/* A copy of the entries the set was built from, one for every array
	 * element of every binding: aprend_binding_set_update replaces some and
	 * builds the set again from all of them. The handles in it are the
	 * caller's. */
	aprend_binding_set_entry *entries{nullptr};
	uint32_t entry_count{0};
	/* Every texture view the set holds and the layout its slot reads the
	 * texture in, for a command list to move the texture there before a pass
	 * that binds the set. */
	struct image_use {
		aprend_texture_view view;
		SPUDGPU_IMAGE_LAYOUT layout;
		/* The binding the view is in, for a pipeline's storage_writes. */
		uint32_t binding;
	};
	image_use *image_uses{nullptr};
	uint32_t image_use_count{0};
	/* The store of every storage buffer the set holds, staged or not. A
	 * command list copies the staged ones before it runs, and orders every
	 * one of them against earlier writes before a pass or a dispatch that
	 * reads the set (a shader may have written it). One run of
	 * storage_store_capacity slots for each of the frame_set_count sets, run
	 * f holding storage_store_counts[f] stores: what sets[f] reads. The same
	 * buffer in two slots is listed twice. */
	struct storage_store {
		aprend_buffer_store *store;
		/* The binding it is in, for a pipeline's storage_writes. */
		uint32_t binding;
	};
	storage_store *storage_stores{nullptr};
	uint32_t storage_store_capacity{0};
	uint32_t storage_store_counts[APREND_MAX_FRAMES_IN_FLIGHT]{};
} aprend_binding_set_t;

/* One binding of a material's layout: its name and what the material
 * holds for it. */
struct aprend_material_slot {
	/* The layout's name for the binding, and its block name if it has one
	 * (NULL otherwise); both the layout's strings. */
	const char *name{nullptr};
	const char *block_name{nullptr};
	/* The layout's binding: number, type and count. */
	aprend_binding_desc binding{};
	/* Where the binding's elements start in aprend_material_t::entries. */
	uint32_t first_entry{0};
	/* An owned block: its members (the layout's), and its range of the
	 * instance's material arena, [offset] bytes into [arena]'s buffer. NULL
	 * and 0 for any other binding. */
	const aprend_shader_member *members{nullptr};
	uint32_t member_count{0};
	aprend_material_arena_buffer *arena{nullptr};
	uint32_t offset{0};
	/* The block's bytes as the caller last set them: block_size of them,
	 * which is also the size of the range's data. */
	uint8_t *bytes{nullptr};
	uint32_t block_size{0};
	/* [bytes] have changed since they were last copied into the range: the
	 * next submission of a list that binds the material copies them. Set at
	 * creation, since a new buffer's contents are undefined. */
	bool dirty{false};
};

/* One copy a submission records ahead of its list: [size] bytes from
 * [source] at [source_offset] into [destination], a uniform buffer, at
 * [destination_offset]. [*dirty] is cleared once the submission has gone in. */
struct aprend_upload_copy {
	spudgpu_buffer source;
	uint64_t source_offset;
	spudgpu_buffer destination;
	uint64_t destination_offset;
	uint64_t size;
	bool *dirty;
};

typedef struct aprend_material_t {
	char *debug_name{nullptr};
	aprend_material_t() = default;
	~aprend_material_t();
	aprend_instance instance{nullptr};
	aprend_binding_layout layout{nullptr};
	/* One for each binding of the layout, in the layout's order. */
	aprend_material_slot slots[APREND_MAX_BINDINGS_PER_LAYOUT]{};
	uint32_t slot_count{0};
	/* One entry for every array element of every binding, what the binding
	 * set is built from: _binding, _array_element and _type are filled at
	 * creation, the resource as the caller sets it. An owned block's entry
	 * holds its buffer set from the start. */
	aprend_binding_set_entry *entries{nullptr};
	/* Parallel to entries: whether the element has its view (or buffer)
	 * and whether it has its sampler, as its type needs. */
	bool *has_resource{nullptr};
	bool *has_sampler{nullptr};
	uint32_t entry_count{0};
	/* Built at the first compile that binds the material; NULL until then. */
	aprend_binding_set set{nullptr};
	/* A resource was set since the binding set was built. */
	bool resources_changed{true};
	/* The lit scene (aprendlit.h) this is the material of, or NULL: a
	 * compile that binds it makes sure the scene's environment has been
	 * written (aprend_lit_scene_prepare_list). */
	struct aprend_lit_scene_t *lit_scene{nullptr};
} aprend_material_t;

/* For a compile: builds [material]'s binding set if it has none, or again if
 * a resource was set since. False, printed, if the material is incomplete
 * or the set can't be built; the material is left as it was. */
bool aprend_material_prepare(aprend_material material);
/* For a submission: writes each dirty block of [material] into a span of
 * the current frame's transient memory and adds the copy from it into the
 * block's range to [copies]. The submission clears each block's dirty flag
 * once it has gone in (aprend_upload_copy::dirty). False, printed, if the
 * transient memory can't be taken; what was added stays, and is only
 * recorded if the caller goes on. Throws std::bad_alloc from [copies]. */
bool aprend_material_stage_uploads(aprend_material material, std::vector<aprend_upload_copy> &copies);
/* From the instance's destructor, once nothing is left on the release
 * queue: destroys every buffer of the material arena. */
void aprend_material_arena_destroy(aprend_instance instance);
/* A uniform buffer in the device's own memory, never mapped, written only
 * by copies: what the material arena's buffers are. NULL, printed, if the
 * device refuses it. */
aprend_uniform_buffer aprend_uniform_buffer_create_device(aprend_instance instance, uint32_t size);

typedef struct aprend_render_pass_t {
	char *debug_name{nullptr};
	aprend_render_pass_t() = default;
	~aprend_render_pass_t() = default;
	aprend_instance instance{nullptr};
	/* As the caller gave it, checked at creation. */
	aprend_render_pass_desc desc{};
} aprend_render_pass_t;

typedef struct aprend_compute_pass_t {
	char *debug_name{nullptr};
	aprend_compute_pass_t() = default;
	~aprend_compute_pass_t() = default;
	aprend_instance instance{nullptr};
	aprend_compute_pass_desc desc{};
} aprend_compute_pass_t;

/* What aprend_send_command records for BEGIN_RENDER_PASS, SET_COMPUTE_PASS
 * and DRAW_LIST: each sends the commands it stands for into [cmd_list] and
 * returns false, with a message, for one that can't be expanded. */
bool aprend_render_pass_expand(aprend_command_list cmd_list, aprend_render_pass pass);
bool aprend_compute_pass_expand(aprend_command_list cmd_list, aprend_compute_pass pass);
bool aprend_draw_list_expand(aprend_command_list cmd_list, aprend_draw_list list, uint32_t material_slot);

typedef struct aprend_draw_list_t {
	char *debug_name{nullptr};
	aprend_draw_list_t() = default;
	~aprend_draw_list_t() = default;
	aprend_instance instance{nullptr};
	aprend_draw_list_desc desc{};
	/* One for each aprend_draw_list_add since the reset. The mesh is stored
	 * with every slot past _vertex_buffer_count zeroed, so two meshes are the
	 * same mesh exactly when their bytes are. */
	struct draw {
		aprend_draw_mesh mesh;
		aprend_material material;
		uint32_t instance_count;
		/* Where its instances start in instance_bytes, in instances. */
		uint32_t first_instance;
	};
	std::vector<draw> draws{};
	/* Every draw's instance bytes, in the order they were added. */
	std::vector<uint8_t> instance_bytes{};
	uint64_t instance_count{0};
	/* The last build: the instanced draws, and the span their instances
	 * were written to, in batch order. Valid while built is true. */
	struct batch {
		/* An index into draws, of the batch's first draw: its mesh and
		 * material are the batch's. */
		uint32_t draw;
		uint32_t first_instance;
		uint32_t instance_count;
	};
	std::vector<batch> batches{};
	/* Scratch for the build's sort, kept so a build of a list of the same
	 * size allocates nothing. */
	std::vector<uint32_t> order{};
	aprend_transient_span span{};
	/* The list was built and nothing was added or reset since. */
	bool built{false};
} aprend_draw_list_t;

/* The set of Aprend's bloom layout that reads [view] through the linear
 * sampler, kept by the view (aprend_texture_view_t::bloom_set) from the
 * first call and released with it. The bloom layout must exist
 * (aprend_builtin_bloom_pipeline has been asked for a pipeline). NULL,
 * printed, if the set can't be made. */
aprend_binding_set aprend_texture_view_bloom_set(aprend_texture_view view);

/* Whether a bloom can render onto targets of [format]: the formats
 * aprend_builtin_downsample_format takes, which every backend renders to
 * and blends. */
inline bool aprend_bloom_target_format(SPUDGPU_FORMAT format) { return aprend_builtin_downsample_format(format); }

/* Whether [set] can sit where a pipeline declares [layout]: whether the set
 * is of [layout]'s core, which is whether the layout it was made through has
 * the same bindings. The set's own hold on the core is what is read, not
 * the handle it was made through, which may have gone. */
inline bool aprend_binding_set_fits(const aprend_binding_set_t *set, const aprend_binding_layout_t *layout) { return set->core == layout->core; }

/* Which of [set]'s descriptor sets a command list compiled in frame [frame]
 * binds: the frame's own if the set has one for each frame, its only one
 * otherwise. */
inline uint32_t aprend_binding_set_frame_slot(const aprend_binding_set_t *set, uint32_t frame) {
	return set->frame_set_count > 1 ? frame : 0;
}

#endif // APREND_INTERNAL_HPP
