
#include "render/aprendpipeline.h"
#include "aprend_internal.hpp"
#include "aprendimages_internal.hpp"
#include <spudfiles.h>
#include <algorithm>
#include <cstring>
#include <utility>

// Maps a vertex element type onto the SpudGPU format used to describe it in
// a vertex_attribute_desc. Every type has one: the pipeline refuses any
// other value before this is reached.
static SPUDGPU_FORMAT aprend_vertex_element_type_to_format(APREND_VERTEX_ELEMENT_TYPE type) {
	switch (type) {
	case APREND_VERTEX_ELEMENT_TYPE_FLOAT:
		return SPUDGPU_FORMAT_R32_FLOAT;
	case APREND_VERTEX_ELEMENT_TYPE_VEC2:
		return SPUDGPU_FORMAT_R32G32_FLOAT;
	case APREND_VERTEX_ELEMENT_TYPE_VEC3:
		return SPUDGPU_FORMAT_R32G32B32_FLOAT;
	case APREND_VERTEX_ELEMENT_TYPE_VEC4:
		return SPUDGPU_FORMAT_R32G32B32A32_FLOAT;
	case APREND_VERTEX_ELEMENT_TYPE_INT:
		return SPUDGPU_FORMAT_R32_SINT;
	case APREND_VERTEX_ELEMENT_TYPE_INT2:
		return SPUDGPU_FORMAT_R32G32_SINT;
	case APREND_VERTEX_ELEMENT_TYPE_INT3:
		return SPUDGPU_FORMAT_R32G32B32_SINT;
	case APREND_VERTEX_ELEMENT_TYPE_INT4:
		return SPUDGPU_FORMAT_R32G32B32A32_SINT;
	case APREND_VERTEX_ELEMENT_TYPE_UINT:
		return SPUDGPU_FORMAT_R32_UINT;
	case APREND_VERTEX_ELEMENT_TYPE_UINT2:
		return SPUDGPU_FORMAT_R32G32_UINT;
	case APREND_VERTEX_ELEMENT_TYPE_UINT3:
		return SPUDGPU_FORMAT_R32G32B32_UINT;
	case APREND_VERTEX_ELEMENT_TYPE_UINT4:
		return SPUDGPU_FORMAT_R32G32B32A32_UINT;
	default:
		return SPUDGPU_FORMAT_UNKNOWN;
	}
}

aprend_shader_t::~aprend_shader_t() { spudgpu_destroy_shader_module(this->shader_module); }
aprend_graphics_pipeline_t::~aprend_graphics_pipeline_t() {
	// A pipeline that failed to create has nothing to destroy.
	if (this->pipeline)
		spudgpu_destroy_shader_pipeline(this->pipeline);
	free(this->entry_points);
	free(this->vertex_elements);
}
aprend_compute_pipeline_t::~aprend_compute_pipeline_t() {
	// A pipeline that failed to create has nothing to destroy.
	if (this->pipeline)
		spudgpu_destroy_compute_pipeline(this->pipeline);
	free(this->entry_point);
}
void aprend_binding_layout_core_release(aprend_binding_layout_core *core) {
	if (!core)
		return;
	if (--core->holders > 0)
		return;
	// The last holder: no layout or set of these bindings is left, so the
	// next layout made with them starts a core of its own.
	std::vector<aprend_binding_layout_core *> &cores = core->instance->binding_layout_cores;
	for (size_t i = 0; i < cores.size(); ++i) {
		if (cores[i] == core) {
			cores.erase(cores.begin() + (std::ptrdiff_t)i);
			break;
		}
	}
	// Pools, then the layout their sets were allocated against.
	for (uint32_t i = 0; i < core->pool_count; ++i)
		spudgpu_destroy_descriptor_pool(core->pools[i]);
	free(core->pools);
	free(core->free_sets);
	if (core->layout)
		spudgpu_destroy_descriptor_set_layout(core->layout);
	free(core);
}

aprend_binding_layout_t::~aprend_binding_layout_t() {
	// NULL for a layout that failed before it found its core.
	aprend_binding_layout_core_release(this->core);
}
aprend_binding_set_t::~aprend_binding_set_t() {
	free(this->entries);
	free(this->image_uses);
	free(this->storage_stores);
	// A set that failed to create gives back what it got as far as acquiring,
	// and lets go of the core only once it has: the core may go with it.
	for (uint32_t frame = 0; frame < this->frame_set_count; ++frame)
		this->core->free_sets[this->core->free_set_count++] = this->sets[frame];
	aprend_binding_layout_core_release(this->core);
}

static_assert(APREND_DESCRIPTOR_TYPE_COUNT <= SPUDGPU_MAX_DESCRIPTOR_POOL_SIZES);

// Adds one more pool to [layout], and room on its free list for the sets that
// pool will hold.
static SPUDRESULT aprend_binding_layout_add_pool(aprend_binding_layout_core *layout) {
	const uint32_t pool_count = layout->pool_count + 1;

	spudgpu_descriptor_pool *pools = (spudgpu_descriptor_pool *)realloc(layout->pools, sizeof(spudgpu_descriptor_pool) * pool_count);
	if (!pools)
		return SPUDRESULT_OUT_OF_MEMORY;
	layout->pools = pools;

	spudgpu_descriptor_set *free_sets =
	    (spudgpu_descriptor_set *)realloc(layout->free_sets, sizeof(spudgpu_descriptor_set) * pool_count * APREND_BINDING_SETS_PER_POOL);
	if (!free_sets)
		return SPUDRESULT_OUT_OF_MEMORY;
	layout->free_sets = free_sets;

	// One pool size per descriptor type the layout uses.
	uint32_t per_set[APREND_DESCRIPTOR_TYPE_COUNT]{};
	for (uint32_t i = 0; i < layout->binding_count; ++i)
		per_set[layout->bindings[i]._type] += layout->bindings[i]._count;

	spudgpu_descriptor_pool_desc pool_desc{};
	pool_desc.base.type = SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_POOL_DESC;
	pool_desc.max_sets = APREND_BINDING_SETS_PER_POOL;
	for (uint32_t type = 0; type < APREND_DESCRIPTOR_TYPE_COUNT; ++type) {
		if (per_set[type] == 0)
			continue;
		pool_desc.pool_sizes[pool_desc.pool_size_count].descriptor_type = type;
		pool_desc.pool_sizes[pool_desc.pool_size_count].count           = APREND_BINDING_SETS_PER_POOL * per_set[type];
		++pool_desc.pool_size_count;
	}
	SPUDRESULT sr = spudgpu_create_descriptor_pool(layout->instance->desc.device, &pool_desc, &layout->pools[pool_count - 1]);
	if (SPUDFAIL(sr))
		return sr;

	layout->pool_count        = pool_count;
	layout->sets_in_last_pool = 0;
	return SPUD_SUCCESS;
}

// A descriptor set of [layout]: one given back by a destroyed
// aprend_binding_set if there is one, else a new one from the last pool,
// adding a pool when that is full.
static SPUDRESULT aprend_binding_layout_acquire_set(aprend_binding_layout_core *layout, spudgpu_descriptor_set *out_set) {
	if (layout->free_set_count > 0) {
		*out_set = layout->free_sets[--layout->free_set_count];
		return SPUD_SUCCESS;
	}
	if (layout->pool_count == 0 || layout->sets_in_last_pool == APREND_BINDING_SETS_PER_POOL) {
		SPUDRESULT sr = aprend_binding_layout_add_pool(layout);
		if (SPUDFAIL(sr))
			return sr;
	}

	spudgpu_descriptor_set_desc set_desc{};
	set_desc.base.type = SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_SET_DESC;
	set_desc.pool           = layout->pools[layout->pool_count - 1];
	set_desc.set_layouts[0] = layout->layout;
	set_desc.set_count      = 1;
	SPUDRESULT sr = spudgpu_create_descriptor_sets(layout->instance->desc.device, &set_desc, out_set);
	if (SPUDFAIL(sr))
		return sr;
	++layout->sets_in_last_pool;
	return SPUD_SUCCESS;
}

static const APREND_BINDING_DESC *aprend_binding_layout_find_desc(const aprend_binding_layout_t *layout, uint32_t binding) {
	for (uint32_t i = 0; i < layout->binding_count; ++i)
		if (layout->bindings[i]._binding == binding)
			return &layout->bindings[i];
	return nullptr;
}

static bool aprend_descriptor_type_reads_view(SPUDGPU_DESCRIPTOR_TYPE type) {
	return type == SPUDGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE || type == SPUDGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
	       type == SPUDGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
}
static bool aprend_descriptor_type_reads_sampler(SPUDGPU_DESCRIPTOR_TYPE type) {
	return type == SPUDGPU_DESCRIPTOR_TYPE_SAMPLER || type == SPUDGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
}

// Whether [offset] and [range] (0 = to the end) stay inside a buffer of
// [size] bytes and cover at least one of them.
static bool aprend_binding_range_valid(uint64_t offset, uint64_t range, uint64_t size) {
	if (offset >= size)
		return false;
	if (range > size - offset)
		return false;
	return true;
}

// Whether entry [index] of a set holds what its _type reads. The type has
// already been checked against the layout's.
static APRESULT aprend_binding_set_entry_resource_valid(APREND_ERROR *error, aprend_instance instance, const APREND_BINDING_SET_ENTRY &entry, uint32_t index) {
	switch (entry._type) {
	case SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER: {
		const auto &r = entry._resource._uniform;
		if (!r._buffer)
			return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "entry %u (binding %u) has no uniform buffer", index, entry._binding);
		if (r._buffer->instance != instance)
			return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "entry %u (binding %u) has a uniform buffer of another instance", index, entry._binding);
		if (!(r._buffer->desc.usage & APREND_BUFFER_USAGE_UNIFORM))
			return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "entry %u (binding %u) has a buffer created without APREND_BUFFER_USAGE_UNIFORM", index, entry._binding);
		// The device's rule for where a uniform buffer range may start.
		if (r._offset % instance->uniform_buffer_offset_alignment != 0)
			return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "entry %u (binding %u) has a uniform buffer _offset of %llu, which is not a multiple of the device's %u", index, entry._binding, (unsigned long long)r._offset, instance->uniform_buffer_offset_alignment);
		// Every copy is the same size.
		if (!aprend_binding_range_valid(r._offset, r._range, r._buffer->desc.size))
			return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "entry %u (binding %u) has an _offset and _range outside its buffer", index, entry._binding);
		return APRESULT_OK;
	}
	case SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER: {
		const auto &r = entry._resource._storage;
		if (!r._buffer)
			return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "entry %u (binding %u) has no storage buffer", index, entry._binding);
		if (r._buffer->instance != instance)
			return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "entry %u (binding %u) has a storage buffer of another instance", index, entry._binding);
		if (!(r._buffer->desc.usage & APREND_BUFFER_USAGE_STORAGE))
			return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "entry %u (binding %u) has a buffer created without APREND_BUFFER_USAGE_STORAGE", index, entry._binding);
		// The device's rule for where a storage buffer range may start.
		if (r._offset % instance->storage_buffer_offset_alignment != 0)
			return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "entry %u (binding %u) has a storage buffer _offset of %llu, which is not a multiple of the device's %u", index, entry._binding, (unsigned long long)r._offset, instance->storage_buffer_offset_alignment);
		if (!aprend_binding_range_valid(r._offset, r._range, r._buffer->desc.size))
			return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "entry %u (binding %u) has an _offset and _range outside its buffer", index, entry._binding);
		return APRESULT_OK;
	}
	default:
		break;
	}

	const auto &r = entry._resource._image;
	if (aprend_descriptor_type_reads_view(entry._type)) {
		if (!r._view)
			return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "entry %u (binding %u) has no texture view", index, entry._binding);
		const APREND_TEXTURE_VIEW_TYPE wanted = entry._type == SPUDGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE
		                                            ? APREND_TEXTURE_VIEW_TYPE_UNORDERED_ACCESS
		                                            : APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE;
		if (r._view->view_type != wanted)
			return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "entry %u (binding %u) has a view of type %u, its slot takes %u", index, entry._binding, (unsigned)r._view->view_type, (unsigned)wanted);
	}
	if (aprend_descriptor_type_reads_sampler(entry._type)) {
		if (!r._sampler)
			return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "entry %u (binding %u) has no sampler", index, entry._binding);
	}
	return APRESULT_OK;
}

