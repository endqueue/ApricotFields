
#include "aprend_internal.hpp"
#include "aprendimages_internal.hpp"

#include "../apristructbase.hpp"

#include <cstdio>

/* Retires [target]'s views and then its texture, and frees the entry. */
static void aprend_pooled_target_release(aprend_pooled_target *target) {
	for (aprend_texture_view view : target->level_targets)
		aprend_texture_view_destroy(view);
	for (aprend_texture_view view : target->level_sources)
		aprend_texture_view_destroy(view);
	aprend_texture2d_destroy(target->texture);
	target->~aprend_pooled_target();
	free(target);
}

/* The bytes of [target]'s texels, every level, as its format counts them. */
static uint64_t aprend_pooled_target_bytes(const aprend_pooled_target *target) {
	const uint64_t bits = spudgpu_format_bit_count(target->format);
	uint64_t texels     = 0;
	for (uint32_t level = 0; level < target->levels; ++level)
		texels += (uint64_t)aprend_mip_size(target->width, level) * aprend_mip_size(target->height, level);
	return texels * bits / 8;
}

/* A new entry of the key given, with its texture and views. NULL, reported,
 * with nothing kept, if it can't be made. */
static aprend_pooled_target *aprend_pooled_target_make(
    aprend_instance instance,
    uint32_t width,
    uint32_t height,
    SPUDGPU_FORMAT format,
    uint32_t levels) {
	aprend_pooled_target *target = (aprend_pooled_target *)malloc(sizeof(aprend_pooled_target));
	if (!target) {
		aprend_report(instance, APRESULT_OUT_OF_MEMORY, "render target pool: out of memory");
		return nullptr;
	}
	target             = new (target) aprend_pooled_target();
	target->used_frame = instance->frame_number;
	target->width      = width;
	target->height = height;
	target->format = format;
	target->levels = levels;
	target->bytes  = aprend_pooled_target_bytes(target);

	APREND_TEXTURE2D_DESC texture_desc{};
	texture_desc.base = apri_struct_base(APRI_STRUCT_TYPE_APREND_TEXTURE2D_DESC);
	texture_desc.width        = width;
	texture_desc.height       = height;
	texture_desc.format       = format;
	texture_desc.usage        = APREND_TEXTURE_USAGE_BIT_SHADER_RESOURCE | APREND_TEXTURE_USAGE_BIT_RENDER_TARGET;
	texture_desc.mip_levels   = levels;
	texture_desc.array_layers = 1;
	texture_desc.sample_count = 1;
	texture_desc.debug_name = "aprend pooled render target";
	(void)aprend_texture2d_create(instance, &texture_desc, &target->texture, nullptr);
	if (!target->texture) {
		aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "render target pool: a %ux%u target of %u levels can't be created", width, height, levels);
		aprend_pooled_target_release(target);
		return nullptr;
	}
	try {
		target->level_targets.assign(levels, nullptr);
		target->level_sources.assign(levels, nullptr);
	} catch (...) {
		aprend_report(instance, APRESULT_OUT_OF_MEMORY, "render target pool: out of memory");
		aprend_pooled_target_release(target);
		return nullptr;
	}
	for (uint32_t level = 0; level < levels; ++level) {
		APREND_TEXTURE_VIEW_DESC view_desc{};
		view_desc.base = apri_struct_base(APRI_STRUCT_TYPE_APREND_TEXTURE_VIEW_DESC);
		view_desc.shape              = APREND_TEXTURE_VIEW_SHAPE_2D;
		view_desc.aspect             = APREND_TEXTURE_VIEW_ASPECT_COLOR;
		view_desc.base_mip           = level;
		view_desc.mip_count          = 1;
		view_desc.base_layer         = 0;
		view_desc.layer_count        = 1;
		view_desc.type               = APREND_TEXTURE_VIEW_TYPE_RENDER_TARGET;
		(void)aprend_texture_view_create_2d(target->texture, &view_desc, &target->level_targets[level], nullptr);
		view_desc.type               = APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE;
		(void)aprend_texture_view_create_2d(target->texture, &view_desc, &target->level_sources[level], nullptr);
		if (!target->level_targets[level] || !target->level_sources[level]) {
			aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "render target pool: the views of level %u can't be created", level);
			aprend_pooled_target_release(target);
			return nullptr;
		}
	}
	return target;
}

