
#include "aprend_internal.hpp"
#include "aprendimages_internal.hpp"

#include <cstdio>

/* Retires [target]'s views and then its texture, and frees the entry. */
static void aprend_pooled_target_release(aprend_pooled_target *target) {
	for (aprend_texture_view view : target->level_targets)
		aprend_destroy_texture_view(view);
	for (aprend_texture_view view : target->level_sources)
		aprend_destroy_texture_view(view);
	aprend_texture2d_destroy(target->texture);
	target->~aprend_pooled_target();
	free(target);
}

/* A new entry of the key given, with its texture and views. NULL, printed,
 * with nothing kept, if it can't be made. */
static aprend_pooled_target *aprend_pooled_target_make(
    aprend_instance instance,
    uint32_t width,
    uint32_t height,
    SPUDGPU_FORMAT format,
    uint32_t levels) {
	aprend_pooled_target *target = (aprend_pooled_target *)malloc(sizeof(aprend_pooled_target));
	if (!target) {
		printf("apricot: render target pool: out of memory\n");
		return nullptr;
	}
	target             = new (target) aprend_pooled_target();
	target->used_frame = instance->frame_number;
	target->width      = width;
	target->height = height;
	target->format = format;
	target->levels = levels;

	aprend_texture2d_desc texture_desc{};
	texture_desc.width        = width;
	texture_desc.height       = height;
	texture_desc.format       = format;
	texture_desc.usage        = APREND_TEXTURE_USAGE_BIT_SHADER_RESOURCE | APREND_TEXTURE_USAGE_BIT_RENDER_TARGET;
	texture_desc.mip_levels   = levels;
	texture_desc.array_layers = 1;
	texture_desc.sample_count = 1;
	texture_desc.debug_name = "aprend pooled render target";
	target->texture = aprend_texture2d_create(instance, &texture_desc);
	if (!target->texture) {
		printf("apricot: render target pool: a %ux%u target of %u levels can't be created\n", width, height, levels);
		aprend_pooled_target_release(target);
		return nullptr;
	}
	try {
		target->level_targets.assign(levels, nullptr);
		target->level_sources.assign(levels, nullptr);
	} catch (...) {
		printf("apricot: render target pool: out of memory\n");
		aprend_pooled_target_release(target);
		return nullptr;
	}
	for (uint32_t level = 0; level < levels; ++level) {
		aprend_texture_view_desc view_desc{};
		view_desc.shape              = APREND_TEXTURE_VIEW_SHAPE_2D;
		view_desc.aspect             = APREND_TEXTURE_VIEW_ASPECT_COLOR;
		view_desc.base_mip           = level;
		view_desc.mip_count          = 1;
		view_desc.base_layer         = 0;
		view_desc.layer_count        = 1;
		view_desc.type               = APREND_TEXTURE_VIEW_TYPE_RENDER_TAGET;
		target->level_targets[level] = aprend_texture_view_create_2d(target->texture, &view_desc);
		view_desc.type               = APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE;
		target->level_sources[level] = aprend_texture_view_create_2d(target->texture, &view_desc);
		if (!target->level_targets[level] || !target->level_sources[level]) {
			printf("apricot: render target pool: the views of level %u can't be created\n", level);
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
		if (made)
			pool.targets.push_back(found);
		cmd_list->pool_takes.push_back(take);
	} catch (...) {
		printf("apricot: render target pool: out of memory\n");
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

void aprend_render_target_pool_begin_frame(aprend_instance instance) {
	aprend_render_target_pool &pool = instance->render_targets;
	const uint64_t frame            = instance->frame_number;
	const uint64_t in_flight        = instance->desc.frames_in_flight;
	// The wait was for the index's last turn, frames_in_flight back: that
	// frame and every one before it have finished.
	if (frame >= in_flight)
		pool.free_below = frame - in_flight + 1;

	size_t kept = 0;
	for (aprend_pooled_target *target : pool.targets) {
		const bool free = !target->taken || target->taken_frame < pool.free_below;
		const bool idle = frame - target->used_frame > APREND_RENDER_TARGET_IDLE_FRAMES;
		if (free && idle) {
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
}