// Whether two blend entries blend the same way, by SpudGPU's rule: two
// that don't blend are the same whatever their factors. Entries that differ
// need SPUDGPU_FEATURE_INDEPENDENT_BLEND.
static bool aprend_blends_equal(const spudgpu_blend_attachment_desc &a, const spudgpu_blend_attachment_desc &b) {
	if (a.blend_enable != b.blend_enable)
		return false;
	if (!a.blend_enable)
		return true;
	return a.src_color_blend_factor == b.src_color_blend_factor && a.dst_color_blend_factor == b.dst_color_blend_factor &&
	       a.color_blend_op == b.color_blend_op && a.src_alpha_blend_factor == b.src_alpha_blend_factor &&
	       a.dst_alpha_blend_factor == b.dst_alpha_blend_factor && a.alpha_blend_op == b.alpha_blend_op;
}

static APRESULT aprend_shader_stage_supported(SPUDGPU_SHADER_STAGE shader_stage, APREND_ERROR *error) {
	switch (shader_stage) {
	case SPUDGPU_SHADER_STAGE_VERTEX:
	case SPUDGPU_SHADER_STAGE_FRAGMENT:
	case SPUDGPU_SHADER_STAGE_COMPUTE:
	case SPUDGPU_SHADER_STAGE_MESH:
		return APRESULT_OK;
	default:
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "shader_stage %u is not one stage of vertex, fragment, compute or mesh", (unsigned)shader_stage);
	}
}

// APRESULT_UNSUPPORTED, [error] saying why, if [shader_stage] needs a
// feature [instance]'s device was created without. A mesh shader module is
// refused there on every backend: on Vulkan the SPIR-V itself needs
// VK_EXT_mesh_shader enabled.
static APRESULT aprend_shader_stage_feature_ok(aprend_instance instance, SPUDGPU_SHADER_STAGE shader_stage, APREND_ERROR *error) {
	if (shader_stage == SPUDGPU_SHADER_STAGE_MESH && !(instance->features & SPUDGPU_FEATURE_MESH_SHADING))
		return aprend_fail(error, APRESULT_UNSUPPORTED, "a mesh shader and the device was created without SPUDGPU_FEATURE_MESH_SHADING");
	return APRESULT_OK;
}

// Reflects [size] bytes of SPIR-V into [shader]'s reflection.
// APRESULT_INVALID_ARGUMENT, [error] saying why, if it can't be. [code] is
// the caller's and may not be aligned to 4, so the words are read through a
// copy where it isn't.
static APRESULT aprend_shader_reflect(
    aprend_shader_t *shader,
    const void *code,
    uint64_t size,
    SPUDGPU_SHADER_STAGE shader_stage,
    APREND_ERROR *error) {
	std::string why;
	bool ok = false;
	try {
		if (((uintptr_t)code % alignof(uint32_t)) == 0) {
			ok = aprend_reflect_spirv((const uint32_t *)code, (size_t)(size / 4), shader_stage, &shader->reflection, &why);
		} else {
			uint32_t *words = (uint32_t *)malloc(size);
			if (!words)
				return aprend_fail(error, APRESULT_OUT_OF_MEMORY, "no memory for a copy of the SPIR-V");
			memcpy(words, code, size);
			ok = aprend_reflect_spirv(words, (size_t)(size / 4), shader_stage, &shader->reflection, &why);
			free(words);
		}
	} catch (const std::bad_alloc &) {
		return aprend_fail(error, APRESULT_OUT_OF_MEMORY, "no memory to reflect the shader");
	}
	if (!ok)
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "the shader can't be reflected: %s", why.c_str());
	return APRESULT_OK;
}

// SpudGPU copies the SPIR-V into its own shader module, so [code] only has to
// live for this call.
static SPUDRESULT aprend_shader_create_module(
    aprend_shader_t *shader,
    const void *code,
    uint64_t size,
    SPUDGPU_SHADER_STAGE shader_stage,
    const char *debug_name) {
	spudgpu_shader_module_desc smd = {};
	smd.base.type = SPUD_STRUCT_TYPE_GPU_SHADER_MODULE_DESC;
	smd.stage                      = shader_stage;
	smd.spirv_code                 = code;
	smd.spirv_size                 = size;
	smd.debug_name = debug_name;
	shader->stage = shader_stage;
	return spudgpu_create_shader_module(shader->instance->desc.device, &smd, &shader->shader_module);
}

// A stage's name, for messages.
static const char *aprend_stage_name(SPUDGPU_SHADER_STAGE stage) {
	switch (stage) {
	case SPUDGPU_SHADER_STAGE_VERTEX:
		return "vertex";
	case SPUDGPU_SHADER_STAGE_FRAGMENT:
		return "fragment";
	case SPUDGPU_SHADER_STAGE_COMPUTE:
		return "compute";
	case SPUDGPU_SHADER_STAGE_MESH:
		return "mesh";
	default:
		return "unknown";
	}
}

// Checks [shader] against what a pipeline declares for it: [entry_point],
// the [layout_count] [layouts] and the [range_count] push constant [ranges].
// Adds the images it uses to [images] and the storage bindings it writes to
// [writes]. Fails, [error] saying why, at the first mismatch. Throws
// std::bad_alloc from [images] or [writes].
/* Whether [desc]'s fragment shader writes each of its color targets, and
 * only those, as the kind of number each target's format holds. A target
 * the shader doesn't write gets undefined values on every API (SpudGPU has
 * no write mask to leave it alone); a location with no target is refused
 * by Metal; and a float written to an integer target, or the other way
 * round, is undefined. Fails, [error] saying why, if not. */
static APRESULT aprend_pipeline_check_outputs(APREND_ERROR *error, const APREND_GRAPHICS_PIPELINE_DESC &desc) {
	const aprend_reflection_store &fragment = desc.fragment_shader->reflection;
	const uint32_t outputs                  = fragment.view.color_output_count;
	if (outputs > desc.color_attachment_count)
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "the fragment shader writes color location %u, and the pipeline has %u color targets", outputs - 1, desc.color_attachment_count);
	for (uint32_t i = 0; i < desc.color_attachment_count; ++i) {
		const APREND_SHADER_SCALAR_TYPE written = i < fragment.output_types.size() ? fragment.output_types[i] : APREND_SHADER_SCALAR_TYPE_NONE;
		if (written == APREND_SHADER_SCALAR_TYPE_NONE)
			return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "the fragment shader doesn't write color location %u, and the pipeline has a target there, which would be left undefined", i);
		const APREND_SHADER_SCALAR_TYPE holds = aprend_format_scalar_type(desc.color_attachment_formats[i]);
		if (holds != APREND_SHADER_SCALAR_TYPE_NONE && written != holds)
			return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "the fragment shader writes %s to color location %u, and its target's format %u holds %s", aprend_scalar_type_name(written), i, (unsigned)desc.color_attachment_formats[i], aprend_scalar_type_name(holds));
	}
	return APRESULT_OK;
}

static APRESULT aprend_pipeline_check_shader(
    APREND_ERROR *error,
    aprend_shader shader,
    const char *entry_point,
    const aprend_binding_layout *layouts,
    uint32_t layout_count,
    const spudgpu_push_constant_range_desc *ranges,
    uint32_t range_count,
    std::vector<aprend_image_expectation> &images,
    std::vector<aprend_storage_write> &writes) {
	const APREND_SHADER_REFLECTION &reflection = shader->reflection.view;
	const char *stage                          = aprend_stage_name(shader->stage);
	if (reflection.entry_point && strcmp(reflection.entry_point, entry_point) != 0)
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "the %s shader's entry point is \"%s\", and the pipeline names \"%s\"", stage, reflection.entry_point, entry_point);

	for (uint32_t r = 0; r < reflection.resource_count; ++r) {
		const APREND_SHADER_RESOURCE &resource = reflection.resources[r];
		if (!resource.used)
			continue;
		if (resource.set >= layout_count)
			return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "the %s shader uses \"%s\" at set %u binding %u, and the pipeline declares %u sets", stage, resource.name, resource.set, resource.binding, layout_count);
		const APREND_BINDING_DESC *binding = aprend_binding_layout_find_desc(layouts[resource.set], resource.binding);
		if (!binding)
			return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "the %s shader uses \"%s\" at set %u binding %u, which the layout at set %u doesn't declare", stage, resource.name, resource.set, resource.binding, resource.set);
		if (binding->_type != resource.type)
			return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "the %s shader reads set %u binding %u (\"%s\") as type %u, and the layout declares type %u", stage, resource.set, resource.binding, resource.name, (unsigned)resource.type, (unsigned)binding->_type);
		if (binding->_count < resource.count)
			return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "the %s shader reads %u descriptors at set %u binding %u (\"%s\"), and the layout declares %u", stage, resource.count, resource.set, resource.binding, resource.name, binding->_count);
		if (!(binding->_stages & shader->stage))
			return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "the %s shader uses set %u binding %u (\"%s\"), and the layout doesn't give the binding to that stage", stage, resource.set, resource.binding, resource.name);
		if (resource.image_shape != APREND_TEXTURE_VIEW_SHAPE_NONE) {
			SPUDGPU_FORMAT format = SPUDGPU_FORMAT_UNKNOWN;
			for (const aprend_reflection_store::storage_format &declared : shader->reflection.storage_formats)
				if (declared.set == resource.set && declared.binding == resource.binding)
					format = declared.format;
			images.push_back({resource.set, resource.binding, resource.image_shape, resource.image_scalar_type, format});
		}
		const bool storage = resource.type == SPUDGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE || resource.type == SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		if (storage && !resource.read_only && !aprend_storage_writes_hold(writes, resource.set, resource.binding))
			writes.push_back({resource.set, resource.binding});
	}

	if (reflection.has_push_constants) {
		// Each used member's bytes, a word at a time, in a range for this
		// stage.
		for (uint32_t m = 0; m < reflection.push_constant_member_count; ++m) {
			const APREND_SHADER_MEMBER &member = reflection.members[reflection.push_constant_first_member + m];
			// Only what this stage touches needs a range of this stage: two
			// stages may share a block and each read part of it.
			if (!member.used)
				continue;
			for (uint32_t byte = member.offset; byte < member.offset + member.size; byte += 4) {
				bool covered = false;
				for (uint32_t i = 0; i < range_count && !covered; ++i)
					covered = (ranges[i].stage_flags & shader->stage) && byte >= ranges[i].offset && byte - ranges[i].offset < ranges[i].size;
				if (!covered)
					return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "the %s shader reads push constant \"%s\" at byte %u, which no push constant range for that stage covers", stage, member.name, byte);
			}
		}
	}
	return APRESULT_OK;
}

// The kind of number an element of [type] gives a vertex shader.
static APREND_SHADER_SCALAR_TYPE aprend_element_scalar_type(APREND_VERTEX_ELEMENT_TYPE type) {
	switch (type) {
	case APREND_VERTEX_ELEMENT_TYPE_FLOAT:
	case APREND_VERTEX_ELEMENT_TYPE_VEC2:
	case APREND_VERTEX_ELEMENT_TYPE_VEC3:
	case APREND_VERTEX_ELEMENT_TYPE_VEC4:
		return APREND_SHADER_SCALAR_TYPE_FLOAT;
	case APREND_VERTEX_ELEMENT_TYPE_INT:
	case APREND_VERTEX_ELEMENT_TYPE_INT2:
	case APREND_VERTEX_ELEMENT_TYPE_INT3:
	case APREND_VERTEX_ELEMENT_TYPE_INT4:
		return APREND_SHADER_SCALAR_TYPE_INT;
	case APREND_VERTEX_ELEMENT_TYPE_UINT:
	case APREND_VERTEX_ELEMENT_TYPE_UINT2:
	case APREND_VERTEX_ELEMENT_TYPE_UINT3:
	case APREND_VERTEX_ELEMENT_TYPE_UINT4:
		return APREND_SHADER_SCALAR_TYPE_UINT;
	default:
		return APREND_SHADER_SCALAR_TYPE_NONE;
	}
}