aprend_pooled_target *aprend_render_target_take(
    aprend_command_list_t *cmd_list,
    uint32_t width,
    uint32_t height,
    SPUDGPU_FORMAT format,
    uint32_t levels) {
	aprend_instance instance        = cmd_list->instance;
	aprend_render_target_pool &pool = instance->render_targets;
	aprend_pooled_target *found     = nullptr;
	for (aprend_pooled_target *target : pool.targets) {
		if (target->width != width || target->height != height || target->format != format || target->levels != levels)
			continue;
		if (target->taken && target->taken_frame >= pool.free_below)
			continue;
		found = target;
		break;
	}
	bool made = false;
	if (!found) {
		found = aprend_pooled_target_make(instance, width, height, format, levels);
		if (!found)
			return nullptr;
		made = true;
	}
	aprend_pool_take take{found, found->taken, found->taken_frame, instance->frame_number};
	try {
		if (made) {
			pool.targets.push_back(found);
			pool.bytes += found->bytes;
		}
		cmd_list->pool_takes.push_back(take);
	} catch (...) {
		aprend_cmd_report(cmd_list, APRESULT_OUT_OF_MEMORY, "render target pool: out of memory");
		// A new entry that made it into the pool stays there, untaken, for
		// a later take; one that didn't is held by nothing.
		const bool in_pool = !pool.targets.empty() && pool.targets.back() == found;
		if (made && !in_pool)
			aprend_pooled_target_release(found);
		return nullptr;
	}
	found->taken       = true;
	found->taken_frame = instance->frame_number;
	found->used_frame  = instance->frame_number;
	return found;
}

void aprend_render_target_give_back(aprend_command_list_t *cmd_list) {
	// A list that took nothing reads nothing of the instance: a reset may
	// then overlap aprend_instance_next_frame, which writes frame_number.
	if (cmd_list->pool_takes.empty())
		return;
	const uint64_t frame = cmd_list->instance->frame_number;
	// The latest take of an entry first, so an entry taken twice by the list
	// ends as it was before the first.
	for (size_t i = cmd_list->pool_takes.size(); i > 0; --i) {
		const aprend_pool_take &take = cmd_list->pool_takes[i - 1];
		if (take.frame != frame)
			continue;
		if (!take.target->taken || take.target->taken_frame != frame)
			continue;
		take.target->taken       = take.was_taken;
		take.target->taken_frame = take.was_taken_frame;
	}
	cmd_list->pool_takes.clear();
}

/* Whether [target] was taken in a frame the GPU may still be running, or
 * by a compile of this frame: what nothing may destroy yet. */
static bool aprend_pooled_target_busy(const aprend_render_target_pool &pool, const aprend_pooled_target *target) {
	return target->taken && target->taken_frame >= pool.free_below;
}


/* Retires every entry of [instance]'s pool that isn't busy and whose last
 * use is at least [idle_frames] frames back; returns how many. */
static uint32_t aprend_render_target_pool_retire(aprend_instance instance, uint64_t idle_frames) {
	aprend_render_target_pool &pool = instance->render_targets;
	const uint64_t frame            = instance->frame_number;
	uint32_t retired                = 0;
	size_t kept                     = 0;
	for (aprend_pooled_target *target : pool.targets) {
		if (!aprend_pooled_target_busy(pool, target) && frame - target->used_frame >= idle_frames) {
			pool.bytes -= target->bytes;
			aprend_pooled_target_release(target);
			++retired;
			continue;
		}
		pool.targets[kept++] = target;
	}
	pool.targets.resize(kept);
	return retired;
}