// One vertex element and the location it is given, UINT32_MAX for one the
// shader doesn't read.
struct aprend_vertex_attribute_plan {
	uint32_t binding;
	const APREND_VERTEX_ELEMENT *element;
	uint32_t location;
};

// Gives each element of [desc]'s vertex bindings its location, by position
// or by the names of [vertex_shader]'s inputs, and checks every input the
// shader uses is fed at each of its locations by an element of its kind of
// number. Fails, [error] saying why, if not. Throws std::bad_alloc.
static APRESULT aprend_pipeline_plan_vertex_input(
    APREND_ERROR *error,
    const APREND_GRAPHICS_PIPELINE_DESC &desc,
    aprend_shader vertex_shader,
    std::vector<aprend_vertex_attribute_plan> &plan) {
	const APREND_SHADER_REFLECTION &reflection = vertex_shader->reflection.view;
	uint32_t position                          = 0;
	for (uint32_t binding = 0; binding < desc._vertex_binding_count; ++binding) {
		const APREND_VERTEX_LAYOUT &layout = desc._vertex_bindings[binding]._layout;
		for (uint32_t i = 0; i < layout.count; ++i) {
			const APREND_VERTEX_ELEMENT &element = layout.elements[i];
			uint32_t location                    = position++;
			if (desc._vertex_inputs_by_name) {
				if (!element.name)
					return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "_vertex_inputs_by_name is set and element %u of _vertex_bindings[%u] has no name", i, binding);
				// The input of that name, and for a matrix the next of its
				// locations not yet given to an earlier element of the name.
				location = UINT32_MAX;
				for (uint32_t k = 0; k < reflection.input_count && location == UINT32_MAX; ++k) {
					const APREND_SHADER_INPUT &input = reflection.inputs[k];
					if (strcmp(input.name, element.name) != 0)
						continue;
					uint32_t taken = 0;
					for (const aprend_vertex_attribute_plan &earlier : plan)
						if (earlier.location != UINT32_MAX && earlier.location >= input.location &&
						    earlier.location < input.location + input.location_count)
							++taken;
					if (taken < input.location_count)
						location = input.location + taken;
				}
			}
			plan.push_back({binding, &element, location});
		}
	}

	for (uint32_t k = 0; k < reflection.input_count; ++k) {
		const APREND_SHADER_INPUT &input = reflection.inputs[k];
		if (!input.used)
			continue;
		const bool fed_kind = input.scalar_type == APREND_SHADER_SCALAR_TYPE_FLOAT || input.scalar_type == APREND_SHADER_SCALAR_TYPE_INT ||
		                      input.scalar_type == APREND_SHADER_SCALAR_TYPE_UINT;
		if (!fed_kind)
			return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "vertex input \"%s\" is read as a kind of number no vertex element type gives (float, int or uint)", input.name);
		for (uint32_t location = input.location; location < input.location + input.location_count; ++location) {
			const aprend_vertex_attribute_plan *fed = nullptr;
			for (const aprend_vertex_attribute_plan &attribute : plan)
				if (attribute.location == location)
					fed = &attribute;
			if (!fed)
				return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "vertex input \"%s\" reads location %u, and no vertex element is given that location", input.name, location);
			if (aprend_element_scalar_type(fed->element->type) != input.scalar_type)
				return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "vertex input \"%s\" at location %u is read as %s, and its element is %s", input.name, location, aprend_scalar_type_name(input.scalar_type), aprend_scalar_type_name(aprend_element_scalar_type(fed->element->type)));
		}
	}
	return APRESULT_OK;
}

bool aprend_binding_set_matches_images(
    const aprend_binding_set_t *set,
    uint32_t slot,
    const std::vector<aprend_image_expectation> &expected,
    const char *who) {
	for (uint32_t i = 0; i < set->entry_count; ++i) {
		const APREND_BINDING_SET_ENTRY &entry = set->entries[i];
		if (!aprend_descriptor_type_reads_view(entry._type))
			continue;
		for (const aprend_image_expectation &expectation : expected) {
			if (expectation.set != slot || expectation.binding != entry._binding)
				continue;
			const aprend_texture_view view = entry._resource._image._view;
			if (view->desc.shape != expectation.shape) {
				aprend_report(set->instance, APRESULT_INVALID_ARGUMENT, "%s: set %u binding %u element %u is a view of shape %u, and the shader declares shape %u there", who, slot, entry._binding, entry._array_element, (unsigned)view->desc.shape, (unsigned)expectation.shape);
				return false;
			}
			const bool stencil = view->desc.aspect == APREND_TEXTURE_VIEW_ASPECT_STENCIL;
			if (stencil && expectation.scalar_type != APREND_SHADER_SCALAR_TYPE_UINT) {
				aprend_report(set->instance, APRESULT_INVALID_ARGUMENT, "%s: set %u binding %u element %u is a stencil, and the shader reads it as other than an unsigned integer", who, slot, entry._binding, entry._array_element);
				return false;
			}
			if (view->desc.aspect == APREND_TEXTURE_VIEW_ASPECT_DEPTH && expectation.scalar_type != APREND_SHADER_SCALAR_TYPE_FLOAT) {
				aprend_report(set->instance, APRESULT_INVALID_ARGUMENT, "%s: set %u binding %u element %u is a depth, and the shader reads it as other than a float", who, slot, entry._binding, entry._array_element);
				return false;
			}
			const SPUDGPU_FORMAT format = aprend_texture_view_format(view);
			// What a read gives follows the format's kind of number on every
			// API: a float or normalized one reads as float, an integer one as
			// its own. Read as another, the result is undefined on Vulkan and
			// D3D12 and refused by Metal's validation.
			const APREND_SHADER_SCALAR_TYPE reads = aprend_format_scalar_type(format);
			if (view->desc.aspect == APREND_TEXTURE_VIEW_ASPECT_COLOR && reads != APREND_SHADER_SCALAR_TYPE_NONE &&
			    expectation.scalar_type != reads) {
				aprend_report(set->instance, APRESULT_INVALID_ARGUMENT, "%s: set %u binding %u element %u is a view of format %u, which reads as %s, and the shader reads it as %s", who, slot, entry._binding, entry._array_element, (unsigned)format, aprend_scalar_type_name(reads), aprend_scalar_type_name(expectation.scalar_type));
				return false;
			}
			if (expectation.format != SPUDGPU_FORMAT_UNKNOWN && format != expectation.format) {
				aprend_report(set->instance, APRESULT_INVALID_ARGUMENT, "%s: set %u binding %u element %u is a storage image view of format %u, and the shader declares format %u there", who, slot, entry._binding, entry._array_element, (unsigned)format, (unsigned)expectation.format);
				return false;
			}
		}
	}
	return true;
}

// The core of [binding_count] [bindings], checked by the caller: the
// instance's core of the same bindings if it has one, else a new one with a
// SpudGPU layout of them. Held once more for the caller either way, through
// [out_core]. Fails, [error] saying why, if a new one can't be made.
static APRESULT aprend_binding_layout_core_acquire(
    aprend_instance instance,
    const APREND_BINDING_DESC *bindings,
    uint32_t binding_count,
    aprend_binding_layout_core **out_core,
    APREND_ERROR *error) {
	// What makes two layouts one: each binding's number, type, count and
	// stages, in binding order. Names are each handle's own.
	APREND_BINDING_DESC key[APREND_MAX_BINDINGS_PER_LAYOUT]{};
	for (uint32_t i = 0; i < binding_count; ++i) {
		key[i]       = bindings[i];
		key[i]._name = nullptr;
	}
	std::sort(key, key + binding_count, [](const APREND_BINDING_DESC &a, const APREND_BINDING_DESC &b) { return a._binding < b._binding; });

	for (aprend_binding_layout_core *core : instance->binding_layout_cores) {
		if (core->binding_count != binding_count)
			continue;
		bool same = true;
		for (uint32_t i = 0; i < binding_count && same; ++i) {
			const APREND_BINDING_DESC &a = core->bindings[i];
			const APREND_BINDING_DESC &b = key[i];
			same = a._binding == b._binding && a._type == b._type && a._count == b._count && a._stages == b._stages;
		}
		if (same) {
			++core->holders;
			*out_core = core;
			return APRESULT_OK;
		}
	}

	aprend_binding_layout_core *core = (aprend_binding_layout_core *)calloc(1, sizeof(aprend_binding_layout_core));
	if (!core)
		return aprend_fail(error, APRESULT_OUT_OF_MEMORY, "no memory for the layout");
	core->instance      = instance;
	core->binding_count = binding_count;
	core->holders       = 1;
	spudgpu_descriptor_set_layout_desc ld{};
	ld.base.type = SPUD_STRUCT_TYPE_GPU_DESCRIPTOR_SET_LAYOUT_DESC;
	for (uint32_t i = 0; i < binding_count; ++i) {
		core->bindings[i]              = key[i];
		ld.bindings[i].binding         = key[i]._binding;
		ld.bindings[i].descriptor_type = key[i]._type;
		ld.bindings[i].count           = key[i]._count;
		ld.bindings[i].stage_flags     = key[i]._stages;
	}
	ld.binding_count = binding_count;
	SPUDRESULT sr    = spudgpu_create_descriptor_set_layout(instance->desc.device, &ld, &core->layout);
	if (SPUDFAIL(sr)) {
		free(core);
		return aprend_fail_spud(error, sr, "creating the descriptor set layout failed");
	}
	try {
		instance->binding_layout_cores.push_back(core);
	} catch (const std::bad_alloc &) {
		spudgpu_destroy_descriptor_set_layout(core->layout);
		free(core);
		return aprend_fail(error, APRESULT_OUT_OF_MEMORY, "no memory for the layout");
	}
	*out_core = core;
	return APRESULT_OK;
}

// Creates a layout of [binding_count] [bindings], checked by the caller,
// with a copy of each binding's name, its destroy's retire promised. Fails,
// [error] saying why, if it can't be created.
static APRESULT aprend_binding_layout_build(
    aprend_instance instance,
    const APREND_BINDING_DESC *bindings,
    uint32_t binding_count,
    aprend_binding_layout_t **out_layout,
    APREND_ERROR *error) {
	APREND_MALLOC__T(result, aprend_binding_layout_t);
	if (!result)
		return aprend_fail(error, APRESULT_OUT_OF_MEMORY, "no memory for the layout");
	APREND_CONSTRUCT__T(result, aprend_binding_layout_t);
	result->instance      = instance;
	result->binding_count = binding_count;

	try {
		for (uint32_t i = 0; i < binding_count; ++i) {
			result->bindings[i] = bindings[i];
			result->names[i]    = bindings[i]._name ? bindings[i]._name : "";
			// The layout's own copy, never the caller's string.
			result->bindings[i]._name = result->names[i].empty() ? nullptr : result->names[i].c_str();
			result->descriptor_count += bindings[i]._count;
		}
	} catch (const std::bad_alloc &) {
		APREND_DESTRUCT__T(result, aprend_binding_layout_t);
		return aprend_fail(error, APRESULT_OUT_OF_MEMORY, "no memory for the layout's names");
	}

	if (const APRESULT acquired = aprend_binding_layout_core_acquire(instance, bindings, binding_count, &result->core, error)) {
		APREND_DESTRUCT__T(result, aprend_binding_layout_t);
		return acquired;
	}
	// Its destroy's retire, promised now so the destroy can't fail.
	if (aprend_instance_reserve_release(instance, 1) != APRESULT_OK) {
		APREND_DESTRUCT__T(result, aprend_binding_layout_t);
		return aprend_fail(error, APRESULT_OUT_OF_MEMORY, "no memory to release the layout later");
	}
	*out_layout = result;
	return APRESULT_OK;
}

/* A shader of [size] bytes of SPIR-V at [code], checked by the caller:
 * reflected first, so SPIR-V that can't be read isn't handed to the device,
 * then its module made and its destroy's retire promised. [debug_name] (the
 * file's name, or NULL) is the module's. */
static APRESULT aprend_shader_make(
    aprend_instance instance,
    const void *code,
    uint64_t size,
    SPUDGPU_SHADER_STAGE shader_stage,
    const char *debug_name,
    aprend_shader *out_shader,
    APREND_ERROR *error) {
	APREND_MALLOC__T(result, aprend_shader_t);
	if (!result)
		return aprend_fail(error, APRESULT_OUT_OF_MEMORY, "no memory for the shader");
	APREND_CONSTRUCT__T(result, aprend_shader_t);
	result->instance = instance;
	if (const APRESULT reflected = aprend_shader_reflect(result, code, size, shader_stage, error)) {
		APREND_DESTRUCT__T(result, aprend_shader_t);
		return reflected;
	}
	const SPUDRESULT sr = aprend_shader_create_module(result, code, size, shader_stage, debug_name);
	if (SPUDFAIL(sr)) {
		APREND_DESTRUCT__T(result, aprend_shader_t);
		return aprend_fail_spud(error, sr, "creating the shader module failed");
	}
	if (aprend_instance_reserve_release(instance, 1) != APRESULT_OK) {
		APREND_DESTRUCT__T(result, aprend_shader_t);
		return aprend_fail(error, APRESULT_OUT_OF_MEMORY, "no memory to release the shader later");
	}
	*out_shader = result;
	return APRESULT_OK;
}

extern "C" {

APRESULT aprend_shader_create_spirv(
    aprend_instance instance,
    const void *spirv_code,
    uint64_t spirv_size,
    SPUDGPU_SHADER_STAGE shader_stage,
    aprend_shader *out_shader,
    APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (out_shader)
		*out_shader = nullptr;
	if (!instance)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is NULL");
	APREND_ENTER(instance, "aprend_shader_create_spirv");
	if (!spirv_code)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "spirv_code is NULL");
	if (spirv_size == 0)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "spirv_size is 0");
	if (spirv_size % 4 != 0)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "spirv_size %llu is not a multiple of 4", (unsigned long long)spirv_size);
	if (const APRESULT result = aprend_shader_stage_supported(shader_stage, out_error))
		return result;
	if (const APRESULT result = aprend_shader_stage_feature_ok(instance, shader_stage, out_error))
		return result;
	if (!out_shader)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_shader is NULL");
	return aprend_shader_make(instance, spirv_code, spirv_size, shader_stage, nullptr, out_shader, out_error);
}

/* Reads the whole file at [filename] into a malloc'd block. Its SpudFiles
 * instance is over the system heap and lives for the call: Aprend keeps no
 * files open. The file is shared with every other reader, writer and
 * rename, since nothing here minds them. APRESULT_NOT_FOUND when no file is
 * there; a size that isn't whole SPIR-V words is APRESULT_INVALID_ARGUMENT;
 * a failed read APRESULT_IO; [error] says why. */
static APRESULT aprend_read_spirv_file(const char *filename, uint8_t **out_data, uint64_t *out_size, APREND_ERROR *error) {
	*out_data = nullptr;
	*out_size = 0;
	const SPUD_ALLOCATOR allocator = spud_allocator_system();
	spudfile_instance_desc files_desc   = {};
	files_desc.base.type           = SPUD_STRUCT_TYPE_FILES_INSTANCE_DESC;
	files_desc.allocator           = &allocator;
	spudfile_instance files             = nullptr;
	SPUDRESULT sr                  = spudfile_instance_create(&files_desc, &files);
	if (SPUDFAIL(sr))
		return aprend_fail(error, APRESULT_IO, "'%s' can't be read: %s", filename, spudresult_str(sr));

	spudfile_open_desc open_desc = {};
	open_desc.base.type     = SPUD_STRUCT_TYPE_FILES_OPEN_DESC;
	open_desc.path          = filename;
	open_desc.access        = SPUDFILE_ACCESS_READ;
	open_desc.create        = SPUDFILE_CREATE_OPEN_EXISTING;
	open_desc.share         = SPUDFILE_SHARE_READ | SPUDFILE_SHARE_WRITE | SPUDFILE_SHARE_DELETE;
	spudfile_file file           = nullptr;
	sr                      = spudfile_file_open(files, &open_desc, &file);
	uint64_t size           = 0;
	if (!SPUDFAIL(sr))
		sr = spudfile_file_get_size(file, &size);
	if (!SPUDFAIL(sr) && (size == 0 || size % 4 != 0)) {
		spudfile_file_close(file);
		spudfile_instance_destroy(files);
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "'%s' is %llu bytes, not a whole number of SPIR-V words", filename,
		                   (unsigned long long)size);
	}
	uint8_t *data = nullptr;
	if (!SPUDFAIL(sr)) {
		data = (uint8_t *)malloc(size);
		if (!data)
			sr = SPUDRESULT_OUT_OF_MEMORY;
	}
	if (!SPUDFAIL(sr)) {
		uint64_t read = 0;
		sr            = spudfile_file_read_at(file, 0, data, size, &read);
		if (!SPUDFAIL(sr) && read != size)
			sr = SPUDRESULT_API_SPECIFIC_FAILURE; // shrunk while being read
	}
	spudfile_file_close(file);
	spudfile_instance_destroy(files);
	if (SPUDFAIL(sr)) {
		free(data);
		if (sr == SPUDRESULT_SPUDFILE_NOT_FOUND)
			return aprend_fail(error, APRESULT_NOT_FOUND, "'%s' doesn't exist", filename);
		if (sr == SPUDRESULT_OUT_OF_MEMORY)
			return aprend_fail(error, APRESULT_OUT_OF_MEMORY, "no memory for the %llu bytes of '%s'", (unsigned long long)size, filename);
		return aprend_fail(error, APRESULT_IO, "'%s' can't be read: %s", filename, spudresult_str(sr));
	}
	*out_data = data;
	*out_size = size;
	return APRESULT_OK;
}

APRESULT aprend_shader_read_from_file_spirv(
    aprend_instance instance,
    const char *filename,
    SPUDGPU_SHADER_STAGE shader_stage,
    aprend_shader *out_shader,
    APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (out_shader)
		*out_shader = nullptr;
	if (!instance)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is NULL");
	APREND_ENTER(instance, "aprend_shader_read_from_file_spirv");
	if (!filename)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "filename is NULL");
	if (const APRESULT result = aprend_shader_stage_supported(shader_stage, out_error))
		return result;
	if (const APRESULT result = aprend_shader_stage_feature_ok(instance, shader_stage, out_error))
		return result;
	if (!out_shader)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_shader is NULL");

	uint8_t *data      = nullptr;
	uint64_t data_size = 0;
	if (const APRESULT result = aprend_read_spirv_file(filename, &data, &data_size, out_error))
		return result;
	// The raw file bytes aren't needed once SpudGPU has its own copy.
	const APRESULT result = aprend_shader_make(instance, data, data_size, shader_stage, filename, out_shader, out_error);
	free(data);
	return result;
}
void aprend_shader_destroy(aprend_shader shader) {
	if (!shader)
		return;
	// Released once the GPU has finished everything submitted so far.
	aprend_instance_retire(shader->instance, shader, &aprend_release_handle<aprend_shader_t>);
}

const APREND_SHADER_REFLECTION *aprend_shader_get_reflection(aprend_shader shader) { return shader ? &shader->reflection.view : nullptr; }

const APREND_SHADER_RESOURCE *aprend_shader_find_resource(aprend_shader shader, const char *name) {
	if (!shader)
		return nullptr;
	if (!name)
		return nullptr;
	for (const APREND_SHADER_RESOURCE &resource : shader->reflection.resources) {
		if (strcmp(resource.name, name) == 0)
			return &resource;
		if (resource.block_name && strcmp(resource.block_name, name) == 0)
			return &resource;
	}
	return nullptr;
}

SPUDGPU_SHADER_STAGE aprend_shader_get_stage(aprend_shader shader) { return shader ? shader->stage : (SPUDGPU_SHADER_STAGE)SPUDGPU_SHADER_STAGE_NONE; }

spudgpu_blend_attachment_desc aprend_blend_premultiplied(void) {
	spudgpu_blend_attachment_desc blend{};
	blend.blend_enable           = true;
	blend.src_color_blend_factor = SPUDGPU_BLEND_FACTOR_ONE;
	blend.dst_color_blend_factor = SPUDGPU_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blend.color_blend_op         = SPUDGPU_BLEND_OP_ADD;
	blend.src_alpha_blend_factor = SPUDGPU_BLEND_FACTOR_ONE;
	blend.dst_alpha_blend_factor = SPUDGPU_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blend.alpha_blend_op         = SPUDGPU_BLEND_OP_ADD;
	return blend;
}
spudgpu_blend_attachment_desc aprend_blend_alpha(void) {
	spudgpu_blend_attachment_desc blend = aprend_blend_premultiplied();
	blend.src_color_blend_factor        = SPUDGPU_BLEND_FACTOR_SRC_ALPHA;
	return blend;
}
spudgpu_blend_attachment_desc aprend_blend_additive(void) {
	spudgpu_blend_attachment_desc blend = aprend_blend_premultiplied();
	blend.dst_color_blend_factor        = SPUDGPU_BLEND_FACTOR_ONE;
	blend.dst_alpha_blend_factor        = SPUDGPU_BLEND_FACTOR_ONE;
	return blend;
}