void aprend_render_target_pool_begin_frame(aprend_instance instance) {
	aprend_render_target_pool &pool = instance->render_targets;
	const uint64_t frame            = instance->frame_number;
	const uint64_t in_flight        = instance->desc.frames_in_flight;
	// The wait was for the index's last turn, frames_in_flight back: that
	// frame and every one before it have finished.
	if (frame >= in_flight)
		pool.free_below = frame - in_flight + 1;
	if (pool.retire_idle_frames == 0)
		return;
	if (pool.bytes <= pool.retire_keep_bytes)
		return;
	// Over the caller's floor, entries free and untaken for its count go, in
	// the pool's order, until it is at or under the floor.
	size_t kept = 0;
	for (aprend_pooled_target *target : pool.targets) {
		if (pool.bytes > pool.retire_keep_bytes && !aprend_pooled_target_busy(pool, target) &&
		    frame - target->used_frame >= pool.retire_idle_frames) {
			pool.bytes -= target->bytes;
			aprend_pooled_target_release(target);
			continue;
		}
		pool.targets[kept++] = target;
	}
	pool.targets.resize(kept);
}

void aprend_render_target_pool_destroy(aprend_instance instance) {
	for (aprend_pooled_target *target : instance->render_targets.targets)
		aprend_pooled_target_release(target);
	instance->render_targets.targets.clear();
	instance->render_targets.bytes = 0;
}

extern "C" {

APRESULT aprend_instance_get_render_target_pool(
    aprend_instance instance,
    APREND_RENDER_TARGET_POOL_INFO *out_info) {
	if (!instance)
		return APRESULT_INVALID_ARGUMENT;
	if (APRI_STALE(instance, "aprend_instance_get_render_target_pool", "instance"))
		return APRESULT_INVALID_ARGUMENT;
	if (!out_info)
		return APRESULT_INVALID_ARGUMENT;
	if (const APRESULT result = apri_struct_out_check(&out_info->base, APRI_STRUCT_TYPE_APREND_RENDER_TARGET_POOL_INFO))
		return result;
	APREND_ENTER(instance, "aprend_instance_get_render_target_pool");
	const aprend_render_target_pool &pool = instance->render_targets;
	out_info->texture_count               = 0;
	out_info->busy_count                  = 0;
	out_info->bytes                       = 0;
	out_info->busy_bytes                  = 0;
	for (const aprend_pooled_target *target : pool.targets) {
		const uint64_t bytes = target->bytes;
		out_info->texture_count++;
		out_info->bytes += bytes;
		if (aprend_pooled_target_busy(pool, target)) {
			out_info->busy_count++;
			out_info->busy_bytes += bytes;
		}
	}
	return APRESULT_OK;
}

APRESULT aprend_instance_trim_render_targets(
    aprend_instance instance,
    uint32_t idle_frames,
    uint32_t *out_trim_count) {
	if (out_trim_count)
		*out_trim_count = 0;
	if (!instance)
		return APRESULT_INVALID_ARGUMENT;
	if (APRI_STALE(instance, "aprend_instance_trim_render_targets", "instance"))
		return APRESULT_INVALID_ARGUMENT;
	APREND_ENTER(instance, "aprend_instance_trim_render_targets");
	const uint32_t trimmed = aprend_render_target_pool_retire(instance, idle_frames);
	if (out_trim_count)
		*out_trim_count = trimmed;
	return APRESULT_OK;
}

APRESULT aprend_instance_set_render_target_retirement(
    aprend_instance instance,
    uint32_t idle_frames,
    uint64_t keep_bytes,
    APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!instance)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is NULL");
	if (APRI_STALE(instance, "aprend_instance_set_render_target_retirement", "instance"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is destroyed");
	if (idle_frames == 0)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "idle_frames is 0");
	APREND_ENTER(instance, "aprend_instance_set_render_target_retirement");
	instance->render_targets.retire_idle_frames = idle_frames;
	instance->render_targets.retire_keep_bytes  = keep_bytes;
	return APRESULT_OK;
}

} // extern "C"