APRESULT aprend_graphics_pipeline_create(
    aprend_instance instance,
    const APREND_GRAPHICS_PIPELINE_DESC *desc_in,
    aprend_graphics_pipeline *out_pipeline,
    APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (out_pipeline)
		*out_pipeline = nullptr;
	if (!instance)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is NULL");
	APREND_ENTER(instance, "aprend_graphics_pipeline_create");
	if (!desc_in)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is NULL");
	if (const APRESULT base = aprend_base_check(out_error, "desc", &desc_in->base, APRI_STRUCT_TYPE_APREND_GRAPHICS_PIPELINE_DESC))
		return base;
	APREND_GRAPHICS_PIPELINE_DESC desc = *desc_in;
	if (!desc.fragment_shader)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "fragment_shader is NULL");
	if (desc.fragment_shader->stage != SPUDGPU_SHADER_STAGE_FRAGMENT)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "fragment_shader was not created for the fragment stage");
	if (desc.fragment_shader->instance != instance)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "fragment_shader belongs to another instance");
	// The geometry comes from a vertex shader or a mesh shader, never both.
	if (desc.mesh_shader) {
		if (desc.vertex_shader)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "both vertex_shader and mesh_shader are given");
		if (desc.mesh_shader->stage != SPUDGPU_SHADER_STAGE_MESH)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "mesh_shader was not created for the mesh stage");
		if (desc.mesh_shader->instance != instance)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "mesh_shader belongs to another instance");
		if (!desc._mesh_entry_point)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_mesh_entry_point is NULL");
		if (!desc._mesh_entry_point[0])
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_mesh_entry_point is an empty string");
		if (desc._vertex_binding_count != 0)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "a mesh shader pipeline has no vertex input, and _vertex_binding_count is %u", desc._vertex_binding_count);
		if (!(instance->features & SPUDGPU_FEATURE_MESH_SHADING))
			return aprend_fail(out_error, APRESULT_UNSUPPORTED, "mesh_shader is given and the device was created without SPUDGPU_FEATURE_MESH_SHADING");
	} else {
		if (!desc.vertex_shader)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "neither vertex_shader nor mesh_shader is given");
		if (desc.vertex_shader->stage != SPUDGPU_SHADER_STAGE_VERTEX)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "vertex_shader was not created for the vertex stage");
		if (desc.vertex_shader->instance != instance)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "vertex_shader belongs to another instance");
		if (!desc._vertex_entry_point)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_vertex_entry_point is NULL");
		if (!desc._vertex_entry_point[0])
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_vertex_entry_point is an empty string");
	}
	if (!desc._fragment_entry_point)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_fragment_entry_point is NULL");
	if (!desc._fragment_entry_point[0])
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_fragment_entry_point is an empty string");
	if (desc._vertex_binding_count > APREND_MAX_VERTEX_BINDINGS)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_vertex_binding_count %u is above APREND_MAX_VERTEX_BINDINGS", desc._vertex_binding_count);
	uint32_t vertex_attribute_count = 0;
	for (uint32_t i = 0; i < desc._vertex_binding_count; ++i) {
		const APREND_VERTEX_LAYOUT &layout = desc._vertex_bindings[i]._layout;
		if (layout.count == 0)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_vertex_bindings[%u] has no elements", i);
		if (!layout.elements)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_vertex_bindings[%u] has a NULL elements array", i);
		if (layout.stride == 0)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_vertex_bindings[%u] has a stride of 0", i);
		// Every format is 32-bit, which each API reads from 4-byte offsets.
		if (layout.stride % 4 != 0)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_vertex_bindings[%u] has a stride of %u, not a multiple of 4", i, layout.stride);
		for (uint32_t e = 0; e < layout.count; ++e) {
			const APREND_VERTEX_ELEMENT &element = layout.elements[e];
			const uint32_t size                  = aprend_vertex_element_type_get_size(element.type);
			if (size == 0)
				return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "element %u of _vertex_bindings[%u] has type %u, which is not an APREND_VERTEX_ELEMENT_TYPE", e, i, (unsigned)element.type);
			if (element.offset % 4 != 0)
				return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "element %u of _vertex_bindings[%u] has an offset of %u, not a multiple of 4", e, i, element.offset);
			if ((uint64_t)element.offset + size > layout.stride)
				return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "element %u of _vertex_bindings[%u] ends at %llu, past the stride of %u", e, i, (unsigned long long)element.offset + size, layout.stride);
		}
		if (layout.count > SPUDGPU_MAX_VERTEX_ATTRIBUTES - vertex_attribute_count)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "the vertex bindings hold more than SPUDGPU_MAX_VERTEX_ATTRIBUTES elements");
		vertex_attribute_count += layout.count;
	}
	if (desc._push_constant_range_count > APREND_MAX_PUSH_CONSTANT_RANGES)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_push_constant_range_count %u is above APREND_MAX_PUSH_CONSTANT_RANGES", desc._push_constant_range_count);
	for (uint32_t i = 0; i < desc._push_constant_range_count; ++i) {
		const spudgpu_push_constant_range_desc &range = desc._push_constant_ranges[i];
		if (range.stage_flags == SPUDGPU_SHADER_STAGE_NONE)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_push_constant_ranges[%u] has no stages", i);
		if (range.size == 0)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_push_constant_ranges[%u] has a size of 0", i);
		if (range.offset % 4 != 0)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_push_constant_ranges[%u] has an offset that is not a multiple of 4", i);
		if (range.size % 4 != 0)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_push_constant_ranges[%u] has a size that is not a multiple of 4", i);
		if (range.size > UINT32_MAX - range.offset)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_push_constant_ranges[%u] runs past what 32 bits can address", i);
	}
	if (desc._cull_mode > SPUDGPU_CULL_MODE_BACK)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_cull_mode %u is not a SPUDGPU_CULL_MODE", (unsigned)desc._cull_mode);
	if (desc._depth_test && desc._depth_compare_op == SPUDGPU_COMPARE_OP_NEVER) {
		// NEVER would discard every fragment - almost certainly an unset field.
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_depth_test is set but _depth_compare_op is NEVER");
	}
	if (desc._depth_bias_clamp != 0.0f && !(instance->features & SPUDGPU_FEATURE_DEPTH_BIAS_CLAMP))
		return aprend_fail(out_error, APRESULT_UNSUPPORTED, "_depth_bias_clamp is %g and the device was created without SPUDGPU_FEATURE_DEPTH_BIAS_CLAMP", (double)desc._depth_bias_clamp);
	if (desc.color_attachment_count > APREND_MAX_COLOR_TARGETS)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "color_attachment_count %u is above APREND_MAX_COLOR_TARGETS", desc.color_attachment_count);
	for (uint32_t i = 0; i < desc.color_attachment_count; ++i) {
		if (desc.color_attachment_formats[i] == SPUDGPU_FORMAT_UNKNOWN)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "color_attachment_formats[%u] is SPUDGPU_FORMAT_UNKNOWN", i);
	}
	if (!(instance->features & SPUDGPU_FEATURE_INDEPENDENT_BLEND)) {
		for (uint32_t i = 1; i < desc.color_attachment_count; ++i) {
			if (!aprend_blends_equal(desc._blends[0], desc._blends[i]))
				return aprend_fail(out_error, APRESULT_UNSUPPORTED, "_blends[%u] differs from _blends[0] and the device was created without SPUDGPU_FEATURE_INDEPENDENT_BLEND", i);
		}
	}
	if (desc._binding_layout_count > APREND_MAX_BINDING_LAYOUTS)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_binding_layout_count %u is above APREND_MAX_BINDING_LAYOUTS", desc._binding_layout_count);
	for (uint32_t i = 0; i < desc._binding_layout_count; ++i) {
		if (!desc._binding_layouts[i])
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_binding_layouts[%u] is NULL", i);
		if (desc._binding_layouts[i]->instance != instance)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_binding_layouts[%u] belongs to another instance", i);
	}

	if (!out_pipeline)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_pipeline is NULL");

	APREND_MALLOC__T(result, aprend_graphics_pipeline_t);
	if (!result)
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the pipeline");
	APREND_CONSTRUCT__T(result, aprend_graphics_pipeline_t);

	result->desc     = desc;
	result->instance = instance;

	// The vertex layouts' elements and their names, copied into one block:
	// every binding's elements, then every name. The desc then points at the
	// copies, so aprend_graphics_pipeline_get_desc never hands back the
	// caller's memory, and everything below reads them.
	{
		size_t element_count = 0;
		size_t name_bytes    = 0;
		for (uint32_t i = 0; i < desc._vertex_binding_count; ++i) {
			const APREND_VERTEX_LAYOUT &layout = desc._vertex_bindings[i]._layout;
			element_count += layout.count;
			for (uint32_t e = 0; e < layout.count; ++e)
				if (layout.elements[e].name)
					name_bytes += strlen(layout.elements[e].name) + 1;
		}
		if (element_count > 0) {
			result->vertex_elements = (char *)malloc(element_count * sizeof(APREND_VERTEX_ELEMENT) + name_bytes);
			if (!result->vertex_elements) {
				APREND_DESTRUCT__T(result, aprend_graphics_pipeline_t);
				return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the vertex layouts' copy");
			}
			APREND_VERTEX_ELEMENT *elements = (APREND_VERTEX_ELEMENT *)result->vertex_elements;
			char *names                     = result->vertex_elements + element_count * sizeof(APREND_VERTEX_ELEMENT);
			for (uint32_t i = 0; i < desc._vertex_binding_count; ++i) {
				APREND_VERTEX_LAYOUT &layout = result->desc._vertex_bindings[i]._layout;
				memcpy(elements, layout.elements, layout.count * sizeof(APREND_VERTEX_ELEMENT));
				for (uint32_t e = 0; e < layout.count; ++e) {
					if (!elements[e].name)
						continue;
					const size_t length = strlen(elements[e].name) + 1;
					memcpy(names, elements[e].name, length);
					elements[e].name = names;
					names += length;
				}
				layout.elements = elements;
				elements += layout.count;
			}
		}
	}

	// The desc against what the shaders declare, and the locations each
	// vertex element is read at.
	std::vector<aprend_vertex_attribute_plan> plan;
	try {
		aprend_shader geometry     = desc.mesh_shader ? desc.mesh_shader : desc.vertex_shader;
		const char *geometry_entry = desc.mesh_shader ? desc._mesh_entry_point : desc._vertex_entry_point;
		APRESULT checked = aprend_pipeline_check_shader(out_error, geometry, geometry_entry, desc._binding_layouts, desc._binding_layout_count,
		                                                desc._push_constant_ranges, desc._push_constant_range_count, result->images,
		                                                result->storage_writes);
		if (checked == APRESULT_OK)
			checked = aprend_pipeline_check_shader(out_error, desc.fragment_shader, desc._fragment_entry_point, desc._binding_layouts,
			                                       desc._binding_layout_count, desc._push_constant_ranges, desc._push_constant_range_count,
			                                       result->images, result->storage_writes);
		if (checked == APRESULT_OK)
			checked = aprend_pipeline_check_outputs(out_error, desc);
		if (checked == APRESULT_OK && !desc.mesh_shader)
			checked = aprend_pipeline_plan_vertex_input(out_error, result->desc, desc.vertex_shader, plan);
		if (checked != APRESULT_OK) {
			APREND_DESTRUCT__T(result, aprend_graphics_pipeline_t);
			return checked;
		}
	} catch (const std::bad_alloc &) {
		APREND_DESTRUCT__T(result, aprend_graphics_pipeline_t);
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory to check the shaders against the desc");
	}

	// The names are copied, so the desc aprend_graphics_pipeline_get_desc
	// hands back never points at a string the caller has released.
	// The first name is the vertex shader's, or the mesh shader's in a mesh
	// shader pipeline; the other of the two is not read and is left as the
	// caller gave it.
	result->is_mesh                  = desc.mesh_shader != nullptr;
	const char *geometry_entry       = result->is_mesh ? desc._mesh_entry_point : desc._vertex_entry_point;
	const size_t vertex_entry_size   = strlen(geometry_entry) + 1;
	const size_t fragment_entry_size = strlen(desc._fragment_entry_point) + 1;
	result->entry_points             = (char *)malloc(vertex_entry_size + fragment_entry_size);
	if (!result->entry_points) {
		APREND_DESTRUCT__T(result, aprend_graphics_pipeline_t);
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the entry points' copy");
	}
	memcpy(result->entry_points, geometry_entry, vertex_entry_size);
	memcpy(result->entry_points + vertex_entry_size, desc._fragment_entry_point, fragment_entry_size);
	result->desc._fragment_entry_point = result->entry_points + vertex_entry_size;

	spudgpu_shader_pipeline_desc pd{};
	pd.base.type = SPUD_STRUCT_TYPE_GPU_SHADER_PIPELINE_DESC;
	if (result->is_mesh) {
		result->desc._mesh_entry_point   = result->entry_points;
		result->desc._vertex_entry_point = nullptr;
		pd.mesh_module                   = desc.mesh_shader->shader_module;
		pd.mesh_entry_point              = result->entry_points;
	} else {
		result->desc._vertex_entry_point = result->entry_points;
		result->desc._mesh_entry_point   = nullptr;
		pd.vertex_module                 = desc.vertex_shader->shader_module;
		pd.vertex_entry_point            = result->entry_points;
	}
	pd.fragment_module      = desc.fragment_shader->shader_module;
	pd.fragment_entry_point = result->desc._fragment_entry_point;

	for (uint32_t binding = 0; binding < desc._vertex_binding_count; ++binding) {
		const APREND_VERTEX_BINDING_DESC &vb = result->desc._vertex_bindings[binding];
		result->vertex_strides[binding]      = vb._layout.stride;
		for (uint32_t e = 0; e < vb._layout.count; ++e) {
			const APREND_VERTEX_ELEMENT &element = vb._layout.elements[e];
			const uint32_t end                   = element.offset + aprend_vertex_element_type_get_size(element.type);
			if (end > result->vertex_extents[binding])
				result->vertex_extents[binding] = end;
		}

		pd.vertex_bindings[binding].binding      = binding;
		pd.vertex_bindings[binding].stride       = result->vertex_strides[binding];
		pd.vertex_bindings[binding].per_instance = vb._per_instance;
	}
	pd.vertex_binding_count = desc._vertex_binding_count;
	// At the locations the plan gives them: by position, running on through
	// the bindings in order, or by name. An element the shader doesn't read
	// by name is no attribute.
	for (const aprend_vertex_attribute_plan &planned : plan) {
		if (planned.location == UINT32_MAX)
			continue;
		spudgpu_vertex_attribute_desc &attribute = pd.vertex_attributes[pd.vertex_attribute_count];
		attribute.location                       = planned.location;
		attribute.binding                        = planned.binding;
		attribute.format                         = aprend_vertex_element_type_to_format(planned.element->type);
		attribute.offset                         = planned.element->offset;
		++pd.vertex_attribute_count;
	}

	for (uint32_t i = 0; i < desc._push_constant_range_count; ++i) {
		pd.push_constant_ranges[i] = desc._push_constant_ranges[i];
		const uint32_t range_end   = desc._push_constant_ranges[i].offset + desc._push_constant_ranges[i].size;
		if (range_end > result->push_constant_end)
			result->push_constant_end = range_end;
	}
	pd.push_constant_range_count = desc._push_constant_range_count;

	pd.primitive_topology = desc._topology;
	pd.cull_mode          = desc._cull_mode;
	pd.front_face_ccw     = desc._front_face_ccw;
	pd.wireframe          = desc._wireframe;
	pd.depth_test_enable  = desc._depth_test;
	pd.depth_write_enable = desc._depth_write;
	pd.depth_compare_op   = desc._depth_compare_op;
	pd.depth_bias_constant = desc._depth_bias_constant;
	pd.depth_bias_slope    = desc._depth_bias_slope;
	pd.depth_bias_clamp    = desc._depth_bias_clamp;

	for (uint32_t i = 0; i < desc.color_attachment_count; ++i) {
		pd.color_attachment_formats[i] = desc.color_attachment_formats[i];
		pd.blend_attachments[i]        = desc._blends[i];
	}
	pd.color_attachment_count = desc.color_attachment_count;
	pd.depth_format           = desc.depth_format;
	for (uint32_t i = 0; i < desc._binding_layout_count; ++i)
		pd.descriptor_set_layouts[i] = desc._binding_layouts[i]->core->layout;
	pd.descriptor_set_layout_count = desc._binding_layout_count;
	pd.debug_name = desc._debug_name;

	SPUDRESULT sr = spudgpu_create_shader_pipeline(instance->desc.device, &pd, &result->pipeline);
	if (SPUDFAIL(sr)) {
		APREND_DESTRUCT__T(result, aprend_graphics_pipeline_t);
		return aprend_fail_spud(out_error, sr, "creating the pipeline failed");
	}
	// Its destroy's retire, promised now so the destroy can't fail.
	if (aprend_instance_reserve_release(instance, 1) != APRESULT_OK) {
		APREND_DESTRUCT__T(result, aprend_graphics_pipeline_t);
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory to release the pipeline later");
	}

	*out_pipeline = result;
	return APRESULT_OK;
}
void aprend_graphics_pipeline_destroy(aprend_graphics_pipeline p) {
	if (!p)
		return;
	// A compiled list that binds it is refused at submit.
	aprend_instance_forget(p->instance, p);
	// Released once the GPU has finished everything submitted so far.
	aprend_instance_retire(p->instance, p, &aprend_release_handle<aprend_graphics_pipeline_t>);
}
APRESULT aprend_graphics_pipeline_get_desc(
    aprend_graphics_pipeline p,
    APREND_GRAPHICS_PIPELINE_DESC *out_desc) {
	if (!p)
		return APRESULT_INVALID_ARGUMENT;
	if (!out_desc)
		return APRESULT_INVALID_ARGUMENT;
	*out_desc      = p->desc;
	out_desc->base = apri_struct_base(APRI_STRUCT_TYPE_APREND_GRAPHICS_PIPELINE_DESC);
	return APRESULT_OK;
}

uint32_t aprend_vertex_element_type_get_size(APREND_VERTEX_ELEMENT_TYPE type) {
	switch (type) {
	case APREND_VERTEX_ELEMENT_TYPE_FLOAT:
	case APREND_VERTEX_ELEMENT_TYPE_INT:
	case APREND_VERTEX_ELEMENT_TYPE_UINT:
		return 4;
	case APREND_VERTEX_ELEMENT_TYPE_VEC2:
	case APREND_VERTEX_ELEMENT_TYPE_INT2:
	case APREND_VERTEX_ELEMENT_TYPE_UINT2:
		return 8;
	case APREND_VERTEX_ELEMENT_TYPE_VEC3:
	case APREND_VERTEX_ELEMENT_TYPE_INT3:
	case APREND_VERTEX_ELEMENT_TYPE_UINT3:
		return 12;
	case APREND_VERTEX_ELEMENT_TYPE_VEC4:
	case APREND_VERTEX_ELEMENT_TYPE_INT4:
	case APREND_VERTEX_ELEMENT_TYPE_UINT4:
		return 16;
	default:
		return 0;
	}
}

uint32_t aprend_vertex_layout_get_element_index(const APREND_VERTEX_LAYOUT *layout, const char *name) {
	if (!layout)
		return UINT32_MAX;
	if (!layout->elements)
		return UINT32_MAX;
	if (!name)
		return UINT32_MAX;
	for (uint32_t i = 0; i < layout->count; ++i) {
		const char *element_name = layout->elements[i].name;
		if (element_name && strcmp(element_name, name) == 0)
			return i;
	}
	return UINT32_MAX;
}

APRESULT aprend_compute_pipeline_create(
    aprend_instance instance,
    const APREND_COMPUTE_PIPELINE_DESC *desc_in,
    aprend_compute_pipeline *out_pipeline,
    APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (out_pipeline)
		*out_pipeline = nullptr;
	if (!instance)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is NULL");
	APREND_ENTER(instance, "aprend_compute_pipeline_create");
	if (!desc_in)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is NULL");
	if (const APRESULT base = aprend_base_check(out_error, "desc", &desc_in->base, APRI_STRUCT_TYPE_APREND_COMPUTE_PIPELINE_DESC))
		return base;
	APREND_COMPUTE_PIPELINE_DESC desc = *desc_in;
	if (!desc.compute_shader)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "compute_shader is NULL");
	if (desc.compute_shader->instance != instance)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "compute_shader belongs to another instance");
	if (desc.compute_shader->stage != SPUDGPU_SHADER_STAGE_COMPUTE)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "compute_shader was not created for the compute stage");
	if (!desc._entry_point)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_entry_point is NULL");
	if (!desc._entry_point[0])
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_entry_point is an empty string");
	if (desc._binding_layout_count > APREND_MAX_BINDING_LAYOUTS)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_binding_layout_count %u is above APREND_MAX_BINDING_LAYOUTS", desc._binding_layout_count);
	for (uint32_t i = 0; i < desc._binding_layout_count; ++i) {
		if (!desc._binding_layouts[i])
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_binding_layouts[%u] is NULL", i);
		if (desc._binding_layouts[i]->instance != instance)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_binding_layouts[%u] belongs to another instance", i);
	}
	if (desc._push_constant_range_count > APREND_MAX_PUSH_CONSTANT_RANGES)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_push_constant_range_count %u is above APREND_MAX_PUSH_CONSTANT_RANGES", desc._push_constant_range_count);
	for (uint32_t i = 0; i < desc._push_constant_range_count; ++i) {
		const spudgpu_push_constant_range_desc &range = desc._push_constant_ranges[i];
		if (range.stage_flags != SPUDGPU_SHADER_STAGE_COMPUTE)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_push_constant_ranges[%u] has a stage other than compute alone", i);
		if (range.size == 0)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_push_constant_ranges[%u] has a size of 0", i);
		if (range.offset % 4 != 0)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_push_constant_ranges[%u] has an offset that is not a multiple of 4", i);
		if (range.size % 4 != 0)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_push_constant_ranges[%u] has a size that is not a multiple of 4", i);
		if (range.size > UINT32_MAX - range.offset)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_push_constant_ranges[%u] runs past what 32 bits can address", i);
	}

	if (!out_pipeline)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_pipeline is NULL");

	APREND_MALLOC__T(result, aprend_compute_pipeline_t);
	if (!result)
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the pipeline");
	APREND_CONSTRUCT__T(result, aprend_compute_pipeline_t);

	result->desc     = desc;
	result->instance = instance;

	try {
		if (const APRESULT checked = aprend_pipeline_check_shader(out_error, desc.compute_shader, desc._entry_point, desc._binding_layouts,
		                                                          desc._binding_layout_count, desc._push_constant_ranges,
		                                                          desc._push_constant_range_count, result->images, result->storage_writes)) {
			APREND_DESTRUCT__T(result, aprend_compute_pipeline_t);
			return checked;
		}
	} catch (const std::bad_alloc &) {
		APREND_DESTRUCT__T(result, aprend_compute_pipeline_t);
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory to check the shader against the desc");
	}

	// The name is copied, so the desc aprend_compute_pipeline_get_desc hands
	// back never points at a string the caller has released.
	const size_t entry_size = strlen(desc._entry_point) + 1;
	result->entry_point     = (char *)malloc(entry_size);
	if (!result->entry_point) {
		APREND_DESTRUCT__T(result, aprend_compute_pipeline_t);
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the entry point's copy");
	}
	memcpy(result->entry_point, desc._entry_point, entry_size);
	result->desc._entry_point = result->entry_point;

	spudgpu_compute_pipeline_desc pd{};
	pd.base.type = SPUD_STRUCT_TYPE_GPU_COMPUTE_PIPELINE_DESC;
	pd.compute_module      = desc.compute_shader->shader_module;
	pd.compute_entry_point = result->entry_point;
	for (uint32_t i = 0; i < desc._binding_layout_count; ++i)
		pd.descriptor_set_layouts[i] = desc._binding_layouts[i]->core->layout;
	pd.descriptor_set_layout_count = desc._binding_layout_count;
	for (uint32_t i = 0; i < desc._push_constant_range_count; ++i) {
		pd.push_constant_ranges[i] = desc._push_constant_ranges[i];
		const uint32_t range_end   = desc._push_constant_ranges[i].offset + desc._push_constant_ranges[i].size;
		if (range_end > result->push_constant_end)
			result->push_constant_end = range_end;
	}
	pd.push_constant_range_count = desc._push_constant_range_count;
	pd.debug_name = desc._debug_name;

	SPUDRESULT sr = spudgpu_create_compute_pipeline(instance->desc.device, &pd, &result->pipeline);
	if (SPUDFAIL(sr)) {
		result->pipeline = nullptr;
		APREND_DESTRUCT__T(result, aprend_compute_pipeline_t);
		return aprend_fail_spud(out_error, sr, "creating the pipeline failed");
	}
	// Its destroy's retire, promised now so the destroy can't fail.
	if (aprend_instance_reserve_release(instance, 1) != APRESULT_OK) {
		APREND_DESTRUCT__T(result, aprend_compute_pipeline_t);
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory to release the pipeline later");
	}

	*out_pipeline = result;
	return APRESULT_OK;
}
void aprend_compute_pipeline_destroy(aprend_compute_pipeline p) {
	if (!p)
		return;
	// A compiled list that binds it is refused at submit.
	aprend_instance_forget(p->instance, p);
	// Released once the GPU has finished everything submitted so far.
	aprend_instance_retire(p->instance, p, &aprend_release_handle<aprend_compute_pipeline_t>);
}
APRESULT aprend_compute_pipeline_get_desc(
    aprend_compute_pipeline p,
    APREND_COMPUTE_PIPELINE_DESC *out_desc) {
	if (!p)
		return APRESULT_INVALID_ARGUMENT;
	if (!out_desc)
		return APRESULT_INVALID_ARGUMENT;
	*out_desc      = p->desc;
	out_desc->base = apri_struct_base(APRI_STRUCT_TYPE_APREND_COMPUTE_PIPELINE_DESC);
	return APRESULT_OK;
}

APRESULT aprend_binding_layout_create(
    aprend_instance instance,
    const APREND_BINDING_DESC *bindings,
    uint32_t binding_count,
    aprend_binding_layout *out_layout,
    APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (out_layout)
		*out_layout = nullptr;
	if (!instance)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is NULL");
	APREND_ENTER(instance, "aprend_binding_layout_create");
	if (!bindings)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "bindings is NULL");
	if (binding_count == 0)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "binding_count is 0");
	if (binding_count > APREND_MAX_BINDINGS_PER_LAYOUT)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "binding_count %u is above APREND_MAX_BINDINGS_PER_LAYOUT", binding_count);
	for (uint32_t i = 0; i < binding_count; ++i) {
		if (bindings[i]._type >= APREND_DESCRIPTOR_TYPE_COUNT)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "entry %u has a _type that is not a SPUDGPU_DESCRIPTOR_TYPE", i);
		if (bindings[i]._count == 0)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "entry %u has a _count of 0", i);
		if (bindings[i]._stages == SPUDGPU_SHADER_STAGE_NONE)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "entry %u has no _stages", i);
		for (uint32_t j = 0; j < i; ++j) {
			if (bindings[j]._binding == bindings[i]._binding)
				return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "entries %u and %u share binding %u", j, i, bindings[i]._binding);
		}
	}

	if (!out_layout)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_layout is NULL");

	return aprend_binding_layout_build(instance, bindings, binding_count, out_layout, out_error);
}

// Whether two declarations of one block, [a_count] members from [a] and
// [b_count] from [b], agree: the shorter's members are the first of the
// longer's, each with the same name, type and place. Two stages may declare
// one block to different lengths (each the members it reads); two that lay
// a member out differently would each read the other's writes wrongly.
static bool aprend_block_declarations_agree(const APREND_SHADER_MEMBER *a, uint32_t a_count, const APREND_SHADER_MEMBER *b, uint32_t b_count) {
	const uint32_t count = a_count < b_count ? a_count : b_count;
	for (uint32_t m = 0; m < count; ++m) {
		if (strcmp(a[m].name, b[m].name) != 0)
			return false;
		if (a[m].type != b[m].type)
			return false;
		if (a[m].offset != b[m].offset)
			return false;
		if (a[m].array_count != b[m].array_count)
			return false;
		if (a[m].array_stride != b[m].array_stride)
			return false;
		if (a[m].matrix_stride != b[m].matrix_stride)
			return false;
		if (a[m].row_major != b[m].row_major)
			return false;
	}
	return true;
}

APRESULT aprend_binding_layout_create_from_shaders(
    aprend_instance instance,
    const aprend_shader *shaders,
    uint32_t shader_count,
    uint32_t set,
    aprend_binding_layout *out_layout,
    APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (out_layout)
		*out_layout = nullptr;
	if (!instance)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is NULL");
	APREND_ENTER(instance, "aprend_binding_layout_create_from_shaders");
	if (!shaders)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "shaders is NULL");
	if (shader_count == 0)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "shader_count is 0");
	for (uint32_t i = 0; i < shader_count; ++i) {
		if (!shaders[i])
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "shaders[%u] is NULL", i);
		if (shaders[i]->instance != instance)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "shaders[%u] is of another instance", i);
	}
	if (!out_layout)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_layout is NULL");

	// The bindings of [set] every shader uses, merged, and for each the
	// shader resource its name and block are taken from.
	APREND_BINDING_DESC bindings[APREND_MAX_BINDINGS_PER_LAYOUT]{};
	const APREND_SHADER_RESOURCE *sources[APREND_MAX_BINDINGS_PER_LAYOUT]{};
	const APREND_SHADER_MEMBER *source_members[APREND_MAX_BINDINGS_PER_LAYOUT]{};
	uint32_t binding_count = 0;
	for (uint32_t i = 0; i < shader_count; ++i) {
		const APREND_SHADER_REFLECTION &reflection = shaders[i]->reflection.view;
		for (uint32_t r = 0; r < reflection.resource_count; ++r) {
			const APREND_SHADER_RESOURCE &resource = reflection.resources[r];
			if (resource.set != set || !resource.used)
				continue;
			uint32_t index = 0;
			while (index < binding_count && bindings[index]._binding != resource.binding)
				++index;
			if (index == binding_count) {
				if (binding_count == APREND_MAX_BINDINGS_PER_LAYOUT)
					return aprend_fail(out_error, APRESULT_LIMIT, "set %u has more than APREND_MAX_BINDINGS_PER_LAYOUT bindings", set);
				++binding_count;
				bindings[index]._binding = resource.binding;
				bindings[index]._type    = resource.type;
				bindings[index]._count   = resource.count;
				bindings[index]._stages  = reflection.stage;
				bindings[index]._name    = resource.name;
				sources[index]           = &resource;
				source_members[index]    = reflection.members;
				continue;
			}
			APREND_BINDING_DESC &binding = bindings[index];
			if (binding._type != resource.type)
				return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "set %u binding %u is of type %u in one shader and %u in another", set, resource.binding, (unsigned)binding._type, (unsigned)resource.type);
			binding._stages |= reflection.stage;
			// An array read to different lengths: the layout has the longest.
			if (resource.count > binding._count)
				binding._count = resource.count;
			if (resource.member_count > 0 &&
			    !aprend_block_declarations_agree(source_members[index] + sources[index]->first_member, sources[index]->member_count,
			                                     reflection.members + resource.first_member, resource.member_count)) {
				return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "set %u binding %u is a block the shaders declare with different members (one is not the start of the other)", set, resource.binding);
			}
			// The block as the shader that declares it largest has it.
			if (resource.block_size > sources[index]->block_size) {
				sources[index]        = &resource;
				source_members[index] = reflection.members;
			}
		}
	}
	if (binding_count == 0)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "no shader uses a binding of set %u", set);

	aprend_binding_layout_t *result = nullptr;
	if (const APRESULT built = aprend_binding_layout_build(instance, bindings, binding_count, &result, out_error))
		return built;
	try {
		for (uint32_t i = 0; i < binding_count; ++i) {
			const APREND_SHADER_RESOURCE &resource = *sources[i];
			if (resource.type != SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER && resource.type != SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER)
				continue;
			aprend_binding_layout_t::block_info &block = result->blocks[i];
			block.known                                = true;
			result->strings.push_back(resource.block_name ? resource.block_name : "");
			block.block_name    = result->strings.back().c_str();
			block.size          = resource.block_size;
			block.first_member  = (uint32_t)result->members.size();
			block.member_count  = resource.member_count;
			block.first_uniform = (uint32_t)result->uniforms.size();
			for (uint32_t m = 0; m < resource.member_count; ++m) {
				APREND_SHADER_MEMBER member = source_members[i][resource.first_member + m];
				result->strings.push_back(member.name);
				member.name = result->strings.back().c_str();
				result->members.push_back(member);
				// Every member with a type is a uniform a buffer of this
				// block can be written by name through.
				if (resource.type == SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER && member.type != APREND_UNIFORM_TYPE_UNKNOWN) {
					APREND_UNIFORM uniform{};
					uniform.name        = member.name;
					uniform.type        = member.type;
					uniform.array_count = member.array_count;
					uniform.offset      = member.offset;
					result->uniforms.push_back(uniform);
				}
			}
			block.uniform_count = (uint32_t)result->uniforms.size() - block.first_uniform;
		}
	} catch (const std::bad_alloc &) {
		// Made, its retire promised: given back the way a destroy would.
		aprend_instance_unreserve_release(instance, 1);
		APREND_DESTRUCT__T(result, aprend_binding_layout_t);
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the layout's blocks");
	}
	*out_layout = result;
	return APRESULT_OK;
}

// The index of [binding] in [layout]'s bindings, or UINT32_MAX.
static uint32_t aprend_binding_layout_index(const aprend_binding_layout_t *layout, uint32_t binding) {
	for (uint32_t i = 0; i < layout->binding_count; ++i)
		if (layout->bindings[i]._binding == binding)
			return i;
	return UINT32_MAX;
}

APRESULT aprend_binding_layout_find(
    aprend_binding_layout layout,
    const char *name,
    uint32_t *out_binding,
    APREND_BINDING_DESC *out_desc) {
	if (!layout)
		return APRESULT_INVALID_ARGUMENT;
	if (!name)
		return APRESULT_INVALID_ARGUMENT;
	for (uint32_t i = 0; i < layout->binding_count; ++i) {
		const bool by_name  = !layout->names[i].empty() && layout->names[i] == name;
		const bool by_block = layout->blocks[i].known && strcmp(layout->blocks[i].block_name, name) == 0;
		if (!by_name && !by_block)
			continue;
		if (out_binding)
			*out_binding = layout->bindings[i]._binding;
		if (out_desc)
			*out_desc = layout->bindings[i];
		return APRESULT_OK;
	}
	return APRESULT_NOT_FOUND;
}

APRESULT aprend_binding_layout_get_binding(
    aprend_binding_layout layout,
    uint32_t index,
    APREND_BINDING_DESC *out_desc) {
	if (!layout)
		return APRESULT_INVALID_ARGUMENT;
	if (!out_desc)
		return APRESULT_INVALID_ARGUMENT;
	if (index >= layout->binding_count)
		return APRESULT_INVALID_ARGUMENT;
	*out_desc = layout->bindings[index];
	return APRESULT_OK;
}

uint32_t aprend_binding_layout_get_binding_count(aprend_binding_layout layout) { return layout ? layout->binding_count : 0; }

APRESULT aprend_binding_layout_get_block(
    aprend_binding_layout layout,
    uint32_t binding,
    uint32_t *out_size,
    const APREND_SHADER_MEMBER **out_members,
    uint32_t *out_member_count) {
	if (!layout)
		return APRESULT_INVALID_ARGUMENT;
	const uint32_t index = aprend_binding_layout_index(layout, binding);
	if (index == UINT32_MAX)
		return APRESULT_NOT_FOUND;
	const aprend_binding_layout_t::block_info &block = layout->blocks[index];
	if (!block.known)
		return APRESULT_NOT_FOUND;
	if (out_size)
		*out_size = block.size;
	if (out_members)
		*out_members = layout->members.data() + block.first_member;
	if (out_member_count)
		*out_member_count = block.member_count;
	return APRESULT_OK;
}

APRESULT aprend_binding_layout_get_uniform_layout(
    aprend_binding_layout layout,
    uint32_t binding,
    APREND_UNIFORM_LAYOUT *out) {
	if (!layout)
		return APRESULT_INVALID_ARGUMENT;
	if (!out)
		return APRESULT_INVALID_ARGUMENT;
	const uint32_t index = aprend_binding_layout_index(layout, binding);
	if (index == UINT32_MAX)
		return APRESULT_NOT_FOUND;
	const aprend_binding_layout_t::block_info &block = layout->blocks[index];
	if (!block.known)
		return APRESULT_NOT_FOUND;
	if (layout->bindings[index]._type != SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
		return APRESULT_NOT_FOUND;
	out->count    = block.uniform_count;
	// The vector is never changed after creation, so the pointer holds.
	out->uniforms = const_cast<APREND_UNIFORM *>(layout->uniforms.data()) + block.first_uniform;
	out->size     = block.size;
	return APRESULT_OK;
}
void aprend_binding_layout_destroy(aprend_binding_layout layout) {
	if (!layout)
		return;
	// Released once the GPU has finished everything submitted so far.
	aprend_instance_retire(layout->instance, layout, &aprend_release_handle<aprend_binding_layout_t>);
}

APRESULT aprend_binding_set_create(
    aprend_binding_layout layout,
    const APREND_BINDING_SET_ENTRY *entries,
    uint32_t entry_count,
    aprend_binding_set *out_set,
    APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (out_set)
		*out_set = nullptr;
	if (!layout)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "layout is NULL");
	APREND_ENTER(layout->instance, "aprend_binding_set_create");
	if (!entries)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "entries is NULL");
	if (entry_count != layout->descriptor_count)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "expected %u entries, got %u", layout->descriptor_count, entry_count);
	// Every entry must name an array element the layout declares, once. The
	// count matches, so nothing repeated also means nothing left unfilled.
	uint32_t image_use_count = 0;
	bool has_per_frame       = false;
	for (uint32_t i = 0; i < entry_count; ++i) {
		const APREND_BINDING_SET_ENTRY &entry = entries[i];
		const APREND_BINDING_DESC *binding    = aprend_binding_layout_find_desc(layout, entry._binding);
		if (!binding)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "entry %u names binding %u, which the layout doesn't declare", i, entry._binding);
		if (entry._array_element >= binding->_count)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "entry %u (binding %u) has _array_element %u of %u", i, entry._binding, entry._array_element, binding->_count);
		if (entry._type != binding->_type)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "entry %u (binding %u) has _type %u, the layout declares %u", i, entry._binding, (unsigned)entry._type, (unsigned)binding->_type);
		for (uint32_t j = 0; j < i; ++j) {
			if (entries[j]._binding == entry._binding && entries[j]._array_element == entry._array_element)
				return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "entries %u and %u both fill binding %u element %u", j, i, entry._binding, entry._array_element);
		}
		if (const APRESULT valid = aprend_binding_set_entry_resource_valid(out_error, layout->instance, entry, i))
			return valid;
		if (aprend_descriptor_type_reads_view(entry._type))
			++image_use_count;
		if (entry._type == SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER && entry._resource._uniform._buffer->part_count > 1)
			has_per_frame = true;
		if (entry._type == SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER && entry._resource._storage._buffer->part_count > 1)
			has_per_frame = true;
	}
	if (!out_set)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_set is NULL");
	// One descriptor set if nothing in it changes with the frame; one for
	// each frame in flight if an entry is a buffer of per-frame copies.
	const uint32_t frame_set_count = has_per_frame ? layout->instance->desc.frames_in_flight : 1;

	// What the writes point at; only needed until the set is written.
	struct write_info {
		spudgpu_descriptor_buffer_info buffer;
		spudgpu_descriptor_image_info image;
	};
	write_info *infos = (write_info *)calloc(entry_count, sizeof(write_info));
	if (!infos)
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the set's writes");
	spudgpu_write_descriptor_set *writes = (spudgpu_write_descriptor_set *)calloc(entry_count, sizeof(spudgpu_write_descriptor_set));
	if (!writes) {
		free(infos);
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the set");
	}

	APREND_MALLOC__T(result, aprend_binding_set_t);
	if (!result) {
		free(writes);
		free(infos);
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the set");
	}
	APREND_CONSTRUCT__T(result, aprend_binding_set_t);
	result->layout   = layout;
	// Held from here on: the destructor lets go of it on every path below.
	result->core = layout->core;
	++result->core->holders;
	result->instance = layout->instance;

	// What the set was built from, for aprend_binding_set_update to change
	// part of. entry_count is never 0: a layout has at least one binding.
	result->entries = (APREND_BINDING_SET_ENTRY *)malloc(sizeof(APREND_BINDING_SET_ENTRY) * entry_count);
	if (!result->entries) {
		APREND_DESTRUCT__T(result, aprend_binding_set_t);
		free(writes);
		free(infos);
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the set");
	}
	memcpy(result->entries, entries, sizeof(APREND_BINDING_SET_ENTRY) * entry_count);
	result->entry_count = entry_count;

	if (image_use_count > 0) {
		result->image_uses = (aprend_binding_set_t::image_use *)calloc(image_use_count, sizeof(aprend_binding_set_t::image_use));
		if (!result->image_uses) {
			APREND_DESTRUCT__T(result, aprend_binding_set_t);
			free(writes);
			free(infos);
			return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the set");
		}
	}

	// Room for every entry of every frame's set: at most each one is a
	// staged storage buffer.
	result->storage_store_capacity = entry_count;
	result->storage_stores =
	    (aprend_binding_set_t::storage_store *)calloc((size_t)entry_count * frame_set_count, sizeof(aprend_binding_set_t::storage_store));
	if (!result->storage_stores) {
		APREND_DESTRUCT__T(result, aprend_binding_set_t);
		free(writes);
		free(infos);
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the set");
	}

	// frame_set_count goes up with each set acquired, so the destructor
	// gives back exactly those if a later one fails.
	for (uint32_t frame = 0; frame < frame_set_count; ++frame) {
		SPUDRESULT sr = aprend_binding_layout_acquire_set(layout->core, &result->sets[frame]);
		if (SPUDFAIL(sr)) {
			result->sets[frame] = nullptr;
			APREND_DESTRUCT__T(result, aprend_binding_set_t);
			free(writes);
			free(infos);
			return aprend_fail_spud(out_error, sr, "taking a descriptor set for the set failed");
		}
		++result->frame_set_count;
	}

	for (uint32_t frame = 0; frame < frame_set_count; ++frame) {
		aprend_binding_set_t::storage_store *frame_stores = result->storage_stores + (size_t)frame * entry_count;
		for (uint32_t i = 0; i < entry_count; ++i) {
			const APREND_BINDING_SET_ENTRY &entry = entries[i];
			spudgpu_write_descriptor_set &write   = writes[i];
			write                                 = {};
			write.base.type                       = SPUD_STRUCT_TYPE_GPU_WRITE_DESCRIPTOR_SET;
			infos[i]                              = {};
			write.dst_set                         = result->sets[frame];
			write.dst_binding                     = entry._binding;
			write.dst_array_element               = entry._array_element;
			write.descriptor_count                = 1;
			write.descriptor_type                 = entry._type;

			if (entry._type == SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
				const auto &r                  = entry._resource._uniform;
				const aprend_buffer_part *part = aprend_buffer_part_of(r._buffer, frame);
				infos[i].buffer.buffer         = part->store.buffer;
				infos[i].buffer.offset         = r._offset;
				infos[i].buffer.range          = r._range ? r._range : r._buffer->desc.size - r._offset;
				write.buffer_info                  = &infos[i].buffer;
				continue;
			}
			if (entry._type == SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
				const auto &r            = entry._resource._storage;
				aprend_buffer_part *part = aprend_buffer_part_of(r._buffer, frame);
				infos[i].buffer.buffer   = part->store.buffer;
				infos[i].buffer.offset   = r._offset;
				infos[i].buffer.range    = r._range ? r._range : r._buffer->desc.size - r._offset;
				write.buffer_info                  = &infos[i].buffer;
				// Every storage buffer, staged or not; the same one in two
				// slots is listed twice, and a command list takes each once.
				frame_stores[result->storage_store_counts[frame]++] = {&part->store, entry._binding};
				continue;
			}

			const auto &r = entry._resource._image;
			if (aprend_descriptor_type_reads_view(entry._type)) {
				// A storage image is read and written in GENERAL; anything
				// sampled is read in SHADER_READ_ONLY.
				const SPUDGPU_IMAGE_LAYOUT image_layout =
				    entry._type == SPUDGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE ? SPUDGPU_IMAGE_LAYOUT_GENERAL : SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY;
				infos[i].image.image_view   = r._view->image_view;
				infos[i].image.image_layout = image_layout;
				write.image_info            = &infos[i].image;

				// The same textures in every frame's set: recorded once.
				if (frame == 0)
					result->image_uses[result->image_use_count++] = {r._view, image_layout, entry._binding};
			}
			if (aprend_descriptor_type_reads_sampler(entry._type))
				write.sampler = r._sampler->sampler;
		}
		spudgpu_update_descriptor_sets(layout->instance->desc.device, writes, entry_count);
	}

	free(writes);
	free(infos);
	// Its destroy's retire, promised now so the destroy can't fail.
	if (aprend_instance_reserve_release(layout->instance, 1) != APRESULT_OK) {
		APREND_DESTRUCT__T(result, aprend_binding_set_t);
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory to release the set later");
	}
	*out_set = result;
	return APRESULT_OK;
}
APRESULT aprend_binding_set_update(
    aprend_binding_set set,
    const APREND_BINDING_SET_ENTRY *entries,
    uint32_t entry_count,
    APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!set)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "set is NULL");
	APREND_ENTER(set->instance, "aprend_binding_set_update");
	if (!entries)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "entries is NULL");
	if (entry_count == 0)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "entry_count is 0");
	if (entry_count > set->entry_count)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "%u entries, and the set has %u", entry_count, set->entry_count);
	aprend_binding_layout layout = set->layout;
	// Each entry names an array element the layout declares, once, and holds
	// what its slot takes: the checks aprend_binding_set_create makes.
	for (uint32_t i = 0; i < entry_count; ++i) {
		const APREND_BINDING_SET_ENTRY &entry = entries[i];
		const APREND_BINDING_DESC *binding    = aprend_binding_layout_find_desc(layout, entry._binding);
		if (!binding)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "entry %u names binding %u, which the layout doesn't declare", i, entry._binding);
		if (entry._array_element >= binding->_count)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "entry %u (binding %u) has _array_element %u of %u", i, entry._binding, entry._array_element, binding->_count);
		if (entry._type != binding->_type)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "entry %u (binding %u) has _type %u, the layout declares %u", i, entry._binding, (unsigned)entry._type, (unsigned)binding->_type);
		for (uint32_t j = 0; j < i; ++j) {
			if (entries[j]._binding == entry._binding && entries[j]._array_element == entry._array_element)
				return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "entries %u and %u both fill binding %u element %u", j, i, entry._binding, entry._array_element);
		}
		if (const APRESULT valid = aprend_binding_set_entry_resource_valid(out_error, layout->instance, entry, i))
			return valid;
	}

	// The set as it will be: what it was built from, with the given entries
	// in place of the ones that fill the same elements.
	APREND_BINDING_SET_ENTRY *merged = (APREND_BINDING_SET_ENTRY *)malloc(sizeof(APREND_BINDING_SET_ENTRY) * set->entry_count);
	if (!merged)
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the set's entries");
	memcpy(merged, set->entries, sizeof(APREND_BINDING_SET_ENTRY) * set->entry_count);
	for (uint32_t i = 0; i < entry_count; ++i)
		for (uint32_t j = 0; j < set->entry_count; ++j)
			if (merged[j]._binding == entries[i]._binding && merged[j]._array_element == entries[i]._array_element)
				merged[j] = entries[i];

	// Descriptor sets a submitted frame may be reading are never rewritten.
	// A whole new set is built, the handle takes what it holds, and the new
	// handle leaves with the old descriptor sets, which the release queue
	// gives back to the layout once the GPU has finished with them.
	aprend_binding_set fresh = nullptr;
	const APRESULT made      = aprend_binding_set_create(layout, merged, set->entry_count, &fresh, out_error);
	free(merged);
	if (made != APRESULT_OK)
		return made;
	std::swap(set->sets, fresh->sets);
	std::swap(set->frame_set_count, fresh->frame_set_count);
	std::swap(set->entries, fresh->entries);
	std::swap(set->entry_count, fresh->entry_count);
	std::swap(set->image_uses, fresh->image_uses);
	std::swap(set->image_use_count, fresh->image_use_count);
	std::swap(set->storage_stores, fresh->storage_stores);
	std::swap(set->storage_store_capacity, fresh->storage_store_capacity);
	std::swap(set->storage_store_counts, fresh->storage_store_counts);
	// A list compiled before would bind the old descriptor sets, which go to
	// the release queue now and may be gone before it is submitted.
	aprend_instance_forget(set->instance, set);
	aprend_binding_set_destroy(fresh);
	return APRESULT_OK;
}
void aprend_binding_set_destroy(aprend_binding_set set) {
	if (!set)
		return;
	// A compiled list that binds it is refused at submit.
	aprend_instance_forget(set->instance, set);
	// Released once the GPU has finished everything submitted so far.
	aprend_instance_retire(set->instance, set, &aprend_release_handle<aprend_binding_set_t>);
}

} // Extern "C"
